# version.h wird aus version.h.in via configure_file generiert. Quelle ist
# version.txt + build_number.txt im Projekt-Root (bumped by scripts/bump_build.sh,
# aufgerufen vom idf.py-Wrapper in idf_env.sh vor jedem build/flash).
# Generierter Header landet in BINARY_DIR und wird ueber INCLUDE_DIRS exportiert.
set(VERSION_FILE   "${CMAKE_SOURCE_DIR}/version.txt")
set(BUILD_NUM_FILE "${CMAKE_SOURCE_DIR}/build_number.txt")

if(EXISTS ${VERSION_FILE})
    file(READ ${VERSION_FILE} ESP_COORD_VERSION_STR)
    string(STRIP "${ESP_COORD_VERSION_STR}" ESP_COORD_VERSION_STR)
else()
    set(ESP_COORD_VERSION_STR "1.1.0")
endif()

# Parse MAJOR.MINOR.BUILD aus version.txt.
if(NOT ESP_COORD_VERSION_STR MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(WARNING "version.txt format unexpected: '${ESP_COORD_VERSION_STR}' — falling back to 1.1.0")
    set(ESP_COORD_VERSION_STR "1.1.0")
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _ "${ESP_COORD_VERSION_STR}")
endif()
set(ESP_COORD_VERSION_MAJOR ${CMAKE_MATCH_1})
set(ESP_COORD_VERSION_MINOR ${CMAKE_MATCH_2})
set(ESP_COORD_VERSION_BUILD ${CMAKE_MATCH_3})

string(TIMESTAMP ESP_COORD_BUILD_DATE "%Y-%m-%d %H:%M:%S")

set(VERSION_HDR_OUT "${CMAKE_CURRENT_BINARY_DIR}/include/version.h")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/version.h.in"
    "${VERSION_HDR_OUT}"
    @ONLY
)

# CMake reconfigure triggern wenn build_number.txt oder version.txt sich aendern
# (passiert ueber bump_build.sh) → configure_file laeuft neu → version.h ist
# auf der neuen Build-Nummer beim naechsten Compile.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${BUILD_NUM_FILE} ${VERSION_FILE})

idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/include"
                       EMBED_FILES "busware.png")

# Damit der generierte Header VOR dem Compile existiert (configure_file haengt
# nicht am Component-Lib-Target).
add_custom_target(version_hdr ALL DEPENDS "${VERSION_HDR_OUT}")
add_dependencies(${COMPONENT_LIB} version_hdr)