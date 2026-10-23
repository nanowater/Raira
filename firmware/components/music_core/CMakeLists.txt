# The portable core of the music controller, compiled from the SAME files as the simulator (src/ in the repository root; nothing is
# copied). The simulator's platform parts (src/platform_sim/, src/main.cpp, src/lib/) are not part of it: the firmware brings its own.
set(SRC ${CMAKE_CURRENT_LIST_DIR}/../../../src)

idf_component_register(
    SRCS
        ${SRC}/music_app.cpp
        ${SRC}/music_ui.cpp
        ${SRC}/browse_ui.cpp
        ${SRC}/tidal_client.cpp
        ${SRC}/bridge_client.cpp
        ${SRC}/hub_source.cpp
        ${SRC}/browse_worker.cpp
        ${SRC}/media_poller.cpp
        ${SRC}/cover_decoder.cpp
        ${SRC}/lyrics.cpp
        ${SRC}/http_util.cpp
        ${SRC}/ui_fonts.cpp
        ${SRC}/settings_ui.cpp
        ${SRC}/fonts/font_mono_16.c
        # ${SRC}/fonts/noto_sans_kr.c is left out for now: it embeds 7 MB of fonts into the app (.incbin); they go to the `fonts`
        # partition instead (docs/PORTING.md, "Flash layout")
    INCLUDE_DIRS ${SRC}
    REQUIRES lvgl nlohmann_json
)

# one C++ standard for the simulator and the firmware (ESP-IDF's default is C++26)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
# the same two scroll settings the simulator build gives the core (CMakeLists.txt in the repository root)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LABEL_SCROLL_DELAY=2000 LV_LABEL_WAIT_CHAR_COUNT=8)
