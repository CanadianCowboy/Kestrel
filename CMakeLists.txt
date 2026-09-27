cmake_minimum_required(VERSION 3.24)

project(Kestrel VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KESTREL_BUILD_UI "Build the Qt/QML desktop application when Qt 6 is available" ON)
option(KESTREL_BUILD_TESTS "Build Kestrel tests" ON)

# Runtime accelerator options.
#
# KESTREL_ENABLE_CUDA compiles in real GPU device discovery. It defaults to ON
# but degrades automatically to a portable stub when no CUDA Toolkit is found,
# so contributors without a GPU or driver are never blocked from building.
option(KESTREL_ENABLE_CUDA "Compile CUDA device discovery when a CUDA Toolkit is available" ON)

# enable_testing() is declared before any add_test() call, including the Qt-only
# app test registered further down.
if(KESTREL_BUILD_TESTS)
    enable_testing()
endif()

# KESTREL_ENABLE_TENSORRT is opt-in because the TensorRT SDK is not vendored.
# Point KESTREL_TENSORRT_ROOT at an unpacked SDK that contains include/ and
# lib/ when enabling it.
option(KESTREL_ENABLE_TENSORRT "Link the TensorRT SDK" OFF)
set(KESTREL_TENSORRT_ROOT "" CACHE PATH "Root of an unpacked TensorRT SDK")

# llama.cpp is the portable GGUF path. Like CUDA it defaults on and degrades
# automatically: a build with no llama.cpp present still compiles and simply
# reports the backend unavailable.
option(KESTREL_ENABLE_LLAMA_CPP "Link llama.cpp for GGUF inference" ON)
set(KESTREL_LLAMA_CPP_ROOT "" CACHE PATH "Root of a llama.cpp install or build tree")

# Resolution order: a CMake package config first, then an explicit root. Never
# found by accident, because a wrong llama.cpp would produce subtly wrong
# tokens rather than a link error.
set(KESTREL_LLAMA_CPP_ENABLED OFF)
if(KESTREL_ENABLE_LLAMA_CPP)
    find_package(llama QUIET CONFIG)
    if(llama_FOUND)
        set(KESTREL_LLAMA_CPP_ENABLED ON)
        message(STATUS "Kestrel: llama.cpp found via its CMake package")
    elseif(KESTREL_LLAMA_CPP_ROOT AND EXISTS "${KESTREL_LLAMA_CPP_ROOT}/include/llama.h")
        set(KESTREL_LLAMA_CPP_ENABLED ON)
        message(STATUS "Kestrel: llama.cpp headers found at ${KESTREL_LLAMA_CPP_ROOT}")
    else()
        message(STATUS "Kestrel: llama.cpp not found; the GGUF backend will report unavailable")
    endif()
endif()

# --- CUDA discovery source selection -----------------------------------------
# Exactly one of these two is compiled. Both define probeCuda(); only the CUDA
# one may include a CUDA header. See src/runtime/cudadevice.h.
if(KESTREL_ENABLE_CUDA)
    find_package(CUDAToolkit QUIET)
endif()

set(KESTREL_CUDA_DISCOVERY_SOURCE src/runtime/cudadiscovery_stub.cpp)
if(KESTREL_ENABLE_CUDA AND CUDAToolkit_FOUND)
    set(KESTREL_CUDA_DISCOVERY_SOURCE src/runtime/cudadiscovery_cuda.cpp)
    message(STATUS "Kestrel: CUDA Toolkit ${CUDAToolkit_VERSION} found; device discovery enabled")
elseif(KESTREL_ENABLE_CUDA)
    message(STATUS "Kestrel: KESTREL_ENABLE_CUDA=ON but no CUDA Toolkit was found; "
                   "compiling the portable device-discovery stub")
else()
    message(STATUS "Kestrel: CUDA disabled; compiling the portable device-discovery stub")
endif()

# --- Platform speech recognition ---------------------------------------------
# The Windows Runtime speech headers are part of the Windows SDK rather than
# something Kestrel vendors, so this checks for them and says what it found
# instead of assuming. On anything else -- or on a Windows SDK without them --
# the stub compiles and the app reports that it cannot listen.
#
# Note on what the SDK actually provides: the Windows.Media.SpeechRecognition
# projection here exposes the ISpeechRecognizer interface but not a projected
# runtime class, so there is no GetSpeechRecognizerAsync to call and reaching
# the engine means creating the ABI interface through RoCreateInstance. The
# desktop flavour of that API also has no interim-text event, so a recognizer
# built on it can report a finished phrase but not the words so far. See
# src/runtime/winrtspeechrecognizer.h.
set(KESTREL_WINRT_SPEECH_SOURCE src/runtime/winrtspeechrecognizer_stub.cpp)
set(KESTREL_WINRT_SPEECH OFF)
if(WIN32)
    find_path(KESTREL_CPPWINRT_INCLUDE_DIR
        NAMES winrt/Windows.Media.SpeechRecognition.h
        PATH_SUFFIXES cppwinrt
    )
    find_library(KESTREL_WINDOWSAPP_LIBRARY NAMES WindowsApp)
    if(KESTREL_CPPWINRT_INCLUDE_DIR AND KESTREL_WINDOWSAPP_LIBRARY)
        message(STATUS "Kestrel: Windows speech headers present; the recognizer stub is "
                       "still used because the SDK ships no projected speech class")
    else()
        message(STATUS "Kestrel: Windows speech recognition headers not found; "
                       "compiling the portable recognizer stub")
    endif()
else()
    message(STATUS "Kestrel: not Windows; compiling the portable recognizer stub")
endif()

# --- TensorRT discovery ------------------------------------------------------
# The TensorRT SDK is intentionally not vendored, so this never finds itself by
# accident: it is reachable only through the explicit cache variables above.
set(KESTREL_TENSORRT_ENABLED OFF)
if(KESTREL_ENABLE_TENSORRT)
    if(KESTREL_TENSORRT_ROOT AND EXISTS "${KESTREL_TENSORRT_ROOT}/include/NvInfer.h")
        set(KESTREL_TENSORRT_ENABLED ON)
        message(STATUS "Kestrel: TensorRT SDK found at ${KESTREL_TENSORRT_ROOT}")
    else()
        message(WARNING
            "Kestrel: KESTREL_ENABLE_TENSORRT=ON but no SDK was found. Set "
            "KESTREL_TENSORRT_ROOT to a directory containing include/NvInfer.h. "
            "Continuing without TensorRT.")
    endif()
endif()

add_library(kestrel_core
    src/core/conversation.cpp
    src/core/conversation.h
    src/core/persona.cpp
    src/core/persona.h
    src/core/presence.cpp
    src/core/presence.h
    src/core/idlepersona.cpp
    src/core/idlepersona.h
    src/core/idletool.cpp
    src/core/idletool.h
    src/core/voicesession.cpp
    src/core/voicesession.h
    src/runtime/cudadevice.cpp
    src/runtime/cudadevice.h
    src/runtime/modelbackend.cpp
    src/runtime/modelbackend.h
    src/runtime/speechrecognizer.h
    src/runtime/speechrecognizer.cpp
    src/runtime/winrtspeechrecognizer.h
    ${KESTREL_WINRT_SPEECH_SOURCE}
    ${KESTREL_CUDA_DISCOVERY_SOURCE}
    src/runtime/engineartifact.cpp
    src/runtime/engineartifact.h
    src/runtime/backendregistry.cpp
    src/runtime/backendregistry.h
    src/runtime/modelbackend.h
    src/runtime/mockbackend.cpp
    src/runtime/mockbackend.h
    src/runtime/llamacppbackend.cpp
    src/runtime/llamacppbackend.h
    src/runtime/tensorrtbackend.cpp
    src/runtime/tensorrtbackend.h
)

target_include_directories(kestrel_core PUBLIC src)
target_compile_features(kestrel_core PUBLIC cxx_std_20)

# Strict compile settings, shared by the portable library and the tests that
# cover it. Sharing them is not tidiness: a test compiled more leniently than
# the code it checks can pass against a translation unit the shipping target
# would not build.
#
# /utf-8 is part of that contract rather than a nicety. The personality layer
# spells out the punctuation it emits (an ellipsis, a middot) as real
# characters, and those strings cross into a UTF-8 UI. Without the flag MSVC
# decodes such literals in the machine's code page and ships mangled text.
if(MSVC)
    set(KESTREL_STRICT_FLAGS /W4 /permissive- /utf-8)
else()
    set(KESTREL_STRICT_FLAGS -Wall -Wextra -Wpedantic)
endif()

target_compile_options(kestrel_core PRIVATE ${KESTREL_STRICT_FLAGS})

if(KESTREL_ENABLE_CUDA AND CUDAToolkit_FOUND)
    target_link_libraries(kestrel_core PRIVATE CUDA::cudart)
    target_compile_definitions(kestrel_core PRIVATE KESTREL_HAS_CUDA=1)
endif()

if(KESTREL_TENSORRT_ENABLED)
    target_compile_definitions(kestrel_core PRIVATE KESTREL_HAS_TENSORRT=1)
    target_include_directories(kestrel_core PRIVATE "${KESTREL_TENSORRT_ROOT}/include")
    if(WIN32)
        target_link_libraries(kestrel_core PRIVATE
            "${KESTREL_TENSORRT_ROOT}/lib/x64/nvinfer.lib"
            "${KESTREL_TENSORRT_ROOT}/lib/x64/nvinfer_onnxruntime.lib"
            "${KESTREL_TENSORRT_ROOT}/lib/x64/nvonnxparser.lib"
        )
    else()
        target_link_libraries(kestrel_core PRIVATE
            "${KESTREL_TENSORRT_ROOT}/lib/libnvinfer.so"
        )
    endif()
    target_compile_definitions(kestrel_core PUBLIC KESTREL_TENSORRT_ENABLED=1)
endif()

if(KESTREL_LLAMA_CPP_ENABLED)
    target_compile_definitions(kestrel_core PRIVATE KESTREL_HAS_LLAMA_CPP=1)
    if(TARGET llama)
        target_link_libraries(kestrel_core PRIVATE llama)
    else()
        target_include_directories(kestrel_core PRIVATE "${KESTREL_LLAMA_CPP_ROOT}/include")
        # llama.cpp ships several import libraries. Link whichever are present so
        # a build tree that omits the CUDA backend still links, rather than
        # failing on a hardcoded ggml-cuda name.
        if(WIN32)
            set(_kestrel_llama_libs "")
            foreach(_comp llama ggml ggml-base ggml-cpu ggml-cuda)
                if(EXISTS "${KESTREL_LLAMA_CPP_ROOT}/lib/${_comp}.lib")
                    list(APPEND _kestrel_llama_libs "${KESTREL_LLAMA_CPP_ROOT}/lib/${_comp}.lib")
                endif()
            endforeach()
            target_link_libraries(kestrel_core PRIVATE ${_kestrel_llama_libs})
        elseif(APPLE)
            target_link_libraries(kestrel_core PRIVATE
                "${KESTREL_LLAMA_CPP_ROOT}/lib/libllama.dylib"
                "${KESTREL_LLAMA_CPP_ROOT}/lib/libggml.dylib"
            )
        else()
            target_link_libraries(kestrel_core PRIVATE
                "${KESTREL_LLAMA_CPP_ROOT}/lib/libllama.so"
                "${KESTREL_LLAMA_CPP_ROOT}/lib/libggml.so"
            )
        endif()
    endif()
endif()

if(KESTREL_BUILD_UI)
    find_package(Qt6 6.6 COMPONENTS Core Quick QuickControls2 QUIET)
    if(Qt6_FOUND)
        # TextToSpeech is optional on purpose. A Qt install without it still
        # builds and runs; Kestrel then speaks as text, which is a supported
        # outcome rather than a broken one, exactly as a missing llama.cpp
        # degrades the GGUF backend to "unavailable" instead of failing the
        # build.
        #
        # It is a separate find_package rather than another entry in COMPONENTS
        # on purpose: one missing component there fails the whole find, which
        # would silently drop the desktop target on a Qt install that has
        # everything else. Asking for the module on its own cannot do that, and
        # it defines Qt6::TextToSpeech when -- and only when -- it is present.
        find_package(Qt6TextToSpeech QUIET)
        qt_standard_project_setup(REQUIRES 6.6)

        qt_add_executable(kestrel
            src/app/main.cpp
            src/app/appcontroller.cpp
            src/app/appcontroller.h
            src/app/generationworker.cpp
            src/app/generationworker.h
    src/app/speechsynthesizer.cpp
    src/app/speechsynthesizer.h
    src/app/localmodelspeechbackend.cpp
    src/app/localmodelspeechbackend.h
    src/app/localvoiceengines.cpp
    src/app/localvoiceengines.h
    src/app/kokorospeechbackend.cpp
    src/app/kokorospeechbackend.h
            src/app/listensession.cpp
            src/app/listensession.h
            src/app/conversationentry.h
            src/app/messagemodel.cpp
            src/app/messagemodel.h
            src/app/conversationmodel.cpp
            src/app/conversationmodel.h
        )

        qt_add_qml_module(kestrel
            URI Kestrel
            VERSION 1.0
            QML_FILES
                src/ui/Main.qml
        )

        target_link_libraries(kestrel PRIVATE
            kestrel_core
            Qt6::Quick
            Qt6::QuickControls2
        )
        target_compile_definitions(kestrel PRIVATE KESTREL_HAS_QT=1)

        if(TARGET Qt6::TextToSpeech)
            target_link_libraries(kestrel PRIVATE Qt6::TextToSpeech)
            target_compile_definitions(kestrel PRIVATE KESTREL_HAS_TEXT_TO_SPEECH=1)
            message(STATUS "Kestrel: Qt TextToSpeech found; responses can be spoken")
        else()
            message(STATUS "Kestrel: Qt TextToSpeech not found; responses are text-only")
        endif()

        # The local neural voice plays its own audio rather than going through
        # the text-to-speech module, so it needs the media module directly. It is
        # optional for the same reason: a Qt install without it falls back to
        # whatever the platform offers.
        find_package(Qt6Multimedia QUIET)
        if(TARGET Qt6::Multimedia)
            target_link_libraries(kestrel PRIVATE Qt6::Multimedia)
            target_compile_definitions(kestrel PRIVATE KESTREL_HAS_QT_MULTIMEDIA=1)
            message(STATUS "Kestrel: Qt Multimedia found; a local neural voice can play")
        else()
            message(STATUS "Kestrel: Qt Multimedia not found; no local neural voice")
        endif()

        # Threading tests for the async generation path. They need a real
        # event loop, so they exist only in a Qt-enabled build.
        if(KESTREL_BUILD_TESTS)
            # AppController is compiled in rather than linked from the app
            # executable, so the send path can be driven end to end: the
            # controller picks a backend, assembles a prompt, hands it to the
            # worker thread, and streams the reply back into the message model.
            # Without this, every layer is tested except the one the user uses.
            add_executable(kestrel_app_tests
                tests/app_tests.cpp
                src/app/generationworker.cpp
                src/app/generationworker.h
                src/app/speechsynthesizer.cpp
                src/app/speechsynthesizer.h
                src/app/localmodelspeechbackend.cpp
                src/app/localmodelspeechbackend.h
                src/app/localvoiceengines.cpp
                src/app/localvoiceengines.h
                src/app/kokorospeechbackend.cpp
                src/app/kokorospeechbackend.h
                src/app/listensession.cpp
                src/app/listensession.h
                src/app/appcontroller.cpp
                src/app/appcontroller.h
                src/app/messagemodel.cpp
                src/app/messagemodel.h
                src/app/conversationmodel.cpp
                src/app/conversationmodel.h
                src/app/conversationentry.h
            )
            set_target_properties(kestrel_app_tests PROPERTIES AUTOMOC ON)
            target_link_libraries(kestrel_app_tests PRIVATE kestrel_core Qt6::Core Qt6::Gui)
            target_compile_options(kestrel_app_tests PRIVATE ${KESTREL_STRICT_FLAGS})
            if(TARGET Qt6::TextToSpeech)
                target_link_libraries(kestrel_app_tests PRIVATE Qt6::TextToSpeech)
                target_compile_definitions(kestrel_app_tests PRIVATE KESTREL_HAS_TEXT_TO_SPEECH=1)
            endif()
            if(TARGET Qt6::Multimedia)
                target_link_libraries(kestrel_app_tests PRIVATE Qt6::Multimedia)
                target_compile_definitions(kestrel_app_tests PRIVATE KESTREL_HAS_QT_MULTIMEDIA=1)
            endif()
            add_test(NAME kestrel_app_tests COMMAND kestrel_app_tests)

            # A Qt-linked test needs Qt's DLLs resolvable at run time. CMake does
            # not put the Qt bin directory on PATH for imported targets, so the
            # test would fail with STATUS_DLL_NOT_FOUND before main() runs.
            # Derive the bin directory from the imported library location
            # rather than hardcoding a Qt install prefix.
            if(WIN32)
                get_target_property(_kestrel_qt_core Qt6::Core IMPORTED_LOCATION_RELEASE)
                if(NOT _kestrel_qt_core)
                    get_target_property(_kestrel_qt_core Qt6::Core IMPORTED_LOCATION)
                endif()
                if(_kestrel_qt_core)
                    get_filename_component(_kestrel_qt_lib "${_kestrel_qt_core}" DIRECTORY)
                    get_filename_component(_kestrel_qt_prefix "${_kestrel_qt_lib}" DIRECTORY)
                    set_tests_properties(kestrel_app_tests PROPERTIES
                        ENVIRONMENT "PATH=${_kestrel_qt_prefix}/bin;$ENV{PATH}")
                endif()
            endif()
        endif()
    else()
        message(STATUS "Qt 6 not found; skipping the Kestrel desktop target")
    endif()
endif()

# Offline engine-build tool.
#
# Deliberately separate from the desktop app: building an engine is a long,
# machine-specific operation, and the app must never write to engines. It
# links only the portable core, so it builds with no Qt and no CUDA toolkit.
add_executable(kestrel-engine-build src/tools/enginebuild.cpp)
target_link_libraries(kestrel-engine-build PRIVATE kestrel_core)

if(KESTREL_TENSORRT_ENABLED)
    target_include_directories(kestrel-engine-build PRIVATE "${KESTREL_TENSORRT_ROOT}/include")
    if(WIN32)
        target_link_libraries(kestrel-engine-build PRIVATE
            "${KESTREL_TENSORRT_ROOT}/lib/x64/nvinfer.lib"
            "${KESTREL_TENSORRT_ROOT}/lib/x64/nvonnxparser.lib"
        )
    else()
        target_link_libraries(kestrel-engine-build PRIVATE
            "${KESTREL_TENSORRT_ROOT}/lib/libnvinfer.so"
            "${KESTREL_TENSORRT_ROOT}/lib/libnvonnxparser.so"
        )
    endif()
endif()

if(KESTREL_BUILD_TESTS)
    add_executable(kestrel_core_tests tests/core_tests.cpp)
    target_link_libraries(kestrel_core_tests PRIVATE kestrel_core)
    target_compile_options(kestrel_core_tests PRIVATE ${KESTREL_STRICT_FLAGS})
    add_test(NAME kestrel_core_tests COMMAND kestrel_core_tests)

    add_executable(kestrel_runtime_tests tests/runtime_tests.cpp)
    target_link_libraries(kestrel_runtime_tests PRIVATE kestrel_core)
    target_compile_options(kestrel_runtime_tests PRIVATE ${KESTREL_STRICT_FLAGS})
    add_test(NAME kestrel_runtime_tests COMMAND kestrel_runtime_tests)

    # A CUDA-enabled build links the cudart import library, so cudart's DLL has
    # to be resolvable when the tests actually run. CMake does not do that
    # implicitly on Windows.
    if(KESTREL_ENABLE_CUDA AND CUDAToolkit_FOUND AND WIN32)
        set_tests_properties(kestrel_runtime_tests PROPERTIES
            ENVIRONMENT "PATH=${CUDAToolkit_BIN_DIR};$ENV{PATH}")
    endif()
endif()
