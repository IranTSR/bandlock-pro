#!/data/data/com.termux/files/usr/bin/bash
# ============================================
# Build qmi_tool for ARM64 Android
# ============================================
# Option 1: Build in Termux (on phone)
#   pkg install clang
#   bash build.sh
#
# Option 2: Build with Android NDK (on PC)
#   Set NDK path and run: bash build.sh ndk
# ============================================

SRC="qmi_tool.c"
OUT="qmi_tool"

if [ "$1" = "ndk" ]; then
    # Cross-compile with Android NDK
    NDK="${ANDROID_NDK_HOME:-$ANDROID_NDK}"
    if [ -z "$NDK" ]; then
        echo "Set ANDROID_NDK_HOME or ANDROID_NDK env var"
        exit 1
    fi
    CC="$NDK/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android34-clang"
    $CC -o $OUT $SRC -Wall -O2 -static
else
    # Build natively in Termux
    clang -o $OUT $SRC -Wall -O2
fi

if [ $? -eq 0 ]; then
    echo "✓ Built: $OUT"
    echo "  Push to phone:"
    echo "    adb push $OUT /data/local/tmp/"
    echo "    adb shell chmod 755 /data/local/tmp/$OUT"
    echo ""
    echo "  Test:"
    echo "    adb shell su -c /data/local/tmp/$OUT test"
    echo "    adb shell su -c /data/local/tmp/$OUT cell_info"
    echo "    adb shell su -c /data/local/tmp/$OUT band_lock 0x4"
    echo "    adb shell su -c /data/local/tmp/$OUT unlock"
else
    echo "✗ Build failed"
fi
