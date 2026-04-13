// uhd_source.cc — UHDSource implementation
//
// Acquires IQ samples from USRP hardware via UHD API.
// Supports TVRX daughterboard (50-860 MHz) on N2x0 platform.

#include "iq_source.h"
#include "iq_source_factory.h"

#ifdef ATSC3_HAS_UHD

#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/version.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace atsc3 {
namespace hal {

// TVRX daughterboard specifications
constexpr double kTvrxMinFreqHz = 50e6;
constexpr double kTvrxMaxFreqHz = 860e6;
constexpr double kTvrxMinGainDb = 0.0;
constexpr double kTvrxMaxGainDb = 95.0;

// N210 over GigE maximum sustainable sample rate
constexpr double kN210MaxSampleRate = 25e6;
constexpr double kMinSampleRate = 1e6;

// Streaming timeout
constexpr double kStreamTimeoutSec = 1.0;

class UHDSource : public IQSource {
public:
    explicit UHDSource(const std::string& device_args)
        : device_args_(device_args),
          frequency_(500e6),
          gain_(30.0),
          sample_rate_(6.25e6),
          last_error_(IQSourceError::kSuccess),
          connected_(false) {
        try {
            // Create the USRP device
            usrp_ = uhd::usrp::multi_usrp::make(device_args);

            // Get device info
            device_name_ = usrp_->get_mboard_name();

            // Get actual tuning range from device
            uhd::freq_range_t freq_range = usrp_->get_rx_freq_range();
            actual_min_freq_ = freq_range.start();
            actual_max_freq_ = freq_range.stop();

            // Get actual gain range from device
            uhd::gain_range_t gain_range = usrp_->get_rx_gain_range();
            actual_min_gain_ = gain_range.start();
            actual_max_gain_ = gain_range.stop();

            // Set initial parameters
            usrp_->set_rx_rate(sample_rate_);
            usrp_->set_rx_freq(frequency_);
            usrp_->set_rx_gain(gain_);

            // Set up streaming
            setup_streaming();

            connected_ = true;
            last_error_ = IQSourceError::kSuccess;

        } catch (const uhd::exception& e) {
            std::cerr << "UHD error: " << e.what() << std::endl;
            last_error_ = IQSourceError::kDeviceNotFound;
            connected_ = false;
        } catch (const std::exception& e) {
            std::cerr << "Error creating UHD source: " << e.what() << std::endl;
            last_error_ = IQSourceError::kDeviceError;
            connected_ = false;
        }
    }

    ~UHDSource() override {
        if (rx_stream_) {
            // Issue stop command
            uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
            rx_stream_->issue_stream_cmd(stream_cmd);
        }
    }

    //--------------------------------------------------------------------------
    // Configuration
    //--------------------------------------------------------------------------

    IQSourceError set_frequency(double hz) override {
        if (!connected_) {
            last_error_ = IQSourceError::kDeviceNotFound;
            return last_error_;
        }

        // Validate against TVRX range
        if (hz < kTvrxMinFreqHz || hz > kTvrxMaxFreqHz) {
            std::cerr << "Warning: Frequency " << hz / 1e6 << " MHz is outside "
                      << "TVRX range (" << kTvrxMinFreqHz / 1e6 << "-"
                      << kTvrxMaxFreqHz / 1e6 << " MHz)" << std::endl;
        }

        // Also check device's actual range
        if (hz < actual_min_freq_ || hz > actual_max_freq_) {
            last_error_ = IQSourceError::kFrequencyOutOfRange;
            return last_error_;
        }

        try {
            usrp_->set_rx_freq(hz);
            frequency_ = usrp_->get_rx_freq();
            last_error_ = IQSourceError::kSuccess;
        } catch (const uhd::exception& e) {
            std::cerr << "UHD error setting frequency: " << e.what() << std::endl;
            last_error_ = IQSourceError::kDeviceError;
        }
        return last_error_;
    }

    double get_frequency() const override { return frequency_; }

    IQSourceError set_gain(double db) override {
        if (!connected_) {
            last_error_ = IQSourceError::kDeviceNotFound;
            return last_error_;
        }

        // Clamp gain to TVRX range with warning
        double clamped_gain = db;
        if (db < kTvrxMinGainDb) {
            std::cerr << "Warning: Gain " << db << " dB clamped to "
                      << kTvrxMinGainDb << " dB (TVRX minimum)" << std::endl;
            clamped_gain = kTvrxMinGainDb;
        } else if (db > kTvrxMaxGainDb) {
            std::cerr << "Warning: Gain " << db << " dB clamped to "
                      << kTvrxMaxGainDb << " dB (TVRX maximum)" << std::endl;
            clamped_gain = kTvrxMaxGainDb;
        }

        // Also check device's actual range
        clamped_gain = std::max(clamped_gain, actual_min_gain_);
        clamped_gain = std::min(clamped_gain, actual_max_gain_);

        try {
            usrp_->set_rx_gain(clamped_gain);
            gain_ = usrp_->get_rx_gain();
            last_error_ = IQSourceError::kSuccess;
        } catch (const uhd::exception& e) {
            std::cerr << "UHD error setting gain: " << e.what() << std::endl;
            last_error_ = IQSourceError::kDeviceError;
        }
        return last_error_;
    }

    double get_gain() const override { return gain_; }

    IQSourceError set_sample_rate(double sps) override {
        if (!connected_) {
            last_error_ = IQSourceError::kDeviceNotFound;
            return last_error_;
        }

        // Validate sample rate for N210 over GigE
        if (sps < kMinSampleRate) {
            last_error_ = IQSourceError::kSampleRateNotSupported;
            return last_error_;
        }

        if (sps > kN210MaxSampleRate) {
            std::cerr << "Warning: Sample rate " << sps / 1e6 << " MS/s exceeds "
                      << "N210 GigE limit (" << kN210MaxSampleRate / 1e6 << " MS/s). "
                      << "May result in overflows." << std::endl;
        }

        try {
            usrp_->set_rx_rate(sps);
            sample_rate_ = usrp_->get_rx_rate();

            // Reconfigure streaming for new sample rate
            if (rx_stream_) {
                uhd::stream_cmd_t stream_cmd(
                    uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
                rx_stream_->issue_stream_cmd(stream_cmd);
            }
            setup_streaming();

            last_error_ = IQSourceError::kSuccess;
        } catch (const uhd::exception& e) {
            std::cerr << "UHD error setting sample rate: " << e.what() << std::endl;
            last_error_ = IQSourceError::kDeviceError;
        }
        return last_error_;
    }

    double get_sample_rate() const override { return sample_rate_; }

    //--------------------------------------------------------------------------
    // Data acquisition
    //--------------------------------------------------------------------------

    size_t read(std::complex<float>* buf, size_t n) override {
        if (!connected_ || !rx_stream_) {
            last_error_ = IQSourceError::kDeviceNotFound;
            return 0;
        }

        try {
            uhd::rx_metadata_t md;
            size_t samples_received = rx_stream_->recv(buf, n, md, kStreamTimeoutSec);

            // Check for errors
            if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
                last_error_ = IQSourceError::kTimeout;
                return samples_received;
            } else if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
                // Overflow is common, just log it
                std::cerr << "O" << std::flush;  // Single char overflow indicator
                last_error_ = IQSourceError::kBufferOverflow;
                return samples_received;
            } else if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
                std::cerr << "UHD receive error: " << md.strerror() << std::endl;
                last_error_ = IQSourceError::kDeviceError;
                return samples_received;
            }

            last_error_ = IQSourceError::kSuccess;
            return samples_received;

        } catch (const uhd::exception& e) {
            std::cerr << "UHD error during receive: " << e.what() << std::endl;
            last_error_ = IQSourceError::kDeviceError;
            return 0;
        }
    }

    IQSourceError get_last_error() const override { return last_error_; }

    //--------------------------------------------------------------------------
    // Signal quality
    //--------------------------------------------------------------------------

    double get_rssi_dbm() override {
        if (!connected_) {
            return -120.0;
        }

        try {
            // Try to read RSSI sensor from TVRX daughterboard
            // Sensor name varies by daughterboard
            std::vector<std::string> sensors = usrp_->get_rx_sensor_names();

            for (const auto& sensor_name : sensors) {
                if (sensor_name == "rssi" || sensor_name == "lo_locked") {
                    if (sensor_name == "rssi") {
                        uhd::sensor_value_t rssi = usrp_->get_rx_sensor("rssi");
                        return rssi.to_real();
                    }
                }
            }

            // If no RSSI sensor, return a placeholder
            // This is expected for some daughterboards
            return -80.0;

        } catch (const uhd::exception& e) {
            // RSSI sensor might not be available
            return -80.0;
        }
    }

    //--------------------------------------------------------------------------
    // Device info
    //--------------------------------------------------------------------------

    std::string get_device_name() const override {
        return "UHDSource: " + device_name_;
    }

    bool is_connected() const override { return connected_; }

    double get_min_frequency() const override { return actual_min_freq_; }
    double get_max_frequency() const override { return actual_max_freq_; }

    double get_min_gain() const override { return actual_min_gain_; }
    double get_max_gain() const override { return actual_max_gain_; }

    double get_min_sample_rate() const override { return kMinSampleRate; }
    double get_max_sample_rate() const override { return kN210MaxSampleRate; }

private:
    void setup_streaming() {
        // Configure stream args
        uhd::stream_args_t stream_args("fc32", "sc16");  // Host format, wire format
        rx_stream_ = usrp_->get_rx_stream(stream_args);

        // Start continuous streaming
        uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        stream_cmd.stream_now = true;
        rx_stream_->issue_stream_cmd(stream_cmd);
    }

    std::string device_args_;
    std::string device_name_;
    double frequency_;
    double gain_;
    double sample_rate_;
    IQSourceError last_error_;
    bool connected_;

    double actual_min_freq_ = kTvrxMinFreqHz;
    double actual_max_freq_ = kTvrxMaxFreqHz;
    double actual_min_gain_ = kTvrxMinGainDb;
    double actual_max_gain_ = kTvrxMaxGainDb;

    uhd::usrp::multi_usrp::sptr usrp_;
    uhd::rx_streamer::sptr rx_stream_;
};

IQSourcePtr create_uhd_source(const std::string& device_args) {
    auto source = std::make_unique<UHDSource>(device_args);
    if (!source->is_connected()) {
        return nullptr;
    }
    return source;
}

bool uhd_support_available() {
    return true;
}

std::vector<std::string> list_uhd_devices() {
    std::vector<std::string> devices;
    try {
        uhd::device_addrs_t addrs = uhd::device::find(uhd::device_addr_t());
        for (const auto& addr : addrs) {
            devices.push_back(addr.to_string());
        }
    } catch (const uhd::exception& e) {
        std::cerr << "Error listing UHD devices: " << e.what() << std::endl;
    }
    return devices;
}

}  // namespace hal
}  // namespace atsc3

#else  // !ATSC3_HAS_UHD

// Stub implementations when UHD is not available

namespace atsc3 {
namespace hal {

IQSourcePtr create_uhd_source(const std::string& /* device_args */) {
    return nullptr;
}

bool uhd_support_available() {
    return false;
}

std::vector<std::string> list_uhd_devices() {
    return {};
}

}  // namespace hal
}  // namespace atsc3

#endif  // ATSC3_HAS_UHD
