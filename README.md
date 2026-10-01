# PeriodicCaller

C++23 ile yazılmış, Windows için **yüksek hassasiyetli periyodik görev zamanlayıcı**.
Verilen bir lambda veya fonksiyonu belirtilen periyotla, çağıran thread'i bloklamadan
ve süreyi olabildiğince yakın tutturarak çalıştırır. Harici bağımlılık yoktur
(yalnızca Win32 + `winmm`).

## Özellikler

- **Bloklamaz:** Tek bir arka plan thread'i (`std::jthread`) tüm görevleri yönetir; `add()` / `remove()` anında döner.
- **Kaymaz (no drift):** Zamanlar mutlak bir ızgara üzerinde ilerler (`next += period`). Görevin kendi süresi sonraki tetiklemeyi kaydırmaz.
- **Yüksek çözünürlüklü timer:** `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` (Windows 10 1803+) kullanılır; `Sleep()`'in ~15.6 ms'lik adımına takılmaz. Eski sistemlerde klasik timer + `timeBeginPeriod(1)`'e düşer.
- **Opsiyonel hybrid spin:** Deadline'dan `spin_threshold` kadar önce uyanıp kalan süreyi busy-wait ile bekler → mikrosaniye seviyesinde hassasiyet. Varsayılan **kapalı**dır (CPU kullanır).
- **Lambda veya fonksiyon:** Görevler `std::move_only_function<void()>` olarak alınır; move-only capture'lar da desteklenir.
- **Kaçırılan tick politikası:** `Skip` (varsayılan, ızgarada kalır) veya `CatchUp` (kaçırılanları art arda çalıştırır).
- **İstatistik:** Her görev için çalışma sayısı, kaçırılan tick, gecikme (lateness) ortalaması / maksimumu.
- Görev içinden `remove()` (kendini dahil) güvenlidir; görevden fırlayan exception'lar yakalanıp sayılır.

## Kullanım

```cpp
#include <periodic_caller/PeriodicCaller.hpp>
using namespace std::chrono_literals;
namespace pc = periodic_caller;

void heartbeat() { /* ... */ }

int main() {
    pc::PeriodicCaller caller;                       // veya: caller({.spin_threshold = 200us});

    auto a = caller.add(1ms,  [] { /* lambda */ });
    auto b = caller.add(500ms, heartbeat, {.run_immediately = true});
    auto c = caller.add(10ms, [] { /* ... */ },
                        {.missed_tick_policy = pc::MissedTickPolicy::CatchUp});

    caller.set_period(a, 2ms);
    if (auto s = caller.stats(a)) { /* s->avg_lateness, s->max_lateness, ... */ }
    caller.remove(c);
}   // destructor worker'ı durdurur
```

### API özeti

| Fonksiyon | Açıklama |
|---|---|
| `PeriodicCaller(SchedulerOptions = {})` | Worker thread'i başlatır. `spin_threshold`, `high_priority_thread` |
| `TaskId add(period, task, TaskOptions = {})` | Görev ekler. `missed_tick_policy`, `run_immediately` |
| `bool remove(TaskId)` | Görevi kaldırır; o an çalışıyorsa o çağrı tamamlanır |
| `bool set_period(TaskId, period)` | Periyodu değiştirir |
| `std::optional<TaskStats> stats(TaskId)` | Gecikme / sayaç istatistikleri |
| `void stop()` | Worker'ı durdurur (destructor da çağırır) |
| `bool uses_high_resolution_timer()` | Yüksek çözünürlüklü timer kullanılıyor mu |

## Derleme

Gereksinimler: Windows 10+, CMake 3.20+, C++23 derleyici (Visual Studio 2022 17.6+ veya MinGW-w64 GCC 13+).

```bat
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\Release\periodic_caller_example.exe
```

Başka bir CMake projesinde:

```cmake
add_subdirectory(PeriodicCaller)
target_link_libraries(my_app PRIVATE periodic_caller::periodic_caller)
```

## Hassasiyet notları

- Görevler tek bir worker thread'inde sırayla çalışır; uzun süren bir görev diğerlerini geciktirir. Görevleri kısa tutun, ağır işleri başka bir thread'e devredin.
- Yüksek çözünürlüklü timer ile tipik gecikme ~0.5 ms civarındadır. Daha iyisi için `spin_threshold` (örn. 200–300 µs) açın.
- Worker varsayılan olarak `THREAD_PRIORITY_TIME_CRITICAL` önceliğinde çalışır (`high_priority_thread = false` ile kapatılabilir).
- Görev seçimi O(n)'dir; onlarca görev için idealdir.
- Bir görevin içinden `PeriodicCaller` nesnesini **yok etmeyin** (`stop()` çağırmak güvenlidir).
