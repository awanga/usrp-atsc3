#!/usr/bin/env python3
"""
cc_extract.py — ATSC 3.0 Closed Caption Extractor

Extracts and displays closed captions from ATSC 3.0 broadcasts.
Supports IMSC1 (Internet Media Subtitles and Captions) format used in ATSC 3.0.

Usage:
    cc_extract.py [options]

Options:
    --output FILE   Output captions to file (SRT format)
    --service ID    Extract from specific service ID
    --host HOST     Listen address for caption notifications
    --port PORT     Listen port (default: 8083)
    --format FMT    Output format: srt, vtt, txt (default: txt)
    -v, --verbose   Verbose output
    -h, --help      Show this help message

ATSC 3.0 Caption Formats:
    - IMSC1: Internet Media Subtitles and Captions (primary)
    - CEA-708: Legacy format (carried in SEI messages)

References:
    ATSC A/343: Captions and Subtitles
    TTML/IMSC: Timed Text Markup Language / IMSC1
"""

import argparse
import html
import re
import signal
import socket
import struct
import sys
import threading
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from datetime import timedelta
from pathlib import Path
from typing import Dict, List, Optional, Any


@dataclass
class CaptionCue:
    """A single caption cue"""

    start_time: timedelta
    end_time: timedelta
    text: str
    region: str = ""
    style: str = ""
    speaker: str = ""


@dataclass
class CaptionTrack:
    """A caption track with multiple cues"""

    track_id: int = 0
    language: str = "en"
    kind: str = "captions"  # captions, subtitles, descriptions
    label: str = "English"
    cues: List[CaptionCue] = field(default_factory=list)


class ImscParser:
    """Parses IMSC1/TTML caption documents"""

    # TTML/IMSC namespaces
    NAMESPACES = {
        "tt": "http://www.w3.org/ns/ttml",
        "tts": "http://www.w3.org/ns/ttml#styling",
        "ttp": "http://www.w3.org/ns/ttml#parameter",
        "ttm": "http://www.w3.org/ns/ttml#metadata",
        "xml": "http://www.w3.org/XML/1998/namespace",
        "smpte": "http://www.smpte-ra.org/schemas/2052-1/2010/smpte-tt",
        "ebutts": "urn:ebu:tt:style",
    }

    def __init__(self, verbose: bool = False):
        self.verbose = verbose

    def parse(self, xml_data: bytes) -> Optional[CaptionTrack]:
        """Parse IMSC1/TTML document"""
        try:
            # Handle BOM and encoding
            xml_str = xml_data.decode("utf-8-sig", errors="replace")

            # Register namespaces
            for prefix, uri in self.NAMESPACES.items():
                ET.register_namespace(prefix, uri)

            root = ET.fromstring(xml_str)
            return self._parse_tt(root)

        except ET.ParseError as e:
            if self.verbose:
                print(f"IMSC parse error: {e}")
            return None
        except Exception as e:
            if self.verbose:
                print(f"Error parsing IMSC: {e}")
            return None

    def _parse_tt(self, root: ET.Element) -> CaptionTrack:
        """Parse tt (timed text) root element"""
        track = CaptionTrack()

        # Get language from xml:lang
        lang = root.get(f"{{{self.NAMESPACES['xml']}}}lang")
        if lang:
            track.language = lang

        # Parse body/div/p elements
        body = root.find("tt:body", self.NAMESPACES)
        if body is None:
            # Try without namespace
            body = root.find("body")
        if body is None:
            return track

        for div in body.findall(".//tt:div", self.NAMESPACES) or body.findall(".//div"):
            for p in div.findall(".//tt:p", self.NAMESPACES) or div.findall(".//p"):
                cue = self._parse_p(p)
                if cue:
                    track.cues.append(cue)

        # Also try direct p elements under body
        for p in body.findall(".//tt:p", self.NAMESPACES) or body.findall(".//p"):
            cue = self._parse_p(p)
            if cue and cue not in track.cues:
                track.cues.append(cue)

        return track

    def _parse_p(self, p: ET.Element) -> Optional[CaptionCue]:
        """Parse p (paragraph) element"""
        begin = p.get("begin")
        end = p.get("end")
        dur = p.get("dur")

        if not begin:
            return None

        start_time = self._parse_time(begin)
        if end:
            end_time = self._parse_time(end)
        elif dur:
            dur_time = self._parse_time(dur)
            end_time = start_time + dur_time
        else:
            end_time = start_time + timedelta(seconds=2)  # Default 2 sec

        # Extract text content
        text = self._get_text(p)
        if not text.strip():
            return None

        return CaptionCue(
            start_time=start_time,
            end_time=end_time,
            text=text.strip(),
            region=p.get("region", ""),
            style=p.get("style", ""),
        )

    def _get_text(self, element: ET.Element) -> str:
        """Extract text from element, handling spans and breaks"""
        parts = []

        if element.text:
            parts.append(element.text)

        for child in element:
            tag_local = child.tag.split("}")[-1] if "}" in child.tag else child.tag

            if tag_local == "br":
                parts.append("\n")
            elif tag_local == "span":
                parts.append(self._get_text(child))

            if child.tail:
                parts.append(child.tail)

        return "".join(parts)

    def _parse_time(self, time_str: str) -> timedelta:
        """
        Parse TTML time expression.

        Formats:
            - clock-time: HH:MM:SS.mmm or HH:MM:SS:FF (frames)
            - offset-time: 1.5s, 500ms, 10f
        """
        if not time_str:
            return timedelta()

        time_str = time_str.strip()

        # Try clock-time format (HH:MM:SS.mmm or HH:MM:SS:FF)
        clock_match = re.match(r"(\d+):(\d+):(\d+)(?:\.(\d+)|:(\d+))?", time_str)
        if clock_match:
            hours = int(clock_match.group(1))
            minutes = int(clock_match.group(2))
            seconds = int(clock_match.group(3))
            frac = clock_match.group(4)
            frames = clock_match.group(5)

            total_seconds = hours * 3600 + minutes * 60 + seconds

            if frac:
                # Milliseconds/fraction
                total_seconds += float(f"0.{frac}")
            elif frames:
                # Assume 30 fps
                total_seconds += int(frames) / 30.0

            return timedelta(seconds=total_seconds)

        # Try offset-time format
        offset_match = re.match(r"([\d.]+)(s|ms|f|h|m|t)?", time_str)
        if offset_match:
            value = float(offset_match.group(1))
            unit = offset_match.group(2) or "s"

            if unit == "ms":
                return timedelta(milliseconds=value)
            elif unit == "s":
                return timedelta(seconds=value)
            elif unit == "m":
                return timedelta(minutes=value)
            elif unit == "h":
                return timedelta(hours=value)
            elif unit == "f":
                # Assume 30 fps
                return timedelta(seconds=value / 30.0)
            elif unit == "t":
                # Ticks (typically 10,000,000 ticks/sec)
                return timedelta(seconds=value / 10000000.0)

        return timedelta()


class CaptionDisplay:
    """Displays captions to console"""

    def __init__(self, verbose: bool = False):
        self.verbose = verbose
        self._last_cue: Optional[CaptionCue] = None

    def display(self, cue: CaptionCue):
        """Display a caption cue"""
        # Format time
        start_str = self._format_time(cue.start_time)

        # Clear previous line if same time
        if self._last_cue and cue.start_time == self._last_cue.start_time:
            print("\033[2K\033[1A", end="")  # Clear line and move up

        # Print caption
        print(f"\033[1m[{start_str}]\033[0m {cue.text}")

        self._last_cue = cue

    def _format_time(self, td: timedelta) -> str:
        """Format timedelta as HH:MM:SS.mmm"""
        total_seconds = td.total_seconds()
        hours = int(total_seconds // 3600)
        minutes = int((total_seconds % 3600) // 60)
        seconds = int(total_seconds % 60)
        millis = int((total_seconds % 1) * 1000)

        if hours > 0:
            return f"{hours}:{minutes:02d}:{seconds:02d}.{millis:03d}"
        else:
            return f"{minutes:02d}:{seconds:02d}.{millis:03d}"


class CaptionWriter:
    """Writes captions to file"""

    def __init__(self, output_path: Path, format: str = "srt"):
        self.output_path = output_path
        self.format = format
        self._cue_count = 0
        self._file = None

    def open(self):
        """Open output file"""
        self._file = open(self.output_path, "w", encoding="utf-8")
        if self.format == "vtt":
            self._file.write("WEBVTT\n\n")

    def close(self):
        """Close output file"""
        if self._file:
            self._file.close()

    def write(self, cue: CaptionCue):
        """Write a caption cue"""
        if not self._file:
            return

        self._cue_count += 1

        if self.format == "srt":
            self._write_srt(cue)
        elif self.format == "vtt":
            self._write_vtt(cue)
        else:  # txt
            self._write_txt(cue)

        self._file.flush()

    def _write_srt(self, cue: CaptionCue):
        """Write SRT format"""
        start = self._format_srt_time(cue.start_time)
        end = self._format_srt_time(cue.end_time)

        self._file.write(f"{self._cue_count}\n")
        self._file.write(f"{start} --> {end}\n")
        self._file.write(f"{cue.text}\n")
        self._file.write("\n")

    def _write_vtt(self, cue: CaptionCue):
        """Write WebVTT format"""
        start = self._format_vtt_time(cue.start_time)
        end = self._format_vtt_time(cue.end_time)

        self._file.write(f"{start} --> {end}\n")
        self._file.write(f"{cue.text}\n")
        self._file.write("\n")

    def _write_txt(self, cue: CaptionCue):
        """Write plain text format"""
        start = self._format_vtt_time(cue.start_time)
        self._file.write(f"[{start}] {cue.text}\n")

    def _format_srt_time(self, td: timedelta) -> str:
        """Format as SRT time: HH:MM:SS,mmm"""
        total_seconds = td.total_seconds()
        hours = int(total_seconds // 3600)
        minutes = int((total_seconds % 3600) // 60)
        seconds = int(total_seconds % 60)
        millis = int((total_seconds % 1) * 1000)
        return f"{hours:02d}:{minutes:02d}:{seconds:02d},{millis:03d}"

    def _format_vtt_time(self, td: timedelta) -> str:
        """Format as WebVTT time: HH:MM:SS.mmm"""
        total_seconds = td.total_seconds()
        hours = int(total_seconds // 3600)
        minutes = int((total_seconds % 3600) // 60)
        seconds = int(total_seconds % 60)
        millis = int((total_seconds % 1) * 1000)
        return f"{hours:02d}:{minutes:02d}:{seconds:02d}.{millis:03d}"


class CaptionExtractor:
    """Extracts and processes ATSC 3.0 captions"""

    def __init__(
        self,
        verbose: bool = False,
        output_path: Optional[Path] = None,
        output_format: str = "txt",
        service_filter: Optional[int] = None,
    ):
        self.verbose = verbose
        self.service_filter = service_filter

        self.parser = ImscParser(verbose=verbose)
        self.display = CaptionDisplay(verbose=verbose)
        self.writer: Optional[CaptionWriter] = None

        if output_path:
            self.writer = CaptionWriter(output_path, output_format)

        # Statistics
        self.stats = {
            "documents_received": 0,
            "cues_extracted": 0,
            "parse_errors": 0,
        }

        self._running = False

    def start(self):
        """Start caption extraction"""
        self._running = True
        if self.writer:
            self.writer.open()
        if self.verbose:
            print("Caption extractor started")
            print()

    def stop(self):
        """Stop extraction"""
        self._running = False
        if self.writer:
            self.writer.close()

    def process_imsc(self, data: bytes, service_id: int = 0) -> int:
        """
        Process IMSC caption document.

        Returns number of cues extracted.
        """
        if not self._running:
            return 0

        # Apply service filter
        if self.service_filter is not None and service_id != self.service_filter:
            return 0

        self.stats["documents_received"] += 1

        track = self.parser.parse(data)
        if track is None:
            self.stats["parse_errors"] += 1
            return 0

        cue_count = 0
        for cue in track.cues:
            self.stats["cues_extracted"] += 1
            cue_count += 1

            # Display to console
            self.display.display(cue)

            # Write to file
            if self.writer:
                self.writer.write(cue)

        return cue_count

    def get_stats(self) -> Dict[str, Any]:
        """Get extraction statistics"""
        return dict(self.stats)

    def print_summary(self):
        """Print extraction summary"""
        print("\n=== Caption Extraction Summary ===")
        print(f"Documents received: {self.stats['documents_received']}")
        print(f"Cues extracted: {self.stats['cues_extracted']}")
        print(f"Parse errors: {self.stats['parse_errors']}")


class CaptionReceiver:
    """Receives caption data via UDP socket"""

    def __init__(
        self, extractor: CaptionExtractor, host: str = "0.0.0.0", port: int = 8083
    ):
        self.extractor = extractor
        self.host = host
        self.port = port
        self._socket: Optional[socket.socket] = None
        self._running = False
        self._thread: Optional[threading.Thread] = None

    def start(self):
        """Start receiving caption data"""
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._socket.bind((self.host, self.port))
        self._socket.settimeout(1.0)

        self._running = True
        self._thread = threading.Thread(target=self._receive_loop, daemon=True)
        self._thread.start()

        print(f"Listening for captions on UDP {self.host}:{self.port}")

    def stop(self):
        """Stop receiving"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)
        if self._socket:
            self._socket.close()

    def _receive_loop(self):
        """Background thread for receiving caption data"""
        while self._running:
            try:
                data, addr = self._socket.recvfrom(65536)
                # Protocol: 4-byte service_id, followed by IMSC data
                if len(data) > 4:
                    service_id = struct.unpack("<I", data[:4])[0]
                    imsc_data = data[4:]
                    self.extractor.process_imsc(imsc_data, service_id)
            except socket.timeout:
                continue
            except Exception as e:
                if self._running:
                    print(f"Receive error: {e}", file=sys.stderr)


class SimulatedCaptionSource:
    """Generates simulated captions for testing"""

    def __init__(self, extractor: CaptionExtractor, interval: float = 2.0):
        self.extractor = extractor
        self.interval = interval
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._time_offset = timedelta()

    # Sample dialogue for simulation
    SAMPLE_CAPTIONS = [
        "Welcome to the ATSC 3.0 broadcast.",
        "This is a test of the caption extraction system.",
        "Captions can include multiple lines\nlike this example.",
        ">> Speaker 1: Hello everyone!",
        ">> Speaker 2: Hi, how are you doing today?",
        "The weather forecast calls for sunny skies.",
        "[Music playing]",
        "And now, back to our program.",
        "[Applause]",
        "Thank you for watching!",
    ]

    def start(self):
        """Start generating simulated captions"""
        self._running = True
        self._thread = threading.Thread(target=self._generate_loop, daemon=True)
        self._thread.start()
        print("Generating simulated captions...")
        print()

    def stop(self):
        """Stop generating"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)

    def _generate_loop(self):
        """Generate simulated captions"""
        caption_idx = 0

        while self._running:
            # Create IMSC document
            text = self.SAMPLE_CAPTIONS[caption_idx % len(self.SAMPLE_CAPTIONS)]
            imsc_data = self._make_imsc(
                text,
                self._time_offset,
                self._time_offset + timedelta(seconds=self.interval),
            )

            self.extractor.process_imsc(imsc_data, service_id=1)

            caption_idx += 1
            self._time_offset += timedelta(seconds=self.interval)
            time.sleep(self.interval)

    def _make_imsc(self, text: str, start: timedelta, end: timedelta) -> bytes:
        """Create a minimal IMSC document"""
        start_str = self._format_time(start)
        end_str = self._format_time(end)

        # Escape HTML entities
        text_escaped = html.escape(text).replace("\n", "<br/>")

        imsc = f"""<?xml version="1.0" encoding="UTF-8"?>
<tt xml:lang="en" xmlns="http://www.w3.org/ns/ttml">
  <body>
    <div>
      <p begin="{start_str}" end="{end_str}">{text_escaped}</p>
    </div>
  </body>
</tt>"""

        return imsc.encode("utf-8")

    def _format_time(self, td: timedelta) -> str:
        """Format timedelta as TTML time"""
        total_seconds = td.total_seconds()
        hours = int(total_seconds // 3600)
        minutes = int((total_seconds % 3600) // 60)
        seconds = total_seconds % 60
        return f"{hours:02d}:{minutes:02d}:{seconds:06.3f}"


def parse_args():
    """Parse command line arguments"""
    parser = argparse.ArgumentParser(
        description="ATSC 3.0 Closed Caption Extractor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    parser.add_argument(
        "--output",
        metavar="FILE",
        help="Output captions to file",
    )
    parser.add_argument(
        "--format",
        choices=["srt", "vtt", "txt"],
        default="txt",
        help="Output format (default: txt)",
    )
    parser.add_argument(
        "--service",
        type=int,
        default=None,
        metavar="ID",
        help="Extract from specific service ID",
    )
    parser.add_argument(
        "--host",
        default="0.0.0.0",
        help="Listen address for caption notifications",
    )
    parser.add_argument(
        "--port", type=int, default=8083, help="Listen port (default: 8083)"
    )
    parser.add_argument(
        "--simulate",
        action="store_true",
        help="Generate simulated captions for testing",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=2.0,
        help="Interval between simulated captions (default: 2s)",
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")

    return parser.parse_args()


def main():
    """Main entry point"""
    args = parse_args()

    # Create extractor
    output_path = Path(args.output) if args.output else None
    extractor = CaptionExtractor(
        verbose=args.verbose,
        output_path=output_path,
        output_format=args.format,
        service_filter=args.service,
    )

    # Set up signal handler
    def signal_handler(sig, frame):
        print("\nStopping...")
        extractor.stop()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Start extraction
    extractor.start()

    print("ATSC 3.0 Closed Caption Extractor")
    print("=" * 40)
    if args.output:
        print(f"  Output: {args.output} ({args.format})")
    if args.service:
        print(f"  Service filter: {args.service}")
    print()

    if args.simulate:
        # Use simulated caption source for testing
        source = SimulatedCaptionSource(extractor, interval=args.interval)
        source.start()
    else:
        # Use real UDP receiver
        receiver = CaptionReceiver(extractor, host=args.host, port=args.port)
        receiver.start()

    # Run indefinitely
    try:
        print("Press Ctrl+C to stop extraction...")
        print()
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass

    # Stop
    extractor.stop()
    if args.simulate:
        source.stop()
    else:
        receiver.stop()

    # Print summary
    extractor.print_summary()

    return 0


if __name__ == "__main__":
    sys.exit(main())
