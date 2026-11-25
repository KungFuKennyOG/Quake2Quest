#!/usr/bin/env bash
# SoF Quest on openMERC: build the arm64-v8a native libraries and the APK (Linux host).
#
#   NDK=<android ndk>  SOF_GAME=<patched SoF SDK Source/Game>  [ANDROID_HOME=<sdk with build-tools;35.0.0 and
#   platforms;android-34>]  [OUT=<build root>]  quest/build-android.sh [deps|native|sdk|apk|all]
#
# deps:   SDL 3 and OpenAL Soft from source (shared libraries) into $OUT/prefix; the OpenXR loader (Maven Central)
# native: the openMERC engine (libmain.so), the GLES 3 renderer and the OMsnd sound module
# sdk:    the SoF SDK game and player modules (quest/sdk/Makefile) - they take the place of openMERC's own
# apk:    an unsigned-release-style debug-signed APK, $OUT/SoFQuest.apk (needs ANDROID_HOME and a JDK; d8 of build-tools 34 fails on SDL's classes)
set -euo pipefail

WHAT=${1:-all}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${OUT:-$REPO/build-quest}
NDK=${NDK:?set NDK to the Android NDK}
API=26
JOBS=${JOBS:-$(nproc)}
SDL_TAG=${SDL_TAG:-release-3.2.10}
OPENAL_TAG=${OPENAL_TAG:-1.24.3}
OPENXR_VER=${OPENXR_VER:-1.1.36}
export CMAKE_POLICY_VERSION_MINIMUM=3.5 # OpenAL Soft's config file asks for an old CMake policy level

TC=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_shared
    -DCMAKE_PREFIX_PATH="$OUT/prefix" -DCMAKE_FIND_ROOT_PATH="$OUT/prefix")
LLVM=$NDK/toolchains/llvm/prebuilt/linux-x86_64

deps() {
	mkdir -p "$OUT/src"
	[ -d "$OUT/src/SDL" ] || git clone -q --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL "$OUT/src/SDL"
	[ -d "$OUT/src/openal-soft" ] || git clone -q --depth 1 --branch "$OPENAL_TAG" https://github.com/kcat/openal-soft "$OUT/src/openal-soft"
	cmake -S "$OUT/src/SDL" -B "$OUT/sdl3" "${TC[@]}" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
		-DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$OUT/prefix"
	cmake --build "$OUT/sdl3" --target install -j "$JOBS"
	cmake -S "$OUT/src/openal-soft" -B "$OUT/openal" "${TC[@]}" -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF \
		-DALSOFT_TESTS=OFF -DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF -DCMAKE_INSTALL_PREFIX="$OUT/prefix"
	cmake --build "$OUT/openal" --target install -j "$JOBS"
	# the Khronos OpenXR loader for Android (Apache 2.0 / MIT), headers and libopenxr_loader.so
	if [ ! -d "$OUT/openxr/prefab" ]; then
		mkdir -p "$OUT/openxr"
		curl -fsSL -o "$OUT/openxr/loader.aar" \
			"https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$OPENXR_VER/openxr_loader_for_android-$OPENXR_VER.aar"
		unzip -q -o "$OUT/openxr/loader.aar" -d "$OUT/openxr"
	fi
}

native() {
	cmake -S "$REPO/engine" -B "$OUT/engine" "${TC[@]}" -DCURL_SUPPORT=OFF -DGL3_RENDERER=OFF -DGLES3_RENDERER=ON \
		-DOM_SOFPM_TESTS=OFF -DXDG_SUPPORT=OFF -DSDL3_DIR="$OUT/prefix/lib/cmake/SDL3" \
		-DOPENXR_DIR="$OUT/openxr/prefab/modules"
	cmake --build "$OUT/engine" --target openMerc ref_gles3 -j "$JOBS"
	cmake -S "$REPO/modules/sound" -B "$OUT/sound" "${TC[@]}" -DOpenAL_DIR="$OUT/prefix/lib/cmake/OpenAL"
	cmake --build "$OUT/sound" -j "$JOBS"
}

sdk() {
	SOF_GAME=${SOF_GAME:?set SOF_GAME to the patched SDK Source/Game directory}
	local src
	src=$(dirname "$SOF_GAME")
	make -C "$src" -f "$REPO/quest/sdk/Makefile" -j "$JOBS" OBJ="$OUT/sdk-obj" OUTDIR="$OUT/sdk" \
		CXX="$LLVM/bin/clang++ --target=aarch64-linux-android$API" \
		CXXFLAGS="-O2 -g -ffp-contract=off" LDLIBS="-static-libstdc++"
}

apk() {
	ANDROID_HOME=${ANDROID_HOME:?set ANDROID_HOME (build-tools 35.0.0, platforms android-34)}
	local bt=$ANDROID_HOME/build-tools/${BUILD_TOOLS:-35.0.0} jar=$ANDROID_HOME/platforms/android-34/android.jar
	local w=$OUT/apk here=$REPO/engine/android lib
	rm -rf "$w"; mkdir -p "$w/classes" "$w/dex" "$w/root/lib/arm64-v8a" "$w/assets"
	find "$OUT/src/SDL/android-project/app/src/main/java" "$here/java" -name '*.java' > "$w/java.txt"
	javac -nowarn --release 11 -classpath "$jar" -d "$w/classes" @"$w/java.txt" 2>&1 | grep -v warning || true
	"$bt/d8" --release --min-api $API --lib "$jar" --output "$w/dex" $(find "$w/classes" -name '*.class')
	python3 "$REPO/tools/mkpak.py" "$REPO/data/base" "$w/assets/zz_openmerc.pak"
	"$bt/aapt2" compile --dir "$here/res" -o "$w/res.zip"
	"$bt/aapt2" link -o "$w/base.apk" -I "$jar" --manifest "$REPO/quest/AndroidManifest.xml" -R "$w/res.zip" \
		-A "$w/assets" --auto-add-overlay --min-sdk-version $API --target-sdk-version 34
	lib=$w/root/lib/arm64-v8a
	cp "$OUT/engine/release/libmain.so" "$lib/libmain.so"
	cp "$OUT/engine/release/ref_gles3.so" "$lib/libref_gles3.so"
	cp "$OUT/sound/OMsnd.so" "$lib/libOMsnd.so"
	cp "$OUT/sdk/gamex64.so" "$lib/libgamex86.so" # the SoF SDK game and player modules, not openMERC's own
	cp "$OUT/sdk/player.so" "$lib/libplayer.so"
	cp "$OUT/prefix/lib/libSDL3.so" "$OUT/prefix/lib/libopenal.so" "$lib/"
	cp "$OUT/openxr/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so" "$lib/"
	cp "$LLVM/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$lib/"
	"$LLVM/bin/llvm-strip" --strip-unneeded "$lib"/*.so
	cp "$w/dex/classes.dex" "$w/root/classes.dex"
	(cd "$w/root" && zip -q -r "$w/base.apk" .)
	"$bt/zipalign" -f -p 4 "$w/base.apk" "$w/aligned.apk"
	[ -f "$OUT/debug.keystore" ] || keytool -genkeypair -keystore "$OUT/debug.keystore" -storepass android -keypass android \
		-alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=SoF Quest Debug,C=US" > /dev/null 2>&1
	"$bt/apksigner" sign --ks "$OUT/debug.keystore" --ks-pass pass:android --key-pass pass:android \
		--out "$OUT/SoFQuest.apk" "$w/aligned.apk"
	echo "APK: $OUT/SoFQuest.apk ($(du -h "$OUT/SoFQuest.apk" | cut -f1))"
}

case "$WHAT" in
	deps) deps ;;
	native) native ;;
	sdk) sdk ;;
	apk) apk ;;
	all) deps; native; sdk; apk ;;
	*) echo "usage: $0 [deps|native|sdk|apk|all]" >&2; exit 2 ;;
esac
