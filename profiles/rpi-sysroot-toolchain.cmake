# Copyright 2026 Aananth C N
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# rpi-sysroot-toolchain.cmake
# Chained into Conan's generated toolchain for RPi cross-compilation via:
#   build_velan.sh --target rpi  (passes --conf user_toolchain to conan install)
#
# Build environment: Ubuntu 22.04 (build machine), Ubuntu 24.04 (RPi sysroot)
# Cross-compiler:    GCC 12 aarch64 (highest available on Ubuntu 22.04)
# Sysroot runtime:   glibc 2.39 / GLIBCXX 3.4.33 (GCC 14 from Ubuntu 24.04)
#
# The GCC 12 cross-compiler ships its own older aarch64 stubs (glibc 2.35,
# GLIBCXX 3.4.30) that appear earlier in the library search path than the
# sysroot's libs.  This would cause version-symbol link errors for binaries
# that link against sysroot shared libs (e.g. Qt6) which reference newer
# symbols.  The solution — well-established for this cross-compilation scenario
# — is --allow-shlib-undefined at link time (see CMAKE_EXE_LINKER_FLAGS_INIT).
#
# VELAN_RPI_SYSROOT may be overridden at cmake configure time:
#   cmake -DVELAN_RPI_SYSROOT=/path/to/sysroot ...

if(NOT DEFINED VELAN_RPI_SYSROOT)
    set(VELAN_RPI_SYSROOT "$ENV{HOME}/sdk/rpi/adas")
endif()

set(CMAKE_SYSROOT        "${VELAN_RPI_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${VELAN_RPI_SYSROOT}")

# Pass --sysroot to the compiler so GCC searches BOTH
#   ${VELAN_RPI_SYSROOT}/usr/include  AND
#   ${VELAN_RPI_SYSROOT}/usr/include/aarch64-linux-gnu
# automatically, resolving all multiarch headers (bits/wchar2-decl.h etc.)
# from a single consistent glibc 2.39 source without mixing build-machine headers.
set(CMAKE_C_FLAGS_INIT   "--sysroot=${VELAN_RPI_SYSROOT}")
set(CMAKE_CXX_FLAGS_INIT "--sysroot=${VELAN_RPI_SYSROOT}")

# Linker flags for cross-compilation:
#
#   --sysroot          : redirect default library search to sysroot
#
#   -rpath-link        : highest-priority dir list for resolving shared-library
#                        transitive deps at link time.  Three dirs cover all
#                        Ubuntu-24.04 RPi sysroot locations:
#                          usr/lib/aarch64-linux-gnu — Qt6, libstdc++, most libs
#                          lib/aarch64-linux-gnu      — libc.so.6, libpthread, libm
#                          usr/lib/gcc/aarch64-linux-gnu/14 — GCC-14 runtime
#
#   --allow-shlib-undefined : suppress link-time version-symbol errors that
#                        arise because the build machine's GCC 12 cross-compiler
#                        ships older runtime stubs (glibc 2.35, GLIBCXX 3.4.30)
#                        that pre-date the RPi sysroot (glibc 2.39, GLIBCXX 3.4.33).
#                        The sysroot's shared libs (Qt6, libsystemd, libmount …)
#                        reference newer symbols (GLIBC_2.36+, GLIBCXX_3.4.32+)
#                        that will be satisfied at runtime on the RPi; suppressing
#                        the link-time check is the standard cross-compilation
#                        approach when host and target runtimes don't match.
#                        Our own compiled code still gets full undefined-symbol
#                        checking — --allow-shlib-undefined only relaxes checks
#                        on already-linked .so files.
# Use CACHE FORCE (not the _INIT variant) so that these flags are re-applied
# on every cmake re-configure — not just the first run.  The _INIT variant
# is ignored when the cache variable already exists from a previous configure.
set(CMAKE_EXE_LINKER_FLAGS
    "--sysroot=${VELAN_RPI_SYSROOT} \
     -Wl,-rpath-link,${VELAN_RPI_SYSROOT}/usr/lib/aarch64-linux-gnu \
     -Wl,-rpath-link,${VELAN_RPI_SYSROOT}/lib/aarch64-linux-gnu \
     -Wl,-rpath-link,${VELAN_RPI_SYSROOT}/usr/lib/gcc/aarch64-linux-gnu/14 \
     -Wl,--allow-shlib-undefined"
    CACHE STRING "Cross-compilation linker flags (forced by rpi-sysroot-toolchain)" FORCE)

# PROGRAM NEVER — host programs (protoc, grpc_cpp_plugin, ninja, moc, rcc) must
# come from the build machine, not the sysroot.  Conan's generated toolchain
# defaults this to BOTH; override with FORCE so it survives Conan's include order.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER CACHE STRING "" FORCE)

# PACKAGE NEVER — the sysroot contains system cmake package configs (e.g.
# gRPCConfig.cmake) that reference aarch64-only executables (grpc_cpp_plugin)
# which cannot run on x86_64.  NEVER means all find_package calls use only the
# Conan-managed build dir; libraries and headers still come from the sysroot via
# LIBRARY/INCLUDE modes (BOTH, Conan default).
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER CACHE STRING "" FORCE)

# Qt6_DIR — point directly at the aarch64 Qt6 cmake dir inside the sysroot.
# Set with FORCE in CACHE so it takes priority over any CMAKE_PREFIX_PATH search
# and survives Conan's toolchain processing.
# QT_HOST_PATH is passed separately from build_velan.sh to supply the x86_64
# host tools (moc, rcc, qmlimportscanner).
set(Qt6_DIR
    "${VELAN_RPI_SYSROOT}/usr/lib/aarch64-linux-gnu/cmake/Qt6"
    CACHE PATH "Qt6 cmake dir in RPi sysroot" FORCE)
