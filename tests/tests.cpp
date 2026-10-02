#include <periodic_caller/PeriodicCaller.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
namespace pc = periodic_caller;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

// std::this_thread::sleep_for is bound to the ~15.6 ms Windows system tick, so
// a "6 ms" sleep can take 15 ms. Task bodies busy-wait to get exact durations.
static void busy_for(std::chrono::nanoseconds d) {
    const auto end = pc::Clock::now() + d;
    while (pc::Clock::now() < end) {
    }
}

// Number of whole periods between `start` and now: the exact tick count a
// drift-free schedule must have reached (sleep_for may oversleep the window).
static int ticks_since(pc::Clock::time_point start, std::chrono::nanoseconds period) {
    return static_cast<int>((pc::Clock::now() - start) / period);
}

static void test_runs_at_period() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    const auto start = pc::Clock::now();
    const auto id = caller.add(5ms, [&] { ++n; });
    std::this_thread::sleep_for(500ms);
    caller.stop();
    const int expected = ticks_since(start, 5ms);
    // Never more than the grid allows; generous lower bound for shared CI runners.
    CHECK(n <= expected + 1 && n >= expected * 8 / 10);
    CHECK(caller.stats(id)->runs == static_cast<std::uint64_t>(n.load()));
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
    const int expected = ticks_since(start, 10ms);
    CHECK(n <= expected + 1 && n >= expected * 9 / 10);
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
    const int after_remove = a;
    std::this_thread::sleep_for(50ms);
    CHECK(a == after_remove);
    CHECK(b == 3);
    CHECK(!caller.stats(id_b).has_value());
}

static void test_run_immediately_and_move_only() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    auto owned = std::make_unique<int>(7);  // move-only capture
    caller.add(1h, [&n, p = std::move(owned)] { n += *p; }, {.run_immediately = true});
    std::this_thread::sleep_for(50ms);
    CHECK(n == 7);
}

static void test_skip_policy() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    const auto id = caller.add(5ms, [&] {
        if (++n == 1) std::this_thread::sleep_for(52ms);
    });
    std::this_thread::sleep_for(200ms);
    caller.stop();
    CHECK(caller.stats(id)->missed_ticks >= 8);
}

static void test_exceptions_are_contained() {
    pc::PeriodicCaller caller;
    const auto id = caller.add(2ms, [] { throw std::runtime_error("boom"); });
    std::this_thread::sleep_for(30ms);
    caller.stop();
    const auto s = caller.stats(id);
    CHECK(s->exceptions > 0 && s->exceptions == s->runs);
}

static void test_invalid_args() {
    pc::PeriodicCaller caller;
    bool threw = false;
    try {
        caller.add(0ms, [] {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

static void test_set_period() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    const auto id = caller.add(100ms, [&] { ++n; });
    CHECK(caller.set_period(id, 5ms));
    std::this_thread::sleep_for(300ms);
    caller.stop();
    CHECK(n >= 40);
}

static void test_dedicated_does_not_block_inline() {
    pc::PeriodicCaller caller;
    std::atomic<int> fast{0};
    const auto fast_id = caller.add(2ms, [&] { ++fast; });
    caller.add(10ms, [] { busy_for(30ms); },
               {.execution_mode = pc::ExecutionMode::Dedicated});
    std::this_thread::sleep_for(500ms);
    caller.stop();
    // ~250 expected; inline would have managed far fewer behind a 30 ms task.
    CHECK(fast >= 200);
    CHECK(caller.stats(fast_id)->avg_lateness < 5ms);
}

static void test_dedicated_overlap_skip() {
    pc::PeriodicCaller caller;
    const auto id = caller.add(10ms, [] { busy_for(25ms); },
                               {.execution_mode = pc::ExecutionMode::Dedicated});
    std::this_thread::sleep_for(500ms);
    caller.stop();
    const auto s = caller.stats(id);
    // Runs every 3rd tick (0, 30, 60 ms ...) -> ~16 runs, ~2 skips per run.
    CHECK(s->runs >= 10 && s->runs <= 20);
    CHECK(s->overrun_skips >= s->runs);
    CHECK(s->max_duration >= 25ms);
}

static void test_dedicated_coalesce() {
    pc::PeriodicCaller caller;
    const auto id = caller.add(10ms, [] { busy_for(25ms); },
                               {.execution_mode = pc::ExecutionMode::Dedicated,
                                .overlap_policy = pc::OverlapPolicy::Coalesce});
    std::this_thread::sleep_for(500ms);
    caller.stop();
    const auto s = caller.stats(id);
    // Back-to-back runs -> ~20 runs; some ticks still dropped (only one is kept).
    CHECK(s->runs >= 16 && s->runs <= 21);
    CHECK(s->overrun_skips > 0);
}

static void test_dedicated_self_remove() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    pc::TaskId id = 0;
    id = caller.add(2ms, [&] {
        if (++n == 3) caller.remove(id);
    }, {.execution_mode = pc::ExecutionMode::Dedicated});
    std::this_thread::sleep_for(100ms);
    CHECK(n == 3);
    CHECK(!caller.stats(id).has_value());
}

static void test_on_error_handler() {
    std::atomic<int> handled{0};
    pc::TaskId seen = 0;
    pc::PeriodicCaller caller({.on_error = [&](pc::TaskId id, std::exception_ptr e) {
        seen = id;
        try {
            std::rethrow_exception(e);
        } catch (const std::runtime_error&) {
            ++handled;
        }
    }});
    const auto id = caller.add(5ms, [] { throw std::runtime_error("boom"); },
                               {.execution_mode = pc::ExecutionMode::Dedicated});
    std::this_thread::sleep_for(50ms);
    caller.stop();
    CHECK(handled > 0);
    CHECK(seen == id);
}

static void test_destroy_while_dedicated_running() {
    std::atomic<bool> finished{false};
    {
        pc::PeriodicCaller caller;
        caller.add(1ms, [&] {
            std::this_thread::sleep_for(50ms);
            finished = true;
        }, {.execution_mode = pc::ExecutionMode::Dedicated, .run_immediately = true});
        std::this_thread::sleep_for(10ms);
    }  // Must wait for the running task, then exit cleanly.
    CHECK(finished);
}

static void test_add_after_stop_throws() {
    pc::PeriodicCaller caller;
    caller.stop();
    bool threw = false;
    try {
        caller.add(1ms, [] {});
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
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

static void test_remove_and_wait_waits_for_running_call() {
    for (const auto mode : {pc::ExecutionMode::Inline, pc::ExecutionMode::Dedicated}) {
        pc::PeriodicCaller caller;
        std::atomic<bool> started{false}, finished{false};
        std::atomic<int> destroyed{0};
        const auto id = caller.add(1ms, [&, t = DestroyTracker(&destroyed)] {
            started = true;
            busy_for(50ms);
            finished = true;
        }, {.execution_mode = mode, .run_immediately = true});
        while (!started) std::this_thread::yield();
        CHECK(caller.remove_and_wait(id));
        CHECK(finished);        // Returned only after the running call ended,
        CHECK(destroyed == 1);  // and after the lambda was destroyed.
        CHECK(!caller.remove_and_wait(id));
    }
}

static void test_remove_destroys_idle_task_immediately() {
    pc::PeriodicCaller caller;
    std::atomic<int> destroyed{0};
    const auto id = caller.add(1h, [t = DestroyTracker(&destroyed)] {},
                               {.execution_mode = pc::ExecutionMode::Dedicated});
    CHECK(caller.remove(id));
    CHECK(destroyed == 1);
}

static void test_remove_and_wait_on_self() {
    for (const auto mode : {pc::ExecutionMode::Inline, pc::ExecutionMode::Dedicated}) {
        pc::PeriodicCaller caller;
        std::atomic<int> n{0};
        std::atomic<int> destroyed{0};
        pc::TaskId id = 0;
        id = caller.add(2ms, [&, t = DestroyTracker(&destroyed)] {
            if (++n == 2) caller.remove_and_wait(id);  // Must not deadlock.
        }, {.execution_mode = mode});
        std::this_thread::sleep_for(60ms);
        CHECK(n == 2);
        CHECK(destroyed == 1);  // Destroyed by the runner right after the call.
    }
}

static void test_remove_and_wait_after_remove() {
    // A task removed with remove() while running is still waited for.
    pc::PeriodicCaller caller;
    std::atomic<bool> started{false}, finished{false};
    const auto id = caller.add(1ms, [&] {
        started = true;
        busy_for(40ms);
        finished = true;
    }, {.execution_mode = pc::ExecutionMode::Dedicated, .run_immediately = true});
    while (!started) std::this_thread::yield();
    CHECK(caller.remove(id));
    caller.remove_and_wait(id);
    CHECK(finished);
}

static void test_task_scope() {
    pc::PeriodicCaller caller;
    std::atomic<int> calls{0};
    std::atomic<int> destroyed{0};
    {
        pc::TaskScope scope(caller);
        scope.add(1ms, [&, t = DestroyTracker(&destroyed)] { ++calls; });
        const auto b = scope.add(2ms, [&, t = DestroyTracker(&destroyed)] { ++calls; busy_for(1ms); },
                                 {.execution_mode = pc::ExecutionMode::Dedicated});
        scope.add(5ms, [&, t = DestroyTracker(&destroyed)] { ++calls; });
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

int main() {
    test_runs_at_period();
    test_no_drift();
    test_remove_and_self_remove();
    test_run_immediately_and_move_only();
    test_skip_policy();
    test_exceptions_are_contained();
    test_invalid_args();
    test_set_period();
    test_dedicated_does_not_block_inline();
    test_dedicated_overlap_skip();
    test_dedicated_coalesce();
    test_dedicated_self_remove();
    test_on_error_handler();
    test_destroy_while_dedicated_running();
    test_add_after_stop_throws();
    test_remove_and_wait_waits_for_running_call();
    test_remove_destroys_idle_task_immediately();
    test_remove_and_wait_on_self();
    test_remove_and_wait_after_remove();
    test_task_scope();
    test_shared_instance();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("all tests passed");
}
