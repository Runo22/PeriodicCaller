#include "periodic_caller/PeriodicCaller.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>

// Lets the .cpp be dropped into a plain Visual Studio project without extra linker setup.
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace periodic_caller {

namespace {

using std::chrono::nanoseconds;

// A timer that fires this much before the tick is accepted and the remainder
// spun even with max_spin = 0, instead of re-arming the timer (which would
// overshoot by a whole timer latency). Covers rounding between QPC and the
// timer's clock.
constexpr nanoseconds early_wake_grace{20'000};

// Latency samples above this are clamped so that one hiccup (a DPC storm, a
// debugger break) cannot inflate the learned wake-up lead.
constexpr double max_latency_sample_ns = 2'000'000.0;

thread_local const TickInfo* t_current_tick = nullptr;
thread_local bool t_is_task_thread = false;  // For the whole life of a task thread.

struct UniqueHandle {
    HANDLE h = nullptr;
    UniqueHandle() = default;
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    ~UniqueHandle() {
        if (h) CloseHandle(h);
    }
};

// Resolved at runtime so the library still runs on Windows versions that lack
// the newer APIs.
template <class Fn>
Fn load_function(const wchar_t* module, const char* name) {
    HMODULE m = GetModuleHandleW(module);
    if (!m) m = LoadLibraryExW(module, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? std::bit_cast<Fn>(GetProcAddress(m, name)) : nullptr;
}

HANDLE create_timer(bool high_resolution) {
    return CreateWaitableTimerExW(nullptr, nullptr,
                                  high_resolution ? CREATE_WAITABLE_TIMER_HIGH_RESOLUTION : 0,
                                  TIMER_ALL_ACCESS);
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

// CPU set ids of the performance cores of a hybrid CPU. Empty when all cores
// are alike, when the APIs are missing, or when the process affinity has been
// restricted (we do not override the application's own choice).
const std::vector<ULONG>& performance_cpu_sets() {
    static const std::vector<ULONG> ids = [] {
        std::vector<ULONG> result;
        using GetFn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
        const auto get = load_function<GetFn>(L"kernel32.dll", "GetSystemCpuSetInformation");
        if (!get) return result;

        DWORD_PTR process_mask = 0, system_mask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) ||
            process_mask != system_mask) {
            return result;
        }

        ULONG length = 0;
        get(nullptr, 0, &length, GetCurrentProcess(), 0);
        if (length == 0) return result;
        std::vector<std::byte> buffer(length);
        auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data());
        if (!get(info, length, &length, GetCurrentProcess(), 0)) return result;

        BYTE best = 0, worst = 0xff;
        for (ULONG offset = 0; offset < length;) {
            const auto* e = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
            if (e->Size == 0) break;
            if (e->Type == CpuSetInformation) {
                best = std::max(best, e->CpuSet.EfficiencyClass);
                worst = std::min(worst, e->CpuSet.EfficiencyClass);
            }
            offset += e->Size;
        }
        if (best == worst) return result;  // Not a hybrid CPU.

        for (ULONG offset = 0; offset < length;) {
            const auto* e = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
            if (e->Size == 0) break;
            if (e->Type == CpuSetInformation && e->CpuSet.EfficiencyClass == best) {
                result.push_back(e->CpuSet.Id);
            }
            offset += e->Size;
        }
        return result;
    }();
    return ids;
}

int to_win32_priority(ThreadPriority p) {
    switch (p) {
        case ThreadPriority::Normal: return THREAD_PRIORITY_NORMAL;
        case ThreadPriority::AboveNormal: return THREAD_PRIORITY_ABOVE_NORMAL;
        case ThreadPriority::Highest: return THREAD_PRIORITY_HIGHEST;
        case ThreadPriority::TimeCritical: return THREAD_PRIORITY_TIME_CRITICAL;
    }
    return THREAD_PRIORITY_NORMAL;
}

// Applies the timing-related settings to the calling thread for its lifetime.
class ThreadTuning {
public:
    ThreadTuning(const SchedulerOptions& options, const wchar_t* name) {
        const HANDLE self = GetCurrentThread();

        static const auto set_description =
            load_function<HRESULT(WINAPI*)(HANDLE, PCWSTR)>(L"kernel32.dll", "SetThreadDescription");
        if (set_description) set_description(self, name);

        if (options.disable_power_throttling) {
            // THREAD_POWER_THROTTLING_STATE: explicitly "not throttled" (HighQoS).
            static const auto set_information = load_function<BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD)>(
                L"kernel32.dll", "SetThreadInformation");
            if (set_information) {
                struct {
                    ULONG version, control_mask, state_mask;
                } state{1, 0x1 /* EXECUTION_SPEED */, 0};
                constexpr int thread_power_throttling = 3;
                set_information(self, thread_power_throttling, &state, static_cast<DWORD>(sizeof(state)));
            }
        }

        if (options.prefer_performance_cores) {
            const auto& cpu_sets = performance_cpu_sets();
            static const auto set_cpu_sets = load_function<BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG)>(
                L"kernel32.dll", "SetThreadSelectedCpuSets");
            if (set_cpu_sets && !cpu_sets.empty()) {
                set_cpu_sets(self, cpu_sets.data(), static_cast<ULONG>(cpu_sets.size()));
            }
        }

        SetThreadPriority(self, to_win32_priority(options.priority));

        if (options.use_mmcss) {
            static const auto av_set = load_function<HANDLE(WINAPI*)(LPCWSTR, LPDWORD)>(
                L"avrt.dll", "AvSetMmThreadCharacteristicsW");
            DWORD task_index = 0;
            if (av_set) _mmcss = av_set(L"Pro Audio", &task_index);
        }
    }

    ~ThreadTuning() {
        if (!_mmcss) return;
        static const auto av_revert =
            load_function<BOOL(WINAPI*)(HANDLE)>(L"avrt.dll", "AvRevertMmThreadCharacteristics");
        if (av_revert) av_revert(_mmcss);
    }

    ThreadTuning(const ThreadTuning&) = delete;
    ThreadTuning& operator=(const ThreadTuning&) = delete;

private:
    HANDLE _mmcss = nullptr;
};

// Learns how late the OS timer fires (exponentially weighted mean and mean
// absolute deviation) and derives how early to arm it.
class LeadEstimator {
public:
    void add(nanoseconds latency) {
        const double x = std::clamp(static_cast<double>(latency.count()), 0.0, max_latency_sample_ns);
        const double a = _samples < 32 ? 1.0 / ++_samples : 1.0 / 32;
        _mean += a * (x - _mean);
        _dev += a * (std::abs(x - _mean) - _dev);
    }

    // mean + 2 * deviation covers the bulk of the wake-ups; clamped to the spin budget.
    nanoseconds lead(nanoseconds max) const {
        if (max <= nanoseconds::zero()) return nanoseconds::zero();
        const auto v = nanoseconds(std::llround(_mean + 2.0 * _dev));
        return std::clamp(v, nanoseconds::zero(), max);
    }

private:
    double _mean = 0.0;
    double _dev = 0.0;
    std::uint32_t _samples = 0;
};

// Largest n with origin + period.offset(n) <= now (now >= origin).
std::uint64_t latest_due(const Period& period, Clock::time_point origin, Clock::time_point now) {
    const auto elapsed = std::chrono::duration_cast<nanoseconds>(now - origin);
    if (elapsed <= nanoseconds::zero()) return 0;
    auto n = static_cast<std::uint64_t>(static_cast<double>(elapsed.count()) *
                                        static_cast<double>(period.den()) /
                                        static_cast<double>(period.num()));
    while (n > 0 && period.offset(n) > elapsed) --n;
    while (period.offset(n + 1) <= elapsed) ++n;
    return n;
}

}  // namespace

const TickInfo* current_tick() noexcept { return t_current_tick; }

namespace detail {

// One task: its thread, timer and schedule. Fields marked [worker] are only
// touched by the task thread once it runs, so the per-tick path needs no
// lock that another thread could hold for long.
struct Worker {
    Worker(const SchedulerOptions& opts, Task t, TaskOptions o, Period p)
        : options(opts), task(std::move(t)), task_options(std::move(o)), period(p) {}

    ~Worker() {
        // Normally joined by its owner; never leave a joinable std::thread behind.
        if (thread.joinable()) {
            if (thread.get_id() == std::this_thread::get_id()) {
                thread.detach();
            } else {
                thread.join();
            }
        }
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    const SchedulerOptions& options;
    TaskId id = 0;
    Task task;  // [worker] Destroyed by the worker when it exits.
    TaskOptions task_options;
    std::wstring name;
    UniqueHandle timer;
    UniqueHandle control_event;  // Auto-reset: stop or a pending control change.

    // [worker] Schedule: tick n is at origin + period.offset(n).
    Period period;
    Clock::time_point origin;
    std::uint64_t next = 0;
    std::int64_t index_bias = 0;  // Frame number = next + index_bias.

    // Control requests from other threads. The worker only ever try_locks.
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> control_pending{false};
    std::mutex control_mutex;
    std::optional<Period> requested_period;

    // Published stats. The worker only try_locks per tick (blocking only once,
    // for the final snapshot), so a reader can never hold it up.
    mutable std::mutex stats_mutex;
    TaskStats published;

    std::atomic<bool> finished{false};  // Task destroyed, thread about to exit.
    std::thread thread;                 // Moved out by whoever joins it.
    std::thread::id thread_id;

    void request_stop() {
        stop_requested.store(true, std::memory_order_release);
        SetEvent(control_event.h);
    }

    void run();

private:
    // [worker] Working copy of the stats plus running sums.
    TaskStats _stats;
    std::int64_t _lateness_sum = 0;
    std::int64_t _duration_sum = 0;
    std::int64_t _timer_latency_sum = 0;
    std::uint64_t _timer_samples = 0;
    std::int64_t _spin_sum = 0;
    LeadEstimator _lead;

    Clock::time_point tick_time(std::uint64_t n) const { return origin + period.offset(n); }
    void apply_control();
    bool wait_until(Clock::time_point tick);
    void skip_if_behind(Clock::time_point now);
    void run_tick();
    void publish(bool final);
};

void Worker::run() {
    t_is_task_thread = true;
    {
        ThreadTuning tuning(options, name.c_str());
        while (!stop_requested.load(std::memory_order_acquire)) {
            apply_control();
            if (!wait_until(tick_time(next))) continue;  // Woken for a control request.
            if (stop_requested.load(std::memory_order_acquire)) break;
            skip_if_behind(Clock::now());
            run_tick();
        }
        task = nullptr;  // Destroy the lambda and its captures on this thread.
        publish(true);
    }
    finished.store(true, std::memory_order_release);
    finished.notify_all();
}

void Worker::apply_control() {
    if (!control_pending.load(std::memory_order_acquire)) return;
    std::unique_lock lock(control_mutex, std::try_to_lock);
    if (!lock) return;  // The writer signals the event again after unlocking.
    control_pending.store(false, std::memory_order_relaxed);
    if (const auto p = std::exchange(requested_period, std::nullopt)) {
        // Re-base the grid on the last tick (or the virtual one before the first).
        const auto last = next > 0 ? tick_time(next - 1) : origin - period.offset(1);
        index_bias += static_cast<std::int64_t>(next) - 1;
        origin = last;
        next = 1;
        period = *p;
    }
}

// Sleeps until `tick`: arms the timer early by the learned latency and spins
// the remainder (bounded by max_spin). Returns false if woken by control_event.
bool Worker::wait_until(Clock::time_point tick) {
    const nanoseconds max_spin = options.max_spin;
    nanoseconds lead = _lead.lead(max_spin);
    for (;;) {
        auto now = Clock::now();
        const auto target = tick - lead;
        if (target > now) {
            const auto wait = std::chrono::duration_cast<nanoseconds>(target - now);
            LARGE_INTEGER due;
            due.QuadPart = -std::max<LONGLONG>((wait.count() + 99) / 100, 1);  // 100 ns units, relative.
            SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE);
            const HANDLE handles[2] = {control_event.h, timer.h};
            const DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0 + 1) {
                CancelWaitableTimer(timer.h);
                return false;
            }
            now = Clock::now();
            const auto latency = std::chrono::duration_cast<nanoseconds>(now - target);
            _lead.add(latency);
            _timer_latency_sum += latency.count();
            ++_timer_samples;
            _stats.max_timer_latency = std::max(_stats.max_timer_latency, latency);
            _stats.avg_timer_latency = nanoseconds(_timer_latency_sum / static_cast<std::int64_t>(_timer_samples));
            _stats.wake_lead = _lead.lead(max_spin);
        }
        if (now >= tick) return true;
        if (tick - now <= std::max(max_spin, early_wake_grace)) {
            while (Clock::now() < tick) YieldProcessor();
            _spin_sum += std::chrono::duration_cast<nanoseconds>(Clock::now() - now).count();
            _stats.total_spin = nanoseconds(_spin_sum);
            return true;
        }
        lead = nanoseconds::zero();  // Woke far too early (rare): sleep again.
    }
}

void Worker::skip_if_behind(Clock::time_point now) {
    if (tick_time(next + 1) > now) return;  // Only the current tick is due.
    const auto latest = latest_due(period, origin, now);
    const auto overdue = latest - next + 1;
    std::uint64_t drop = 0;
    if (task_options.missed_tick_policy == MissedTickPolicy::Skip) {
        drop = overdue - 1;
    } else if (task_options.max_catch_up != 0 && overdue > task_options.max_catch_up) {
        drop = overdue - task_options.max_catch_up;
    }
    next += drop;
    _stats.skipped_ticks += drop;
}

void Worker::run_tick() {
    TickInfo info;
    info.id = id;
    info.index = static_cast<std::uint64_t>(static_cast<std::int64_t>(next) + index_bias);
    info.scheduled = tick_time(next);
    info.deadline = task_options.deadline ? info.scheduled + *task_options.deadline : tick_time(next + 1);

    std::exception_ptr error;
    t_current_tick = &info;
    const auto start = Clock::now();
    try {
        task();
    } catch (...) {
        error = std::current_exception();
    }
    const auto end = Clock::now();
    t_current_tick = nullptr;

    const auto lateness = std::chrono::duration_cast<nanoseconds>(start - info.scheduled);
    const auto duration = std::chrono::duration_cast<nanoseconds>(end - start);
    auto& s = _stats;
    ++s.runs;
    _lateness_sum += lateness.count();
    _duration_sum += duration.count();
    s.last_lateness = lateness;
    s.avg_lateness = nanoseconds(_lateness_sum / static_cast<std::int64_t>(s.runs));
    s.max_lateness = std::max(s.max_lateness, lateness);
    s.last_duration = duration;
    s.avg_duration = nanoseconds(_duration_sum / static_cast<std::int64_t>(s.runs));
    s.max_duration = std::max(s.max_duration, duration);
    ++s.lateness_histogram[TaskStats::histogram_bucket(lateness)];

    if (error) {
        ++s.exceptions;
        if (options.on_error) {
            try {
                options.on_error(id, error);
            } catch (...) {
            }
        }
    }
    const bool missed = end > info.deadline;
    if (missed) ++s.deadline_misses;
    ++next;
    publish(false);

    if (missed && options.on_deadline_miss) {
        try {
            options.on_deadline_miss(DeadlineMiss{info, start, end});
        } catch (...) {
        }
    }
}

void Worker::publish(bool final) {
    std::unique_lock lock(stats_mutex, std::defer_lock);
    if (final) {
        lock.lock();
    } else if (!lock.try_lock()) {
        return;  // A reader is copying right now; publish on the next tick.
    }
    published = _stats;
}

}  // namespace detail

using detail::Worker;

struct PeriodicCaller::Impl {
    SchedulerOptions options;
    Clock::time_point epoch = Clock::now();
    bool high_resolution = false;
    bool time_period_raised = false;

    // Guards the maps and flags below. Never taken by a task thread on its own
    // (only by user calls such as remove() made from inside a task).
    mutable std::mutex mutex;
    std::unordered_map<TaskId, std::shared_ptr<Worker>> tasks;  // Kept after stop() for stats.
    std::vector<std::shared_ptr<Worker>> retired;               // Removed, not joined yet.
    TaskId next_id = 1;
    bool stopped = false;

    explicit Impl(SchedulerOptions opts) : options(std::move(opts)) {
        if (options.max_spin < std::chrono::microseconds::zero()) {
            throw std::invalid_argument("PeriodicCaller: max_spin must not be negative");
        }
        if (const HANDLE probe = create_timer(true)) {
            high_resolution = true;
            CloseHandle(probe);
        } else {
            // Pre-1803 Windows: classic timer, made precise via a 1 ms system tick.
            time_period_raised = timeBeginPeriod(1) == TIMERR_NOERROR;
        }
    }

    ~Impl() {
        stop();
        if (time_period_raised) timeEndPeriod(1);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    TaskId add(Period period, Task task, TaskOptions task_options) {
        const auto approx = period.approx();
        if (task_options.phase && (*task_options.phase < nanoseconds::zero() || *task_options.phase >= approx)) {
            throw std::invalid_argument("PeriodicCaller::add: phase must be in [0, period)");
        }
        if (task_options.deadline && *task_options.deadline <= nanoseconds::zero()) {
            throw std::invalid_argument("PeriodicCaller::add: deadline must be positive");
        }

        auto w = std::make_shared<Worker>(options, std::move(task), std::move(task_options), period);
        w->timer.h = create_timer(high_resolution);
        w->control_event.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!w->timer.h || !w->control_event.h) {
            throw std::runtime_error("PeriodicCaller::add: failed to create timer/event");
        }

        const auto now = Clock::now();
        if (const auto& phase = w->task_options.phase) {
            w->origin = epoch + *phase;
            if (w->origin < now) {
                const auto n = latest_due(period, w->origin, now);
                w->next = w->origin + period.offset(n) == now ? n : n + 1;
            }
        } else {
            w->origin = now;
            w->next = w->task_options.run_immediately ? 0 : 1;
        }
        w->index_bias = -static_cast<std::int64_t>(w->next);

        TaskId id;
        {
            std::scoped_lock lock(mutex);
            if (stopped) throw std::logic_error("PeriodicCaller::add: scheduler is stopped");
            id = next_id++;
            w->id = id;
            w->name = w->task_options.name.empty() ? L"PeriodicCaller #" + std::to_wstring(id)
                                                   : widen(w->task_options.name);
            Worker& worker = *w;
            const auto it = tasks.emplace(id, std::move(w)).first;
            try {
                worker.thread = std::thread([&worker] { worker.run(); });
            } catch (...) {
                tasks.erase(it);
                throw;
            }
            worker.thread_id = worker.thread.get_id();
        }
        reap_finished();
        return id;
    }

    bool remove(TaskId id) {
        {
            std::scoped_lock lock(mutex);
            const auto it = tasks.find(id);
            if (it == tasks.end()) return false;
            auto w = std::move(it->second);
            tasks.erase(it);
            w->request_stop();
            retired.push_back(std::move(w));
        }
        reap_finished();
        return true;
    }

    bool remove_and_wait(TaskId id) {
        std::shared_ptr<Worker> w;
        std::thread thread;
        bool registered = false;
        {
            std::scoped_lock lock(mutex);
            if (const auto it = tasks.find(id); it != tasks.end()) {
                w = std::move(it->second);
                tasks.erase(it);
                w->request_stop();
                retired.push_back(w);
                registered = true;
            } else {
                const auto r = std::ranges::find(retired, id, [](const auto& p) { return p->id; });
                if (r == retired.end()) return false;
                w = *r;
            }
            // From inside the task itself we would wait for ourselves forever.
            if (w->thread_id == std::this_thread::get_id()) return registered;
            thread = std::move(w->thread);  // Empty if someone else is joining it.
        }
        w->finished.wait(false, std::memory_order_acquire);
        if (thread.joinable()) thread.join();
        {
            std::scoped_lock lock(mutex);
            std::erase(retired, w);
        }
        reap_finished();
        return registered;
    }

    // Joins removed workers that have already finished (a matter of
    // microseconds), never the calling thread itself.
    void reap_finished() {
        std::vector<std::pair<std::shared_ptr<Worker>, std::thread>> done;
        {
            std::scoped_lock lock(mutex);
            for (auto it = retired.begin(); it != retired.end();) {
                auto& w = *it;
                if (w->finished.load(std::memory_order_acquire) && w->thread.joinable() &&
                    w->thread_id != std::this_thread::get_id()) {
                    auto thread = std::move(w->thread);
                    done.emplace_back(std::move(w), std::move(thread));
                    it = retired.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& [w, t] : done) t.join();
    }

    // From a task thread it only requests the stop: joining there could
    // deadlock (two tasks stopping at once would wait for each other). The
    // destructor joins those threads later.
    void stop() {
        std::vector<std::pair<std::shared_ptr<Worker>, std::thread>> to_join;
        {
            std::scoped_lock lock(mutex);
            stopped = true;
            const auto collect = [&](const std::shared_ptr<Worker>& w) {
                w->request_stop();
                if (!t_is_task_thread && w->thread.joinable()) {
                    to_join.emplace_back(w, std::move(w->thread));
                }
            };
            for (auto& [id, w] : tasks) collect(w);
            for (auto& w : retired) collect(w);
        }
        for (auto& [w, t] : to_join) t.join();
        {
            std::scoped_lock lock(mutex);
            // Keep only workers still running (the calling task's own thread).
            std::erase_if(retired, [](const auto& w) { return !w->thread.joinable(); });
        }
    }

    bool set_period(TaskId id, Period period) {
        std::scoped_lock lock(mutex);
        const auto it = tasks.find(id);
        if (it == tasks.end()) return false;
        auto& w = *it->second;
        {
            std::scoped_lock control(w.control_mutex);
            w.requested_period = period;
            w.control_pending.store(true, std::memory_order_release);
        }
        SetEvent(w.control_event.h);
        return true;
    }

    std::optional<TaskStats> stats(TaskId id) const {
        std::shared_ptr<Worker> w;
        {
            std::scoped_lock lock(mutex);
            const auto it = tasks.find(id);
            if (it == tasks.end()) return std::nullopt;
            w = it->second;
        }
        std::scoped_lock lock(w->stats_mutex);
        return w->published;
    }
};

PeriodicCaller::PeriodicCaller(SchedulerOptions options)
    : _impl(std::make_unique<Impl>(std::move(options))) {}

PeriodicCaller::~PeriodicCaller() = default;

TaskId PeriodicCaller::add(Period period, Task task, TaskOptions options) {
    if (!period.positive()) throw std::invalid_argument("PeriodicCaller::add: period must be positive");
    if (!task) throw std::invalid_argument("PeriodicCaller::add: task is empty");
    return _impl->add(period, std::move(task), std::move(options));
}

bool PeriodicCaller::remove(TaskId id) { return _impl->remove(id); }

bool PeriodicCaller::remove_and_wait(TaskId id) { return _impl->remove_and_wait(id); }

bool PeriodicCaller::set_period(TaskId id, Period period) {
    if (!period.positive()) throw std::invalid_argument("PeriodicCaller::set_period: period must be positive");
    return _impl->set_period(id, period);
}

std::optional<TaskStats> PeriodicCaller::stats(TaskId id) const { return _impl->stats(id); }

void PeriodicCaller::stop() { _impl->stop(); }

bool PeriodicCaller::uses_high_resolution_timer() const noexcept { return _impl->high_resolution; }

Clock::time_point PeriodicCaller::epoch() const noexcept { return _impl->epoch; }

namespace {
// Defined in this translation unit, so there is exactly one per module that
// contains PeriodicCaller.cpp: one per process when it lives in a DLL.
std::mutex shared_instance_mutex;
PeriodicCaller* shared_instance = nullptr;  // Leaked on purpose; see header.
}  // namespace

PeriodicCaller& PeriodicCaller::shared(SchedulerOptions options) {
    std::scoped_lock lock(shared_instance_mutex);
    if (!shared_instance) shared_instance = new PeriodicCaller(std::move(options));
    return *shared_instance;
}

void PeriodicCaller::shutdown_shared() {
    PeriodicCaller* instance;
    {
        std::scoped_lock lock(shared_instance_mutex);
        instance = std::exchange(shared_instance, nullptr);
    }
    delete instance;
}

}  // namespace periodic_caller
