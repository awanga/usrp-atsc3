// atsc3_constellation_demapper_impl.cc — Constellation Demapper implementation

#include "atsc3_constellation_demapper_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

constellation_demapper::sptr constellation_demapper::make(int modulation, int code_rate,
                                                          float noise_variance) {
    return gnuradio::make_block_sptr<constellation_demapper_impl>(modulation, code_rate,
                                                                  noise_variance);
}

constellation_demapper_impl::constellation_demapper_impl(int modulation, int code_rate,
                                                         float noise_variance)
    : gr::sync_interpolator("atsc3_constellation_demapper",
                            gr::io_signature::make(1, 1, sizeof(gr_complex)),
                            gr::io_signature::make(1, 1, sizeof(int8_t)),
                            ::atsc3::config::Atsc3Config::bits_per_symbol(
                                static_cast<::atsc3::config::Modulation>(modulation))),
      modulation_(modulation),
      code_rate_(code_rate),
      noise_variance_(noise_variance) {
    reconfigure();
}

constellation_demapper_impl::~constellation_demapper_impl() = default;

void constellation_demapper_impl::reconfigure() {
    ::atsc3::ofdm::DemapperConfig config;
    config.modulation = static_cast<::atsc3::config::Modulation>(modulation_);
    config.code_rate = static_cast<::atsc3::config::CodeRate>(code_rate_);
    config.noise_variance = noise_variance_;
    config.use_max_log = true;
    config.llr_clip = 127;

    demapper_ = std::make_unique<::atsc3::ofdm::ConstellationDemapper>(config);

    // Update interpolation factor when modulation changes
    set_interpolation(static_cast<int>(demapper_->bits_per_symbol()));
}

int constellation_demapper_impl::work(int noutput_items, gr_vector_const_void_star& input_items,
                                      gr_vector_void_star& output_items) {
    const gr_complex* in = static_cast<const gr_complex*>(input_items[0]);
    int8_t* out = static_cast<int8_t*>(output_items[0]);

    size_t bits_per_sym = demapper_->bits_per_symbol();
    size_t num_symbols = static_cast<size_t>(noutput_items) / bits_per_sym;

    // Demap symbols to LLRs
    size_t llrs_written =
        demapper_->demap(reinterpret_cast<const ::atsc3::sample_t*>(in), num_symbols, out);

    return static_cast<int>(llrs_written);
}

void constellation_demapper_impl::set_modulation(int modulation) {
    modulation_ = modulation;
    reconfigure();
}

void constellation_demapper_impl::set_code_rate(int code_rate) {
    code_rate_ = code_rate;
    reconfigure();
}

void constellation_demapper_impl::set_noise_variance(float noise_variance) {
    noise_variance_ = noise_variance;
    reconfigure();
}

}  // namespace atsc3
}  // namespace gr
