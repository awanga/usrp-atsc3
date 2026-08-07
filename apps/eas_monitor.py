#!/usr/bin/env python3
"""
eas_monitor.py — ATSC 3.0 Emergency Alert Monitor

Monitors for Advanced Emergency Alert (AEA) messages in ATSC 3.0 broadcasts.
Parses CAP (Common Alerting Protocol) alert messages and displays warnings.

Usage:
    eas_monitor.py [options]

Options:
    --host HOST     Metrics server host for alert notifications
    --port PORT     Alert notification port (default: 8082)
    --log FILE      Log alerts to file
    --audio         Play audio alert tone when alert received
    -v, --verbose   Verbose output
    -h, --help      Show this help message

Alert Types (SAME codes):
    EAN - Emergency Action Notification (highest priority)
    EAT - Emergency Action Termination
    NIC - National Information Center
    NPT - National Periodic Test
    RMT - Required Monthly Test
    RWT - Required Weekly Test
    TOR - Tornado Warning
    SVR - Severe Thunderstorm Warning
    FFW - Flash Flood Warning
    ...and many more per ATSC A/331

References:
    ATSC A/331: Signaling, Delivery, and Synchronization
    CAP v1.2: Common Alerting Protocol
    SAME: Specific Area Message Encoding
"""

import argparse
import json
import signal
import socket
import struct
import sys
import threading
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
from pathlib import Path
from typing import Dict, List, Optional, Any


class AlertSeverity(Enum):
    """CAP alert severity levels"""

    EXTREME = "Extreme"
    SEVERE = "Severe"
    MODERATE = "Moderate"
    MINOR = "Minor"
    UNKNOWN = "Unknown"


class AlertUrgency(Enum):
    """CAP alert urgency levels"""

    IMMEDIATE = "Immediate"
    EXPECTED = "Expected"
    FUTURE = "Future"
    PAST = "Past"
    UNKNOWN = "Unknown"


@dataclass
class AeaHeader:
    """AEA (Advanced Emergency Alert) header per ATSC A/331"""

    aea_id: int = 0
    aea_type: int = 0  # 0=alert, 1=update, 2=cancel
    priority: int = 0  # 0-255, higher = more important
    wakeup: bool = False  # Device should wake from standby
    valid_from: datetime = field(default_factory=datetime.now)
    valid_to: datetime = field(default_factory=datetime.now)
    issuer: str = ""
    audience: str = ""  # Geographic area codes


@dataclass
class CapAlert:
    """CAP (Common Alerting Protocol) alert message"""

    identifier: str = ""
    sender: str = ""
    sent: datetime = field(default_factory=datetime.now)
    status: str = ""  # Actual, Exercise, System, Test, Draft
    msg_type: str = ""  # Alert, Update, Cancel, Ack, Error
    scope: str = ""  # Public, Restricted, Private
    code: str = ""  # SAME event code (TOR, SVR, etc.)

    # Info block (may have multiple)
    category: str = ""
    event: str = ""
    urgency: AlertUrgency = AlertUrgency.UNKNOWN
    severity: AlertSeverity = AlertSeverity.UNKNOWN
    certainty: str = ""
    effective: Optional[datetime] = None
    expires: Optional[datetime] = None
    sender_name: str = ""
    headline: str = ""
    description: str = ""
    instruction: str = ""
    areas: List[str] = field(default_factory=list)

    # Raw XML for debugging
    raw_xml: str = ""


# SAME event codes (subset of most common)
SAME_CODES = {
    "EAN": "Emergency Action Notification",
    "EAT": "Emergency Action Termination",
    "NIC": "National Information Center",
    "NPT": "National Periodic Test",
    "RMT": "Required Monthly Test",
    "RWT": "Required Weekly Test",
    "TOR": "Tornado Warning",
    "TOW": "Tornado Watch",
    "SVR": "Severe Thunderstorm Warning",
    "SVS": "Severe Weather Statement",
    "FFW": "Flash Flood Warning",
    "FFA": "Flash Flood Watch",
    "FFS": "Flash Flood Statement",
    "HUW": "Hurricane Warning",
    "HUA": "Hurricane Watch",
    "HLS": "Hurricane Statement",
    "TSW": "Tsunami Warning",
    "TSA": "Tsunami Watch",
    "EQW": "Earthquake Warning",
    "VOW": "Volcano Warning",
    "CEM": "Civil Emergency Message",
    "CDW": "Civil Danger Warning",
    "LAE": "Local Area Emergency",
    "BZW": "Blizzard Warning",
    "WSW": "Winter Storm Warning",
    "HWW": "High Wind Warning",
    "EVI": "Evacuation Immediate",
    "SPW": "Shelter in Place Warning",
    "ADR": "Administrative Message",
}


class EasMonitor:
    """Monitors and parses AEA/CAP emergency alerts"""

    def __init__(self, verbose: bool = False, log_file: Optional[str] = None):
        self.verbose = verbose
        self.log_file = Path(log_file) if log_file else None
        self._running = False

        # Statistics
        self.stats = {
            "alerts_received": 0,
            "parse_errors": 0,
            "by_type": {},
            "by_severity": {},
        }

        # Recent alerts (for deduplication)
        self._recent_alerts: Dict[str, datetime] = {}

        # Alert callbacks
        self._callbacks: List[callable] = []

    def start(self):
        """Start monitoring"""
        self._running = True
        if self.verbose:
            print("EAS Monitor started")
            print("Waiting for emergency alerts...")
            print()

    def stop(self):
        """Stop monitoring"""
        self._running = False

    def add_callback(self, callback: callable):
        """Register callback for alert notifications"""
        self._callbacks.append(callback)

    def process_aea(self, data: bytes) -> Optional[CapAlert]:
        """
        Process AEA message from LLS signaling.

        Args:
            data: Raw AEA message bytes

        Returns:
            Parsed CapAlert or None if parsing failed
        """
        if len(data) < 20:
            return None

        self.stats["alerts_received"] += 1

        try:
            # Parse AEA header (per ATSC A/331 Section 6.3)
            header = self._parse_aea_header(data)
            if header is None:
                self.stats["parse_errors"] += 1
                return None

            # Find CAP XML payload
            cap_start = data.find(b"<?xml")
            if cap_start < 0:
                cap_start = data.find(b"<alert")
            if cap_start < 0:
                self.stats["parse_errors"] += 1
                return None

            # Parse CAP XML
            cap_xml = data[cap_start:].decode("utf-8", errors="replace")
            alert = self._parse_cap(cap_xml)
            if alert is None:
                self.stats["parse_errors"] += 1
                return None

            # Check for duplicate
            if self._is_duplicate(alert):
                if self.verbose:
                    print(f"[Duplicate] {alert.identifier}")
                return None

            # Update statistics
            self._update_stats(alert)

            # Log alert
            self._log_alert(alert)

            # Display alert
            self._display_alert(alert, header)

            # Notify callbacks
            for callback in self._callbacks:
                try:
                    callback(alert, header)
                except Exception as e:
                    if self.verbose:
                        print(f"Callback error: {e}")

            return alert

        except Exception as e:
            self.stats["parse_errors"] += 1
            if self.verbose:
                print(f"Parse error: {e}")
            return None

    def _parse_aea_header(self, data: bytes) -> Optional[AeaHeader]:
        """Parse AEA header from raw bytes"""
        if len(data) < 16:
            return None

        header = AeaHeader()

        # AEA header format (simplified - actual format per A/331)
        # Byte 0-3: aea_id
        # Byte 4: aea_type (2 bits), priority (6 bits)
        # Byte 5: flags (wakeup, etc.)
        # Rest: timestamps and strings

        header.aea_id = struct.unpack(">I", data[0:4])[0]
        header.aea_type = (data[4] >> 6) & 0x03
        header.priority = data[4] & 0x3F
        header.wakeup = bool(data[5] & 0x80)

        return header

    def _parse_cap(self, xml_str: str) -> Optional[CapAlert]:
        """Parse CAP XML message"""
        try:
            # Handle CAP namespace
            xml_str = xml_str.replace(
                'xmlns="urn:oasis:names:tc:emergency:cap:1.2"', ""
            )

            root = ET.fromstring(xml_str)
            alert = CapAlert()
            alert.raw_xml = xml_str

            # Parse alert element
            alert.identifier = self._get_text(root, "identifier", "")
            alert.sender = self._get_text(root, "sender", "")
            alert.status = self._get_text(root, "status", "")
            alert.msg_type = self._get_text(root, "msgType", "")
            alert.scope = self._get_text(root, "scope", "")

            sent_str = self._get_text(root, "sent", "")
            if sent_str:
                alert.sent = self._parse_datetime(sent_str)

            # Parse info element (take first one)
            info = root.find("info")
            if info is not None:
                alert.category = self._get_text(info, "category", "")
                alert.event = self._get_text(info, "event", "")
                alert.urgency = self._parse_urgency(self._get_text(info, "urgency", ""))
                alert.severity = self._parse_severity(
                    self._get_text(info, "severity", "")
                )
                alert.certainty = self._get_text(info, "certainty", "")
                alert.sender_name = self._get_text(info, "senderName", "")
                alert.headline = self._get_text(info, "headline", "")
                alert.description = self._get_text(info, "description", "")
                alert.instruction = self._get_text(info, "instruction", "")

                effective_str = self._get_text(info, "effective", "")
                if effective_str:
                    alert.effective = self._parse_datetime(effective_str)

                expires_str = self._get_text(info, "expires", "")
                if expires_str:
                    alert.expires = self._parse_datetime(expires_str)

                # Parse areas
                for area in info.findall("area"):
                    desc = self._get_text(area, "areaDesc", "")
                    if desc:
                        alert.areas.append(desc)

                # Extract SAME code from eventCode
                for event_code in info.findall("eventCode"):
                    value_name = self._get_text(event_code, "valueName", "")
                    if value_name == "SAME":
                        alert.code = self._get_text(event_code, "value", "")

            return alert

        except ET.ParseError as e:
            if self.verbose:
                print(f"CAP XML parse error: {e}")
            return None

    def _get_text(self, element: ET.Element, tag: str, default: str) -> str:
        """Get text content of child element"""
        child = element.find(tag)
        return child.text if child is not None and child.text else default

    def _parse_datetime(self, dt_str: str) -> datetime:
        """Parse CAP datetime string"""
        try:
            # CAP uses ISO 8601 format
            dt_str = dt_str.replace("Z", "+00:00")
            return datetime.fromisoformat(dt_str)
        except ValueError:
            return datetime.now()

    def _parse_urgency(self, urgency_str: str) -> AlertUrgency:
        """Parse urgency string"""
        try:
            return AlertUrgency(urgency_str)
        except ValueError:
            return AlertUrgency.UNKNOWN

    def _parse_severity(self, severity_str: str) -> AlertSeverity:
        """Parse severity string"""
        try:
            return AlertSeverity(severity_str)
        except ValueError:
            return AlertSeverity.UNKNOWN

    def _is_duplicate(self, alert: CapAlert) -> bool:
        """Check if alert is a duplicate"""
        # Clean old entries
        now = datetime.now()
        self._recent_alerts = {
            k: v
            for k, v in self._recent_alerts.items()
            if (now - v).total_seconds() < 3600  # 1 hour
        }

        # Check for duplicate
        if alert.identifier in self._recent_alerts:
            return True

        self._recent_alerts[alert.identifier] = now
        return False

    def _update_stats(self, alert: CapAlert):
        """Update statistics"""
        event_type = alert.code or alert.event or "Unknown"
        self.stats["by_type"][event_type] = self.stats["by_type"].get(event_type, 0) + 1

        severity = alert.severity.value
        self.stats["by_severity"][severity] = (
            self.stats["by_severity"].get(severity, 0) + 1
        )

    def _log_alert(self, alert: CapAlert):
        """Log alert to file"""
        if self.log_file is None:
            return

        try:
            with open(self.log_file, "a") as f:
                log_entry = {
                    "timestamp": datetime.now().isoformat(),
                    "identifier": alert.identifier,
                    "code": alert.code,
                    "event": alert.event,
                    "headline": alert.headline,
                    "severity": alert.severity.value,
                    "urgency": alert.urgency.value,
                    "areas": alert.areas,
                }
                f.write(json.dumps(log_entry) + "\n")
        except IOError:
            pass

    def _display_alert(self, alert: CapAlert, header: AeaHeader):
        """Display alert to console"""
        # Color codes for severity
        severity_colors = {
            AlertSeverity.EXTREME: "\033[91m",  # Red
            AlertSeverity.SEVERE: "\033[93m",  # Yellow
            AlertSeverity.MODERATE: "\033[94m",  # Blue
            AlertSeverity.MINOR: "\033[92m",  # Green
            AlertSeverity.UNKNOWN: "\033[0m",  # Default
        }
        reset = "\033[0m"
        color = severity_colors.get(alert.severity, reset)

        # Get event description
        event_desc = SAME_CODES.get(alert.code, alert.event)

        print()
        print(f"{color}{'=' * 60}{reset}")
        print(f"{color}EMERGENCY ALERT{reset}")
        print(f"{color}{'=' * 60}{reset}")
        print()
        print(f"  Event:     {event_desc} ({alert.code})")
        print(f"  Severity:  {color}{alert.severity.value}{reset}")
        print(f"  Urgency:   {alert.urgency.value}")
        print(f"  Time:      {alert.sent.strftime('%Y-%m-%d %H:%M:%S')}")
        print()

        if alert.headline:
            print(f"  Headline:  {alert.headline}")
            print()

        if alert.areas:
            print(f"  Areas:     {', '.join(alert.areas[:5])}")
            if len(alert.areas) > 5:
                print(f"             ...and {len(alert.areas) - 5} more")
            print()

        if alert.description:
            # Wrap description
            desc = alert.description[:500]
            if len(alert.description) > 500:
                desc += "..."
            print(f"  Details:   {desc}")
            print()

        if alert.instruction:
            print(f"  Action:    {alert.instruction[:200]}")
            print()

        print(f"  Sender:    {alert.sender_name or alert.sender}")
        print(f"  ID:        {alert.identifier}")
        if alert.expires:
            print(f"  Expires:   {alert.expires.strftime('%Y-%m-%d %H:%M:%S')}")

        print(f"{color}{'=' * 60}{reset}")
        print()

    def get_stats(self) -> Dict[str, Any]:
        """Get monitoring statistics"""
        return dict(self.stats)

    def print_summary(self):
        """Print monitoring summary"""
        print("\n=== EAS Monitor Summary ===")
        print(f"Alerts received: {self.stats['alerts_received']}")
        print(f"Parse errors: {self.stats['parse_errors']}")
        if self.stats["by_type"]:
            print("By type:")
            for event_type, count in self.stats["by_type"].items():
                print(f"  {event_type}: {count}")
        if self.stats["by_severity"]:
            print("By severity:")
            for severity, count in self.stats["by_severity"].items():
                print(f"  {severity}: {count}")


class AlertReceiver:
    """Receives AEA notifications via UDP socket"""

    def __init__(self, monitor: EasMonitor, host: str = "0.0.0.0", port: int = 8082):
        self.monitor = monitor
        self.host = host
        self.port = port
        self._socket: Optional[socket.socket] = None
        self._running = False
        self._thread: Optional[threading.Thread] = None

    def start(self):
        """Start receiving alert notifications"""
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._socket.bind((self.host, self.port))
        self._socket.settimeout(1.0)

        self._running = True
        self._thread = threading.Thread(target=self._receive_loop, daemon=True)
        self._thread.start()

        print(f"Listening for alerts on UDP {self.host}:{self.port}")

    def stop(self):
        """Stop receiving"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)
        if self._socket:
            self._socket.close()

    def _receive_loop(self):
        """Background thread for receiving alert notifications"""
        while self._running:
            try:
                data, addr = self._socket.recvfrom(65536)
                self.monitor.process_aea(data)
            except socket.timeout:
                continue
            except Exception as e:
                if self._running:
                    print(f"Receive error: {e}", file=sys.stderr)


class SimulatedAlertSource:
    """Generates simulated alerts for testing"""

    def __init__(self, monitor: EasMonitor, interval: float = 10.0):
        self.monitor = monitor
        self.interval = interval
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._counter = 0

    def start(self):
        """Start generating simulated alerts"""
        self._running = True
        self._thread = threading.Thread(target=self._generate_loop, daemon=True)
        self._thread.start()
        print("Generating simulated alerts...")

    def stop(self):
        """Stop generating"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)

    def _generate_loop(self):
        """Generate simulated alerts"""
        # Test alerts of varying severity
        test_alerts = [
            ("TOR", AlertSeverity.EXTREME, "Tornado Warning"),
            ("SVR", AlertSeverity.SEVERE, "Severe Thunderstorm Warning"),
            ("FFW", AlertSeverity.SEVERE, "Flash Flood Warning"),
            ("RWT", AlertSeverity.MINOR, "Required Weekly Test"),
        ]

        while self._running:
            self._counter += 1
            alert_type = test_alerts[self._counter % len(test_alerts)]

            # Create fake AEA message
            aea_data = self._make_test_aea(alert_type[0], alert_type[1], alert_type[2])
            self.monitor.process_aea(aea_data)

            time.sleep(self.interval)

    def _make_test_aea(self, code: str, severity: AlertSeverity, event: str) -> bytes:
        """Create a test AEA message"""
        # Minimal AEA header
        header = struct.pack(
            ">IBBH",
            self._counter,  # aea_id
            0x40,  # type=alert, priority=0
            0x00,  # flags
            0,  # reserved
        )
        header += b"\x00" * 8  # padding

        # CAP XML
        now = datetime.now().isoformat()
        expires = datetime.now().replace(hour=23, minute=59).isoformat()

        cap_xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<alert xmlns="urn:oasis:names:tc:emergency:cap:1.2">
    <identifier>TEST-{self._counter:06d}</identifier>
    <sender>test@example.com</sender>
    <sent>{now}</sent>
    <status>Test</status>
    <msgType>Alert</msgType>
    <scope>Public</scope>
    <info>
        <category>Met</category>
        <event>{event}</event>
        <urgency>Immediate</urgency>
        <severity>{severity.value}</severity>
        <certainty>Observed</certainty>
        <eventCode>
            <valueName>SAME</valueName>
            <value>{code}</value>
        </eventCode>
        <effective>{now}</effective>
        <expires>{expires}</expires>
        <senderName>National Weather Service</senderName>
        <headline>TEST - {event} for Test County</headline>
        <description>TEST ALERT. {event} in effect until further notice.</description>
        <instruction>This is a test. Take shelter if real.</instruction>
        <area>
            <areaDesc>Test County</areaDesc>
            <geocode>
                <valueName>SAME</valueName>
                <value>012345</value>
            </geocode>
        </area>
    </info>
</alert>"""

        return header + cap_xml.encode("utf-8")


def parse_args():
    """Parse command line arguments"""
    parser = argparse.ArgumentParser(
        description="ATSC 3.0 Emergency Alert Monitor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    parser.add_argument(
        "--host",
        default="0.0.0.0",
        help="Listen address for alert notifications",
    )
    parser.add_argument(
        "--port", type=int, default=8082, help="Listen port (default: 8082)"
    )
    parser.add_argument(
        "--log",
        metavar="FILE",
        help="Log alerts to file (JSON lines format)",
    )
    parser.add_argument(
        "--audio",
        action="store_true",
        help="Play audio alert tone (not implemented)",
    )
    parser.add_argument(
        "--simulate",
        action="store_true",
        help="Generate simulated alerts for testing",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=10.0,
        help="Interval between simulated alerts (default: 10s)",
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")

    return parser.parse_args()


def main():
    """Main entry point"""
    args = parse_args()

    # Create monitor
    monitor = EasMonitor(verbose=args.verbose, log_file=args.log)

    # Set up signal handler
    def signal_handler(sig, frame):
        print("\nStopping...")
        monitor.stop()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Start monitoring
    monitor.start()

    print("ATSC 3.0 Emergency Alert Monitor")
    print("=" * 40)
    if args.log:
        print(f"  Logging to: {args.log}")
    print()

    if args.simulate:
        # Use simulated alert source for testing
        source = SimulatedAlertSource(monitor, interval=args.interval)
        source.start()
    else:
        # Use real UDP receiver
        receiver = AlertReceiver(monitor, host=args.host, port=args.port)
        receiver.start()

    # Run indefinitely
    try:
        print("Press Ctrl+C to stop monitoring...")
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass

    # Stop
    monitor.stop()
    if args.simulate:
        source.stop()
    else:
        receiver.stop()

    # Print summary
    monitor.print_summary()

    return 0


if __name__ == "__main__":
    sys.exit(main())
