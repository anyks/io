# Remote debug runners

Эти скрипты запускают сборку/тесты/стрессы на удалённых машинах по SSH без Git/CI.

- linux_run.sh — Linux (EPOLL и IO_URING)
- windows_run.sh — Windows (MSYS2 MinGW64)
- solaris_run.sh — Solaris (Event Ports и /dev/poll) — обёртка над scripts/solaris/*
- all.sh — последовательный прогон всех платформ

## Подготовка

1) Скопируй `.env.example` в `.env` и заполни хосты/пользователей/пути:

```
cp scripts/remote/.env.example scripts/remote/.env
```

2) Если ключ не в ssh-agent, укажи путь к приватному ключу (PEM/OpenSSH) в `SSH_KEY`.

3) Убедись, что на Linux установлен liburing-dev (для io_uring), на Windows — MSYS2 в `C:\\msys64`.

## Запуск

- Linux: `bash scripts/remote/linux_run.sh`
- Windows: `bash scripts/remote/windows_run.sh`
- Solaris: `bash scripts/remote/solaris_run.sh`
- Все: `bash scripts/remote/all.sh`

Скрипты упакуют логи стрессов и заберут их в текущую директорию.
