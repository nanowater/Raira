cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../lvgl)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(music_controller)
