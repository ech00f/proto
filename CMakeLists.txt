cmake_minimum_required(VERSION 3.20)
project(proto VERSION 0.1.0 LANGUAGES CXX)

if(NOT UNIX)
    message(FATAL_ERROR "proto currently supports Linux and macOS terminals")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PROTO_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl>=7.68)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)

add_library(proto_core
    src/api_client.cpp
    src/completion_service.cpp
    src/config.cpp
    src/context.cpp
    src/history.cpp
    src/local_completion.cpp
    src/safety.cpp
    src/text.cpp
)
target_include_directories(proto_core PUBLIC include)
target_link_libraries(proto_core PUBLIC PkgConfig::CURL nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(proto_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(PROTO_ENABLE_SANITIZERS)
    target_compile_options(proto_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(proto_core PUBLIC -fsanitize=address,undefined)
endif()

add_library(proto_terminal src/editor.cpp src/shell.cpp src/signals.cpp)
target_link_libraries(proto_terminal PUBLIC proto_core)
target_compile_options(proto_terminal PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(proto main.cpp)
target_link_libraries(proto PRIVATE proto_terminal)
target_compile_options(proto PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

include(CTest)
if(BUILD_TESTING)
    add_executable(proto_tests tests/unit_tests.cpp)
    target_link_libraries(proto_tests PRIVATE proto_terminal)
    target_compile_options(proto_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME unit COMMAND proto_tests)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.py
                    $<TARGET_FILE:proto>)
        set_tests_properties(integration PROPERTIES TIMEOUT 90)
    endif()
endif()

install(TARGETS proto RUNTIME DESTINATION bin)
