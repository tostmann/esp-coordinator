# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-coordinator)

# Target guard, narrowed on the wifi-coex branch: this variant is ESP32-C6-only
# (WiFi + 802.15.4 coex; C5/H2 builds come from the master branch). Master's
# three-target guard would let a stray `set-target esp32h2` fail late and
# confusingly inside wifi_coex.cpp / the improv component instead of loudly
# here (merge-review finding, 2026-07-19).
if(NOT IDF_TARGET STREQUAL "esp32c6")
    message(FATAL_ERROR
        "wifi-coex variant is esp32c6-only; run 'idf.py set-target esp32c6'. "
        "For esp32c5/esp32h2 build from the master branch.")
endif()
