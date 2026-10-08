cmake_minimum_required(VERSION 3.16)
project(bf_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(BF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(bf_host_tests
    test_main.c
    "${BF_ROOT}/components/common/bf_crc.c"
    "${BF_ROOT}/components/common/bf_fault.c"
)

target_include_directories(bf_host_tests PRIVATE
    "${BF_ROOT}/components/common/include"
    "${BF_ROOT}/components/servo/include"
)

add_test(NAME bf_host_tests COMMAND bf_host_tests)
