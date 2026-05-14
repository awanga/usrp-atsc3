#!/usr/bin/env python3
"""
capture_iq.py — ATSC 3.0 IQ Capture Utility for USRP N2x0 + TVRX

Captures IQ samples from USRP hardware, verifies ATSC 3.0 signal presence
via bootstrap correlation, and saves to SigMF format for integration testing.

Usage:
    # Capture channel 28 for 10 seconds
    ./capture_iq.py --channel 28 --duration 10

    # Capture specific frequency with custom gain
    ./capture_iq.py --freq 557e6 --gain 35 --duration 5

    # Scan for active ATSC 3.0 channels
    ./capture_iq.py --scan

Requirements:
    - USRP N2x0 with TVRX daughterboard
    - python3-uhd package
    - numpy

Reference: ATSC A/322 Section 5 (Bootstrap)
"""

import argparse
import datetime
import json
import os
import sys
import time
from pathlib import Path

try:
    import numpy as np
except ImportError:
    print("ERROR: numpy required. Install with: pip install numpy")
    sys.exit(1)

# UHD is optional - only required for hardware capture
try:
    import uhd
    UHD_AVAILABLE = True
except ImportError:
    uhd = None
    UHD_AVAILABLE = False


# =============================================================================
# Constants
# =============================================================================

# ATSC 3.0 channel plan (US)
CHANNEL_PLAN = {
    ch: 473e6 + 6e6 * (ch - 14) for ch in range(14, 52) if ch != 37
}

# Default capture parameters
DEFAULT_SAMPLE_RATE = 12.5e6  # 12.5 MS/s (2x oversampling for 6 MHz)
DEFAULT_GAIN = 30.0           # dB
DEFAULT_DURATION = 10.0       # seconds
DEFAULT_OUTPUT_DIR = "test/captures"

# TVRX constraints
TVRX_FREQ_MIN = 50e6
TVRX_FREQ_MAX = 860e6
TVRX_GAIN_MIN = 0.0
TVRX_GAIN_MAX = 36.5

# Bootstrap correlation parameters
BOOTSTRAP_FFT_SIZE = 4096  # Bootstrap always 4K OFDM
BOOTSTRAP_SYNC_THRESHOLD = 0.6  # Correlation threshold
BOOTSTRAP_SEARCH_SAMPLES = 500000  # ~40ms at 12.5 MS/s

# Signal quality thresholds
RSSI_MIN_DBM = -80.0  # Minimum usable signal level


# =============================================================================
# SigMF Metadata
# =============================================================================

def create_sigmf_meta(
    freq_hz: float,
    sample_rate: float,
    gain_db: float,
    num_samples: int,
    channel: int = None,
    capture_time: datetime.datetime = None,
    annotations: list = None
) -> dict:
    """Create SigMF metadata structure."""
    if capture_time is None:
        capture_time = datetime.datetime.utcnow()

    meta = {
        "global": {
            "core:datatype": "cf32_le",  # complex float32 little-endian
            "core:sample_rate": sample_rate,
            "core:version": "1.0.0",
            "core:description": f"ATSC 3.0 capture from USRP N2x0 + TVRX",
            "core:author": "gr-atsc3 capture_iq.py",
            "core:recorder": "UHD",
            "atsc3:channel": channel,
        },
        "captures": [
            {
                "core:sample_start": 0,
                "core:frequency": freq_hz,
                "core:datetime": capture_time.isoformat() + "Z",
            }
        ],
        "annotations": annotations or []
    }

    # Add gain as extension
    meta["global"]["uhd:gain_db"] = gain_db

    return meta


def write_sigmf(
    filepath: Path,
    samples: np.ndarray,
    metadata: dict
):
    """Write SigMF data and metadata files."""
    # Write binary data (.sigmf-data)
    data_path = filepath.with_suffix(".sigmf-data")
    samples.astype(np.complex64).tofile(data_path)

    # Write metadata (.sigmf-meta)
    meta_path = filepath.with_suffix(".sigmf-meta")
    with open(meta_path, "w") as f:
        json.dump(metadata, f, indent=2)

    print(f"  Wrote: {data_path} ({data_path.stat().st_size / 1e6:.1f} MB)")
    print(f"  Wrote: {meta_path}")

    return data_path, meta_path


# =============================================================================
# Bootstrap Correlation (ATSC 3.0 Detection)
# =============================================================================

def detect_bootstrap(samples: np.ndarray, sample_rate: float) -> dict:
    """
    Detect ATSC 3.0 bootstrap using Schmidl-Cox autocorrelation.

    The bootstrap uses a known OFDM structure with repeated patterns
    that create a correlation peak when the signal is present.

    Returns dict with:
        detected: bool - True if bootstrap found
        correlation: float - Peak correlation value
        offset: int - Sample offset of bootstrap start
        cfo_hz: float - Coarse CFO estimate
    """
    result = {
        "detected": False,
        "correlation": 0.0,
        "offset": 0,
        "cfo_hz": 0.0
    }

    if len(samples) < BOOTSTRAP_FFT_SIZE * 2:
        return result

    # Bootstrap has CP + repeated halves structure
    # Use Schmidl-Cox metric: P[d] = sum(r[n+d] * conj(r[n+d+L]))
    # where L = FFT_SIZE / 2

    L = BOOTSTRAP_FFT_SIZE // 2
    search_len = min(len(samples) - 2*L, BOOTSTRAP_SEARCH_SAMPLES)

    if search_len <= 0:
        return result

    # Vectorized Schmidl-Cox using sliding window correlation
    # Create views for the two halves
    first_half = samples[:search_len + L]
    second_half = samples[L:search_len + 2*L]

    # Element-wise product for correlation
    prod = first_half[:search_len + L] * np.conj(second_half[:search_len + L])

    # Use convolution with ones for sliding sum (fast)
    window = np.ones(L)
    P = np.convolve(prod, window, mode='valid')[:search_len]

    # Energy of second half
    energy = np.abs(second_half[:search_len + L])**2
    R = np.convolve(energy, window, mode='valid')[:search_len]

    # Schmidl-Cox metric
    with np.errstate(divide='ignore', invalid='ignore'):
        M = np.abs(P)**2 / (R**2 + 1e-10)

    # Find peak
    peak_idx = np.argmax(M)
    peak_val = M[peak_idx]

    result["correlation"] = float(peak_val)
    result["offset"] = int(peak_idx)

    if peak_val > BOOTSTRAP_SYNC_THRESHOLD:
        result["detected"] = True

        # Estimate CFO from phase of correlation
        phase = np.angle(P[peak_idx])
        cfo_norm = phase / (2 * np.pi)  # Normalized to subcarrier spacing
        result["cfo_hz"] = float(cfo_norm * sample_rate / L)

    return result


# =============================================================================
# Signal Chain Validation
# =============================================================================

def validate_capture(samples: np.ndarray, sample_rate: float) -> dict:
    """
    Run signal chain validation on captured samples.

    Tests:
    1. Bootstrap detection
    2. Signal power / SNR estimation
    3. Basic spectral check

    Returns dict with validation results.
    """
    results = {
        "valid": False,
        "bootstrap": None,
        "power_dbfs": -100.0,
        "snr_estimate_db": 0.0,
        "bandwidth_occupied": 0.0,
        "errors": []
    }

    # Check minimum length
    if len(samples) < BOOTSTRAP_FFT_SIZE * 10:
        results["errors"].append("Capture too short for validation")
        return results

    # 1. Bootstrap detection
    bootstrap = detect_bootstrap(samples[:BOOTSTRAP_SEARCH_SAMPLES], sample_rate)
    results["bootstrap"] = bootstrap

    if not bootstrap["detected"]:
        results["errors"].append("No ATSC 3.0 bootstrap detected")
        return results

    # 2. Signal power
    power = np.mean(np.abs(samples)**2)
    results["power_dbfs"] = float(10 * np.log10(power + 1e-20))

    # 3. SNR estimate (simple: signal vs noise floor outside channel)
    # Use FFT to look at spectrum
    fft_size = 8192
    num_ffts = min(len(samples) // fft_size, 100)

    if num_ffts > 0:
        spectrum = np.zeros(fft_size)
        for i in range(num_ffts):
            chunk = samples[i*fft_size:(i+1)*fft_size]
            spectrum += np.abs(np.fft.fftshift(np.fft.fft(chunk)))**2
        spectrum /= num_ffts

        # Signal: center 70% of bandwidth
        center = fft_size // 2
        signal_bins = int(fft_size * 0.35)
        signal_power = np.mean(spectrum[center-signal_bins:center+signal_bins])

        # Noise: outer 10% on each side
        noise_bins = int(fft_size * 0.05)
        noise_power = np.mean(np.concatenate([
            spectrum[:noise_bins],
            spectrum[-noise_bins:]
        ]))

        if noise_power > 0:
            results["snr_estimate_db"] = float(10 * np.log10(signal_power / noise_power))

        # Bandwidth occupancy (bins above noise floor)
        noise_threshold = noise_power * 10
        occupied = np.sum(spectrum > noise_threshold)
        results["bandwidth_occupied"] = float(occupied / fft_size * sample_rate / 1e6)

    # Validation criteria
    if (bootstrap["detected"] and
        results["power_dbfs"] > -40 and
        results["snr_estimate_db"] > 5):
        results["valid"] = True

    return results


# =============================================================================
# USRP Interface
# =============================================================================

class USRPCapture:
    """USRP N2x0 capture interface."""

    def __init__(self, device_args: str = ""):
        self.usrp = None
        self.streamer = None
        self.device_args = device_args

    def connect(self) -> bool:
        """Connect to USRP device."""
        if not UHD_AVAILABLE:
            print("ERROR: python3-uhd required. Install with: apt install python3-uhd")
            return False
        try:
            self.usrp = uhd.usrp.MultiUSRP(self.device_args)
            rx_info = self.usrp.get_usrp_rx_info()
            print(f"Connected to: {self.usrp.get_mboard_name()}")
            print(f"  Serial: {rx_info.get('mboard_serial', 'Unknown')}")
            print(f"  Daughterboard: {rx_info.get('rx_subdev_name', 'Unknown')}")

            return True

        except Exception as e:
            print(f"ERROR: Failed to connect to USRP: {e}")
            return False

    def configure(
        self,
        freq_hz: float,
        sample_rate: float,
        gain_db: float
    ) -> bool:
        """Configure USRP for capture."""
        if self.usrp is None:
            return False

        try:
            # Validate TVRX constraints
            if not (TVRX_FREQ_MIN <= freq_hz <= TVRX_FREQ_MAX):
                print(f"ERROR: Frequency {freq_hz/1e6:.3f} MHz outside TVRX range")
                return False

            gain_db = max(TVRX_GAIN_MIN, min(TVRX_GAIN_MAX, gain_db))

            # Configure
            self.usrp.set_rx_rate(sample_rate)
            self.usrp.set_rx_freq(uhd.types.TuneRequest(freq_hz))
            self.usrp.set_rx_gain(gain_db)

            # Verify
            actual_rate = self.usrp.get_rx_rate()
            actual_freq = self.usrp.get_rx_freq()
            actual_gain = self.usrp.get_rx_gain()

            print(f"  Frequency: {actual_freq/1e6:.3f} MHz")
            print(f"  Sample rate: {actual_rate/1e6:.3f} MS/s")
            print(f"  Gain: {actual_gain:.1f} dB")

            # Create streamer
            stream_args = uhd.usrp.StreamArgs("fc32", "sc16")
            self.streamer = self.usrp.get_rx_stream(stream_args)

            return True

        except Exception as e:
            print(f"ERROR: Failed to configure USRP: {e}")
            return False

    def capture(self, num_samples: int, timeout: float = 5.0) -> np.ndarray:
        """Capture samples from USRP."""
        if self.streamer is None:
            return np.array([], dtype=np.complex64)

        try:
            # Allocate buffer
            samples = np.zeros(num_samples, dtype=np.complex64)

            # Start streaming
            stream_cmd = uhd.types.StreamCMD(uhd.types.StreamMode.num_done)
            stream_cmd.num_samps = num_samples
            stream_cmd.stream_now = True
            self.streamer.issue_stream_cmd(stream_cmd)

            # Receive
            metadata = uhd.types.RXMetadata()
            recv_samples = 0
            chunk_size = min(10000, num_samples)

            start_time = time.time()
            while recv_samples < num_samples:
                if time.time() - start_time > timeout:
                    print(f"WARNING: Capture timeout after {recv_samples} samples")
                    break

                remaining = num_samples - recv_samples
                chunk = min(chunk_size, remaining)

                n = self.streamer.recv(samples[recv_samples:recv_samples+chunk], metadata)
                recv_samples += n

                if metadata.error_code != uhd.types.RXMetadataErrorCode.none:
                    if metadata.error_code == uhd.types.RXMetadataErrorCode.overflow:
                        print("WARNING: Overflow detected")
                    elif metadata.error_code == uhd.types.RXMetadataErrorCode.timeout:
                        print("WARNING: Timeout")
                        break

            return samples[:recv_samples]

        except Exception as e:
            print(f"ERROR: Capture failed: {e}")
            return np.array([], dtype=np.complex64)

    def get_rssi(self) -> float:
        """Get RSSI from TVRX sensor."""
        try:
            return self.usrp.get_rx_sensor("rssi").to_real()
        except Exception:
            return -100.0

    def close(self):
        """Clean up."""
        self.streamer = None
        self.usrp = None


# =============================================================================
# Channel Scanner
# =============================================================================

def scan_channels(
    usrp: USRPCapture,
    sample_rate: float = DEFAULT_SAMPLE_RATE,
    gain_db: float = DEFAULT_GAIN
) -> list:
    """Scan all channels for ATSC 3.0 signals."""
    active_channels = []

    print("\nScanning for ATSC 3.0 signals...")
    print("-" * 60)

    for channel, freq in sorted(CHANNEL_PLAN.items()):
        print(f"  Channel {channel:2d} ({freq/1e6:.1f} MHz): ", end="", flush=True)

        if not usrp.configure(freq, sample_rate, gain_db):
            print("Config failed")
            continue

        # Quick capture for detection
        samples = usrp.capture(BOOTSTRAP_SEARCH_SAMPLES)
        if len(samples) == 0:
            print("No data")
            continue

        # Check for bootstrap
        bootstrap = detect_bootstrap(samples, sample_rate)
        rssi = usrp.get_rssi()

        if bootstrap["detected"]:
            print(f"ATSC 3.0 FOUND (corr={bootstrap['correlation']:.2f}, RSSI={rssi:.1f} dBm)")
            active_channels.append({
                "channel": channel,
                "freq_hz": freq,
                "correlation": bootstrap["correlation"],
                "rssi_dbm": rssi,
                "cfo_hz": bootstrap["cfo_hz"]
            })
        else:
            print(f"No signal (RSSI={rssi:.1f} dBm)")

    print("-" * 60)
    print(f"Found {len(active_channels)} active ATSC 3.0 channel(s)")

    return active_channels


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="ATSC 3.0 IQ Capture Utility for USRP N2x0 + TVRX",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --channel 28 --duration 10
  %(prog)s --freq 557e6 --gain 35 --duration 5
  %(prog)s --scan
  %(prog)s --channel 28 --validate-only
        """
    )

    # Capture options
    parser.add_argument("--channel", "-c", type=int,
                        help="ATSC 3.0 channel number (14-51, except 37)")
    parser.add_argument("--freq", "-f", type=float,
                        help="Center frequency in Hz (alternative to --channel)")
    parser.add_argument("--duration", "-d", type=float, default=DEFAULT_DURATION,
                        help=f"Capture duration in seconds (default: {DEFAULT_DURATION})")
    parser.add_argument("--rate", "-r", type=float, default=DEFAULT_SAMPLE_RATE,
                        help=f"Sample rate in S/s (default: {DEFAULT_SAMPLE_RATE/1e6} MS/s)")
    parser.add_argument("--gain", "-g", type=float, default=DEFAULT_GAIN,
                        help=f"Gain in dB (default: {DEFAULT_GAIN})")

    # Output options
    parser.add_argument("--output", "-o", type=str,
                        help="Output file path (without extension)")
    parser.add_argument("--output-dir", type=str, default=DEFAULT_OUTPUT_DIR,
                        help=f"Output directory (default: {DEFAULT_OUTPUT_DIR})")

    # Device options
    parser.add_argument("--device", type=str, default="",
                        help="UHD device args (e.g., 'addr=192.168.10.2')")

    # Mode options
    parser.add_argument("--scan", action="store_true",
                        help="Scan all channels for ATSC 3.0 signals")
    parser.add_argument("--validate-only", action="store_true",
                        help="Validate signal without saving capture")
    parser.add_argument("--skip-validation", action="store_true",
                        help="Skip post-capture validation")
    parser.add_argument("--force", action="store_true",
                        help="Save capture even if validation fails")

    args = parser.parse_args()

    # Determine frequency
    if args.channel:
        if args.channel not in CHANNEL_PLAN:
            print(f"ERROR: Invalid channel {args.channel}. Valid: 14-36, 38-51")
            return 1
        freq_hz = CHANNEL_PLAN[args.channel]
        channel = args.channel
    elif args.freq:
        freq_hz = args.freq
        channel = None
        # Find matching channel if any
        for ch, f in CHANNEL_PLAN.items():
            if abs(f - freq_hz) < 100e3:
                channel = ch
                break
    elif not args.scan:
        print("ERROR: Must specify --channel, --freq, or --scan")
        return 1
    else:
        freq_hz = None
        channel = None

    # Connect to USRP
    print("=" * 60)
    print("ATSC 3.0 IQ Capture Utility")
    print("=" * 60)

    usrp = USRPCapture(args.device)
    if not usrp.connect():
        return 1

    # Scan mode
    if args.scan:
        active = scan_channels(usrp, args.rate, args.gain)
        usrp.close()
        if active:
            print("\nTo capture, run:")
            for ch in active:
                print(f"  {sys.argv[0]} --channel {ch['channel']} --duration {args.duration}")
        return 0

    # Configure
    print(f"\nConfiguring for capture...")
    if channel:
        print(f"  Channel: {channel}")
    if not usrp.configure(freq_hz, args.rate, args.gain):
        usrp.close()
        return 1

    # Quick validation
    print(f"\nChecking for ATSC 3.0 signal...")
    test_samples = usrp.capture(BOOTSTRAP_SEARCH_SAMPLES)
    bootstrap = detect_bootstrap(test_samples, args.rate)

    if not bootstrap["detected"]:
        print(f"  WARNING: No ATSC 3.0 bootstrap detected (corr={bootstrap['correlation']:.3f})")
        if not args.force:
            print("  Use --force to capture anyway")
            usrp.close()
            return 1
    else:
        print(f"  Bootstrap detected: corr={bootstrap['correlation']:.3f}, CFO={bootstrap['cfo_hz']:.1f} Hz")

    if args.validate_only:
        print("\nValidate-only mode, not saving capture.")
        usrp.close()
        return 0

    # Capture
    num_samples = int(args.duration * args.rate)
    print(f"\nCapturing {args.duration:.1f} seconds ({num_samples:,} samples)...")

    start_time = time.time()
    samples = usrp.capture(num_samples, timeout=args.duration + 5)
    elapsed = time.time() - start_time

    print(f"  Captured {len(samples):,} samples in {elapsed:.1f}s")

    rssi = usrp.get_rssi()
    print(f"  RSSI: {rssi:.1f} dBm")

    usrp.close()

    if len(samples) < num_samples * 0.9:
        print(f"  WARNING: Only captured {100*len(samples)/num_samples:.1f}% of requested samples")

    # Validate capture
    validation = None
    if not args.skip_validation:
        print(f"\nValidating capture...")
        validation = validate_capture(samples, args.rate)

        print(f"  Bootstrap: {'Yes' if validation['bootstrap']['detected'] else 'No'}")
        print(f"  Power: {validation['power_dbfs']:.1f} dBFS")
        print(f"  SNR estimate: {validation['snr_estimate_db']:.1f} dB")
        print(f"  Bandwidth: {validation['bandwidth_occupied']:.1f} MHz")

        if not validation["valid"]:
            print(f"  VALIDATION FAILED: {', '.join(validation['errors'])}")
            if not args.force:
                print("  Use --force to save anyway")
                return 1

    # Generate output filename
    if args.output:
        output_path = Path(args.output)
    else:
        output_dir = Path(args.output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)

        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        if channel:
            filename = f"ch{channel:02d}_{int(args.duration)}sec_{timestamp}"
        else:
            filename = f"f{freq_hz/1e6:.1f}MHz_{int(args.duration)}sec_{timestamp}"
        output_path = output_dir / filename

    # Create SigMF metadata
    annotations = []
    if validation and validation["bootstrap"]["detected"]:
        annotations.append({
            "core:sample_start": validation["bootstrap"]["offset"],
            "core:sample_count": BOOTSTRAP_FFT_SIZE,
            "core:description": "Bootstrap sync point",
            "atsc3:cfo_hz": validation["bootstrap"]["cfo_hz"]
        })

    metadata = create_sigmf_meta(
        freq_hz=freq_hz,
        sample_rate=args.rate,
        gain_db=args.gain,
        num_samples=len(samples),
        channel=channel,
        annotations=annotations
    )

    # Add validation results to metadata
    if validation:
        metadata["global"]["atsc3:validation"] = {
            "power_dbfs": validation["power_dbfs"],
            "snr_estimate_db": validation["snr_estimate_db"],
            "bandwidth_mhz": validation["bandwidth_occupied"],
            "valid": validation["valid"]
        }

    # Write files
    print(f"\nWriting capture...")
    write_sigmf(output_path, samples, metadata)

    # Summary
    print("\n" + "=" * 60)
    print("Capture complete!")
    if channel:
        print(f"  Channel: {channel} ({freq_hz/1e6:.3f} MHz)")
    else:
        print(f"  Frequency: {freq_hz/1e6:.3f} MHz")
    print(f"  Duration: {len(samples)/args.rate:.1f} seconds")
    print(f"  Samples: {len(samples):,}")
    print(f"  File size: {len(samples) * 8 / 1e6:.1f} MB")
    if validation:
        print(f"  Validation: {'PASSED' if validation['valid'] else 'FAILED'}")
    print("=" * 60)

    return 0


if __name__ == "__main__":
    sys.exit(main())
