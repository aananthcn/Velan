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

# Local override of portaudio/19.7 from Conan Center.
# Changes from upstream:
#   PA_USE_JACK=OFF  — pa_jack.c depends on libjack which is absent from
#                      cross-compile sysroots and missing from upstream package_info().
#   libalsa dep      — upstream relies on a system libasound2-dev install; we declare
#                      libalsa as a proper Conan dependency so it is cross-built for the
#                      target arch (e.g. aarch64) without any manual apt install.

from conan import ConanFile
from conan.tools.apple import is_apple_os
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
from conan.tools.files import copy, get, rmdir
import os

class portaudioRecipe(ConanFile):
    name = "portaudio"
    version = "19.7"
    package_type = "library"

    license = "MIT"
    homepage = "https://www.portaudio.com"
    url = "https://github.com/conan-io/conan-center-index"
    description = "A free, cross-platform, open source, audio I/O library."
    topics = ("audio",)

    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    implements = ["auto_shared_fpic"]
    languages = "C"

    def requirements(self):
        if self.settings.os == "Linux":
            # libalsa is the Conan-managed ALSA library.  The Conan build bakes
            # in a wrong alsa.conf path; the runtime fix is ALSA_CONFIG_DIR set
            # in run_velan.sh (the standard ALSA override mechanism).
            self.requires("libalsa/1.2.10")

    def source(self):
        get(self,
            url="https://files.portaudio.com/archives/pa_stable_v190700_20210406.tgz",
            sha256="47efbf42c77c19a05d22e627d42873e991ec0c1357219c0d74ce6a2948cb2def",
            strip_root=True)

    def layout(self):
        cmake_layout(self, src_folder="src")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.cache_variables["CMAKE_POLICY_VERSION_MINIMUM"] = "3.5"
        tc.cache_variables["PA_BUILD_STATIC"] = not self.options.shared
        tc.cache_variables["PA_BUILD_SHARED"] = self.options.shared
        # Disable JACK: pa_jack.c depends on libjack which is not available in
        # cross-compile sysroots and is not declared in upstream package_info().
        tc.cache_variables["PA_USE_JACK"] = False
        # Force ALSA on Linux; without this, cmake auto-detection can silently
        # skip ALSA when aarch64 ALSA headers are not on the default search path,
        # producing a portaudio with zero functional backends.
        if self.settings.os == "Linux":
            tc.cache_variables["PA_USE_ALSA"] = True
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE.txt", self.source_folder, os.path.join(self.package_folder, "licenses"))
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))
        rmdir(self, os.path.join(self.package_folder, "lib", "share"))

    def package_info(self):
        suffix = ""
        if not self.options.shared and self.settings.os == "Windows":
            suffix = "_static"
        target_name = f"portaudio{suffix}"

        if self.settings.arch in ("x86_64", "armv8") and self.settings.os == "Windows":
            suffix += "_x64"

        libname = f"portaudio{suffix}"
        self.cpp_info.set_property("cmake_target_name", target_name)
        self.cpp_info.libs = [libname]
        if is_apple_os(self):
            self.cpp_info.frameworks = ["CoreAudio", "AudioToolbox", "AudioUnit",
                                        "CoreFoundation", "CoreServices"]
        elif self.settings.os == "Windows":
            self.cpp_info.system_libs = ["winmm", "dsound", "ole32", "uuid", "setupapi"]
        elif self.settings.os == "Linux":
            # libalsa is a Conan dep declared in requirements(); its link flags
            # propagate automatically — no manual "asound" entry needed here.
            self.cpp_info.system_libs = ["pthread", "m"]
