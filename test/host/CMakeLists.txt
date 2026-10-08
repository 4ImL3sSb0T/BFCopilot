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
    "${BF_ROOT}/components/flapping/flapping.c"
    "${BF_ROOT}/components/protocol/bf_protocol.c"
)

target_include_directories(bf_host_tests PRIVATE
    "${BF_ROOT}/components/common/include"
    "${BF_ROOT}/components/servo/include"
    "${BF_ROOT}/components/flapping/include"
    "${BF_ROOT}/components/protocol/include"
)

add_executable(bf_flight_tests test_flight.c
    "${BF_ROOT}/components/safety/safety.c"
    "${BF_ROOT}/components/flight/flight.c"
    "${BF_ROOT}/components/common/bf_fault.c"
    "${BF_ROOT}/components/common/bf_crc.c"
    "${BF_ROOT}/components/flapping/flapping.c"
    "${BF_ROOT}/components/protocol/bf_protocol.c")
target_include_directories(bf_flight_tests PRIVATE stubs)
foreach(component common board safety flight servo sensors telemetry parameter flapping protocol radio)
    target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/components/${component}/include")
endforeach()
if(NOT WIN32)
    target_link_libraries(bf_host_tests PRIVATE m)
    target_link_libraries(bf_flight_tests PRIVATE m)
endif()

add_test(NAME bf_host_tests COMMAND bf_host_tests)
add_test(NAME bf_flight_tests COMMAND bf_flight_tests)
