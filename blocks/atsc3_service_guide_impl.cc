// atsc3_service_guide_impl.cc — Service Guide implementation

#include "atsc3_service_guide_impl.h"

#include <gnuradio/io_signature.h>
#include <sstream>

namespace gr {
namespace atsc3 {

service_guide::sptr service_guide::make() {
    return gnuradio::make_block_sptr<service_guide_impl>();
}

service_guide_impl::service_guide_impl()
    : gr::block("atsc3_service_guide", gr::io_signature::make(0, 0, 0),
                gr::io_signature::make(0, 0, 0)) {
    // Create message ports
    port_catalog_ = pmt::mp("catalog");
    port_services_ = pmt::mp("services");

    message_port_register_in(port_catalog_);
    message_port_register_out(port_services_);

    // Set message handler
    set_msg_handler(port_catalog_, [this](pmt::pmt_t msg) { handle_catalog(msg); });
}

service_guide_impl::~service_guide_impl() = default;

int service_guide_impl::general_work(int /* noutput_items */, gr_vector_int& /* ninput_items */,
                                     gr_vector_const_void_star& /* input_items */,
                                     gr_vector_void_star& /* output_items */) {
    // Message-only block, no streaming work
    return 0;
}

void service_guide_impl::handle_catalog(pmt::pmt_t msg) {
    std::lock_guard<std::mutex> lock(services_mutex_);

    if (!pmt::is_dict(msg)) {
        return;
    }

    // Clear existing services
    services_.clear();

    // Extract service count
    if (!pmt::dict_has_key(msg, pmt::mp("services"))) {
        return;
    }

    auto services_vec = pmt::dict_ref(msg, pmt::mp("services"), pmt::PMT_NIL);
    if (!pmt::is_vector(services_vec)) {
        return;
    }

    size_t num_services = pmt::length(services_vec);
    services_.reserve(num_services);

    for (size_t i = 0; i < num_services; i++) {
        auto svc_pmt = pmt::vector_ref(services_vec, i);
        if (!pmt::is_dict(svc_pmt)) {
            continue;
        }

        ServiceInfo info;

        // Extract service info
        if (pmt::dict_has_key(svc_pmt, pmt::mp("service_id"))) {
            info.service_id = static_cast<uint16_t>(
                pmt::to_long(pmt::dict_ref(svc_pmt, pmt::mp("service_id"), pmt::from_long(0))));
        }

        if (pmt::dict_has_key(svc_pmt, pmt::mp("major_channel"))) {
            info.major_channel = static_cast<uint16_t>(
                pmt::to_long(pmt::dict_ref(svc_pmt, pmt::mp("major_channel"), pmt::from_long(0))));
        }

        if (pmt::dict_has_key(svc_pmt, pmt::mp("minor_channel"))) {
            info.minor_channel = static_cast<uint16_t>(
                pmt::to_long(pmt::dict_ref(svc_pmt, pmt::mp("minor_channel"), pmt::from_long(0))));
        }

        if (pmt::dict_has_key(svc_pmt, pmt::mp("name"))) {
            info.name =
                pmt::symbol_to_string(pmt::dict_ref(svc_pmt, pmt::mp("name"), pmt::intern("")));
        }

        if (pmt::dict_has_key(svc_pmt, pmt::mp("video_codec"))) {
            info.video_codec = pmt::symbol_to_string(
                pmt::dict_ref(svc_pmt, pmt::mp("video_codec"), pmt::intern("HEVC")));
        } else {
            info.video_codec = "HEVC";  // Default
        }

        if (pmt::dict_has_key(svc_pmt, pmt::mp("audio_codec"))) {
            info.audio_codec = pmt::symbol_to_string(
                pmt::dict_ref(svc_pmt, pmt::mp("audio_codec"), pmt::intern("AC-4")));
        } else {
            info.audio_codec = "AC-4";  // Default
        }

        if (pmt::dict_has_key(svc_pmt, pmt::mp("video_width"))) {
            info.video_width = static_cast<uint16_t>(
                pmt::to_long(pmt::dict_ref(svc_pmt, pmt::mp("video_width"), pmt::from_long(1920))));
        }

        if (pmt::dict_has_key(svc_pmt, pmt::mp("video_height"))) {
            info.video_height = static_cast<uint16_t>(pmt::to_long(
                pmt::dict_ref(svc_pmt, pmt::mp("video_height"), pmt::from_long(1080))));
        }

        if (pmt::dict_has_key(svc_pmt, pmt::mp("audio_channels"))) {
            info.audio_channels = static_cast<uint8_t>(
                pmt::to_long(pmt::dict_ref(svc_pmt, pmt::mp("audio_channels"), pmt::from_long(2))));
        }

        if (pmt::dict_has_key(svc_pmt, pmt::mp("active"))) {
            info.active =
                pmt::to_bool(pmt::dict_ref(svc_pmt, pmt::mp("active"), pmt::from_bool(false)));
        }

        services_.push_back(info);
    }

    // Publish updated service list
    auto services_msg = build_services_pmt();
    message_port_pub(port_services_, services_msg);
}

pmt::pmt_t service_guide_impl::build_services_pmt() const {
    pmt::pmt_t dict = pmt::make_dict();

    dict = pmt::dict_add(dict, pmt::mp("service_count"),
                         pmt::from_long(static_cast<long>(services_.size())));

    pmt::pmt_t services_vec = pmt::make_vector(services_.size(), pmt::PMT_NIL);

    for (size_t i = 0; i < services_.size(); i++) {
        const auto& svc = services_[i];

        pmt::pmt_t svc_dict = pmt::make_dict();
        svc_dict = pmt::dict_add(svc_dict, pmt::mp("service_id"), pmt::from_long(svc.service_id));
        svc_dict =
            pmt::dict_add(svc_dict, pmt::mp("major_channel"), pmt::from_long(svc.major_channel));
        svc_dict =
            pmt::dict_add(svc_dict, pmt::mp("minor_channel"), pmt::from_long(svc.minor_channel));
        svc_dict = pmt::dict_add(svc_dict, pmt::mp("name"), pmt::intern(svc.name));
        svc_dict = pmt::dict_add(svc_dict, pmt::mp("video_codec"), pmt::intern(svc.video_codec));
        svc_dict = pmt::dict_add(svc_dict, pmt::mp("audio_codec"), pmt::intern(svc.audio_codec));
        svc_dict = pmt::dict_add(svc_dict, pmt::mp("video_width"), pmt::from_long(svc.video_width));
        svc_dict =
            pmt::dict_add(svc_dict, pmt::mp("video_height"), pmt::from_long(svc.video_height));
        svc_dict =
            pmt::dict_add(svc_dict, pmt::mp("audio_channels"), pmt::from_long(svc.audio_channels));
        svc_dict = pmt::dict_add(svc_dict, pmt::mp("active"), pmt::from_bool(svc.active));

        // Add channel string for display
        std::ostringstream channel_str;
        channel_str << svc.major_channel << "." << svc.minor_channel;
        svc_dict = pmt::dict_add(svc_dict, pmt::mp("channel"), pmt::intern(channel_str.str()));

        pmt::vector_set(services_vec, i, svc_dict);
    }

    dict = pmt::dict_add(dict, pmt::mp("services"), services_vec);

    // Add JSON representation
    dict = pmt::dict_add(dict, pmt::mp("json"), pmt::intern(get_all_services_json()));

    return dict;
}

size_t service_guide_impl::get_service_count() const {
    std::lock_guard<std::mutex> lock(services_mutex_);
    return services_.size();
}

std::string service_guide_impl::get_service_json(size_t index) const {
    std::lock_guard<std::mutex> lock(services_mutex_);

    if (index >= services_.size()) {
        return "{}";
    }

    const auto& svc = services_[index];

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"service_id\": " << svc.service_id << ",\n";
    oss << "  \"channel\": \"" << svc.major_channel << "." << svc.minor_channel << "\",\n";
    oss << "  \"name\": \"" << svc.name << "\",\n";
    oss << "  \"video_codec\": \"" << svc.video_codec << "\",\n";
    oss << "  \"audio_codec\": \"" << svc.audio_codec << "\",\n";
    oss << "  \"video_resolution\": \"" << svc.video_width << "x" << svc.video_height << "\",\n";
    oss << "  \"audio_channels\": " << static_cast<int>(svc.audio_channels) << ",\n";
    oss << "  \"active\": " << (svc.active ? "true" : "false") << "\n";
    oss << "}";

    return oss.str();
}

std::string service_guide_impl::get_all_services_json() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"service_count\": " << services_.size() << ",\n";
    oss << "  \"services\": [\n";

    for (size_t i = 0; i < services_.size(); i++) {
        const auto& svc = services_[i];

        oss << "    {\n";
        oss << "      \"service_id\": " << svc.service_id << ",\n";
        oss << "      \"channel\": \"" << svc.major_channel << "." << svc.minor_channel << "\",\n";
        oss << "      \"name\": \"" << svc.name << "\",\n";
        oss << "      \"video_codec\": \"" << svc.video_codec << "\",\n";
        oss << "      \"audio_codec\": \"" << svc.audio_codec << "\",\n";
        oss << "      \"video_resolution\": \"" << svc.video_width << "x" << svc.video_height
            << "\",\n";
        oss << "      \"audio_channels\": " << static_cast<int>(svc.audio_channels) << ",\n";
        oss << "      \"active\": " << (svc.active ? "true" : "false") << "\n";
        oss << "    }";

        if (i < services_.size() - 1) {
            oss << ",";
        }
        oss << "\n";
    }

    oss << "  ]\n";
    oss << "}";

    return oss.str();
}

}  // namespace atsc3
}  // namespace gr
