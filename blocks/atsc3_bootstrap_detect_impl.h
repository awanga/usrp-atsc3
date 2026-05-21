// atsc3_bootstrap_detect_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_BOOTSTRAP_DETECT_IMPL_H
#define INCLUDED_ATSC3_BOOTSTRAP_DETECT_IMPL_H

#include "atsc3_bootstrap_detect.h"

#include <sync/bootstrap_detector.h>

namespace gr {
namespace atsc3 {

class bootstrap_detect_impl : public bootstrap_detect {
private:
    ::atsc3::sync::BootstrapDetector detector_;
    float threshold_;
    bool locked_;
    float cfo_hz_;

public:
    bootstrap_detect_impl(float threshold);
    ~bootstrap_detect_impl() override;

    // Block interface
    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;

    int general_work(int noutput_items, gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;

    // Public API
    float get_cfo_hz() const override {
        return cfo_hz_;
    }
    bool is_locked() const override {
        return locked_;
    }
    void set_threshold(float threshold) override;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_BOOTSTRAP_DETECT_IMPL_H */
