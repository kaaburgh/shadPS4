<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# e0b_uma_probe — эксперимент E0b

Отдельная программа, не связанная со сборкой shadPS4. Проверяет на конкретной машине, может ли
guest-память shadPS4 быть **одной физической памятью** для guest CPU и Vulkan GPU внутри sparse
arena в стиле #5047. Отвечает на вопросы §3 и §Q10 документа
`documents/Research/uma-shared-buffer-backend-review.md` (ревизия 2).

Программа воспроизводит модель памяти shadPS4 на Linux (`core/address_space.cpp`):
- вся guest «физическая» память — один `memfd`;
- `backing_base` — его каноническое `MAP_SHARED`-отображение;
- guest VA — это `mmap(MAP_SHARED|MAP_FIXED)` того же memfd по PA-смещению, поэтому один PA
  может жить на нескольких VA (aliases);
- GPU видит память через sparse arena (`SPARSE_BINDING|SPARSE_RESIDENCY`, BDA). Доступ идёт двумя
  путями: через **BDA** (как DMA-путь shadPS4) и через **storage descriptor** на смещении arena
  (как `ObtainBuffer()`).

Одно отличие от shadPS4: memfd создаётся с `MFD_ALLOW_SEALING` и запечатывается `F_SEAL_SHRINK`.
Без этого udmabuf не работает.

## Сборка и запуск

Нужны cmake ≥ 3.20, компилятор C++20, заголовки и loader Vulkan, заголовки ядра
(`linux/udmabuf.h`).

| дистрибутив | пакеты |
|---|---|
| Arch | `cmake vulkan-headers vulkan-icd-loader linux-api-headers` (+ `vulkan-tools`, `vulkan-validation-layers` по желанию) |
| Fedora | `cmake vulkan-headers vulkan-loader-devel kernel-headers` (+ `vulkan-tools`, `vulkan-validation-layers`) |
| Ubuntu/Debian | `cmake libvulkan-dev linux-libc-dev` (+ `vulkan-tools`, `vulkan-validationlayers`) |

```sh
cd tools/e0b_uma_probe
./run_e0b.sh                 # сборка + факты о системе + прогон -> e0b-results-<host>-<time>.tar.gz
E0B_VALIDATE=1 ./run_e0b.sh  # плюс короткий прогон с validation layers, если они установлены
```

Пришлите получившийся `.tar.gz` с каждой машины. Прогон занимает несколько минут. Пиковый объём
pinned-памяти — около 300 MiB.

Полезные опции (`./build/e0b_uma_probe --help`):
- `--list-devices`, `--device N`. По умолчанию выбирается дискретный GPU, потом интегрированный.
- `--tests caps,t1,t2,t3,t4,t5,t6,t7` — прогнать только часть тестов.
- `--t6-max 1024` — если T6 долго идёт на 3300U.

### Подготовка udmabuf (обе машины)

Без `/dev/udmabuf` все `dmabuf_*` тесты уйдут в UNSUPPORTED. Проверьте так:

```sh
ls -l /dev/udmabuf || sudo modprobe udmabuf
sudo setfacl -m u:$USER:rw /dev/udmabuf    # доступ до перезагрузки; либо добавить себя в группу-владельца (часто kvm)
cat /sys/module/udmabuf/parameters/size_limit_mb   # 64 во всех релизных ядрах по 7.2; самый большой dma-buf в тестах — 1 MiB
```

### Особенности машин

- **Ryzen 3 PRO 3300U / Vega (RADV).** Нужен именно RADV, а не AMDVLK: проверьте `driverName` в
  отчёте или задайте `AMD_VULKAN_ICD=RADV`. Sparse в RADV включён для GFX8+, Raven подходит.
- **RTX 5070 Ti.** Номер драйвера попадёт в отчёт. Если в системе есть ещё и iGPU, проверьте
  `device.name` в логе или задайте `--device`.

## Какие backend'ы проверяются

| backend | что это | зачем |
|---|---|---|
| `host_bulk` | `VK_EXT_external_memory_host` над чанком **канонического** memfd-отображения | основной путь для NVIDIA; на RADV ожидается отказ (`AMDGPU_GEM_USERPTR_ANONONLY` → `-EPERM`) |
| `host_gva` | host-import **guest VA** диапазона: guest-mmap'ы уже «сшили» 16 KiB куски | сшивка на NVIDIA без udmabuf; импорт привязан к mapping'у |
| `dmabuf_bulk` | udmabuf над одним PA-диапазоном → `VK_EXT_external_memory_dma_buf` | основной кандидат для AMD |
| `dmabuf_list` | `UDMABUF_CREATE_LIST` из разбросанных 16 KiB кусков | сшивка из review Sol |
| `vk_export` | Vulkan-память, экспортированная как dma-buf/opaque fd и `mmap`'нутая в guest VA | вариант с «перевёрнутым владением» из таблицы Q5 |
| `vk_hostvisible` | обычная HOST_VISIBLE Vulkan-память в arena, CPU-вид через `vkMapMemory` | принимает ли sparse arena системную память вообще; контроль логики тестов |
| `device_local` | обычная device-local память (как shadPS4 сейчас) | контроль для T6/T7 |

Для imported/exported памяти arena создаётся с `VkExternalMemoryBufferCreateInfo` нужного типа
(VUID-VkSparseMemoryBind-memory-02731) и с `SPARSE_ALIASED`, если есть `sparseResidencyAliased`.

## Что отвечает каждый тест

| тест | вопрос | как читать |
|---|---|---|
| **caps** | Что драйвер обещает до передачи данных | Главная строка: `host[memfd_canonical(backing_base)]` — результат import и `sparse-compatible types`. Пустое пересечение (`0x0 {}`) = sparse не принимает этот вид памяти. Также `sparse[...]` (alignment/типы для arena с external handle), `extbuf[...]`, `dmabuf[...]`, `export_pool` |
| **T1** `bulk_import+sparse_alias` | Один PA-диапазон забинжен в arena по **двум** VA (sparse alias). CPU пишет через VA1, GPU читает через VA2, GPU пишет, CPU читает через VA2 и через канонический mapping | PASS = фундамент M2 для этого механизма. Проверки: `cpu->gpu`, `gpu->gpu(alias)`, `gpu->cpu(...)` |
| **T1b** `plain_buffer_import(planB)` | То же без sparse: обычный VkBuffer поверх import | Если T1 UNSUPPORTED, а T1b PASS, остаётся «план B»: BDA-таблица указывает на per-import буферы |
| **T2** `stitched_scattered` | 64 KiB из четырёх разбросанных, несравнимых PA-кусков по 16 KiB (`host_gva`, `dmabuf_list`), плюс `(planB)` | Проверяет, снимает ли сшивка ограничение `VA ≡ PA (mod 64K)` |
| **T2** `noncongruent_contiguous` | Непрерывный PA со сдвигом 16 KiB относительно 64 KiB | Bulk-импорт, начатый с этого PA, тоже решает задачу: отсчёт `memoryOffset` идёт от начала импорта |
| **T3** `overlap_alias_barrier` | Два разных `VkDeviceMemory`, делящих страницы (сдвинутые aliases). Запись через первый, barrier, чтение через второй | Вне модели Vulkan, поэтому проверяется функционально. `no_barrier` — только INFO |
| **T4** `coherence_stress` | 256 случайных CPU→GPU→CPU циклов **без** flush/invalidate. Для udmabuf есть вариант с `DMA_BUF_IOCTL_SYNC` | Вердикт о когерентности дают только варианты **без** ioctl, на x86 ожидается PASS. Вариант `+DMA_BUF_IOCTL_SYNC` всегда INFO, а если ioctl возвращает ошибки — UNSUPPORTED: ioctl синхронизирует только собственный mapping udmabuf и доказательством не считается |
| **T5** `remap_identity_timeline` | Блок arena перебинживается на другой PA. `vkQueueBindSparse` ждёт timeline предыдущей работы, следующая работа ждёт bind | Проверяет упорядочивание rebind, которого нет в #5047 (там residency только растёт). Метрики: время вызовов bind |
| **T6** `bo_scaling` | Время submit+wait при N = 1…4096 импортированных объектов (и device-local для сравнения). N ограничен `maxMemoryAllocationCount` с запасом (факт `T6.max_objects`) | На RADV каждая импортированная память — в глобальном BO-списке каждого submit. Рост `*_submit_wait_median_us` с N означает, что сшивать нужно по VMA, а не по блоку. Упор в лимит (число allocations, pinned memory, fd) — INFO, большие N — SKIP |
| **T7** `nonresident_read` | Что возвращает чтение незабинженного блока | Для PRT-семантики (дыры должны читаться нулями) |

Статусы:
- **PASS** — данные совпали.
- **FAIL** — данные не совпали, это реальная проблема корректности. Проверки GPU засчитывают и
  «шейдер обработал не все dword'ы», так что тихо не выполнившийся bind не даёт ложного PASS.
- **UNSUPPORTED** — это ответ, а не ошибка программы: драйвер, ядро или права этого не дают.
  Причина — в `detail`.
- **ERROR** — неожиданная ошибка API, результат неизвестен.
- **INFO** — измерение.

Коды выхода probe: 0 — нет FAIL и ERROR; 1 — есть FAIL или ERROR (с `--validate` любая ошибка
validation layers тоже считается ERROR); 2 — ошибка настройки (нет устройства и т.п.).
`run_e0b.sh` сначала всегда собирает tarball, затем возвращает код probe. Если упал только
validation-прогон (`E0B_VALIDATE=1`), скрипт возвращает 3.

Host-pointer import выполняется, только если указатель и размер кратны runtime
`minImportedHostPointerAlignment`. Иначе вариант получает UNSUPPORTED с причиной, и вызов Vulkan
не делается.

## Plan-B тесты: обычный импортированный VkBuffer (T4b/T5b/T6b)

На RTX 5070 Ti (driver 595.91.07) sparse arena принимает только memory type {1}, а host import
даёт {2,3}. Поэтому shared-путь там возможен только через **обычный** VkBuffer поверх импорта
(T1b/T2/T3 `(planB)` проходят). T4b/T5b/T6b проверяют этот путь глубже.

Тесты не зависят от механизма импорта. Они работают через интерфейс `Importer`
(`src/planb.cpp`): «импортировать то, что сейчас отображено в guest VA `[va, va+len)`».
Реализаций две:
- `host` — `VK_EXT_external_memory_host` на guest VA указатель (NVIDIA);
- `dmabuf` — udmabuf над memfd-кусками, которые отображены по этому VA (берутся из таблицы
  отображений `GuestMemory`), плюс `VK_EXT_external_memory_dma_buf` (путь для AMD).

Новый механизм добавляется ещё одним `Importer`, логику тестов при этом переписывать не нужно.

| тест | что проверяет | как читать |
|---|---|---|
| **T4b** `planb_coherence_stress` | `--t4-iters` случайных циклов на каждый путь (BDA и SSBO): CPU пишет через guest VA → GPU сверяет и пишет через импортированный буфер → CPU сверяет через alias VA и канонический mapping. Без staging-копий и без flush/invalidate | PASS = все mismatch-счётчики и «gpu dwords not checked» равны нулю |
| **T5b** `planb_remap_identity` | Guest VA отображает backing A → import#1 → GPU читает A и пишет в него → host ждёт timeline и fence → import#1 уничтожается → тот же VA перемапливается на B → import#2 → GPU должен видеть B, ни одного stale dword из A, запись должна попасть в B, а A остаться нетронутым | Каждый шаг пишется в лог (`# T5b ... step N (+µs)`) и в JSON (`lifecycle: ...`) |
| **T6b** `planb_import_scaling` | N = 1/16/128/1024/2048/4096 импортов по 64 KiB (лимит тот же, что в T6: `T6b.max_objects`). Отдельно: `create_import_*` (буфер + import + bind); `first_submit_wait_after_import_us` (первый submit после импортов, может включать разовую работу по residency); steady state `empty_*` / `tiny_dispatch_*` (медианы после прогрева); `touch_all_*` (по одному dispatch на каждый буфер в одном submit); `destroy_*` | INFO. FAIL только если какой-то буфер не получил запись из touch-all. Упор в лимит ресурсов — INFO, большие N — SKIP |

Проверено на lavapipe с validation layers: 0 ошибок, T4b/T5b PASS для обоих importer'ов, T6b
INFO. Проведён и негативный контроль: если после remap оставить старый dma-buf import,
T5b падает по всем проверкам фазы 2 (stale A виден, B не виден). Оговорка: host import в lavapipe
не pin'ит страницы и читает CPU VA напрямую, поэтому на lavapipe устаревший host import
«видит» B. Проверить устаревание host import можно только на реальном драйвере. Протокол T5b
(дождаться GPU → destroy → remap → новый import) — единственный допустимый по спецификации.

## Ожидания (гипотезы, которые проверяются)

| | RTX 5070 Ti (NVIDIA) | 3300U (RADV) |
|---|---|---|
| host import memfd | вероятно OK | **отказ** (ANONONLY) |
| sparse ∩ sysmem типы | **главная неизвестная** | ожидается непусто |
| udmabuf import | неизвестно | ожидается OK |
| `vk_export` mmap | вероятно нет (opaque fd не mmap'ится) | dma-buf скорее всего mmap'ится |
| T4 без sync | PASS (x86 snooping) | PASS (snooped PTE для foreign dma-buf) |

## Самопроверка на lavapipe

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./build/e0b_uma_probe --allow-cpu --validate --selftest-memfd-as-dmabuf
```

lavapipe реализует sparse bind через `mmap` собственного memfd, поэтому imported и exported память
на нём в arena не бинжится. Такие тесты там помечаются UNSUPPORTED. Логику тестов проверяют
`vk_hostvisible` (T1, T4, T5) и `(planB)`-варианты T1b, T2, T3: на lavapipe все они PASS, ошибок
validation ноль. `--selftest-memfd-as-dmabuf` подсовывает memfd вместо настоящего udmabuf. Это
нужно только для самопроверки, на реальных машинах этот флаг не использовать.

## Что E0b не проверяет

- Скорость GPU-доступа к системной памяти (PCIe и snooped-доступ на APU). Это отдельный
  микробенчмарк.
- Гонки guest-потоков с ещё не завершённой работой GPU и точность fence'ов. Это E0 и E1 на
  самом shadPS4.
- Texture/image путь.
- Всё, кроме Linux x86-64.

## Регенерация шейдеров

SPIR-V лежит в `src/spirv_shaders.h`. После правки `shaders/*.comp` выполните
`shaders/regen_spirv.sh` (нужны glslangValidator и, по желанию, spirv-val).
