// atsc3_l1_monitor_impl.cc — L1 Monitor implementation

#include "atsc3_l1_monitor_impl.h"

#include <gnuradio/io_signature.h>
#include <sstream>

namespace gr {
namespace atsc3 {

l1_monitor::sptr l1_monitor::make() {
    return gnuradio::make_block_sptr<l1_monitor_impl>();
}

l1_monitor_impl::l1_monitor_impl()
    : gr::block("atsc3_l1_monitor", gr::io_signature::make(0, 0, 0),
                gr::io_signature::make(0, 0, 0)),
      port_l1_config_(pmt::mp("l1_config")),
      port_l1_info_(pmt::mp("l1_info")),
      port_l1_config_out_(pmt::mp("l1_config_out")) {
    // Register message ports
    message_port_register_in(port_l1_config_);
    message_port_register_out(port_l1_info_);
    message_port_register_out(port_l1_config_out_);  // Broadcast to downstream blocks

    // Set message handler
    set_msg_handler(port_l1_config_, [this](pmt::pmt_t msg) { handle_l1_config(msg); });
}

l1_monitor_impl::~l1_monitor_impl() = default;

int l1_monitor_impl::general_work(int /* noutput_items */, gr_vector_int& /* ninput_items */,
                                  gr_vector_const_void_star& /* input_items */,
                                  gr_vector_void_star& /* output_items */) {
    // Message-only block, no streaming work
    return 0;
}

void l1_monitor_impl::handle_l1_config(pmt::pmt_t msg) {
    std::lock_guard<std::mutex> lock(config_mutex_);

    // Parse PMT message into Atsc3Config
    if (pmt::is_dict(msg)) {
        // Extract FFT size
        if (pmt::dict_has_key(msg, pmt::mp("fft_size"))) {
            auto fft = pmt::to_long(pmt::dict_ref(msg, pmt::mp("fft_size"), pmt::from_long(8192)));
            switch (fft) {
                case 8192:
                    config_.fft_size = ::atsc3::config::FftSize::FFT_8K;
                    break;
                case 16384:
                    config_.fft_size = ::atsc3::config::FftSize::FFT_16K;
                    break;
                case 32768:
                    config_.fft_size = ::atsc3::config::FftSize::FFT_32K;
                    break;
            }
        }

        // Extract pilot pattern
        if (pmt::dict_has_key(msg, pmt::mp("pilot_pattern"))) {
            auto pp = pmt::to_long(pmt::dict_ref(msg, pmt::mp("pilot_pattern"), pmt::from_long(3)));
            config_.pilot_pattern = static_cast<::atsc3::config::PilotPattern>(pp);
        }

        // Extract number of PLPs
        if (pmt::dict_has_key(msg, pmt::mp("num_plps"))) {
            config_.num_plps = static_cast<uint8_t>(
                pmt::to_long(pmt::dict_ref(msg, pmt::mp("num_plps"), pmt::from_long(1))));
        }

        // Extract PLP configurations
        if (pmt::dict_has_key(msg, pmt::mp("plps"))) {
            auto plps_vec = pmt::dict_ref(msg, pmt::mp("plps"), pmt::PMT_NIL);
            if (pmt::is_vector(plps_vec)) {
                size_t num =
                    std::min(pmt::length(plps_vec), static_cast<size_t>(::atsc3::config::MAX_PLPS));
                for (size_t i = 0; i < num; i++) {
                    auto plp_dict = pmt::vector_ref(plps_vec, i);
                    if (pmt::is_dict(plp_dict)) {
                        auto& plp = config_.plps[i];

                        if (pmt::dict_has_key(plp_dict, pmt::mp("plp_id"))) {
                            plp.plp_id = static_cast<uint8_t>(pmt::to_long(
                                pmt::dict_ref(plp_dict, pmt::mp("plp_id"), pmt::from_long(0))));
                        }

                        if (pmt::dict_has_key(plp_dict, pmt::mp("modulation"))) {
                            plp.modulation = static_cast<::atsc3::config::Modulation>(pmt::to_long(
                                pmt::dict_ref(plp_dict, pmt::mp("modulation"), pmt::from_long(0))));
                        }

                        if (pmt::dict_has_key(plp_dict, pmt::mp("code_rate"))) {
                            plp.code_rate = static_cast<::atsc3::config::CodeRate>(pmt::to_long(
                                pmt::dict_ref(plp_dict, pmt::mp("code_rate"), pmt::from_long(0))));
                        }

                        if (pmt::dict_has_key(plp_dict, pmt::mp("ti_mode"))) {
                            plp.ti_mode = static_cast<::atsc3::config::TimeInterleaveMode>(
                                pmt::to_long(pmt::dict_ref(plp_dict, pmt::mp("ti_mode"),
                                                           pmt::from_long(0))));
                        }

                        plp.valid = true;
                    }
                }
            }
        }

        config_.valid = true;
    }

    // Publish updated L1 info
    auto l1_info = build_l1_info_pmt();
    message_port_pub(port_l1_info_, l1_info);

    // Broadcast L1 config to downstream blocks for auto-configuration
    // Re-emit the original message so blocks can extract their specific parameters
    message_port_pub(port_l1_config_out_, msg);
}

pmt::pmt_t l1_monitor_impl::build_l1_info_pmt() const {
    pmt::pmt_t dict = pmt::make_dict();

    dict = pmt::dict_add(dict, pmt::mp("valid"), pmt::from_bool(config_.valid));
    dict = pmt::dict_add(dict, pmt::mp("fft_size"),
                         pmt::from_long(static_cast<long>(config_.get_fft_samples())));
    dict =
        pmt::dict_add(dict, pmt::mp("fft_size_str"), pmt::intern(fft_size_str(config_.fft_size)));
    dict = pmt::dict_add(dict, pmt::mp("cp_samples"),
                         pmt::from_long(static_cast<long>(config_.get_cp_samples())));
    dict = pmt::dict_add(dict, pmt::mp("pilot_pattern"),
                         pmt::from_long(static_cast<long>(config_.pilot_pattern)));
    dict = pmt::dict_add(dict, pmt::mp("pilot_pattern_str"),
                         pmt::intern(pilot_pattern_str(config_.pilot_pattern)));
    dict = pmt::dict_add(dict, pmt::mp("num_plps"), pmt::from_long(config_.num_plps));

    // Add PLP details
    pmt::pmt_t plps = pmt::make_vector(config_.num_plps, pmt::PMT_NIL);
    for (size_t i = 0; i < config_.num_plps; i++) {
        const auto& plp = config_.plps[i];
        pmt::pmt_t plp_dict = pmt::make_dict();

        plp_dict = pmt::dict_add(plp_dict, pmt::mp("plp_id"), pmt::from_long(plp.plp_id));
        plp_dict = pmt::dict_add(plp_dict, pmt::mp("modulation"),
                                 pmt::intern(modulation_str(plp.modulation)));
        plp_dict = pmt::dict_add(plp_dict, pmt::mp("code_rate"),
                                 pmt::intern(code_rate_str(plp.code_rate)));
        plp_dict =
            pmt::dict_add(plp_dict, pmt::mp("ti_mode"), pmt::intern(ti_mode_str(plp.ti_mode)));
        plp_dict = pmt::dict_add(plp_dict, pmt::mp("ti_depth"), pmt::from_long(plp.ti_depth));

        pmt::vector_set(plps, i, plp_dict);
    }
    dict = pmt::dict_add(dict, pmt::mp("plps"), plps);

    // Add JSON representation
    dict = pmt::dict_add(dict, pmt::mp("json"), pmt::intern(get_l1_json()));

    return dict;
}

int l1_monitor_impl::get_fft_size() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return static_cast<int>(config_.get_fft_samples());
}

int l1_monitor_impl::get_pilot_pattern() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return static_cast<int>(config_.pilot_pattern);
}

int l1_monitor_impl::get_num_plps() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_.num_plps;
}

bool l1_monitor_impl::is_config_valid() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_.valid;
}

std::string l1_monitor_impl::get_l1_json() const {
    std::lock_guard<std::mutex> lock(config_mutex_);

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"valid\": " << (config_.valid ? "true" : "false") << ",\n";
    oss << "  \"fft_size\": " << config_.get_fft_samples() << ",\n";
    oss << "  \"fft_size_str\": \"" << fft_size_str(config_.fft_size) << "\",\n";
    oss << "  \"cp_samples\": " << config_.get_cp_samples() << ",\n";
    oss << "  \"pilot_pattern\": \"" << pilot_pattern_str(config_.pilot_pattern) << "\",\n";
    oss << "  \"num_plps\": " << static_cast<int>(config_.num_plps) << ",\n";
    oss << "  \"plps\": [\n";

    for (size_t i = 0; i < config_.num_plps; i++) {
        const auto& plp = config_.plps[i];
        oss << "    {\n";
        oss << "      \"plp_id\": " << static_cast<int>(plp.plp_id) << ",\n";
        oss << "      \"modulation\": \"" << modulation_str(plp.modulation) << "\",\n";
        oss << "      \"code_rate\": \"" << code_rate_str(plp.code_rate) << "\",\n";
        oss << "      \"ti_mode\": \"" << ti_mode_str(plp.ti_mode) << "\",\n";
        oss << "      \"ti_depth\": " << static_cast<int>(plp.ti_depth) << "\n";
        oss << "    }";
        if (i + 1 < static_cast<size_t>(config_.num_plps)) {
            oss << ",";
        }
        oss << "\n";
    }

    oss << "  ]\n";
    oss << "}";

    return oss.str();
}

const char* l1_monitor_impl::fft_size_str(::atsc3::config::FftSize size) {
    switch (size) {
        case ::atsc3::config::FftSize::FFT_8K:
            return "8K";
        case ::atsc3::config::FftSize::FFT_16K:
            return "16K";
        case ::atsc3::config::FftSize::FFT_32K:
            return "32K";
        default:
            return "Unknown";
    }
}

const char* l1_monitor_impl::pilot_pattern_str(::atsc3::config::PilotPattern pattern) {
    switch (pattern) {
        case ::atsc3::config::PilotPattern::PP1:
            return "PP1";
        case ::atsc3::config::PilotPattern::PP2:
            return "PP2";
        case ::atsc3::config::PilotPattern::PP3:
            return "PP3";
        case ::atsc3::config::PilotPattern::PP4:
            return "PP4";
        case ::atsc3::config::PilotPattern::PP5:
            return "PP5";
        case ::atsc3::config::PilotPattern::PP6:
            return "PP6";
        case ::atsc3::config::PilotPattern::PP7:
            return "PP7";
        case ::atsc3::config::PilotPattern::PP8:
            return "PP8";
        default:
            return "Unknown";
    }
}

const char* l1_monitor_impl::modulation_str(::atsc3::config::Modulation mod) {
    switch (mod) {
        case ::atsc3::config::Modulation::QPSK:
            return "QPSK";
        case ::atsc3::config::Modulation::QAM16:
            return "16-QAM";
        case ::atsc3::config::Modulation::QAM64:
            return "64-QAM";
        case ::atsc3::config::Modulation::QAM256:
            return "256-QAM";
        case ::atsc3::config::Modulation::QAM1024:
            return "1024-QAM";
        case ::atsc3::config::Modulation::QAM4096:
            return "4096-QAM";
        case ::atsc3::config::Modulation::NUC_QPSK:
            return "NUC-QPSK";
        case ::atsc3::config::Modulation::NUC_16:
            return "NUC-16";
        case ::atsc3::config::Modulation::NUC_64:
            return "NUC-64";
        case ::atsc3::config::Modulation::NUC_256:
            return "NUC-256";
        case ::atsc3::config::Modulation::NUC_1024:
            return "NUC-1024";
        case ::atsc3::config::Modulation::NUC_4096:
            return "NUC-4096";
        default:
            return "Unknown";
    }
}

const char* l1_monitor_impl::code_rate_str(::atsc3::config::CodeRate rate) {
    switch (rate) {
        case ::atsc3::config::CodeRate::RATE_2_15:
            return "2/15";
        case ::atsc3::config::CodeRate::RATE_3_15:
            return "3/15";
        case ::atsc3::config::CodeRate::RATE_4_15:
            return "4/15";
        case ::atsc3::config::CodeRate::RATE_5_15:
            return "5/15";
        case ::atsc3::config::CodeRate::RATE_6_15:
            return "6/15";
        case ::atsc3::config::CodeRate::RATE_7_15:
            return "7/15";
        case ::atsc3::config::CodeRate::RATE_8_15:
            return "8/15";
        case ::atsc3::config::CodeRate::RATE_9_15:
            return "9/15";
        case ::atsc3::config::CodeRate::RATE_10_15:
            return "10/15";
        case ::atsc3::config::CodeRate::RATE_11_15:
            return "11/15";
        case ::atsc3::config::CodeRate::RATE_12_15:
            return "12/15";
        case ::atsc3::config::CodeRate::RATE_13_15:
            return "13/15";
        default:
            return "Unknown";
    }
}

const char* l1_monitor_impl::ti_mode_str(::atsc3::config::TimeInterleaveMode mode) {
    switch (mode) {
        case ::atsc3::config::TimeInterleaveMode::NONE:
            return "None";
        case ::atsc3::config::TimeInterleaveMode::CTI:
            return "CTI";
        case ::atsc3::config::TimeInterleaveMode::HTI:
            return "HTI";
        default:
            return "Unknown";
    }
}

}  // namespace atsc3
}  // namespace gr
