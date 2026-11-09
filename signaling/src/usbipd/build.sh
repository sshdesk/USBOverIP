
mkdir -p build


cmake -B build -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Debug  -DRTC_STATIC=1 -DCMAKE_POSITION_INDEPENDENT_CODE=ON 
cmake --build build --config Debug



