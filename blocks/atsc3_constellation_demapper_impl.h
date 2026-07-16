// atsc3_constellation_demapper_impl.h — Implementation header

#ifndef INCLUDED_ATSC3_CONSTELLATION_DEMAPPER_IMPL_H
#define INCLUDED_ATSC3_CONSTELLATION_DEMAPPER_IMPL_H

#include "atsc3_constellation_demapper.h"

#include <memory>
#include <ofdm/constellation_demapper.h>
#include <pmt/pmt.h>

namespace gr {
namespace atsc3 {

class constellation_demapper_impl : public constellation_demapper {
private:
    int modulation_;
    int code_rate_;
    float noise_variance_;
    int plp_id_;

    std::unique_ptr<::atsc3::ofdm::ConstellationDemapper> demapper_;

    // Message port for L1 config
    pmt::pmt_t port_l1_config_;

    void reconfigure();
    void handle_l1_config(pmt::pmt_t msg);

public:
    constellation_demapper_impl(int modulation, int code_rate, float noise_variance);
    ~constellation_demapper_impl() override;

    int work(int noutput_items, gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

    // Public API
    void set_modulation(int modulation) override;
    void set_code_rate(int code_rate) override;
    void set_noise_variance(float noise_variance) override;
    int get_bits_per_symbol() const override {
        return static_cast<int>(demapper_->bits_per_symbol());
    }
    void set_plp_id(int plp_id) override;
    int get_plp_id() const override {
        return plp_id_;
    }
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_CONSTELLATION_DEMAPPER_IMPL_H */
