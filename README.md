# io

[![CI](https://github.com/anyks/io/actions/workflows/ci.yml/badge.svg)](https://github.com/anyks/io/actions/workflows/ci.yml)
[![Linux](https://img.shields.io/badge/Linux-tested-22c55e?logo=linux)](https://github.com/anyks/io/actions/workflows/ci.yml)
[![macOS](https://img.shields.io/badge/macOS-tested-22c55e?logo=apple)](https://github.com/anyks/io/actions/workflows/ci.yml)
[![Windows IOCP](https://img.shields.io/badge/Windows-IOCP%20WIP-f59e0b?logo=windows)](#заметки-для-windows-iocp)

Кроссплатформенная библиотека событийного ввода-вывода и сетевого цикла на C++17 с несколькими бэкендами (kqueue/macOS, epoll/Linux, io_uring/Linux, IOCP/Windows, event ports/devpoll на Solaris). Библиотека предоставляет единый API для асинхронных операций, набор тестов (GoogleTest) и готовые пресеты санитайзеров (ASan/TSan/UBSan).

Ключевые возможности
- Асинхронные connect/accept/read/write через единый интерфейс INetEngine
- Коллбэки on_accept/on_read/on_write/on_close/on_user
- Таймаут ожидания входящих данных на сокете (per-socket read idle timeout)
- Пауза/возобновление чтения per-socket (pause_read/resume_read)
- Пользовательские события post(uint32_t)
- Логирование завершения connect (SO_ERROR) во всех бэкендах в Debug-сборке
- Дополнительно (Windows/IOCP): настройка глубины AcceptEx и авто-тюнинг

### Поведение при закрытом peer (SIGPIPE/EPIPE)

- На POSIX-платформах библиотека подавляет SIGPIPE один раз на процесс внутри инициализации бэкенда, чтобы запись в уже закрытый peer не завершала процесс. Для этого используется утилита `io::suppress_sigpipe_once()`.
- В write‑пути при ошибках `EPIPE`/`ECONNRESET` библиотека единообразно:
	- логирует событие в Debug‑сборках,
	- снимает интерес к записи (EPOLLOUT/EVFILT_WRITE/POLLOUT) и очищает `want_write`,
	- ожидает дальнейшее закрытие по стандартным событиям (ERR/HUP/RDHUP) без лишнего кручения цикла.

Диагностика «сломанных записей» (см. ниже) позволяет оценить частоту таких ситуаций при нагрузке.

## Быстрый старт

```bash
# Конфигурация/сборка/тесты (Debug)
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug

# Примеры: сервер и клиент (в двух терминалах)
./build/debug/example_server
./build/debug/example_client

# Санитайзеры
cmake --build --preset asan && ctest --preset asan
cmake --build --preset tsan && ctest --preset tsan
cmake --build --preset ubsan && ctest --preset ubsan
```

## Требования
- CMake >= 3.16
- Компилятор с поддержкой C++17
- Поддерживаемые ОС: macOS, Linux, Windows, Solaris (event ports/devpoll)
- Для io_uring: установленная liburing (если включено IO_WITH_IOURING)

## Сборка

Используя CMake Presets:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Или вручную:

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
ctest --output-on-failure
```

Доступные пресеты (см. CMakePresets.json)
- debug: Debug + тесты/примеры включены
- release: Release + тесты/примеры включены
- asan: Debug + AddressSanitizer
- tsan: Debug + ThreadSanitizer
- ubsan: Debug + UndefinedBehaviorSanitizer

Полезные опции сборки (см. CMakeLists.txt)
- IO_BUILD_TESTS=ON/OFF — собирать тесты (по умолчанию ON)
- IO_BUILD_EXAMPLES=ON/OFF — собирать примеры (по умолчанию ON)
- IO_WITH_IOURING=ON/OFF — Linux: использовать io_uring (по умолчанию OFF)
- IO_WITH_EVENTPORTS=ON/OFF — Solaris: event ports или fallback devpoll (по умолчанию ON)
- IO_ENABLE_ASAN/TSAN/UBSAN=ON — включить соответствующий санитайзер
- IO_MAX_CONNECTIONS=N — ограничить количество одновременных соединений (0 = без ограничений)
- Windows/IOCP: IO_IOCP_ACCEPT_DEPTH=N — целевая глубина параллельных AcceptEx (дефолт 4)

## Установка

```bash
cmake --install build/release --prefix ./_install
```

## Структура
- include/io/net.hpp — публичный сетевой API (INetEngine, NetCallbacks)
- src/net_kqueue.cpp — реализация для kqueue (macOS/*BSD)
- src/net_epoll.cpp — реализация для epoll (Linux)
- src/net_iouring.cpp — реализация для io_uring (Linux, требуется liburing)
- src/net_iocp.cpp — реализация IOCP (Windows)
- src/net_eventports.cpp — реализация Event Ports (Solaris)
- src/net_devpoll.cpp — реализация /dev/poll (Solaris, fallback)
- src/net_factory.cpp — фабрика создания движка
- tests/*.cpp — интеграционные и нагрузочные тесты (GoogleTest)
- CMakeLists.txt, CMakePresets.json — конфигурация сборки

## Пакетирование
Включён базовый CPack (TGZ). При необходимости добавим DEB/RPM/PKG.

Примечание: краткий «контекст для переноса» с инструкциями для Linux и сводкой последних изменений доступен в `docs/CONTEXT.md`.

## API: ключевые методы
- add_socket(fd, buf, size, on_read): регистрирует сокет, буфер чтения и коллбэк
- connect(fd, host, port, async): устанавливает соединение на уже добавленном сокете; async=true — неблокирующий режим
- accept(listen_fd, async, max_connections): асинхронный accept с ограничением числа одновременных соединений
- write(fd, data, size): неблокирующая отправка с доотправкой в event loop
- post(value): пользовательское событие, доставляется в on_user FIFO
- set_read_timeout(fd, timeout_ms): таймаут ожидания входящих данных; если за timeout_ms не пришло ни байта — сокет закрывается и вызывается on_close; 0 отключает таймаут
- pause_read(fd) / resume_read(fd): временно отключить/включить события чтения для сокета

Дополнительно (только IOCP/Windows):
- set_accept_depth(listen_fd, depth): настраивает целевую глубину параллельных AcceptEx; по умолчанию задаётся опцией сборки IO_IOCP_ACCEPT_DEPTH (дефолт 4)
- set_accept_depth_ex(listen_fd, depth, aggressive_cancel): расширенная версия; при aggressive_cancel=true уменьшает текущую глубину немедленно, отменяя лишние незавершённые AcceptEx
- set_accept_autotune(listen_fd, cfg): включает авто-тюнинг глубины AcceptEx на основе количества принятых соединений в скользящем окне времени. Пороговые значения и шаги обновления задаются в AcceptAutotuneConfig.

Примечание: set_read_timeout реализован во всех бэкендах (kqueue/epoll/io_uring/IOCP).

Логирование (Debug)
- В Debug-сборке включены диагностические логи (отключаются с NDEBUG).
- Во всех бэкендах логируется завершение connect (результат getsockopt(SO_ERROR) или эквивалент), что помогает при отладке сетевых флапов.

Расширенное логирование kqueue (macOS/*BSD)
- Доступна дополнительная детализация событий kqueue (EVFILT_READ/WRITE/TIMER/USER) через флаг сборки `IO_ENABLE_KQUEUE_VERBOSE`.
- По умолчанию флаг выключен (OFF), чтобы не засорять stderr.
- Включение:
	```bash
	# Вариант A: отдельный build-каталог
	cmake -S . -B build/debug-verbose -DIO_ENABLE_KQUEUE_VERBOSE=ON -DCMAKE_BUILD_TYPE=Debug
	cmake --build build/debug-verbose -j
	ctest --test-dir build/debug-verbose --output-on-failure

	# Вариант B: через пресет debug с добавлением опции
	cmake --preset debug -DIO_ENABLE_KQUEUE_VERBOSE=ON
	cmake --build --preset debug -j
	ctest --preset debug --output-on-failure
	```
- Где смотреть логи:
	- Стандартный поток ошибок (stderr) во время запуска примеров/тестов.
	- Префиксы строк: `[io/kqueue][DBG]` — отладочные события, `[io/kqueue][ERR]` — ошибки (активны в Debug).
- Что логируется дополнительно при `IO_ENABLE_KQUEUE_VERBOSE=ON`:
	- Количество событий за итерацию `loop_once`.
	- Поля kevent: `fd`, `filter`, `flags`, `fflags`, `data`.
	- Этапы завершения `connect` с `SO_ERROR`.
	- Принятие соединений (`accept: client fd=...`).

Включение расширенного логирования (общие рекомендации)
- По умолчанию логи активны в Debug-профиле:
	```bash
	cmake --preset debug
	cmake --build --preset debug
	```
- В других профилях (Release/RelWithDebInfo) логи отключены макросом NDEBUG. Если крайне необходимо включить их там, можно переопределить флаги компиляции, убрав NDEBUG. Например:
	```bash
	cmake -S . -B build/release-logs -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O3 -U NDEBUG"
	cmake --build build/release-logs -j
	```
	Замечание: это принудительный приём; рекомендуется использовать Debug/asan/tsan/ubsan для отладки.

### Диагностика: счётчик «broken pipe»

Для оценки частоты неудачных записей (EPIPE/ECONNRESET) доступен простой счётчик:

```cpp
// Обнулить перед сценарием
io::reset_broken_pipe_count();

// ... выполнить тест/нагрузку ...

// Прочитать значение
auto bp = io::broken_pipe_count();
std::fprintf(stderr, "broken-pipe count = %llu\n", static_cast<unsigned long long>(bp));
```

Счётчик инкрементируется из всех Unix‑бэкендов при перехвате `EPIPE`/`ECONNRESET` в `send()`.

## Мини-пример

```cpp
using io::socket_t;
io::NetCallbacks cbs{};
cbs.on_accept = [](socket_t fd){ /* новый сокет */ };
cbs.on_read = [&](socket_t s, char* b, size_t n){ /* обработать */ };
cbs.on_close = [](socket_t s){};
std::unique_ptr<io::INetEngine> eng(io::create_engine());
eng->init(cbs);

// (Необязательно) Диагностика: обнулить счётчик "broken pipe" перед сценарием
io::reset_broken_pipe_count();

socket_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
if (listen_fd == io::kInvalidSocket) { /* handle error */ }
// bind/listen ...
eng->accept(listen_fd, true, 1024);

socket_t cli = ::socket(AF_INET, SOCK_STREAM, 0);
static char buf[4096];
eng->add_socket(cli, buf, sizeof(buf), cbs.on_read);
eng->connect(cli, "127.0.0.1", 12345, true);
eng->set_read_timeout(cli, 5000); // 5s idle таймаут

// ... выполнить взаимодействие клиент↔сервер ...

// (Необязательно) Прочитать счётчик после сценария
auto bp = io::broken_pipe_count();
std::fprintf(stderr, "broken-pipe count = %llu\n", static_cast<unsigned long long>(bp));
```
Примечание про переносимость сокетов
- В API используется тип `socket_t`: на Windows это `SOCKET`, на POSIX — `int32_t`.
- Для проверки валидности сокета используйте `io::kInvalidSocket` вместо -1.

## Примеры: echo-клиент и echo-сервер

Протокол демонстрации: «кадрированная» передача — кадр = [4 байта длины в big-endian][payload]. Сервер отражает (echo) полученные кадры;
клиент отправляет 100 сообщений разной длины и проверяет, что все эхо-кадры пришли и совпадают побайтно.

Сборка примеров (в Debug уже включено):
```bash
cmake --preset debug
cmake --build --preset debug --target example_server example_client
```

Запуск:
```bash
# Терминал 1 — сервер (слушает 0.0.0.0:8080)
./build/debug/example_server

# Терминал 2 — клиент (подключается к 127.0.0.1:8080, шлёт 100 кадров и проверяет ответы)
./build/debug/example_client
```

Клиент завершится с кодом 0 при успехе и выведет `All 100 frames verified`.

## Запуск тестов и стресс под санитайзерами

Полный набор тестов:
```bash
ctest --preset debug
```

Быстрый запуск вручную из каталога сборки:
```bash
ctest --test-dir ./build/debug --output-on-failure -j4
```

Запуск подмножеств тестов по шаблону имени:
```bash
ctest --preset debug -R NetHighload
ctest --test-dir ./build/debug -R NetTimeout
```

Матрица санитайзеров (сборка + запуск):
```bash
cmake --build --preset asan && ctest --preset asan
cmake --build --preset ubsan && ctest --preset ubsan
cmake --build --preset tsan && ctest --preset tsan
```

TSan‑стресс одного теста (100 повторов, остановка на первом сбое):
```bash
ctest --test-dir ./build/tsan -R NetHighload.ManyClientsEchoNoBlock --repeat-until-fail 100 --output-on-failure -j1
```

## Трюки и полезные команды

- Запустить один тест по точному имени через ctest (с подробным выводом):
	```bash
	ctest --test-dir ./build/debug -R '^NetIntegration.ClientServerEchoAndPost$' -VV
	```

- Запустить тестовый бинарник напрямую с фильтром GoogleTest:
	```bash
	./build/debug/io_tests --gtest_filter=NetHighload.ManyClientsEchoNoBlock
	```

- Включить подробный вывод прогонов для всех тестов пресета:
	```bash
	ctest --preset debug -VV
	```

- Пересобрать только библиотеку и примеры:
	```bash
	cmake --build --preset debug --target io example_server example_client -j
	```

- Полностью пересоздать отладочную сборку:
	```bash
	rm -rf build/debug
	cmake --preset debug
	cmake --build --preset debug -j
	```

- Быстрая смена профиля на санитайзеры:
	```bash
	cmake --build --preset asan && ctest --preset asan
	cmake --build --preset tsan && ctest --preset tsan
	cmake --build --preset ubsan && ctest --preset ubsan
	```

## Диагностика других бэкендов (Linux/Windows/Solaris)

Ниже — практические подсказки по включению и интерпретации диагностики для остальных бэкендов. По умолчанию в Debug уже логируется завершение `connect` (через `SO_ERROR` или эквивалент). Для глубокой трассировки рекомендуется временно добавить `IO_LOG_DBG` в соответствующие места реализации.

### epoll (Linux)
- Где смотреть: `src/net_epoll.cpp`.
- Что полезно логировать дополнительно:
	- Результат `epoll_wait`: число событий за итерацию цикла.
	- По каждому событию: `fd`, маска (`EPOLLIN`/`EPOLLOUT`/`EPOLLERR`/`EPOLLHUP`), размеры очередей на запись.
	- Завершение `connect`: `getsockopt(SO_ERROR)` (уже логируется в Debug), переход к подписке на `EPOLLIN`.
	- Принятие соединений (`accept`) — `client fd` и перевод в неблокирующий режим.
- Советы:
	- Для тестов без фоновых потоков обязательно «помпите» цикл через `loop_once()` во время ожиданий (см. тесты `Net*`).
	- Для поиска горячих мест включите ASan/TSan: `--preset asan/tsan`.

### io_uring (Linux)
- Где смотреть: `src/net_iouring.cpp`.
- Что полезно логировать:
	- Отправка и завершение операций: SQE/CQE (поля `user_data`, `res`, `flags`).
	- `connect/accept/read/write` — постановка и завершение, размеры буферов, ошибки.
- Особенности:
	- Если `liburing` недоступна, CMake автоматически откатится на epoll (будет сообщение при конфигурации).
	- При отладке важно связать `user_data` с конкретным `fd`/операцией.

### IOCP (Windows)
- Где смотреть: `src/net_iocp.cpp`.
- Что полезно логировать:
	- Комлиты завершений: код статуса, число переданных байт, тип операции.
	- Ошибки Winsock: `WSAGetLastError()` и расшифровка.
	- Завершение `AcceptEx`/`ConnectEx` и последующая подписка на чтение.
- Полезные опции:
	- `-DIO_IOCP_ACCEPT_DEPTH=N` — целевая параллельная глубина AcceptEx (по умолчанию 4).

### Event Ports и /dev/poll (Solaris)
- Где смотреть: `src/net_eventports.cpp` и `src/net_devpoll.cpp`.
- Общая архитектура: один поток, `loop_once()` с таймаутом ближайшего idle‑дедлайна; переассоциация `fd` после обработки (one‑shot).
- Event Ports:
	- Ожидание: `port_getn(timeout_ms)`; пользовательские события через `port_send()`.
	- Логирование при `-DIO_ENABLE_EVENTPORTS_VERBOSE=ON`: количество событий, `portev_source/events/object`.
- /dev/poll (fallback):
	- Ожидание: `ioctl(DP_POLL)` с `dp_timeout`, равным ближайшему дедлайну.
	- Пользовательские события через внутреннюю трубу; логирование при `-DIO_ENABLE_DEVPOLL_VERBOSE=ON`.

### Общий чек‑лист отладки
- Убедитесь, что `add_socket()` вызывается до `connect()` — это гарантирует корректную доставку `on_read` после установления соединения.
- В тестах без фонового `event_loop()` используйте циклы с `loop_once()` вместо `sleep()` во время ожиданий.
- `set_read_timeout(fd, ms)` — это idle‑таймер: он переармируется после каждого успешного чтения.
- `pause_read()/resume_read()` должны сопровождаться включением/выключением интереса к чтению у конкретного бэкенда (kqueue: EV_ENABLE/EV_DISABLE; epoll: модификация интересов).
- При `EPIPE/ECONNRESET` в `send()` запись прекращается: счётчик broken‑pipe доступен через `io::broken_pipe_count()`.

## Заметки для Windows (IOCP)

- Бэкенд: IOCP (I/O Completion Ports). Тип сокета `socket_t` равен `SOCKET`. Фабрика `create_engine()` выберет IOCP автоматически.
- Сборка Visual Studio:
	```bash
	cmake -S . -B build/win-vs -G "Visual Studio 17 2022" -A x64
	cmake --build build/win-vs --config Debug --target io io_tests example_server example_client
	ctest --test-dir build/win-vs -C Debug --output-on-failure
	```
- Линковка: целевой таргет сам подтянет `ws2_32` и `Mswsock`.
- AcceptEx глубина: опция `-DIO_IOCP_ACCEPT_DEPTH=8` (по умолчанию 4). Доступны API `set_accept_depth`, `set_accept_depth_ex`, `set_accept_autotune`.
- Файрвол: при первом запуске сервера Windows может спросить разрешения для `example_server.exe`.
- Особенности отладки:
	- Для подробных логов используйте Debug-конфигурацию. Для включения логов в Release см. раздел про расширенное логирование.
	- Если запускаете в WSL — используйте Linux-сборку (epoll/io_uring); нативный IOCP доступен только в Windows.

### Быстрая локальная сборка (MSYS2 MinGW64, x86_64, bash)

Если у вас установлен MSYS2 и вы заходите по SSH сразу в bash (MSYS2 MinGW64), используйте обычные bash‑скрипты — всё как на Linux:

```bash
./scripts/windows/msys2/build.sh Debug
```

- По умолчанию вывод в `/e/io` (то есть `E:\io`). Переопределить можно переменной окружения `BUILD_WIN=/path`.
- Генератор выбирается автоматически: Ninja (если есть в PATH) или "MinGW Makefiles".
- Для Release: `./scripts/windows/msys2/build.sh Release`.
- Убедитесь, что установлены пакеты в MSYS2 MinGW64:
	- `pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja`

Примечание: Windows job в GitHub Actions можно игнорировать — локальная отладка через MSYS2 обычно быстрее.

#### Авто‑сборка по новым коммитам (bash‑watcher)

Чтобы не запускать сборку вручную, включите bash‑вотчер: каждые 15 секунд он подтягивает `origin/main` и при изменениях делает `reset --hard` и пересборку (в `/e/io`). Запустите:

```bash
./scripts/windows/msys2/watch_build.sh
```

Требования:
- Git в PATH;
- MSYS2 MinGW64 с пакетами toolchain/cmake/ninja (см. выше).

Замечание: вотчер выполняет `git reset --hard origin/main` — локальные незакоммиченные изменения будут потеряны.

Альтернатива (если у вас cmd по умолчанию): доступны эквивалентные .bat‑скрипты `scripts\windows\msys2\build.bat` и `watch_build.bat`.

## Заметки для Solaris (event ports/devpoll)

- По умолчанию используется Event Ports (`IO_WITH_EVENTPORTS=ON`). Переключение на `/dev/poll`: `-DIO_WITH_EVENTPORTS=OFF`.
- Сборка (SunOS):
	```bash
	cmake -S . -B build/sol -DCMAKE_BUILD_TYPE=Debug -DIO_WITH_EVENTPORTS=ON
	cmake --build build/sol -j
	ctest --test-dir build/sol --output-on-failure
	```
- Idle‑таймауты чтения в обоих бэкендах реализованы без фоновых потоков: ближайший дедлайн превращается в таймаут вызова ожидания (`port_getn` или `DP_POLL`), после каждого цикла просроченные соединения закрываются.
- Пользовательские события: Event Ports — `port_send()`; /dev/poll — внутренняя пользовательская труба.
- Диагностическое логирование можно расширить флагами: `IO_ENABLE_EVENTPORTS_VERBOSE` и `IO_ENABLE_DEVPOLL_VERBOSE`.

### Запуск стресс‑сценариев на Solaris

В репозитории есть готовые сценарии для удалённого запуска на Solaris‑хосте (настройки — `scripts/solaris/.env`). На GitHub Actions CI для Solaris намеренно не включён: используйте скрипты ниже для ручных (или cron) прогонов.

```bash
# Синхронизировать проект на удалённый Solaris
bash scripts/solaris/solaris_sync.sh

# Сконфигурировать и собрать
bash scripts/solaris/solaris_configure.sh
bash scripts/solaris/solaris_build.sh

# Последовательный стресс (повторы до 300 раз)
bash scripts/solaris/solaris_stress_highload.sh 300

# Параллельная фаза (4 инстанса) + стресс (200 повторов)
bash scripts/solaris/solaris_parallel_and_stress.sh 4 200
```

Логи тестов сохраняются в каталоге сборки (например, `build/sol/ctest_stress_highload.log`).

## Совместимость и ограничения

Минимальные требования по платформам (общее ориентировочное руководство)
- macOS/*BSD (kqueue): современные версии macOS и BSD-систем; kqueue является системным API (используется по умолчанию).
- Linux (epoll): ядро Linux 2.6+; epoll доступен в стандартных дистрибутивах.
- Linux (io_uring): ядро 5.1+ (рекомендуется 5.4+), требуется установленная liburing; при отсутствии — автоматический откат на epoll.
- Windows (IOCP): поддерживается на современных версиях Windows (рекомендуется Windows 10/Server 2016 и новее).
- Solaris: Event Ports (Solaris 10+) по умолчанию; при отключении или недоступности — fallback на /dev/poll.

Триггеринг и семантика событий
- epoll: используется level-triggered (EPOLLIN/EPOLLOUT без EPOLLET) — безопасно для пошаговой обработки.
- kqueue: EVFILT_READ/WRITE с включением/выключением (EV_ENABLE/EV_DISABLE) — поведение ближе к level-triggered.
- event ports/devpoll: семантика близка к level-triggered; после обработки дескриптор переассоциируется с актуальной маской интересов.
- io_uring: модель завершений асинхронных операций; чтения/записи/accept/connect постановляются и завершаются единичными событиями.

Известные ограничения и практические заметки
- Жизненный цикл буферов чтения: память, переданная в `add_socket`, должна оставаться валидной, пока сокет не будет удалён/закрыт.
- `connect` предполагает, что сокет уже добавлен через `add_socket` (для корректной доставки on_read после установления соединения).
- Таймаут чтения (`set_read_timeout`) — idle-таймер: закрывает сокет, если за заданный период не пришло ни одного байта. Это не таймаут «всего запроса/ответа».
- Backpressure на запись: `write` складывает данные в очередь и отправляет порциями по готовности. Большие bursts могут увеличивать задержки; контролируйте объёмы и проглатывайте подтверждения через on_write при необходимости.
- Авто‑тюнинг AcceptEx доступен только для IOCP (Windows); на других бэкендах вызовы возвращают `true`, но не меняют поведение.
- Solaris /dev/poll: таймеры чтения реализованы без фоновых потоков — через ближайший дедлайн и `DP_POLL` таймаут; поведение согласовано с Event Ports.

### Дополнительно о Solaris

Подробности архитектуры и диагностики см. в `docs/solaris.md`.
- Логирование: диагностические логи (включая SO_ERROR при завершении connect) активны только в Debug‑сборках; для Release см. раздел «Включение расширенного логирования».

## Релиз (tag → артефакты)

Готов релизный workflow (GitHub Actions), который собирает пакеты (CPack TGZ) на Linux/macOS/Windows и публикует их в GitHub Release при пуше тега `v*`.

Шаги:
```bash
# 1) Обновите CHANGELOG.md и при необходимости версию (docs)
# 2) Создайте тег и запушьте
git tag v0.1.0
git push origin v0.1.0

# 3) Дождитесь завершения workflow Release в Actions; пакеты будут прикреплены к релизу
```
## Сборка с io_uring (Linux)

Поддержка io_uring отключена по умолчанию; для включения:
1) Установите liburing (хедеры и библиотека). Примеры:
	- Debian/Ubuntu: `sudo apt-get install -y liburing-dev`
	- Fedora: `sudo dnf install -y liburing-devel`
2) Сконфигурируйте с опцией `IO_WITH_IOURING=ON`:
```bash
cmake -S . -B build/iouring -DIO_WITH_IOURING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/iouring -j
ctest --test-dir build/iouring --output-on-failure
```
Если liburing не найден, сборка автоматически откатится на epoll (будет предупреждение в выводе CMake).
