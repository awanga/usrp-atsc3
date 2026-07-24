// atsc3_service_selector_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_SERVICE_SELECTOR_IMPL_H
#define INCLUDED_ATSC3_SERVICE_SELECTOR_IMPL_H

#include "atsc3_service_selector.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <pmt/pmt.h>
#include <vector>

namespace gr {
namespace atsc3 {

// Simple IP packet header for filtering
struct IpHeader {
    uint32_t dest_addr;
    uint16_t dest_port;
    uint32_t tsi;
};

class service_selector_impl : public service_selector {
private:
    int service_id_;
    bool service_locked_;

    // Message port for runtime service selection
    pmt::pmt_t port_service_select_;

    // Handler for service selection messages
    void handle_service_select(pmt::pmt_t msg);

    // TSI values for selected service
    uint32_t video_tsi_;
    uint32_t audio_tsi_;

    // Statistics
    uint64_t video_bytes_;
    uint64_t audio_bytes_;
    uint64_t packets_received_;
    uint64_t packets_filtered_;

    // Reassembly buffers for ROUTE segments
    std::vector<uint8_t> video_reassembly_;
    std::vector<uint8_t> audio_reassembly_;
    static constexpr size_t MAX_SEGMENT_SIZE = 2 * 1024 * 1024;  // 2MB max segment

    // Current reassembly state
    uint32_t current_video_toi_;
    uint32_t current_audio_toi_;

    mutable std::mutex mutex_;

    // Parse IP header from packet
    bool parse_ip_header(const uint8_t* data, size_t len, IpHeader& header) const;

    // Extract LCT payload
    bool extract_lct_payload(const uint8_t* data, size_t len, uint32_t& toi,
                             const uint8_t*& payload, size_t& payload_len) const;

public:
    service_selector_impl(int service_id);
    ~service_selector_impl() override;

    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;

    int general_work(int noutput_items, gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;

    // Public API
    void set_service_id(int service_id) override;
    int get_service_id() const override {
        return service_id_;
    }
    uint64_t get_video_bytes() const override {
        return video_bytes_;
    }
    uint64_t get_audio_bytes() const override {
        return audio_bytes_;
    }
    bool is_service_locked() const override {
        return service_locked_;
    }
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_SERVICE_SELECTOR_IMPL_H */
