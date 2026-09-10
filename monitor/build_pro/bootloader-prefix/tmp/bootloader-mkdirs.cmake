# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/claude/esp-idf-v5.5.2/components/bootloader/subproject"
  "/home/claude/whm-firmware/monitor/build_pro/bootloader"
  "/home/claude/whm-firmware/monitor/build_pro/bootloader-prefix"
  "/home/claude/whm-firmware/monitor/build_pro/bootloader-prefix/tmp"
  "/home/claude/whm-firmware/monitor/build_pro/bootloader-prefix/src/bootloader-stamp"
  "/home/claude/whm-firmware/monitor/build_pro/bootloader-prefix/src"
  "/home/claude/whm-firmware/monitor/build_pro/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/claude/whm-firmware/monitor/build_pro/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/claude/whm-firmware/monitor/build_pro/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
