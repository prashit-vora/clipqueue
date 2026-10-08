#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/ClipQueue.app/Contents/MacOS
cc -Os -std=c11 -Wall -Wextra -Wno-deprecated-declarations -mmacosx-version-min=11.0 \
    -arch x86_64 -arch arm64 -Isrc src/macos.c src/core.c src/sha256.c \
    -framework ApplicationServices -framework CoreFoundation -framework ImageIO \
    -o build/ClipQueue.app/Contents/MacOS/clipqueue
cp platform/macos/Info.plist build/ClipQueue.app/Contents/Info.plist
codesign --force --sign - --identifier local.clipqueue.daemon build/ClipQueue.app
cc -Os -std=c11 -Wall -Wextra -Isrc tests/portable/core_test.c src/core.c src/sha256.c -o build/core-test
