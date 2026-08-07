#!/usr/bin/env python3
"""
record_service.py — ATSC 3.0 Service Recorder

Records ROUTE segments from an ATSC 3.0 broadcast to disk.
Organizes content by service ID and content type (video/audio).

Usage:
    record_service.py [options]

Options:
    --output DIR    Output directory (default: ./recordings)
    --service ID    Record specific service ID (default: all)
    --duration SEC  Recording duration in seconds (0 = indefinite)
    --host HOST     Metrics server host for segment notifications
    --port PORT     Metrics server port (default: 8081)
    -v, --verbose   Verbose output
    -h, --help      Show this help message

Directory Structure:
    recordings/
        service_1/
            video/
                segment_0001.m4s
                segment_0002.m4s
            audio/
                segment_0001.m4s
        service_2/
            ...
"""

import argparse
import json
import os
import signal
import socket
import struct
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Dict, Any, Optional


class SegmentRecorder:
    """Records ROUTE segments to disk"""

    def __init__(
        self,
        output_dir: str = "./recordings",
        service_filter: Optional[int] = None,
        verbose: bool = False,
    ):
        self.output_dir = Path(output_dir)
        self.service_filter = service_filter
        self.verbose = verbose

        self._running = False
        self._start_time: Optional[float] = None

        # Statistics
        self.stats = {
            "segments_received": 0,
            "segments_written": 0,
            "bytes_written": 0,
            "video_segments": 0,
            "audio_segments": 0,
            "services_seen": set(),
            "errors": 0,
        }

        # Segment counters per service/type
        self._segment_counters: Dict[str, int] = {}

    def start(self):
        """Start recording"""
        self._running = True
        self._start_time = time.time()

        # Create output directory
        self.output_dir.mkdir(parents=True, exist_ok=True)

        if self.verbose:
            print(f"Recording to: {self.output_dir.absolute()}")
            if self.service_filter is not None:
                print(f"Filtering service ID: {self.service_filter}")

    def stop(self):
        """Stop recording"""
        self._running = False

    def write_segment(
        self,
        service_id: int,
        content_type: str,
        segment_data: bytes,
        toi: int = 0,
        presentation_time: int = 0,
    ) -> bool:
        """
        Write a segment to disk.

        Args:
            service_id: Service ID
            content_type: "video" or "audio"
            segment_data: Raw segment bytes
            toi: Transport Object Identifier
            presentation_time: Presentation timestamp

        Returns:
            True if written successfully
        """
        if not self._running:
            return False

        # Apply service filter
        if self.service_filter is not None and service_id != self.service_filter:
            return False

        self.stats["segments_received"] += 1
        self.stats["services_seen"].add(service_id)

        # Create service directory
        service_dir = self.output_dir / f"service_{service_id}" / content_type
        service_dir.mkdir(parents=True, exist_ok=True)

        # Get segment number
        counter_key = f"{service_id}_{content_type}"
        if counter_key not in self._segment_counters:
            self._segment_counters[counter_key] = 0
        self._segment_counters[counter_key] += 1
        seg_num = self._segment_counters[counter_key]

        # Write segment file
        filename = f"segment_{seg_num:06d}.m4s"
        filepath = service_dir / filename

        try:
            with open(filepath, "wb") as f:
                f.write(segment_data)

            self.stats["segments_written"] += 1
            self.stats["bytes_written"] += len(segment_data)

            if content_type == "video":
                self.stats["video_segments"] += 1
            elif content_type == "audio":
                self.stats["audio_segments"] += 1

            if self.verbose:
                print(
                    f"[{datetime.now().strftime('%H:%M:%S')}] "
                    f"Service {service_id} {content_type}: {filename} "
                    f"({len(segment_data)} bytes)"
                )

            return True

        except IOError as e:
            self.stats["errors"] += 1
            if self.verbose:
                print(f"Error writing {filepath}: {e}", file=sys.stderr)
            return False

    def write_metadata(self, service_id: int, metadata: Dict[str, Any]):
        """Write service metadata to JSON file"""
        service_dir = self.output_dir / f"service_{service_id}"
        service_dir.mkdir(parents=True, exist_ok=True)

        meta_path = service_dir / "metadata.json"
        try:
            with open(meta_path, "w") as f:
                json.dump(metadata, f, indent=2)
        except IOError as e:
            if self.verbose:
                print(f"Error writing metadata: {e}", file=sys.stderr)

    def get_stats(self) -> Dict[str, Any]:
        """Get recording statistics"""
        stats = dict(self.stats)
        stats["services_seen"] = list(stats["services_seen"])
        if self._start_time:
            stats["duration_sec"] = time.time() - self._start_time
        return stats

    def print_summary(self):
        """Print recording summary"""
        stats = self.get_stats()
        print("\n=== Recording Summary ===")
        print(f"Duration: {stats.get('duration_sec', 0):.1f} seconds")
        print(f"Services recorded: {stats['services_seen']}")
        print(f"Segments received: {stats['segments_received']}")
        print(f"Segments written: {stats['segments_written']}")
        print(f"  Video: {stats['video_segments']}")
        print(f"  Audio: {stats['audio_segments']}")
        print(f"Total bytes: {stats['bytes_written']:,}")
        print(f"Errors: {stats['errors']}")


class SegmentReceiver:
    """Receives segment notifications via UDP socket"""

    def __init__(
        self, recorder: SegmentRecorder, host: str = "0.0.0.0", port: int = 8081
    ):
        self.recorder = recorder
        self.host = host
        self.port = port
        self._socket: Optional[socket.socket] = None
        self._running = False
        self._thread: Optional[threading.Thread] = None

    def start(self):
        """Start receiving segment notifications"""
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._socket.bind((self.host, self.port))
        self._socket.settimeout(1.0)

        self._running = True
        self._thread = threading.Thread(target=self._receive_loop, daemon=True)
        self._thread.start()

        print(f"Listening for segments on UDP {self.host}:{self.port}")

    def stop(self):
        """Stop receiving"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)
        if self._socket:
            self._socket.close()

    def _receive_loop(self):
        """Background thread for receiving segment notifications"""
        while self._running:
            try:
                data, addr = self._socket.recvfrom(65536)
                self._process_notification(data)
            except socket.timeout:
                continue
            except Exception as e:
                if self._running:
                    print(f"Receive error: {e}", file=sys.stderr)

    def _process_notification(self, data: bytes):
        """Process incoming segment notification"""
        # Protocol: 4-byte service_id, 4-byte type (0=video, 1=audio),
        #           4-byte TOI, 4-byte length, followed by segment data
        if len(data) < 16:
            return

        service_id, content_type_id, toi, seg_len = struct.unpack("<IIII", data[:16])
        segment_data = data[16 : 16 + seg_len]

        content_type = "video" if content_type_id == 0 else "audio"
        self.recorder.write_segment(service_id, content_type, segment_data, toi)


class SimulatedSegmentSource:
    """Generates simulated segments for testing"""

    def __init__(self, recorder: SegmentRecorder, interval: float = 1.0):
        self.recorder = recorder
        self.interval = interval
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._counter = 0

    def start(self):
        """Start generating simulated segments"""
        self._running = True
        self._thread = threading.Thread(target=self._generate_loop, daemon=True)
        self._thread.start()
        print("Generating simulated segments...")

    def stop(self):
        """Stop generating"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)

    def _generate_loop(self):
        """Generate simulated segments"""
        while self._running:
            self._counter += 1

            # Simulate video segment (service 1)
            video_data = self._make_fake_segment("video", self._counter)
            self.recorder.write_segment(1, "video", video_data, toi=self._counter)

            # Simulate audio segment (service 1, every 4th)
            if self._counter % 4 == 0:
                audio_data = self._make_fake_segment("audio", self._counter // 4)
                self.recorder.write_segment(
                    1, "audio", audio_data, toi=self._counter // 4
                )

            # Occasionally add service 2
            if self._counter % 10 == 0:
                video_data = self._make_fake_segment("video", self._counter // 10)
                self.recorder.write_segment(2, "video", video_data)

            time.sleep(self.interval)

    def _make_fake_segment(self, content_type: str, seq: int) -> bytes:
        """Create a fake CMAF/fMP4 segment header for testing"""
        # Minimal ftyp + moof + mdat structure (not valid, but recognizable)
        header = b"ftyp" + b"iso6" + struct.pack(">I", seq)
        if content_type == "video":
            # Simulate ~100KB video segment
            payload = os.urandom(100 * 1024)
        else:
            # Simulate ~10KB audio segment
            payload = os.urandom(10 * 1024)
        return header + payload


def parse_args():
    """Parse command line arguments"""
    parser = argparse.ArgumentParser(
        description="ATSC 3.0 Service Recorder",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    parser.add_argument(
        "--output",
        default="./recordings",
        metavar="DIR",
        help="Output directory (default: ./recordings)",
    )
    parser.add_argument(
        "--service",
        type=int,
        default=None,
        metavar="ID",
        help="Record specific service ID (default: all)",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=0,
        metavar="SEC",
        help="Recording duration in seconds (0 = indefinite)",
    )
    parser.add_argument(
        "--host",
        default="0.0.0.0",
        help="Listen address for segment notifications",
    )
    parser.add_argument(
        "--port", type=int, default=8081, help="Listen port (default: 8081)"
    )
    parser.add_argument(
        "--simulate",
        action="store_true",
        help="Generate simulated segments for testing",
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")

    return parser.parse_args()


def main():
    """Main entry point"""
    args = parse_args()

    # Create recorder
    recorder = SegmentRecorder(
        output_dir=args.output,
        service_filter=args.service,
        verbose=args.verbose,
    )

    # Set up signal handler
    def signal_handler(sig, frame):
        print("\nStopping...")
        recorder.stop()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Start recording
    recorder.start()

    print("ATSC 3.0 Service Recorder")
    print(f"  Output: {args.output}")
    if args.service:
        print(f"  Service filter: {args.service}")
    if args.duration > 0:
        print(f"  Duration: {args.duration}s")
    print()

    if args.simulate:
        # Use simulated segment source for testing
        source = SimulatedSegmentSource(recorder, interval=0.5)
        source.start()
    else:
        # Use real UDP receiver
        receiver = SegmentReceiver(recorder, host=args.host, port=args.port)
        receiver.start()

    # Run for specified duration or indefinitely
    try:
        if args.duration > 0:
            time.sleep(args.duration)
            print("\nDuration reached.")
        else:
            print("Press Ctrl+C to stop recording...")
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        pass

    # Stop
    recorder.stop()
    if args.simulate:
        source.stop()
    else:
        receiver.stop()

    # Print summary
    recorder.print_summary()

    return 0


if __name__ == "__main__":
    sys.exit(main())
