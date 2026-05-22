from conan import ConanFile


class VelanConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    options  = {"cuda": [True, False], "hailo8": [True, False]}
    default_options = {"cuda": False, "hailo8": False}
    generators = "CMakeDeps", "CMakeToolchain", "VirtualBuildEnv"

    def requirements(self):
        self.requires("grpc/1.54.3")           # brings protobuf as transitive dep
        self.requires("portaudio/19.7")
        self.requires("libcurl/8.6.0")
        self.requires("nlohmann_json/3.11.3")
        self.requires("whisper.cpp/1.7.4")     # local recipe in conan/recipes/whisper/

    def build_requirements(self):
        # grpc_cpp_plugin must run on the build machine.
        self.tool_requires("grpc/1.54.3")
        # protoc lives in the protobuf package, not in grpc. Adding it as a
        # direct tool_requires causes VirtualBuildEnv to put protobuf's bin/
        # on PATH so find_program(protoc) resolves to the Conan binary (3.21.12)
        # instead of the system one (3.12.4 on Ubuntu 22.04).
        self.tool_requires("protobuf/3.21.12")

    def configure(self):
        # Forward aicore options into the whisper.cpp package options so the
        # library is compiled with the right hardware backend.
        self.options["whisper.cpp"].cuda   = self.options.cuda
        self.options["whisper.cpp"].hailo8 = self.options.hailo8
