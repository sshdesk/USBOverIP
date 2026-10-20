# widnows build process

cmd 

set GITHUB_TOKEN=ghp_*******************

git submodule update --init

cd src


$env:CONAN_HOME = "C:/cd/"


$profilePath = (Resolve-Path (Join-Path (Get-Location) "../conan-deps/src/profiles/windows_debug_z7")).Path


conan install --requires="sdl/2.28.3" --requires="sdl_ttf/2.22.0" --profile:all=$profilePath --build=missing


python ../conan-deps/src/conan-cache-generator.py --type windows_debug --download --unpack

python ../conan-deps/src/conan-cache-generator.py --build-project --type windows_debug



# to start usbipd 

./usbipd


# to start usbip-win2

usbip-win2 list -r  192.168.0.19 

usbip-win2 attach -r 192.168.0.19 --select

usbip-win2 detach -a




# For linux 

TBD

conan remove firebase-cpp-sdk/13.12.0 --confirm


conan create .



conan cache clean "openssl/*" --build
conan remove "openssl/*" --confirm




for windows 
$env:CONAN_HOME="C:\Users\akumr\streaming_host\conan-deps\src\temp\conan-cache-msvc193-release"
$env:CONAN_HOME="E:\workspace\shared-usb\cd"
conan list "*:*"


cd E:\workspace\shared-usb\firebase\src


conan install . --build=never -s build_type=RelWithDebInfo

 conan install . --output-folder=build --build=missing -s build_type=Debug -s os=Windows

conan install . --output-folder=build --build=missing -s build_type=Release

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release


cmake --build build




#It is  possible to build your main application or source code in Debug mode while consuming its Conan dependencies in Release mode

conan install . -s '&:build_type=Debug' -s build_type=Release --output-folder=build

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Debug
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=build/Debug/generators/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Debug

cmake --build build







conan install . --output-folder=build --build=missing --build=never=google-webrtc* -s build_type=Release

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
          



conan build . --output-folder=build

cp ../google-services.json .


windows 

conan install . --output-folder=build --build=missing -s build_type=Release
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake
cmake --build . --config Release
xcopy ..\google-services.json .\Release\ /Y







#GitHub Pages (Free Permanent Hosting) 
 
Step 1: Create a new repository on GitHub and upload both index.html and app.js. 

Step 2: Go to your repository Settings -> Pages (under Code and automation).

Step 3: Under Build and deployment > Branch, select main (or master) and / (root)

Step 4: Click Save. Your site will be live at 

https://<username>.github.io/<repo-name>/ in ~1 minute.

Crucial Step for Firestore Rules:
Because your signaling server uses Firebase Firestore, ensure your Firestore security rules allow read/write access. Go to the Firebase Console -> Firestore Database -> Rules and set:  JavaScriptrules_version = '2';
service cloud.firestore {
  match /databases/{database}/documents {
    match /rooms/{roomId}/{document=**} {
      allow read, write: if true;
    }
  }
}
