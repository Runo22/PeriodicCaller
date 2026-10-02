// Loads tests/test_plugin.cpp as a DLL, runs its tasks on the shared scheduler,
// stops and unloads it while the scheduler keeps running.
#include <periodic_caller/PeriodicCaller.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

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

using SchedulerFn = void* (*)();
using StartFn = void (*)(std::atomic<int>*, std::atomic<int>*);
using StopFn = void (*)();

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugin.dll>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // A host task keeps running on the shared scheduler the whole time.
    std::atomic<int> host_calls{0};
    const auto host_task = pc::PeriodicCaller::shared().add(2ms, [&] { ++host_calls; });

    for (int round = 0; round < 3; ++round) {  // Load/unload repeatedly.
        const HMODULE plugin = LoadLibraryA(argv[1]);
        CHECK(plugin != nullptr);
        if (!plugin) break;
        const auto scheduler = std::bit_cast<SchedulerFn>(GetProcAddress(plugin, "plugin_scheduler"));
        const auto start = std::bit_cast<StartFn>(GetProcAddress(plugin, "plugin_start"));
        const auto stop = std::bit_cast<StopFn>(GetProcAddress(plugin, "plugin_stop"));
        CHECK(scheduler && start && stop);
        if (!scheduler || !start || !stop) break;

        // The plugin sees the very same instance as the host.
        CHECK(scheduler() == &pc::PeriodicCaller::shared());

        std::atomic<int> calls{0};
        std::atomic<int> destroyed{0};
        start(&calls, &destroyed);
        std::this_thread::sleep_for(100ms);
        CHECK(calls > 20);

        stop();
        // Both task objects were destroyed before plugin_stop() returned.
        CHECK(destroyed == 2);
        const int calls_at_stop = calls;

        CHECK(FreeLibrary(plugin));
        std::this_thread::sleep_for(50ms);
        CHECK(calls == calls_at_stop);  // Nothing ran after stop (or we'd have crashed).
    }

    const int before = host_calls;
    std::this_thread::sleep_for(50ms);
    CHECK(host_calls > before);  // Shared scheduler unaffected by the unloads.

    CHECK(pc::PeriodicCaller::shared().remove_and_wait(host_task));
    pc::PeriodicCaller::shutdown_shared();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("plugin tests passed");
}
