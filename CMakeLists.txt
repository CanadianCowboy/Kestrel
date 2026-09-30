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

# The llama.cpp tree lives outside the repository and its path is per machine,
# so it cannot be committed. Reading it from the environment as well as from
# -D means one exported variable is enough to make every build of this checkout
# -- including the one the desktop packager runs -- link real inference.
#
# Without this the default build links no llama.cpp at all, so every runtime
# backend reports unavailable and the registry falls through to the mock. That
# is not a degraded mode someone chose: it is what an ordinary build produced,
# and it is why the app answered with canned text by default.
if(NOT KESTREL_LLAMA_CPP_ROOT AND DEFINED ENV{KESTREL_LLAMA_CPP_ROOT})
    set(KESTREL_LLAMA_CPP_ROOT "$ENV{KESTREL_LLAMA_CPP_ROOT}")
    message(STATUS
        "Kestrel: KESTREL_LLAMA_CPP_ROOT from the environment: ${KESTREL_LLAMA_CPP_ROOT}")
endif()

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

# ONNX Runtime GenAI is the Windows-native real-inference path. See
# src/runtime/ortgenaibackend.h for why this exists instead of a TensorRT path.
#
# The tree is laid out with include/, lib/ and bin/ beside one another, which is
# what the NuGet packages unpack to once the contents of
# Microsoft.ML.OnnxRuntimeGenAI (the GenAI extension) and
# Microsoft.ML.OnnxRuntime.Gpu.Windows (onnxruntime.dll with the CUDA execution
# provider) are placed in one directory. It is read from the environment as well
# as from -D, for the same reason KESTREL_LLAMA_CPP_ROOT is: the tree lives
# outside the repository and its path differs per machine.
option(KESTREL_ENABLE_ORT_GENAI "Link ONNX Runtime GenAI for ONNX model inference" ON)
set(KESTREL_ORT_GENAI_ROOT "" CACHE PATH "Root of an ONNX Runtime GenAI tree (include/, lib/, bin/)")
if(NOT KESTREL_ORT_GENAI_ROOT AND DEFINED ENV{KESTREL_ORT_GENAI_ROOT})
    set(KESTREL_ORT_GENAI_ROOT "$ENV{KESTREL_ORT_GENAI_ROOT}")
    message(STATUS
        "Kestrel: KESTREL_ORT_GENAI_ROOT from the environment: ${KESTREL_ORT_GENAI_ROOT}")
endif()

set(KESTREL_ORT_GENAI_ENABLED OFF)
if(KESTREL_ENABLE_ORT_GENAI)
    # The GenAI header is the marker, and the import library beside it is what
    # proves the tree has a native half rather than only managed assemblies --
    # a NuGet folder dropped in wholesale satisfies neither of these checks in
    # the place CMake looks.
    if(KESTREL_ORT_GENAI_ROOT AND EXISTS "${KESTREL_ORT_GENAI_ROOT}/include/ort_genai.h")
        set(KESTREL_ORT_GENAI_ENABLED ON)
        message(STATUS
            "Kestrel: ONNX Runtime GenAI headers found at ${KESTREL_ORT_GENAI_ROOT}")
    else()
        message(STATUS
            "Kestrel: ONNX Runtime GenAI not found; that backend will report unavailable")
    endif()
endif()

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
# Exactly one of these two is compiled, for the same reason as CUDA discovery:
# sapi.h is a Windows SDK header and belongs in a single translation unit, while
# the app reports honestly on the platforms that do not have it.
#
# SAPI 5 rather than Windows.Media.SpeechRecognition. The latter's SDK
# projection exposes the ISpeechRecognizer ABI interface but no projected runtime
# class, so there is no activation factory to create and no GetSpeechRecognizerAsync
# to call; the desktop flavour has no interim-text event either. SAPI's shared
# recognizer is an ordinary COM class with real CoCreateInstance activation and
# its interface is declared in sapi.h, so it needs no Speech SDK install. Every
# call the adapter makes is a vtable call, so the only library it links is winmm,
# for the capture-device count in the microphone probe.
set(KESTREL_SAPI_SPEECH_SOURCE src/runtime/sapirecognizer_stub.cpp)
if(WIN32)
    find_path(KESTREL_SAPI_INCLUDE_DIR NAMES sapi.h)
    find_library(KESTREL_WINMM_LIBRARY NAMES winmm)
    if(KESTREL_SAPI_INCLUDE_DIR AND KESTREL_WINMM_LIBRARY)
        set(KESTREL_SAPI_SPEECH_SOURCE src/runtime/sapirecognizer_win32.cpp)
        message(STATUS "Kestrel: SAPI 5 headers found; desktop dictation enabled")
    else()
        message(STATUS "Kestrel: SAPI 5 headers or winmm not found; "
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

# The platform keystore, one implementation per platform, chosen here for the
# same reason the CUDA discovery source is chosen: the platform API has to live
# in a single translation unit so that everything above it stays portable, and
# so a build for one platform cannot pick up another's headers by accident.
if(WIN32)
    set(KESTREL_SECRET_STORE_SOURCE src/storage/secretstore_windows.cpp)
elseif(APPLE)
    set(KESTREL_SECRET_STORE_SOURCE src/storage/secretstore_macos.cpp)
else()
    set(KESTREL_SECRET_STORE_SOURCE src/storage/secretstore_linux.cpp)
endif()

add_library(kestrel_core
    src/core/conversation.cpp
    src/core/conversation.h
    src/core/idlepersona.cpp
    src/core/idlepersona.h
    src/core/pathtext.cpp
    src/core/pathtext.h
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
    src/runtime/modeldiscovery.cpp
    src/runtime/modeldiscovery.h
    src/runtime/speechrecognizer.h
    src/runtime/speechrecognizer.cpp
    src/runtime/sapirecognizer.h
    ${KESTREL_SAPI_SPEECH_SOURCE}
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
    src/runtime/ortgenaibackend.cpp
    src/runtime/ortgenaibackend.h
    src/storage/atomicfile.cpp
    src/storage/atomicfile.h
    src/storage/json.cpp
    src/storage/json.h
    src/storage/paths.cpp
    src/storage/paths.h
    src/storage/profile.cpp
    src/storage/profile.h
    src/storage/secretstore.h
    src/storage/secretsearch.cpp
    src/storage/secretsearch.h
    src/storage/secretstore_memory.cpp
    src/storage/sha256.cpp
    src/storage/sha256.h
    ${KESTREL_SECRET_STORE_SOURCE}
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

if(APPLE)
    # SecItemAdd and friends live in Security, and the CF* string, data and
    # dictionary calls the same file is written against live in CoreFoundation.
    # Only the macOS secret store needs either. Named at configure time rather
    # than with a pragma in the source so that the dependency is visible where
    # the build is described.
    #
    # CoreFoundation is not pulled in by Security at link time: the frameworks
    # are peers, and the Keychain Services headers only declare the CF types
    # they take. Linking Security alone leaves every CFData, CFString and
    # CFDictionary symbol undefined.
    target_link_libraries(kestrel_core PRIVATE
        "-framework Security"
        "-framework CoreFoundation")
endif()

if(KESTREL_ENABLE_CUDA AND CUDAToolkit_FOUND)
    target_link_libraries(kestrel_core PRIVATE CUDA::cudart)
    target_compile_definitions(kestrel_core PRIVATE KESTREL_HAS_CUDA=1)
endif()

# waveInGetNumDevs, for the microphone probe that decides whether the SAPI
# adapter or the mock is used. It is a device count rather than an attempt to
# open a device, so probing cannot block or leave a handle behind.
if(WIN32 AND KESTREL_SAPI_INCLUDE_DIR AND KESTREL_WINMM_LIBRARY)
    target_link_libraries(kestrel_core PRIVATE ${KESTREL_WINMM_LIBRARY})
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

    # --- llama.cpp runtime staging -------------------------------------------
    # Linking an import library is only half the job on Windows. llama.dll and
    # the ggml*.dll chain under it are resolved by name when the process loads,
    # not from the link line, so a binary linked against a perfectly good
    # llama.lib still dies before main() with STATUS_DLL_NOT_FOUND (0xc0000135).
    #
    # That failure is worth spelling out because it is not a crash you can see.
    # There is no dialog, no message box and no stderr: the process simply never
    # reaches the first line of main, so a test binary prints nothing, a CTest
    # job reports "no output", and a launcher reports nothing at all. It reads
    # exactly like a hang or a silent skip, which is how it cost an afternoon.
    #
    # Copying the DLLs beside the executable is what fixes all of it at once,
    # and unlike the PATH fix-up used for cudart further down it also covers
    # the real application: PATH is only set for CTest, so a double-clicked or
    # packaged kestrel.exe would still fail to start. Staging at build time
    # additionally leaves the DLLs in the staging directory, which is where the
    # desktop packager collects everything else.
    if(WIN32)
        if(TARGET llama)
            get_target_property(_kestrel_llama_loc llama IMPORTED_LOCATION_RELEASE)
            if(NOT _kestrel_llama_loc)
                get_target_property(_kestrel_llama_loc llama IMPORTED_LOCATION)
            endif()
            if(_kestrel_llama_loc)
                get_filename_component(_kestrel_llama_bin "${_kestrel_llama_loc}" DIRECTORY)
            endif()
        else()
            set(_kestrel_llama_bin "${KESTREL_LLAMA_CPP_ROOT}/bin")
        endif()

        if(_kestrel_llama_bin AND EXISTS "${_kestrel_llama_bin}")
            file(GLOB _kestrel_llama_dlls "${_kestrel_llama_bin}/*.dll")
            # Recorded so the desktop packager ships the same set. Package on a
            # different machine than the build is the normal case, and a
            # hand-maintained second list is how that drifts.
            set(KESTREL_LLAMA_CPP_DLLS "${_kestrel_llama_dlls}" CACHE INTERNAL
                "llama.cpp runtime DLLs to stage and ship")
            if(_kestrel_llama_dlls)
                list(LENGTH _kestrel_llama_dlls _kestrel_llama_dll_count)
                message(STATUS
                    "Kestrel: staging ${_kestrel_llama_dll_count} llama.cpp runtime DLL(s) from ${_kestrel_llama_bin}")
            else()
                message(WARNING
                    "Kestrel: found no DLLs in ${_kestrel_llama_bin}; a llama.cpp build will "
                    "fail to start with STATUS_DLL_NOT_FOUND until the runtime is staged")
            endif()
        else()
            message(WARNING
                "Kestrel: llama.cpp links, but no DLL directory was found (looked in "
                "'${_kestrel_llama_bin}'); the built binaries will fail to start")
        endif()
    endif()
endif()

# --- ONNX Runtime GenAI: link and runtime staging -----------------------------
if(KESTREL_ORT_GENAI_ENABLED)
    target_compile_definitions(kestrel_core PRIVATE KESTREL_HAS_ORT_GENAI=1)
    target_include_directories(kestrel_core PRIVATE "${KESTREL_ORT_GENAI_ROOT}/include")
    if(WIN32)
        set(_kestrel_ort_libs "")
        # onnxruntime.lib is needed for the GenAI DLL's own imports to resolve
        # at load time even though this code calls only the GenAI entry points,
        # and onnxruntime-genai.lib carries those. Both are named only if
        # present, so a tree with the GenAI half but no runtime half still
        # configures and reports its own problem at run time rather than
        # failing here with a path CMake would otherwise have to guess at.
        # onnxruntime-genai-cuda is listed alongside the others because the CUDA
        # build of ONNX Runtime GenAI ships it as a separate DLL. Without it the
        # CUDA execution provider fails at load time with Error 126 -- "failed
        # to load library" -- which arrives as a load failure naming a file the
        # package plainly does not contain, and is otherwise a long afternoon.
        foreach(_comp onnxruntime-genai onnxruntime-genai-cuda onnxruntime)
            if(EXISTS "${KESTREL_ORT_GENAI_ROOT}/lib/${_comp}.lib")
                list(APPEND _kestrel_ort_libs "${KESTREL_ORT_GENAI_ROOT}/lib/${_comp}.lib")
            endif()
        endforeach()
        target_link_libraries(kestrel_core PRIVATE ${_kestrel_ort_libs})
    else()
        target_link_libraries(kestrel_core PRIVATE
            "${KESTREL_ORT_GENAI_ROOT}/lib/libonnxruntime-genai.so"
            "${KESTREL_ORT_GENAI_ROOT}/lib/libonnxruntime.so"
        )
    endif()

    # The same STATUS_DLL_NOT_FOUND problem llama.cpp has, for the same reason:
    # these are resolved by name when the process loads. onnxruntime.dll plus
    # the provider DLLs are staged beside the executable, which also means the
    # desktop packager finds them already in the build tree.
    set(KESTREL_ORT_GENAI_DLLS "" CACHE INTERNAL "ONNX Runtime GenAI DLLs to stage and ship")
    if(WIN32 AND EXISTS "${KESTREL_ORT_GENAI_ROOT}/bin")
        file(GLOB _kestrel_ort_dlls "${KESTREL_ORT_GENAI_ROOT}/bin/*.dll")
        set(KESTREL_ORT_GENAI_DLLS "${_kestrel_ort_dlls}" CACHE INTERNAL
            "ONNX Runtime GenAI DLLs to stage and ship")
        if(_kestrel_ort_dlls)
            list(LENGTH _kestrel_ort_dlls _kestrel_ort_dll_count)
            message(STATUS
                "Kestrel: staging ${_kestrel_ort_dll_count} ONNX Runtime DLL(s) from ${KESTREL_ORT_GENAI_ROOT}/bin")
            # The CUDA execution provider is a separate DLL loaded by
            # onnxruntime.dll. Without it the backend still runs, on the CPU,
            # and says so -- which is correct behaviour rather than a failure,
            # so this is reported as information and not as a warning.
            if(EXISTS "${KESTREL_ORT_GENAI_ROOT}/bin/onnxruntime_providers_cuda.dll"
               AND EXISTS "${KESTREL_ORT_GENAI_ROOT}/bin/onnxruntime-genai-cuda.dll")
                message(STATUS "Kestrel: ONNX Runtime has the CUDA execution provider")
            elseif(EXISTS "${KESTREL_ORT_GENAI_ROOT}/bin/onnxruntime_providers_cuda.dll")
                message(WARNING
                    "Kestrel: onnxruntime_providers_cuda.dll is present but "
                    "onnxruntime-genai-cuda.dll is not; the CUDA execution provider will fail to "
                    "load. Add Microsoft.ML.OnnxRuntimeGenAI.CUDA to the tree.")
            else()
                message(STATUS "Kestrel: ONNX Runtime is CPU-only; this backend will use the CPU provider")
            endif()
        else()
            message(WARNING
                "Kestrel: found no DLLs in ${KESTREL_ORT_GENAI_ROOT}/bin; an ONNX Runtime GenAI "
                "build will fail to start with STATUS_DLL_NOT_FOUND")
        endif()
    endif()
endif()

# Copies the staged llama.cpp DLLs beside one executable, so that executable
# can start. Call it for every target that links kestrel_core: the desktop
# app, the offline engine builder and each test binary. The one that links the
# library is the one that inherits the load-time dependency, so a test failing
# without this is not a special case.
#
# Called explicitly rather than from a cmake_language(DEFER) sweep over
# BUILDSYSTEM_TARGETS. The DEFER does run at the end of this directory, but the
# target list it sees there is empty -- verified on CMake 4.4 with an explicit
# print -- so the sweep silently stages nothing and the failure it was meant to
# prevent comes straight back. An explicit call is visible at the point the
# executable is created, which is where a reader can check it.
#
# It is a no-op when llama.cpp is not linked, when the platform resolves
# libraries some other way, or when no DLLs were found at configure time.
function(kestrel_stage_llama_runtime target)
    if(NOT KESTREL_LLAMA_CPP_ENABLED OR NOT WIN32 OR NOT KESTREL_LLAMA_CPP_DLLS)
        return()
    endif()
    if(NOT TARGET ${target})
        # Optional targets (the app, the Qt tests) do not exist in every
        # configuration, so their call sites run inside their own if() blocks.
        # Reaching here means a caller got the target name wrong.
        message(FATAL_ERROR "kestrel_stage_llama_runtime: no such target '${target}'")
    endif()
    if(NOT KESTREL_MULTI_CONFIG_GENERATOR)
        # Nothing to do, and doing it would be the failure the configure-time
        # copy was written to fix. In a single-config build every executable
        # lands in CMAKE_BINARY_DIR, so the copy below already reached all of
        # them -- and this one runs again per target, with every target copying
        # the same files into that one directory at the same moment. That is
        # how a 187 MB provider DLL was being written by several concurrent
        # copies and lost the race. The per-target copy is for multi-config
        # generators alone, where each configuration has an output directory of
        # its own and one configure-time copy cannot reach them.
        return()
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${KESTREL_LLAMA_CPP_DLLS} "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Staging llama.cpp runtime beside ${target}")
endfunction()

# True when the generator gives each configuration its own output directory.
#
# Read into a cache variable so the staging functions below and the configure-time
# copy further down agree on one answer, and so the question is asked once.
get_property(KESTREL_MULTI_CONFIG_GENERATOR GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(KESTREL_MULTI_CONFIG_GENERATOR)
    set(KESTREL_MULTI_CONFIG_GENERATOR ON CACHE INTERNAL
        "Staging per target is required: each configuration has its own output directory")
else()
    set(KESTREL_MULTI_CONFIG_GENERATOR OFF CACHE INTERNAL
        "Staging at configure time reaches every configuration's executables")
endif()

# The same job for ONNX Runtime GenAI, for the same reason and with the same
# no-op guard. Separate from the llama.cpp one rather than generalised, because
# the two runtimes are independently optional: a build may have either, both or
# neither, and a shared function would have to decide what "staged" means when
# one list is empty and the other is not.
function(kestrel_stage_ort_genai_runtime target)
    if(NOT KESTREL_ORT_GENAI_ENABLED OR NOT WIN32 OR NOT KESTREL_ORT_GENAI_DLLS)
        return()
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR "kestrel_stage_ort_genai_runtime: no such target '${target}'")
    endif()
    # Single-config builds are staged once at configure time; see the note on
    # kestrel_stage_llama_runtime for why running this there is a regression
    # rather than belt and braces.
    if(NOT KESTREL_MULTI_CONFIG_GENERATOR)
        return()
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${KESTREL_ORT_GENAI_DLLS} "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Staging ONNX Runtime beside ${target}")
endfunction()

# Staging the runtimes once, at configure time, into the build directory.
#
# This replaced a POST_BUILD copy attached to every executable, which failed.
# Nine targets each copied the same five files into the same directory at the
# same time, and one of them failed on onnxruntime_providers_shared.dll with
# "No such file or directory" -- a 187 MB provider DLL being written by several
# concurrent copies. The nine copies were also nine times the work for one
# result.
#
# A configure-time copy has neither problem. In a single-configuration build
# every executable lands in CMAKE_BINARY_DIR, so one copy reaches all of them,
# it happens before anything is run, and it cannot race itself. The per-target
# function above stays for the multi-configuration case, where executables go to
# a per-configuration subdirectory that CMAKE_BINARY_DIR alone does not cover.
if(WIN32)
    if(KESTREL_LLAMA_CPP_DLLS)
        file(COPY ${KESTREL_LLAMA_CPP_DLLS} DESTINATION "${CMAKE_BINARY_DIR}")
    endif()
    if(KESTREL_ORT_GENAI_DLLS)
        file(COPY ${KESTREL_ORT_GENAI_DLLS} DESTINATION "${CMAKE_BINARY_DIR}")
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

        # MACOSX_BUNDLE, which qt_add_executable does not turn on by itself --
        # WIN32 and MACOSX_BUNDLE are both opt-in arguments there, and without
        # them the command is a plain add_executable.
        #
        # A macOS application has to be a bundle. Not for tidiness: a GUI
        # process that is not a bundle has no Info.plist, so it has no bundle
        # identifier for the window server to associate a Dock icon with, and
        # it cannot carry the metadata macOS expects of an app.
        #
        # It also does not build at all without this. Qt puts the generated
        # QML module for a target in a directory named after that target, so
        # with no bundle the link step tries to write a file named kestrel
        # where a directory named kestrel already is, and ld fails with
        # "open() failed, errno=21 (Is a directory) for 'kestrel'". Putting the
        # executable inside kestrel.app/Contents/MacOS puts it somewhere the
        # generated module never goes.
        #
        # WIN32 puts the Windows build in the GUI subsystem, which is what
        # stops a console window opening behind the app when a user
        # double-clicks it. The report this app can print at launch then has
        # nowhere to write, so main() borrows the launching console when it is
        # asked to report -- see attachToLaunchConsole() there.
        #
        # Each is ignored on the other platforms, so neither line changes
        # anything for the desktop jobs that do not need it.
        qt_add_executable(kestrel MACOSX_BUNDLE WIN32
            src/app/main.cpp
            src/app/appcontroller.cpp
            src/app/appcontroller.h
            # The layout audit is compiled into the app as well as into
            # kestrel_qml_tests: the test asserts the detector works, and the app
            # runs it over the real window. It links no kestrel_core -- it is Qt
            # Quick and nothing else, which is what lets the test target stay a
            # pure-Qt one.
            src/ui/layoutaudit.cpp
            src/ui/layoutaudit.h
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
                src/ui/MessageBubble.qml
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

        # The app links kestrel_core, so it carries the llama.cpp load-time
        # dependency and cannot start without the runtime beside it. This is the
        # one call that is not a test, and it is the one the packager depends
        # on: the staging directory is assembled from build outputs.
        kestrel_stage_llama_runtime(kestrel)
        kestrel_stage_ort_genai_runtime(kestrel)

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
            # Links kestrel_core, so it needs the llama.cpp runtime beside it for
            # the same reason the non-Qt tests do. kestrel_qml_tests below
            # deliberately does not: it links no kestrel_core.
            kestrel_stage_llama_runtime(kestrel_app_tests)
            kestrel_stage_ort_genai_runtime(kestrel_app_tests)

            # Geometry tests for the QML itself. The app test proves a reply
            # arrives; nothing proves the reply is legible, which is how a
            # bubble sized to four pixels could sit in the tree passing every
            # test. This loads src/ui/MessageBubble.qml and asserts the numbers
            # a user would see, so a layout regression is a failed build rather
            # than a screenshot nobody compares against last week.
            add_executable(kestrel_qml_tests
                tests/qml_tests.cpp
                src/ui/layoutaudit.cpp
                src/ui/layoutaudit.h
            )
            set_target_properties(kestrel_qml_tests PROPERTIES AUTOMOC ON)
            target_link_libraries(kestrel_qml_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
            # src, so layoutaudit.h is reached as "ui/layoutaudit.h" exactly as
            # the app reaches it. Deliberately not kestrel_core: this target
            # links no runtime and therefore needs no model beside it.
            target_include_directories(kestrel_qml_tests PRIVATE src)
            # The test loads the component from source rather than from the
            # app's compiled QML module, so it needs the directory itself.
            target_compile_definitions(kestrel_qml_tests PRIVATE
                KESTREL_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
            add_test(NAME kestrel_qml_tests COMMAND kestrel_qml_tests)
            # No display on a build agent, and the window is never shown anyway.
            set_tests_properties(kestrel_qml_tests PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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
                    # Both Qt-linked test binaries need this, not just the one
                    # that existed when the fix-up was written.
                    #
                    # PATH is set with ENVIRONMENT_MODIFICATION, not by putting
                    # $ENV{PATH} into ENVIRONMENT. ENVIRONMENT is a CMake list,
                    # so CMake splits it on every `;` -- and a Windows PATH is
                    # nothing but `;`. That silently truncated the inherited
                    # PATH to its first entry, taking the CUDA bin directory
                    # with it, which is the one directory this fix-up exists to
                    # find. path_list_prepend leaves the inherited list intact.
                    foreach(_kestrel_qt_test kestrel_app_tests kestrel_qml_tests)
                        set_tests_properties(${_kestrel_qt_test} PROPERTIES
                            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_kestrel_qt_prefix}/bin")
                    endforeach()
                    # kestrel_app_tests links kestrel_core, and kestrel_core links
                    # CUDA::cudart when CUDA is enabled, so this binary carries
                    # the cudart dependency too and needs its bin directory on
                    # PATH for the same reason the non-Qt tests get one further
                    # down. The Qt fix-up above gives it the Qt directory only.
                    #
                    # Set here rather than by adding the name to that list further
                    # down, because CMake treats a property set on a test that was
                    # never defined as a configuration error rather than ignoring
                    # it -- and this test does not exist in a build without Qt. Up
                    # here the test is known to exist, and this is the only place
                    # it is.
                    #
                    # ENVIRONMENT_MODIFICATION is replaced, not appended to, so
                    # the Qt entry has to be restated alongside the CUDA one.
                    # kestrel_qml_tests is deliberately left out: it links no
                    # kestrel_core and so has no cudart to resolve.
                    if(KESTREL_ENABLE_CUDA AND CUDAToolkit_FOUND)
                        set_tests_properties(kestrel_app_tests PROPERTIES
                            ENVIRONMENT_MODIFICATION
                                "PATH=path_list_prepend:${_kestrel_qt_prefix}/bin;PATH=path_list_prepend:${CUDAToolkit_BIN_DIR}")
                    endif()
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
kestrel_stage_llama_runtime(kestrel-engine-build)
kestrel_stage_ort_genai_runtime(kestrel-engine-build)

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

    add_executable(kestrel_storage_tests tests/storage_tests.cpp)
    target_link_libraries(kestrel_storage_tests PRIVATE kestrel_core)
    add_test(NAME kestrel_storage_tests COMMAND kestrel_storage_tests)
    # Deliberately not part of the Qt PATH fix-up further down: this binary
    # links no Qt. It does link the platform keystore, whose library is either
    # the system one or already a load-time dependency of the process. It does
    # need cudart under a CUDA build, which is handled with the other tests.
    #
    # Bounded, and the only test here that is. The other three are pure
    # computation and finish in milliseconds. This one talks to the real
    # platform keystore -- DPAPI, secret-tool, the macOS login keychain -- and
    # each of those can block rather than answer: a locked keyring in a
    # session with nobody present to approve it waits, and it does not come
    # back with an error to report.
    #
    # That is what a macOS core run did on 2026-09-27: every other job on the
    # same commit finished in about three minutes and this one was still going
    # at thirteen, with nothing in the job output to say which test it was
    # inside. A hang is worse than a failure for exactly that reason -- it
    # costs the full job timeout and still reports nothing actionable.
    #
    # CTest names the test it was running when the timeout fires, so the bound
    # turns an unactionable stall into a named failure. Five minutes is far
    # more than a healthy keystore round trip needs and far less than the
    # six hours it was costing.
    set_tests_properties(kestrel_storage_tests PROPERTIES TIMEOUT 300)

    # The document format the profile is written in. Separate from the storage
    # tests because it has no keystore in it at all: a JSON parser that only
    # runs when a platform keystore happens to be present is a JSON parser that
    # is never run.
    add_executable(kestrel_json_tests tests/json_tests.cpp)
    target_link_libraries(kestrel_json_tests PRIVATE kestrel_core)
    add_test(NAME kestrel_json_tests COMMAND kestrel_json_tests)

    add_executable(kestrel_profile_tests tests/profile_tests.cpp)
    target_link_libraries(kestrel_profile_tests PRIVATE kestrel_core)
    add_test(NAME kestrel_profile_tests COMMAND kestrel_profile_tests)

    # The secret store's report parser, tested as text in and text out so that
    # it needs neither libsecret nor a keyring and runs on every platform. The
    # Linux backend is the only caller, and it is only compiled on Linux, so a
    # test that reached through the backend would only ever run on one of the
    # three CI machines.
    add_executable(kestrel_secretsearch_tests tests/secretsearch_tests.cpp)
    target_link_libraries(kestrel_secretsearch_tests PRIVATE kestrel_core)
    add_test(NAME kestrel_secretsearch_tests COMMAND kestrel_secretsearch_tests)

    # A CUDA-enabled build links the cudart import library, so cudart's DLL has
    # to be resolvable when the tests actually run. CMake does not do that
    # implicitly on Windows.
    # Every binary below links kestrel_core, which links CUDA::cudart, so every
    # one of them inherits the dependency -- not just the runtime tests, which
    # is why this started out wrong.
    # Same reason as the Qt fix-up above: a Windows PATH interpolated into
    # ENVIRONMENT is truncated at the first `;`, which would drop everything
    # after cudart's own directory.
    if(KESTREL_ENABLE_CUDA AND CUDAToolkit_FOUND AND WIN32)
        foreach(_kestrel_cuda_test
                kestrel_core_tests
                kestrel_runtime_tests
                kestrel_storage_tests
                kestrel_json_tests
                kestrel_profile_tests
                kestrel_secretsearch_tests)
            set_tests_properties(${_kestrel_cuda_test} PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CUDAToolkit_BIN_DIR}")
        endforeach()
    endif()

    # Every test binary links kestrel_core and so needs the llama.cpp runtime
    # beside it. Listed rather than derived: the same list as the cudart fix-up
    # above, so a test cannot be added to one and forgotten in the other.
    foreach(_kestrel_llama_test
            kestrel_core_tests
            kestrel_runtime_tests
            kestrel_storage_tests
            kestrel_json_tests
            kestrel_profile_tests
            kestrel_secretsearch_tests)
        kestrel_stage_llama_runtime(${_kestrel_llama_test})
        kestrel_stage_ort_genai_runtime(${_kestrel_llama_test})
    endforeach()
endif()
