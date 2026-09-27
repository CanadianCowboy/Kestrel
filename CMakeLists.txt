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
)

target_include_directories(kestrel_core PUBLIC src)
target_compile_features(kestrel_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(kestrel_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(kestrel_core PRIVATE -Wall -Wextra -Wpedantic)
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
            # llama.cpp splits ggml across several shared objects -- libggml.so
            # plus libggml-base.so and libggml-cpu.so, one more per enabled
            # backend -- and recent releases add libllama-impl.so. Naming two of
            # them by hand links on a tree that happens to match that pair and
            # fails to resolve the rest everywhere else, so link what is
            # actually installed. Both the unversioned development symlinks and
            # the versioned objects are collected, because a prefix can have
            # one form for llama and the other for ggml; a name that matches
            # both patterns appears twice and is linked once.
            # GLOB is evaluated once per configure, which matches how the rest
            # of this file resolves optional dependencies.
            set(_kestrel_llama_libdir "${KESTREL_LLAMA_CPP_ROOT}/lib")
            file(GLOB _kestrel_llama_libs
                "${_kestrel_llama_libdir}/libllama.so"
                "${_kestrel_llama_libdir}/libllama-impl.so"
                "${_kestrel_llama_libdir}/libggml*.so"
                "${_kestrel_llama_libdir}/libllama*.so.*"
                "${_kestrel_llama_libdir}/libggml*.so.*")
            list(REMOVE_DUPLICATES _kestrel_llama_libs)
            if(NOT _kestrel_llama_libs)
                message(WARNING
                    "Kestrel: ${KESTREL_LLAMA_CPP_ROOT} has headers but no llama.cpp "
                    "shared libraries in ${_kestrel_llama_libdir}, so the link will fail. "
                    "Point KESTREL_LLAMA_CPP_ROOT at a llama.cpp install or build tree, "
                    "or set KESTREL_ENABLE_LLAMA_CPP=OFF to build without GGUF inference.")
            else()
                list(SORT _kestrel_llama_libs)
                message(STATUS "Kestrel: linking llama.cpp from ${_kestrel_llama_libdir}: "
                               "${_kestrel_llama_libs}")
            endif()
            target_link_libraries(kestrel_core PRIVATE ${_kestrel_llama_libs})
        endif()
    endif()

    # llama.cpp is linked as a bare path rather than an imported target, so
    # CMake cannot infer where the objects live. Without an rpath the installed
    # binary starts and then fails to load libllama.so, because the Linux loader
    # searches the system paths only -- /opt and ~/.local are not among them.
    # kestrel_core is static, so the property belongs on the executables.
    if(UNIX AND NOT APPLE AND KESTREL_LLAMA_CPP_ROOT)
        set(_kestrel_llama_rpath "${KESTREL_LLAMA_CPP_ROOT}/lib")
    endif()
endif()

if(KESTREL_BUILD_UI)
    find_package(Qt6 6.6 COMPONENTS Core Quick QuickControls2 QUIET)
    if(Qt6_FOUND)
        qt_standard_project_setup(REQUIRES 6.6)

        qt_add_executable(kestrel
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
        )

        target_link_libraries(kestrel PRIVATE
            kestrel_core
            Qt6::Quick
            Qt6::QuickControls2
        )
        target_compile_definitions(kestrel PRIVATE KESTREL_HAS_QT=1)

        if(UNIX AND NOT APPLE)
            # One identifier for the desktop entry, the icon, and the window
            # association. A binary named kestrel that never names its own
            # .desktop file gets a generic icon and an unlisted window menu.
            set(KESTREL_DESKTOP_ID "io.github.canadiancowboy.kestrel")
            target_compile_definitions(kestrel PRIVATE
                KESTREL_DESKTOP_FILE_ID="${KESTREL_DESKTOP_ID}")
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
    add_test(NAME kestrel_core_tests COMMAND kestrel_core_tests)

    add_executable(kestrel_runtime_tests tests/runtime_tests.cpp)
    target_link_libraries(kestrel_runtime_tests PRIVATE kestrel_core)
    add_test(NAME kestrel_runtime_tests COMMAND kestrel_runtime_tests)

    # A CUDA-enabled build links the cudart import library, so cudart's DLL has
    # to be resolvable when the tests actually run. CMake does not do that
    # implicitly on Windows.
    if(KESTREL_ENABLE_CUDA AND CUDAToolkit_FOUND AND WIN32)
        set_tests_properties(kestrel_runtime_tests PROPERTIES
            ENVIRONMENT "PATH=${CUDAToolkit_BIN_DIR};$ENV{PATH}")
    endif()
endif()

# --- Linux install rules ------------------------------------------------------
# Windows and macOS ship through their own platform tooling. Linux is the only
# place where CMake has to assemble the installed application itself, and
# without these rules `cmake --install` copies nothing at all: the app still
# runs from the build tree, so the omission is invisible until someone tries to
# install it.
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)

    if(_kestrel_llama_rpath)
        foreach(_kestrel_rpath_target kestrel kestrel-engine-build)
            if(TARGET ${_kestrel_rpath_target})
                # Qt's setup gives an installed binary "$ORIGIN:$ORIGIN/../lib"
                # so it can find the Qt libraries installed beside it. Append
                # rather than replace: a binary that resolves llama.cpp but not
                # Qt6Core is no more useful than one that resolves neither.
                get_target_property(_kestrel_target_rpath
                    ${_kestrel_rpath_target} INSTALL_RPATH)
                if(_kestrel_target_rpath AND
                   NOT _kestrel_target_rpath MATCHES "-NOTFOUND$")
                    set(_kestrel_merged_rpath
                        "${_kestrel_target_rpath}:${_kestrel_llama_rpath}")
                else()
                    set(_kestrel_merged_rpath
                        "$ORIGIN:$ORIGIN/../lib:${_kestrel_llama_rpath}")
                endif()
                set_target_properties(${_kestrel_rpath_target} PROPERTIES
                    INSTALL_RPATH "${_kestrel_merged_rpath}")
            endif()
        endforeach()
    endif()

    # Only the desktop app is installed. kestrel-engine-build writes engine
    # records beside an engine and is a maintainer tool, so it stays in the
    # build tree rather than on an end user's PATH.
    if(TARGET kestrel)
        install(TARGETS kestrel
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
        install(FILES "packaging/linux/${KESTREL_DESKTOP_ID}.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
        install(FILES "packaging/linux/${KESTREL_DESKTOP_ID}.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
        install(FILES "packaging/linux/${KESTREL_DESKTOP_ID}.metainfo.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
    endif()
endif()
