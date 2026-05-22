# cmake/toolchains/rpi.cmake
# Cross-compilation toolchain for Raspberry Pi 5 (AArch64 / armv8)
#
# Prerequisites on the build machine (Ubuntu/Debian):
#   sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
#
# Sysroot (~/sdk/rpi/adas) must be populated first:
#   ./scripts/sync_rpi_sysroot.sh --ip <rpi-ip>

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_STRIP        aarch64-linux-gnu-strip)

# RPi sysroot populated by scripts/sync_rpi_sysroot.sh (~/sdk/rpi/adas).
# CMAKE_SYSROOT passes --sysroot to the compiler so it searches the multiarch
# sub-path (usr/include/aarch64-linux-gnu) automatically — required for glibc
# headers like bits/wchar2-decl.h that live there on Debian/Ubuntu arm64.
set(CMAKE_SYSROOT "$ENV{HOME}/sdk/rpi/adas")
set(CMAKE_FIND_ROOT_PATH "$ENV{HOME}/sdk/rpi/adas")

# Programs (cmake, ninja, etc.) must come from the build machine.
# Libraries and headers must come from the AArch64 sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
