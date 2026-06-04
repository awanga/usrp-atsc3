// bench_fftw.cc — Performance benchmark for FFTW plan modes
//
// Compares FFTW_ESTIMATE, FFTW_MEASURE, and FFTW_PATIENT planning modes
// for ATSC 3.0 FFT sizes (4K, 8K, 16K, 32K).
//
// Run with: ./bench_fftw --gtest_filter=*Benchmark*
//
// Measures:
// - Planning time (one-time cost)
// - Execution time per FFT (amortized cost)
//
// This benchmark directly uses FFTW to test all three planning modes,
// as the FftEngine wrapper only exposes FFTW_ESTIMATE and FFTW_PATIENT.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

#ifndef ATSC3_FIXED_POINT
#include <fftw3.h>
#endif

namespace atsc3 {
namespace ofdm {
namespace {

// RDTSC cycle counter for precise timing
#if defined(__x86_64__) || defined(__i386__)
inline uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#else
inline uint64_t rdtsc() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}
#endif

// Get CPU frequency estimate (cycles per second)
uint64_t estimate_cpu_freq() {
    auto start_time = std::chrono::high_resolution_clock::now();
    uint64_t start_cycles = rdtsc();

    while (true) {
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
        if (elapsed.count() >= 100)
            break;
    }

    uint64_t end_cycles = rdtsc();
    auto end_time = std::chrono::high_resolution_clock::now();

    auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    uint64_t cycles = end_cycles - start_cycles;

    return (cycles * 1000000) / elapsed_us;
}

#ifndef ATSC3_FIXED_POINT

// Plan mode descriptor
struct PlanMode {
    const char* name;
    unsigned flag;
};

// FFTW plan modes to test
const PlanMode plan_modes[] = {
    {"ESTIMATE", FFTW_ESTIMATE}, {"MEASURE", FFTW_MEASURE}, {"PATIENT", FFTW_PATIENT}};

// ATSC 3.0 FFT sizes
const size_t fft_sizes[] = {4096, 8192, 16384, 32768};

class FftwBenchmark : public ::testing::Test {
protected:
    void SetUp() override {
        cpu_freq_ = estimate_cpu_freq();
        printf("\nCPU frequency estimate: %.2f GHz\n\n", cpu_freq_ / 1e9);
    }

    uint64_t cpu_freq_;
};

// Test planning time for each mode
TEST_F(FftwBenchmark, PlanningTime) {
    printf("FFTW Planning Time (one-time cost):\n");
    printf("===================================\n");
    printf("%-10s %8s %12s %12s %12s\n", "Size", "Mode", "Time (ms)", "Time (s)", "Speedup");
    printf("---------- -------- ------------ ------------ ------------\n");

    for (size_t fft_size : fft_sizes) {
        double estimate_time_ms = 0.0;

        for (const PlanMode& mode : plan_modes) {
            // Allocate buffers
            fftwf_complex* in = fftwf_alloc_complex(fft_size);
            fftwf_complex* out = fftwf_alloc_complex(fft_size);

            // Fill with random data
            std::mt19937 rng(42);
            std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
            for (size_t i = 0; i < fft_size; ++i) {
                in[i][0] = dist(rng);
                in[i][1] = dist(rng);
            }

            // Time plan creation
            auto start = std::chrono::high_resolution_clock::now();

            fftwf_plan plan = fftwf_plan_dft_1d(static_cast<int>(fft_size), in, out, FFTW_FORWARD,
                                                mode.flag | FFTW_DESTROY_INPUT);

            auto end = std::chrono::high_resolution_clock::now();
            auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
            double elapsed_s = elapsed_ms / 1000.0;

            // Calculate speedup relative to ESTIMATE
            double speedup = 1.0;
            if (mode.flag == FFTW_ESTIMATE) {
                estimate_time_ms = elapsed_ms;
            } else if (estimate_time_ms > 0) {
                speedup = elapsed_ms / estimate_time_ms;
            }

            printf("%8zuK %8s %12.3f %12.4f %12.1fx\n", fft_size / 1024, mode.name, elapsed_ms,
                   elapsed_s, speedup);

            // Cleanup
            fftwf_destroy_plan(plan);
            fftwf_free(in);
            fftwf_free(out);
        }
    }
    printf("\n");
}

// Test execution time for each mode
TEST_F(FftwBenchmark, ExecutionTime) {
    printf("FFTW Execution Time (per FFT, averaged over 1000 iterations):\n");
    printf("==============================================================\n");
    printf("%-10s %8s %12s %12s %12s %12s\n", "Size", "Mode", "Time (us)", "Cycles/sample",
           "Throughput", "vs ESTIMATE");
    printf("---------- -------- ------------ ------------ ------------ ------------\n");

    for (size_t fft_size : fft_sizes) {
        double estimate_time_us = 0.0;

        for (const PlanMode& mode : plan_modes) {
            // Allocate buffers
            fftwf_complex* in = fftwf_alloc_complex(fft_size);
            fftwf_complex* out = fftwf_alloc_complex(fft_size);

            // Fill with random data
            std::mt19937 rng(42);
            std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
            for (size_t i = 0; i < fft_size; ++i) {
                in[i][0] = dist(rng);
                in[i][1] = dist(rng);
            }

            // Create plan
            fftwf_plan plan = fftwf_plan_dft_1d(static_cast<int>(fft_size), in, out, FFTW_FORWARD,
                                                mode.flag | FFTW_DESTROY_INPUT);

            // Warm up
            for (int i = 0; i < 10; ++i) {
                fftwf_execute(plan);
            }

            // Benchmark
            const size_t iterations = 1000;
            uint64_t start_cycles = rdtsc();
            auto start = std::chrono::high_resolution_clock::now();

            for (size_t i = 0; i < iterations; ++i) {
                fftwf_execute(plan);
            }

            uint64_t end_cycles = rdtsc();
            auto end = std::chrono::high_resolution_clock::now();

            auto elapsed_us =
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            double time_per_fft_us = static_cast<double>(elapsed_us) / iterations;
            double cycles_per_sample =
                static_cast<double>(end_cycles - start_cycles) / (iterations * fft_size);

            // Throughput in MS/s (mega-samples per second)
            double throughput_msps = (fft_size * iterations) / static_cast<double>(elapsed_us);

            // Calculate speedup relative to ESTIMATE
            double speedup = 1.0;
            if (mode.flag == FFTW_ESTIMATE) {
                estimate_time_us = time_per_fft_us;
            } else if (estimate_time_us > 0) {
                speedup = estimate_time_us / time_per_fft_us;
            }

            printf("%8zuK %8s %12.2f %12.2f %10.1f MS/s %12.2fx\n", fft_size / 1024, mode.name,
                   time_per_fft_us, cycles_per_sample, throughput_msps, speedup);

            // Cleanup
            fftwf_destroy_plan(plan);
            fftwf_free(in);
            fftwf_free(out);
        }
    }
    printf("\n");
}

// Test with wisdom caching
TEST_F(FftwBenchmark, WisdomCaching) {
    printf("FFTW Wisdom Caching (8K FFT, MEASURE mode):\n");
    printf("===========================================\n");

    const size_t fft_size = 8192;
    const char* wisdom_file = "/tmp/fftw_bench_wisdom.txt";

    // Clear any existing wisdom
    fftwf_forget_wisdom();

    // First run: no wisdom
    {
        fftwf_complex* in = fftwf_alloc_complex(fft_size);
        fftwf_complex* out = fftwf_alloc_complex(fft_size);

        auto start = std::chrono::high_resolution_clock::now();
        fftwf_plan plan =
            fftwf_plan_dft_1d(static_cast<int>(fft_size), in, out, FFTW_FORWARD, FFTW_MEASURE);
        auto end = std::chrono::high_resolution_clock::now();

        double plan_time_ms =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
        printf("First plan (no wisdom):  %8.3f ms\n", plan_time_ms);

        // Save wisdom
        fftwf_export_wisdom_to_filename(wisdom_file);

        fftwf_destroy_plan(plan);
        fftwf_free(in);
        fftwf_free(out);
    }

    // Clear and reload wisdom
    fftwf_forget_wisdom();
    fftwf_import_wisdom_from_filename(wisdom_file);

    // Second run: with wisdom
    {
        fftwf_complex* in = fftwf_alloc_complex(fft_size);
        fftwf_complex* out = fftwf_alloc_complex(fft_size);

        auto start = std::chrono::high_resolution_clock::now();
        fftwf_plan plan =
            fftwf_plan_dft_1d(static_cast<int>(fft_size), in, out, FFTW_FORWARD, FFTW_MEASURE);
        auto end = std::chrono::high_resolution_clock::now();

        double plan_time_ms =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
        printf("Second plan (with wisdom): %8.3f ms\n", plan_time_ms);

        fftwf_destroy_plan(plan);
        fftwf_free(in);
        fftwf_free(out);
    }

    printf("\n");
    printf("Recommendation: Use FFTW_MEASURE with wisdom caching for production.\n");
    printf("                Use FFTW_ESTIMATE for quick testing.\n");
    printf("                FFTW_PATIENT rarely provides significant benefit\n");
    printf("                over FFTW_MEASURE for OFDM FFT sizes.\n");
    printf("\n");

    // Clean up wisdom file
    std::remove(wisdom_file);
}

// Summary with recommendations
TEST_F(FftwBenchmark, Recommendations) {
    printf("FFTW Plan Mode Recommendations for ATSC 3.0:\n");
    printf("=============================================\n\n");

    printf("| Use Case              | Recommended Mode | Wisdom | Rationale                    |\n");
    printf("|-----------------------|------------------|--------|------------------------------|\n");
    printf("| Development/testing   | FFTW_ESTIMATE    | No     | Fast startup, acceptable perf|\n");
    printf("| Production (startup)  | FFTW_MEASURE     | Yes    | Optimal perf with caching    |\n");
    printf("| Production (cached)   | FFTW_MEASURE     | Load   | Near-instant planning        |\n");
    printf("| Batch processing      | FFTW_PATIENT     | Yes    | Maximum throughput           |\n\n");

    printf("Current FftEngine defaults:\n");
    printf("  - use_patient_plan = false: Uses FFTW_ESTIMATE\n");
    printf("  - use_patient_plan = true:  Uses FFTW_PATIENT\n\n");

    printf("Recommendation: Add FFTW_MEASURE option to FftConfig for production use.\n");
}

#else  // ATSC3_FIXED_POINT

TEST(FftwBenchmark, FixedPointNoFftw) {
    printf("FFTW benchmark not available in fixed-point mode.\n");
    printf("Fixed-point mode uses Cooley-Tukey reference implementation.\n");
}

#endif  // ATSC3_FIXED_POINT

}  // namespace
}  // namespace ofdm
}  // namespace atsc3
