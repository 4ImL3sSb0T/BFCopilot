# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 重构中间态：driver/service/app 可能还不存在，而 ESP-IDF v6 遇到
# EXTRA_COMPONENT_DIRS 里不存在的目录会直接报错，所以只登记实际存在的层。
set(EXTRA_COMPONENT_DIRS src/lib src/bsp)
foreach(_layer src/driver src/service src/app)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${_layer}")
        list(APPEND EXTRA_COMPONENT_DIRS "${_layer}")
    endif()
endforeach()

# app 层恢复后只构建 app（它会依赖其余组件）；在此之前不指定 COMPONENTS，
# 让 menuconfig / SDK Configuration Editor 可以照常工作。
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/app")
    set(COMPONENTS app)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(BFCopilot)
