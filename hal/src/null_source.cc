// null_source.cc — NullSource implementation
//
// Returns zeros for all reads. Useful for testing downstream DSP code
// without requiring hardware or IQ capture files.

#include "iq_source.h"
#include "iq_source_factory.h"

#include <algorithm>

namespace atsc3 {
namespace hal {

class NullSource : public IQSource {
public:
    explicit NullSource(double sample_rate)
        : sample_rate_(sample_rate),
          frequency_(500e6),
          gain_(30.0),
          last_error_(IQSourceError::kSuccess) {}

    ~NullSource() override = default;

    //--------------------------------------------------------------------------
    // Configuration
    //--------------------------------------------------------------------------

    IQSourceError set_frequency(double hz) override {
        // Accept any frequency in a reasonable range
        if (hz < 1e6 || hz > 10e9) {
            last_error_ = IQSourceError::kFrequencyOutOfRange;
            return last_error_;
        }
        frequency_ = hz;
        last_error_ = IQSourceError::kSuccess;
        return last_error_;
    }

    double get_frequency() const override { return frequency_; }

    IQSourceError set_gain(double db) override {
        // Accept any gain in a reasonable range
        if (db < 0.0 || db > 100.0) {
            last_error_ = IQSourceError::kGainOutOfRange;
            return last_error_;
        }
        gain_ = db;
        last_error_ = IQSourceError::kSuccess;
        return last_error_;
    }

    double get_gain() const override { return gain_; }

    IQSourceError set_sample_rate(double sps) override {
        // Accept any reasonable sample rate
        if (sps < 1e3 || sps > 100e6) {
            last_error_ = IQSourceError::kSampleRateNotSupported;
            return last_error_;
        }
        sample_rate_ = sps;
        last_error_ = IQSourceError::kSuccess;
        return last_error_;
    }

    double get_sample_rate() const override { return sample_rate_; }

    //--------------------------------------------------------------------------
    // Data acquisition
    //--------------------------------------------------------------------------

    size_t read(std::complex<float>* buf, size_t n) override {
        // Fill buffer with zeros
        std::fill(buf, buf + n, std::complex<float>(0.0f, 0.0f));
        last_error_ = IQSourceError::kSuccess;
        return n;
    }

    IQSourceError get_last_error() const override { return last_error_; }

    //--------------------------------------------------------------------------
    // Signal quality
    //--------------------------------------------------------------------------

    double get_rssi_dbm() override {
        // Return noise floor level for null source
        return -120.0;
    }

    //--------------------------------------------------------------------------
    // Device info
    //--------------------------------------------------------------------------

    std::string get_device_name() const override { return "NullSource (test)"; }

    bool is_connected() const override { return true; }

    double get_min_frequency() const override { return 1e6; }
    double get_max_frequency() const override { return 10e9; }

    double get_min_gain() const override { return 0.0; }
    double get_max_gain() const override { return 100.0; }

    double get_min_sample_rate() const override { return 1e3; }
    double get_max_sample_rate() const override { return 100e6; }

private:
    double sample_rate_;
    double frequency_;
    double gain_;
    IQSourceError last_error_;
};

IQSourcePtr create_null_source(double sample_rate) {
    return std::make_unique<NullSource>(sample_rate);
}

}  // namespace hal
}  // namespace atsc3
