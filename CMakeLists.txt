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

# wolfSSL/wolfSSH pairing + the PSRAM crypto arena's condition B, applied
# globally so the MANAGED components see them without local edits:
#  - WOLFSSL_WOLFSSH: compiles wc_SSH_KDF and friends into wolfCrypt
#  - WOLFSSL_TRACK_MEMORY: on FreeRTOS, wolfSSL's XMALLOC bypasses
#    wolfSSL_SetAllocators UNLESS this is set - without it the PSRAM
#    arena is dead code and crypto silently uses the general heap.
idf_build_set_property(COMPILE_DEFINITIONS "WOLFSSL_WOLFSSH" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "WOLFSSL_TRACK_MEMORY" APPEND)
project(whm_bringup)
