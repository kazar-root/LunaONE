#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

project_dir=$(cd "$(dirname "$0")/../.." && pwd)
android_source="$project_dir/app/android/src/main"
apk_dir=${1:-"$project_dir/build/android"}
mkdir -p "$apk_dir"
apk_dir=$(cd "$apk_dir" && pwd)
native_dir=${LUNACORE_ANDROID_NATIVE_DIR:-"$apk_dir/native"}
app_version=0.1

: "${ANDROID_HOME:?Set ANDROID_HOME to the Android SDK directory}"
: "${JAVA_HOME:?Set JAVA_HOME to a JDK 17 directory}"
toolchain="${ANDROID_NDK_HOME:-"$ANDROID_HOME/ndk/28.2.13676358"}/build/cmake/android.toolchain.cmake"
platform="$ANDROID_HOME/platforms/android-35/android.jar"
build_tools="$ANDROID_HOME/build-tools/35.0.0"
for dependency in "$toolchain" "$platform" "$build_tools/aapt2" "$JAVA_HOME/bin/javac"; do
    if [[ ! -f "$dependency" ]]; then
        printf 'Missing Android build dependency %s\n' "$dependency" >&2
        exit 1
    fi
done

package_dir=$(mktemp -d "$apk_dir/package.XXXXXXXX")
trap 'rm -r "$package_dir"' EXIT
mkdir -p "$package_dir/classes" "$package_dir/apk"
native_entries=()
for abi in arm64-v8a armeabi-v7a; do
    abi_build="$native_dir"
    if [[ "$abi" == armeabi-v7a ]]; then abi_build="$native_dir-$abi"; fi
    cmake -S "$project_dir" -B "$abi_build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-26 \
        -DANDROID_STL=c++_static -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build "$abi_build" --target lunaone_android --parallel 6
    strip_tool=$(sed -n 's/^CMAKE_STRIP:FILEPATH=//p' "$abi_build/CMakeCache.txt")
    native_entry="lib/$abi/liblunaone_android.so"
    mkdir -p "$package_dir/apk/lib/$abi"
    "$strip_tool" --strip-unneeded -o "$package_dir/apk/$native_entry" "$abi_build/liblunaone_android.so"
    native_entries+=("$native_entry")
done

mkdir -p "$package_dir/generated"
"$build_tools/aapt2" compile --dir "$android_source/res" -o "$package_dir/resources.zip"
"$build_tools/aapt2" link -I "$platform" --manifest "$android_source/AndroidManifest.xml" \
    --min-sdk-version 26 --target-sdk-version 35 --version-code 10 --version-name "$app_version" --debug-mode \
    --java "$package_dir/generated" -o "$package_dir/base.apk" "$package_dir/resources.zip"
sources=("$android_source"/java/app/lunacore/android/*.java "$package_dir/generated/app/lunacore/android/R.java")
"$JAVA_HOME/bin/javac" -encoding UTF-8 --release 17 -classpath "$platform" \
    -d "$package_dir/classes" "${sources[@]}"
"$JAVA_HOME/bin/jar" --create --file "$package_dir/classes.jar" -C "$package_dir/classes" .
"$build_tools/d8" --lib "$platform" --min-api 26 --output "$package_dir/apk" "$package_dir/classes.jar"

cmake -E chdir "$package_dir/apk" zip -q -0 "$package_dir/base.apk" classes.dex "${native_entries[@]}"
"$build_tools/zipalign" -P 16 -f 4 "$package_dir/base.apk" "$package_dir/aligned.apk"

debug_key="$apk_dir/debug.keystore"
if [[ ! -f "$debug_key" ]]; then
    "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$debug_key" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=LunaCore Android Debug'
fi
apk_name="LunaONE-Android-GoogleTV-$app_version.apk"
"$build_tools/apksigner" sign --ks "$debug_key" --ks-pass pass:android --out "$package_dir/$apk_name" \
    "$package_dir/aligned.apk"
"$build_tools/apksigner" verify --verbose "$package_dir/$apk_name"
"$build_tools/zipalign" -c -P 16 4 "$package_dir/$apk_name"
mv "$package_dir/$apk_name" "$apk_dir/"
printf 'APK %s\n' "$apk_dir/$apk_name"
