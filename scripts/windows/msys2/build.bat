@echo off
setlocal

REM Simple local build for MSYS2 MinGW64 on Windows (x86_64)
REM Usage: build.bat [Debug|Release]  (default: Debug)

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Debug"

set "MSYS=C:\msys64"
if not exist "%MSYS%\usr\bin\bash.exe" (
  echo [ERROR] MSYS2 not found at %MSYS%
  echo Install MSYS2 from https://www.msys2.org/ and ensure it is in C:\msys64
  exit /b 1
)

set "BUILD_WIN=E:\io"
if not exist "%BUILD_WIN%" (
  mkdir "%BUILD_WIN%" 2>nul
)

set "BASH=%MSYS%\usr\bin\bash.exe"
set "MSYSTEM=MINGW64"
set "CHERE_INVOKING=1"

echo [INFO] Building %CFG% into %BUILD_WIN%

"%BASH%" -lc "export PATH=/mingw64/bin:/usr/bin:$PATH; echo 'GCC:'; gcc --version || true; echo 'CMake:'; cmake --version || true; GEN=Ninja; command -v ninja >/dev/null 2>&1 || GEN='MinGW Makefiles'; echo Using generator: $GEN; BUILD=$(cygpath -u \"%BUILD_WIN%\"); cmake -S . -B \"$BUILD\" -G \"$GEN\" -DCMAKE_BUILD_TYPE=%CFG% -DIO_BUILD_TESTS=OFF -DIO_BUILD_EXAMPLES=ON"
if errorlevel 1 goto :error

"%BASH%" -lc "export PATH=/mingw64/bin:/usr/bin:$PATH; BUILD=$(cygpath -u \"%BUILD_WIN%\"); cmake --build \"$BUILD\" -j"
if errorlevel 1 goto :error

echo [OK] Build complete. Outputs in %BUILD_WIN%
exit /b 0

:error
echo [FAIL] Build failed. See output above.
exit /b 1
