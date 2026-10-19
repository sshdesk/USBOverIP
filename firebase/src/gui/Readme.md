# 1. Direct Conan to a custom cache folder location
$env:CONAN_HOME="C:/cd/"

# 2. Run the install command targeting your specific profile and force building missing dependencies
conan install --requires="sdl/2.28.3" --requires="sdl_ttf/2.22.0" --profile:all="E:\workspace\shared-usb\firebase\conan-deps\src\profiles\windows_debug_z7" --build=missing

