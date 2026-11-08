cmake_minimum_required(VERSION 3.16)
project(idn_timing_native_tests C)

# Host-native tests for the real firmware module
# firmware/ESP32S3/main/idn_timing.c. No ESP-IDF headers are needed.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(IDN_FW_MAIN "${CMAKE_CURRENT_LIST_DIR}/../../../../firmware/ESP32S3/main" ABSOLUTE)

enable_testing()

add_executable(test_idn_timing
    test_idn_timing.c
    "${IDN_FW_MAIN}/idn_timing.c"
)
target_include_directories(test_idn_timing PRIVATE "${IDN_FW_MAIN}")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_idn_timing PRIVATE -Wall -Wextra -Werror)
elseif(MSVC)
    target_compile_options(test_idn_timing PRIVATE /W4 /WX)
endif()

add_test(NAME idn_timing COMMAND test_idn_timing)
