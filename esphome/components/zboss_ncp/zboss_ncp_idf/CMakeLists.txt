# The NCP firmware in ../../../../main, built as an ESP-IDF component for
# ESPHome (CONFIG_NCP_ESPHOME). ESPHome owns the network, so upstream's own
# WiFi (wifi_coex.cpp), Improv and info page are left out; src/ncp_host.cpp
# answers the firmware's wifi_coex_* calls from ESPHome instead.
set(NCP_MAIN "${CMAKE_CURRENT_LIST_DIR}/../../../../main")
get_filename_component(NCP_MAIN "${NCP_MAIN}" ABSOLUTE)

file(GLOB ncp_srcs "${NCP_MAIN}/*.cpp")
list(FILTER ncp_srcs EXCLUDE REGEX "/(wifi_coex|improv_provisioning|web_info)\\.cpp$")

# version.h exactly as main/CMakeLists.txt makes it, from ../version.txt
file(READ "${NCP_MAIN}/../version.txt" ESP_COORD_VERSION_STR)
string(STRIP "${ESP_COORD_VERSION_STR}" ESP_COORD_VERSION_STR)
if(NOT ESP_COORD_VERSION_STR MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "version.txt format unexpected: '${ESP_COORD_VERSION_STR}'")
endif()
set(ESP_COORD_VERSION_MAJOR ${CMAKE_MATCH_1})
set(ESP_COORD_VERSION_MINOR ${CMAKE_MATCH_2})
set(ESP_COORD_VERSION_BUILD ${CMAKE_MATCH_3})
string(TIMESTAMP ESP_COORD_BUILD_DATE "%Y-%m-%d %H:%M:%S")
configure_file("${NCP_MAIN}/version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/include/version.h" @ONLY)

set(priv_reqs nvs_flash esp_partition esp_timer esp_driver_gpio esp_driver_uart
              esp_driver_usb_serial_jtag lwip esp_system)
if(CONFIG_ESP_COEX_SW_COEXIST_ENABLE)
    list(APPEND priv_reqs esp_coex)
endif()

idf_component_register(SRCS ${ncp_srcs} "src/ncp_host.cpp"
                       INCLUDE_DIRS "src" "${NCP_MAIN}" "${CMAKE_CURRENT_BINARY_DIR}/include"
                       PRIV_REQUIRES ${priv_reqs})
