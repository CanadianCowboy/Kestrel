cmake_minimum_required(VERSION 3.24)

project(Kestrel VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KESTREL_BUILD_UI "Build the Qt/QML desktop application when Qt 6 is available" ON)
option(KESTREL_BUILD_TESTS "Build Kestrel tests" ON)

add_library(kestrel_core
    src/core/conversation.cpp
    src/core/conversation.h
    src/core/voicesession.cpp
    src/core/voicesession.h
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
endif()
