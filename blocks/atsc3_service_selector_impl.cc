// atsc3_service_selector_impl.cc — Service Selector implementation

#include "atsc3_service_selector_impl.h"

#include <cstring>
#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

service_selector::sptr service_selector::make(int service_id) {
    return gnuradio::make_block_sptr<service_selector_impl>(service_id);
}

service_selector_impl::service_selector_impl(int service_id)
    : gr::block("atsc3_service_selector", gr::io_signature::make(1, 1, sizeof(uint8_t)),
                gr::io_signature::make(2, 2, sizeof(uint8_t))),
      service_id_(service_id),
      service_locked_(false),
      port_service_select_(pmt::mp("service_select")),
      video_tsi_(0),
      audio_tsi_(0),
      video_bytes_(0),
      audio_bytes_(0),
      packets_received_(0),
      packets_filtered_(0),
      current_video_toi_(0),
      current_audio_toi_(0) {
    // Register message port for runtime service selection
    message_port_register_in(port_service_select_);
    set_msg_handler(port_service_select_, [this](pmt::pmt_t msg) { handle_service_select(msg); });

    // Pre-allocate reassembly buffers
    video_reassembly_.reserve(MAX_SEGMENT_SIZE);
    audio_reassembly_.reserve(MAX_SEGMENT_SIZE);

    // Set output multiple to allow variable output sizes
    set_output_multiple(1);
}

service_selector_impl::~service_selector_impl() = default;

void service_selector_impl::forecast(int noutput_items, gr_vector_int& ninput_items_required) {
    // We need at least one input item to produce any output
    ninput_items_required[0] = noutput_items;
}

int service_selector_impl::general_work(int noutput_items, gr_vector_int& ninput_items,
                                        gr_vector_const_void_star& input_items,
                                        gr_vector_void_star& output_items) {
    const uint8_t* in = static_cast<const uint8_t*>(input_items[0]);
    uint8_t* out_video = static_cast<uint8_t*>(output_items[0]);
    uint8_t* out_audio = static_cast<uint8_t*>(output_items[1]);

    int video_produced = 0;
    int audio_produced = 0;

    size_t input_len = static_cast<size_t>(ninput_items[0]);

    // Process input as packets
    // In real implementation, we would parse packet boundaries from tags
    // For now, process entire input as one packet

    packets_received_++;

    // Parse IP header to get TSI
    IpHeader header;
    if (!parse_ip_header(in, input_len, header)) {
        // Not a valid IP packet, skip
        consume_each(static_cast<int>(input_len));
        return 0;
    }

    // Check if this packet belongs to our service
    // In a real implementation, we would match TSI from service catalog
    bool is_video = (header.tsi == video_tsi_) && (video_tsi_ != 0);
    bool is_audio = (header.tsi == audio_tsi_) && (audio_tsi_ != 0);

    // If not locked to a service yet and service_id_ == 0, lock to first service
    if (!service_locked_ && service_id_ == 0) {
        // Auto-discover: treat first TSI as video, second as audio
        if (video_tsi_ == 0) {
            video_tsi_ = header.tsi;
            is_video = true;
            service_locked_ = true;
        } else if (audio_tsi_ == 0 && header.tsi != video_tsi_) {
            audio_tsi_ = header.tsi;
            is_audio = true;
        }
    }

    if (!is_video && !is_audio) {
        packets_filtered_++;
        consume_each(static_cast<int>(input_len));
        return 0;
    }

    // Extract LCT payload
    uint32_t toi;
    const uint8_t* payload;
    size_t payload_len;

    if (!extract_lct_payload(in, input_len, toi, payload, payload_len)) {
        consume_each(static_cast<int>(input_len));
        return 0;
    }

    // Route to appropriate output
    if (is_video) {
        // Check if this is a new segment
        if (toi != current_video_toi_ && !video_reassembly_.empty()) {
            // Output completed segment
            size_t copy_len = std::min(video_reassembly_.size(),
                                       static_cast<size_t>(noutput_items - video_produced));
            std::memcpy(out_video + video_produced, video_reassembly_.data(), copy_len);
            video_produced += static_cast<int>(copy_len);
            video_bytes_ += copy_len;
            video_reassembly_.clear();
        }

        current_video_toi_ = toi;

        // Add to reassembly buffer
        if (video_reassembly_.size() + payload_len <= MAX_SEGMENT_SIZE) {
            video_reassembly_.insert(video_reassembly_.end(), payload, payload + payload_len);
        }
    } else if (is_audio) {
        // Check if this is a new segment
        if (toi != current_audio_toi_ && !audio_reassembly_.empty()) {
            // Output completed segment
            size_t copy_len = std::min(audio_reassembly_.size(),
                                       static_cast<size_t>(noutput_items - audio_produced));
            std::memcpy(out_audio + audio_produced, audio_reassembly_.data(), copy_len);
            audio_produced += static_cast<int>(copy_len);
            audio_bytes_ += copy_len;
            audio_reassembly_.clear();
        }

        current_audio_toi_ = toi;

        // Add to reassembly buffer
        if (audio_reassembly_.size() + payload_len <= MAX_SEGMENT_SIZE) {
            audio_reassembly_.insert(audio_reassembly_.end(), payload, payload + payload_len);
        }
    }

    consume_each(static_cast<int>(input_len));

    // Return minimum of video/audio produced for gr::block
    produce(0, video_produced);
    produce(1, audio_produced);

    return WORK_CALLED_PRODUCE;
}

void service_selector_impl::set_service_id(int service_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    service_id_ = service_id;
    service_locked_ = false;
    video_tsi_ = 0;
    audio_tsi_ = 0;
    video_reassembly_.clear();
    audio_reassembly_.clear();
}

bool service_selector_impl::parse_ip_header(const uint8_t* data, size_t len,
                                            IpHeader& header) const {
    // Minimum IP header size
    if (len < 20) {
        return false;
    }

    // Check IP version (should be 4)
    uint8_t version = (data[0] >> 4) & 0x0F;
    if (version != 4) {
        return false;
    }

    // Get header length
    size_t ihl = static_cast<size_t>((data[0] & 0x0F) * 4);
    if (len < ihl + 8) {  // Need at least UDP header too
        return false;
    }

    // Extract destination address
    header.dest_addr = (static_cast<uint32_t>(data[16]) << 24) |
                       (static_cast<uint32_t>(data[17]) << 16) |
                       (static_cast<uint32_t>(data[18]) << 8) | static_cast<uint32_t>(data[19]);

    // Get protocol (should be UDP = 17)
    if (data[9] != 17) {
        return false;
    }

    // UDP header starts at ihl
    const uint8_t* udp = data + ihl;
    header.dest_port = (static_cast<uint16_t>(udp[2]) << 8) | static_cast<uint16_t>(udp[3]);

    // LCT header starts after UDP (8 bytes)
    if (len < ihl + 8 + 4) {
        return false;
    }

    const uint8_t* lct = udp + 8;

    // Extract TSI from LCT header
    // TSI location depends on header flags, simplified extraction
    // Assume TSI is at offset 8 in LCT header (32-bit)
    if (len >= ihl + 8 + 12) {
        header.tsi = (static_cast<uint32_t>(lct[8]) << 24) | (static_cast<uint32_t>(lct[9]) << 16) |
                     (static_cast<uint32_t>(lct[10]) << 8) | static_cast<uint32_t>(lct[11]);
    } else {
        header.tsi = 0;
    }

    return true;
}

bool service_selector_impl::extract_lct_payload(const uint8_t* data, size_t len, uint32_t& toi,
                                                const uint8_t*& payload,
                                                size_t& payload_len) const {
    // Find LCT header after IP + UDP
    if (len < 20 + 8 + 16) {  // Min IP + UDP + LCT
        return false;
    }

    size_t ihl = static_cast<size_t>((data[0] & 0x0F) * 4);
    const uint8_t* lct = data + ihl + 8;
    size_t lct_available = len - ihl - 8;

    if (lct_available < 16) {
        return false;
    }

    // LCT header length in 32-bit words
    uint8_t lct_header_len = ((lct[0] & 0x0F) + 1) * 4;

    if (lct_available < lct_header_len) {
        return false;
    }

    // Extract TOI (assuming it's at offset 12, 32-bit)
    if (lct_header_len >= 16) {
        toi = (static_cast<uint32_t>(lct[12]) << 24) | (static_cast<uint32_t>(lct[13]) << 16) |
              (static_cast<uint32_t>(lct[14]) << 8) | static_cast<uint32_t>(lct[15]);
    } else {
        toi = 0;
    }

    payload = lct + lct_header_len;
    payload_len = lct_available - lct_header_len;

    return payload_len > 0;
}

void service_selector_impl::handle_service_select(pmt::pmt_t msg) {
    // Accept either a plain integer or a dict with "service_id" key
    int new_service_id = -1;

    if (pmt::is_integer(msg)) {
        new_service_id = static_cast<int>(pmt::to_long(msg));
    } else if (pmt::is_dict(msg)) {
        if (pmt::dict_has_key(msg, pmt::mp("service_id"))) {
            auto val = pmt::dict_ref(msg, pmt::mp("service_id"), pmt::from_long(-1));
            new_service_id = static_cast<int>(pmt::to_long(val));
        }
    }

    if (new_service_id >= 0) {
        set_service_id(new_service_id);
    }
}

}  // namespace atsc3
}  // namespace gr
