// atsc3_fec_decode_impl.cc — FEC Decoder implementation

#include "atsc3_fec_decode_impl.h"

#include <gnuradio/io_signature.h>

namespace gr {
namespace atsc3 {

fec_decode::sptr fec_decode::make(int code_rate, int codeword_length, int max_iterations) {
    return gnuradio::make_block_sptr<fec_decode_impl>(code_rate, codeword_length, max_iterations);
}

fec_decode_impl::fec_decode_impl(int code_rate, int codeword_length, int max_iterations)
    : gr::block("atsc3_fec_decode", gr::io_signature::make(1, 1, sizeof(int8_t)),
                gr::io_signature::make(1, 1, sizeof(uint8_t))),
      code_rate_(code_rate),
      codeword_length_(codeword_length),
      max_iterations_(max_iterations),
      total_codewords_(0),
      failed_codewords_(0),
      iteration_sum_(0.0) {
    reconfigure();

    // Register message port
    port_reset_in_ = pmt::intern("reset");
    message_port_register_in(port_reset_in_);
    set_msg_handler(port_reset_in_, [this](pmt::pmt_t msg) { this->handle_reset_msg(msg); });
}

fec_decode_impl::~fec_decode_impl() = default;

void fec_decode_impl::reconfigure() {
    // Configure LDPC decoder
    ::atsc3::fec::LdpcConfig config;

    // Map code rate index (0-11) to CodeRate enum
    // Index 0 = 2/15, 1 = 3/15, ..., 11 = 13/15
    config.code_rate = static_cast<::atsc3::config::CodeRate>(code_rate_);

    // Map codeword length to short_codeword flag
    config.short_codeword = (codeword_length_ == 16200);
    config.max_iterations = static_cast<size_t>(max_iterations_);

    ldpc_decoder_ = std::make_unique<::atsc3::fec::LdpcDecoder>(config);

    // Calculate output size (information bits packed into bytes)
    size_t info_bits = ldpc_decoder_->info_bits();
    size_t output_bytes = (info_bits + 7) / 8;
    decoded_bits_.resize(output_bytes);
}

void fec_decode_impl::forecast(int noutput_items, gr_vector_int& ninput_items_required) {
    // For each output byte, we need approximately codeword_length / output_bytes input LLRs
    size_t info_bits = ldpc_decoder_->info_bits();
    size_t output_bytes = (info_bits + 7) / 8;

    // Input LLRs needed per output byte
    double llrs_per_byte = static_cast<double>(codeword_length_) / output_bytes;
    ninput_items_required[0] = static_cast<int>(noutput_items * llrs_per_byte);
}

int fec_decode_impl::general_work(int noutput_items, gr_vector_int& ninput_items,
                                  gr_vector_const_void_star& input_items,
                                  gr_vector_void_star& output_items) {
    const int8_t* in = static_cast<const int8_t*>(input_items[0]);
    uint8_t* out = static_cast<uint8_t*>(output_items[0]);

    size_t info_bits = ldpc_decoder_->info_bits();
    size_t output_bytes = (info_bits + 7) / 8;

    int available_codewords = ninput_items[0] / codeword_length_;
    int output_capacity = noutput_items / static_cast<int>(output_bytes);
    int codewords_to_process = std::min(available_codewords, output_capacity);

    if (codewords_to_process == 0) {
        return 0;
    }

    int produced = 0;
    int consumed = 0;

    for (int cw = 0; cw < codewords_to_process; ++cw) {
        const int8_t* llr_in = in + cw * codeword_length_;

        // Decode codeword using result-returning API
        auto result = ldpc_decoder_->decode(llr_in);

        // Update statistics
        update_statistics(result.converged, static_cast<int>(result.iterations_used));

        // Copy decoded bits to output
        size_t bytes_to_copy = std::min(result.decoded_bits.size(), output_bytes);
        std::memcpy(out + cw * output_bytes, result.decoded_bits.data(), bytes_to_copy);

        produced += static_cast<int>(output_bytes);
        consumed += codeword_length_;
    }

    consume_each(consumed);
    return produced;
}

void fec_decode_impl::update_statistics(bool converged, int iterations) {
    total_codewords_++;
    iteration_sum_ += iterations;

    if (!converged) {
        failed_codewords_++;
    }

    // Update atomic statistics
    last_converged_.store(converged);
    avg_iterations_.store(static_cast<float>(iteration_sum_ / total_codewords_));
    fer_.store(static_cast<float>(failed_codewords_) / static_cast<float>(total_codewords_));
}

void fec_decode_impl::set_code_rate(int code_rate) {
    code_rate_ = code_rate;
    reconfigure();
}

void fec_decode_impl::set_codeword_length(int length) {
    codeword_length_ = length;
    reconfigure();
}

void fec_decode_impl::reset_stats() {
    // Reset LDPC decoder internal state
    if (ldpc_decoder_) {
        ldpc_decoder_->reset();
    }

    // Reset statistics
    total_codewords_ = 0;
    failed_codewords_ = 0;
    iteration_sum_ = 0.0;
    avg_iterations_.store(0.0f);
    fer_.store(0.0f);
    last_converged_.store(false);
}

void fec_decode_impl::handle_reset_msg(pmt::pmt_t /*msg*/) {
    reset_stats();
}

}  // namespace atsc3
}  // namespace gr
