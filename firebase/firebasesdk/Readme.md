Final build once you have comlete conand repository

$env:CONAN_HOME="C:\cr\"

python ../conan-deps/src/conan-cache-generator.py --type windows_release --download --unpack

python ../conan-deps/src/conan-cache-generator.py --build-project --type windows_release




*************************************************************************************************

for release
delete C:\cr\

cd C:\cr\profiles\default

[settings]
arch=x86_64
build_type=Release
compiler=msvc
compiler.cppstd=20
compiler.runtime=dynamic
compiler.version=194
os=Windows


downlaod conan-cache-msvc193-windows_release.7z

python ../conan-deps/src/conan-cache-generator.py --type windows_release --download --unpack

python ../conan-deps/src/conan-cache-generator.py --build-project --type windows_release

conan install --requires="sdl/2.28.3" --requires="sdl_ttf/2.22.0" --profile:all=..\conan-deps\src\profiles\windows_release_z7 --build=missing


for sdl compile issue, go to C:\cr\p\sdl* change of sdl in yaml


# 1. Point your command line session to the script's release cache directory
$env:CONAN_HOME="C:\cr\"

# 2. Export the universal Firebase wrapper recipe into this new environment
conan export E:\workspace\shared-usb\firebase\firebasesdk --version=13.12.0

# 3. Force package processing inside C:\cr\ to generate the static binary link block
cd E:\workspace\shared-usb\firebase\firebasesdk
conan create . --version=13.12.0

# 4. Navigate into your project repository source tree directory
cd E:\workspace\shared-usb\firebase\src

# 5. Run the exact project build toolchain process for release binaries
python ../conan-deps/src/conan-cache-generator.py --build-project --type windows_release



# 6. Run the cache builder phase to populate C:\cr\ with your public libraries
python ../conan-deps/src/conan-cache-generator.py --build-cache --type windows_release



cd E:\workspace\shared-usb\firebase\conan-deps\src
python conan-cache-generator.py --pack --type windows_release

*****************************************************************************






[settings]
arch=x86_64
build_type=Debug
compiler=msvc
compiler.cppstd=20
compiler.runtime=dynamic
compiler.version=194
os=Windows



# 1. Ensure the session matches your target architecture cache
$env:CONAN_HOME="C:\cd\"

# 2. Update the sandbox instance tracking definitions
conan export E:\workspace\shared-usb\firebase\firebasesdk --version=13.12.0

# 3. Create the package binary using the dynamic workspace path rules
cd E:\workspace\shared-usb\firebase\firebasesdk
conan create . --version=13.12.0

# 4. Return to your src folder and test your build script automation
cd E:\workspace\shared-usb\firebase\src
python ../conan-deps/src/conan-cache-generator.py --build-project --type windows_debug





cd E:\workspace\shared-usb\firebase\conan-deps\src
python conan-cache-generator.py --pack --type windows_debug







