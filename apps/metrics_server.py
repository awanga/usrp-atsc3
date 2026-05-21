#!/usr/bin/env python3
"""
metrics_server.py — ATSC 3.0 Metrics HTTP Server

Provides a simple HTTP endpoint for real-time signal quality metrics.
Serves JSON metrics at /metrics and an HTML dashboard at /.

Usage:
    metrics_server.py [options]

Options:
    --host HOST     Bind address (default: 0.0.0.0)
    --port PORT     Listen port (default: 8080)
    --refresh SECS  Dashboard refresh rate (default: 1.0)
    -v, --verbose   Verbose output
    -h, --help      Show this help message
"""

import argparse
import json
import sys
import threading
import time
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from typing import Dict, Any, Optional
import random


class MetricsStore:
    """Thread-safe storage for current metrics"""

    def __init__(self):
        self._lock = threading.Lock()
        self._metrics: Dict[str, Any] = {
            'rssi_dbm': -120.0,
            'snr_db': 0.0,
            'mer_db': 0.0,
            'evm_percent': 100.0,
            'ber_pre_fec': 0.5,
            'fer': 1.0,
            'ldpc_converged': False,
            'avg_ldpc_iterations': 50.0,
            'signal_locked': False,
            'frame_locked': False,
            'timestamp_ms': 0,
            'uptime_ms': 0,
            'services': []
        }
        self._start_time = time.time()

    def update(self, metrics: Dict[str, Any]):
        """Update metrics with new values"""
        with self._lock:
            self._metrics.update(metrics)
            self._metrics['timestamp_ms'] = int(time.time() * 1000)
            self._metrics['uptime_ms'] = int((time.time() - self._start_time) * 1000)

    def get(self) -> Dict[str, Any]:
        """Get current metrics snapshot"""
        with self._lock:
            return dict(self._metrics)

    def get_json(self, indent: Optional[int] = None) -> str:
        """Get metrics as JSON string"""
        return json.dumps(self.get(), indent=indent)


class SimulatedMetricsUpdater:
    """Generates simulated metrics for testing"""

    def __init__(self, store: MetricsStore, update_rate: float = 1.0):
        self.store = store
        self.update_rate = update_rate
        self._running = False
        self._thread: Optional[threading.Thread] = None

        # Simulation state
        self._rssi_base = -60.0
        self._locked = True

    def start(self):
        """Start background metrics updates"""
        self._running = True
        self._thread = threading.Thread(target=self._update_loop, daemon=True)
        self._thread.start()

    def stop(self):
        """Stop background updates"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)

    def _update_loop(self):
        """Background thread for updating metrics"""
        while self._running:
            self._generate_metrics()
            time.sleep(self.update_rate)

    def _generate_metrics(self):
        """Generate simulated metrics"""
        # Add some variation
        rssi = self._rssi_base + random.gauss(0, 2)

        # Occasionally simulate signal loss
        if random.random() < 0.02:  # 2% chance of drop
            self._locked = False
            rssi = -100 + random.uniform(0, 10)
        elif random.random() < 0.1:  # 10% chance of recovery
            self._locked = True
            self._rssi_base = random.uniform(-70, -40)

        if self._locked:
            snr = rssi + 90 + random.gauss(0, 1)
            mer = snr - random.uniform(0, 3)
            evm = 100 / (10 ** (mer / 20))
            ber = 1e-6 * (10 ** ((30 - snr) / 10))
            iterations = max(1, min(50, 5 + int((30 - snr) * 2)))

            services = [
                {'service_id': 1, 'name': 'ATSC3 Main', 'active': True},
                {'service_id': 2, 'name': 'ATSC3 Sub-1', 'active': True},
            ]
        else:
            snr = 0
            mer = 0
            evm = 100
            ber = 0.5
            iterations = 50
            services = []

        metrics = {
            'rssi_dbm': round(rssi, 1),
            'snr_db': round(snr, 1),
            'mer_db': round(mer, 1),
            'evm_percent': round(evm, 2),
            'ber_pre_fec': ber,
            'fer': 0.0 if self._locked else 1.0,
            'ldpc_converged': self._locked,
            'avg_ldpc_iterations': iterations,
            'signal_locked': self._locked,
            'frame_locked': self._locked,
            'services': services
        }

        self.store.update(metrics)


def generate_dashboard_html(refresh_rate: float) -> str:
    """Generate HTML dashboard page"""
    return f'''<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta http-equiv="refresh" content="{refresh_rate}">
    <title>ATSC 3.0 Metrics Dashboard</title>
    <style>
        body {{
            font-family: monospace;
            background-color: #1a1a2e;
            color: #eef;
            margin: 20px;
        }}
        h1 {{
            color: #00ff88;
            border-bottom: 2px solid #00ff88;
            padding-bottom: 10px;
        }}
        .metrics-table {{
            border-collapse: collapse;
            width: 100%;
            max-width: 600px;
        }}
        .metrics-table th, .metrics-table td {{
            border: 1px solid #444;
            padding: 8px 12px;
            text-align: left;
        }}
        .metrics-table th {{
            background-color: #2a2a4e;
            color: #00ff88;
        }}
        .metrics-table tr:nth-child(even) {{
            background-color: #222238;
        }}
        .value {{
            font-weight: bold;
            color: #fff;
        }}
        .good {{ color: #00ff88; }}
        .warning {{ color: #ffaa00; }}
        .bad {{ color: #ff4444; }}
        .locked {{ color: #00ff88; font-weight: bold; }}
        .unlocked {{ color: #ff4444; font-weight: bold; }}
        .services-list {{
            margin-top: 20px;
        }}
        .service {{
            background-color: #2a2a4e;
            padding: 8px;
            margin: 4px 0;
            border-radius: 4px;
        }}
        .timestamp {{
            color: #888;
            font-size: 0.9em;
            margin-top: 20px;
        }}
        #json-link {{
            display: inline-block;
            margin-top: 20px;
            color: #00aaff;
        }}
    </style>
</head>
<body>
    <h1>ATSC 3.0 Signal Metrics</h1>

    <table class="metrics-table">
        <tr><th>Metric</th><th>Value</th></tr>
        <tr><td>Signal Lock</td><td id="signal_locked">-</td></tr>
        <tr><td>Frame Lock</td><td id="frame_locked">-</td></tr>
        <tr><td>RSSI</td><td id="rssi_dbm">- dBm</td></tr>
        <tr><td>SNR</td><td id="snr_db">- dB</td></tr>
        <tr><td>MER</td><td id="mer_db">- dB</td></tr>
        <tr><td>EVM</td><td id="evm_percent">- %</td></tr>
        <tr><td>Pre-FEC BER</td><td id="ber_pre_fec">-</td></tr>
        <tr><td>FER</td><td id="fer">-</td></tr>
        <tr><td>LDPC Converged</td><td id="ldpc_converged">-</td></tr>
        <tr><td>Avg LDPC Iterations</td><td id="avg_ldpc_iterations">-</td></tr>
    </table>

    <div class="services-list">
        <h2>Services</h2>
        <div id="services">No services detected</div>
    </div>

    <p class="timestamp">
        Last update: <span id="timestamp">-</span><br>
        Uptime: <span id="uptime">-</span>
    </p>

    <a id="json-link" href="/metrics">View raw JSON</a>

    <script>
        async function updateMetrics() {{
            try {{
                const response = await fetch('/metrics');
                const data = await response.json();

                // Update lock status
                document.getElementById('signal_locked').innerHTML =
                    data.signal_locked ?
                    '<span class="locked">LOCKED</span>' :
                    '<span class="unlocked">UNLOCKED</span>';

                document.getElementById('frame_locked').innerHTML =
                    data.frame_locked ?
                    '<span class="locked">LOCKED</span>' :
                    '<span class="unlocked">UNLOCKED</span>';

                // Update metrics with color coding
                const rssi = data.rssi_dbm;
                const rssiClass = rssi > -50 ? 'good' : (rssi > -70 ? 'warning' : 'bad');
                document.getElementById('rssi_dbm').innerHTML =
                    `<span class="${{rssiClass}}">${{rssi.toFixed(1)}} dBm</span>`;

                const snr = data.snr_db;
                const snrClass = snr > 25 ? 'good' : (snr > 15 ? 'warning' : 'bad');
                document.getElementById('snr_db').innerHTML =
                    `<span class="${{snrClass}}">${{snr.toFixed(1)}} dB</span>`;

                const mer = data.mer_db;
                const merClass = mer > 25 ? 'good' : (mer > 15 ? 'warning' : 'bad');
                document.getElementById('mer_db').innerHTML =
                    `<span class="${{merClass}}">${{mer.toFixed(1)}} dB</span>`;

                document.getElementById('evm_percent').textContent =
                    data.evm_percent.toFixed(2) + ' %';

                document.getElementById('ber_pre_fec').textContent =
                    data.ber_pre_fec.toExponential(2);

                document.getElementById('fer').textContent =
                    data.fer.toFixed(4);

                document.getElementById('ldpc_converged').innerHTML =
                    data.ldpc_converged ?
                    '<span class="good">Yes</span>' :
                    '<span class="bad">No</span>';

                document.getElementById('avg_ldpc_iterations').textContent =
                    data.avg_ldpc_iterations.toFixed(1);

                // Update services
                const servicesDiv = document.getElementById('services');
                if (data.services && data.services.length > 0) {{
                    servicesDiv.innerHTML = data.services.map(s =>
                        `<div class="service">${{s.service_id}}: ${{s.name}} (${{s.active ? 'active' : 'inactive'}})</div>`
                    ).join('');
                }} else {{
                    servicesDiv.textContent = 'No services detected';
                }}

                // Update timestamp
                const ts = new Date(data.timestamp_ms);
                document.getElementById('timestamp').textContent = ts.toISOString();

                const uptime = data.uptime_ms / 1000;
                const hours = Math.floor(uptime / 3600);
                const mins = Math.floor((uptime % 3600) / 60);
                const secs = Math.floor(uptime % 60);
                document.getElementById('uptime').textContent =
                    `${{hours}}h ${{mins}}m ${{secs}}s`;

            }} catch (e) {{
                console.error('Failed to fetch metrics:', e);
            }}
        }}

        // Initial update
        updateMetrics();

        // Auto-refresh
        setInterval(updateMetrics, {int(refresh_rate * 1000)});
    </script>
</body>
</html>
'''


class MetricsHandler(BaseHTTPRequestHandler):
    """HTTP request handler for metrics endpoints"""

    # Class-level references (set by server)
    metrics_store: MetricsStore = None
    refresh_rate: float = 1.0
    verbose: bool = False

    def log_message(self, format, *args):
        """Override to control logging"""
        if self.verbose:
            super().log_message(format, *args)

    def do_GET(self):
        """Handle GET requests"""
        if self.path == '/metrics':
            self._serve_json()
        elif self.path == '/' or self.path == '/index.html':
            self._serve_dashboard()
        elif self.path == '/favicon.ico':
            self._serve_404()
        else:
            self._serve_404()

    def _serve_json(self):
        """Serve metrics as JSON"""
        content = self.metrics_store.get_json(indent=2)
        self._send_response(200, 'application/json', content)

    def _serve_dashboard(self):
        """Serve HTML dashboard"""
        content = generate_dashboard_html(self.refresh_rate)
        self._send_response(200, 'text/html', content)

    def _serve_404(self):
        """Serve 404 response"""
        self._send_response(404, 'text/plain', 'Not Found')

    def _send_response(self, code: int, content_type: str, content: str):
        """Send HTTP response"""
        self.send_response(code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', len(content))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(content.encode('utf-8'))


def parse_args():
    """Parse command line arguments"""
    parser = argparse.ArgumentParser(
        description='ATSC 3.0 Metrics HTTP Server',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )

    parser.add_argument('--host', default='0.0.0.0',
                        help='Bind address (default: 0.0.0.0)')
    parser.add_argument('--port', type=int, default=8080,
                        help='Listen port (default: 8080)')
    parser.add_argument('--refresh', type=float, default=1.0, metavar='SECS',
                        help='Dashboard refresh rate (default: 1.0)')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Verbose output')

    return parser.parse_args()


def main():
    """Main entry point"""
    args = parse_args()

    # Create metrics store
    store = MetricsStore()

    # Start simulated metrics updater
    updater = SimulatedMetricsUpdater(store, update_rate=args.refresh)
    updater.start()

    # Configure handler
    MetricsHandler.metrics_store = store
    MetricsHandler.refresh_rate = args.refresh
    MetricsHandler.verbose = args.verbose

    # Create and start server
    server = HTTPServer((args.host, args.port), MetricsHandler)

    print(f"ATSC 3.0 Metrics Server")
    print(f"  Dashboard: http://{args.host}:{args.port}/")
    print(f"  JSON API:  http://{args.host}:{args.port}/metrics")
    print(f"  Refresh:   {args.refresh}s")
    print()
    print("Press Ctrl+C to stop")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        updater.stop()
        server.shutdown()

    return 0


if __name__ == '__main__':
    sys.exit(main())
