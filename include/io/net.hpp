#pragma once
#include "io/socket_t.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <atomic>

namespace io {

using CloseCallback = std::function<void(socket_t socket)>;
using WriteCallback = std::function<void(socket_t socket, size_t bytes_written)>;
using ReadCallback = std::function<void(socket_t socket, char *buffer, size_t bytes_received)>;
using AcceptCallback = std::function<void(socket_t socket)>;   // уведомление о новом сокете (accept/connect)
using UserEventCallback = std::function<void(uint32_t value)>; // пользовательское событие из post()

struct NetCallbacks {
	CloseCallback on_close;	   // событие закрытия сокета
	WriteCallback on_write;	   // событие физической записи в сокет
	ReadCallback on_read;	   // событие чтения данных
	AcceptCallback on_accept;  // новое входящее (или исходящее) соединение
	UserEventCallback on_user; // пользовательское событие post(uint32_t)
};

// Конфигурация авто-тюнинга глубины параллельных AcceptEx (только IOCP).
// Каждые window_ms оценивается количество принятых соединений за окно и подстраивается глубина:
// если >= high_watermark — увеличиваем на up_step; если <= low_watermark — уменьшаем на down_step.
// Глубина ограничена [min_depth, max_depth]. При уменьшении, если aggressive_cancel_on_downscale=true,
// лишние AcceptEx отменяются немедленно (CancelIoEx).
struct AcceptAutotuneConfig {
	bool enabled{true};
	uint32_t window_ms{200};
	uint32_t min_depth{1};
	uint32_t max_depth{64};
	uint32_t up_step{1};
	uint32_t down_step{1};
	uint32_t high_watermark{8};
	uint32_t low_watermark{2};
	bool aggressive_cancel_on_downscale{true};
};

class INetEngine {
  public:
	virtual ~INetEngine() = default;

	virtual bool init(const NetCallbacks &cbs) = 0;
	virtual void destroy() = 0;

	// Однократная обработка событий. Возвращает true, если было обработано
	// хотя бы одно событие, иначе false. По умолчанию no-op, движки могут
	// переопределить для inline-режима без внутренних потоков.
	virtual bool loop_once(uint32_t /*timeout_ms*/) { return false; }

	// Запустить цикл обработки событий в текущем потоке.
	// wait_ms семантика:
	//  < 0  — бесконечное ожидание (блокирующее);
	//  == 0 — неблокирующий опрос (busy-poll);
	//  > 0  — ожидание событий не более указанного числа миллисекунд.
	// По умолчанию делегирует в event_loop(), который можно переопределить в бэкенде.
	virtual void start(int32_t wait_ms = -1) {
		loop_running_.store(true, std::memory_order_relaxed);
		event_loop(loop_running_, wait_ms);
	}

	// Остановить цикл, запущенный start(). Безопасно вызывать из callback’ов.
	// Вызывает wake() для немедленного пробуждения блокирующих ожиданий внутри event_loop().
	virtual void stop() {
		loop_running_.store(false, std::memory_order_relaxed);
		wake();
	}

	// Новый универсальный цикл обработки событий. Бэкенд должен блокироваться
	// на нативном ожидании (epoll_wait/kevent/io_uring_wait_cqe/GQCS и т.д.)
	// и периодически проверять run_flag.load(). Значение wait_ms см. выше.
	// Реализация по умолчанию совместима со старыми бэкендами через loop_once().
	virtual void event_loop(std::atomic<bool> &run_flag, int32_t wait_ms) {
		const uint32_t inf = 0xFFFFFFFFu;
		const uint32_t tm = (wait_ms < 0) ? inf : static_cast<uint32_t>(wait_ms);
		while (run_flag.load(std::memory_order_relaxed)) {
			(void)loop_once(tm);
		}
	}

	virtual bool add_socket(socket_t socket, char *buffer, size_t buffer_size, ReadCallback cb) = 0;
	virtual bool delete_socket(socket_t socket) = 0;

	// Устанавливает соединение на уже созданном сокете (добавленном через add_socket)
	// Если async=false — выполняет блокирующий connect() в вызывающем потоке,
	// затем переводит сокет в неблокирующий режим и настраивает наблюдение.
	// Если async=true — выполняет неблокирующую попытку connect и отслеживает завершение в event loop.
	virtual bool connect(socket_t socket, const char *host, uint16_t port, bool async) = 0;
	virtual bool disconnect(socket_t socket) = 0;

	virtual bool accept(socket_t listen_socket, bool async, uint32_t max_connections) = 0;

	// Отправка данных с поддержкой ожидания готовности на запись в event loop
	virtual bool write(socket_t socket, const char *data, size_t data_size) = 0;

	// Проброс произвольного событийного числа uint32_t в поток событий
	virtual bool post(uint32_t user_event_value) = 0;

	// Установить (или снять) таймаут ожидания входящих данных для сокета.
	// Если за timeout_ms миллисекунд не пришло ни одного байта, сокет закрывается
	// и вызывается on_close. Значение 0 отключает таймаут.
	virtual bool set_read_timeout(socket_t socket, uint32_t timeout_ms) = 0;

	// Приостановить генерацию событий чтения для сокета и возобновить их позже.
	// На платформах с масками интересов (epoll/event ports/devpoll) — убираем POLLIN/EPOLLIN/эквивалент;
	// на kqueue используем EV_DISABLE/EV_ENABLE; на io_uring/IOCP — больше не постим новые read-операции
	// (в IOCP дополнительно отменяем висящий WSARecv).
	virtual bool pause_read(socket_t socket) = 0;
	virtual bool resume_read(socket_t socket) = 0;

	// Настроить глубину параллельных accept-операций для listen-сокета (только IOCP).
	// Для других движков — no-op и возвращает true. Значение >=1, 0 будет трактоваться как 1.
	virtual bool set_accept_depth(socket_t listen_socket, uint32_t depth) = 0;

	// Расширенная версия: позволяет указать, нужно ли агрессивно уменьшать глубину
	// (отменой лишних уже поставленных AcceptEx). Для IOCP: если aggressive_cancel=true
	// и новая глубина меньше текущего числа outstanding accept’ов — будет вызван CancelIoEx
	// для части висящих AcceptEx. Для других движков — эквивалент set_accept_depth.
	virtual bool set_accept_depth_ex(socket_t listen_socket, uint32_t depth, bool aggressive_cancel) = 0;

	// Авто-тюнинг глубины AcceptEx по темпам входящих соединений (только IOCP).
	// Повторные вызовы обновляют конфигурацию. Если cfg.enabled=false — авто-тюнинг отключается.
	virtual bool set_accept_autotune(socket_t listen_socket, const AcceptAutotuneConfig &cfg) = 0;
protected:
	// Флаг выполнения цикла start/stop по умолчанию.
	std::atomic<bool> loop_running_{false};

	// Метод пробуждения блокирующего ожидания внутри event_loop(). Бэкенды
	// должны переопределить его (например, записью в eventfd/pipe, PostQueuedCompletionStatus, и т.п.).
	virtual void wake() {}
};

// Фабрика выбирает реализацию в зависимости от ОС/движка
INetEngine *create_engine();

} // namespace io

// Platform helpers shared across backends
namespace io {
// Suppress SIGPIPE for the current process (POSIX). Safe to call multiple times.
void suppress_sigpipe_once();

// Diagnostics: count of broken pipes (EPIPE/ECONNRESET on write)
void record_broken_pipe();
uint64_t broken_pipe_count();
void reset_broken_pipe_count();
}
