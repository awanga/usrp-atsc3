// iq_source.cc — Common IQSource utilities

#include "iq_source.h"

namespace atsc3 {
namespace hal {

const char* iq_source_error_string(IQSourceError err) {
    switch (err) {
        case IQSourceError::kSuccess:
            return "Success";
        case IQSourceError::kDeviceNotFound:
            return "Device not found";
        case IQSourceError::kFrequencyOutOfRange:
            return "Frequency out of range";
        case IQSourceError::kGainOutOfRange:
            return "Gain out of range";
        case IQSourceError::kSampleRateNotSupported:
            return "Sample rate not supported";
        case IQSourceError::kBufferOverflow:
            return "Buffer overflow";
        case IQSourceError::kBufferUnderflow:
            return "Buffer underflow";
        case IQSourceError::kTimeout:
            return "Timeout waiting for samples";
        case IQSourceError::kDeviceError:
            return "Device error";
        case IQSourceError::kFileNotFound:
            return "File not found";
        case IQSourceError::kFileReadError:
            return "File read error";
        case IQSourceError::kEndOfFile:
            return "End of file";
        default:
            return "Unknown error";
    }
}

}  // namespace hal
}  // namespace atsc3
