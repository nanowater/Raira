# The ESP32 side of the interfaces in src/platform/ (the simulator's counterpart is src/platform_sim/): what the portable core calls and
# the firmware has to provide. Each file says which interface it implements.
set(SRC ${CMAKE_CURRENT_LIST_DIR}/../../../src)

idf_component_register(
    SRCS
        jpeg_esp.cpp
        http_esp.cpp
        ${SRC}/fonts/font_image.cpp
        font_partition.cpp
        settings_nvs.cpp
    INCLUDE_DIRS .
    REQUIRES music_core
    PRIV_REQUIRES esp_partition esp_rom esp_http_client esp-tls mbedtls nvs_flash
    # The core only CALLS these functions and this component does not know the core, so the linker may reach the core after it has
    # passed this library and then never takes the functions out of it: link every file of the library.
    WHOLE_ARCHIVE
)

target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
