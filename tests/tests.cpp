#include <periodic_caller/PeriodicCaller.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <thread>

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

static void test_runs_at_period() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    const auto id = caller.add(5ms, [&] { ++n; });
    std::this_thread::sleep_for(500ms);
    caller.stop();
    // ~100 expected; generous bounds for shared CI runners.
    CHECK(n >= 80 && n <= 102);
    CHECK(caller.stats(id)->runs == static_cast<std::uint64_t>(n.load()));
}

static void test_no_drift() {
    pc::PeriodicCaller caller;
    std::atomic<int> n{0};
    // Task takes 60% of its period; an absolute schedule must not drift.
    caller.add(10ms, [&] {
        ++n;
        std::this_thread::sleep_for(6ms);
    });
    std::this_thread::sleep_for(1000ms);
    caller.stop();
    CHECK(n >= 90 && n <= 101);
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

int main() {
    test_runs_at_period();
    test_no_drift();
    test_remove_and_self_remove();
    test_run_immediately_and_move_only();
    test_skip_policy();
    test_exceptions_are_contained();
    test_invalid_args();
    test_set_period();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("all tests passed");
}
