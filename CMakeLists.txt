# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-coordinator)

# Multi-target guard. This firmware supports esp32c6 (primary, shipped) and
# esp32c5. CONFIG_IDF_TARGET is intentionally NOT pinned in sdkconfig.defaults
# so one defconfig serves both, which means a bare `idf.py build` with no prior
# `idf.py set-target esp32c6|esp32c5` would silently fall back to the IDF
# default chip. Turn that into a loud, immediate failure instead of a
# wrong-chip image.
if(NOT IDF_TARGET STREQUAL "esp32c6" AND NOT IDF_TARGET STREQUAL "esp32c5")
    message(FATAL_ERROR
        "Unsupported IDF_TARGET '${IDF_TARGET}'. "
        "Run 'idf.py set-target esp32c6' (primary) or 'idf.py set-target esp32c5' first.")
endif()
