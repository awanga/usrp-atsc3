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
      noise_variance_(noise_variance),
      plp_id_(-1),
      port_l1_config_(pmt::intern("l1_config")) {
    // Register L1 config input message port for multi-PLP auto-configuration
    message_port_register_in(port_l1_config_);
    set_msg_handler(port_l1_config_, [this](pmt::pmt_t msg) { this->handle_l1_config(msg); });

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

void constellation_demapper_impl::set_plp_id(int plp_id) {
    plp_id_ = plp_id;
    // If plp_id >= 0, block will auto-configure when L1 config is received
    // If plp_id == -1, block uses manual modulation/code_rate settings
}

void constellation_demapper_impl::handle_l1_config(pmt::pmt_t msg) {
    // Skip auto-configuration if manual mode (plp_id_ == -1)
    if (plp_id_ < 0) {
        return;
    }

    // Extract PLP configuration from L1 message
    // Expected format: dict with "plps" key containing vector of PLP configs
    if (!pmt::is_dict(msg)) {
        return;
    }

    pmt::pmt_t plps_key = pmt::intern("plps");
    if (!pmt::dict_has_key(msg, plps_key)) {
        return;
    }

    pmt::pmt_t plps = pmt::dict_ref(msg, plps_key, pmt::PMT_NIL);
    if (!pmt::is_vector(plps)) {
        return;
    }

    // Search for our PLP ID in the configuration
    size_t num_plps = pmt::length(plps);
    for (size_t i = 0; i < num_plps; ++i) {
        pmt::pmt_t plp_config = pmt::vector_ref(plps, i);
        if (!pmt::is_dict(plp_config)) {
            continue;
        }

        pmt::pmt_t plp_id_key = pmt::intern("plp_id");
        if (!pmt::dict_has_key(plp_config, plp_id_key)) {
            continue;
        }

        int config_plp_id = pmt::to_long(pmt::dict_ref(plp_config, plp_id_key, pmt::from_long(-1)));
        if (config_plp_id != plp_id_) {
            continue;
        }

        // Found our PLP - extract modulation and code rate
        pmt::pmt_t mod_key = pmt::intern("modulation");
        pmt::pmt_t cr_key = pmt::intern("code_rate");

        if (pmt::dict_has_key(plp_config, mod_key)) {
            int new_mod =
                pmt::to_long(pmt::dict_ref(plp_config, mod_key, pmt::from_long(modulation_)));
            if (new_mod != modulation_) {
                modulation_ = new_mod;
            }
        }

        if (pmt::dict_has_key(plp_config, cr_key)) {
            int new_cr =
                pmt::to_long(pmt::dict_ref(plp_config, cr_key, pmt::from_long(code_rate_)));
            if (new_cr != code_rate_) {
                code_rate_ = new_cr;
            }
        }

        reconfigure();
        break;
    }
}

}  // namespace atsc3
}  // namespace gr
