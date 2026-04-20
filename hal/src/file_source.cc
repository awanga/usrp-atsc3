// file_source.cc — FileSource implementation
//
// Reads IQ samples from a file containing interleaved complex<float32> data.
// Supports looping for CI replay tests.

#include "iq_source.h"
#include "iq_source_factory.h"

#include <cmath>
#include <fstream>
#include <vector>

namespace atsc3 {
namespace hal {

class FileSource : public IQSource {
public:
    FileSource(const std::string& file_path, double sample_rate, bool loop)
        : file_path_(file_path),
          sample_rate_(sample_rate),
          loop_(loop),
          frequency_(500e6),
          gain_(0.0),
          last_error_(IQSourceError::kSuccess),
          rssi_dbm_(-100.0),
          samples_read_(0) {
        file_.open(file_path, std::ios::binary);
        if (!file_.is_open()) {
            last_error_ = IQSourceError::kFileNotFound;
        }
    }

    ~FileSource() override {
        if (file_.is_open()) {
            file_.close();
        }
    }

    //--------------------------------------------------------------------------
    // Configuration
    //--------------------------------------------------------------------------

    IQSourceError set_frequency(double hz) override {
        // FileSource accepts any frequency (it's just metadata)
        // In a real scenario, the file was recorded at a specific frequency
        if (hz < 1e6 || hz > 10e9) {
            last_error_ = IQSourceError::kFrequencyOutOfRange;
            return last_error_;
        }
        frequency_ = hz;
        last_error_ = IQSourceError::kSuccess;
        return last_error_;
    }

    double get_frequency() const override {
        return frequency_;
    }

    IQSourceError set_gain(double db) override {
        // FileSource doesn't have real gain control
        // Just store the value for compatibility
        gain_ = db;
        last_error_ = IQSourceError::kSuccess;
        return last_error_;
    }

    double get_gain() const override {
        return gain_;
    }

    IQSourceError set_sample_rate(double sps) override {
        // Sample rate should match the file's actual sample rate
        // We store it but can't change the file's data
        if (sps <= 0.0) {
            last_error_ = IQSourceError::kSampleRateNotSupported;
            return last_error_;
        }
        sample_rate_ = sps;
        last_error_ = IQSourceError::kSuccess;
        return last_error_;
    }

    double get_sample_rate() const override {
        return sample_rate_;
    }

    //--------------------------------------------------------------------------
    // Data acquisition
    //--------------------------------------------------------------------------

    size_t read(std::complex<float>* buf, size_t n) override {
        if (!file_.is_open()) {
            last_error_ = IQSourceError::kFileNotFound;
            return 0;
        }

        // Read directly into the buffer
        // complex<float> is guaranteed to be two contiguous floats
        file_.read(reinterpret_cast<char*>(buf), n * sizeof(std::complex<float>));

        size_t bytes_read = static_cast<size_t>(file_.gcount());
        size_t samples_read = bytes_read / sizeof(std::complex<float>);

        if (samples_read < n) {
            if (file_.eof()) {
                if (loop_) {
                    // Reset to beginning and read remaining samples
                    file_.clear();
                    file_.seekg(0, std::ios::beg);

                    // Try to read the remaining samples
                    size_t remaining = n - samples_read;
                    file_.read(reinterpret_cast<char*>(buf + samples_read),
                               remaining * sizeof(std::complex<float>));
                    bytes_read = static_cast<size_t>(file_.gcount());
                    samples_read += bytes_read / sizeof(std::complex<float>);
                    last_error_ = IQSourceError::kSuccess;
                } else {
                    last_error_ = IQSourceError::kEndOfFile;
                }
            } else if (file_.fail()) {
                last_error_ = IQSourceError::kFileReadError;
            }
        } else {
            last_error_ = IQSourceError::kSuccess;
        }

        // Update RSSI estimate from read samples
        if (samples_read > 0) {
            update_rssi(buf, samples_read);
        }

        samples_read_ += samples_read;
        return samples_read;
    }

    IQSourceError get_last_error() const override {
        return last_error_;
    }

    //--------------------------------------------------------------------------
    // Signal quality
    //--------------------------------------------------------------------------

    double get_rssi_dbm() override {
        // Return RSSI computed from signal power
        // This is an approximation assuming 50 ohm impedance
        return rssi_dbm_;
    }

    //--------------------------------------------------------------------------
    // Device info
    //--------------------------------------------------------------------------

    std::string get_device_name() const override {
        return "FileSource: " + file_path_;
    }

    bool is_connected() const override {
        return file_.is_open();
    }

    double get_min_frequency() const override {
        return 1e6;
    }
    double get_max_frequency() const override {
        return 10e9;
    }

    double get_min_gain() const override {
        return 0.0;
    }
    double get_max_gain() const override {
        return 100.0;
    }

    double get_min_sample_rate() const override {
        return 1e3;
    }
    double get_max_sample_rate() const override {
        return 100e6;
    }

private:
    // Update RSSI estimate from sample buffer
    void update_rssi(const std::complex<float>* buf, size_t n) {
        // Compute average power
        double power_sum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            float re = buf[i].real();
            float im = buf[i].imag();
            power_sum += static_cast<double>(re * re + im * im);
        }
        double avg_power = power_sum / static_cast<double>(n);

        // Convert to dBm assuming 50 ohm impedance and 1V = 0 dBFS
        // P_dBm = 10 * log10(P_watts / 1mW)
        // For normalized samples, assume full scale = 1V RMS into 50 ohms = +10 dBm
        // So 0 dB relative power = +10 dBm
        constexpr double kFullScaleDbm = 10.0;
        if (avg_power > 1e-20) {
            rssi_dbm_ = kFullScaleDbm + 10.0 * std::log10(avg_power);
        } else {
            rssi_dbm_ = -120.0;  // Noise floor
        }
    }

    std::string file_path_;
    double sample_rate_;
    bool loop_;
    double frequency_;
    double gain_;
    IQSourceError last_error_;
    double rssi_dbm_;
    size_t samples_read_;

    std::ifstream file_;
};

IQSourcePtr create_file_source(const std::string& file_path, double sample_rate, bool loop) {
    auto source = std::make_unique<FileSource>(file_path, sample_rate, loop);
    if (!source->is_connected()) {
        return nullptr;
    }
    return source;
}

}  // namespace hal
}  // namespace atsc3
