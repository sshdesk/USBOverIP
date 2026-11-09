
rm -rf build
mkdir -p  build/Debug

cmake -B build -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POSITION_INDEPENDENT_CODE=ON 

cmake --build build --config Debug


cd ..
