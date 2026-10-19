from conan import ConanFile
from conan.tools.files import copy
import os
from conan.tools.cmake import CMake

class StreamingHostRecipe(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"

    def requirements(self):
        self.requires("openssl/3.6.2")
        self.requires("opus/1.5.2")

        self.requires("google-webrtc/136")

        self.requires("zlib/1.3.1")
        self.requires("spdlog/1.17.0")

        #self.requires("sdl/2.30.8")
        self.requires("sdl/2.28.3")
        self.requires("sdl_ttf/2.22.0")
      

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def generate(self):
        bin_dir = os.path.join(self.build_folder, "bin")
        licenses_dir = os.path.join(bin_dir, "licenses")

        for dep in self.dependencies.values():
            if len(dep.cpp_info.bindirs):
                copy(self, "*.dll", dep.cpp_info.bindirs[0], bin_dir)
                copy(self, "*.exe", dep.cpp_info.bindirs[0], bin_dir)

            pkg_folder = dep.package_folder

            license_paths = [
                os.path.join(pkg_folder, "licenses", "LICENSE"),
                os.path.join(pkg_folder, "licenses", "license.txt"),
                os.path.join(pkg_folder, "LICENSE"),
                os.path.join(pkg_folder, "license.txt"),
                os.path.join(pkg_folder, "COPYING"),
                os.path.join(pkg_folder, "COPYING.txt"),
            ]
            
            for root, _, files in os.walk(pkg_folder):
                for file in files:
                    if file.lower().startswith("license") or file.lower().startswith("copying"):
                        license_paths.append(os.path.join(root, file))
            
            for license_path in license_paths:
                if os.path.exists(license_path):
                    dest_dir = os.path.join(licenses_dir, dep.ref.name)
                    os.makedirs(dest_dir, exist_ok=True)
                    
                    copy(self, os.path.basename(license_path), os.path.dirname(license_path), dest_dir)
