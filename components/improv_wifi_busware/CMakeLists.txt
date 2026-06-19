# ESP-IDF component manifest for improv-wifi-busware.
#
# Same source tree is consumed by:
#   - ESP-IDF (CULFW32)        — only src/improv_wifi.cpp + src/backend_idf.cpp compile
#   - PlatformIO/Arduino       — src/arduino_facade.cpp + src/backend_arduino.cpp also compile
#
# Backend translation units are guarded with #ifdef ARDUINO / ESP_PLATFORM
# so the wrong-platform file produces an empty translation unit. The right
# IDF-side requires for esp_wifi/netif/event are listed unconditionally so
# pure-IDF builds link the WiFi backend without depending on arduino-esp32.

idf_component_register(
    SRCS
        "src/improv_wifi.cpp"
        "src/serial_filter.cpp"
        "src/backend_idf.cpp"
        "src/backend_arduino.cpp"
        "src/arduino_facade.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        nvs_flash
        log
        freertos
)
