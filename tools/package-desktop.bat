@echo off
rem Assembles a self-contained copy of Kestrel on the Desktop:
rem
rem   Desktop\Kestrel\kestrel.exe          the Release build
rem   Desktop\Kestrel\*.dll, platforms\   windeployqt's Qt runtime
rem   Desktop\Kestrel\Kestrel\             the QML module directory (see below)
rem   Desktop\Kestrel\models\              where a GGUF is dropped to be found
rem   Desktop\Kestrel\.kestrel-voice\      the local neural voice
rem   Desktop\Kestrel\tools\               the Kokoro server script
rem
rem The layout is not arbitrary. LocalVoiceEngines::discover() looks in the
rem directory the executable is in and in its parent for exactly
rem .kestrel-voice/Scripts/python.exe, .kestrel-voice/models/kokoro-v1.0.onnx
rem and tools/kokoro_voice_server.py, so this arrangement is found without the
rem app being told anything -- no environment variable, no registry, no config.
rem
rem .kestrel-voice\piper is deliberately not copied. Nothing in the build reads
rem it; the app only mentions "piper" as a word that marks a neural voice in a
rem voice's name. It is 582 MB of dead weight.
rem
rem The piper *Python package* is excluded too, for the stronger reason. It is
rem GPL-3.0-or-later, and it is the one thing in the venv that drags a strong
rem copyleft into a package that is otherwise MIT, Apache-2.0, BSD and
rem zlib-licensed. Nothing imports it. See step 5 for how the exclusion is
rem applied to the site-packages copy.
rem
rem phonemizer-fork is GPL-3.0-or-later as well, and that one cannot be
rem dropped: kokoro-onnx requires it. It is left in, and THIRD-PARTY-NOTICES.md
rem says so in as many words, because shipping a copyleft component has
rem obligations attached and pretending otherwise helps nobody.
rem
rem Machine-specific paths come from the environment:
rem
rem   KESTREL_VCVARS     full path to vcvars64.bat
rem   KESTREL_QT_PREFIX  the Qt msvc2022_64 prefix
rem   KESTREL_STAGE      where to assemble it; defaults to a Kestrel folder on
rem                       the Desktop
rem
rem The voice itself is not downloaded by this script. .kestrel-voice has to
rem exist already, and the script says so rather than producing a package that
rem cannot speak.
rem
rem Neither is the language model. The models\ directory is created empty and
rem that is the whole installation step: drop a .gguf in it and the next launch
rem finds it without a command line, a setting or an environment variable. The
rem app picks the largest GGUF it finds there, because among quantisations of
rem different families the bigger file is the better model and the filename says
rem nothing about which is which.
rem
rem The llama.cpp runtime is different. It is linked into kestrel.exe, so a
rem package without it cannot start at all -- and it fails silently, before
rem main, with no window and no message. So the build stages it beside the
rem executable and this script refuses to ship a package where it is absent,
rem rather than handing over a folder that looks complete and does nothing.
rem
rem The ONNX Runtime GenAI runtime is the same case, and the more important one:
rem it is the backend the app prefers, so a package missing it has no working
rem local inference at all.

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
if "%KESTREL_STAGE%"=="" set "KESTREL_STAGE=%USERPROFILE%\OneDrive\Desktop\Kestrel"
set "QT_PREFIX=%KESTREL_QT_PREFIX%"
set "STAGE=%KESTREL_STAGE%"

rem Canonicalised rather than left as "%~dp0..": a path with ".." in the middle
rem is fine for cd and not reliably fine for call.
for %%I in ("%~dp0..") do set "REPO=%%~fI"

cd /d "%REPO%"
if errorlevel 1 exit /b 1

if not exist "%REPO%\.kestrel-voice\Scripts\python.exe" (
    echo    FAILED: %REPO%\.kestrel-voice is not there.
    echo            The local voice is a download, not a build product, so this
    echo            script assembles the copy but does not fetch it. See
    echo            README.md for how it is installed.
    exit /b 1
)

echo === 1/6 building Release ===
call "%REPO%\tools\packagerelease.bat" >nul
if not exist "%REPO%\build-release\kestrel.exe" (
    echo    FAILED: the Release build did not produce kestrel.exe
    exit /b 1
)

echo === 2/6 clearing %STAGE% ===
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
if errorlevel 1 exit /b 1

echo === 3/6 executable, QML module, Qt runtime ===
copy /y "%REPO%\build-release\kestrel.exe" "%STAGE%\" >nul || exit /b 1

rem The QML module directory. Not optional and not obvious: qt_add_qml_module
rem generates a qmldir for the build tree, and engine.loadFromModule finds the
rem module through it. Without this directory next to the executable the app
rem starts, fails to find its own scene, and exits with "Kestrel failed to load
rem its QML scene." The Main.qml inside it is a real file, not a link, and the
rem qmldir's "prefer :/qt/qml/Kestrel/" line makes Qt read the compiled-in copy
rem in preference to it.
if not exist "%REPO%\build-release\Kestrel\qmldir" (
    echo    FAILED: build-release\Kestrel\qmldir is missing
    exit /b 1
)
robocopy "%REPO%\build-release\Kestrel" "%STAGE%\Kestrel" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1

rem --no-compiler-runtime is asked for explicitly. windeployqt can deploy the
rem C++ runtime itself, but it is asked to deploy this app's Qt runtime, and
rem the compiler runtime is resolved from the toolchain that built the binary --
rem which is the answer below, already known, and correct.
"%QT_PREFIX%\bin\windeployqt.exe" --release --no-translations --no-compiler-runtime "%STAGE%\kestrel.exe" >nul || exit /b 1

rem windeployqt drops vc_redist.x64.exe next to the DLLs it copies. The three
rem DLLs are copied below, so the 18 MB installer is a second way to do a job
rem that is already done, and it is the kind of file a user double-clicks by
rem mistake.
del /q "%STAGE%\vc_redist.x64.exe" >nul 2>&1

rem windeployqt drops vc_redist.x64.exe next to the DLLs it copies. The three
rem DLLs are already there by then, so the 18 MB installer is a second way to do
rem a job that is done, and it is the kind of file a user double-clicks by
rem mistake.

rem windeployqt also leaves the compiler runtime to whatever is already
rem installed, which on a machine with Visual Studio is nothing to think about
rem and on a machine without it is a window that never opens. Copied
rem explicitly.
rem
rem From VCToolsRedistDir, which vcvars has already resolved for the toolchain
rem that built this binary. Asking the compiler where its own runtime lives is
rem the only version of this that is reliably right: searching the install tree
rem has to re-derive the same answer, and a search that took the newest match by
rem date picked the "arm" directory Visual Studio keeps beside "x64" for the
rem cross compiler -- an ARM msvcp140.dll beside an x64 executable, with no
rem vcruntime140_1.dll beside that at all, copied over the right one with no
rem error. The x64 subdirectory is named, and a directory only counts if it
rem actually holds the runtime.
set "CRT="
if defined VCToolsRedistDir for /f "delims=" %%d in ('dir /b /ad "%VCToolsRedistDir%\x64" 2^>nul') do (
    if not defined CRT if exist "%VCToolsRedistDir%\x64\%%d\vcruntime140_1.dll" set "CRT=%VCToolsRedistDir%\x64\%%d\"
)
if defined CRT (
    copy /y "%CRT%msvcp140.dll" "%STAGE%\" >nul || exit /b 1
    copy /y "%CRT%vcruntime140.dll" "%STAGE%\" >nul || exit /b 1
    copy /y "%CRT%vcruntime140_1.dll" "%STAGE%\" >nul || exit /b 1
)

rem And confirm the runtime is really there rather than trusting the copy. A
rem package missing it starts on this machine -- which has Visual Studio -- and
rem nowhere else, which is the failure this whole step exists to prevent, and
rem which is why it was silent the first time.
set "CRT_MISSING="
for %%d in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do (
    if not exist "%STAGE%\%%d" set "CRT_MISSING=%%d"
)
if not "%CRT_MISSING%"=="" (
    echo    FAILED: the compiler runtime is incomplete: %STAGE%\%CRT_MISSING% is missing
    echo            A copy without it does not start on a machine that has no
    echo            Visual Studio, which is the machine a desktop copy is for.
    exit /b 1
)
if defined CRT (echo    compiler runtime from %CRT%) else (echo    compiler runtime: already present)


rem windeployqt copies only the platform plugin the app actually runs on, so
rem the package gets qwindows.dll and nothing else. That leaves the display-less
rem review mode -- KESTREL_SCREENSHOT with QT_QPA_PLATFORM=offscreen, which is
rem how a UI change is checked on a machine with no screen -- unable to start at
rem all. Qt does not exit when a platform plugin is missing: it puts up a modal
rem "no Qt platform plugin could be initialized" dialog, so the process sits
rem there forever with no output and no file, which reads exactly like a hang in
rem the capture code rather than a packaging gap. Copied explicitly, and checked
rem so a missing Qt install fails the package rather than the reviewer.
if not exist "%QT_PREFIX%\plugins\platforms\qoffscreen.dll" (
    echo    FAILED: %QT_PREFIX%\plugins\platforms\qoffscreen.dll is missing
    exit /b 1
)
copy /y "%QT_PREFIX%\plugins\platforms\qoffscreen.dll" "%STAGE%\platforms\" >nul || exit /b 1

rem The native inference runtimes. CMakeLists stages these beside kestrel.exe in
rem the build tree, because that is where the linker resolves them from, and
rem kestrel.exe is copied to the stage root on its own -- so the staging does
rem not travel with it. They have to be copied explicitly or the package is an
rem app that dies at load time with no window.
rem
rem The ggml*.dll set is a wildcard rather than a fixed list because it changes
rem with how llama.cpp was built: a CPU build has no ggml-cuda.dll, and a build
rem against a different backend has a different set again. Enumerating the
rem wildcard is also the only way a build that gained or lost a backend keeps
rem working without editing this script.
if exist "%REPO%\build-release\llama.dll" (
    for %%f in ("%REPO%\build-release\llama.dll" "%REPO%\build-release\ggml*.dll") do copy /y "%%~f" "%STAGE%\" >nul
)
if exist "%REPO%\build-release\onnxruntime-genai.dll" (
    for %%f in ("%REPO%\build-release\onnxruntime*.dll" "%REPO%\build-release\onnxruntime*.pyd") do copy /y "%%~f" "%STAGE%\" >nul
)

rem The llama.cpp runtime. Copied above, checked here, because a copy that
rem silently did nothing is the same outcome as no copy at all and this is the
rem last place anyone is still watching.
rem
rem A build with KESTREL_ENABLE_LLAMA_CPP=OFF, or one whose llama.cpp root has
rem no bin\ directory, produces a kestrel.exe that cannot start -- the process
rem dies at load time with no window, which a user cannot tell from a crash in
rem their own configuration. Refusing to build the package is the only place
rem that can be caught while someone is still watching.
set "LLAMA_MISSING="
for %%d in (llama.dll ggml.dll) do (
    if not exist "%STAGE%\%%d" set "LLAMA_MISSING=%%d"
)
if not "%LLAMA_MISSING%"=="" (
    echo    FAILED: the llama.cpp runtime is missing from the package: %LLAMA_MISSING%
    echo            kestrel.exe is linked against it, so without it the app
    echo            cannot start at all. Configure with KESTREL_LLAMA_CPP_ROOT
    echo            pointing at a llama.cpp install tree, or with
    echo            KESTREL_ENABLE_LLAMA_CPP=OFF to ship a text-only app.
    exit /b 1
)
if exist "%STAGE%\ggml-cuda.dll" (echo    llama.cpp runtime: CUDA build) else (echo    llama.cpp runtime: CPU build)

rem The ONNX Runtime GenAI runtime, staged into the build tree by CMakeLists.txt.
rem Checked for the same reason as the llama.cpp DLLs above, and for the same
rem cost if it is skipped: kestrel.exe imports it, so a package without it does
rem not start, and does not start silently -- no window, no message, nothing a
rem user can tell from a crash in their own configuration.
set "ORT_MISSING="
for %%d in (onnxruntime-genai.dll onnxruntime.dll) do (
    if not exist "%STAGE%\%%d" set "ORT_MISSING=%%d"
)
if not "%ORT_MISSING%"=="" (
    echo    FAILED: the ONNX Runtime GenAI runtime is missing from the package: %ORT_MISSING%
    echo            kestrel.exe is linked against it, so without it the app
    echo            cannot start at all. Configure with KESTREL_ORT_GENAI_ROOT
    echo            pointing at an ONNX Runtime GenAI tree, or with
    echo            KESTREL_ENABLE_ORT_GENAI=OFF to ship without that backend.
    exit /b 1
)
if exist "%STAGE%\onnxruntime_providers_cuda.dll" (echo    ONNX Runtime: CUDA execution provider present) else (echo    ONNX Runtime: CPU only, the CUDA provider is not present)

rem Where the model goes. Empty on purpose -- a GGUF is gigabytes and copying
rem one into a package would make the package unmovable.
mkdir "%STAGE%\models" || exit /b 1
if not exist "%STAGE%\models\" (
    echo    FAILED: could not create %STAGE%\models
    exit /b 1
)

echo === 5/6 local voice runtime ===
mkdir "%STAGE%\.kestrel-voice" || exit /b 1
robocopy "%REPO%\.kestrel-voice\models" "%STAGE%\.kestrel-voice\models" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
robocopy "%REPO%\.kestrel-voice\Scripts" "%STAGE%\.kestrel-voice\Scripts" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
rem /XD drops piper: unused, and GPL-3.0-or-later in an otherwise permissive
rem package. robocopy takes bare directory names here and excludes them at any
rem depth, so this catches site-packages\piper and site-packages\piper_tts-*.dist-info
rem together. The venv on disk is untouched -- this is a packaging decision,
rem not an uninstall, so piper still works if anyone wants it locally.
robocopy "%REPO%\.kestrel-voice\Lib" "%STAGE%\.kestrel-voice\Lib" /E /XD piper piper_tts-1.8.0.dist-info /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
if exist "%STAGE%\.kestrel-voice\Lib\site-packages\piper" (
    echo    FAILED: piper is GPL-3.0-or-later and must not be redistributed
    echo            with this package. It was not excluded from the copy.
    exit /b 1
)
copy /y "%REPO%\.kestrel-voice\pyvenv.cfg" "%STAGE%\.kestrel-voice\" >nul
copy /y "%REPO%\.kestrel-voice\COPYING" "%STAGE%\.kestrel-voice\" >nul

echo === 6/6 Kokoro server script ===
mkdir "%STAGE%\tools" || exit /b 1
copy /y "%REPO%\tools\kokoro_voice_server.py" "%STAGE%\tools\" >nul || exit /b 1

rem The licence material travels with the distribution, not just with the
rem repository. A notice that only exists in the git checkout is worth nothing
rem to the person holding the packaged folder, which is the whole point of
rem making one -- and phonemizer-fork in particular is GPL-3.0, which obliges
rem anyone redistributing this to convey that licence. Generated output is
rem included rather than rebuilt here, so what ships is exactly what was
rem reviewed, and the packaging run cannot disagree with the repository.
if not exist "%REPO%\THIRD-PARTY-NOTICES.md" (
    echo    FAILED: THIRD-PARTY-NOTICES.md is missing.
    echo            Run: .kestrel-voice\Scripts\python.exe tools\generate-third-party-notices.py
    exit /b 1
)
copy /y "%REPO%\THIRD-PARTY-NOTICES.md" "%STAGE%\" >nul || exit /b 1
copy /y "%REPO%\LICENSE" "%STAGE%\" >nul || exit /b 1
robocopy "%REPO%\licenses" "%STAGE%\licenses" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1

echo.
echo Assembled in %STAGE%
endlocal
exit /b 0
