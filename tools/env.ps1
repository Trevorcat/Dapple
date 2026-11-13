# dot-source this file: . .\tools\env.ps1   (session-only)
$T = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:JAVA_HOME = "D:\Android\openjdk\jdk-17.0.12"
$env:ANDROID_HOME = "$T\android-sdk"
$env:ANDROID_SDK_ROOT = "$T\android-sdk"
$env:FLUTTER_ROOT = "$T\flutter"
$env:PUB_CACHE = "$T\pub-cache"
$env:GRADLE_USER_HOME = "$T\gradle-home"
$env:ANDROID_USER_HOME = "$T\android-user-home"
$env:ANDROID_AVD_HOME = "$T\android-avd"
$env:FLUTTER_SUPPRESS_ANALYTICS = "true"
$env:CLI_DART_DISABLE_TELEMETRY = "1"
$env:PATH = "$T\android-sdk\emulator;$T\flutter\bin;$env:JAVA_HOME\bin;$T\android-sdk\platform-tools;$T\android-sdk\cmake\3.31.6\bin;$T\llvm-mingw-20261020-ucrt-x86_64\bin;$T\cmake-4.4.3-windows-x86_64\bin;$T\ninja;$env:PATH"
