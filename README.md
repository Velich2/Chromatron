# Chromatron

Плагин хроматической аберрации «дорогой оптики» для **After Effects**, **Premiere Pro** и **OpenFX (DaVinci Resolve)**.
CPU и GPU (CUDA, OpenCL, Metal) рендерят одним и тем же исходником ядер.

![preview](docs/preview.png)

## Режимы

### RGB Separation
| Параметр | Что делает |
|---|---|
| Color Pair | Red/Cyan, Green/Magenta, Blue/Yellow, Red/Blue (зелёный на месте) |
| Angle | направление, в котором расходятся группы каналов (в противоположные стороны) |
| Distance | расстояние между группами, px |
| Spread | 0 — жёсткий цифровой сдвиг, 1 — мягкий «призменный» размаз канала |

### Lens Aberration
Стилизованная латеральная хроматическая аберрация: спектр (400–700 нм) масштабируется вокруг оптического центра,
эффект растёт к углам кадра.

| Параметр | Что делает |
|---|---|
| Lateral | разброс спектра в углу кадра, px; знак меняет стороны тёплой/холодной каймы |
| Fringe Balance | −1 красно-оранжевый / циан · 0 оранжевый / тил · +1 золотой / лазурь |

**Без magenta, purple и грязного зелёного.** Дисперсия монотонна по спектру, а из хроматической части смещения
удаляется компонента magenta↔green (с сохранением яркости). Каймы лежат только на тёплой/холодной оси, заданной
Fringe Balance – в том числе на тонких линиях, где обычный спектральный сдвиг даёт розовый/зелёный. Это проверяется
тестом для всех значений Balance и для ACES.

### Physical Lens
Модель по физике стекла, без стилизации: показатель преломления по формуле Коши (n − n₀ ∝ 1/λ²), фокальный сдвиг
каждой длины волны для выбранной коррекции объектива, спектр по функциям цветового соответствия CIE 1931.
Латеральный цвет (разное увеличение длин волн) растёт к углам, осевой (продольный) расфокусирует все длины волн,
кроме сфокусированной, одинаково по всему кадру – отсюда пурпурный ореол светосильных объективов. Пурпурные и
зелёные каймы здесь не вычищаются: у реальной оптики они бывают.

| Параметр | Что делает |
|---|---|
| Correction | Singlet – некорригированная линза; Achromat – линии F и C (486 / 656 нм) в одном фокусе, остаётся вторичный спектр (пурпурный / зелёный); Apochromat – в одном фокусе ещё и линия d (588 нм) |
| Lateral Color | разброс спектра в углу кадра, px (для выбранной коррекции – её остаточный спектр) |
| Axial Color | расфокус самой далёкой от фокуса длины волны, px, по всему кадру |
| Focus Wavelength | длина волны в идеальном фокусе, нм (550 – зелёный, как у большинства объективов) |

### Общие
**Edge Mask** – для всех режимов:

| Параметр | Что делает |
|---|---|
| Contrast Threshold | 0 – везде; выше – только на всё более контрастных переходах света (на 1.0 – HDR-источники) |
| Threshold Softness | мягкость порога |
| Show Mask | где появляются каймы, в ч/б: чёрный – пиксель не меняется, белый – сильная кайма. Учтены порог, Mix и рост эффекта к углам кадра |

**Optics:**

| Параметр | Что делает |
|---|---|
| Optical Center | оптическая ось (Lens Aberration и Physical Lens) |
| Fringe Blur | смягчает цвет кайм, px; яркость и детали изображения остаются резкими. Если значение сравнимо с шириной каймы, цвет уходит в серый. В Physical Lens недоступен: там размытие задаёт Axial Color |
| Mix | смешивание с оригиналом |
| Use GPU | рендер на GPU, если хост его даёт; выключено – на CPU |
| Color Space | кодировка входа (см. ниже), последний параметр в списке |

В конце каждой группы есть кнопка **Reset**: она возвращает параметры этой группы к исходным значениям
(Mode, Use GPU и Color Space не трогает).

Число спектральных отсчётов подбирается автоматически: от 12 и больше, если соседние длины волн расходятся
сильнее чем на 1,25 px; оставшиеся промежутки сглаживаются, поэтому полос в каймах не бывает.

## Цвет и ACES
Оптика всегда считается в **линейном свете**. Параметр *Color Space*:

| Значение | Обработка |
|---|---|
| **Auto** (по умолчанию; в AE и в программах без упорядочивания списков — последний пункт) | Resolve/OFX: пространство, которое программа сообщает для клипа (OpenFX colour management), иначе *Rec.709 Gamma 2.4*. After Effects: 32 бит → *Linear*, 8/16 бит → *sRGB* |
| Linear (Rec.709 primaries) | как есть |
| sRGB / Rec.709 Gamma 2.4 (BT.1886) | декод → линейный Rec.709 → обратно (BT.1886 с нулевым чёрным = степень 2.4) |
| Rec.709 (Scene) | обратная камерная кривая BT.709 → линейный Rec.709 → обратно |
| ACEScg | линейный AP1 |
| ACEScct / ACEScc | декод → ACEScg (AP1) → обратно |
| ACES2065-1 (AP0) | AP0 → AP1 → обратно |
| DaVinci Wide Gamut / Intermediate | декод Intermediate → DWG → ACEScg (AP1) → обратно |
| DaVinci Wide Gamut (linear) | DWG → AP1 → обратно |

Широкие гаммы (ACES, DaVinci Wide Gamut) обрабатываются в AP1 с его коэффициентами яркости; оси кайм переносятся
из Rec.709 в AP1 — оттенки одинаковы в любом пайплайне. Матрица DWG↔AP1 выведена из первичных цветов DWG
(Bradford D65→D60, тем же способом получается эталонная Rec.709→AP1).
Ручной выбор нужен, только если Auto ошибся. Точная кривая почти не важна: кадр декодируется и кодируется обратно
той же кривой, поэтому неточность влияет лишь на то, насколько линейно считается оптика, а не на цвета картинки;
неизвестные программе пространства Auto относит к своему семейству (линейное / гамма / лог).
В Resolve: проект ACES → *ACEScct*; DaVinci YRGB Color Managed с широкой гаммой (timeline DaVinci Wide Gamut /
Intermediate) → *DaVinci Wide Gamut / Intermediate*; DaVinci YRGB без управления цветом → *Rec.709 Gamma 2.4*
(по умолчанию); таймлайн или клип в «Rec.709 (Scene)» → *Rec.709 (Scene)*.
В After Effects: 8/16 бит без линеаризации → *sRGB*, линеаризованный 32 бит → *Linear*, OCIO/ACES → *ACEScg*.

## Как устроено
```
src/kernels/ChromatronKernels.h   ядра одним исходником: C++ / CUDA / OpenCL C / Metal
src/core/                         модель: настройки → спектральные отсчёты, пирамида размытия, маска; конвейер проходов
src/backends/                     CPU (std::thread), CUDA (stream), OpenCL (cl_mem), Metal (MTLBuffer)
src/ofx/                          OpenFX-плагин (Resolve): CPU/CUDA/OpenCL/Metal по выбору хоста
src/ae/                           After Effects: SmartFX, 8/16/32 bpc, GPU через PF_Cmd_SMART_RENDER_GPU;
                                  Premiere Pro: PF_Cmd_RENDER (BGRA 8u/32f на CPU) и GPU-фильтр xGPUFilterEntry
tools/chromatron_cli.cpp          рендер PPM/PFM тем же конвейером
tests/test_core.cpp               инварианты модели и паритет OpenCL ↔ CPU
```
Проходы кадра: импорт (декод цвета) → 3 уровня гауссова размытия → (маска: яркость → локальный контраст →
порог → дилатация) → спектральный сбор с отсечением magenta/green → кодирование обратно.

## Сборка
Нужны CMake ≥ 3.20 и C++17.

**Resolve / OFX** (Windows, macOS, Linux):
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build      # Chromatron.ofx.bundle → папка OFX-плагинов
```
OpenFX SDK скачивается автоматически (тег `OFX_Release_1.5.1`) или задаётся `-DOFX_SDK_DIR=...`.
CUDA собирается, если найден CUDA Toolkit (Windows/Linux); OpenCL — если найден OpenCL SDK; Metal — на macOS.

**Windows одной командой** — `build_windows.bat` (нужны Visual Studio 2022 или 2026 с *Desktop development with C++*,
Git, по желанию CUDA Toolkit). Скрипт сам находит Visual Studio и папку After Effects SDK
(по умолчанию `C:\Users\Velich\AfterEffectsSDK_26.5_win`, переопределяется переменной `AE_SDK_ROOT`),
а также Premiere Pro SDK для GPU-рендера в Premiere (по умолчанию `C:\Users\Velich\Premiere_Pro_24.0_C++_Win_SDK`,
переменная `PR_SDK_ROOT`; без него Premiere считает эффект на CPU),
собирает AE и OFX в Release, запускает тесты и сразу устанавливает плагины: `Chromatron.aex` в
`C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore`, `Chromatron.ofx.bundle` в
`C:\Program Files\Common Files\OFX\Plugins`. Запускать из консоли от администратора, при закрытых After Effects и Resolve:
```
build_windows.bat              :: сборка + тесты + установка
build_windows.bat noinstall    :: только сборка и тесты
```

**After Effects** (Windows, macOS) — нужен After Effects SDK от Adobe (не распространяется):
```
cmake -S . -B build -DCHROMATRON_BUILD_AE=ON -DAE_SDK_DIR=/path/to/AfterEffectsSDK
cmake --build build --config Release
```
Результат: `build/AfterEffects/Chromatron.aex` (Windows) или `Chromatron.plugin` (macOS).
GPU в AE: CUDA и OpenCL (Windows), Metal (macOS). Если AE выбрал DirectX, эффект рендерится на CPU.

**Premiere Pro** загружает тот же `.aex`. С `-DPR_SDK_DIR=/path/to/Premiere_Pro_SDK` в модуль добавляется
GPU-фильтр Premiere (Mercury: CUDA, OpenCL с кадрами 32f и 16f, Metal). Без Premiere SDK, при ускорении
через DirectX или в режиме Software Only эффект рендерится на CPU через `PF_Cmd_RENDER`.

**Проверка:**
```
ctest --test-dir build
build/chromatron_cli --synthetic 1920x1080 out.ppm --mode abr --lateral 8 --blur 2 --threshold 0.5
build/chromatron_cli --synthetic 1920x1080 out.ppm --mode phys --correction achromat --lateral-color 10 --axial 3
```

## CI
GitHub Actions (`.github/workflows/build.yml`) на каждый PR собирает и тестирует проект на Linux (CPU + OpenCL на
pocl), Windows (CPU + CUDA + OpenCL) и macOS (CPU + Metal, universal). Готовые OFX-бандлы для каждой платформы
лежат в артефактах запуска. After Effects в CI не собирается: SDK Adobe нельзя выкладывать в репозиторий.

## Статус проверки
* Ядра, CPU-бэкенд, OpenCL-бэкенд (на pocl) — собраны и протестированы, OpenCL совпадает с CPU.
* OFX-модуль собирается на Linux, экспортирует точки входа и регистрирует плагин.
* CUDA-ядра компилируются в PTX; Metal-исходник проверен синтаксически. На реальных GPU, в Resolve и
  в After Effects ещё не запускалось — AE-часть написана по SDK, но без SDK под рукой не компилировалась.
