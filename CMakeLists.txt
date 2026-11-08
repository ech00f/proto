cmake_minimum_required(VERSION 3.20)
project(proto VERSION 0.1.0 LANGUAGES CXX)

if(NOT UNIX)
    message(FATAL_ERROR "proto currently supports Linux and macOS terminals")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PROTO_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PROTO_BUILD_BASH_MODULE "Build native Bash/Readline integration when headers are available" ON)

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
    src/plan.cpp
    src/safety.cpp
    src/settings.cpp
    src/text.cpp
)
target_include_directories(proto_core PUBLIC include)
target_link_libraries(proto_core PUBLIC PkgConfig::CURL nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(proto_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
set_target_properties(proto_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(PROTO_ENABLE_SANITIZERS)
    target_compile_options(proto_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(proto_core PUBLIC -fsanitize=address,undefined)
endif()

add_library(proto_terminal src/editor.cpp src/shell.cpp src/signals.cpp src/tui.cpp)
target_link_libraries(proto_terminal PUBLIC proto_core)
target_compile_options(proto_terminal PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
set_target_properties(proto_terminal PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(PROTO_BUILD_BASH_MODULE AND NOT PROTO_ENABLE_SANITIZERS)
    find_path(BASH_INCLUDE_DIR builtins.h PATH_SUFFIXES bash)
    find_path(READLINE_INCLUDE_DIR readline/readline.h)
    if(BASH_INCLUDE_DIR AND READLINE_INCLUDE_DIR)
        include(CMakePushCheckState)
        include(CheckCXXSourceCompiles)
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_INCLUDES ${READLINE_INCLUDE_DIR})
        check_cxx_source_compiles("
            #include <cstdio>
            #include <readline/readline.h>
            #if !defined(RL_READLINE_VERSION) || RL_READLINE_VERSION < 0x0801
            #error GNU Readline 8.1+ is required
            #endif
            using Lookup = decltype(rl_function_of_keyseq_len);
            using Timeout = decltype(rl_set_keyboard_input_timeout);
            using KeySequence = decltype(rl_key_sequence_length);
            int main() { return 0; }
        " PROTO_HAS_GNU_READLINE)
        cmake_pop_check_state()
    endif()
    if(BASH_INCLUDE_DIR AND READLINE_INCLUDE_DIR AND PROTO_HAS_GNU_READLINE)
        enable_language(C)
        add_library(proto_bash MODULE src/bash_bridge.c src/bash_integration.cpp)
        target_include_directories(proto_bash SYSTEM PRIVATE ${BASH_INCLUDE_DIR}
            ${BASH_INCLUDE_DIR}/include ${BASH_INCLUDE_DIR}/builtins ${READLINE_INCLUDE_DIR})
        target_link_libraries(proto_bash PRIVATE proto_terminal)
        target_compile_options(proto_bash PRIVATE -Wall -Wextra -Wpedantic)
        set_target_properties(proto_bash PROPERTIES PREFIX "" OUTPUT_NAME "proto-bash" SUFFIX ".so")
        if(APPLE)
            target_link_options(proto_bash PRIVATE "-undefined" "dynamic_lookup")
        endif()
        install(TARGETS proto_bash LIBRARY DESTINATION lib/proto)
    else()
        message(STATUS "Bash/GNU Readline headers unavailable: shell/proto.bash will use on-demand completions")
    endif()
endif()

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
        if(TARGET proto_bash)
            set(PROTO_TEST_BASH_MODULE "$<TARGET_FILE:proto_bash>")
        else()
            set(PROTO_TEST_BASH_MODULE "-")
        endif()
        add_test(NAME shell-integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/shell_integration_test.py
                    $<TARGET_FILE:proto> ${PROTO_TEST_BASH_MODULE})
        set_tests_properties(shell-integration PROPERTIES TIMEOUT 90)
    endif()
endif()

install(TARGETS proto RUNTIME DESTINATION bin)
install(FILES shell/proto.bash shell/proto.zsh DESTINATION share/proto)
