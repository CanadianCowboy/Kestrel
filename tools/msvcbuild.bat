@echo off
rem Builds and tests Kestrel on Windows with MSVC, Ninja and Qt.
rem
rem   tools\msvcbuild.bat              build core + tests (no Qt)
rem   tools\msvcbuild.bat ui           build everything, including the UI
rem   tools\msvcbuild.bat test         build everything and run ctest
rem   tools\msvcbuild.bat coretest     build without Qt and run ctest
rem   tools\msvcbuild.bat clean        wipe the build directory first
rem
rem Every machine-specific path comes from the environment, so this runs
rem unchanged somewhere else. Set any of these to override:
rem
rem   KESTREL_VCVARS     full path to vcvars64.bat
rem   KESTREL_QT_PREFIX  the Qt msvc2022_64 prefix
rem   KESTREL_CMAKE_ARGS extra -D options, space separated
rem
rem The last one is how a backend gets turned on without editing this file. A
rem GGUF build is a different binary from a mock one, so the option belongs to
rem the invocation rather than to the script:
rem
rem   set KESTREL_CMAKE_ARGS=-DKESTREL_LLAMA_CPP_ROOT=D:/kestrel-deps/llama.cpp
rem   tools\msvcbuild.bat test
rem
rem The Qt prefix is passed as a cache variable rather than left to the
rem environment. The environment works for the main Qt6 package, but an optional
rem module such as TextToSpeech resolves its own dependencies while being
rem found, and those lookups need the prefix in a place CMake consults
rem unconditionally.

rem Everything below runs in this shell, so the vcvars settings have to survive
rem into the cmake calls; setlocal alone does that, and the matching endlocal
rem restores the caller's environment on the way out.
setlocal

rem --- Visual Studio -------------------------------------------------------
rem vswhere is the supported way to ask which Visual Studio is installed, and
rem asking beats naming: a hardcoded install path is wrong on a machine with a
rem different edition, a different year, or a Build Tools install in a
rem different root. Only a machine without vswhere falls back to a path, and
rem that fallback is this machine's -- hence the override.
set "KESTREL_VSPATH="
if "%KESTREL_VCVARS%"=="" for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "KESTREL_VSPATH=%%i"
if "%KESTREL_VSPATH%"=="" (
    echo    looking for the Visual Studio C++ toolchain
) else (
    echo    using Visual Studio at %KESTREL_VSPATH%
)
if "%KESTREL_VCVARS%"=="" if not "%KESTREL_VSPATH%"=="" set "KESTREL_VCVARS=%KESTREL_VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if "%KESTREL_VCVARS%"=="" set "KESTREL_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%KESTREL_VCVARS%" (
    echo    FAILED: no Visual Studio C++ toolchain at
    echo            "%KESTREL_VCVARS%"
    echo            Set KESTREL_VCVARS to the vcvars64.bat of your install.
    exit /b 1
)
call "%KESTREL_VCVARS%"
if errorlevel 1 exit /b 1

rem --- Qt -------------------------------------------------------------------
if "%KESTREL_QT_PREFIX%"=="" set "KESTREL_QT_PREFIX=C:\Qt\6.9.0\msvc2022_64"
if not exist "%KESTREL_QT_PREFIX%\bin\cmake.exe" if not exist "%KESTREL_QT_PREFIX%\lib\cmake" (
    echo    FAILED: no Qt at "%KESTREL_QT_PREFIX%"
    echo            Set KESTREL_QT_PREFIX to your Qt msvc2022_64 prefix.
    exit /b 1
)

set "MODE=%~1"
if "%MODE%"=="" set "MODE=core"

set "BUILD_DIR=build"
set "UI_FLAG=-DKESTREL_BUILD_UI=OFF"
set "QT_FLAG=-DCMAKE_PREFIX_PATH=%KESTREL_QT_PREFIX%"

if /I "%MODE%"=="ui" set "UI_FLAG=-DKESTREL_BUILD_UI=ON"
if /I "%MODE%"=="test" set "UI_FLAG=-DKESTREL_BUILD_UI=ON"

if /I "%MODE%"=="clean" (
    rmdir /s /q "%BUILD_DIR%" 2>NUL
    set "MODE=core"
)

cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Debug %UI_FLAG% %QT_FLAG% %KESTREL_CMAKE_ARGS% || exit /b 1
cmake --build "%BUILD_DIR%" || exit /b 1

if /I "%MODE%"=="test" ctest --test-dir "%BUILD_DIR%" -C Debug --output-on-failure || exit /b 1
if /I "%MODE%"=="coretest" ctest --test-dir "%BUILD_DIR%" -C Debug --output-on-failure || exit /b 1

endlocal
