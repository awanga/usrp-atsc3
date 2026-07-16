// atsc3_lock_supervisor_impl.cc — Lock Supervisor implementation

#include "atsc3_lock_supervisor_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

lock_supervisor::sptr lock_supervisor::make(int unlock_threshold, int relock_delay_ms) {
    return gnuradio::make_block_sptr<lock_supervisor_impl>(unlock_threshold, relock_delay_ms);
}

lock_supervisor_impl::lock_supervisor_impl(int unlock_threshold, int relock_delay_ms)
    : gr::sync_block("atsc3_lock_supervisor", gr::io_signature::make(0, 0, 0),
                     gr::io_signature::make(0, 0, 0)),
      unlock_threshold_(unlock_threshold),
      relock_delay_ms_(relock_delay_ms),
      last_reset_time_(std::chrono::steady_clock::now()),
      port_lock_status_in_(pmt::intern("lock_status")),
      port_reset_out_(pmt::intern("reset")) {
    // Register message ports
    message_port_register_in(port_lock_status_in_);
    message_port_register_out(port_reset_out_);

    set_msg_handler(port_lock_status_in_,
                    [this](pmt::pmt_t msg) { this->handle_lock_status(msg); });
}

lock_supervisor_impl::~lock_supervisor_impl() = default;

int lock_supervisor_impl::work(int /*noutput_items*/, gr_vector_const_void_star& /*input_items*/,
                               gr_vector_void_star& /*output_items*/) {
    // This block only processes messages, no stream data
    return 0;
}

void lock_supervisor_impl::handle_lock_status(pmt::pmt_t msg) {
    // Skip processing during relock delay
    if (in_relock_delay()) {
        return;
    }

    // Extract lock status from message
    bool locked = false;
    if (pmt::is_dict(msg)) {
        pmt::pmt_t locked_val = pmt::dict_ref(msg, pmt::intern("locked"), pmt::PMT_NIL);
        if (!pmt::is_null(locked_val)) {
            locked = pmt::to_bool(locked_val);
        }
    } else if (pmt::is_bool(msg)) {
        locked = pmt::to_bool(msg);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (locked) {
        // Lock achieved - reset unlock counter
        consecutive_unlocks_.store(0);
        locked_.store(true);
    } else {
        // Unlock detected - increment counter
        int count = consecutive_unlocks_.fetch_add(1) + 1;

        if (count >= unlock_threshold_) {
            // Threshold reached - trigger reset
            trigger_reset();
        }
    }
}

void lock_supervisor_impl::trigger_reset() {
    // Update state
    locked_.store(false);
    consecutive_unlocks_.store(0);
    reset_count_.fetch_add(1);
    last_reset_time_ = std::chrono::steady_clock::now();

    // Send reset message to all connected blocks
    pmt::pmt_t reset_msg = pmt::make_dict();
    reset_msg = pmt::dict_add(reset_msg, pmt::intern("reset"), pmt::from_bool(true));
    reset_msg =
        pmt::dict_add(reset_msg, pmt::intern("reset_count"), pmt::from_long(reset_count_.load()));
    message_port_pub(port_reset_out_, reset_msg);
}

bool lock_supervisor_impl::in_relock_delay() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reset_time_);
    return elapsed.count() < relock_delay_ms_;
}

void lock_supervisor_impl::set_unlock_threshold(int threshold) {
    std::lock_guard<std::mutex> lock(mutex_);
    unlock_threshold_ = threshold;
}

void lock_supervisor_impl::set_relock_delay(int delay_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    relock_delay_ms_ = delay_ms;
}

void lock_supervisor_impl::manual_reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    trigger_reset();
}

}  // namespace atsc3
}  // namespace gr
