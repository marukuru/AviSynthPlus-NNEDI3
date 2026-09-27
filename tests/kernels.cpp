// SPDX-License-Identifier: GPL-2.0-or-later
#include "Parallel.h"
#include "PortableKernels.h"
#include <avs/cpuid.h>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

struct Job {
    unsigned calls = 0;
    bool fail = false;
};

static void work(void *arg)
{
    auto *entry = static_cast<Public_MT_Data_Thread *>(arg);
    auto *job = static_cast<Job *>(entry->pData);
    ++job->calls;
    if (job->fail) throw std::runtime_error("worker error");
}

int main()
{
    try {
        for (unsigned threads : {1u, 2u, 7u}) {
            ParallelExecutor executor(threads);
            std::vector<Job> state(threads);
            std::vector<Public_MT_Data_Thread> jobs(threads);
            for (unsigned i = 0; i < threads; ++i) {
                jobs[i].pFunc = work;
                jobs[i].pData = &state[i];
            }
            for (unsigned iteration = 0; iteration < 100; ++iteration) {
                const bool fail = iteration % 13 == 0;
                state[iteration % threads].fail = fail;
                bool caught = false;
                try { executor.run(jobs.data()); }
                catch (const std::runtime_error &) { caught = true; }
                if (caught != fail) throw std::runtime_error("worker exception lost");
                for (auto &job : state) {
                    if (job.calls != iteration + 1) throw std::runtime_error("worker skipped or duplicated a pass");
                    job.fail = false;
                }
            }
        }

        // Synthetic masks exercise dispatch even when the host lacks an ISA.
        const int all = optimizationRequirements(8) | CPUF_FMA4;
        if (resolveOptimization(0, all, true) != 8 || resolveOptimization(0, 0, true) != 1 ||
            resolveOptimization(0, all, false) != 1 || resolveOptimization(9, all, true) != -1 ||
            resolveOptimization(-1, all, true) != -1)
            throw std::runtime_error("invalid automatic CPU dispatch");
        for (int opt = 2; opt <= 8; ++opt) {
            const int required = optimizationRequirements(opt);
            if (resolveOptimization(opt, required, true) != opt || resolveOptimization(opt, all, false) != -1)
                throw std::runtime_error("invalid forced CPU dispatch");
            for (int bit = 0; bit < 31; ++bit) if (required & (1 << bit))
                if (resolveOptimization(opt, all & ~(1 << bit), true) != -1)
                    throw std::runtime_error("missing CPU prerequisite accepted");
        }
        if (resolveOptimization(0, optimizationRequirements(6) | CPUF_FMA4, true) != 6 ||
            resolveOptimization(0, optimizationRequirements(7), true) != 7)
            throw std::runtime_error("invalid FMA preference");

        std::vector<int> flags;
#if defined(__i386__) || defined(__x86_64__)
        if (__builtin_cpu_supports("sse2")) flags.push_back(CPUF_SSE2);
        if (__builtin_cpu_supports("avx2")) flags.push_back(CPUF_AVX2);
#endif
        int cases = 0;
        for (int cpu : flags) for (bool wide : {false, true})
        for (int length : {32, 48, 64, 96, 128, 192, 288}) for (int count : {32, 64, 128, 256, 512}) {
            std::vector<float> input(length), storage(count * length), out(count);
            auto *data = reinterpret_cast<int16_t *>(input.data());
            auto *weights = reinterpret_cast<int16_t *>(storage.data());
            auto *factors = reinterpret_cast<float *>(weights + count * length);
            for (int j = 0; j < length; ++j) data[j] = wide ? (j*8179) % 32768 : (j*127) % 256;
            for (int j = 0; j < count * length; ++j) weights[j] = static_cast<int16_t>(j*7919u);
            for (int i = 0; i < count*2; ++i) factors[i] = float(i % 13 - 6) / 17.0f;
            float scale = 0.137f;
            auto dot = selectIntegerDotProduct(cpu, wide);
            if (!dot) throw std::runtime_error("missing SIMD kernel");
            dot(input.data(), storage.data(), out.data(), count, length, &scale);
            for (int i = 0; i < count; ++i) {
                int64_t sum = 0;
                for (int j = 0; j < length; ++j) sum += int(data[j]) * int(weights[i*length+j]);
                const int off = i/4*8 + i%4;
                const float expected = float(sum) * factors[off] * scale + factors[off+4];
                if (out[i] != expected) throw std::runtime_error("SIMD differs from 64-bit reference");
            }
            ++cases;
        }
        std::cout << "Passed worker reuse/exception checks and " << cases << " SIMD kernel comparisons.\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
