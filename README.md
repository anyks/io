# io

[![CI](https://github.com/anyks/io/actions/workflows/ci.yml/badge.svg)](https://github.com/anyks/io/actions/workflows/ci.yml)

Кроссплатформенная библиотека событийного ввода-вывода и сетевого цикла на C++17 с несколькими бэкендами (kqueue/macOS, epoll/Linux, io_uring/Linux, IOCP/Windows, event ports/devpoll на Solaris). Библиотека предоставляет единый API для асинхронных операций, набор тестов (GoogleTest) и готовые пресеты санитайзеров (ASan/TSan/UBSan).

Ключевые возможности
- Асинхронные connect/accept/read/write через единый интерфейс INetEngine
- Коллбэки on_accept/on_read/on_write/on_close/on_user
- Таймаут ожидания входящих данных на сокете (per-socket read idle timeout)
- Пауза/возобновление чтения per-socket (pause_read/resume_read)
- Пользовательские события post(uint32_t)
- Логирование завершения connect (SO_ERROR) во всех бэкендах в Debug-сборке
- Дополнительно (Windows/IOCP): настройка глубины AcceptEx и авто-тюнинг

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

Включение расширенного логирования
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

## Мини-пример

```cpp
using io::socket_t;
io::NetCallbacks cbs{};
cbs.on_accept = [](socket_t fd){ /* новый сокет */ };
cbs.on_read = [&](socket_t s, char* b, size_t n){ /* обработать */ };
cbs.on_close = [](socket_t s){};
std::unique_ptr<io::INetEngine> eng(io::create_engine());
eng->init(cbs);

socket_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
if (listen_fd == io::kInvalidSocket) { /* handle error */ }
// bind/listen ...
eng->accept(listen_fd, true, 1024);

socket_t cli = ::socket(AF_INET, SOCK_STREAM, 0);
static char buf[4096];
eng->add_socket(cli, buf, sizeof(buf), cbs.on_read);
eng->connect(cli, "127.0.0.1", 12345, true);
eng->set_read_timeout(cli, 5000); // 5s idle таймаут
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

## Заметки для Solaris (event ports/devpoll)

- По умолчанию используется Event Ports (`IO_WITH_EVENTPORTS=ON`). При недоступности можно переключиться на `/dev/poll`: `-DIO_WITH_EVENTPORTS=OFF`.
- Сборка (SunOS):
	```bash
	cmake -S . -B build/sol -DCMAKE_BUILD_TYPE=Debug -DIO_WITH_EVENTPORTS=ON
	cmake --build build/sol -j
	ctest --test-dir build/sol --output-on-failure
	```
- Таймауты чтения:
	- Event Ports: таймеры через `timer_create` + `SIGEV_PORT` (событие приходит в порт).
	- /dev/poll: эмуляция через pipe и отдельный поток-таймер per-socket.
- Логирование SO_ERROR при завершении connect включено в обоих бэкендах (Debug).

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
- Solaris /dev/poll: таймеры чтения эмулируются через pipe и фоновые потоки per-socket; это влияет на масштабируемость при очень большом числе тайм-аутов.
- Логирование: диагностические логи (включая SO_ERROR при завершении connect) активны только в Debug‑сборках; для Release см. раздел «Включение расширенного логирования».
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
