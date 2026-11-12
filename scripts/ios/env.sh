# source on the Mac: toolchains under ~/dapple/tools only
export DAPPLE_HOME="$HOME/dapple"; T="$DAPPLE_HOME/tools"
export PATH="$T/cmake/CMake.app/Contents/bin:$T/ninja:$T/flutter/bin:$T/vulkansdk/macOS/bin:$PATH"
export PUB_CACHE="$DAPPLE_HOME/pub-cache"
export FLUTTER_SUPPRESS_ANALYTICS=true CLI_DART_DISABLE_TELEMETRY=1
export CP_HOME_DIR="$DAPPLE_HOME/cocoapods"
