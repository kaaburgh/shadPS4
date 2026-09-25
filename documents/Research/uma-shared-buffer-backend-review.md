<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# UMA / shared-buffer backend для shadPS4: независимый review плана экспериментов

> **Статус:** review плана, код не менялся.
> **База review:** upstream `shadps4-emu/shadPS4` `e4ca349` (`add Sys shutdown to posix sockets (#5104)`).
> Это уже **после** #5047 *и* после #5100 (`buffer_cache: Rework memory tracker and implement
> batched uploads`), #5069 (uffd) и #5092 (invalidate images after raw buffer writes).
> Все ссылки `file:line` ниже проверены на `e4ca349`. На более новой базе перепроверьте их.
>
> **Ревизия 2** учитывает второй независимый review (Sol). Исправлено: ограничение
> `VA ≡ PA (mod 64 KiB)` было сформулировано слишком сильно; неточно было обоснование CPU-когерентности
> для udmabuf; указаны версии ядра для лимита udmabuf; для E1 выбран другой примитив отложенных
> операций; убраны преждевременные kill-критерии. Подробности — в §8.

---

## 0. Вердикт коротко

1. **Главный риск плана не там, где он его ищет.** План считает, что основная неизвестная — «может ли
   Vulkan дать одну физическую память CPU и GPU». Но сейчас shadPS4 выдаёт guest'у сигналы
   «GPU закончил» (EOP/EOS/ReleaseMem label writes, EOP IRQ) **в момент разбора PM4 на CPU, до того
   как host GPU выполнил работу**. Корректность этого держится именно на mirror-модели:
   snapshot-копии гасят WAR-гонки, а readback с `scheduler.Finish()` гасит RAW-гонки. Если убрать
   копии, но оставить эти ранние сигналы, получатся гонки данных. Проявятся они как раз как
   vertex corruption. Шаги 7–8 плана в таком виде проверяли бы модель fence'ов, а не backing.
2. **Точность fence'ов нужно проверять первой**, на текущем mirrored backend, до всякого
   shared-memory кода. Это дешевле PoC и заранее отвечает на вопрос, окупится ли проект.
3. **На AMD Linux host-pointer import для shadPS4 недоступен в принципе**, а не «иногда падает».
   Guest-память в shadPS4 — это `memfd` (`MAP_SHARED`). libdrm создаёт userptr BO с
   `AMDGPU_GEM_USERPTR_ANONONLY`, а ядро отвечает на file-backed VMA `-EPERM`. Историческая попытка
   в #2819 прямо это фиксировала. Для RADV (Vega UMA) нужен **другой механизм**: `udmabuf` (memfd →
   dma-buf) + `VK_EXT_external_memory_dma_buf`. Поэтому PoC на RTX с `VK_EXT_external_memory_host`
   ничего не говорит про AMD-путь. Feasibility на AMD надо проверять с первого дня, а не на шаге 9.
4. **Sparse + shared memory упирается в гранулярность.** Sparse block — 64 KiB (RADV: проверено в
   исходниках; на NVIDIA обычно так же), а guest отображает память кусками по 16 KiB. Требование
   Vulkan относится к `(resourceOffset, memoryOffset, size)` **внутри `VkDeviceMemory`**, а не к PA.
   - При **крупных** импортах PA-чанков (дёшево, мало объектов) блок шарится, только если VA→PA
     непрерывен и `VA ≡ PA (mod 64 KiB)`.
   - Остальные блоки можно покрыть **сшитыми** импортами: `UDMABUF_CREATE_LIST` на AMD, import
     guest-VA диапазона на NVIDIA. Цена — число объектов, re-import на каждый remap и стоимость BO list
     на submit (§3.3).
   - Непокрываемые блоки (без memfd) остаются mirrored.
   Значит, per-block policy нужна с самого начала, это не «потом».
5. **Refactor (шаг 2) до feasibility не делать.** Правильную границу абстракции задают вещи, которых
   в текущем коде нет вовсе: rebind/unbind с ожиданием in-flight работы, per-block policy,
   PA-индексированная таблица import'ов, hazard tracking по timeline. Refactor «вслепую» почти
   наверняка придётся переделывать, а upstream BufferCache сейчас меняется каждые пару недель.
6. Уровень абстракции «physical backing/residency provider под sparse arenas» для **хранения**
   выбран верно: `resident_ranges` (`IntervalList<Backing{memory, offset}>`) уже по сути является
   sparse page table. Но **когерентность не сводится к свойству backing**. Главная её часть
   (timeline/fence semantics) глобальна и одинакова для всех backing'ов.

---

## 1. Что сейчас держит корректность и что пропадёт вместе с копиями

### 1.1 Guest видит «GPU done» раньше, чем host GPU что-то сделал

- **EOP**: `src/video_core/amdgpu/liverpool.cpp:682-694` — `rasterizer->OnFence()`, затем
  `SignalFence(... TryWriteBacking(address, &data, ...))` и `IrqC::Signal(GfxEop)`. Всё это на
  CP-потоке, во время разбора, без ожидания host GPU.
- **EOS**: `liverpool.cpp:663-681` — аналогично. `Finish()` вызывается только для GDS store.
- **ReleaseMem / WriteData** на compute-очереди: `liverpool.cpp:~1073-1117` — то же самое.
- **WriteData** (gfx): `liverpool.cpp:732-746` — `std::memcpy` в guest-память во время разбора.
- **DmaData**: `liverpool.cpp:695-731` → `Rasterizer::FillBuffer/CopyBuffer`. Если регион не
  GPU-modified, это **CPU `std::fill`/`std::memcpy` во время разбора**
  (`vk_rasterizer.cpp:1148-1197`).
- **EventWrite ZPassDone**: результаты occlusion пишутся CPU во время разбора (`liverpool.cpp:650-659`).
- **WaitRegMem**: CP-поток крутится на guest-памяти (`liverpool.cpp:783-800`).
- `OnFence()` (`vk_rasterizer.cpp:402-405`) лишь сбрасывает upload batch. GPU он не ждёт.
- `AcquireMem` игнорируется (`liverpool.cpp:769-772`).

### 1.2 Почему это сегодня (в основном) работает

- **CPU→GPU, WAR:** `FlushSyncBatch` снимает snapshot guest-памяти в staging
  (`buffer_cache.cpp:443-448`, `CopySparseMemory`) на границе сессии. После #5100 это
  «перед каждым label write/IRQ или submit», а upload command buffer отправляется перед primary.
  Guest, увидев ранний EOP, может сразу переписывать данные: host GPU работает со своей копией.
- **GPU→CPU, RAW:** в режиме `Precise` GPU-modified страницы read-protected. Чтение CPU даёт fault,
  затем `ReadMemory` → `DownloadMemory` → `scheduler.Finish()` (`buffer_cache.cpp:131-169`).
  Этот `Finish()` попутно синхронизирует timeline. При `readbacks_mode = Disabled` (это **значение
  по умолчанию**, `core/emulator_settings.h:424`) CPU просто никогда не видит GPU-результатов в
  буферах. Данные устаревшие, но согласованные.

### 1.3 Что случится при shared backing с тем же порядком сигналов

| Направление | Сейчас | Shared + ранние fence'ы |
|---|---|---|
| Guest пишет буфер после (раннего) EOP, host GPU ещё не прочитал | GPU читает snapshot → ОК | GPU читает **новые** байты → corruption (очень похоже на vertex explosions) |
| Guest читает GPU-результат после (раннего) EOP | Disabled: stale-but-consistent; Precise: fault + Finish | читает **ещё не записанное** / частично записанное → torn data |
| CP-side memcpy (WriteData/DmaData fast path) в регион, который читает уже записанный, но не выполненный draw | upload в следующей сессии | draw видит данные «из будущего» |
| WaitRegMem на значение, которое пишет шейдер | fault → readback → Finish | **deadlock**: CP ждёт значение, которое GPU запишет, только когда CP сделает submit |
| Guest unmap / переиспользование PA после раннего fence | mirror остаётся на VA, данные в своём VkDeviceMemory | поздняя GPU-запись попадает в PA, который guest уже отдал под другое |

Вывод: для shared backing нужна **точная модель времени**. Любой guest-visible сигнал прогресса GPU
должен появляться только после фактического завершения host GPU. Каждая CP-side операция над
guest-памятью должна либо выполняться на GPU timeline, либо упорядочиваться относительно неё.
Эвристики здесь не спасают: каждая пропущенная точка — это уже гонка данных, а не просто лишний
readback.

**Prior art.** Vita3K (тоже unified-memory консоль) при переходе на BDA +
`VK_EXT_external_memory_host` сформулировал требование прямо: *«the host gpu must be done when the
notification is signaled»* (Vita3K PR #2272). Это тот же вывод.

---

## 2. Ответы на вопросы

### Q1. Есть ли фундаментальная ошибка в последовательности?

Да, их три. Есть и несколько мелких.

**(a) Fidelity fence'ов отсутствует как отдельный этап, а на ней держится весь проект.**
Её надо поставить **до** шагов 6–8 и проверить на текущем mirrored backend (эксперимент E1 в §6).
Если «fence at completion» обходится на Bloodborne в 2× frame time, это станет известно за неделю, а
не после M3.

**(b) AMD проверяется слишком поздно, и её механизм отличается.** На RADV
`VK_EXT_external_memory_host` для memfd-памяти shadPS4 не работает (§3.1). Шаг 9 («тот же backend на
AMD») не может быть «тем же backend'ом». Standalone probe надо гонять на **обеих** машинах с первого
дня. 3300U — более информативная цель по семантике: настоящий UMA, AMD APU как PS4, snooped и
non-snooped доступ как Onion/Garlic, открытый драйвер, который можно отлаживать. RTX удобнее
скоростью итераций, но sysmem-через-PCIe имеет противоположный UMA профиль производительности.

**(c) Шаги 4–5 не учитывают ограничения sparse.**
- гранулярность 64 KiB: при bulk-импорте нужна сравнимость `VA ≡ PA (mod 64 KiB)`, иначе нужен
  сшитый импорт со своей ценой (§3.3);
- bind одной памяти в два arena offset'а требует `sparseResidencyAliased` и
  `VK_BUFFER_CREATE_SPARSE_ALIASED_BIT`. Arenas сейчас создаются без этого флага
  (`buffer.cpp:108-110`);
- sparse bind импортированной памяти требует, чтобы arena была создана с
  `VkExternalMemoryBufferCreateInfo{handleTypes = ...}` (VUID-VkSparseMemoryBind-memory-02731).
  Это свойство **создания** буфера: на существующие arenas его не «докрутить».

**Мелкие:**
- (d) Шаг 6 «оставить существующий dirty/readback tracking как safety net» опасен (§2 Q6/Q8). Для
  shared-диапазонов upload/download машинерия mirror'а **пишет** в ту же память и теряет записи.
  Safety net должна только читать.
- (e) Шаг 1: baseline нужен не только по perf-счётчикам. Нужны hazard census и shareability census
  (§6, E0). Кроме того, `main` уже ушёл дальше #5047: #5100 переписал tracker и uploads.
  Baseline надо привязать к конкретному commit'у.
- (f) Шаг 8 надо явно сформулировать как проверку модели fence'ов. Если fence'ы ранние, на shared
  path стоит **ожидать больше** explosions, чем на main. Это не баг backing'а.

### Q2. Refactor (шаг 2) до PoC?

**Нет.** Причины:
1. Refactor не даёт информации о feasibility. Главные неизвестные (§3) он не трогает.
2. План сам откладывает выбор границы абстракции на шаг 10, а шаг 2 фиксирует её заранее.
3. Операций, которые определят интерфейс, в текущем коде нет совсем:
   - **unbind/rebind** (сейчас residency монотонна: `EnsureResident` только добавляет, памяти никто
     не освобождает, `buffer_cache.cpp:284-338`);
   - ожидание in-flight работы перед rebind (`SubmitPendingArenaBinds` только signal'ит семафор для
     следующего submit, `buffer_cache.cpp:406-422`, и не ждёт предыдущих);
   - PA-индексированная таблица import'ов;
   - per-block policy.
   Выделять `SparseMirroredBacking` из кода, где этих операций нет, значит выделять не ту границу.
4. Upstream BufferCache меняется очень быстро (#5047 → #5100 за недели). Долгоживущая
   refactor-ветка будет постоянно конфликтовать.
5. «Статистически неразличимая производительность» — дорогой acceptance criterion. Он съест время,
   которое лучше потратить на E0/E1.

Что **стоит** сделать сразу, потому что это маленькие и полезные вещи без изменения поведения:
счётчики (§7), probe флагов (`SPARSE_ALIASED`, external handle types) и, возможно, отдельный
upstream-фикс упорядочивания sparse binds. Интеграцию потом делать через минимальные хуки
(policy-ветка в `EnsureResident`, отдельный путь в `FlushSyncBatch`/`DownloadMemory` для shared-блоков).
Refactor — только после того, как shared-путь заработает.

Ограничение «без virtual calls в hot path» здесь почти ничего не значит. Hot path
(`ObtainBuffer`) касается residency только через `EnsureResident`, а тот и так ходит в interval list.
Policy — это per-block **данные** (бит), а не полиморфизм.

### Q3. Backing/residency provider под sparse arenas или GuestBufferBacking вокруг BufferCache?

Для **хранения** — да, provider под arenas.
- Arenas и BDA page table индексированы guest **VA** (`address_space[]`, `buffer_cache.h:151`;
  BDA entry = `arena BDA + offset`, `buffer_cache.cpp:328-330`). Shared backing их не меняет.
  Меняется только то, какой `(VkDeviceMemory, offset)` забинжен в блок.
- `resident_ranges` уже хранит `Backing{memory, offset}` на VA-интервалах (`buffer_cache.h:156-166`).
  Это и есть нужная таблица.
- Shaders, recompiler и BDA не надо трогать. Это хорошая новость, и гипотеза о #5047 тут верна.

Но граница должна выглядеть так:
- **PhysicalImportTable (PA-индекс):** какие куски memfd импортированы в Vulkan, чанками по
  64–256 MiB, лениво, и `(VkDeviceMemory, offset)` для PA. Ключ — **PA, а не VA**. Aliases
  получаются автоматически, потому что оба VA указывают на один PA.
- **VaBindingSync:** события VMM (map/unmap/remap/protect/mtype) → sparse bind/unbind с правильным
  ожиданием. Плюс per-block решение shared или mirrored.
- **CoherencePolicy per block:** mirrored → текущие upload/download; shared → ничего не копировать.
- **TimelineSync (глобально, одно на всё):** ранние fence'ы (как сейчас) или fence at completion.
  Это **не** свойство backing'а: shared-блоки требуют точного режима, а mirrored с ним тоже
  корректны.

`GuestBufferBacking` вокруг всего BufferCache — неправильный уровень. Он дублирует ObtainBuffer,
BDA и barrier tracking и прячет смешение shared и mirrored блоков внутри одной arena. А такое смешение
неизбежно (§3.3).

### Q4. Есть ли фундаментальное ограничение Vulkan для «imported host memory + sparse binding»?

**На уровне спецификации запрета нет.** Условия (проверено по `validusage.json`):
- `VUID-VkSparseMemoryBind-memory-01096`: memory type должен входить в `memoryTypeBits` sparse буфера;
- `VUID-VkSparseMemoryBind-resourceOffset-09491`: `resourceOffset`, `memoryOffset` и `size` кратны
  `alignment` sparse буфера (64 KiB);
- `VUID-VkSparseMemoryBind-memory-02731`: для импортированной памяти arena должна быть создана с
  `VkExternalMemoryBufferCreateInfo`, где указан этот handle type.
Capability запрашивается через `vkGetPhysicalDeviceExternalBufferProperties` с
`VkPhysicalDeviceExternalBufferInfo{flags = SPARSE_*, usage, handleType}`.

**На практике:**
- **RADV (проверено в исходниках Mesa main):**
  - `memoryTypeBits` sparse-буфера — все типы, кроме 32-bit (`radv_buffer.c:188-189`);
  - alignment `RADV_SPARSE_BUFFER_ALIGNMENT = 64 KiB`;
  - `sparseResidencyAliased = enable_sparse` (`has_sparse = family >= POLARIS10`, Raven подходит);
  - virtual bind принимает любой невиртуальный BO (`radv_amdgpu_winsys_bo_virtual_bind`);
  - `GetPhysicalDeviceExternalBufferProperties` игнорирует sparse flags и сообщает importable.
  Значит, sparse + **dma-buf import** на RADV должен работать. Sparse + **host pointer** на RADV
  упирается не в sparse, а в ANONONLY (§3.1).
- **NVIDIA:** главная неизвестная — входит ли sysmem (`HOST_VISIBLE`) memory type, который возвращает
  `vkGetMemoryHostPointerPropertiesEXT` (или `vkGetMemoryFdPropertiesKHR` для dma-buf), в
  `memoryTypeBits` sparse-буфера, созданного с external handle type. NVIDIA держит DEVICE_LOCAL и
  HOST_VISIBLE типы раздельно и сужает `memoryTypeBits` по видам ресурсов. Пересечение может
  оказаться пустым. **Это go/no-go вопрос первого дня** (E0b, §6). Если пусто — план B ниже.
- **Гранулярность** 64 KiB действует на обоих вендорах. Но это ограничение на раскладку
  `VkDeviceMemory`, а не на guest PA: сшитые импорты его снимают, платя числом объектов (§3.3).
- **Aliases внутри sparse** требуют `sparseResidencyAliased` + `SPARSE_ALIASED_BIT`, иначе нет
  гарантии data consistency. Даже с флагом нужны memory dependencies между записью через один alias
  и доступом через другой (global memory barrier этого достаточно).

**План B, если NVIDIA не даёт sysmem в sparse:** BDA page table уже даёт indirection.
Для DMA/BDA-пути entries можно направить прямо на обычные (не sparse) буферы, забинженные на
импортированные чанки. Именно так делал #2819. Для descriptor-путей (`ObtainBuffer`: VB/IB/UBO/SSBO
требуют одного непрерывного VkBuffer) sparse arena всё равно нужна. Можно, как в #2819,
импортировать непрерывные guest-VA диапазоны целиком, но тогда каждый remap требует re-import.

### Q5. Есть ли механизм лучше, чем `VK_EXT_external_memory_host`?

Для Linux и текущей VMM (всё guest-physical — один memfd; aliases — это `mmap(MAP_SHARED|MAP_FIXED)`
того же fd по PA-offset'у, `address_space.cpp:719-759`) расклад такой:

| Механизм | NVIDIA (5070 Ti) | RADV (Vega) | Aliases | Замечания |
|---|---|---|---|---|
| `VK_EXT_external_memory_host` на **`backing_base`-чанки** | вероятно да (pin shmem страниц), **проверить** | **нет**: `ANONONLY` → `-EPERM` на memfd | да (ключ по PA) | основной вариант — чанки канонического `backing_base`. Для блоков без `VA ≡ PA (mod 64K)` можно импортировать **guest VA диапазон**: host-mapping guest'а уже «сшит» из memfd-страниц. Но такой import привязан к mapping'у и требует re-import на каждый remap (урок #2819). Pin заселяет страницы → импортировать лениво |
| **`udmabuf`** (memfd → dma-buf) + `VK_EXT_external_memory_dma_buf` | неизвестно (dma-buf import у NVIDIA исторически ограничен), **проверить** | **да, основной кандидат** | да | нужен `memfd_create(..., MFD_ALLOW_SEALING)` + `F_SEAL_SHRINK` (сейчас memfd создаётся с `0`, и sealing запрещён); доступ к `/dev/udmabuf` (часто `root:kvm`); **лимит 64 MiB на dma-buf во всех релизных ядрах по v7.2** (§3.5); страницы pin'ятся (`memfd_pin_folios`) → лениво, чанками. `UDMABUF_CREATE_LIST` сшивает до 1024 произвольных 4 KiB-выровненных кусков memfd в один dma-buf (§3.3). Нет MMU-notifier stall'ов |
| Vulkan-owned memory + `VK_EXT_map_memory_placed` | есть в драйверах, проверить `memoryMapRangePlaced` | `memoryMapPlaced = true`, **`memoryMapRangePlaced = false`** | **нет**: одна map на `VkDeviceMemory` (`VUID-vkMapMemory-memory-00678`) | переворачивает владение guest-physical памятью. Нельзя с host-imported памятью (`VUID-VkMemoryMapInfo-flags-09575`). Как основной путь не подходит |
| Vulkan-owned + export dma-buf + свой `mmap` в guest VA | нет (NVIDIA opaque/dma-buf не mmap'ится userspace'ом, насколько известно) | технически да | да | то же переворачивание владения, упирается в GTT-лимиты; как вторая фаза |
| ReBAR (`DEVICE_LOCAL \| HOST_VISIBLE`) для **WC_GARLIC** диапазонов | да | n/a | как у map_placed | интересная **вторая фаза для dGPU**: Garlic на PS4 тоже WC для CPU, игры не читают его CPU'ем. VRAM-скорость для GPU. Aliases не поддержаны |

**Рекомендация.**
- AMD: udmabuf-путь — первым.
- NVIDIA: probe обоих путей (host-pointer и udmabuf). Если udmabuf-import на NVIDIA работает, у вас
  **один** механизм на оба вендора. Это сильно упрощает жизнь.
- Не переходить на анонимную память ради обхода ANONONLY: пропадут aliases, и появятся
  MMU-notifier stall'ы (§3.2).

### Q6. Верна ли гипотеза «same backing ⇒ dirty source-of-truth tracking не нужен»?

**В узком смысле верна:** когда копий нет, нечему устаревать. Биты «CPU modified» и «GPU modified»
как признак того, *где лежат свежие данные*, для shared-блоков не нужны. Но формулировка «оставить
только guest sync translation и cache sync» недооценивает, что остаётся.

1. **Tracking заменяется hazard tracking'ом, а не исчезает.** `IsRegionGpuModified` сегодня — это
   policy-переключатель в полудюжине мест: stream-buffer путь `ObtainBuffer`, CPU fast path
   `FillBuffer`/`CopyBuffer`, `ObtainBufferForImage`, texture cache. В shared-режиме вопрос этих мест
   другой: *«есть ли in-flight host GPU работа, читающая или пишущая этот диапазон?»*. Это таблица
   «last GPU tick per range» против `scheduler.IsFree(tick)`, а не dirty bits. Если fence'ы полностью
   точные и все CP-side операции перенесены на GPU timeline, таблица нужна только для CP-side
   решений и отладки. Если не полностью, без неё не обойтись.
2. **Guest sync translation — это не «перевести fence'ы», а переделать всё из §1.1.** Задерживать
   *все* label writes и IRQ до completion, но уметь разрешать CP-side WaitRegMem без host-ожидания
   (§5, E1).
3. **CPU cache coherence.** `HOST_COHERENT` — неверное формальное обоснование: это свойство
   относится к `vkMapMemory`-mapping'у, а guest CPU ходит через **свой** memfd-mapping.
   - Для udmabuf на amdgpu настоящее основание такое. Foreign dma-buf импортируется без USWC
     (`amdgpu_dma_buf_create_obj`, `flags = 0` для не-amdgpu exporter'а) → `ttm_cached` →
     GPU PTE получают `AMDGPU_PTE_SNOOPED` (`amdgpu_ttm_tt_pde_flags`), плюс x86 DMA coherence.
   - Для NVIDIA host import — x86 PCIe snooping. На x86 CPU-кэши физически тегированы, поэтому два
     разных VA одного PA (`backing_base` и guest VA) когерентны.
   - `DMA_BUF_IOCTL_SYNC` на udmabuf этого **не** обеспечивает (§3.5). Всё сказанное специфично для
     x86. На ARM/Apple нужна отдельная аргументация — ещё одна причина отложить Mac.
   - Если где-то понадобится non-coherent путь, придётся flush'ить записанные CPU диапазоны, а для
     этого снова нужен write tracking.
4. **Vulkan-правила видимости.**
   - Host→device: host-записи до `vkQueueSubmit` видимы автоматически. Записи **после** submit уже
     отправленной работе не гарантированы. Текущая архитектура (CP разрешает ожидания до записи)
     это соблюдает, но только при точных fence'ах.
   - Device→host: перед сигналом нужен `srcStage ALL_COMMANDS / MEMORY_WRITE → dstStage HOST /
     HOST_READ`, затем host-ожидание timeline'а.
   - Guest cache actions в EOP/ReleaseMem (`tc_wb_action_ena` и т.п., `pm4_cmds.h:912-916`) и
     `AcquireMem` — естественные точки для этих барьеров. Сейчас они игнорируются.
5. **Texture cache никуда не девается.** Он использует тот же `PageManager` для write-watchers
   (`texture_cache.cpp:844-934`) и `InvalidateMemory`. Страницы под images по-прежнему нужно
   write-protect'ить. Image→buffer пути (`SynchronizeMemoryFromImage`) в shared-режиме **пишут в
   guest RAM** (§2 Q8).
6. **CPU/GPU атомики на одной памяти** (если игра делает lock-free обмен CPU↔GPU) Vulkan не
   гарантирует, тем более через PCIe. Это редкий паттерн, но census должен его видеть.

### Q7. Какие guest-memory cases проверить помимо aliases

Все они следуют из текущей VMM (`core/memory.cpp`, `core/address_space.cpp`):

1. **Direct, VA/PA не сравнимы mod 64 KiB**: PA `…4000` отображён на VA `…0000`. Bulk-импортом блок
   не забиндить. Должен сработать сшитый импорт или fallback в mirrored.
2. **Direct, склейка 16 KiB кусков**: два `MapDirectMemory` с `Fixed` встык по VA из несмежных PA
   внутри одного 64 KiB блока. Тот же выбор: сшивка или mirrored.
3. **Flexible**: `fmem_map` фрагментируется. Один flexible VMA может состоять из нескольких
   несмежных PA-кусков (`memory.cpp:606-646`, по 16 KiB). Отдельно проверить, даёт ли guest вообще
   GPU-доступ к flexible (shadPS4 вызывает `rasterizer->MapMemory` для любого типа в пределах 40 бит).
4. **Pooled**: `PoolExpand/PoolCommit/PoolDecommit` — commit/decommit churn и **повторный commit
   другого PA на тот же VA**. Identity меняется на том же адресе → rebind + ожидание in-flight работы.
5. **Partial unmap** середины 64 KiB блока; `MAP_FIXED` overwrite части существующей mapping
   (путь «early GPU unmap», `memory.cpp:590-593`).
6. **Remap** того же VA на другой PA сразу после unmap, пока host GPU ещё работает (ранние fence'ы!).
7. **Aliases с разными правами**: один PA в CPU-only VA и в CPU+GPU VA; alias с GPU-only VA
   (без CPU prot); запись через CPU-only alias должна быть видна GPU через другой VA.
8. **Смена memory type** (`SetDirectMemoryType`, mtypeprotect): WB_ONION ↔ WC_GARLIC. Если policy
   выбирается по типу, это событие смены policy.
9. **`Protect`**: снятие/добавление `GpuRead/GpuWrite` на уже забинженном диапазоне.
10. **VMA без memfd-backing**: `phys_addr == -1` → `MAP_ANONYMOUS|MAP_PRIVATE`
    (`address_space.cpp:752-755`), то есть Code/Stack/anon. Shared для них невозможен, должен быть
    fallback. File mappings с GPU prot guest'у запрещены (`memory.cpp:747-750`).
11. **PRT areas** (`SetPrtArea`, `memory.cpp:132-145`): «притворяемся, что всё отображено». Дыры
    должны читаться как 0, а не как чужая память. Нужен bind на zero-page или
    `residencyNonResidentStrict` (проверить свойство).
12. **Arena migration** (`GetArena`, `buffer_cache.cpp:251-281`): после миграции старая и новая arena
    держат bind на ту же память → aliasing между двумя sparse буферами без `SPARSE_ALIASED`. На
    device-local это уже спорно по спецификации, а на shared это ещё и alias с CPU.
13. **Adjacent access через границу 4 GiB arena page** на shared-блоках (миграция и rebind).
14. **Free → Allocate** того же PA другим владельцем (guest сам выбирает PA для direct memory,
    поэтому «карантин» PA невозможен).
15. **Никогда не трогать memfd-страницы под pin**: сейчас `PUNCH_HOLE`/`MADV_REMOVE` нигде не
    используются (проверено). Если кто-то добавит reclaim guest RAM, pinned import (NVIDIA) молча
    разойдётся с memfd. Это инвариант, его надо зафиксировать ассертом или комментарием.

### Q8. Hidden assumptions текущего BufferCache (#5047 + #5100)

1. **Monotonic residency.** Память блоков никогда не освобождается и не переbind'ивается
   (`EnsureResident`, `buffer_cache.cpp:284-338`). Отсюда: `SubmitPendingArenaBinds` не ждёт
   предыдущих submit'ов (`buffer_cache.cpp:406-422`). Для rebind/unbind это **гонка**: sparse bind
   не упорядочен с уже отправленными command buffers без семафоров, и in-flight работа увидит новый PA
   посреди выполнения. Нужно wait на timeline последнего submit'а. Это serialization, так что
   стоимость зависит от частоты VMM churn (надо измерить, E0).
2. **Arena = приватный scratch эмулятора.** `SynchronizeMemoryFromImage` делает `TileImage(...)`
   прямо в arena (`buffer_cache.cpp:350-387`), а HTILE обрабатывается через `FillBuffer(...,
   ZmaskUncompressed)` (`buffer_cache.cpp:341-345`). В shared-режиме это **записи эмулятора в
   guest-visible память**: guest начнёт видеть синтетический HTILE и detiled данные. Такие пути
   для shared-блоков нужно переносить в отдельный scratch.
3. **Download пишет в backing.** `DownloadMemory` → `TryWriteBacking` (`buffer_cache.cpp:162-167`)
   с окном расширения 512 KiB (`buffer_cache.cpp:111-119`). В shared-режиме это read-modify-write
   памяти, которую параллельно пишут guest-потоки, и получаются **lost writes**. Upload
   (`FlushSyncBatch`) симметрично: snapshot в staging, затем GPU копирует его обратно в ту же память
   позже, при выполнении. Всё, что CPU записал в промежутке, откатывается. Поэтому mirror-машинерия
   не должна *писать* в shared-блоки ни при каких условиях. См. шаг 6 плана.
4. **Default CPU-dirty.** Новые `RegionManager` стартуют с `cpu.Fill(~0ULL)` (всё «CPU modified»,
   `region_manager.h:31`). `SynchronizeDmaBuffers` добавляет **все** resident ranges в upload batch
   (`buffer_cache.cpp:220-228`). Для shared-блоков оба места нужно явно исключить.
5. **Tracking по VA, а копии данных — через PA** (`CopySparseMemory`/`TryWriteBacking` идут через
   `backing_base + paddr`). Поэтому **текущий mirror некорректен для aliases**: запись CPU через VA2
   не защищает VA1 и не помечает его dirty, а GPU-копия VA1 устаревает. Shared backing это *чинит*
   по построению. Alias-тесты из шага 5 плана полезно сначала прогнать на `main`, чтобы получить
   baseline-падение.
6. **Одна memory type на все arenas** (`arena_memory_type_index`, DEVICE_LOCAL,
   `buffer_cache.cpp:76-79`) и arenas без external handle types и без `SPARSE_ALIASED`
   (`buffer.cpp:108-110`). Shared-режим требует пересоздания arenas с другими create-параметрами.
7. **Fault buffer обрабатывается на следующем кадре** (`TickFrame`, `buffer_cache.cpp:92-97`), то есть
   один кадр DMA-чтений из non-resident блоков даёт нули. Shared-блоки можно биндить **сразу по
   событию VMM**, и fault-путь для них исчезнет. Это улучшение корректности, его стоит измерить.
8. **Stream-buffer путь** (read-only ≤16 KiB, `buffer_cache.cpp:174-179`) — это snapshot в момент
   записи command buffer'а, а arena-путь — чтение в момент выполнения. При точных fence'ах это одно
   и то же. При ранних fence'ах в одной сессии появляются смешанные «версии» памяти.
9. **CP-side операции над guest-памятью** (§1.1) предполагают, что GPU видит snapshot.

### Q9. Какие уроки #2819 / #3150 / #3404 важно не повторить

- **#2819** (проверено по коду commit'а `52253b45` «Import memory» и `87b37712`):
  - импортировались **guest VA** диапазоны на `MapMemory`, и import никогда не пересоздавался на
    unmap/remap → identity привязана к VA, после remap указывает на старый PA;
  - в коде прямо стоит комментарий *«May fail to import the host memory if it is backed by a file.
    (AMD on Linux)»* — это ANONONLY из §3.1;
  - fallback был «копировать весь map», и его не тестировали как равноправный путь.
  Уроки: ключ — PA, import — канонического `backing_base` (или udmabuf); mixed shared/mirrored
  с первого дня; AMD-путь проверять первым. Reviewer тогда сформулировал ценность в одном
  предложении: *«reserve … guaranteed to produce correct results, free from potential sync issues
  and will work on AMD linux»*. Shared-путь должен выиграть у этого по корректности, а не только по
  скорости.
- **#3150** (readbacks-lite): угадывание, *когда* CPU нужны данные, упирается в data contention.
  Shared memory снимает вопрос *«какие данные копировать»*, но **не** вопрос *«когда можно
  трогать»*. Этот ответ должен быть точным.
- **#3404** (guest fences): классификация labels на «GPU-GPU barrier» и «CPU fence» по наличию
  WaitRegMem — эвристика. Автор сам признал, что label может быть и тем и другим. Итог: регрессии
  (device lost, серые модели) и per-game toggle. Урок: **не классифицировать**. Задерживать *все*
  guest-visible записи до completion, а CP-side WaitRegMem разрешать через таблицу pending-записей
  (§5, E1). Для неизвестного writer'а (label пишет шейдер) делать flush + host wait и перепроверять.
  Появление per-game toggle'а — сигнал, что в дизайне сидит эвристика.
- **#373**: ReBAR и `map_memory_placed` там упомянуты как путь к отказу от dirty tracking. Сейчас
  видно, что placed map не поддерживает aliases и host-imported память, а ReBAR подходит только для
  Garlic-подобной памяти (§Q5).

### Q10. Более безопасный первый эксперимент

Три дешёвых эксперимента. Их можно вести параллельно, и ни один не меняет архитектуру:

- **E0 — census на текущем `main` (1–3 дня, только инструментирование).**
  1. *Hazard census:* на каждом `ObtainBuffer`/upload записывать `last_gpu_tick` по 64 KiB блокам.
     В uffd/signal fault handler (`page_manager.cpp:340-406`, `InvalidateMemory`/`ReadMemory`)
     считать доступы CPU к блокам с `!scheduler.IsFree(last_gpu_tick)`: запись — WAR, чтение
     GPU-written — RAW. Классифицировать по VA-alias'у (другой VA тот же PA), VMA type и
     **PS4 memory type** (WB_ONION/WC_GARLIC, `PhysicalMemoryArea::memory_type`). Гонять в
     `Disabled` и в `Precise`.
  2. *Shareability census:* для каждого GPU-используемого 64 KiB блока определить класс:
     `{direct-importable (непрерывен и сравним mod 64K); stitchable (memfd-backed, но
     фрагментирован или не сравним); partially unmapped; без memfd-backing; exact alias; shifted
     или overlapping alias}`. Отдельно посчитать **стоимость**: сколько сшитых объектов понадобилось
     бы, если сшивать по VMA-прогонам, а не по блокам, и как часто они инвалидируются remap'ами.
     Пока E0b не показал реальную стоимость сшивки, долю «shareable» как kill-критерий не
     использовать.
  3. *VMM churn:* map/unmap/remap/pool commit/decommit в секунду для GPU-visible диапазонов.

  Это даёт числа, которые решают судьбу проекта: сколько трафика Bloodborne *вообще* можно сделать
  shared, сколько гонок откроет shared при текущих fence'ах, дёшев ли rebind.

- **E0b — standalone Vulkan probe (1–2 дня), на обеих машинах.** Одна программа, без shadPS4:
  список расширений и фич; `memoryTypeBits` sparse-буфера с `VkExternalMemoryBufferCreateInfo` для
  `HOST_ALLOCATION` и `DMA_BUF`; `vkGetMemoryHostPointerPropertiesEXT` для (a) anon `mmap`,
  (b) `MAP_SHARED` memfd; udmabuf → `vkGetMemoryFdPropertiesKHR`; пересечения; sparse alignment;
  `sparseResidencyAliased`; `residencyNonResidentStrict`; `minImportedHostPointerAlignment`. Затем
  функциональный тест: bind импортированного чанка в sparse arena **в два offset'а**, BDA,
  compute-шейдер читает A и пишет B, CPU проверяет через memfd-mapping **по третьему VA** (alias).
  Шаги 3–5 плана сжимаются в эту одну программу. Добавить (по review Sol):
  - `UDMABUF_CREATE_LIST`: 4×16 KiB из разнесённых PA → один 64 KiB блок; PA offset, не
    выровненный на 64K;
  - overlapping aliases: два сшитых dma-buf с общими страницами, запись через один, **barrier**,
    чтение через другой, и тот же сценарий без barrier;
  - доступ CPU через исходный memfd-mapping;
  - на AMD проверить, что swiotlb не используется (`dmesg`, debugfs swiotlb);
  - стоимость submit в зависимости от числа импортированных BO (1 / 100 / 1 000 / 10 000);
  - чтение `size_limit_mb` и `list_limit` в runtime.
  Главная машина для E0b — обе, 3300U в том числе: производительность здесь не нужна.

- **E1 — fence-at-completion на текущем mirrored backend.** Задерживать label writes и EOP/EOS/
  ReleaseMem IRQ до завершения host tick'а. Примитив — `Scheduler::DeferPriorityOperation`
  (`vk_scheduler.h:440-446`): у него есть свой поток, который ждёт timeline semaphore
  (`vk_scheduler.cpp:251-273`). **Не** `DeferOperation`: он выполняется только на submit или
  `PopPendingOperations` (`vk_scheduler.cpp:134-141`). Если CP простаивает, ожидая guest'а, а guest
  ждёт label, label не запишется никогда. После каждого отложенного сигнала нужен flush, иначе tick
  не будет отправлен на GPU. CP-side WaitRegMem разрешать консервативно и логировать каждый путь
  разрешения (§5). Критерий — корректность: WAR/RAW в census ≈0, без регрессий. Производительность
  **измеряется с разложением** (§5), но не является kill-критерием. E1 полезен и без shared memory:
  даёт честные fence'ы вместо эвристик #3404.

---

## 3. Проверенные факты о драйверах и ядре

### 3.1 AMD: host-pointer import не может импортировать память shadPS4

- shadPS4 на Linux: `memfd_create("BackingDmem", 0)` + `mmap(MAP_SHARED)`
  (`address_space.cpp:719-736`). Guest VA — это `mmap(MAP_SHARED|MAP_FIXED, backing_fd, phys_addr)`
  (`address_space.cpp:752-756`).
- RADV `radv_amdgpu_winsys_bo_from_ptr` → `ac_drm_create_bo_from_user_mem` →
  libdrm `amdgpu_create_bo_from_user_mem`. Флаги:
  `AMDGPU_GEM_USERPTR_ANONONLY | AMDGPU_GEM_USERPTR_REGISTER | AMDGPU_GEM_USERPTR_VALIDATE`
  (libdrm `amdgpu/amdgpu_bo.c`).
- Ядро, `amdgpu_ttm.c`: `if ((gtt->userflags & AMDGPU_GEM_USERPTR_ANONONLY) && vma->vm_file)
  return -EPERM`. У memfd и у `MAP_SHARED|MAP_ANONYMOUS` (shmem) `vm_file != NULL`.
- RADV кладёт host-pointer import ровно в **один** memory type: первый GTT без WC
  (`radv_GetMemoryHostPointerPropertiesEXT`).

### 3.2 AMD: почему userptr вообще плох для этой задачи

`amdgpu_hmm_invalidate_gfx` на любую MMU-invalidation userptr-диапазона (munmap, mprotect,
uffd-wp — все они идут через `change_protection` с mmu notifier) делает
`dma_resv_wait_timeout(..., MAX_SCHEDULE_TIMEOUT)`: **CPU-поток блокируется до завершения всей GPU
работы над BO**. Затем BO revalidate'ится на следующем CS. С одним большим импортом и page-protection
tracking'ом это stall'ы и лишние walk'и. udmabuf/dma-buf-путь этим не страдает: страницы pinned,
notifier'а нет.

### 3.3 Гранулярность sparse и 16 KiB guest

- `block_size = max(sparse alignment, 16 KiB)` (`buffer_cache.cpp:70`). RADV:
  `RADV_SPARSE_BUFFER_ALIGNMENT = 64 KiB`. NVIDIA: проверить `reqs.alignment` (обычно 64 KiB).
- Guest выделяет VA с шагом 16 KiB по умолчанию (`memory.cpp:581`). Flexible PA-куски по 16 KiB.
- VUID-09491 требует кратности 64 KiB для `resourceOffset`, `memoryOffset` и `size` **в
  `VkDeviceMemory`**. Отсюда три класса блоков:
  1. **Bulk import** (крупные PA-чанки `backing_base` или udmabuf по PA): блок бинжится, только если
     весь 64 KiB VA-блок отображён на непрерывный PA и `(VA − PA) mod 64 KiB == 0`. Объектов мало,
     remap'ы стоят только rebind.
  2. **Stitched import:**
     - AMD: `UDMABUF_CREATE_LIST` принимает до `list_limit = 1024` элементов с 4 KiB-выровненными
       `offset/size` из любых memfd. Проверок пересечения нет (`udmabuf.c:350-420`), так что один
       dma-buf может собрать произвольную раскладку, а разные dma-buf могут делить страницы.
     - NVIDIA: host import guest-VA диапазона, который уже «сшит» mmap'ами guest'а.
     Цена: (a) импорт привязан к mapping'у, каждый remap → новый объект; (b) число BO. На RADV
     импортированные BO не бывают «always valid», их надо проверять на каждом CS, так что стоимость
     submit растёт с их числом (измерить в E0b); (c) лимит размера udmabuf (§3.5). Сшивать разумно
     по VMA-прогонам, а не по одному блоку.
  3. **Невозможные:** VMA без memfd (Code/Stack/anon) → mirrored. Частично unmapped блоки можно
     закрыть «жертвенной» страницей, но GPU-записи в дыры попадут туда же. Для PRT-чтений нужны
     нули.
- **Overlapping aliases через разные `VkDeviceMemory` находятся вне модели Vulkan.**
  `SPARSE_ALIASED` покрывает один `VkDeviceMemory`, забинженный в несколько мест, а не два объекта с
  общими физическими страницами. Корректность держится на global memory barrier между записью через
  один alias и доступом через другой плюс на аппаратной когерентности. Нужен функциональный тест, а
  при провале — fallback в mirrored.

### 3.4 Sparse, external memory и placed map: правила спецификации

Проверено по registry `validusage.json`:
- `VUID-VkSparseMemoryBind-memory-02731` — handle type импортированной памяти должен быть в
  `VkExternalMemoryBufferCreateInfo::handleTypes` ресурса;
- `VUID-VkSparseMemoryBind-resourceOffset-09491` — кратность alignment;
- `VUID-VkBufferCreateInfo-flags-00917` — `SPARSE_ALIASED` требует `sparseResidencyAliased`;
- `VUID-vkMapMemory-memory-00678` / `VUID-VkMemoryMapInfo-memory-07958` — одна host-mapping на
  `VkDeviceMemory`;
- `VUID-VkMemoryMapInfo-flags-09571/09572` — без `memoryMapRangePlaced` placed map только целиком;
- `VUID-VkMemoryMapInfo-flags-09575` — placed map нельзя для host-imported памяти.

### 3.5 udmabuf (ядро, `drivers/dma-buf/udmabuf.c`)

- Требует shmem или hugetlb memfd, `F_SEAL_SHRINK` и **отсутствие** `F_SEAL_WRITE`.
- Страницы pin'ятся через `memfd_pin_folios`.
- **Лимит размера:**
  - `size_limit_mb = 64` во **всех релизных ядрах по v7.2 включительно** (проверено на тегах v7.1 и
    v7.2).
  - `INT_MAX` появился только в цикле 7.3 (commit `44e9eb5a7621`, «dma-buf/udmabuf: Disable the size
    limit by default»; mainline сейчас 7.3-rc4).
  - По данным Sol, уже есть patch, предлагающий вернуть лимит на 256 MiB (я его не проверял).
  - `list_limit = 1024`.
  - Вывод: проектировать под чанки ≤ 64 MiB, а реальные значения читать в runtime из
    `/sys/module/udmabuf/parameters/`.
- **CPU sync.** `DMA_BUF_IOCTL_SYNC` → `begin/end_cpu_udmabuf` синхронизирует
  **собственную** sg-таблицу udmabuf, смапленную для его misc device
  (`ubuf->device->this_device`). Mapping importer'а (amdgpu) создаётся в `map_udmabuf` с
  `DMA_ATTR_SKIP_CPU_SYNC` (так и в v7.2), и ioctl его не касается. На x86 (coherent DMA) обе
  операции — no-op. Поэтому A/B-тест «с ioctl и без» на x86 ничего не покажет. Реальные условия
  когерентности:
  - importer DMA-coherent;
  - нет swiotlb bounce;
  - GPU PTE snooped (§Q6.3).
  Их и надо проверять в E0b, плюс стресс-тест на гонки.
- Для shadPS4 это значит: `memfd_create(..., MFD_ALLOW_SEALING)` (под `#ifdef __linux__`, в коде
  есть предупреждение про FreeBSD) и `fcntl(F_ADD_SEALS, F_SEAL_SHRINK)`.

### 3.6 Что осталось непроверенным (закрывается E0b)

- NVIDIA: пересечение `memoryTypeBits` (sparse + external) с sysmem типами; наличие и поведение
  `VK_EXT_external_memory_host` и `VK_EXT_external_memory_dma_buf` на вашей версии драйвера; import
  shmem-страниц; `sparseResidencyAliased`; стоимость `vkQueueBindSparse`. На Blackwell используется
  open kernel module, так что поведение pin/import можно при необходимости читать в исходниках.
- AMD 3300U:
  - доступ к `/dev/udmabuf`;
  - GTT/TTM-лимиты при 5+ GiB импортированной памяти: `mem_info_gtt_total`; на свежих ядрах
    TTM-лимит по умолчанию порядка половины RAM — проверить, как считаются imported/pinned страницы;
  - объём RAM машины: Bloodborne + pinned guest memory;
  - bandwidth GPU при snooped (cached GTT) и non-snooped (WC GTT) доступе.

---

## 4. Замечание о PS4 memory types — бесплатная подсказка для policy

PS4 guest сам сообщает свою модель когерентности: `ORBIS_KERNEL_WB_ONION = 0` (CPU cached, GPU через
coherent шину) и `ORBIS_KERNEL_WC_GARLIC = 3` (CPU write-combined, GPU non-coherent, быстрый).
shadPS4 хранит это в `PhysicalMemoryArea::memory_type` (`core/memory.h:73`).

- **Onion** — это диапазоны, где PS4 *сам* гарантирует CPU↔GPU handoff. Они первые кандидаты на
  shared. Если E0 покажет, что конфликтные диапазоны Bloodborne — Onion, то «shared только для Onion»
  — гораздо меньший проект.
- **Garlic** на dGPU разумно оставить mirrored в VRAM, а позже, возможно, перенести в ReBAR. На UMA
  его можно шарить через non-snooped (WC) память. Это прямой аналог PS4.
- Raven (3300U) повторяет ту же дихотомию: cached GTT ≈ Onion (snooped), WC GTT ≈ Garlic. Поэтому
  3300U — хороший семантический стенд.

---

## 5. Как сделать fence'ы точными без эвристик #3404

- **Production-цель — порядок пакетов guest'а переносится в порядок host timeline** (поправка Sol).
  - CP-записи, которые потребляет GPU (WriteData, DmaData destination), выполняются **на GPU
    timeline в позиции пакета**: `vkCmdUpdateBuffer`/copy между соответствующими draw'ами. Для
    семантики EOP/ReleaseMem перед записью нужен end-of-pipe barrier (`ALL_COMMANDS`), иначе запись
    обгонит предыдущие draw'ы.
  - Сигналы, которые наблюдает CPU (labels, IRQ, equeue), становятся видимы при завершении точки,
    в которой они стоят. Для этого после каждого такого сигнала нужна граница submission/tick и
    `DeferPriorityOperation`.
  - «Всё выполнить в конце batch'а» — лишь консервативный вариант для E1, не финальная модель.
- **CP-side WaitRegMem.** Порядок разрешения:
  1. условие уже выполнено в памяти → продолжить;
  2. его выполнит pending-запись из уже записанного потока → продолжить, но **вставить full
     pipeline barrier**: одна Vulkan queue даёт порядок submission, а не порядок выполнения;
  3. иначе writer — шейдер или guest CPU → submit и host wait, затем перепроверка; если условие всё
     ещё не выполнено, ждать guest CPU как сейчас, не держа неотправленную работу.
  Шаг 2 — **гипотеза, а не закон**. Семантику разных engine/queue, ABA и условия с масками нужно
  доказать на PoC. В E1 логировать каждый путь разрешения. Нужен и oracle-режим: дополнительно
  дождаться host completion и проверить, что память действительно удовлетворяет условию.
- **Flip/VO labels** (`videoout/driver.cpp:236-273`): тоже убедиться, что guest видит flip только
  после host completion кадра.
- **Unmap/remap GPU-visible диапазона:** host GPU должен дойти до последнего tick'а, использовавшего
  блок, перед rebind. Карантин PA невозможен, потому что guest выбирает PA сам.
- Это ровно тот объём работы, которого #3404 пытался избежать эвристикой. E1 даёт его цену в числах.
- **Разложение стоимости в E1** (вместо kill-критерия). Нужно отделить цену точной семантики от цены
  примитивной реализации:
  - *fence latency* — время от разбора EOP до host completion, распределение;
  - *guest wait* — сколько guest-потоки реально ждут labels и GfxEop equeue (это неустранимая часть:
    guest теперь видит настоящую задержку host GPU);
  - *overhead реализации* — лишние submit'ы, барьеры, CPU-время CP.
  Приговор выносить только по guest wait при хорошей реализации.

---

## 6. Предлагаемый порядок экспериментов

Практический порядок (согласован с review Sol): **E0b → E0 → E1 → E2 → E3 → E4 → E5 → E6**. E0b и E0
независимы, их можно вести параллельно.

| # | Эксперимент | Где | Критерий успеха | Kill / pivot критерий |
|---|---|---|---|---|
| **E0b** | Standalone Vulkan probe + функциональный bind/alias/BDA тест, включая stitched udmabuf и overlapping aliases (§Q10). Реализован: `tools/e0b_uma_probe/` | **обе** машины с первого дня | на каждом вендоре найден механизм: пересечение типов, bind, alias, CPU↔GPU через третий VA; измерена стоимость submit от числа BO | NVIDIA: пустое пересечение → план B (BDA-only + VA-import) или только AMD |
| **E0** | Census: hazards (WAR/RAW vs host timeline), shareability по новой классификации, стоимость сшивки, VMM churn, memory types | `main` + счётчики, RTX | числа есть, воспроизводимы (pad replay) | нет, пока E0b не дал стоимость сшивки |
| **E1** | Fence-at-completion на mirrored backend (`DeferPriorityOperation` + flush) | `main`, RTX, затем 3300U | WAR/RAW ≈ 0; нет deadlock; нет регрессий корректности; стоимость разложена (§5) | не kill. Решение — только по неустранимому guest wait при хорошей реализации |
| **E2** | Baseline perf (вместо шага 1): p50/p95/p99, submits, `Finish()` count/time, upload/download bytes, fault counts, binds | `main` и `main+E1` | стабильная дисперсия на N≥10 прогонах | — |
| **E3** | Минимальная интеграция: shared policy только для shareable + Onion блоков; PA import table; VMM→bind с ожиданием; arenas с external + `SPARSE_ALIASED`; mirror-машинерия **не пишет** в shared | ветка от upstream, 3300U и RTX | identity checker зелёный; alias/remap matrix (§Q7) зелёная | — |
| **E4** | Выключить upload/download для shared-блоков; hazard tracker как диагностика | то же | 0 lost writes, 0 hazards, 0 copy bytes для shared | — |
| **E5** | Bloodborne death-loop на RTX (sysmem/PCIe) и затем на 3300U | обе | нет vertex corruption после N death/load циклов | — |
| **E6** | Только теперь — граница абстракции и refactor | — | — | — |

Шаг 2 плана (refactor) переезжает в E6. Шаги 3–5 сливаются в E0b. Шаг 9 (AMD) частично переезжает
в E0b, а целиком идёт параллельно E5.

**Отладочный инструмент, который стоит сделать в E3: binding identity checker.** Периодически, в
точке GPU idle, compute-шейдер через BDA хэширует N случайных shared 64 KiB блоков. CPU хэширует те
же блоки через guest VA **и** через каждый alias VA. Любое расхождение — ошибка identity (не тот PA,
устаревший import, неправильный rebind). Проверка не зависит от семантики игры.

---

## 7. Instrumentation (для E0/E2)

Куда вешать счётчики (в `DebugState` уже есть `num_batches_per_frame`):

- `FlushSyncBatch` (`buffer_cache.cpp:429`): bytes/ranges uploaded, batches per frame.
- `DownloadMemory` (`buffer_cache.cpp:131`): bytes, число вызовов, время в `scheduler.Finish()`.
- **Все** вызовы `scheduler.Finish()` и `Wait()`: число и суммарное время за кадр. Это главный
  скрытый cost.
- `EnsureResident` (`buffer_cache.cpp:284`): binds, allocated bytes; `GetArena` migrations.
- `FaultManager::ProcessFaultBuffer`: число fault-блоков за кадр.
- uffd handler (`page_manager.cpp:394`): write faults; signal handler: read faults. Для каждого fault
  класс hazard'а из E0.
- Stream buffer bytes (`ObtainBuffer` fast path).
- VMM: map/unmap/protect/pool commit/decommit по GPU-visible диапазонам.
- Submits per frame, bindSparse per frame.

Воспроизводимость: в fork `main` есть guest pad record/replay (`79866f2`). Upstream его не содержит,
его нужно cherry-pick'нуть в экспериментальную ветку. Потоковую недетерминированность он не убирает,
поэтому сравнивать распределения по N≥10 прогонам (например, Mann-Whitney на per-frame times), а не
средние.

---

## 8. Ревизия 2: что изменилось после review Sol

Принято:
- **Ограничение `VA ≡ PA (mod 64 KiB)` было слишком сильным.** Оно верно только для bulk-импорта
  PA-чанков. `UDMABUF_CREATE_LIST` (AMD) и import guest-VA (NVIDIA) его снимают. Уточнение от меня:
  цена сшивки — число объектов, re-import на remap и стоимость BO list на submit. Поэтому сшивать
  по VMA-прогонам. Overlapping aliases через разные `VkDeviceMemory` — вне модели Vulkan (§3.3).
- **Kill-критерии E0 (доля shareable) и E1 (цена) убраны.** E1 — эксперимент на корректность с
  разложением стоимости (§5).
- **Production-модель — порядок пакетов на host timeline**, а не «всё в конце batch'а». Правило
  PendingWrites для WaitRegMem — гипотеза, её надо доказать в PoC (§5).
- **`HOST_COHERENT` — неверное формальное обоснование** CPU-когерентности memfd-mapping'а (§Q6.3).
- **Лимит udmabuf нельзя закладывать в дизайн**, его надо читать в runtime (§3.5).
- **3300U — рабочая машина уже для E0b.**

Уточнено или оспорено:
- **`DMA_BUF_IOCTL_SYNC`** на udmabuf синхронизирует только собственный misc-device mapping udmabuf.
  Mapping importer'а создаётся с `DMA_ATTR_SKIP_CPU_SYNC`. A/B-тест «с ioctl и без» на x86 поэтому
  неинформативен. Проверять нужно swiotlb, snooped PTE (amdgpu для foreign dma-buf ставит
  `ttm_cached` → `AMDGPU_PTE_SNOOPED`) и стресс-гонки (§3.5).
- **Лимит 64 MiB — не «некоторые snapshots»**, а все релизные ядра по v7.2. `INT_MAX` есть только в
  7.3-rc.
- **Для E1 нужен `DeferPriorityOperation`, а не `DeferOperation`.** Второй выполняется только на
  submit или `PopPendingOperations`, и при простое CP label не будет записан никогда.

---

## 9. Источники

- Upstream shadPS4 `e4ca349`: файлы, указанные выше.
- #2819 (исторические commit'ы `52253b45` «Import memory», `87b37712` «Removed host buffers»):
  https://github.com/shadps4-emu/shadPS4/pull/2819
- #3404: https://github.com/shadps4-emu/shadPS4/pull/3404
- #5047: https://github.com/shadps4-emu/shadPS4/pull/5047
- #5100: https://github.com/shadps4-emu/shadPS4/pull/5100
- libdrm `amdgpu/amdgpu_bo.c` (`amdgpu_create_bo_from_user_mem`): https://gitlab.freedesktop.org/mesa/libdrm
- Mesa RADV (`radv_buffer.c`, `radv_device.c`, `radv_physical_device.c`, `radv_formats.c`,
  `winsys/amdgpu/radv_amdgpu_bo.c`, `radv_constants.h`, `ac_gpu_info.c`): https://gitlab.freedesktop.org/mesa/mesa
- Linux `drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c`, `amdgpu_hmm.c`, `amdgpu_dma_buf.c`,
  `drivers/dma-buf/udmabuf.c` (mainline 7.3-rc4 и теги v7.1/v7.2; commit `44e9eb5a7621`):
  https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
- Vulkan registry `validusage.json` (KhronosGroup/Vulkan-Headers).
- Vita3K memory mapping (BDA + external host): https://github.com/Vita3K/Vita3K/pull/2272
- A. Sawicki, «Vulkan Memory Types on PC and How to Use Them»:
  https://asawicki.info/news_1740_vulkan_memory_types_on_pc_and_how_to_use_them
