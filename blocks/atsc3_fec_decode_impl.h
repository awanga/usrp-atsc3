// atsc3_fec_decode_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_FEC_DECODE_IMPL_H
#define INCLUDED_ATSC3_FEC_DECODE_IMPL_H

#include "atsc3_fec_decode.h"

#include <atomic>
#include <fec/ldpc_decoder.h>
#include <memory>
#include <pmt/pmt.h>
#include <vector>

namespace gr {
namespace atsc3 {

class fec_decode_impl : public fec_decode {
private:
    int code_rate_;
    int codeword_length_;
    int max_iterations_;

    std::unique_ptr<::atsc3::fec::LdpcDecoder> ldpc_decoder_;

    // Output buffer for decoded bits
    std::vector<uint8_t> decoded_bits_;

    // Statistics (thread-safe)
    std::atomic<float> avg_iterations_{0.0f};
    std::atomic<float> fer_{0.0f};
    std::atomic<bool> last_converged_{false};

    // Running statistics
    uint64_t total_codewords_;
    uint64_t failed_codewords_;
    double iteration_sum_;

    // Multi-PLP support
    int plp_id_;

    // Message ports
    pmt::pmt_t port_reset_in_;
    pmt::pmt_t port_l1_config_;

    void reconfigure();
    void update_statistics(bool converged, int iterations);
    void handle_reset_msg(pmt::pmt_t msg);
    void handle_l1_config(pmt::pmt_t msg);

public:
    fec_decode_impl(int code_rate, int codeword_length, int max_iterations);
    ~fec_decode_impl() override;

    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;

    int general_work(int noutput_items, gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;

    // Public API
    void set_code_rate(int code_rate) override;
    void set_codeword_length(int length) override;
    float get_avg_iterations() const override {
        return avg_iterations_.load();
    }
    float get_fer() const override {
        return fer_.load();
    }
    bool last_converged() const override {
        return last_converged_.load();
    }
    void reset_stats() override;
    void set_plp_id(int plp_id) override;
    int get_plp_id() const override {
        return plp_id_;
    }
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_FEC_DECODE_IMPL_H */
