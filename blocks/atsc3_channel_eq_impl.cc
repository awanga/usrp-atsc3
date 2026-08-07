// atsc3_channel_eq_impl.cc — Channel Equalizer implementation

#include "atsc3_channel_eq_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

channel_eq::sptr channel_eq::make(int fft_size, int pilot_pattern, bool use_mmse) {
    return gnuradio::make_block_sptr<channel_eq_impl>(fft_size, pilot_pattern, use_mmse);
}

channel_eq_impl::channel_eq_impl(int fft_size, int pilot_pattern, bool use_mmse)
    : gr::sync_block("atsc3_channel_eq", gr::io_signature::make(1, 1, sizeof(gr_complex)),
                     gr::io_signature::make(1, 1, sizeof(gr_complex))),
      fft_size_(fft_size),
      pilot_pattern_(pilot_pattern),
      use_mmse_(use_mmse),
      snr_db_(0.0f),
      mer_db_(0.0f),
      symbol_index_(0) {
    reconfigure();
    set_output_multiple(fft_size);

    // Register message port
    port_reset_in_ = pmt::intern("reset");
    message_port_register_in(port_reset_in_);
    set_msg_handler(port_reset_in_, [this](pmt::pmt_t msg) { this->handle_reset_msg(msg); });
}

channel_eq_impl::~channel_eq_impl() = default;

void channel_eq_impl::reconfigure() {
    // Configure pilot extractor
    ::atsc3::ofdm::PilotExtractorConfig pe_config;
    pe_config.fft_size = static_cast<size_t>(fft_size_);
    pe_config.pattern = static_cast<::atsc3::ofdm::PilotPattern>(pilot_pattern_);
    pilot_extractor_ = std::make_unique<::atsc3::ofdm::PilotExtractor>(pe_config);

    // Configure channel estimator
    ::atsc3::channel::ChannelEstimatorConfig ce_config;
    ce_config.fft_size = static_cast<size_t>(fft_size_);
    channel_estimator_ = std::make_unique<::atsc3::channel::ChannelEstimator>(ce_config);

    // Configure equalizer
    ::atsc3::channel::EqualizerConfig eq_config;
    eq_config.fft_size = static_cast<size_t>(fft_size_);
    eq_config.mode = use_mmse_ ? ::atsc3::channel::EqualizationMode::MMSE
                               : ::atsc3::channel::EqualizationMode::ZERO_FORCING;
    equalizer_ = std::make_unique<::atsc3::channel::Equalizer>(eq_config);
}

int channel_eq_impl::work(int noutput_items, gr_vector_const_void_star& input_items,
                          gr_vector_void_star& output_items) {
    const gr_complex* in = static_cast<const gr_complex*>(input_items[0]);
    gr_complex* out = static_cast<gr_complex*>(output_items[0]);

    int symbols_to_process = noutput_items / fft_size_;
    int produced = 0;

    for (int sym = 0; sym < symbols_to_process; ++sym) {
        const auto* sym_in = reinterpret_cast<const ::atsc3::sample_t*>(in + sym * fft_size_);

        // Extract pilots from FFT output
        auto pilots = pilot_extractor_->extract(sym_in, symbol_index_);

        // Estimate channel from pilots
        auto channel_estimate = channel_estimator_->estimate(pilots, symbol_index_);

        // Equalize received symbols
        auto eq_result = equalizer_->equalize(sym_in, channel_estimate.h_estimate, symbol_index_);

        // Copy equalized symbols to output
        size_t num_out = std::min(eq_result.symbols.size(), static_cast<size_t>(fft_size_));
        std::memcpy(out + sym * fft_size_, eq_result.symbols.data(), num_out * sizeof(gr_complex));

        // Update SNR estimate
        snr_db_ = channel_estimate.estimated_snr_db;

        symbol_index_++;
        produced += fft_size_;
    }

    return produced;
}

void channel_eq_impl::set_pilot_pattern(int pattern) {
    pilot_pattern_ = pattern;
    reconfigure();
}

void channel_eq_impl::set_mmse(bool use_mmse) {
    use_mmse_ = use_mmse;
    reconfigure();
}

void channel_eq_impl::reset() {
    // Reset channel estimator and equalizer state
    if (channel_estimator_) {
        channel_estimator_->reset();
    }
    if (equalizer_) {
        equalizer_->reset();
    }

    // Reset metrics
    snr_db_ = 0.0f;
    mer_db_ = 0.0f;
    symbol_index_ = 0;
}

void channel_eq_impl::handle_reset_msg(pmt::pmt_t /*msg*/) {
    reset();
}

}  // namespace atsc3
}  // namespace gr
