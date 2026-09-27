// SPDX-License-Identifier: GPL-2.0-or-later
#include "Parallel.h"
#include <algorithm>
#include <fstream>
#include <set>
#include <utility>
#ifdef __linux__
#include <sched.h>
#endif

ParallelExecutor::ParallelExecutor(unsigned count)
{
    try {
        for (unsigned i = 1; i < count; ++i)
            workers_.emplace_back(&ParallelExecutor::worker, this, i);
    } catch (...) {
        stop();
        throw;
    }
}

ParallelExecutor::~ParallelExecutor() { stop(); }

void ParallelExecutor::stop() noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto &thread : workers_)
        thread.join();
}

void ParallelExecutor::execute(unsigned index) noexcept
{
    try {
        jobs_[index].pFunc(&jobs_[index]);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!error_)
            error_ = std::current_exception();
    }
}

void ParallelExecutor::worker(unsigned index)
{
    unsigned generation = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        ready_.wait(lock, [&] { return stopping_ || generation != generation_; });
        if (stopping_)
            return;
        generation = generation_;
        lock.unlock();
        execute(index);
        lock.lock();
        if (--remaining_ == 0)
            done_.notify_one();
    }
}

void ParallelExecutor::run(Public_MT_Data_Thread *jobs)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_ = jobs;
        error_ = nullptr;
        remaining_ = static_cast<unsigned>(workers_.size());
        ++generation_;
    }
    ready_.notify_all();
    execute(0);
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&] { return remaining_ == 0; });
    if (error_)
        std::rethrow_exception(error_);
}

unsigned available_cpus(bool logical)
{
#ifdef __linux__
    cpu_set_t affinity;
    if (sched_getaffinity(0, sizeof(affinity), &affinity) == 0) {
        unsigned count = CPU_COUNT(&affinity);
        if (!logical) {
            std::set<std::pair<int, int>> cores;
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (!CPU_ISSET(cpu, &affinity))
                    continue;
                const auto path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
                int package, core;
                if (!(std::ifstream(path + "physical_package_id") >> package) ||
                    !(std::ifstream(path + "core_id") >> core))
                    return std::max(1u, count);
                cores.emplace(package, core);
            }
            count = static_cast<unsigned>(cores.size());
        }
        return std::max(1u, count);
    }
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}
