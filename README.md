# PeriodicCaller

C++23 ile yazılmış, Windows için **yüksek hassasiyetli periyodik görev zamanlayıcı**.
Verilen bir lambda veya fonksiyonu belirtilen periyotla, çağıran thread'i bloklamadan
ve süreyi olabildiğince yakın tutturarak çalıştırır. Harici bağımlılık yoktur
(yalnızca Win32 + `winmm`).

## Özellikler

- **Bloklamaz:** Tek bir scheduler thread'i (`std::jthread`) tüm görevlerin zamanlamasını yönetir; `add()` / `remove()` anında döner.
- **Inline / Dedicated çalışma modu:** Kısa görevler scheduler thread'inde (en hassas), uzun görevler kendi kalıcı worker thread'inde çalışır; uzun görev diğerlerini geciktirmez.
- **Çakışma (overlap) politikası:** Dedicated görev hâlâ çalışırken tick gelirse `Skip` (atla, varsayılan) veya `Coalesce` (bitince bir kez daha çalıştır).
- **Kaymaz (no drift):** Zamanlar mutlak bir ızgara üzerinde ilerler (`next += period`). Görevin kendi süresi sonraki tetiklemeyi kaydırmaz.
- **Yüksek çözünürlüklü timer:** `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` (Windows 10 1803+) kullanılır; `Sleep()`'in ~15.6 ms'lik adımına takılmaz. Eski sistemlerde klasik timer + `timeBeginPeriod(1)`'e düşer.
- **Opsiyonel hybrid spin:** Deadline'dan `spin_threshold` kadar önce uyanıp kalan süreyi busy-wait ile bekler → mikrosaniye seviyesinde hassasiyet. Varsayılan **kapalı**dır (CPU kullanır).
- **Lambda veya fonksiyon:** Görevler `std::move_only_function<void()>` olarak alınır; move-only capture'lar da desteklenir.
- **Kaçırılan tick politikası:** `Skip` (varsayılan, ızgarada kalır) veya `CatchUp` (kaçırılanları art arda çalıştırır).
- **İstatistik:** Çalışma sayısı, kaçırılan / çakışma nedeniyle atlanan tick, gecikme (lateness) ortalaması / maksimumu, görev süresi.
- **Windows ince ayarları:** Thread'ler `TIME_CRITICAL` öncelikte çalışır, EcoQoS (güç kısıtlama) devre dışı bırakılır, debugger'da görünen thread isimleri verilir; opsiyonel MMCSS ("Pro Audio") kaydı.
- Görev içinden `remove()` (kendini dahil) güvenlidir; görevden fırlayan exception'lar yakalanır, sayılır ve opsiyonel `on_error` callback'ine iletilir.

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

    // Uzun süren görev: kendi thread'inde, çakışan tick'ler atlanır.
    auto d = caller.add(20ms, [] { /* ağır iş */ },
                        {.execution_mode = pc::ExecutionMode::Dedicated,
                         .overlap_policy = pc::OverlapPolicy::Skip});

    caller.set_period(a, 2ms);
    if (auto s = caller.stats(a)) { /* s->avg_lateness, s->max_lateness, ... */ }
    caller.remove(c);
}   // destructor worker'ı durdurur
```

### API özeti

| Fonksiyon | Açıklama |
|---|---|
| `PeriodicCaller(SchedulerOptions = {})` | Scheduler'ı başlatır. `spin_threshold`, `high_priority_thread`, `use_mmcss`, `on_error` |
| `TaskId add(period, task, TaskOptions = {})` | Görev ekler. `execution_mode`, `overlap_policy`, `missed_tick_policy`, `run_immediately` |
| `bool remove(TaskId)` | Görevi kaldırır; o an çalışıyorsa o çağrı tamamlanır |
| `bool set_period(TaskId, period)` | Periyodu değiştirir |
| `std::optional<TaskStats> stats(TaskId)` | Gecikme / sayaç istatistikleri |
| `void stop()` | Scheduler'ı ve tüm worker'ları durdurur; istatistikler okunabilir kalır (destructor da çağırır) |
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

## Inline mı, Dedicated mı?

| | Inline (varsayılan) | Dedicated |
|---|---|---|
| Nerede çalışır | Scheduler thread'inde | Görevin kendi kalıcı thread'inde |
| Hassasiyet | En iyi | + birkaç µs thread uyanma gecikmesi |
| Uzun sürerse | Diğer tüm görevleri geciktirir | Sadece kendi tick'lerini etkiler (`OverlapPolicy`) |
| Maliyet | Yok | Görev başına bir thread |

Kural: görev süresi periyodunun küçük bir kısmıysa (`stats().max_duration` ile ölçün) Inline, değilse Dedicated.
Dedicated görevler farklı bir thread'de çalıştığı için paylaşılan verilere erişimde senkronizasyon gerekir.

## Hassasiyet notları

- Yüksek çözünürlüklü timer ile tipik gecikme ~0.5 ms civarındadır. Daha iyisi için `spin_threshold` (örn. 200–300 µs) açın.
- Worker varsayılan olarak `THREAD_PRIORITY_TIME_CRITICAL` önceliğinde çalışır (`high_priority_thread = false` ile kapatılabilir).
- Görev seçimi O(n)'dir; onlarca görev için idealdir.
- Ağır sistem yükü altında `use_mmcss = true` gecikme sıçramalarını azaltır.
- Bir görevin içinden `PeriodicCaller` nesnesini **yok etmeyin** (`stop()` ve `remove()` çağırmak güvenlidir).
- Destructor, o an çalışmakta olan görevlerin bitmesini bekler.
