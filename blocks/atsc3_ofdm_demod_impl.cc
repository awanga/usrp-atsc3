// atsc3_ofdm_demod_impl.cc — OFDM Demodulator implementation

#include "atsc3_ofdm_demod_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

ofdm_demod::sptr ofdm_demod::make(int fft_size, int cp_length) {
    return gnuradio::make_block_sptr<ofdm_demod_impl>(fft_size, cp_length);
}

ofdm_demod_impl::ofdm_demod_impl(int fft_size, int cp_length)
    : gr::sync_block("atsc3_ofdm_demod", gr::io_signature::make(1, 1, sizeof(gr_complex)),
                     gr::io_signature::make(1, 1, sizeof(gr_complex))),
      fft_size_(fft_size),
      cp_length_(cp_length),
      state_name_("SEARCHING") {
    reconfigure();

    // Input: fft_size + cp_length samples per symbol
    // Output: fft_size samples per symbol
    set_output_multiple(fft_size);

    // Register message ports
    port_reset_in_ = pmt::intern("reset");
    port_lock_status_out_ = pmt::intern("lock_status");

    message_port_register_in(port_reset_in_);
    message_port_register_out(port_lock_status_out_);

    set_msg_handler(port_reset_in_, [this](pmt::pmt_t msg) { this->handle_reset_msg(msg); });
}

ofdm_demod_impl::~ofdm_demod_impl() = default;

void ofdm_demod_impl::reconfigure() {
    // Configure CP removal - map cp_length to nearest CpFraction enum
    ::atsc3::ofdm::CpRemovalConfig cp_config;
    cp_config.fft_size = static_cast<size_t>(fft_size_);

    // Scale cp_length relative to 8192-point FFT for fraction lookup
    int scaled_cp = (cp_length_ * 8192) / fft_size_;
    if (scaled_cp >= 3648) {
        cp_config.cp_fraction = ::atsc3::ofdm::CpFraction::k4096_8192;
    } else if (scaled_cp >= 2752) {
        cp_config.cp_fraction = ::atsc3::ofdm::CpFraction::k3072_8192;
    } else if (scaled_cp >= 2240) {
        cp_config.cp_fraction = ::atsc3::ofdm::CpFraction::k2432_8192;
    } else if (scaled_cp >= 1792) {
        cp_config.cp_fraction = ::atsc3::ofdm::CpFraction::k2048_8192;
    } else if (scaled_cp >= 1280) {
        cp_config.cp_fraction = ::atsc3::ofdm::CpFraction::k1536_8192;
    } else if (scaled_cp >= 896) {
        cp_config.cp_fraction = ::atsc3::ofdm::CpFraction::k1024_8192;
    } else if (scaled_cp >= 640) {
        cp_config.cp_fraction = ::atsc3::ofdm::CpFraction::k768_8192;
    } else if (scaled_cp >= 448) {
        cp_config.cp_fraction = ::atsc3::ofdm::CpFraction::k512_8192;
    } else if (scaled_cp >= 288) {
        cp_config.cp_fraction = ::atsc3::ofdm::CpFraction::k384_8192;
    } else {
        cp_config.cp_fraction = ::atsc3::ofdm::CpFraction::k192_8192;
    }
    cp_removal_ = std::make_unique<::atsc3::ofdm::CpRemoval>(cp_config);

    // Configure FFT engine using factory
    ::atsc3::ofdm::FftConfig fft_config;
    if (fft_size_ == 32768) {
        fft_config.size = ::atsc3::ofdm::FftSize::k32K;
    } else if (fft_size_ == 16384) {
        fft_config.size = ::atsc3::ofdm::FftSize::k16K;
    } else if (fft_size_ == 4096) {
        fft_config.size = ::atsc3::ofdm::FftSize::k4K;
    } else {
        fft_config.size = ::atsc3::ofdm::FftSize::k8K;
    }
    fft_config.direction = ::atsc3::ofdm::FftDirection::kForward;
    fft_engine_ = ::atsc3::ofdm::FftEngine::create(fft_config);

    // Allocate buffers
    fft_input_.resize(fft_size_);
    fft_output_.resize(fft_size_);

    // Set up CP removal callback to fill fft_input_
    symbol_ready_ = false;
    cp_removal_->set_output_callback([this](const ::atsc3::sample_t* symbol, size_t len) {
        if (len <= fft_input_.size()) {
            std::memcpy(fft_input_.data(), symbol, len * sizeof(::atsc3::sample_t));
            symbol_ready_ = true;
        }
    });

    // Update block I/O ratio
    set_relative_rate(static_cast<double>(fft_size_) / (fft_size_ + cp_length_));
}

int ofdm_demod_impl::work(int noutput_items, gr_vector_const_void_star& input_items,
                          gr_vector_void_star& output_items) {
    const gr_complex* in = static_cast<const gr_complex*>(input_items[0]);
    gr_complex* out = static_cast<gr_complex*>(output_items[0]);

    int symbol_in_len = fft_size_ + cp_length_;
    int symbols_to_process = noutput_items / fft_size_;
    int produced = 0;

    for (int sym = 0; sym < symbols_to_process; ++sym) {
        const auto* sym_in = reinterpret_cast<const ::atsc3::sample_t*>(in + sym * symbol_in_len);

        // Feed samples to CP removal - callback fills fft_input_
        symbol_ready_ = false;
        cp_removal_->process(sym_in, static_cast<size_t>(symbol_in_len));

        if (symbol_ready_) {
            // Perform FFT (2-arg version)
            fft_engine_->process(fft_input_.data(), fft_output_.data());

            // Copy to output
            std::memcpy(out + sym * fft_size_, fft_output_.data(), fft_size_ * sizeof(gr_complex));

            produced += fft_size_;
        }
    }

    return produced;
}

void ofdm_demod_impl::set_fft_size(int fft_size) {
    fft_size_ = fft_size;
    reconfigure();
}

void ofdm_demod_impl::set_cp_length(int cp_length) {
    cp_length_ = cp_length;
    reconfigure();
}

void ofdm_demod_impl::reset() {
    // Reset CP removal state
    if (cp_removal_) {
        cp_removal_->reset();
    }

    // Clear buffers
    std::fill(fft_input_.begin(), fft_input_.end(), ::atsc3::sample_t{0, 0});
    std::fill(fft_output_.begin(), fft_output_.end(), ::atsc3::sample_t{0, 0});
    symbol_ready_ = false;

    // Reset lock state
    locked_ = false;
    prev_locked_ = false;
    confidence_ = 0.0f;
    state_name_ = "SEARCHING";
}

void ofdm_demod_impl::handle_reset_msg(pmt::pmt_t /*msg*/) {
    reset();
}

void ofdm_demod_impl::emit_lock_status() {
    pmt::pmt_t msg = pmt::make_dict();
    msg = pmt::dict_add(msg, pmt::intern("locked"), pmt::from_bool(locked_));
    msg = pmt::dict_add(msg, pmt::intern("state"), pmt::intern(state_name_));
    msg = pmt::dict_add(msg, pmt::intern("confidence"), pmt::from_float(confidence_));
    message_port_pub(port_lock_status_out_, msg);
}

}  // namespace atsc3
}  // namespace gr
