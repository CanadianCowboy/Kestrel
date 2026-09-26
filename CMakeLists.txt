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

# KESTREL_ENABLE_TENSORRT is opt-in because the TensorRT SDK is not vendored.
# Point KESTREL_TENSORRT_ROOT at an unpacked SDK that contains include/ and
# lib/ when enabling it.
option(KESTREL_ENABLE_TENSORRT "Link the TensorRT SDK" OFF)
set(KESTREL_TENSORRT_ROOT "" CACHE PATH "Root of an unpacked TensorRT SDK")

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

if(KESTREL_BUILD_UI)
    find_package(Qt6 6.6 COMPONENTS Core Quick QuickControls2 QUIET)
    if(Qt6_FOUND)
        qt_standard_project_setup(REQUIRES 6.6)

        qt_add_executable(kestrel
            src/app/main.cpp
            src/app/appcontroller.cpp
            src/app/appcontroller.h
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
    else()
        message(STATUS "Qt 6 not found; skipping the Kestrel desktop target")
    endif()
endif()

if(KESTREL_BUILD_TESTS)
    enable_testing()

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
