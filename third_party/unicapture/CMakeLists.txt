# Derived from hyperion-webos/unicapture/CMakeLists.txt (MIT, (c) 2021 webOS Brew).
# Modified: only the two video-plane backends are built; UI backends removed.
add_library(unicapture STATIC
        unicapture.c
        converter.c
        log.c
        utils.c
)
set(UNICAPTURE_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(unicapture PUBLIC ${UNICAPTURE_INCLUDE_DIRS} ${PMLOG_INCLUDE_DIRS})
target_link_libraries(unicapture PUBLIC yuv dl pthread ${PMLOG_LDFLAGS})

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_library(dile_vt_backend SHARED
        backends/libdile_vt.c
)
target_include_directories(dile_vt_backend PRIVATE ${UNICAPTURE_INCLUDE_DIRS} backends)
target_link_libraries(dile_vt_backend dile_vt)
# libdile_vt.so seems to be missing DT_NEEDED for libPmLogLib.so.3
set_target_properties(dile_vt_backend PROPERTIES LINK_FLAGS "-lPmLogLib")

add_library(vtcapture_backend SHARED
        backends/libvtcapture.cpp
)
target_include_directories(vtcapture_backend PRIVATE ${UNICAPTURE_INCLUDE_DIRS} backends)
target_link_libraries(vtcapture_backend vtcapture)

add_dependencies(unicapture dile_vt_backend vtcapture_backend)
