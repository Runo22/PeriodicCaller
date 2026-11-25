# PeriodicCaller

C++23 ile yazılmış, Windows için **hassas ve düşük CPU'lu periyodik görev zamanlayıcı**.
Birkaç adet frame-kritik görevi (örn. 5 ms ve 60 Hz) belirtilen periyotla, frame kaçırmadan ve
CPU'yu kilitlemeden çalıştırır. Harici bağımlılık yoktur (yalnızca Win32 + `winmm`).

## Nasıl çalışır

- **Her görev kendi thread'inde, kendi yüksek çözünürlüklü timer'ıyla çalışır.** Görevler birbirini geciktiremez;
  ortak bir scheduler kilidi yoktur. Görev thread'inin tick başına izlediği yol, başka bir thread'in tutabileceği hiçbir
  kilidi beklemez (öncelik terslenmesi / priority inversion yok). `stats()` okumak görevi asla bekletmez.
- **Kaymaz, tam periyot:** tick *n* = `origin + n * period`. Periyot kesirli tutulur: `hz(60)` tam olarak 1/60 s'dir,
  saatte de yılda da tam 60 Hz kalır.
- **Adaptif erken uyanma:** her thread, Windows timer'ının o makinede tipik olarak ne kadar geç uyandığını öğrenir,
  timer'ı o kadar erken kurar ve kalan birkaç µs'yi busy-wait ile bekler. Busy-wait tick başına **en fazla
  `max_spin`** (varsayılan 50 µs) sürer; böylece CPU kullanımı neredeyse sıfır kalır.
- **Frame kaçırmaz (varsayılan):** geciken bir frame ilk fırsatta çalışır, ardından birikmiş frame'ler art arda
  çalıştırılır (`MissedTickPolicy::CatchUp`). Görev **bir sonraki tick'ten önce bitmezse** (deadline = 1 periyot)
  bu bir *deadline miss* olarak sayılır ve `on_deadline_miss`'e bildirilir.
- **Windows ince ayarları:** `TIME_CRITICAL` öncelik, EcoQoS/güç kısıtlaması kapalı, hibrit CPU'larda (Intel 12. nesil+)
  P-çekirdek tercihi, debugger'da görünen thread isimleri, opsiyonel MMCSS.
- Görev içinden `remove()` / `set_period()` / `stop()` güvenlidir; exception'lar yakalanır, sayılır ve `on_error`'a iletilir.

## Kullanım

```cpp
#include <periodic_caller/PeriodicCaller.hpp>
using namespace std::chrono_literals;
namespace pc = periodic_caller;

pc::PeriodicCaller caller({
    .on_deadline_miss = [](const pc::DeadlineMiss& d) { /* log: d.tick.index, d.end - d.tick.deadline */ },
});

auto control = caller.add(5ms, [] { /* her 5 ms */ }, {.name = "control"});
auto video   = caller.add(pc::hz(60), [] {
    const auto* t = pc::current_tick();   // frame numarası, planlanan zaman, deadline
    /* t->index ... */
}, {.name = "video"});

caller.set_period(video, pc::hz(50));
if (auto s = caller.stats(control)) {
    // s->deadline_misses, s->avg_lateness, s->lateness_percentile(0.99), s->max_duration, s->total_spin ...
}
caller.remove_and_wait(control);
// destructor tüm görevleri durdurur ve bekler
```

### Qt ile

Görevler **kendi thread'lerinde** çalışır, GUI thread'inde değil. Widget'lara dokunmak için olayı GUI thread'ine aktarın:

```cpp
caller.add(pc::hz(60), [this] {
    auto frame = compute();                                   // ağır olmayan iş, görev thread'inde
    QMetaObject::invokeMethod(this, [this, frame] { show(frame); }, Qt::QueuedConnection);
});
```

`PeriodicCaller`'ı bir `QObject`'in üyesi yapıp destructor'da otomatik durmasına güvenebilirsiniz. Paylaşılan veriye
erişirken (görev thread'i ↔ GUI thread'i) senkronizasyon gerekir; görev içinde uzun süre kilit beklemeyin.

### API özeti

| Fonksiyon | Açıklama |
|---|---|
| `PeriodicCaller(SchedulerOptions = {})` | Zamanlayıcıyı oluşturur (thread'ler görev eklendikçe açılır) |
| `TaskId add(Period, Task, TaskOptions = {})` | Görev ekler; kendi thread'i hemen başlar |
| `bool remove(TaskId)` | Kaldırır, beklemez; o an çalışan çağrı tamamlanır |
| `bool remove_and_wait(TaskId)` | Kaldırır, çalışan çağrının bitmesini ve lambda'nın yok edilmesini bekler |
| `bool set_period(TaskId, Period)` | Periyodu değiştirir; yeni ızgara son tick'ten başlar, frame numarası devam eder |
| `std::optional<TaskStats> stats(TaskId)` | İstatistik (en fazla 1 tick eski; `stop()` sonrası kesin) |
| `void stop()` | Tüm görevleri durdurur ve bekler; istatistikler okunabilir kalır |
| `bool uses_high_resolution_timer()` | Yüksek çözünürlüklü timer (Windows 10 1803+) kullanılıyor mu |
| `Clock::time_point epoch()` | `phase` ızgarasının başlangıcı |
| `const TickInfo* current_tick()` | Görev içinden: frame numarası, planlanan zaman, deadline (dışarıda `nullptr`) |
| `Period(5ms)`, `pc::hz(60)` | Periyot: herhangi bir tam sayılı `std::chrono` süresi veya frekans |
| `static PeriodicCaller& shared(SchedulerOptions = {})` | Süreç genelinde ortak instance (DLL içinde tek) |
| `static void shutdown_shared()` | Ortak instance'ı durdurur ve yok eder |
| `TaskScope(PeriodicCaller& = shared())` | Bir grup görevi (örn. bir plugin'in) sahiplenir; `clear()`/destructor hepsini kaldırıp bekler |

### `SchedulerOptions`

| Alan | Varsayılan | Açıklama |
|---|---|---|
| `max_spin` | `50us` | Tick başına busy-wait üst sınırı. `0` = hiç spin yok (en düşük CPU, başlama gecikmesi = OS timer gecikmesi) |
| `priority` | `TimeCritical` | Görev thread'lerinin önceliği (`Normal`, `AboveNormal`, `Highest`, `TimeCritical`) |
| `use_mmcss` | `false` | MMCSS "Pro Audio": gerçek zamanlı sınıf öncelik. Ağır yük altında frame güvenliği için açılabilir; açıkken Windows ağ paket işlemeyi kısabilir (`NetworkThrottlingIndex`) |
| `prefer_performance_cores` | `true` | Hibrit CPU'da P-çekirdekleri seçer; homojen CPU'da (örn. i7-10700) etkisiz |
| `disable_power_throttling` | `true` | Thread'leri EcoQoS'tan çıkarır (Windows 11 arka plandaki uygulamayı E-çekirdeğe/düşük frekansa itebilir) |
| `on_error` | — | Görev exception fırlatınca, görev thread'inde çağrılır |
| `on_deadline_miss` | — | Deadline kaçınca, görev thread'inde çağrılır (kısa tutun) |

### `TaskOptions`

| Alan | Varsayılan | Açıklama |
|---|---|---|
| `missed_tick_policy` | `CatchUp` | `CatchUp`: hiçbir frame atılmaz, gecikenler art arda çalışır. `Skip`: yalnızca en güncel frame çalışır, eskiler atılır |
| `max_catch_up` | `0` (sınırsız) | `CatchUp`'ta art arda çalıştırılacak en fazla frame; fazlası atılır. Debugger durdurması / uyku sonrası binlerce çalıştırmayı önler |
| `run_immediately` | `false` | İlk çağrı hemen mi, bir periyot sonra mı |
| `deadline` | 1 periyot | Görevin tick'ten itibaren bitmesi gereken süre |
| `phase` | — | Görevi ortak ızgaraya hizalar: `epoch() + phase + n * period`. Aynı periyotlu görevleri birbirinden ayırmak için |
| `name` | `PeriodicCaller #<id>` | Debugger / profiler'da görünen thread adı (UTF-8) |

### `TaskStats`

`runs`, `deadline_misses`, `skipped_ticks`, `exceptions`; gecikme (`last/avg/max_lateness`), süre (`last/avg/max_duration`),
OS timer gecikmesi (`avg/max_timer_latency`), öğrenilen erken uyanma (`wake_lead`), toplam spin süresi (`total_spin`),
gecikme histogramı (`lateness_histogram`, 2'nin kuvveti µs kovaları) ve `lateness_percentile(p)`.

## Hassasiyet, CPU ve frame güvenliği

- **CPU:** her tick bir uyanma + en fazla `max_spin` busy-wait'tir. 2 × 5 ms + 3 × 60 Hz (~580 tick/s) için en kötü durum
  ~%3 tek çekirdek (`max_spin = 50us`), tipik olarak çok daha azı; `max_spin = 0` ile ~%0. CPU'yu sürekli döndüren bir
  zamanlayıcı en az bir çekirdeğin %100'ünü harcar.
- **Başlama gecikmesi:** Windows timer'ı tipik olarak onlarca–birkaç yüz µs geç uyanır. Adaptif lead bunun `max_spin`'e
  sığan kısmını telafi eder. Daha düşük gecikme isterseniz `max_spin`'i 100–200 µs'ye çıkarın (CPU ile orantılı artar).
- **Frame kaçırma** gecikmenin ortalamasından değil, nadir büyük sapmalardan gelir (sürücü DPC/ISR patlamaları, güç
  durumu geçişleri, sayfa hataları). Öneriler:
  - Hedef makinede **LatencyMon** çalıştırın; sorunlu sürücüleri (Wi-Fi, ses, ACPI) gösterir.
  - Güç planı: *Yüksek Performans* / *Nihai Performans*.
  - Görev içinde bellek ayırma, disk/ağ I/O ve uzun süren kilitlerden kaçının; bunları başka bir thread'e aktarın.
  - Ağır sistem yükü altında `use_mmcss = true` deneyin.
  - `stats().deadline_misses`, `max_lateness` ve `on_deadline_miss` ile izleyin.
- Windows gerçek zamanlı bir işletim sistemi değildir; frame kaçırma son derece nadir hale getirilebilir ve
  ölçülebilir, ama %100 garanti edilemez.

### Benchmark

`periodic_caller_bench` hedef iş yükünü (2 × 5 ms, 2 × 60 Hz, 1 × 50 Hz) farklı `max_spin` değerleriyle çalıştırır ve
görev başına gecikme yüzdeliklerini, deadline miss'leri ve süreç CPU kullanımını yazar:

```bat
build\Release\periodic_caller_bench.exe --seconds 30 --spin 0,50,200 --work 200
build\Release\periodic_caller_bench.exe --seconds 600 --spin 50            :: nadir sapmaları görmek için uzun koşu
```

Gerçek sonuçlar için sanal makinede değil **hedef PC'de** çalıştırın.

## Derleme

Gereksinimler: Windows 10 1803+ (daha eskisi de çalışır, hassasiyet düşer), **MSVC (Visual Studio 2022 17.2+ veya Visual Studio 2026)**, CMake 3.21+.
Yalnızca MSVC desteklenir.

```bat
cmake --preset msvc-x64
cmake --build --preset release
ctest --preset release
build\Release\periodic_caller_example.exe
```

Testlerin bir kısmını çalıştırmak için: `build\Release\periodic_caller_tests.exe catch_up` (adında geçen testler).

Visual Studio'da: **File → Open → Folder** ile klasörü açın; `CMakePresets.json` otomatik tanınır.

CMake kullanmadan mevcut bir `.vcxproj`'a eklemek için:
1. `src/PeriodicCaller.cpp` dosyasını projeye ekleyin, `include/` klasörünü *Additional Include Directories*'e ekleyin.
2. *C++ Language Standard* = **Preview - Features from the Latest C++ Working Draft (`/std:c++latest`)**.
3. `winmm.lib` kod içinden (`#pragma comment`) otomatik linklenir.

Başka bir CMake projesinde (örn. Qt5 uygulamanız):

```cmake
add_subdirectory(PeriodicCaller)
target_link_libraries(my_app PRIVATE periodic_caller::periodic_caller)
```

## Birden fazla DLL (plugin) ile kullanım

Kod hangi modüle derlenirse zamanlayıcı o modülde yaşar. Tüm plugin'lerin **tek bir ortak zamanlayıcı** kullanması için:

1. `PeriodicCaller.cpp`'yi **tek bir DLL**'e (örn. util DLL'iniz) derleyin ve o projede `PERIODIC_CALLER_EXPORTS` tanımlayın.
   Bu DLL'i kullanan tüm modüllerde (exe, plugin'ler) `PERIODIC_CALLER_IMPORTS` tanımlayın.
   CMake ile: `-DPERIODIC_CALLER_SHARED=ON` bunu otomatik yapar.
   Kendi export makronuz varsa `PERIODIC_CALLER_API`'yi önceden tanımlayabilirsiniz.
2. Herkes `PeriodicCaller::shared()` kullanır; bu, süreçte **tek** instance döndürür.
3. Plugin görevlerini bir `TaskScope` ile ekler ve kendi kapanış fonksiyonunda temizler:

```cpp
// plugin.cpp
pc::TaskScope* g_scope = nullptr;   // Global nesne DEĞİL, pointer (aşağıya bakın)

extern "C" __declspec(dllexport) void plugin_start() {
    g_scope = new pc::TaskScope();                 // PeriodicCaller::shared() kullanır
    g_scope->add(5ms, [] { /* ... */ });
    g_scope->add(pc::hz(60), [] { /* ... */ });
}

extern "C" __declspec(dllexport) void plugin_stop() {
    delete std::exchange(g_scope, nullptr);        // Görevleri kaldırır, çalışanları BEKLER
}
// Host: plugin_stop() -> FreeLibrary(plugin)
```

Kurallar:

- **Unload'dan önce temizlik şart.** Görevin kodu plugin DLL'inde; plugin `FreeLibrary` ile kalkarken görev kayıtlı veya çalışıyorsa süreç çöker.
  `TaskScope::clear()` / destructor'ı ve `remove_and_wait()`, çalışan çağrının bitmesini **ve** lambda'nın yok edilmesini bekler; döndükten sonra unload güvenlidir.
- **DllMain'de / global destructor'da temizlik yapmayın.** `DLL_PROCESS_DETACH` loader lock altında çalışır; orada thread beklemek deadlock'a yol açar.
  Bu yüzden `TaskScope` global nesne değil, `plugin_stop()` içinde silinen bir pointer olmalı.
- `shared()` instance'ı bilerek otomatik yok edilmez (aynı loader lock nedeniyle). Süreç kapanırken bir şey yapmanız gerekmez;
  util DLL'i süreç bitmeden kaldırılacaksa host önce tüm plugin'leri durdurup sonra `PeriodicCaller::shutdown_shared()` çağırmalıdır.
- Tüm modüller aynı Visual Studio sürümü ve aynı `/MD` ayarıyla derlenmelidir (API'de `std::move_only_function` vb. STL tipleri var).
- `remove()` beklemez (görev içinden veya hızlı kaldırma için); unload öncesi her zaman `remove_and_wait()` / `TaskScope` kullanın.

## Notlar

- Bir görevin içinden `PeriodicCaller` nesnesini **yok etmeyin**. `stop()`, `remove()`, `set_period()` çağırmak güvenlidir;
  görev içinden `stop()` beklemeden durdurur.
- İki görev `remove_and_wait()` ile **birbirini** beklememeli (deadlock).
- Her görev bir thread kullanır; bu kütüphane bir avuç (≈ 1–20) hassas görev için tasarlanmıştır, yüzlerce görev için değil.

## 0.1'den geçiş

- `ExecutionMode` ve `OverlapPolicy` kaldırıldı: her görev artık kendi thread'inde (eski *Dedicated*'ın daha hassas hali);
  aynı görevin çağrıları hiçbir zaman üst üste binmez.
- `spin_threshold` → `max_spin` (artık sabit değil, adaptif ve üst sınır; varsayılan 50 µs).
- `high_priority_thread` → `priority`.
- `MissedTickPolicy` varsayılanı `Skip` → `CatchUp`; `TaskStats::missed_ticks` → `skipped_ticks`, `overrun_skips` kaldırıldı,
  `deadline_misses` ve histogram eklendi.
- `remove()` artık boştaki görevin lambda'sını da beklemeden, görevin kendi thread'inde (hemen ardından) yok eder;
  yok edilmesini beklemek için `remove_and_wait()`.
- Periyot `std::chrono::nanoseconds` yerine `Period`: tüm `std::chrono` tam sayılı süreleri ve `pc::hz()` kabul edilir.
