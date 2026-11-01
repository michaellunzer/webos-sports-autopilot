cmake_minimum_required(VERSION 3.6)
project(webos-nfl-autopilot VERSION 0.1.0 LANGUAGES C CXX)

# HOST_BUILD=ON builds only the portable core (detector + autopilot), the unit
# tests and the replay tool, so they can run on a normal PC.
# The default (cross-compiling with the webOS buildroot-nc4 toolchain) builds
# the on-TV service and the capture backends.
if(CMAKE_CROSSCOMPILING)
    set(_host_default OFF)
else()
    set(_host_default ON)
endif()
option(HOST_BUILD "Build host-side core, tests and tools only" ${_host_default})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(nflap_core STATIC
    src/core/detector.c
    src/core/autopilot.c
    src/core/frame_io.c
)
target_include_directories(nflap_core PUBLIC src/core)
target_link_libraries(nflap_core PUBLIC m)
target_compile_options(nflap_core PRIVATE -Wall -Wextra)

if(HOST_BUILD)
    enable_testing()
    foreach(t test_detector test_autopilot)
        add_executable(${t} tests/${t}.c)
        target_link_libraries(${t} nflap_core)
        target_compile_options(${t} PRIVATE -Wall -Wextra)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    add_executable(replay tools/replay.c)
    target_link_libraries(replay nflap_core)
    return()
endif()

# ---------------------------------------------------------------------------
# On-TV build (webOS, armv7). Mirrors hyperion-webos' build setup.
# ---------------------------------------------------------------------------
set(COMMON_FLAGS "-mcpu=cortex-a9 -mfloat-abi=softfp -mfpu=neon")
add_compile_options(-mcpu=cortex-a9 -mfloat-abi=softfp -mfpu=neon)
add_definitions(-DSECURITY_COMPATIBILITY)

add_subdirectory(third_party/libyuv)
add_subdirectory(third_party/tv-native-apis)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB2 REQUIRED glib-2.0)
pkg_check_modules(GTHREAD2 REQUIRED gthread-2.0)
pkg_check_modules(PBNJSON REQUIRED pbnjson_c)
pkg_check_modules(LS2 REQUIRED luna-service2)
pkg_check_modules(PMLOG REQUIRED PmLogLib)

add_subdirectory(third_party/unicapture)

add_executable(nfl-autopilot
    src/tv/main.c
    src/tv/service.c
    src/tv/settings.c
    src/tv/keys.c
    src/tv/remote_listen.c
    src/tv/luna_util.c
    src/tv/debug_http.c
    src/tv/recorder.c
)
target_include_directories(nfl-autopilot PRIVATE
    src/tv
    ${GLIB2_INCLUDE_DIRS} ${GTHREAD2_INCLUDE_DIRS} ${PBNJSON_INCLUDE_DIRS}
    ${LS2_INCLUDE_DIRS} ${PMLOG_INCLUDE_DIRS})
target_compile_options(nfl-autopilot PRIVATE -Wall -Wextra)
target_compile_definitions(nfl-autopilot PRIVATE NFLAP_VERSION="${PROJECT_VERSION}")
set_target_properties(nfl-autopilot PROPERTIES
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN:$ORIGIN/lib"
    BUILD_RPATH "$ORIGIN:$ORIGIN/lib"
)
target_link_libraries(nfl-autopilot
    nflap_core unicapture pthread dl rt
    ${GTHREAD2_LDFLAGS} ${PBNJSON_LDFLAGS} ${LS2_LDFLAGS} ${GLIB2_LDFLAGS} ${PMLOG_LDFLAGS})
