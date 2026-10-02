// A minimal "plugin" DLL: registers tasks on the process-wide scheduler that
// lives in periodic_caller.dll, and removes them in plugin_stop() so that the
// host can FreeLibrary() it safely.
#include <periodic_caller/PeriodicCaller.hpp>

#include <atomic>
#include <chrono>
#include <utility>

using namespace std::chrono_literals;
namespace pc = periodic_caller;

namespace {

// Counts destructions of lambda captures: proves the task objects (whose
// destructor code lives in this DLL) are gone before plugin_stop() returns.
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

// Not a global object: its destructor would run inside DllMain (loader lock).
pc::TaskScope* scope = nullptr;

}  // namespace

extern "C" __declspec(dllexport) void* plugin_scheduler() { return &pc::PeriodicCaller::shared(); }

extern "C" __declspec(dllexport) void plugin_start(std::atomic<int>* calls, std::atomic<int>* destroyed) {
    scope = new pc::TaskScope();  // Uses PeriodicCaller::shared().
    scope->add(1ms, [calls, t = DestroyTracker(destroyed)] { ++*calls; });
    scope->add(5ms,
               [calls, t = DestroyTracker(destroyed)] {
                   const auto end = pc::Clock::now() + 3ms;
                   while (pc::Clock::now() < end) {
                   }
                   ++*calls;
               },
               {.execution_mode = pc::ExecutionMode::Dedicated});
}

extern "C" __declspec(dllexport) void plugin_stop() {
    delete std::exchange(scope, nullptr);  // Removes the tasks and waits for them.
}
