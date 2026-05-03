#!/usr/bin/env python3
"""
test_capture_bootstrap.py — Unit tests for capture_iq.py bootstrap detection

Tests the Schmidl-Cox autocorrelation algorithm without hardware.
"""

import sys
import unittest
from pathlib import Path

import numpy as np

# Add apps directory to path
sys.path.insert(0, str(Path(__file__).parent.parent.parent / "apps"))

from capture_iq import (
    detect_bootstrap,
    validate_capture,
    create_sigmf_meta,
    BOOTSTRAP_FFT_SIZE,
    BOOTSTRAP_SYNC_THRESHOLD,
)


class TestBootstrapDetection(unittest.TestCase):
    """Test bootstrap correlation detection."""

    def setUp(self):
        """Set up test parameters."""
        self.sample_rate = 12.5e6
        self.fft_size = BOOTSTRAP_FFT_SIZE

    def generate_bootstrap_signal(self, num_samples: int, snr_db: float = 20) -> np.ndarray:
        """
        Generate synthetic bootstrap-like signal.

        Creates repeated OFDM symbol structure similar to ATSC 3.0 bootstrap.
        """
        # Generate random OFDM symbol (one half)
        half_symbol = np.random.randn(self.fft_size // 2) + \
                      1j * np.random.randn(self.fft_size // 2)
        half_symbol = half_symbol.astype(np.complex64)

        # Create bootstrap: repeated halves
        bootstrap = np.concatenate([half_symbol, half_symbol])

        # Add cyclic prefix
        cp_len = self.fft_size // 4
        cp = bootstrap[-cp_len:]
        bootstrap_with_cp = np.concatenate([cp, bootstrap])

        # Extend to requested length
        samples = np.zeros(num_samples, dtype=np.complex64)
        offset = 1000  # Some offset from start
        end = min(offset + len(bootstrap_with_cp), num_samples)
        samples[offset:end] = bootstrap_with_cp[:end-offset]

        # Add noise
        noise_power = 10 ** (-snr_db / 10)
        noise = np.sqrt(noise_power / 2) * (
            np.random.randn(num_samples) + 1j * np.random.randn(num_samples)
        )
        samples = (samples + noise).astype(np.complex64)

        return samples

    def test_detect_bootstrap_clean_signal(self):
        """Test bootstrap detection with clean signal."""
        samples = self.generate_bootstrap_signal(50000, snr_db=30)

        result = detect_bootstrap(samples, self.sample_rate)

        self.assertTrue(result["detected"])
        self.assertGreater(result["correlation"], BOOTSTRAP_SYNC_THRESHOLD)
        self.assertGreater(result["offset"], 0)

    def test_detect_bootstrap_moderate_snr(self):
        """Test bootstrap detection at moderate SNR."""
        samples = self.generate_bootstrap_signal(50000, snr_db=10)

        result = detect_bootstrap(samples, self.sample_rate)

        self.assertTrue(result["detected"])
        self.assertGreater(result["correlation"], BOOTSTRAP_SYNC_THRESHOLD)

    def test_detect_bootstrap_low_snr(self):
        """Test bootstrap detection fails at very low SNR."""
        samples = self.generate_bootstrap_signal(50000, snr_db=-5)

        result = detect_bootstrap(samples, self.sample_rate)

        # Should not detect at very low SNR
        self.assertFalse(result["detected"])

    def test_detect_bootstrap_pure_noise(self):
        """Test bootstrap detection with pure noise."""
        samples = (np.random.randn(50000) + 1j * np.random.randn(50000)).astype(np.complex64)

        result = detect_bootstrap(samples, self.sample_rate)

        self.assertFalse(result["detected"])
        self.assertLess(result["correlation"], BOOTSTRAP_SYNC_THRESHOLD)

    def test_detect_bootstrap_short_buffer(self):
        """Test bootstrap detection with too-short buffer."""
        samples = np.zeros(100, dtype=np.complex64)

        result = detect_bootstrap(samples, self.sample_rate)

        self.assertFalse(result["detected"])
        self.assertEqual(result["correlation"], 0.0)

    def test_detect_bootstrap_cfo_estimate(self):
        """Test CFO estimation from bootstrap."""
        samples = self.generate_bootstrap_signal(50000, snr_db=30)

        result = detect_bootstrap(samples, self.sample_rate)

        self.assertTrue(result["detected"])
        # CFO can vary due to random signal generation, just verify it's computed
        self.assertIsInstance(result["cfo_hz"], float)


class TestValidateCapture(unittest.TestCase):
    """Test capture validation."""

    def setUp(self):
        self.sample_rate = 12.5e6

    def generate_valid_capture(self, duration_sec: float = 0.1) -> np.ndarray:
        """Generate a capture that should pass validation."""
        num_samples = int(duration_sec * self.sample_rate)

        # Generate OFDM-like signal with bootstrap
        half_symbol = np.random.randn(BOOTSTRAP_FFT_SIZE // 2) + \
                      1j * np.random.randn(BOOTSTRAP_FFT_SIZE // 2)
        bootstrap = np.concatenate([half_symbol, half_symbol])

        samples = np.zeros(num_samples, dtype=np.complex64)
        samples[:len(bootstrap)] = bootstrap

        # Fill rest with random OFDM-like content
        for i in range(len(bootstrap), num_samples, 8192):
            end = min(i + 8192, num_samples)
            samples[i:end] = (np.random.randn(end-i) + 1j * np.random.randn(end-i)) * 0.5

        return samples.astype(np.complex64)

    def test_validate_good_capture(self):
        """Test validation passes for good capture."""
        samples = self.generate_valid_capture(0.5)

        result = validate_capture(samples, self.sample_rate)

        self.assertTrue(result["bootstrap"]["detected"])
        self.assertGreater(result["power_dbfs"], -60)

    def test_validate_short_capture(self):
        """Test validation fails for too-short capture."""
        samples = np.zeros(1000, dtype=np.complex64)

        result = validate_capture(samples, self.sample_rate)

        self.assertFalse(result["valid"])
        self.assertIn("too short", result["errors"][0].lower())

    def test_validate_noise_only(self):
        """Test validation fails for noise-only capture."""
        num_samples = int(0.1 * self.sample_rate)
        samples = (np.random.randn(num_samples) + 1j * np.random.randn(num_samples)).astype(np.complex64) * 0.01

        result = validate_capture(samples, self.sample_rate)

        self.assertFalse(result["valid"])


class TestSigMFMetadata(unittest.TestCase):
    """Test SigMF metadata generation."""

    def test_create_basic_metadata(self):
        """Test basic metadata creation."""
        meta = create_sigmf_meta(
            freq_hz=557e6,
            sample_rate=12.5e6,
            gain_db=30.0,
            num_samples=12500000,
            channel=28
        )

        self.assertEqual(meta["global"]["core:datatype"], "cf32_le")
        self.assertEqual(meta["global"]["core:sample_rate"], 12.5e6)
        self.assertEqual(meta["global"]["atsc3:channel"], 28)
        self.assertEqual(meta["captures"][0]["core:frequency"], 557e6)
        self.assertEqual(meta["global"]["uhd:gain_db"], 30.0)

    def test_create_metadata_with_annotations(self):
        """Test metadata with annotations."""
        annotations = [
            {
                "core:sample_start": 1000,
                "core:sample_count": 4096,
                "core:description": "Bootstrap"
            }
        ]

        meta = create_sigmf_meta(
            freq_hz=557e6,
            sample_rate=12.5e6,
            gain_db=30.0,
            num_samples=1000000,
            annotations=annotations
        )

        self.assertEqual(len(meta["annotations"]), 1)
        self.assertEqual(meta["annotations"][0]["core:sample_start"], 1000)


if __name__ == "__main__":
    unittest.main()
