#include "periodic_caller/PeriodicCaller.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace periodic_caller {

namespace {

using std::chrono::nanoseconds;

// Waitable timers use 100 ns units; negative values mean "relative".
LONGLONG toRelativeDueTime(nanoseconds wait) {
    const auto ticks = (wait.count() + 99) / 100;  // Round up: never wake early.
    return -std::max<LONGLONG>(ticks, 1);
}

struct UniqueHandle {
    HANDLE h = nullptr;
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : h(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    ~UniqueHandle() {
        if (h) CloseHandle(h);
    }
};

}  // namespace

struct PeriodicCaller::Impl {
    struct Entry {
        TaskId id;
        nanoseconds period;
        Clock::time_point next;
        Task task;
        MissedTickPolicy policy;
        TaskStats stats;
        nanoseconds latenessSum{0};
        bool removed = false;
        bool executing = false;
    };

    SchedulerOptions options;
    mutable std::mutex mutex;
    std::unordered_map<TaskId, std::shared_ptr<Entry>> entries;
    TaskId nextId = 1;

    UniqueHandle timer;
    UniqueHandle wakeEvent;  // Auto-reset; signalled whenever the schedule changes.
    bool highResolution = false;
    bool timePeriodRaised = false;

    std::jthread worker;

    explicit Impl(SchedulerOptions opts) : options(opts) {
        timer.h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
        highResolution = timer.h != nullptr;
        if (!highResolution) {
            // Pre-1803 Windows: classic timer, made precise via a 1 ms system tick.
            timer.h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            timePeriodRaised = timeBeginPeriod(1) == TIMERR_NOERROR;
        }
        wakeEvent.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!timer.h || !wakeEvent.h) {
            if (timePeriodRaised) timeEndPeriod(1);
            throw std::runtime_error("PeriodicCaller: failed to create timer/event handles");
        }

        worker = std::jthread([this](std::stop_token st) { run(st); });
    }

    ~Impl() {
        stop();
        if (timePeriodRaised) timeEndPeriod(1);
    }

    void wake() { SetEvent(wakeEvent.h); }

    void stop() {
        if (!worker.joinable()) return;
        worker.request_stop();
        wake();
        // Joining from inside a task would deadlock; the loop exits on its own.
        if (worker.get_id() != std::this_thread::get_id()) worker.join();
    }

    // Sleeps until `deadline`. Returns false if woken early by wakeEvent.
    bool waitUntil(Clock::time_point deadline) {
        const auto wakeAt = deadline - options.spinThreshold;
        const auto now = Clock::now();
        if (wakeAt > now) {
            LARGE_INTEGER due;
            due.QuadPart = toRelativeDueTime(wakeAt - now);
            SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE);
            const HANDLE handles[2] = {wakeEvent.h, timer.h};
            const DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0 + 1) {
                CancelWaitableTimer(timer.h);
                return false;
            }
        }
        if (options.spinThreshold.count() > 0) {
            while (Clock::now() < deadline) YieldProcessor();
        }
        return true;
    }

    void run(std::stop_token st) {
        if (options.highPriorityThread) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        }

        std::unique_lock lock(mutex);
        while (!st.stop_requested()) {
            std::shared_ptr<Entry> due;
            for (auto& [id, e] : entries) {
                if (!due || e->next < due->next) due = e;
            }

            if (!due) {
                lock.unlock();
                WaitForSingleObject(wakeEvent.h, INFINITE);
                lock.lock();
                continue;
            }

            const auto scheduled = due->next;
            if (Clock::now() < scheduled) {
                lock.unlock();
                waitUntil(scheduled);
                lock.lock();
                continue;  // Re-evaluate: the schedule may have changed meanwhile.
            }

            due->executing = true;
            lock.unlock();
            const auto start = Clock::now();
            bool threw = false;
            try {
                due->task();
            } catch (...) {
                threw = true;
            }
            const auto end = Clock::now();
            lock.lock();
            due->executing = false;

            if (due->removed) continue;

            auto& s = due->stats;
            const auto lateness = std::chrono::duration_cast<nanoseconds>(start - scheduled);
            ++s.runs;
            if (threw) ++s.exceptions;
            s.lastLateness = lateness;
            s.maxLateness = std::max(s.maxLateness, lateness);
            due->latenessSum += lateness;
            s.avgLateness = due->latenessSum / static_cast<long long>(s.runs);

            // Advance on the absolute grid; never relative to `end` (that drifts).
            due->next += due->period;
            if (due->next <= end && due->policy == MissedTickPolicy::Skip) {
                const auto behind = (end - due->next) / due->period + 1;
                due->next += behind * due->period;
                s.missedTicks += static_cast<std::uint64_t>(behind);
            }
        }
    }
};

PeriodicCaller::PeriodicCaller(SchedulerOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

PeriodicCaller::~PeriodicCaller() = default;

TaskId PeriodicCaller::add(std::chrono::nanoseconds period, Task task, TaskOptions options) {
    if (period <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("PeriodicCaller::add: period must be positive");
    }
    if (!task) throw std::invalid_argument("PeriodicCaller::add: task is empty");

    auto entry = std::make_shared<Impl::Entry>();
    entry->period = period;
    entry->task = std::move(task);
    entry->policy = options.missedTickPolicy;
    entry->next = Clock::now() + (options.runImmediately ? std::chrono::nanoseconds::zero() : period);

    TaskId id;
    {
        std::scoped_lock lock(impl_->mutex);
        id = impl_->nextId++;
        entry->id = id;
        impl_->entries.emplace(id, std::move(entry));
    }
    impl_->wake();
    return id;
}

bool PeriodicCaller::remove(TaskId id) {
    {
        std::scoped_lock lock(impl_->mutex);
        const auto it = impl_->entries.find(id);
        if (it == impl_->entries.end()) return false;
        it->second->removed = true;
        impl_->entries.erase(it);
    }
    impl_->wake();
    return true;
}

bool PeriodicCaller::setPeriod(TaskId id, std::chrono::nanoseconds period) {
    if (period <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("PeriodicCaller::setPeriod: period must be positive");
    }
    {
        std::scoped_lock lock(impl_->mutex);
        const auto it = impl_->entries.find(id);
        if (it == impl_->entries.end()) return false;
        auto& e = *it->second;
        // While executing, `next` is the current tick and gets advanced afterwards.
        if (!e.executing) e.next += period - e.period;
        e.period = period;
    }
    impl_->wake();
    return true;
}

std::optional<TaskStats> PeriodicCaller::stats(TaskId id) const {
    std::scoped_lock lock(impl_->mutex);
    const auto it = impl_->entries.find(id);
    if (it == impl_->entries.end()) return std::nullopt;
    return it->second->stats;
}

void PeriodicCaller::stop() { impl_->stop(); }

bool PeriodicCaller::usesHighResolutionTimer() const noexcept { return impl_->highResolution; }

}  // namespace periodic_caller
