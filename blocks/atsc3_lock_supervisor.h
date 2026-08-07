// atsc3_lock_supervisor.h — GNU Radio ATSC 3.0 Lock Supervisor Block
//
// Monitors lock status from sync blocks and orchestrates reset sequence
// when lock is lost. Provides graceful signal recovery without flowgraph
// teardown.
//
// Message Ports:
//   Input:  "lock_status" - receives lock status from bootstrap_detect, ofdm_demod
//   Output: "reset" - sends reset commands to all registered blocks

#ifndef INCLUDED_ATSC3_LOCK_SUPERVISOR_H
#define INCLUDED_ATSC3_LOCK_SUPERVISOR_H

#include <gnuradio/sync_block.h>

namespace gr {
namespace atsc3 {

class lock_supervisor : virtual public gr::sync_block {
public:
    typedef std::shared_ptr<lock_supervisor> sptr;

    /*!
     * \brief Create ATSC 3.0 lock supervisor block
     * \param unlock_threshold Number of consecutive unlock reports before reset
     * \param relock_delay_ms Delay after reset before accepting new lock (ms)
     * \return Shared pointer to new instance
     */
    static sptr make(int unlock_threshold = 3, int relock_delay_ms = 100);

    /*!
     * \brief Set unlock threshold
     * \param threshold Number of consecutive unlocks to trigger reset
     */
    virtual void set_unlock_threshold(int threshold) = 0;

    /*!
     * \brief Get unlock threshold
     */
    virtual int get_unlock_threshold() const = 0;

    /*!
     * \brief Set relock delay
     * \param delay_ms Milliseconds to wait after reset before accepting lock
     */
    virtual void set_relock_delay(int delay_ms) = 0;

    /*!
     * \brief Get relock delay
     */
    virtual int get_relock_delay() const = 0;

    /*!
     * \brief Check if system is currently locked
     */
    virtual bool is_locked() const = 0;

    /*!
     * \brief Get number of resets since start
     */
    virtual int get_reset_count() const = 0;

    /*!
     * \brief Manually trigger a reset
     */
    virtual void manual_reset() = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_LOCK_SUPERVISOR_H */
