cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../lvgl)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})
project(raira)
