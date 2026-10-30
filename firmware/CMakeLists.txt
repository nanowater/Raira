cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../lvgl)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# one lock file per chip: the S3 and the P4 resolve different components (the P4 gets esp_hosted / esp_wifi_remote)
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})
project(raira)
