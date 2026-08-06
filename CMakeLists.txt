# CMake project for ESP32-P4 Weighing Scale HMI
# Target: ESP32-P4 (Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1)

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Pull in custom components folder
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

# Default device ID, read from DEVICE_ID file at project root.
# Only takes effect on first boot / if NVS has no saved device ID -
# manually editing the Device ID on the HMI's Settings screen always
# takes priority once saved. Edit DEVICE_ID before flashing each unit.
# This one is fine as a compile define since it's edited rarely
# (once per physical unit, right before flashing that unit).
if(EXISTS "${CMAKE_SOURCE_DIR}/DEVICE_ID")
    file(STRINGS "${CMAKE_SOURCE_DIR}/DEVICE_ID" DEFAULT_DEVICE_ID)
    string(STRIP "${DEFAULT_DEVICE_ID}" DEFAULT_DEVICE_ID)
    add_compile_definitions(DEFAULT_DEVICE_ID="${DEFAULT_DEVICE_ID}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/DEVICE_ID")
endif()

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
