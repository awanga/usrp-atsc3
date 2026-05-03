#pragma once

// ATSC 3.0 Runtime Configuration
//
// Populated by L1 preamble decoder with parameters needed by all downstream
// blocks. No dynamic allocation - fixed max PLP count for RTL portability.
//
// Reference: ATSC A/322 Section 5 (L1 Signaling)

#include <array>
#include <cstddef>
#include <cstdint>

namespace atsc3 {
namespace config {

// Maximum number of PLPs supported (ATSC 3.0 limit is 64)
constexpr size_t MAX_PLPS = 64;

// Modulation types per ATSC A/322 §7.5
enum class Modulation : uint8_t {
    QPSK = 0,
    QAM16 = 1,
    QAM64 = 2,
    QAM256 = 3,
    QAM1024 = 4,
    QAM4096 = 5,
    // Non-uniform constellations
    NUC_QPSK = 6,
    NUC_16 = 7,
    NUC_64 = 8,
    NUC_256 = 9,
    NUC_1024 = 10,
    NUC_4096 = 11
};

// LDPC code rates per ATSC A/322 §9.2
enum class CodeRate : uint8_t {
    RATE_2_15 = 0,
    RATE_3_15 = 1,
    RATE_4_15 = 2,
    RATE_5_15 = 3,
    RATE_6_15 = 4,
    RATE_7_15 = 5,
    RATE_8_15 = 6,
    RATE_9_15 = 7,
    RATE_10_15 = 8,
    RATE_11_15 = 9,
    RATE_12_15 = 10,
    RATE_13_15 = 11
};

// FFT sizes per ATSC A/322 §7.1
enum class FftSize : uint8_t { FFT_8K = 0, FFT_16K = 1, FFT_32K = 2 };

// Cyclic prefix lengths (as index into table)
enum class CpLength : uint8_t {
    CP_192_8192 = 0,
    CP_384_8192 = 1,
    CP_512_8192 = 2,
    CP_768_8192 = 3,
    CP_1024_8192 = 4,
    CP_1536_8192 = 5,
    CP_2048_8192 = 6,
    CP_2432_8192 = 7,
    CP_3072_8192 = 8,
    CP_3648_8192 = 9,
    CP_4096_8192 = 10
};

// Pilot pattern (PP1-PP8)
enum class PilotPattern : uint8_t {
    PP1 = 1,
    PP2 = 2,
    PP3 = 3,
    PP4 = 4,
    PP5 = 5,
    PP6 = 6,
    PP7 = 7,
    PP8 = 8
};

// Time interleaver mode
enum class TimeInterleaveMode : uint8_t {
    NONE = 0,
    CTI = 1,  // Convolutional time interleaving
    HTI = 2   // Hybrid time interleaving
};

// Codeword length
enum class CodewordLength : uint8_t {
    SHORT = 0,  // 16200 bits
    LONG = 1    // 64800 bits
};

// PLP type
enum class PlpType : uint8_t { NON_DISPERSED = 0, DISPERSED = 1 };

// Physical Layer Pipe configuration
// Extracted from L1-Post for each PLP
struct PlpConfig {
    // PLP identification
    uint8_t plp_id = 0;
    PlpType plp_type = PlpType::NON_DISPERSED;
    uint8_t plp_layer = 0;  // For layered division multiplexing

    // Modulation and coding
    Modulation modulation = Modulation::QPSK;
    CodeRate code_rate = CodeRate::RATE_7_15;
    CodewordLength codeword_length = CodewordLength::LONG;

    // Interleaving parameters
    TimeInterleaveMode ti_mode = TimeInterleaveMode::CTI;
    uint8_t ti_depth = 0;        // TI depth (0-15)
    uint16_t ti_num_blocks = 0;  // Number of TI blocks
    bool ti_extended = false;    // Extended interleaving

    // FEC parameters
    uint16_t num_fec_blocks = 0;   // FEC blocks per interleaver unit
    uint32_t fec_block_start = 0;  // Start address in frame

    // LDM (Layered Division Multiplexing) parameters
    uint8_t ldm_injection_level = 0;  // dB below upper layer

    // Valid flag
    bool valid = false;
};

// Full ATSC 3.0 configuration from L1 preamble
// This struct is populated by L1Decoder and distributed via ConfigBus
struct Atsc3Config {
    // ===== L1-Basic (from bootstrap) =====
    uint8_t version = 0;  // L1-Basic version

    // ===== L1-Pre (preamble OFDM config) =====
    FftSize fft_size = FftSize::FFT_8K;
    CpLength cp_length = CpLength::CP_1024_8192;
    PilotPattern pilot_pattern = PilotPattern::PP3;
    uint8_t gi_fraction = 0;  // Guard interval fraction index
    bool preamble_reduced_carriers = false;

    // L1-Post configuration
    uint16_t l1_post_size = 0;  // L1-Post size in cells
    Modulation l1_post_modulation = Modulation::QPSK;
    CodeRate l1_post_code_rate = CodeRate::RATE_5_15;
    bool l1_post_scrambled = false;
    uint8_t l1_post_num_symbols = 0;

    // ===== L1-Post (frame content) =====
    uint8_t num_subframes = 1;
    uint8_t num_plps = 0;

    // PLP configurations (fixed array, no dynamic alloc)
    std::array<PlpConfig, MAX_PLPS> plps;

    // Frame timing
    uint16_t frame_length_samples = 0;
    uint8_t num_ofdm_symbols = 0;

    // ===== Derived parameters =====
    // (computed after L1 decode, not transmitted)

    // FFT size in samples
    size_t get_fft_samples() const {
        switch (fft_size) {
            case FftSize::FFT_8K:
                return 8192;
            case FftSize::FFT_16K:
                return 16384;
            case FftSize::FFT_32K:
                return 32768;
            default:
                return 8192;
        }
    }

    // CP length in samples (for current FFT size)
    size_t get_cp_samples() const {
        static constexpr size_t cp_numerators[] = {192,  384,  512,  768,  1024, 1536,
                                                   2048, 2432, 3072, 3648, 4096};
        size_t fft = get_fft_samples();
        size_t num = cp_numerators[static_cast<size_t>(cp_length)];
        return (num * fft) / 8192;
    }

    // Active carriers for current FFT size
    size_t get_active_carriers() const {
        switch (fft_size) {
            case FftSize::FFT_8K:
                return 6913;
            case FftSize::FFT_16K:
                return 13825;
            case FftSize::FFT_32K:
                return 27649;
            default:
                return 6913;
        }
    }

    // First active carrier (centered in FFT)
    size_t get_first_active_carrier() const {
        return (get_fft_samples() - get_active_carriers()) / 2;
    }

    // LDPC codeword length in bits
    size_t get_codeword_bits(size_t plp_idx) const {
        if (plp_idx >= num_plps)
            return 0;
        return plps[plp_idx].codeword_length == CodewordLength::SHORT ? 16200 : 64800;
    }

    // Code rate as float (for SNR calculations)
    static float code_rate_value(CodeRate rate) {
        static constexpr float rates[] = {
            2.0f / 15.0f, 3.0f / 15.0f, 4.0f / 15.0f,  5.0f / 15.0f,  6.0f / 15.0f,  7.0f / 15.0f,
            8.0f / 15.0f, 9.0f / 15.0f, 10.0f / 15.0f, 11.0f / 15.0f, 12.0f / 15.0f, 13.0f / 15.0f};
        return rates[static_cast<size_t>(rate)];
    }

    // Bits per QAM symbol
    static size_t bits_per_symbol(Modulation mod) {
        switch (mod) {
            case Modulation::QPSK:
            case Modulation::NUC_QPSK:
                return 2;
            case Modulation::QAM16:
            case Modulation::NUC_16:
                return 4;
            case Modulation::QAM64:
            case Modulation::NUC_64:
                return 6;
            case Modulation::QAM256:
            case Modulation::NUC_256:
                return 8;
            case Modulation::QAM1024:
            case Modulation::NUC_1024:
                return 10;
            case Modulation::QAM4096:
            case Modulation::NUC_4096:
                return 12;
            default:
                return 2;
        }
    }

    // Validation
    bool valid = false;
};

// Get default Atsc3Config with typical broadcast values
inline Atsc3Config get_default_config() {
    Atsc3Config config;
    config.fft_size = FftSize::FFT_8K;
    config.cp_length = CpLength::CP_1024_8192;
    config.pilot_pattern = PilotPattern::PP3;
    config.num_plps = 1;
    config.plps[0].plp_id = 0;
    config.plps[0].modulation = Modulation::QAM64;
    config.plps[0].code_rate = CodeRate::RATE_7_15;
    config.plps[0].codeword_length = CodewordLength::LONG;
    config.plps[0].ti_mode = TimeInterleaveMode::CTI;
    config.plps[0].valid = true;
    config.valid = true;
    return config;
}

}  // namespace config
}  // namespace atsc3
