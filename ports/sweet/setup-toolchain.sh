#!/bin/bash
set -euo pipefail
export THEOS=${THEOS:-$PWD/.theos}
mkdir -p "$THEOS/toolchain" "$THEOS/sdks"
curl -fL --retry 4 https://github.com/L1ghtmann/llvm-project/releases/download/test-210562a/iOSToolchain-x86_64.tar.xz -o /tmp/hackios-toolchain.tar.xz
tar -xJf /tmp/hackios-toolchain.tar.xz -C "$THEOS/toolchain"
git init -q "$THEOS/sdks"
git -C "$THEOS/sdks" sparse-checkout set iPhoneOS16.5.sdk
git -C "$THEOS/sdks" fetch --depth 1 https://github.com/theos/sdks.git 0222fd5413cf4b9af096f37b4621afa2688572f7
git -C "$THEOS/sdks" checkout -q FETCH_HEAD
