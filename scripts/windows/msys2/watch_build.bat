@echo off
setlocal ENABLEDELAYEDEXPANSION

REM Continuous watcher: pulls origin/main and builds on changes via MSYS2 MinGW64
REM Usage: double-click or run from repo root. Stop with Ctrl+C.

REM Ensure we're in repo root (this script resides under scripts\windows\msys2)
cd /d "%~dp0..\..\.."

REM Validate git
where git >nul 2>nul
if errorlevel 1 (
  echo [ERROR] git not found in PATH
  exit /b 1
)

REM Initial fetch
git fetch origin main
if errorlevel 1 (
  echo [ERROR] git fetch failed
  exit /b 1
)

echo [INFO] Starting watch loop. Building into E:\io on changes to origin/main

:loop
for /f %%H in ('git rev-parse HEAD') do set LOCAL=%%H
for /f %%H in ('git rev-parse origin/main') do set REMOTE=%%H

if not "%LOCAL%"=="%REMOTE%" (
  echo [INFO] Detected new commit: !LOCAL! -> !REMOTE!
  git reset --hard origin/main
  if errorlevel 1 (
    echo [WARN] git reset failed, retrying after delay
    timeout /t 10 >nul
    goto next
  )
  call scripts\windows\msys2\build.bat Debug
)

:next
REM Poll again after short delay
timeout /t 15 >nul
git fetch origin main >nul 2>nul
goto loop
