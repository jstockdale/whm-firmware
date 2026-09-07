cmake_minimum_required(VERSION 3.16)
# Single source of truth for the version: version.txt (one line).
# Feeds IDF's app descriptor (PROJECT_VER) and the WHM_VERSION_STR macro,
# so the home screen, sysinfo, and esptool app_init can never disagree again.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" WHM_VERSION LIMIT_COUNT 1)
# make version.txt a configure dependency, or incremental builds keep the old value
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/version.txt")
set(PROJECT_VER "${WHM_VERSION}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(whm_bringup)
