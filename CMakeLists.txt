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
    src/core/voicesession.cpp
    src/core/voicesession.h
    src/runtime/cudadevice.cpp
    src/runtime/cudadevice.h
    src/runtime/modelbackend.cpp
    src/runtime/modelbackend.h
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
    src/storage/atomicfile.cpp
    src/storage/atomicfile.h
    src/storage/json.cpp
    src/storage/json.h
    src/storage/paths.cpp
    src/storage/paths.h
    src/storage/profile.cpp
    src/storage/profile.h
    src/storage/secretstore.h
    src/storage/secretstore_memory.cpp
    src/storage/sha256.cpp
    src/storage/sha256.h
    ${KESTREL_SECRET_STORE_SOURCE}
)

target_include_directories(kestrel_core PUBLIC src)
target_compile_features(kestrel_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(kestrel_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(kestrel_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

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
        # Ignored on Windows and Linux, so this line changes nothing for the
        # other two desktop jobs.
        qt_add_executable(kestrel MACOSX_BUNDLE
            src/app/main.cpp
            src/app/appcontroller.cpp
            src/app/appcontroller.h
            src/app/generationworker.cpp
            src/app/generationworker.h
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
            add_test(NAME kestrel_app_tests COMMAND kestrel_app_tests)

            # Geometry tests for the QML itself. The app test proves a reply
            # arrives; nothing proves the reply is legible, which is how a
            # bubble sized to four pixels could sit in the tree passing every
            # test. This loads src/ui/MessageBubble.qml and asserts the numbers
            # a user would see, so a layout regression is a failed build rather
            # than a screenshot nobody compares against last week.
            add_executable(kestrel_qml_tests
                tests/qml_tests.cpp
            )
            set_target_properties(kestrel_qml_tests PROPERTIES AUTOMOC ON)
            target_link_libraries(kestrel_qml_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
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
    add_test(NAME kestrel_core_tests COMMAND kestrel_core_tests)

    add_executable(kestrel_runtime_tests tests/runtime_tests.cpp)
    target_link_libraries(kestrel_runtime_tests PRIVATE kestrel_core)
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
                kestrel_profile_tests)
            set_tests_properties(${_kestrel_cuda_test} PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CUDAToolkit_BIN_DIR}")
        endforeach()
    endif()
endif()
