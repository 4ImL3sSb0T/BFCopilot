cmake_minimum_required(VERSION 3.16)

# Hardware test firmware for the butterfly PCB.
#
# It links the parts of the product that own hardware - board, parameter, servo and
# sensors - plus the flapping service, so the real trajectory generator can be exercised
# on the servos. The rest of the flight stack is excluded: a bring-up tool must not have
# its hardware operations gated on an arming or fault state, and nothing here runs a
# control loop or a self-check. This project supplies its own link (`net`) and its own
# console commands (`hwtest`, `flap`).
set(BF_BENCH ON)
set(BF_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(EXTRA_COMPONENT_DIRS "${BF_ROOT}/src/bsp" "${BF_ROOT}/src/driver" "${BF_ROOT}/src/service"
                         "${CMAKE_CURRENT_LIST_DIR}/src/app")
set(COMPONENTS app)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bf_bench)
