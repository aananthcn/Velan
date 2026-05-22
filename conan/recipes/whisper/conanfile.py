import os
import subprocess
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get


class WhisperCppConan(ConanFile):
    name     = "whisper.cpp"
    # version is injected via `conan create --version`
    settings = "os", "compiler", "build_type", "arch"
    options  = {
        "shared": [True, False],
        "cuda":   [True, False],
        "hailo8": [True, False],
    }
    default_options = {"shared": False, "cuda": False, "hailo8": False}

    def layout(self):
        cmake_layout(self)

    def source(self):
        get(self,
            url=f"https://github.com/ggerganov/whisper.cpp/archive/refs/tags/v{self.version}.tar.gz",
            strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["BUILD_SHARED_LIBS"]      = self.options.shared
        tc.variables["WHISPER_BUILD_TESTS"]    = False
        tc.variables["WHISPER_BUILD_EXAMPLES"] = False
        tc.variables["GGML_CUDA"]              = self.options.cuda
        tc.variables["GGML_HAILO8"]            = self.options.hailo8
        if self.options.cuda:
            # Restrict CUDA compilation to only the GPUs present on this machine.
            # Without this, whisper.cpp builds for every architecture from sm_52
            # onwards (20+ targets), multiplying nvcc build time unnecessarily.
            try:
                raw = subprocess.check_output(
                    ["nvidia-smi", "--query-gpu=compute_cap", "--format=csv,noheader"],
                    text=True, stderr=subprocess.DEVNULL
                ).strip().splitlines()
                arches = ";".join(line.strip().replace(".", "") for line in raw if line.strip())
                if arches:
                    tc.variables["CMAKE_CUDA_ARCHITECTURES"] = arches
                    self.output.info(f"CUDA architectures restricted to: {arches}")
            except Exception:
                self.output.warning("nvidia-smi not found; whisper.cpp will compile for all CUDA architectures")
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        # whisper.h is in include/ in recent versions (v1.6+)
        copy(self, "whisper.h",
             src=self.source_folder,
             dst=os.path.join(self.package_folder, "include"))
        copy(self, "*.h",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"))
        # ggml public headers live in ggml/include/ in the source tree.
        # whisper.h includes ggml.h so these must be co-located in package/include/.
        copy(self, "*.h",
             src=os.path.join(self.source_folder, "ggml", "include"),
             dst=os.path.join(self.package_folder, "include"))
        # Static or shared libraries from the build tree
        copy(self, "*.a",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)
        copy(self, "*.so*",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name",   "whisper")
        self.cpp_info.set_property("cmake_target_name", "whisper::whisper")
        # GNU ld processes libs left-to-right: a referrer must precede its definer.
        # libggml.a references ggml_backend_cuda_reg → libggml-cuda.a must follow it.
        self.cpp_info.libs = ["whisper", "ggml", "ggml-cpu", "ggml-base"]
        self.cpp_info.system_libs = ["m", "pthread"]
        if self.settings.os == "Linux":
            self.cpp_info.system_libs.append("dl")
        if self.options.cuda:
            # Insert after "ggml" so the CUDA backend symbol is resolved in order.
            self.cpp_info.libs.insert(2, "ggml-cuda")
            # CUDA libs required by libggml-cuda.a (dynamic, found via ldconfig).
            # "cuda" (driver API: cuDeviceGet etc.) must follow "ggml-cuda" so
            # --as-needed keeps it; cudart/cublas/cublasLt likewise.
            self.cpp_info.system_libs += ["cudart", "cublas", "cublasLt", "cuda"]
            # Propagate the CUDA compile definition to all consumers so that
            # #ifdef GGML_USE_CUDA guards in consumer code (e.g. Speech2TextManager)
            # correctly enable GPU inference paths.
            self.cpp_info.defines.append("GGML_USE_CUDA")
