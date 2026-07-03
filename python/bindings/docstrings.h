// docstrings.h — Python docstrings for gr-atsc3 blocks
#pragma once

// bootstrap_detect docstrings
[[maybe_unused]] static const char* DOC_BOOTSTRAP_DETECT = R"doc(
ATSC 3.0 Bootstrap Detector Block

Detects ATSC 3.0 bootstrap signals and provides coarse CFO estimation.

Input: IQ samples at 6.144 MS/s
Output: IQ samples with bootstrap timing tag, coarse CFO estimate

AXI4-S: TDATA=cf32 TVALID TREADY TLAST(frame)
)doc";

[[maybe_unused]] static const char* DOC_BOOTSTRAP_DETECT_MAKE = R"doc(
Create ATSC 3.0 bootstrap detector block

Args:
    threshold: Detection threshold (0.0-1.0, default 0.7)

Returns:
    Shared pointer to new bootstrap_detect instance
)doc";

[[maybe_unused]] static const char* DOC_BOOTSTRAP_DETECT_GET_CFO_HZ = R"doc(
Get current coarse CFO estimate in Hz

Returns:
    Carrier frequency offset estimate in Hz
)doc";

[[maybe_unused]] static const char* DOC_BOOTSTRAP_DETECT_IS_LOCKED = R"doc(
Check if bootstrap is currently locked

Returns:
    True if bootstrap is detected and locked
)doc";

[[maybe_unused]] static const char* DOC_BOOTSTRAP_DETECT_SET_THRESHOLD = R"doc(
Set detection threshold

Args:
    threshold: Detection threshold (0.0-1.0)
)doc";

// ofdm_demod docstrings
[[maybe_unused]] static const char* DOC_OFDM_DEMOD = R"doc(
ATSC 3.0 OFDM Demodulator Block

Performs FFT-based OFDM demodulation with cyclic prefix removal.

Input: Time-domain IQ samples (CFO corrected)
Output: Frequency-domain subcarriers per OFDM symbol

AXI4-S: TDATA=cf32 TVALID TREADY TLAST(symbol)
)doc";

[[maybe_unused]] static const char* DOC_OFDM_DEMOD_MAKE = R"doc(
Create ATSC 3.0 OFDM demodulator block

Args:
    fft_size: FFT size (8192, 16384, or 32768)
    cp_length: Cyclic prefix length in samples

Returns:
    Shared pointer to new ofdm_demod instance
)doc";

[[maybe_unused]] static const char* DOC_OFDM_DEMOD_SET_FFT_SIZE = R"doc(
Set FFT size (reconfigurable via L1 signaling)

Args:
    fft_size: FFT size (8192, 16384, or 32768)
)doc";

[[maybe_unused]] static const char* DOC_OFDM_DEMOD_SET_CP_LENGTH = R"doc(
Set cyclic prefix length

Args:
    cp_length: Cyclic prefix length in samples
)doc";

[[maybe_unused]] static const char* DOC_OFDM_DEMOD_GET_FFT_SIZE = R"doc(
Get current FFT size

Returns:
    Current FFT size
)doc";

[[maybe_unused]] static const char* DOC_OFDM_DEMOD_GET_CP_LENGTH = R"doc(
Get current CP length

Returns:
    Current cyclic prefix length in samples
)doc";

// channel_eq docstrings
[[maybe_unused]] static const char* DOC_CHANNEL_EQ = R"doc(
ATSC 3.0 Channel Equalizer Block

Performs channel estimation and equalization using scattered pilots.

Input: Frequency-domain OFDM symbols
Output: Equalized QAM symbols

AXI4-S: TDATA=cf32 TVALID TREADY TLAST(symbol)
)doc";

[[maybe_unused]] static const char* DOC_CHANNEL_EQ_MAKE = R"doc(
Create ATSC 3.0 channel equalizer block

Args:
    fft_size: FFT size
    pilot_pattern: Pilot pattern (1-8)
    use_mmse: Use MMSE equalization (vs ZF)

Returns:
    Shared pointer to new channel_eq instance
)doc";

[[maybe_unused]] static const char* DOC_CHANNEL_EQ_SET_PILOT_PATTERN = R"doc(
Set pilot pattern

Args:
    pattern: Pilot pattern index (1-8)
)doc";

[[maybe_unused]] static const char* DOC_CHANNEL_EQ_SET_MMSE = R"doc(
Set equalization mode

Args:
    use_mmse: True for MMSE, False for Zero-Forcing
)doc";

[[maybe_unused]] static const char* DOC_CHANNEL_EQ_GET_SNR_DB = R"doc(
Get current SNR estimate in dB

Returns:
    Estimated signal-to-noise ratio in dB
)doc";

[[maybe_unused]] static const char* DOC_CHANNEL_EQ_GET_MER_DB = R"doc(
Get current MER estimate in dB

Returns:
    Modulation error ratio in dB
)doc";

// fec_decode docstrings
[[maybe_unused]] static const char* DOC_FEC_DECODE = R"doc(
ATSC 3.0 FEC Decoder Block

LDPC and BCH decoder for ATSC 3.0.

Input: Soft LLR values from demapper
Output: Decoded bits

AXI4-S: TDATA=int8(LLR) TVALID TREADY TLAST(codeword)
)doc";

[[maybe_unused]] static const char* DOC_FEC_DECODE_MAKE = R"doc(
Create ATSC 3.0 FEC decoder block

Args:
    code_rate: LDPC code rate index (0-11)
    codeword_length: Codeword length (16200 or 64800)
    max_iterations: Maximum LDPC iterations

Returns:
    Shared pointer to new fec_decode instance
)doc";

[[maybe_unused]] static const char* DOC_FEC_DECODE_SET_CODE_RATE = R"doc(
Set LDPC code rate

Args:
    code_rate: Code rate index (0-11)
)doc";

[[maybe_unused]] static const char* DOC_FEC_DECODE_SET_CODEWORD_LENGTH = R"doc(
Set codeword length

Args:
    length: Codeword length (16200 or 64800)
)doc";

[[maybe_unused]] static const char* DOC_FEC_DECODE_GET_AVG_ITERATIONS = R"doc(
Get average LDPC iterations

Returns:
    Average number of iterations for convergence
)doc";

[[maybe_unused]] static const char* DOC_FEC_DECODE_GET_FER = R"doc(
Get frame error rate

Returns:
    Frame error rate (0.0-1.0)
)doc";

[[maybe_unused]] static const char* DOC_FEC_DECODE_LAST_CONVERGED = R"doc(
Check if last codeword converged

Returns:
    True if the last codeword successfully converged
)doc";

// alp_demux docstrings
[[maybe_unused]] static const char* DOC_ALP_DEMUX = R"doc(
ATSC 3.0 ALP Demultiplexer Block

Extracts ALP packets from decoded baseband frames.

Input: Decoded baseband frames
Output: ALP packets (IP or MPEG-TS encapsulated)

AXI4-S: TDATA=uint8 TVALID TREADY TLAST(packet) TID(plp_id)
)doc";

[[maybe_unused]] static const char* DOC_ALP_DEMUX_MAKE = R"doc(
Create ATSC 3.0 ALP demultiplexer block

Args:
    plp_id: PLP ID to extract (0-63, or -1 for all)

Returns:
    Shared pointer to new alp_demux instance
)doc";

[[maybe_unused]] static const char* DOC_ALP_DEMUX_SET_PLP_ID = R"doc(
Set PLP ID to extract

Args:
    plp_id: PLP ID (0-63, or -1 for all)
)doc";

[[maybe_unused]] static const char* DOC_ALP_DEMUX_GET_PLP_ID = R"doc(
Get current PLP ID

Returns:
    Current PLP ID filter value
)doc";

[[maybe_unused]] static const char* DOC_ALP_DEMUX_GET_PACKET_COUNT = R"doc(
Get packet count

Returns:
    Total number of ALP packets extracted
)doc";

[[maybe_unused]] static const char* DOC_ALP_DEMUX_GET_ERROR_COUNT = R"doc(
Get error count (CRC failures, etc.)

Returns:
    Total number of packet errors
)doc";
