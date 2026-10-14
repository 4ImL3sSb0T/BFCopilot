cmake_minimum_required(VERSION 3.16)
project(bf_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(BF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(bf_host_tests
    test_main.c
    "${BF_ROOT}/src/bsp/common/bf_crc.c"
    "${BF_ROOT}/src/bsp/common/bf_fault.c"
    "${BF_ROOT}/src/service/flapping/flapping.c"
    "${BF_ROOT}/src/service/protocol/bf_protocol.c"
)

target_include_directories(bf_host_tests PRIVATE
    "${BF_ROOT}/src/bsp/common/include"
    "${BF_ROOT}/src/driver/servo/include"
    "${BF_ROOT}/src/service/flapping/include"
    "${BF_ROOT}/src/service/protocol/include"
)

add_executable(bf_flight_tests test_flight.c
    "${BF_ROOT}/src/service/safety/safety.c"
    "${BF_ROOT}/src/app/flight/flight.c"
    "${BF_ROOT}/src/bsp/common/bf_fault.c"
    "${BF_ROOT}/src/bsp/common/bf_crc.c"
    "${BF_ROOT}/src/service/flapping/flapping.c"
    "${BF_ROOT}/src/service/protocol/bf_protocol.c")
target_include_directories(bf_flight_tests PRIVATE stubs)
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/bsp/common/include")
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/bsp/board/include")
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/driver/servo/include")
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/driver/sensors/include")
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/driver/lsm6ds3tr_c/include")
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/service/safety/include")
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/app/flight/include")
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/service/telemetry/include")
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/service/parameter/include")
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/service/flapping/include")
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/service/protocol/include")
target_include_directories(bf_flight_tests PRIVATE "${BF_ROOT}/src/service/radio/include")
if(NOT WIN32)
    target_link_libraries(bf_host_tests PRIVATE m)
    target_link_libraries(bf_flight_tests PRIVATE m)
endif()

add_test(NAME bf_host_tests COMMAND bf_host_tests)
add_test(NAME bf_flight_tests COMMAND bf_flight_tests)

add_executable(bf_motion_tests test_motion.c "${BF_ROOT}/src/driver/sensors/motion_math.c")
target_include_directories(bf_motion_tests PRIVATE "${BF_ROOT}/src/driver/sensors")
if(NOT WIN32)
    target_link_libraries(bf_motion_tests PRIVATE m)
endif()
add_test(NAME bf_motion_tests COMMAND bf_motion_tests)

add_executable(bf_motion_driver_tests test_motion_driver.c
    "${BF_ROOT}/src/driver/sensors/motion.c" "${BF_ROOT}/src/driver/sensors/motion_math.c"
    "${BF_ROOT}/src/driver/lsm6ds3tr_c/vendor/lsm6ds3tr-c_reg.c")
target_include_directories(bf_motion_driver_tests PRIVATE motion_stubs stubs
    "${BF_ROOT}/src/driver/sensors" "${BF_ROOT}/src/driver/sensors/include"
    "${BF_ROOT}/src/bsp/board/include" "${BF_ROOT}/src/driver/lsm6ds3tr_c/vendor")
if(NOT WIN32)
    target_link_libraries(bf_motion_driver_tests PRIVATE m)
endif()
add_test(NAME bf_motion_driver_tests COMMAND bf_motion_driver_tests)

add_executable(bf_servo_driver_tests test_servo_driver.c "${BF_ROOT}/src/driver/servo/servo.c")
target_include_directories(bf_servo_driver_tests PRIVATE servo_stubs stubs
    "${BF_ROOT}/src/driver/servo/include" "${BF_ROOT}/src/bsp/board/include")
if(NOT WIN32)
    target_link_libraries(bf_servo_driver_tests PRIVATE m)
endif()
add_test(NAME bf_servo_driver_tests COMMAND bf_servo_driver_tests)
