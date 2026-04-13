#pragma once

// IQSource — Hardware Abstraction Layer for IQ sample acquisition
//
// All downstream DSP code uses IQSource* only, never direct UHD or file I/O.
// This enables:
//   - Unit testing with NullSource (zeros) or FileSource (IQ captures)
//   - Swapping hardware backends without changing DSP code
//   - Future FPGA streaming via custom IQSource implementation

#include <complex>
#include <cstddef>
#include <memory>
#include <string>

namespace atsc3 {
namespace hal {

// Forward declarations for error handling
enum class IQSourceError {
    kSuccess = 0,
    kDeviceNotFound,
    kFrequencyOutOfRange,
    kGainOutOfRange,
    kSampleRateNotSupported,
    kBufferOverflow,
    kBufferUnderflow,
    kTimeout,
    kDeviceError,
    kFileNotFound,
    kFileReadError,
    kEndOfFile,
};

// Convert error code to human-readable string
const char* iq_source_error_string(IQSourceError err);

// IQSource interface — pure virtual base class
//
// All methods that can fail return IQSourceError.
// Hardware parameters are validated against device capabilities.
class IQSource {
public:
    virtual ~IQSource() = default;

    // Prevent copying (sources may hold hardware handles)
    IQSource(const IQSource&) = delete;
    IQSource& operator=(const IQSource&) = delete;

    // Allow move semantics
    IQSource(IQSource&&) = default;
    IQSource& operator=(IQSource&&) = default;

    //--------------------------------------------------------------------------
    // Configuration
    //--------------------------------------------------------------------------

    // Set center frequency in Hz
    // Returns error if frequency is outside device tuning range
    virtual IQSourceError set_frequency(double hz) = 0;

    // Get current center frequency in Hz
    virtual double get_frequency() const = 0;

    // Set gain in dB
    // Returns error if gain is outside device range
    // Note: TVRX gain range is 0-95 dB
    virtual IQSourceError set_gain(double db) = 0;

    // Get current gain in dB
    virtual double get_gain() const = 0;

    // Set sample rate in samples per second
    // Returns error if rate is not supported by device
    // Note: N210 over GigE supports up to ~25 MS/s
    virtual IQSourceError set_sample_rate(double sps) = 0;

    // Get current sample rate in samples per second
    virtual double get_sample_rate() const = 0;

    //--------------------------------------------------------------------------
    // Data acquisition
    //--------------------------------------------------------------------------

    // Read IQ samples into buffer
    // Returns number of samples actually read (may be less than n)
    // buf must point to at least n elements
    //
    // Blocking: waits for samples to be available (with internal timeout)
    // Returns 0 on timeout or end-of-file
    virtual size_t read(std::complex<float>* buf, size_t n) = 0;

    // Get last error from read() or configuration methods
    virtual IQSourceError get_last_error() const = 0;

    //--------------------------------------------------------------------------
    // Signal quality
    //--------------------------------------------------------------------------

    // Get received signal strength in dBm
    // For hardware sources: reads from device RSSI register
    // For file sources: computes from signal power
    virtual double get_rssi_dbm() = 0;

    //--------------------------------------------------------------------------
    // Device info
    //--------------------------------------------------------------------------

    // Get human-readable device name/description
    virtual std::string get_device_name() const = 0;

    // Check if source is connected and operational
    virtual bool is_connected() const = 0;

    // Get device-specific tuning range
    virtual double get_min_frequency() const = 0;
    virtual double get_max_frequency() const = 0;

    // Get device-specific gain range
    virtual double get_min_gain() const = 0;
    virtual double get_max_gain() const = 0;

    // Get device-specific sample rate limits
    virtual double get_min_sample_rate() const = 0;
    virtual double get_max_sample_rate() const = 0;

protected:
    // Protected constructor - use factory functions
    IQSource() = default;
};

// Shared pointer type for polymorphic use
using IQSourcePtr = std::unique_ptr<IQSource>;

}  // namespace hal
}  // namespace atsc3
