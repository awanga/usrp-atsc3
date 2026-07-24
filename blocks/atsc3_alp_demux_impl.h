// atsc3_alp_demux_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_ALP_DEMUX_IMPL_H
#define INCLUDED_ATSC3_ALP_DEMUX_IMPL_H

#include "atsc3_alp_demux.h"

#include <atomic>
#include <deque>
#include <framing/alp_demux.h>
#include <memory>
#include <pmt/pmt.h>
#include <vector>

namespace gr {
namespace atsc3 {

class alp_demux_impl : public alp_demux {
private:
    int plp_id_;

    std::unique_ptr<::atsc3::framing::AlpDemux> alp_demux_;

    // Output buffer for ALP packet data
    std::vector<uint8_t> packet_buffer_;

    // Pending packets from callbacks
    enum class PacketType { IP, TS, SIGNALING, OTHER };
    std::deque<std::vector<uint8_t>> pending_packets_;
    std::deque<PacketType> pending_types_;

    // Message port names
    pmt::pmt_t port_ip_out_;
    pmt::pmt_t port_ts_out_;
    pmt::pmt_t port_sig_out_;

    // Statistics (thread-safe)
    std::atomic<uint64_t> packet_count_{0};
    std::atomic<uint64_t> error_count_{0};
    std::atomic<uint64_t> ip_packet_count_{0};
    std::atomic<uint64_t> ts_packet_count_{0};
    std::atomic<uint64_t> sig_packet_count_{0};
    std::atomic<uint64_t> other_packet_count_{0};

    void reconfigure();

public:
    alp_demux_impl(int plp_id);
    ~alp_demux_impl() override;

    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;

    int general_work(int noutput_items, gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;

    // Public API
    void set_plp_id(int plp_id) override;
    int get_plp_id() const override {
        return plp_id_;
    }
    uint64_t get_packet_count() const override {
        return packet_count_.load();
    }
    uint64_t get_error_count() const override {
        return error_count_.load();
    }
    void get_packet_stats(uint64_t* ip_count, uint64_t* ts_count, uint64_t* sig_count,
                          uint64_t* other_count) const override;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_ALP_DEMUX_IMPL_H */
