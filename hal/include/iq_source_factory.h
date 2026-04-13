#pragma once

// IQSource Factory — Create IQSource instances for different backends
//
// Usage:
//   auto src = atsc3::hal::create_file_source("capture.iq", 6.25e6);
//   auto src = atsc3::hal::create_uhd_source("addr=192.168.10.2");
//   auto src = atsc3::hal::create_null_source();

#include "iq_source.h"

#include <string>
#include <vector>

namespace atsc3 {
namespace hal {

//------------------------------------------------------------------------------
// Factory functions
//------------------------------------------------------------------------------

// Create a NullSource that returns zeros
// Useful for unit testing downstream code without hardware or files
//
// Parameters:
//   sample_rate: Sample rate to report (default: 6.25 MS/s)
IQSourcePtr create_null_source(double sample_rate = 6.25e6);

// Create a FileSource that reads IQ samples from a file
//
// Parameters:
//   file_path:   Path to .iq file (interleaved complex<float32>)
//   sample_rate: Sample rate of the recording
//   loop:        If true, restart from beginning when EOF reached (default: false)
//
// Returns nullptr on failure (file not found, etc.)
IQSourcePtr create_file_source(const std::string& file_path,
                               double sample_rate,
                               bool loop = false);

// Create a UHDSource that acquires samples from USRP hardware
//
// Parameters:
//   device_args: UHD device address string (e.g., "addr=192.168.10.2")
//                Empty string uses first available device
//
// Returns nullptr if UHD support is not compiled in or device not found
//
// Note: UHD support requires linking against libuhd and is controlled by
// the ATSC3_HAS_UHD compile definition.
IQSourcePtr create_uhd_source(const std::string& device_args = "");

//------------------------------------------------------------------------------
// Utility functions
//------------------------------------------------------------------------------

// Check if UHD support is compiled in
bool uhd_support_available();

// List available UHD devices
// Returns a vector of device address strings
// Empty vector if no devices found or UHD not available
std::vector<std::string> list_uhd_devices();

}  // namespace hal
}  // namespace atsc3
