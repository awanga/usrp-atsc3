// atsc3_l1_monitor_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_L1_MONITOR_IMPL_H
#define INCLUDED_ATSC3_L1_MONITOR_IMPL_H

#include "atsc3_l1_monitor.h"

#include <config/atsc3_config.h>
#include <mutex>

namespace gr {
namespace atsc3 {

class l1_monitor_impl : public l1_monitor {
private:
    // Message ports
    pmt::pmt_t port_l1_config_;
    pmt::pmt_t port_l1_info_;
    pmt::pmt_t port_l1_config_out_;  // Broadcast L1 config to downstream blocks

    // Current L1 configuration
    ::atsc3::config::Atsc3Config config_;
    mutable std::mutex config_mutex_;

    // Message handler for incoming L1 config
    void handle_l1_config(pmt::pmt_t msg);

    // Build L1 info PMT from current config
    pmt::pmt_t build_l1_info_pmt() const;

    // Convert enum to string
    static const char* fft_size_str(::atsc3::config::FftSize size);
    static const char* pilot_pattern_str(::atsc3::config::PilotPattern pattern);
    static const char* modulation_str(::atsc3::config::Modulation mod);
    static const char* code_rate_str(::atsc3::config::CodeRate rate);
    static const char* ti_mode_str(::atsc3::config::TimeInterleaveMode mode);

public:
    l1_monitor_impl();
    ~l1_monitor_impl() override;

    // No streaming work - message only block
    int general_work(int noutput_items, gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;

    // Public API
    int get_fft_size() const override;
    int get_pilot_pattern() const override;
    int get_num_plps() const override;
    bool is_config_valid() const override;
    std::string get_l1_json() const override;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_L1_MONITOR_IMPL_H */
