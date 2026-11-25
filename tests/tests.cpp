#include <periodic_caller/PeriodicCaller.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
namespace pc = periodic_caller;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

template <class E, class F>
static bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    } catch (...) {
    }
    return false;
}

// std::this_thread::sleep_for is bound to the ~15.6 ms Windows system tick, so
// a "6 ms" sleep can take 15 ms. Task bodies busy-wait to get exact durations.
static void busy_for(std::chrono::nanoseconds d) {
    const auto end = pc::Clock::now() + d;
    while (pc::Clock::now() < end) {
    }
}

// Ticks a drift-free schedule must have reached since `start` (first tick one
// period after start).
static long long ticks_since(pc::Clock::time_point start, pc::Period period) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(pc::Clock::now() - start);
    long long n = 0;
    while (period.offset(static_cast<std::uint64_t>(n + 1)) <= elapsed) ++n;
    return n;
}

static double cpu_seconds() {
    FILETIME creation, exit, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    const auto to_100ns = [](FILETIME f) {
        return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return static_cast<double>(to_100ns(kernel) + to_100ns(user)) / 1e7;
}

// Counts destructions of a lambda capture.
struct DestroyTracker {
    std::atomic<int>* destroyed;
    explicit DestroyTracker(std::atomic<int>* d) : destroyed(d) {}
    DestroyTracker(DestroyTracker&& other) noexcept : destroyed(std::exchange(other.destroyed, nullptr)) {}
    DestroyTracker(const DestroyTracker&) = delete;
    DestroyTracker& operator=(const DestroyTracker&) = delete;
    DestroyTracker& operator=(DestroyTracker&&) = delete;
    ~DestroyTracker() {
        if (destroyed) ++*destroyed;
    }
};

// ---------------------------------------------------------------- Period math

static_assert(pc::hz(60).offset(60) == 1s);
static_assert(pc::hz(60).offset(3) == 50ms);
static_assert(pc::Period(5ms).offset(200) == 1s);
static_assert(pc::Period(std::chrono::duration<long long, std::ratio<1, 60>>{1}) == pc::hz(60));

static void test_period_math() {
    const pc::Period p5(5ms);
    CHECK(p5.num() == 5'000'000 && p5.den() == 1);
    CHECK(p5.approx() == 5ms);

    const auto p60 = pc::hz(60);
    CHECK(p60.num() * 60 == 1'000'000'000LL * p60.den());  // Exactly 1/60 s.
    CHECK(p60.offset(1) == 16'666'666ns);                  // floor
    CHECK(p60.approx() == 16'666'667ns);                   // rounded
    CHECK(p60.offset(6000) == 100s);                       // No drift over many ticks.
    CHECK(p60.offset(60ULL * 3600 * 24 * 365) == std::chrono::hours(24 * 365));

    CHECK(pc::Period(1h).offset(2) == 2h);
    CHECK(pc::Period(250us).offset(4) == 1ms);
    CHECK(pc::hz(1000) == pc::Period(1ms));
    CHECK(pc::Period::from_ratio(10, 4) == pc::Period::from_ratio(5, 2));

    CHECK(throws<std::invalid_argument>([] { (void)pc::hz(0); }));
    CHECK(throws<std::invalid_argument>([] { (void)pc::Period::from_ratio(1, 0); }));
    CHECK(throws<std::invalid_argument>([] { (void)pc::Period(std::chrono::hours(1'000'000'000'000LL)); }));
}

static void test_histogram_helpers() {
    using S = pc::TaskStats;
    CHECK(S::histogram_bucket(-5us) == 0);
    CHECK(S::histogram_bucket(999ns) == 0);
    CHECK(S::histogram_bucket(1us) == 1);
    CHECK(S::histogram_bucket(1999ns) == 1);
    CHECK(S::histogram_bucket(2us) == 2);
    CHECK(S::histogram_bucket(5ms) == 13);  // [4096, 8192) us
    CHECK(S::histogram_bucket(1h) == S::histogram_buckets - 1);

    S s;
    CHECK(s.lateness_percentile(0.99) == 0ns);
    s.lateness_histogram[1] = 90;  // 1-2 us
    s.lateness_histogram[5] = 9;   // 16-32 us
    s.lateness_histogram[10] = 1;  // 512-1024 us
    CHECK(s.lateness_percentile(0.5) == 2us);
    CHECK(s.lateness_percentile(0.99) == 32us);
    CHECK(s.lateness_percentile(1.0) == 1024us);
}

// ------------------------------------------------------------------ Timing

static void test_high_resolution_timer_available() {
    pc::PeriodicCaller caller;
    CHECK(caller.uses_high_resolution_timer());  // Windows 10 1803+ everywhere we run.
}

static void test_runs_at_period() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    const auto start = pc::Clock::now();
    const auto id = caller.add(5ms, [&] { ++n; });
    std::this_thread::sleep_for(500ms);
    caller.stop();
    const auto expected = ticks_since(start, 5ms);
    // Never more than the grid allows; generous lower bound for shared CI runners.
    CHECK(n <= expected + 1 && n >= expected * 8 / 10);
    const auto s = caller.stats(id);
    CHECK(s->runs == static_cast<std::uint64_t>(n.load()));
    CHECK(s->skipped_ticks == 0);
}

static void test_hz_rate_is_exact() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    const auto start = pc::Clock::now();
    caller.add(pc::hz(60), [&] { ++n; });
    std::this_thread::sleep_for(1000ms);
    caller.stop();
    const auto expected = ticks_since(start, pc::hz(60));  // ~60
    CHECK(n <= expected + 1 && n >= expected - 2);         // CatchUp: none lost.
}

static void test_no_drift() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    const auto start = pc::Clock::now();
    // Task takes 60% of its period; an absolute schedule must not drift.
    caller.add(10ms, [&] {
        ++n;
        busy_for(6ms);
    });
    std::this_thread::sleep_for(1000ms);
    caller.stop();
    const auto expected = ticks_since(start, 10ms);
    CHECK(n <= expected + 1 && n >= expected - 3);
}

static void test_tasks_do_not_delay_each_other() {
    pc::PeriodicCaller caller;
    std::atomic<int> fast{0};
    const auto start = pc::Clock::now();
    const auto fast_id = caller.add(2ms, [&] { ++fast; });
    const auto slow_id = caller.add(10ms, [] { busy_for(30ms); });  // Always late.
    std::this_thread::sleep_for(500ms);
    caller.stop();
    const auto expected = ticks_since(start, 2ms);
    CHECK(fast >= expected - 3);  // CatchUp, own thread: nothing lost.
    const auto fs = caller.stats(fast_id);
    CHECK(fs->avg_lateness < 1ms);
    CHECK(fs->deadline_misses <= fs->runs / 10);
    CHECK(caller.stats(slow_id)->deadline_misses == caller.stats(slow_id)->runs);
}

static void test_current_tick_and_frame_index() {
    pc::PeriodicCaller caller;
    CHECK(pc::current_tick() == nullptr);
    std::mutex m;
    std::vector<pc::TickInfo> seen;
    pc::TaskId id = 0;
    id = caller.add(pc::hz(200), [&] {
        const auto* t = pc::current_tick();
        if (!t) return;
        std::scoped_lock lock(m);
        seen.push_back(*t);
    });
    std::this_thread::sleep_for(200ms);
    caller.stop();
    CHECK(seen.size() >= 20);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        CHECK(seen[i].id == id);
        CHECK(seen[i].index == i);
        CHECK(seen[i].deadline - seen[i].scheduled == 5ms);
        if (i > 0) CHECK(seen[i].scheduled - seen[i - 1].scheduled == 5ms);
    }
}

static void test_phase_alignment() {
    pc::PeriodicCaller caller;
    std::atomic<long long> a_offset{-1}, b_offset{-1};
    const auto epoch = caller.epoch();
    const auto record = [&](std::atomic<long long>& out) {
        if (const auto* t = pc::current_tick()) {
            out = std::chrono::duration_cast<std::chrono::nanoseconds>(t->scheduled - epoch).count();
        }
    };
    caller.add(10ms, [&] { record(a_offset); }, {.phase = 0ms});
    caller.add(10ms, [&] { record(b_offset); }, {.phase = 5ms});
    caller.add(pc::hz(60), [] {}, {.phase = 16ms});  // Valid: below the period.
    std::this_thread::sleep_for(100ms);
    caller.stop();
    CHECK(a_offset >= 0 && a_offset % 10'000'000 == 0);
    CHECK(b_offset >= 0 && b_offset % 10'000'000 == 5'000'000);
}

// ------------------------------------------------- Deadlines and late frames

static void test_deadline_miss_default_is_one_period() {
    std::mutex m;
    std::vector<pc::DeadlineMiss> misses;
    pc::PeriodicCaller caller({.on_deadline_miss = [&](const pc::DeadlineMiss& d) {
        std::scoped_lock lock(m);
        misses.push_back(d);
    }});
    std::atomic<int> n{0};
    const auto id = caller.add(5ms, [&] {
        if (++n == 3) busy_for(8ms);  // Only the 3rd frame overruns.
    });
    std::this_thread::sleep_for(200ms);
    caller.stop();
    const auto s = caller.stats(id);
    CHECK(s->deadline_misses >= 1);
    std::scoped_lock lock(m);
    CHECK(misses.size() == s->deadline_misses);
    bool found = false;
    for (const auto& d : misses) {
        CHECK(d.tick.id == id);
        CHECK(d.end > d.tick.deadline);
        CHECK(d.tick.deadline - d.tick.scheduled == 5ms);
        if (d.tick.index == 2) found = true;
    }
    CHECK(found);
}

static void test_custom_deadline() {
    pc::PeriodicCaller caller;
    const auto id = caller.add(10ms, [] { busy_for(2ms); }, {.deadline = 1ms});
    const auto ok = caller.add(10ms, [] { busy_for(2ms); }, {.deadline = 9ms});
    std::this_thread::sleep_for(150ms);
    caller.stop();
    const auto s = caller.stats(id);
    CHECK(s->runs > 5 && s->deadline_misses == s->runs);
    CHECK(caller.stats(ok)->deadline_misses <= 1);
}

static void test_catch_up_runs_every_frame() {
    pc::PeriodicCaller caller;
    std::mutex m;
    std::vector<std::uint64_t> indices;
    const auto start = pc::Clock::now();
    const auto id = caller.add(5ms, [&] {
        const auto index = pc::current_tick()->index;
        {
            std::scoped_lock lock(m);
            indices.push_back(index);
        }
        if (index == 0) busy_for(52ms);  // ~10 frames late.
    });
    std::this_thread::sleep_for(300ms);
    caller.stop();
    const auto expected = ticks_since(start, 5ms);
    const auto s = caller.stats(id);
    CHECK(s->skipped_ticks == 0);
    CHECK(static_cast<long long>(s->runs) >= expected - 3);  // Caught up: nothing lost.
    CHECK(s->deadline_misses >= 8);                           // The late frames.
    std::scoped_lock lock(m);
    for (std::size_t i = 0; i < indices.size(); ++i) CHECK(indices[i] == i);  // No gaps.
}

static void test_skip_policy() {
    pc::PeriodicCaller caller;
    std::mutex m;
    std::vector<std::uint64_t> indices;
    const auto id = caller.add(5ms, [&] {
        const auto index = pc::current_tick()->index;
        {
            std::scoped_lock lock(m);
            indices.push_back(index);
        }
        if (index == 0) busy_for(52ms);
    }, {.missed_tick_policy = pc::MissedTickPolicy::Skip});
    std::this_thread::sleep_for(200ms);
    caller.stop();
    const auto s = caller.stats(id);
    CHECK(s->skipped_ticks >= 8);
    std::scoped_lock lock(m);
    CHECK(indices.size() >= 2 && indices[1] >= 9);  // Jumped to the latest frame.
    for (std::size_t i = 1; i < indices.size(); ++i) CHECK(indices[i] > indices[i - 1]);
}

static void test_max_catch_up() {
    pc::PeriodicCaller caller;
    const auto id = caller.add(5ms, [] {
        if (pc::current_tick()->index == 0) busy_for(52ms);
    }, {.max_catch_up = 2});
    std::this_thread::sleep_for(200ms);
    caller.stop();
    const auto s = caller.stats(id);
    CHECK(s->skipped_ticks >= 7 && s->skipped_ticks <= 10);  // ~10 overdue, 2 kept.
}

// ------------------------------------------------------------------ Errors

static void test_exceptions_are_contained() {
    std::atomic<int> handled{0};
    std::atomic<pc::TaskId> seen{0};
    pc::PeriodicCaller caller({.on_error = [&](pc::TaskId id, std::exception_ptr e) {
        seen = id;
        try {
            std::rethrow_exception(e);
        } catch (const std::runtime_error&) {
            ++handled;
        }
    }});
    const auto id = caller.add(2ms, [] { throw std::runtime_error("boom"); });
    std::this_thread::sleep_for(30ms);
    caller.stop();
    const auto s = caller.stats(id);
    CHECK(s->exceptions > 0 && s->exceptions == s->runs);
    CHECK(handled == static_cast<int>(s->exceptions));
    CHECK(seen == id);
}

static void test_throwing_callbacks_are_contained() {
    pc::PeriodicCaller caller({.on_error = [](pc::TaskId, std::exception_ptr) { throw 1; },
                               .on_deadline_miss = [](const pc::DeadlineMiss&) { throw 2; }});
    const auto id = caller.add(2ms, [] {
        busy_for(3ms);
        throw std::runtime_error("boom");
    });
    std::this_thread::sleep_for(30ms);
    caller.stop();
    CHECK(caller.stats(id)->runs > 2);
}

static void test_invalid_args() {
    pc::PeriodicCaller caller;
    CHECK(throws<std::invalid_argument>([&] { caller.add(0ms, [] {}); }));
    CHECK(throws<std::invalid_argument>([&] { caller.add(-5ms, [] {}); }));
    CHECK(throws<std::invalid_argument>([&] { caller.add(1ms, pc::Task{}); }));
    CHECK(throws<std::invalid_argument>([&] { caller.add(10ms, [] {}, {.phase = 10ms}); }));
    CHECK(throws<std::invalid_argument>([&] { caller.add(10ms, [] {}, {.phase = -1ms}); }));
    CHECK(throws<std::invalid_argument>([&] { caller.add(10ms, [] {}, {.deadline = 0ms}); }));
    const auto id = caller.add(1h, [] {});
    CHECK(throws<std::invalid_argument>([&] { caller.set_period(id, 0ms); }));
    CHECK(!caller.set_period(id + 100, 1ms));
    CHECK(!caller.stats(id + 100).has_value());
    CHECK(throws<std::invalid_argument>([] { pc::PeriodicCaller bad({.max_spin = -1us}); }));
}

// ----------------------------------------------------------- Control calls

static void test_set_period() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    const auto id = caller.add(100ms, [&] { ++n; });
    CHECK(caller.set_period(id, 5ms));  // Takes effect while sleeping.
    std::this_thread::sleep_for(300ms);
    const int at_5ms = n;
    CHECK(at_5ms >= 40);
    CHECK(caller.set_period(id, pc::hz(20)));
    std::this_thread::sleep_for(300ms);
    caller.stop();
    CHECK(n - at_5ms >= 4 && n - at_5ms <= 8);
}

static void test_set_period_keeps_frame_index_and_grid() {
    pc::PeriodicCaller caller;
    std::mutex m;
    std::vector<pc::TickInfo> seen;
    pc::TaskId id = 0;
    id = caller.add(4ms, [&] {
        const auto t = *pc::current_tick();
        {
            std::scoped_lock lock(m);
            seen.push_back(t);
        }
        if (t.index == 4) caller.set_period(id, 8ms);  // From inside the task.
    });
    std::this_thread::sleep_for(150ms);
    caller.stop();
    std::scoped_lock lock(m);
    CHECK(seen.size() >= 10);
    for (std::size_t i = 1; i < seen.size(); ++i) {
        CHECK(seen[i].index == i);
        CHECK(seen[i].scheduled - seen[i - 1].scheduled == (i <= 4 ? 4ms : 8ms));
    }
}

static void test_run_immediately_and_move_only() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    auto owned = std::make_unique<int>(7);  // move-only capture
    caller.add(1h, [&n, p = std::move(owned)] { n += *p; }, {.run_immediately = true});
    std::this_thread::sleep_for(50ms);
    CHECK(n == 7);
}

static void test_remove_and_self_remove() {
    pc::PeriodicCaller caller;
    std::atomic<int> a{0}, b{0};
    const auto id_a = caller.add(2ms, [&] { ++a; });
    pc::TaskId id_b = 0;
    id_b = caller.add(2ms, [&] {
        if (++b == 3) caller.remove(id_b);
    });
    std::this_thread::sleep_for(50ms);
    CHECK(caller.remove(id_a));
    CHECK(!caller.remove(id_a));
    std::this_thread::sleep_for(5ms);  // A call in progress may still complete.
    const int after_remove = a;
    std::this_thread::sleep_for(50ms);
    CHECK(a == after_remove);
    CHECK(b == 3);
    CHECK(!caller.stats(id_b).has_value());
}

static void test_remove_destroys_task_soon() {
    pc::PeriodicCaller caller;
    std::atomic<int> destroyed{0};
    const auto id = caller.add(1h, [t = DestroyTracker(&destroyed)] {});
    CHECK(caller.remove(id));
    for (int i = 0; i < 100 && destroyed == 0; ++i) std::this_thread::sleep_for(1ms);
    CHECK(destroyed == 1);
}

static void test_remove_and_wait_waits_for_running_call() {
    pc::PeriodicCaller caller;
    std::atomic<bool> started{false}, finished{false};
    std::atomic<int> destroyed{0};
    const auto id = caller.add(1ms, [&, t = DestroyTracker(&destroyed)] {
        started = true;
        busy_for(50ms);
        finished = true;
    }, {.run_immediately = true});
    while (!started) std::this_thread::yield();
    CHECK(caller.remove_and_wait(id));
    CHECK(finished);        // Returned only after the running call ended,
    CHECK(destroyed == 1);  // and after the lambda was destroyed.
    CHECK(!caller.remove_and_wait(id));
}

static void test_remove_and_wait_on_self() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    std::atomic<int> destroyed{0};
    pc::TaskId id = 0;
    id = caller.add(2ms, [&, t = DestroyTracker(&destroyed)] {
        if (++n == 2) CHECK(caller.remove_and_wait(id));  // Must not deadlock.
    });
    std::this_thread::sleep_for(60ms);
    CHECK(n == 2);
    CHECK(destroyed == 1);  // Destroyed by its thread right after the call.
}

static void test_remove_and_wait_after_remove() {
    // A task removed with remove() while running is still waited for.
    pc::PeriodicCaller caller;
    std::atomic<bool> started{false}, finished{false};
    const auto id = caller.add(1ms, [&] {
        started = true;
        busy_for(40ms);
        finished = true;
    }, {.run_immediately = true});
    while (!started) std::this_thread::yield();
    CHECK(caller.remove(id));
    caller.remove_and_wait(id);
    CHECK(finished);
}

static void test_concurrent_remove_and_wait() {
    // Two threads waiting for the same task both return after it finished.
    pc::PeriodicCaller caller;
    std::atomic<bool> started{false}, finished{false};
    const auto id = caller.add(1ms, [&] {
        started = true;
        busy_for(40ms);
        finished = true;
    }, {.run_immediately = true});
    while (!started) std::this_thread::yield();
    std::atomic<int> ok{0};
    std::thread other([&] {
        caller.remove_and_wait(id);
        if (finished) ++ok;
    });
    caller.remove_and_wait(id);
    if (finished) ++ok;
    other.join();
    CHECK(ok == 2);
}

// -------------------------------------------------------- Stats and CPU cost

static void test_stats_consistency() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    const auto id = caller.add(1ms, [&] { ++n; });
    std::atomic<bool> done{false};
    std::atomic<int> regressions{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&] {  // Hammer stats(): must never stall the task.
            std::uint64_t last = 0;
            while (!done) {
                const auto s = caller.stats(id);
                if (s->runs < last) ++regressions;
                last = s->runs;
            }
        });
    }
    const auto start = pc::Clock::now();
    std::this_thread::sleep_for(300ms);
    done = true;
    for (auto& t : readers) t.join();
    caller.stop();
    const auto s = caller.stats(id);
    CHECK(regressions == 0);
    CHECK(s->runs == static_cast<std::uint64_t>(n.load()));  // Exact after stop().
    CHECK(static_cast<long long>(s->runs) >= ticks_since(start, 1ms) - 3);
    std::uint64_t histogram_total = 0;
    for (const auto c : s->lateness_histogram) histogram_total += c;
    CHECK(histogram_total == s->runs);
    CHECK(s->max_lateness >= s->avg_lateness && s->avg_lateness >= 0ns);
    CHECK(s->max_duration >= s->avg_duration);
    CHECK(s->max_timer_latency >= s->avg_timer_latency);
}

static void test_spin_budget() {
    for (const auto max_spin : {0us, 30us, 200us}) {
        pc::PeriodicCaller caller({.max_spin = max_spin});
        const auto id = caller.add(5ms, [] {});
        std::this_thread::sleep_for(300ms);
        caller.stop();
        const auto s = caller.stats(id);
        CHECK(s->runs > 40);
        CHECK(s->wake_lead <= max_spin);
        // Per tick at most max(max_spin, 20 us early-wake grace) plus clock-read slack.
        const auto per_tick = std::max<std::chrono::nanoseconds>(max_spin, 20us) + 20us;
        CHECK(s->total_spin <= per_tick * static_cast<long long>(s->runs) + 5ms);  // + one preemption.
        if (max_spin == 0us) CHECK(s->wake_lead == 0ns);
    }
}

static void test_cpu_usage_is_low() {
    // The target workload: two 5 ms tasks and three ~60 Hz tasks.
    for (const auto max_spin : {0us, 50us}) {
        pc::PeriodicCaller caller({.max_spin = max_spin});
        std::atomic<int> n{0};
        caller.add(5ms, [&] { ++n; });
        caller.add(5ms, [&] { ++n; }, {.phase = 2500us});
        caller.add(pc::hz(60), [&] { ++n; });
        caller.add(pc::hz(50), [&] { ++n; });
        caller.add(pc::hz(30), [&] { ++n; });
        std::this_thread::sleep_for(100ms);  // Let the lead estimate settle.
        const double cpu0 = cpu_seconds();
        const auto t0 = pc::Clock::now();
        std::this_thread::sleep_for(2s);
        const double cpu = cpu_seconds() - cpu0;
        const double wall = std::chrono::duration<double>(pc::Clock::now() - t0).count();
        caller.stop();
        const double percent_of_one_core = 100.0 * cpu / wall;
        std::printf("  cpu (max_spin=%lld us): %.2f%% of one core, %d runs\n",
                    static_cast<long long>(max_spin.count()), percent_of_one_core, n.load());
        // ~540 wake-ups/s; a spinning scheduler would use 100%.
        CHECK(percent_of_one_core < (max_spin == 0us ? 5.0 : 10.0));
    }
}

// --------------------------------------------------------------- Lifecycle

static void test_destroy_while_running() {
    std::atomic<bool> finished{false};
    {
        pc::PeriodicCaller caller;
        caller.add(1ms, [&] {
            busy_for(50ms);
            finished = true;
        }, {.run_immediately = true});
        std::this_thread::sleep_for(10ms);
    }  // Must wait for the running task, then exit cleanly.
    CHECK(finished);
}

static void test_stop() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    const auto id = caller.add(2ms, [&] { ++n; });
    std::this_thread::sleep_for(30ms);
    caller.stop();
    caller.stop();  // Idempotent.
    const int at_stop = n;
    std::this_thread::sleep_for(20ms);
    CHECK(n == at_stop);
    CHECK(caller.stats(id)->runs == static_cast<std::uint64_t>(at_stop));  // Still readable.
    CHECK(throws<std::logic_error>([&] { caller.add(1ms, [] {}); }));
    CHECK(caller.remove(id));
    CHECK(!caller.stats(id).has_value());
}

static void test_stop_from_inside_a_task() {
    std::atomic<int> other{0};
    {
        pc::PeriodicCaller caller;
        caller.add(2ms, [&] { ++other; });
        caller.add(2ms, [&] {
            if (other > 5) caller.stop();  // Must not deadlock.
        });
        std::this_thread::sleep_for(60ms);
        const int at = other;
        std::this_thread::sleep_for(20ms);
        CHECK(other == at);  // Everything stopped.
    }
}

static void test_add_remove_stress() {
    pc::PeriodicCaller caller;
    std::atomic<int> calls{0};
    std::atomic<int> destroyed{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 40; ++i) {
                const auto id = caller.add(1ms, [&, d = DestroyTracker(&destroyed)] { ++calls; },
                                           {.run_immediately = (i % 2) == 0});
                if ((i + t) % 3 == 0) {
                    caller.remove(id);
                } else {
                    caller.remove_and_wait(id);
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    caller.stop();
    CHECK(destroyed == 160);
}

// ------------------------------------------------------- Shared / TaskScope

static void test_task_scope() {
    pc::PeriodicCaller caller;
    std::atomic<int> calls{0};
    std::atomic<int> destroyed{0};
    {
        pc::TaskScope scope(caller);
        scope.add(1ms, [&, t = DestroyTracker(&destroyed)] { ++calls; });
        const auto b = scope.add(2ms, [&, t = DestroyTracker(&destroyed)] {
            ++calls;
            busy_for(1ms);
        });
        scope.add(pc::hz(60), [&, t = DestroyTracker(&destroyed)] { ++calls; });
        std::this_thread::sleep_for(30ms);
        CHECK(scope.remove(b));
        CHECK(!scope.remove(b));
        CHECK(destroyed == 1);
    }  // Destructor clears the rest and waits.
    CHECK(destroyed == 3);
    const int at_end = calls;
    std::this_thread::sleep_for(20ms);
    CHECK(calls == at_end);
}

static void test_shared_instance() {
    auto& a = pc::PeriodicCaller::shared();
    auto& b = pc::PeriodicCaller::shared();
    CHECK(&a == &b);
    std::atomic<int> n{0};
    {
        pc::TaskScope scope;  // Defaults to the shared instance.
        scope.add(1ms, [&] { ++n; });
        std::this_thread::sleep_for(20ms);
    }
    CHECK(n > 0);
    pc::PeriodicCaller::shutdown_shared();
    CHECK(pc::PeriodicCaller::shared().add(1h, [] {}) > 0);  // Re-created on demand.
    pc::PeriodicCaller::shutdown_shared();
}

int main(int argc, char** argv) {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"period_math", test_period_math},
        {"histogram_helpers", test_histogram_helpers},
        {"high_resolution_timer_available", test_high_resolution_timer_available},
        {"runs_at_period", test_runs_at_period},
        {"hz_rate_is_exact", test_hz_rate_is_exact},
        {"no_drift", test_no_drift},
        {"tasks_do_not_delay_each_other", test_tasks_do_not_delay_each_other},
        {"current_tick_and_frame_index", test_current_tick_and_frame_index},
        {"phase_alignment", test_phase_alignment},
        {"deadline_miss_default_is_one_period", test_deadline_miss_default_is_one_period},
        {"custom_deadline", test_custom_deadline},
        {"catch_up_runs_every_frame", test_catch_up_runs_every_frame},
        {"skip_policy", test_skip_policy},
        {"max_catch_up", test_max_catch_up},
        {"exceptions_are_contained", test_exceptions_are_contained},
        {"throwing_callbacks_are_contained", test_throwing_callbacks_are_contained},
        {"invalid_args", test_invalid_args},
        {"set_period", test_set_period},
        {"set_period_keeps_frame_index_and_grid", test_set_period_keeps_frame_index_and_grid},
        {"run_immediately_and_move_only", test_run_immediately_and_move_only},
        {"remove_and_self_remove", test_remove_and_self_remove},
        {"remove_destroys_task_soon", test_remove_destroys_task_soon},
        {"remove_and_wait_waits_for_running_call", test_remove_and_wait_waits_for_running_call},
        {"remove_and_wait_on_self", test_remove_and_wait_on_self},
        {"remove_and_wait_after_remove", test_remove_and_wait_after_remove},
        {"concurrent_remove_and_wait", test_concurrent_remove_and_wait},
        {"stats_consistency", test_stats_consistency},
        {"spin_budget", test_spin_budget},
        {"cpu_usage_is_low", test_cpu_usage_is_low},
        {"destroy_while_running", test_destroy_while_running},
        {"stop", test_stop},
        {"stop_from_inside_a_task", test_stop_from_inside_a_task},
        {"add_remove_stress", test_add_remove_stress},
        {"task_scope", test_task_scope},
        {"shared_instance", test_shared_instance},
    };
    const char* filter = argc > 1 ? argv[1] : nullptr;  // Optional: run tests whose name contains it.
    for (const auto& t : tests) {
        if (filter && !std::strstr(t.name, filter)) continue;
        const int before = failures;
        std::printf("%s\n", t.name);
        std::fflush(stdout);
        t.fn();
        if (failures != before) std::fprintf(stderr, "  ^ %s failed\n", t.name);
    }
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("all tests passed");
}
