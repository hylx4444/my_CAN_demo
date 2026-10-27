# 文件作用：定义 WEMOS S2 Mini 的 ESP-IDF 顶层构建入口。
# 包含模块：CMake 版本要求、ESP-IDF 构建加载、工程命名、目标芯片检查。
# 调用方式：由 idf.py build 读取；目标芯片必须为 esp32s2。
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(can_sensor_web_s2)
if(NOT IDF_TARGET STREQUAL "esp32s2")
    message(FATAL_ERROR "S2 Mini requires: idf.py set-target esp32s2")
endif()
