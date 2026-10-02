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
| `bool remove(TaskId)` | Görevi kaldırır, beklemez; o an çalışıyorsa o çağrı tamamlanır |
| `bool remove_and_wait(TaskId)` | Kaldırır, çalışan çağrının bitmesini ve lambda'nın yok edilmesini bekler |
| `bool set_period(TaskId, period)` | Periyodu değiştirir |
| `std::optional<TaskStats> stats(TaskId)` | Gecikme / sayaç istatistikleri |
| `void stop()` | Scheduler'ı ve tüm worker'ları durdurur; istatistikler okunabilir kalır (destructor da çağırır) |
| `bool uses_high_resolution_timer()` | Yüksek çözünürlüklü timer kullanılıyor mu |
| `static PeriodicCaller& shared(SchedulerOptions = {})` | Süreç genelinde ortak instance (DLL içinde tek) |
| `static void shutdown_shared()` | Ortak instance'ı durdurur ve yok eder |
| `TaskScope(PeriodicCaller& = shared())` | Bir grup task'ı (örn. bir plugin'in) sahiplenir; `clear()`/destructor hepsini kaldırıp bekler |

## Derleme

Gereksinimler: Windows 10 1803+ (daha eskisi de çalışır, hassasiyet düşer), **MSVC (Visual Studio 2022 17.2+ veya Visual Studio 2026)**, CMake 3.21+.
Yalnızca MSVC desteklenir.

```bat
cmake --preset msvc-x64
cmake --build --preset release
ctest --preset release
build\Release\periodic_caller_example.exe
```

Visual Studio'da: **File → Open → Folder** ile klasörü açın; `CMakePresets.json` otomatik tanınır.

CMake kullanmadan mevcut bir `.vcxproj`'a eklemek için:
1. `src/PeriodicCaller.cpp` dosyasını projeye ekleyin, `include/` klasörünü *Additional Include Directories*'e ekleyin.
2. *C++ Language Standard* = **Preview - Features from the Latest C++ Working Draft (`/std:c++latest`)**.
3. `winmm.lib` kod içinden (`#pragma comment`) otomatik linklenir.

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

## Birden fazla DLL (plugin) ile kullanım

Kod hangi modüle derlenirse scheduler o modülde yaşar. Tüm plugin'lerin **tek bir ortak scheduler** kullanması için:

1. `PeriodicCaller.cpp`'yi **tek bir DLL**'e (örn. util DLL'iniz) derleyin ve o projede `PERIODIC_CALLER_EXPORTS` tanımlayın.
   Bu DLL'i kullanan tüm modüllerde (exe, plugin'ler) `PERIODIC_CALLER_IMPORTS` tanımlayın.
   CMake ile: `-DPERIODIC_CALLER_SHARED=ON` bunu otomatik yapar.
   Kendi export makronuz varsa `PERIODIC_CALLER_API`'yi önceden tanımlayabilirsiniz.
2. Herkes `PeriodicCaller::shared()` kullanır; bu, süreçte **tek** instance döndürür.
3. Plugin task'larını bir `TaskScope` ile ekler ve kendi kapanış fonksiyonunda temizler:

```cpp
// plugin.cpp
pc::TaskScope* g_scope = nullptr;   // Global nesne DEĞİL, pointer (aşağıya bakın)

extern "C" __declspec(dllexport) void plugin_start() {
    g_scope = new pc::TaskScope();                 // PeriodicCaller::shared() kullanır
    g_scope->add(1ms, [] { /* ... */ });
    g_scope->add(20ms, [] { /* ağır iş */ }, {.execution_mode = pc::ExecutionMode::Dedicated});
}

extern "C" __declspec(dllexport) void plugin_stop() {
    delete std::exchange(g_scope, nullptr);        // Task'ları kaldırır, çalışanları BEKLER
}
// Host: plugin_stop() -> FreeLibrary(plugin)
```

Kurallar:

- **Unload'dan önce temizlik şart.** Task'ın kodu plugin DLL'inde; plugin `FreeLibrary` ile kalkarken task kayıtlı veya çalışıyorsa süreç çöker.
  `TaskScope::clear()` / destructor'ı ve `remove_and_wait()`, çalışan çağrının bitmesini **ve** lambda'nın yok edilmesini bekler; döndükten sonra unload güvenlidir.
- **DllMain'de / global destructor'da temizlik yapmayın.** `DLL_PROCESS_DETACH` loader lock altında çalışır; orada thread beklemek deadlock'a yol açar.
  Bu yüzden `TaskScope` global nesne değil, `plugin_stop()` içinde silinen bir pointer olmalı.
- `shared()` instance'ı bilerek otomatik yok edilmez (aynı loader lock nedeniyle). Süreç kapanırken bir şey yapmanız gerekmez;
  util DLL'i süreç bitmeden kaldırılacaksa host önce tüm plugin'leri durdurup sonra `PeriodicCaller::shutdown_shared()` çağırmalıdır.
- Tüm modüller aynı Visual Studio sürümü ve aynı `/MD` ayarıyla derlenmelidir (API'de `std::move_only_function` vb. STL tipleri var).
- `remove()` beklemez (task içinden veya hızlı kaldırma için); unload öncesi her zaman `remove_and_wait()` / `TaskScope` kullanın.

## Hassasiyet notları

- Yüksek çözünürlüklü timer ile tipik gecikme ~0.5 ms civarındadır. Daha iyisi için `spin_threshold` (örn. 200–300 µs) açın.
- Worker varsayılan olarak `THREAD_PRIORITY_TIME_CRITICAL` önceliğinde çalışır (`high_priority_thread = false` ile kapatılabilir).
- Görev seçimi O(n)'dir; onlarca görev için idealdir.
- Ağır sistem yükü altında `use_mmcss = true` gecikme sıçramalarını azaltır.
- Bir görevin içinden `PeriodicCaller` nesnesini **yok etmeyin** (`stop()` ve `remove()` çağırmak güvenlidir).
- Destructor, o an çalışmakta olan görevlerin bitmesini bekler.
