// atsc3_service_guide_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_SERVICE_GUIDE_IMPL_H
#define INCLUDED_ATSC3_SERVICE_GUIDE_IMPL_H

#include "atsc3_service_guide.h"

#include <mutex>
#include <string>
#include <vector>

namespace gr {
namespace atsc3 {

// Service info structure for display
struct ServiceInfo {
    uint16_t service_id = 0;
    uint16_t major_channel = 0;
    uint16_t minor_channel = 0;
    std::string name;
    std::string video_codec;
    std::string audio_codec;
    uint16_t video_width = 0;
    uint16_t video_height = 0;
    uint8_t audio_channels = 0;
    bool active = false;
};

class service_guide_impl : public service_guide {
private:
    // Message ports
    pmt::pmt_t port_catalog_;
    pmt::pmt_t port_services_;

    // Service list
    std::vector<ServiceInfo> services_;
    mutable std::mutex services_mutex_;

    // Message handler for catalog updates
    void handle_catalog(pmt::pmt_t msg);

    // Build services PMT
    pmt::pmt_t build_services_pmt() const;

public:
    service_guide_impl();
    ~service_guide_impl() override;

    // No streaming work - message only block
    int general_work(int noutput_items, gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;

    // Public API
    size_t get_service_count() const override;
    std::string get_service_json(size_t index) const override;
    std::string get_all_services_json() const override;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_SERVICE_GUIDE_IMPL_H */
