cmake_minimum_required(VERSION 3.13)
project(idn_session_native_tests C)

enable_testing()

set(FIRMWARE_MAIN ${CMAKE_CURRENT_LIST_DIR}/../../../../firmware/ESP32S3/main)

add_executable(test_idn_session
    test_session.c
    ${FIRMWARE_MAIN}/idn_session.c
)

target_include_directories(test_idn_session PRIVATE ${FIRMWARE_MAIN})
set_property(TARGET test_idn_session PROPERTY C_STANDARD 99)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_idn_session PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME idn_session COMMAND test_idn_session)
