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

// constellation_demapper docstrings
[[maybe_unused]] static const char* DOC_CONSTELLATION_DEMAPPER = R"doc(
ATSC 3.0 Constellation Demapper Block

Converts equalized QAM symbols to soft LLR values for LDPC decoding.
Supports uniform QAM and non-uniform constellations (NUC).

Input: Equalized QAM symbols (complex)
Output: Soft LLRs (int8_t, bits_per_symbol per input)

AXI4-S: TDATA=cf32 in, int8_t out TVALID TREADY TLAST(codeword)
)doc";

[[maybe_unused]] static const char* DOC_CONSTELLATION_DEMAPPER_MAKE = R"doc(
Create ATSC 3.0 constellation demapper block

Args:
    modulation: Modulation type (0=QPSK, 1=QAM16, 2=QAM64, etc.)
    code_rate: Code rate index (0-11, for NUC selection)
    noise_variance: Noise variance for LLR scaling

Returns:
    Shared pointer to new constellation_demapper instance
)doc";

[[maybe_unused]] static const char* DOC_CONSTELLATION_DEMAPPER_SET_MODULATION = R"doc(
Set modulation type

Args:
    modulation: Modulation type index
)doc";

[[maybe_unused]] static const char* DOC_CONSTELLATION_DEMAPPER_SET_CODE_RATE = R"doc(
Set code rate (for NUC table selection)

Args:
    code_rate: Code rate index (0-11)
)doc";

[[maybe_unused]] static const char* DOC_CONSTELLATION_DEMAPPER_SET_NOISE_VARIANCE = R"doc(
Set noise variance for LLR scaling

Args:
    noise_variance: Noise variance estimate
)doc";

[[maybe_unused]] static const char* DOC_CONSTELLATION_DEMAPPER_GET_BITS_PER_SYMBOL = R"doc(
Get bits per symbol for current modulation

Returns:
    Number of bits per QAM symbol
)doc";

// cell_deinterleaver docstrings
[[maybe_unused]] static const char* DOC_CELL_DEINTERLEAVER = R"doc(
ATSC 3.0 Cell De-interleaver Block

Reverses cell interleaving applied at transmitter.
Uses bit-reversal permutation based on FFT size.

Input: Interleaved LLR cells
Output: De-interleaved LLR cells

AXI4-S: TDATA=int8_t TVALID TREADY TLAST(FEC block)
)doc";

[[maybe_unused]] static const char* DOC_CELL_DEINTERLEAVER_MAKE = R"doc(
Create ATSC 3.0 cell de-interleaver block

Args:
    fft_size: FFT size (determines permutation size)

Returns:
    Shared pointer to new cell_deinterleaver instance
)doc";

[[maybe_unused]] static const char* DOC_CELL_DEINTERLEAVER_SET_FFT_SIZE = R"doc(
Set FFT size (reconfigures permutation)

Args:
    fft_size: FFT size (8192, 16384, or 32768)
)doc";

[[maybe_unused]] static const char* DOC_CELL_DEINTERLEAVER_GET_FFT_SIZE = R"doc(
Get current FFT size

Returns:
    Current FFT size
)doc";

// freq_deinterleaver docstrings
[[maybe_unused]] static const char* DOC_FREQ_DEINTERLEAVER = R"doc(
ATSC 3.0 Frequency De-interleaver Block

Reverses frequency interleaving within each OFDM symbol.
Uses LFSR-based permutation per ATSC A/322 Section 8.3.

Input: Interleaved LLR cells (per symbol)
Output: De-interleaved LLR cells

AXI4-S: TDATA=int8_t TVALID TREADY TLAST(symbol)
)doc";

[[maybe_unused]] static const char* DOC_FREQ_DEINTERLEAVER_MAKE = R"doc(
Create ATSC 3.0 frequency de-interleaver block

Args:
    fft_size: FFT size (8192, 16384, or 32768)

Returns:
    Shared pointer to new freq_deinterleaver instance
)doc";

[[maybe_unused]] static const char* DOC_FREQ_DEINTERLEAVER_SET_FFT_SIZE = R"doc(
Set FFT size (reconfigures permutation table)

Args:
    fft_size: FFT size (8192, 16384, or 32768)
)doc";

[[maybe_unused]] static const char* DOC_FREQ_DEINTERLEAVER_GET_FFT_SIZE = R"doc(
Get current FFT size

Returns:
    Current FFT size
)doc";

[[maybe_unused]] static const char* DOC_FREQ_DEINTERLEAVER_GET_NUM_CARRIERS = R"doc(
Get number of active carriers

Returns:
    Number of active subcarriers for current FFT size
)doc";

// time_deinterleaver docstrings
[[maybe_unused]] static const char* DOC_TIME_DEINTERLEAVER = R"doc(
ATSC 3.0 Time De-interleaver Block

Reverses time interleaving (CTI or HTI mode).
Requires settling period before valid output.

Input: Interleaved LLR cells
Output: De-interleaved LLR cells

AXI4-S: TDATA=int8_t TVALID TREADY TLAST(TI block)
)doc";

[[maybe_unused]] static const char* DOC_TIME_DEINTERLEAVER_MAKE = R"doc(
Create ATSC 3.0 time de-interleaver block

Args:
    ti_mode: Time interleaving mode (0=NONE, 1=CTI, 2=HTI)
    ti_depth: Time interleaving depth (0-15)

Returns:
    Shared pointer to new time_deinterleaver instance
)doc";

[[maybe_unused]] static const char* DOC_TIME_DEINTERLEAVER_SET_TI_MODE = R"doc(
Set time interleaving mode

Args:
    ti_mode: Mode (0=NONE, 1=CTI, 2=HTI)
)doc";

[[maybe_unused]] static const char* DOC_TIME_DEINTERLEAVER_SET_TI_DEPTH = R"doc(
Set time interleaving depth

Args:
    ti_depth: Depth (0-15)
)doc";

[[maybe_unused]] static const char* DOC_TIME_DEINTERLEAVER_IS_SETTLED = R"doc(
Check if delay lines are settled

Returns:
    True if output is valid (settling complete)
)doc";

[[maybe_unused]] static const char* DOC_TIME_DEINTERLEAVER_GET_SETTLING_BLOCKS = R"doc(
Get number of blocks needed for settling

Returns:
    Number of blocks required before output is valid
)doc";

[[maybe_unused]] static const char* DOC_TIME_DEINTERLEAVER_RESET = R"doc(
Reset internal state (clears delay lines)
)doc";

// route_parser docstrings
[[maybe_unused]] static const char* DOC_ROUTE_PARSER = R"doc(
ATSC 3.0 ROUTE Parser Block

Parses ROUTE/DASH signaling from ALP signaling packets.
Discovers broadcast services and provides segment notifications.

Input: Signaling byte stream (from ALP demux)
Output: Message ports for catalog and segment updates

AXI4-S: TDATA=uint8_t TVALID TREADY TLAST(packet)
)doc";

[[maybe_unused]] static const char* DOC_ROUTE_PARSER_MAKE = R"doc(
Create ATSC 3.0 ROUTE parser block

Returns:
    Shared pointer to new route_parser instance
)doc";

[[maybe_unused]] static const char* DOC_ROUTE_PARSER_GET_SERVICE_COUNT = R"doc(
Get number of discovered services

Returns:
    Number of services in catalog
)doc";

[[maybe_unused]] static const char* DOC_ROUTE_PARSER_GET_PACKET_COUNT = R"doc(
Get parsed packet count

Returns:
    Total signaling packets processed
)doc";

[[maybe_unused]] static const char* DOC_ROUTE_PARSER_GET_ERROR_COUNT = R"doc(
Get parse error count

Returns:
    Number of parse errors encountered
)doc";

[[maybe_unused]] static const char* DOC_ROUTE_PARSER_RESET = R"doc(
Reset parser state and clear service catalog
)doc";

// service_selector docstrings
[[maybe_unused]] static const char* DOC_SERVICE_SELECTOR = R"doc(
ATSC 3.0 Service Selector Block

Selects a service and extracts video/audio elementary streams.

Input: IP packets from ALP demux
Output 0: Video ES (HEVC NALs)
Output 1: Audio ES (AC-4/AAC frames)

AXI4-S: TDATA=uint8_t TVALID TREADY TLAST(packet)
)doc";

[[maybe_unused]] static const char* DOC_SERVICE_SELECTOR_MAKE = R"doc(
Create ATSC 3.0 service selector block

Args:
    service_id: Service ID to select (0 = first available)

Returns:
    Shared pointer to new service_selector instance
)doc";

[[maybe_unused]] static const char* DOC_SERVICE_SELECTOR_SET_SERVICE_ID = R"doc(
Set service to extract

Args:
    service_id: Service ID to select
)doc";

[[maybe_unused]] static const char* DOC_SERVICE_SELECTOR_GET_SERVICE_ID = R"doc(
Get currently selected service ID

Returns:
    Current service ID
)doc";

[[maybe_unused]] static const char* DOC_SERVICE_SELECTOR_GET_VIDEO_BYTES = R"doc(
Get video bytes extracted

Returns:
    Total video ES bytes output
)doc";

[[maybe_unused]] static const char* DOC_SERVICE_SELECTOR_GET_AUDIO_BYTES = R"doc(
Get audio bytes extracted

Returns:
    Total audio ES bytes output
)doc";

[[maybe_unused]] static const char* DOC_SERVICE_SELECTOR_IS_SERVICE_LOCKED = R"doc(
Check if service is locked

Returns:
    True if service is actively being extracted
)doc";

// av_player docstrings
[[maybe_unused]] static const char* DOC_AV_PLAYER = R"doc(
ATSC 3.0 A/V Player Block

Live audio/video playback via GStreamer.
IMPORTANT: Pipeline is initialized in start() on main thread.

Input 0: Video ES (HEVC NALs)
Input 1: Audio ES (AC-4/AAC frames)
Output: Message port for state changes

AXI4-S: TDATA=uint8_t TVALID TREADY TLAST(frame)
)doc";

[[maybe_unused]] static const char* DOC_AV_PLAYER_MAKE = R"doc(
Create ATSC 3.0 A/V player block

Args:
    enable_video: Enable video playback
    enable_audio: Enable audio playback
    volume: Initial volume (0.0 - 1.0)

Returns:
    Shared pointer to new av_player instance
)doc";

[[maybe_unused]] static const char* DOC_AV_PLAYER_PLAY = R"doc(
Start playback

Returns:
    True if playback started successfully
)doc";

[[maybe_unused]] static const char* DOC_AV_PLAYER_PAUSE = R"doc(
Pause playback

Returns:
    True if paused successfully
)doc";

[[maybe_unused]] static const char* DOC_AV_PLAYER_STOP = R"doc(
Stop playback

Returns:
    True if stopped successfully
)doc";

[[maybe_unused]] static const char* DOC_AV_PLAYER_SET_VOLUME = R"doc(
Set volume level

Args:
    volume: Volume (0.0 - 1.0)
)doc";

[[maybe_unused]] static const char* DOC_AV_PLAYER_GET_VOLUME = R"doc(
Get current volume

Returns:
    Current volume level (0.0 - 1.0)
)doc";

[[maybe_unused]] static const char* DOC_AV_PLAYER_GET_STATE = R"doc(
Get current player state

Returns:
    PlayerState enum value
)doc";

[[maybe_unused]] static const char* DOC_AV_PLAYER_GET_VIDEO_FRAMES = R"doc(
Get video frames rendered

Returns:
    Total frames rendered
)doc";

[[maybe_unused]] static const char* DOC_AV_PLAYER_GET_AUDIO_SAMPLES = R"doc(
Get audio samples played

Returns:
    Total audio samples played
)doc";

// l1_monitor docstrings
[[maybe_unused]] static const char* DOC_L1_MONITOR = R"doc(
ATSC 3.0 L1 Monitor Block

Monitors and displays L1 signaling information.

Input: Message port for L1 config updates
Output: Message port for JSON-formatted L1 info

No streaming I/O - message ports only.
)doc";

[[maybe_unused]] static const char* DOC_L1_MONITOR_MAKE = R"doc(
Create ATSC 3.0 L1 monitor block

Returns:
    Shared pointer to new l1_monitor instance
)doc";

[[maybe_unused]] static const char* DOC_L1_MONITOR_GET_FFT_SIZE = R"doc(
Get current FFT size

Returns:
    FFT size (8192, 16384, or 32768)
)doc";

[[maybe_unused]] static const char* DOC_L1_MONITOR_GET_PILOT_PATTERN = R"doc(
Get current pilot pattern

Returns:
    Pilot pattern index (1-8)
)doc";

[[maybe_unused]] static const char* DOC_L1_MONITOR_GET_NUM_PLPS = R"doc(
Get number of PLPs

Returns:
    Number of physical layer pipes
)doc";

[[maybe_unused]] static const char* DOC_L1_MONITOR_IS_CONFIG_VALID = R"doc(
Check if L1 config is valid

Returns:
    True if L1 config has been received
)doc";

[[maybe_unused]] static const char* DOC_L1_MONITOR_GET_L1_JSON = R"doc(
Get L1 info as JSON string

Returns:
    JSON-formatted L1 signaling info
)doc";

// service_guide docstrings
[[maybe_unused]] static const char* DOC_SERVICE_GUIDE = R"doc(
ATSC 3.0 Service Guide Block

Displays available broadcast services from service catalog.

Input: Message port for catalog updates
Output: Message port for JSON-formatted service list

No streaming I/O - message ports only.
)doc";

[[maybe_unused]] static const char* DOC_SERVICE_GUIDE_MAKE = R"doc(
Create ATSC 3.0 service guide block

Returns:
    Shared pointer to new service_guide instance
)doc";

[[maybe_unused]] static const char* DOC_SERVICE_GUIDE_GET_SERVICE_COUNT = R"doc(
Get number of services

Returns:
    Number of services in guide
)doc";

[[maybe_unused]] static const char* DOC_SERVICE_GUIDE_GET_SERVICE_JSON = R"doc(
Get service info as JSON

Args:
    index: Service index

Returns:
    JSON-formatted service info
)doc";

[[maybe_unused]] static const char* DOC_SERVICE_GUIDE_GET_ALL_SERVICES_JSON = R"doc(
Get all services as JSON

Returns:
    JSON array of all services
)doc";
