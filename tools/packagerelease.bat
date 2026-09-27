@echo off
rem Configures and builds the Release configuration into build-release.
rem
rem Release rather than the usual Debug because the Debug CRT (msvcp140d.dll,
rem ucrtbased.dll) is not redistributable and does not ship with Windows, so a
rem Debug build cannot be made to work on a machine without Visual Studio --
rem which is exactly the machine a desktop copy is for.
rem
rem KESTREL_BUILD_TESTS=OFF because the suites assert(), and a Release build
rem defines NDEBUG: the assertions compile away and the test files are then
rem full of unused locals. The suites are run against Debug, which is where
rem they mean anything.
rem
rem Machine-specific paths come from the environment. Set KESTREL_VCVARS or
rem KESTREL_QT_PREFIX to override; see tools\msvcbuild.bat for how each is
rem resolved.
rem
rem KESTREL_LLAMA_CPP_ROOT is read by CMakeLists.txt and is not passed here on
rem purpose: the same variable then configures the Debug build, the tests and
rem this one, so there is a single place to point at a llama.cpp tree instead of
rem three command lines that can disagree.

setlocal

set "KESTREL_VSPATH="
if "%KESTREL_VCVARS%"=="" for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "KESTREL_VSPATH=%%i"
if "%KESTREL_VCVARS%"=="" if not "%KESTREL_VSPATH%"=="" set "KESTREL_VCVARS=%KESTREL_VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if "%KESTREL_VCVARS%"=="" set "KESTREL_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%KESTREL_VCVARS%" (
    echo    FAILED: no Visual Studio C++ toolchain at "%KESTREL_VCVARS%"
    echo            Set KESTREL_VCVARS to the vcvars64.bat of your install.
    exit /b 1
)
call "%KESTREL_VCVARS%"
if errorlevel 1 exit /b 1

if "%KESTREL_QT_PREFIX%"=="" set "KESTREL_QT_PREFIX=C:\Qt\6.9.0\msvc2022_64"

cd /d "%~dp0.."
rem KESTREL_CMAKE_ARGS is the same passthrough tools\msvcbuild.bat takes, and
rem it is the only way to point this at a llama.cpp or ONNX Runtime GenAI tree
rem without editing the script. Without it a release build silently configures
rem with no inference backend at all: CMake enables the backends, finds no root,
rem and produces a kestrel.exe that links nothing -- which then fails the
rem packager's own check with a message about a missing DLL rather than a
rem missing root, and sends you looking in the wrong place entirely.
cmake -S . -B build-release -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DKESTREL_BUILD_UI=ON ^
      -DKESTREL_BUILD_TESTS=OFF ^
      -DCMAKE_PREFIX_PATH="%KESTREL_QT_PREFIX%" ^
      %KESTREL_CMAKE_ARGS% || exit /b 1
cmake --build build-release || exit /b 1

endlocal
