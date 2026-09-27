// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ThreadPoolDef.h"
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

// One executor per MT_MULTI_INSTANCE filter. The caller executes slot zero;
// workers sleep between passes and keep their scratch buffers across frames.
class ParallelExecutor {
public:
    explicit ParallelExecutor(unsigned count);
    ~ParallelExecutor();
    ParallelExecutor(const ParallelExecutor &) = delete;
    ParallelExecutor &operator=(const ParallelExecutor &) = delete;
    void run(Public_MT_Data_Thread *jobs);

private:
    void worker(unsigned index);
    void execute(unsigned index) noexcept;
    void stop() noexcept;

    std::mutex mutex_;
    std::condition_variable ready_, done_;
    std::vector<std::thread> workers_;
    Public_MT_Data_Thread *jobs_ = nullptr;
    std::exception_ptr error_;
    unsigned generation_ = 0, remaining_ = 0;
    bool stopping_ = false;
};

unsigned available_cpus(bool logical);
