# CMake project for ESP32-P4 Weighing Scale HMI
# Target: ESP32-P4 (Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1)

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Pull in custom components folder
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

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
