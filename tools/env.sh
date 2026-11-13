# source this file (Git Bash). Session-only environment for Dapple.
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export JAVA_HOME="D:/Android/openjdk/jdk-17.0.12"
export ANDROID_HOME="$T/android-sdk"
export ANDROID_SDK_ROOT="$T/android-sdk"
export FLUTTER_ROOT="$T/flutter"
export PUB_CACHE="$T/pub-cache"
export GRADLE_USER_HOME="$T/gradle-home"
export ANDROID_USER_HOME="$T/android-user-home"
export ANDROID_AVD_HOME="$T/android-avd"
export FLUTTER_SUPPRESS_ANALYTICS=true
export CLI_DART_DISABLE_TELEMETRY=1
export PATH="$ANDROID_HOME/emulator:$FLUTTER_ROOT/bin:$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmake/3.31.6/bin:$T/llvm-mingw-20261020-ucrt-x86_64/bin:$T/cmake-4.4.3-windows-x86_64/bin:$T/ninja:$PATH"
