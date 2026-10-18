# coinsim.exe: the coin firmware (firmware/main) built for Windows with clang (llvm-mingw).
#
# Needs a firmware build first (`scripts/idf firmware build`): it supplies the configuration
# (firmware/build/config/sdkconfig.h) and the managed components (LVGL, the codec headers).
#
#   cmake -G Ninja -S sim/coin -B sim/coin/build -DCMAKE_C_COMPILER=<llvm-mingw>/bin/clang.exe
#   cmake --build sim/coin/build
cmake_minimum_required(VERSION 3.20)
project(coinsim C)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware)
set(FW_SDKCONFIG ${FW}/build/config/sdkconfig.h)
set(MC ${FW}/managed_components)
if(NOT EXISTS ${FW_SDKCONFIG} OR NOT EXISTS ${MC}/lvgl__lvgl)
    message(FATAL_ERROR "Build the firmware once first (scripts/idf firmware build): the simulator uses its sdkconfig.h and managed components.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------- libopus (the coin's encoder is libopus too, inside esp_audio_codec) ----------
include(FetchContent)
set(OPUS_SOURCE "https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz" CACHE STRING "opus 1.5.2 tarball (URL or local path)")
FetchContent_Declare(opus URL ${OPUS_SOURCE}
    URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opus)

# ---------- version: as IDF derives PROJECT_VER, plus "-sim" ----------
execute_process(COMMAND git describe --always --tags --dirty WORKING_DIRECTORY ${FW}
    OUTPUT_VARIABLE FW_VER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT FW_VER)
    set(FW_VER "unknown")
endif()

set(SIM_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FW}/main
    ${MC}/lvgl__lvgl
    ${MC}/lvgl__lvgl/src
    ${MC}/espressif__esp_codec_dev/include
    ${MC}/espressif__esp_codec_dev/interface
    ${MC}/espressif__esp_audio_codec/include
    ${MC}/espressif__esp_audio_codec/include/encoder
    ${MC}/espressif__esp_audio_codec/include/encoder/impl
    ${MC}/espressif__esp_audio_codec/include/decoder
    ${MC}/espressif__esp_audio_codec/include/decoder/impl)
set(SIM_DEFINES
    COINSIM=1
    "COINSIM_FW_SDKCONFIG=\"${FW_SDKCONFIG}\""
    "LV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"sdkconfig.h\""
    _CRT_SECURE_NO_WARNINGS
    _CRT_RAND_S)

# ---------- LVGL, from the firmware's managed component, with the firmware's configuration ----------
file(GLOB_RECURSE LVGL_SRC ${MC}/lvgl__lvgl/src/*.c)
add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC ${SIM_INCLUDES})
target_compile_definitions(lvgl PUBLIC ${SIM_DEFINES})
target_compile_options(lvgl PRIVATE -w)

# ---------- the firmware and its simulated hardware ----------
set(FW_SRC app_main.c battlog.c pmic.c input.c imu.c ui.c kbd.c eyes.c diag.c audio.c ogg.c clipstore.c power.c ble.c settings.c scene.c)
list(TRANSFORM FW_SRC PREPEND ${FW}/main/)
set(SIM_SRC main.c simlink.c freertos.c esp_core.c storage.c periph.c audio_dev.c display_sim.c ble_link_sim.c net_sim.c)
list(TRANSFORM SIM_SRC PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/src/)

# cJSON from the ESP-IDF the firmware was built with (scene.c parses scenes with it).
file(READ ${FW}/build/project_description.json FW_DESC)
string(JSON IDF_DIR GET "${FW_DESC}" idf_path)
set(CJSON_DIR ${IDF_DIR}/components/json/cJSON)
add_library(cjson STATIC ${CJSON_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${CJSON_DIR})
target_compile_options(cjson PRIVATE -w)

add_executable(coinsim ${FW_SRC} ${SIM_SRC})
target_compile_definitions(coinsim PRIVATE "PROJECT_VER=\"${FW_VER}-sim\"")
target_compile_options(coinsim PRIVATE -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter)
set_source_files_properties(${FW_SRC} PROPERTIES COMPILE_OPTIONS "-include;sim_force.h")
target_link_libraries(coinsim PRIVATE lvgl opus cjson ws2_32 winmm)
target_link_options(coinsim PRIVATE -static)
