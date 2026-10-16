cmake_minimum_required(VERSION 3.11)
project(lvgl LANGUAGES C CXX)

option(WERROR "Treat warnings as errors for LVGL targets" OFF)
option(ASAN "Enable Sanitizer Options" OFF)
option(LVGL_USE_INSTALLED "Use installed version of LVGL" OFF)
option(LVGL_USE_CONF_H "Use an lv_conf.h instead of kconfig" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

if(ASAN)
    message(STATUS "Enabling Sanitizer")
    add_compile_options(-fsanitize=address,undefined,null)
    add_link_options(-fsanitize=address,undefined,null,leak)
endif()

if(LVGL_USE_INSTALLED)
    find_package(lvgl REQUIRED)
elseif(LVGL_USE_CONF_H)
    set(LV_BUILD_SET_CONFIG_OPTS ON CACHE BOOL "parse lv_conf.h" FORCE)
    add_subdirectory(lvgl)
else()
    set(LV_BUILD_USE_KCONFIG ON CACHE BOOL "use kconfig" FORCE)
    set(DOTCONFIG "${CMAKE_CURRENT_LIST_DIR}/.config")
    set(DEFCONFIG "${CMAKE_CURRENT_LIST_DIR}/configs/get_started.defconfig")
    set(LV_BUILD_KCONFIG_ROOT "${CMAKE_CURRENT_LIST_DIR}/Kconfig" CACHE PATH "path to root kconfig" FORCE)

    if(EXISTS "${DOTCONFIG}")
        message(STATUS "Using .config found to build LVGL")
        set(LV_BUILD_DOTCONFIG_PATH "${DOTCONFIG}" CACHE PATH "path to .config" FORCE)
        unset(LV_BUILD_DEFCONFIG_PATH)
    else()
        message(STATUS "No .config found, using ${DEFCONFIG} to build LVGL")
        set(LV_BUILD_DEFCONFIG_PATH "${DEFCONFIG}" CACHE PATH "path to defconfig" FORCE)
        unset(LV_BUILD_DOTCONFIG_PATH)
    endif()

    add_subdirectory(lvgl)
endif()

if(CONFIG_LV_USE_DEMO_TRUCK)
    add_subdirectory(demos/lv_demo_truck)
    set(EXTRA_LIBS lv_demo_truck_ui)
endif()

file(GLOB LV_LINUX_SRC src/lib/*.c)
set(LV_LINUX_INC src/lib)
set_property(GLOBAL PROPERTY LV_LINUX_BACKEND_SRC "")
set(DISPLAY_BACKENDS src/lib/display_backends)
set(INDEV_BACKENDS src/lib/indev_backends)

function(lv_linux_add_backend dir file)
    set_property(GLOBAL APPEND PROPERTY LV_LINUX_BACKEND_SRC ${dir}/${file})
endfunction()

function(lv_linux_add_backend_if)
    if(ARGC LESS 3)
        message(FATAL_ERROR "lv_linux_add_backend_if: expected <condition...> <folder> <file>, got: ${ARGV}")
    endif()
    set(condition ${ARGV})
    list(POP_BACK condition file)
    list(POP_BACK condition folder)
    if(${condition})
        lv_linux_add_backend(${folder} ${file})
    endif()
endfunction()

# Add every backend in case we're using an installed version of LVGL to avoid having to parse installed lv_conf.h
lv_linux_add_backend_if(LVGL_USE_INSTALLED OR CONFIG_LV_USE_WAYLAND     ${DISPLAY_BACKENDS} wayland.c)
lv_linux_add_backend_if(LVGL_USE_INSTALLED OR CONFIG_LV_USE_LINUX_DRM   ${DISPLAY_BACKENDS} drm.c)
lv_linux_add_backend_if(LVGL_USE_INSTALLED OR CONFIG_LV_USE_LINUX_FBDEV ${DISPLAY_BACKENDS} fbdev.c)
lv_linux_add_backend_if(LVGL_USE_INSTALLED OR CONFIG_LV_USE_SDL         ${DISPLAY_BACKENDS} sdl.c)
lv_linux_add_backend_if(LVGL_USE_INSTALLED OR CONFIG_LV_USE_X11         ${DISPLAY_BACKENDS} x11.c)
lv_linux_add_backend_if(LVGL_USE_INSTALLED OR CONFIG_LV_USE_GLFW        ${DISPLAY_BACKENDS} glfw3.c)
lv_linux_add_backend_if(LVGL_USE_INSTALLED OR CONFIG_LV_USE_EVDEV       ${INDEV_BACKENDS}   evdev.c)

get_property(BACKEND_SRC GLOBAL PROPERTY LV_LINUX_BACKEND_SRC)

# Library type is determined by BUILD_SHARED_LIBS (shared if ON, static if OFF)
add_library(lvgl_linux ${LV_LINUX_SRC} ${BACKEND_SRC})

set(LIBS lvgl::lvgl)

if(CONFIG_LV_BUILD_DEMOS)
list(APPEND LIBS lvgl::demos)
endif()

if(CONFIG_LV_BUILD_EXAMPLES)
list(APPEND LIBS lvgl::examples)
endif()

target_link_libraries(lvgl_linux PUBLIC ${LIBS})
# ---- App core: everything that is the same on the simulator and on the ESP32 ----
# Depends only on LVGL, nlohmann_json and the interfaces in src/platform/ (http.h, jpeg.h).
add_library(music_core STATIC
    src/music_app.cpp
    src/music_ui.cpp
    src/tidal_client.cpp
    src/media_poller.cpp
    src/cover_decoder.cpp
    src/lyrics.cpp
    src/http_util.cpp
    src/ui_fonts.cpp
    src/settings_ui.cpp
    src/fonts/font_mono_16.c
    src/fonts/noto_sans_kr.c
)
target_include_directories(music_core PUBLIC src)
target_link_libraries(music_core PUBLIC lvgl_linux)

# ---- Simulator platform layer: libcurl for HTTP, libjpeg-turbo for JPEG ----
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
add_library(music_platform_sim STATIC
    src/platform_sim/http_curl.cpp
    src/platform_sim/jpeg_turbo.cpp
    src/platform_sim/settings_file.cpp
    src/platform_sim/wifi_mock.cpp
)
target_include_directories(music_platform_sim PUBLIC src)
target_link_libraries(music_platform_sim PUBLIC CURL::libcurl PkgConfig::TURBOJPEG)

add_executable(lvglsim src/main.cpp)
target_link_libraries(lvglsim PUBLIC music_core music_platform_sim lvgl_linux ${EXTRA_LIBS})
# .incbin is invisible to the dependency scanner, so declare the fonts as explicit dependencies
set(NOTO_REGULAR "${CMAKE_CURRENT_LIST_DIR}/assets/fonts/NotoSansKR-Regular-subset.otf")
set(NOTO_BOLD "${CMAKE_CURRENT_LIST_DIR}/assets/fonts/NotoSansKR-Bold-subset.otf")
set_source_files_properties(src/fonts/noto_sans_kr.c PROPERTIES
    COMPILE_DEFINITIONS "NOTO_SANS_KR_PATH=\"${NOTO_REGULAR}\";NOTO_SANS_KR_BOLD_PATH=\"${NOTO_BOLD}\""
    OBJECT_DEPENDS "${NOTO_REGULAR};${NOTO_BOLD}")
target_compile_definitions(music_core PRIVATE LV_LABEL_SCROLL_DELAY=2000 LV_LABEL_WAIT_CHAR_COUNT=8)

if(CONFIG_LV_USE_DEMO_TRUCK)
    target_compile_definitions(lvglsim PUBLIC CONFIG_LV_USE_DEMO_TRUCK)
endif()

if(WERROR)
    target_compile_options(lvglsim PRIVATE -Werror)
    target_compile_options(lvgl_linux PRIVATE -Werror)
    if(TARGET lvgl)
        target_compile_options(lvgl PRIVATE -Werror)
    endif()
endif()


# Install the lvgl_linux library and its headers
install(DIRECTORY src/lib/
    DESTINATION include/lvgl
    FILES_MATCHING
    PATTERN "backends*" EXCLUDE
    PATTERN "*.h")

install(TARGETS lvgl_linux
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/lvglsim DEPENDS lvglsim)
