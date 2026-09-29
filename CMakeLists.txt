# CMake project for ESP32-P4 Weighing Scale HMI
# Target: ESP32-P4 (Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1)

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

# Reads a UUID from a file at project root and exposes it as DEFAULT_<NAME>.
# Only used when NVS has no saved value (see weight_config.c).
function(read_uuid_file name)
    set(path "${CMAKE_SOURCE_DIR}/${name}")
    if(NOT EXISTS "${path}")
        message(WARNING "${name} file not found - no default ${name} compiled in")
        return()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")

    file(STRINGS "${path}" val LIMIT_COUNT 1)
    string(STRIP "${val}" val)
    string(TOLOWER "${val}" val)
    string(LENGTH "${val}" len)
    # CMake regex has no {n} quantifier, so check shape + length
    if(NOT len EQUAL 36 OR
       NOT val MATCHES "^[0-9a-f]+-[0-9a-f]+-[0-9a-f]+-[0-9a-f]+-[0-9a-f]+$")
        message(FATAL_ERROR "${name} does not contain a valid UUID: '${val}'")
    endif()

    add_compile_definitions(DEFAULT_${name}="${val}")
    message(STATUS "Default ${name}: ${val}")
endfunction()

read_uuid_file(DEVICE_ID)
read_uuid_file(MACHINE_ID)

project(esp-weight_hmi_p4)

target_compile_options(__idf_chmorgan__esp-audio-player PRIVATE
    -Wno-ignored-qualifiers
)

# GCC 14 is overly strict about string truncation. We size buffers
# intentionally and accept truncation where it occurs.
idf_build_set_property(COMPILE_OPTIONS
    "-Wno-error=stringop-truncation;-Wno-error=format-truncation"
    APPEND
)
