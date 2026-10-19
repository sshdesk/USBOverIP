
$env:CONAN_HOME = "C:/cd/"


$profilePath = (Resolve-Path (Join-Path (Get-Location) "../conan-deps/src/profiles/windows_debug_z7")).Path


conan install --requires="sdl/2.28.3" --requires="sdl_ttf/2.22.0" --profile:all=$profilePath --build=missing