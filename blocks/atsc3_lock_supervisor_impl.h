// atsc3_lock_supervisor_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_LOCK_SUPERVISOR_IMPL_H
#define INCLUDED_ATSC3_LOCK_SUPERVISOR_IMPL_H

#include "atsc3_lock_supervisor.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <pmt/pmt.h>

namespace gr {
namespace atsc3 {

class lock_supervisor_impl : public lock_supervisor {
private:
    // Configuration
    int unlock_threshold_;
    int relock_delay_ms_;

    // Lock state tracking
    std::atomic<bool> locked_{false};
    std::atomic<int> consecutive_unlocks_{0};
    std::atomic<int> reset_count_{0};

    // Timing
    std::chrono::steady_clock::time_point last_reset_time_;
    std::mutex mutex_;

    // Message ports
    pmt::pmt_t port_lock_status_in_;
    pmt::pmt_t port_reset_out_;

    // Message handlers
    void handle_lock_status(pmt::pmt_t msg);

    // Reset logic
    void trigger_reset();
    bool in_relock_delay() const;

public:
    lock_supervisor_impl(int unlock_threshold, int relock_delay_ms);
    ~lock_supervisor_impl() override;

    // sync_block interface - minimal work function
    int work(int noutput_items, gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

    // Public API
    void set_unlock_threshold(int threshold) override;
    int get_unlock_threshold() const override {
        return unlock_threshold_;
    }
    void set_relock_delay(int delay_ms) override;
    int get_relock_delay() const override {
        return relock_delay_ms_;
    }
    bool is_locked() const override {
        return locked_.load();
    }
    int get_reset_count() const override {
        return reset_count_.load();
    }
    void manual_reset() override;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_LOCK_SUPERVISOR_IMPL_H */
