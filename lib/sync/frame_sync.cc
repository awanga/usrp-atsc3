// frame_sync.cc — Frame synchronization implementation
//
// Superframe and subframe boundary tracking using preamble correlation

#include "frame_sync.h"

#include "dsp/cordic.h"

#include <algorithm>
#include <cmath>

namespace atsc3 {
namespace sync {

namespace {

#ifdef ATSC3_FIXED_POINT
int16_t saturate_i16(int64_t v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32767) {
        return -32767;
    }
    return static_cast<int16_t>(v);
}

// Exact floor(sqrt(x)) via binary digit recurrence -- shifts, compares,
// add/subtract only, no float, no library sqrt call. Needed alongside
// CORDIC rather than instead of it: correlate_preamble()'s normalization
// denominator is sqrt(sig_power * ref_power), a square root of a scalar
// *product* of two power accumulators, not a vector magnitude. CORDIC's
// circular vectoring mode (lib/dsp/cordic.h) computes sqrt(x^2 + y^2) for
// a 2D point and is exactly right for the correlation magnitude below,
// but there is no circular-mode identity for a scalar sqrt like this one
// -- that needs hyperbolic-mode CORDIC, out of scope this milestone (see
// hdl/docs/placeholder_status.md). This is a different, equally
// legitimate fixed-point primitive, not a workaround.
uint64_t integer_sqrt(uint64_t x) {
    uint64_t res = 0;
    uint64_t bit = uint64_t(1) << 62;  // highest even power of 4 <= x
    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}
#endif

}  // namespace

FrameSync::FrameSync(const FrameSyncConfig& config)
    : config_(config),
      state_(FrameSyncState::SEARCHING),
      frame_number_(0),
      subframe_number_(0),
      symbol_position_(0),
      sample_counter_(0),
      consecutive_hits_(0),
      consecutive_misses_(0),
      holdover_counter_(0),
      expected_boundary_(0),
      confidence_(0.0) {
    init_preamble_reference();
}

FrameSync::~FrameSync() = default;

void FrameSync::init_preamble_reference() {
    // Initialize preamble reference sequence
    // In real ATSC 3.0, this would be the known bootstrap/preamble symbols
    // For now, use a simple pilot-like sequence
    size_t ref_len = config_.frame_params.fft_size;
    preamble_ref_.resize(ref_len);

    // Generate reference preamble (simplified)
    // Real implementation would use ATSC 3.0 bootstrap structure
    for (size_t k = 0; k < ref_len; ++k) {
        // Pseudo-random pattern based on index
        double phase = 2.0 * 3.14159265358979 * ((k * 7) % 13) / 13.0;
        float re = static_cast<float>(std::cos(phase));
        float im = static_cast<float>(std::sin(phase));
#ifdef ATSC3_FIXED_POINT
        preamble_ref_[k] = sample_t(float_to_q15(re * 0.5f), float_to_q15(im * 0.5f));
#else
        preamble_ref_[k] = sample_t(re * 0.5f, im * 0.5f);
#endif
    }
}

void FrameSync::reset() {
    state_ = FrameSyncState::SEARCHING;
    frame_number_ = 0;
    subframe_number_ = 0;
    symbol_position_ = 0;
    sample_counter_ = 0;
    consecutive_hits_ = 0;
    consecutive_misses_ = 0;
    holdover_counter_ = 0;
    expected_boundary_ = 0;
    confidence_ = 0.0;
}

void FrameSync::update_frame_params(const FrameParams& params) {
    config_.frame_params = params;
    // Reinitialize reference if needed
    if (params.fft_size != preamble_ref_.size()) {
        init_preamble_reference();
    }
}

void FrameSync::set_bootstrap_offset(size_t sample_index, double conf) {
    // Bootstrap detector provides coarse timing
    expected_boundary_ = sample_index;
    confidence_ = conf;

    if (state_ == FrameSyncState::SEARCHING) {
        transition_state(FrameSyncState::ACQUIRING);
        consecutive_hits_ = 1;
    } else if (state_ == FrameSyncState::HOLDOVER) {
        // Reacquire from bootstrap
        transition_state(FrameSyncState::ACQUIRING);
        consecutive_hits_ = 1;
    }
}

void FrameSync::transition_state(FrameSyncState new_state) {
    if (new_state == state_) {
        return;
    }

    // Reset counters on state change
    switch (new_state) {
        case FrameSyncState::SEARCHING:
            consecutive_hits_ = 0;
            consecutive_misses_ = 0;
            confidence_ = 0.0;
            break;

        case FrameSyncState::ACQUIRING:
            consecutive_misses_ = 0;
            break;

        case FrameSyncState::LOCKED:
            consecutive_misses_ = 0;
            confidence_ = 1.0;
            break;

        case FrameSyncState::HOLDOVER:
            holdover_counter_ = 0;
            break;
    }

    state_ = new_state;
}

double FrameSync::correlate_preamble(const sample_t* symbols, size_t n, size_t offset) {
    if (offset + preamble_ref_.size() > n) {
        return 0.0;
    }

    size_t corr_len = std::min(preamble_ref_.size(), n - offset);

#ifdef ATSC3_FIXED_POINT
    // Genuine fixed-point cross-correlation (Phase 9.0b rewrite) -- no
    // per-sample double, no q15_to_float. Accumulated as raw (unshifted)
    // Q1.15 x Q1.15 products, up to corr_len (<= fft_size, up to 32768)
    // terms, in wide int64 accumulators.
    int64_t corr_re = 0;
    int64_t corr_im = 0;
    int64_t sig_power = 0;
    int64_t ref_power = 0;

    for (size_t i = 0; i < corr_len; ++i) {
        const sample_t& sig = symbols[offset + i];
        const sample_t& ref = preamble_ref_[i];

        int64_t sig_re = sig.real();
        int64_t sig_im = sig.imag();
        int64_t ref_re = ref.real();
        int64_t ref_im = ref.imag();

        // Correlation: sig * conj(ref)
        corr_re += sig_re * ref_re + sig_im * ref_im;
        corr_im += sig_im * ref_re - sig_re * ref_im;

        sig_power += sig_re * sig_re + sig_im * sig_im;
        ref_power += ref_re * ref_re + ref_im * ref_im;
    }

    if (sig_power <= 0 || ref_power <= 0) {
        return 0.0;
    }

    // Normalization denominator: sqrt(sig_power) * sqrt(ref_power), not
    // sqrt(sig_power * ref_power) -- the product alone can reach ~1e26
    // for fft_size=32768 at full scale, far beyond even int64/uint64.
    // Computing each factor's sqrt first keeps every intermediate well
    // within uint64 (each accumulator is bounded by ~fft_size * 2 *
    // 32767^2, comfortably < 2^63).
    uint64_t sig_mag = integer_sqrt(static_cast<uint64_t>(sig_power));
    uint64_t ref_mag = integer_sqrt(static_cast<uint64_t>(ref_power));
    uint64_t norm = sig_mag * ref_mag;
    if (norm == 0) {
        return 0.0;
    }

    // Correlation magnitude via CORDIC vectoring (this part genuinely is
    // a 2D vector magnitude, unlike the denominator above). corr_re/
    // corr_im can be far outside int16_t range -- normalize into range
    // with an adaptive shift first and scale the result back, same
    // technique as bootstrap_detector's metric computation (see that
    // file for why this must not simply saturate).
    int64_t re = corr_re;
    int64_t im = corr_im;
    int shift = 0;
    while ((re > 32767 || re < -32767 || im > 32767 || im < -32767) && shift < 48) {
        re >>= 1;
        im >>= 1;
        ++shift;
    }
    dsp::CordicVectorResult vec = dsp::cordic_vector(saturate_i16(re), saturate_i16(im));
    uint64_t corr_mag = static_cast<uint64_t>(vec.magnitude) << shift;

    return static_cast<double>(corr_mag) / static_cast<double>(norm);
#else
    // Cross-correlation between received symbols and reference
    double corr_re = 0.0;
    double corr_im = 0.0;
    double sig_power = 0.0;
    double ref_power = 0.0;

    for (size_t i = 0; i < corr_len; ++i) {
        const sample_t& sig = symbols[offset + i];
        const sample_t& ref = preamble_ref_[i];

        double sig_re = sig.real();
        double sig_im = sig.imag();
        double ref_re = ref.real();
        double ref_im = ref.imag();

        // Correlation: sig * conj(ref)
        corr_re += sig_re * ref_re + sig_im * ref_im;
        corr_im += sig_im * ref_re - sig_re * ref_im;

        sig_power += sig_re * sig_re + sig_im * sig_im;
        ref_power += ref_re * ref_re + ref_im * ref_im;
    }

    // Normalized correlation magnitude
    double norm = std::sqrt(sig_power * ref_power);
    if (norm < 1e-10) {
        return 0.0;
    }

    return std::sqrt(corr_re * corr_re + corr_im * corr_im) / norm;
#endif
}

void FrameSync::emit_frame_event(FrameEvent::Type type) {
    if (!frame_callback_) {
        return;
    }

    FrameEvent event;
    event.type = type;
    event.sample_index = sample_counter_;
    event.frame_number = frame_number_;
    event.subframe_number = subframe_number_;
    event.symbol_number = symbol_position_;
    event.confidence = confidence_;

    frame_callback_(event);
}

size_t FrameSync::get_samples_to_boundary() const {
    if (expected_boundary_ > sample_counter_) {
        return expected_boundary_ - sample_counter_;
    }
    return 0;
}

void FrameSync::handle_searching(const sample_t* symbols, size_t n) {
    // Search for preamble by sliding correlation
    size_t search_step = config_.frame_params.fft_size / 4;  // Coarse search

    for (size_t offset = 0; offset + preamble_ref_.size() <= n; offset += search_step) {
        double corr = correlate_preamble(symbols, n, offset);

        if (corr >= config_.detection_threshold) {
            expected_boundary_ = sample_counter_ + offset;
            confidence_ = corr;
            transition_state(FrameSyncState::ACQUIRING);
            consecutive_hits_ = 1;
            return;
        }
    }
}

void FrameSync::handle_acquiring(const sample_t* symbols, size_t n) {
    // Fine search around expected boundary
    size_t search_start = 0;
    size_t search_end = n;

    // Limit search to window around expected boundary
    if (expected_boundary_ >= sample_counter_ && expected_boundary_ < sample_counter_ + n) {
        size_t expected_offset = expected_boundary_ - sample_counter_;
        size_t half_window = config_.search_window / 2;

        search_start = (expected_offset > half_window) ? expected_offset - half_window : 0;
        search_end = std::min(expected_offset + half_window, n);
    }

    double best_corr = 0.0;
    size_t best_offset = 0;

    for (size_t offset = search_start; offset + preamble_ref_.size() <= search_end && offset < n;
         ++offset) {
        double corr = correlate_preamble(symbols, n, offset);
        if (corr > best_corr) {
            best_corr = corr;
            best_offset = offset;
        }
    }

    if (best_corr >= config_.detection_threshold) {
        expected_boundary_ = sample_counter_ + best_offset;
        confidence_ = 0.9 * confidence_ + 0.1 * best_corr;
        ++consecutive_hits_;

        if (consecutive_hits_ >= config_.lock_acquire_count) {
            transition_state(FrameSyncState::LOCKED);
            emit_frame_event(FrameEvent::FRAME_START);
        }
    } else {
        ++consecutive_misses_;
        if (consecutive_misses_ >= config_.lock_loss_count) {
            transition_state(FrameSyncState::SEARCHING);
        }
    }
}

void FrameSync::handle_locked(const sample_t* symbols, size_t n) {
    // Track frame boundaries
    // Check correlation at expected boundary

    if (expected_boundary_ >= sample_counter_ && expected_boundary_ < sample_counter_ + n) {
        size_t expected_offset = expected_boundary_ - sample_counter_;

        // Fine correlation search
        size_t half_window = config_.search_window / 2;
        size_t search_start = (expected_offset > half_window) ? expected_offset - half_window : 0;
        size_t search_end = std::min(expected_offset + half_window + 1, n);

        double best_corr = 0.0;
        size_t best_offset = expected_offset;

        for (size_t offset = search_start;
             offset + preamble_ref_.size() <= n && offset < search_end; ++offset) {
            double corr = correlate_preamble(symbols, n, offset);
            if (corr > best_corr) {
                best_corr = corr;
                best_offset = offset;
            }
        }

        if (best_corr >= config_.detection_threshold * 0.8) {  // Relaxed threshold
            // Timing error available for future refinement
            // (best_offset vs expected_offset delta)

            confidence_ = 0.95 * confidence_ + 0.05 * best_corr;
            consecutive_misses_ = 0;

            // Emit frame event
            emit_frame_event(FrameEvent::FRAME_START);

            // Update expected boundary for next frame
            ++frame_number_;
            size_t frame_samples = config_.frame_params.samples_per_frame();
            expected_boundary_ = sample_counter_ + best_offset + frame_samples;

        } else {
            ++consecutive_misses_;
            if (consecutive_misses_ >= config_.lock_loss_count) {
                transition_state(FrameSyncState::HOLDOVER);
            }
        }
    }

    // Track symbols within frame
    // Symbol tracking logic would go here (using samples_per_symbol())...
}

void FrameSync::handle_holdover(const sample_t* symbols, size_t n) {
    // Continue with predicted timing
    ++holdover_counter_;

    // Try to reacquire
    if (expected_boundary_ >= sample_counter_ && expected_boundary_ < sample_counter_ + n) {
        size_t expected_offset = expected_boundary_ - sample_counter_;
        double corr = correlate_preamble(symbols, n, expected_offset);

        if (corr >= config_.detection_threshold) {
            transition_state(FrameSyncState::LOCKED);
            consecutive_hits_ = 1;
            emit_frame_event(FrameEvent::FRAME_START);
            return;
        }
    }

    // Check holdover timeout
    if (holdover_counter_ >= config_.holdover_frames) {
        transition_state(FrameSyncState::SEARCHING);
    } else {
        // Continue predicting
        ++frame_number_;
        size_t frame_samples = config_.frame_params.samples_per_frame();
        expected_boundary_ += frame_samples;
        confidence_ *= 0.9;  // Decay confidence
    }
}

void FrameSync::process(const sample_t* symbols, size_t n) {
    switch (state_) {
        case FrameSyncState::SEARCHING:
            handle_searching(symbols, n);
            break;
        case FrameSyncState::ACQUIRING:
            handle_acquiring(symbols, n);
            break;
        case FrameSyncState::LOCKED:
            handle_locked(symbols, n);
            break;
        case FrameSyncState::HOLDOVER:
            handle_holdover(symbols, n);
            break;
    }

    sample_counter_ += n;
}

}  // namespace sync
}  // namespace atsc3
