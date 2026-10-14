cmake_minimum_required(VERSION 3.16)

# Hardware test firmware for the butterfly PCB.
#
# It links only the parts of the product that own hardware - board, parameter, servo and
# sensors - and excludes the flight stack entirely, because a bring-up tool must not have
# its hardware operations gated on an arming or fault state. This project supplies its own
# link (`net`) and its own console commands (`hwtest`).
set(BF_BENCH ON)
set(BF_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(EXTRA_COMPONENT_DIRS "${BF_ROOT}/src/bsp" "${BF_ROOT}/src/driver" "${BF_ROOT}/src/service"
                         "${CMAKE_CURRENT_LIST_DIR}/src/app")
set(COMPONENTS app)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bf_bench)
