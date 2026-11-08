cmake_minimum_required(VERSION 3.16)
project(idn_decoder_native_tests C)

# Host-native tests for the real firmware module firmware/ESP32S3/main/idn_decoder.c.
set(IDN_FW_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/../../../../firmware/ESP32S3/main")

add_executable(test_idn_decoder
    test_idn_decoder.c
    "${IDN_FW_MAIN}/idn_decoder.c"
)
target_include_directories(test_idn_decoder PRIVATE "${IDN_FW_MAIN}")
set_target_properties(test_idn_decoder PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_idn_decoder PRIVATE -Wall -Wextra -Werror)
elseif(MSVC)
    target_compile_options(test_idn_decoder PRIVATE /W4 /WX)
endif()

enable_testing()
add_test(NAME idn_decoder COMMAND test_idn_decoder)
