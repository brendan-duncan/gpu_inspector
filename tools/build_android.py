#!/usr/bin/env python3
"""Builds the Vulkan layer for Android and packages it as a layer APK.

    python tools/build_android.py                 # arm64-v8a layer + build/android/gpu_inspector_layer.apk
    python tools/build_android.py --abi arm64-v8a,x86_64
    python tools/build_android.py --no-apk        # only the .so files
    python tools/build_android.py --debug         # Debug build of the layer

Outputs (what the inspector app and app/tools/stage_layer.mjs look for):

    build/android/lib/<abi>/libVkLayer_inspector_capture.so
    build/android/gpu_inspector_layer.apk         # the layer packaged as an installable app
    build/android/gpu_inspector_layer.apk.json    # package name, version, ABIs

The APK carries no code: it exists so Android 10+ can load the layer into any debuggable app
through the `gpu_debug_layer_app` setting, the way RenderDoc ships its layer in its own APK.
On Android 9 (or when the APK is unavailable) the inspector pushes the .so into the target's data
directory with `run-as` instead, which only needs the .so files.

Needs the Android SDK (ANDROID_HOME / ANDROID_SDK_ROOT, or the default install location) with an
NDK, CMake and Ninja (the SDK's own copies are used when none is on PATH), and for the APK the
build-tools (aapt2, zipalign, apksigner) plus a Java runtime (JAVA_HOME, PATH, or the JDK bundled
with Android Studio). The APK is signed with the standard Android debug key
(~/.android/debug.keystore, created if missing).
"""
import argparse
import glob
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "android")
LAYER_LIB = "libVkLayer_inspector_capture.so"
APK_PACKAGE = "com.brendanduncan.gpuinspector.layer"
APK_NAME = "gpu_inspector_layer.apk"
MIN_SDK = 28   # Android 9: the first release with the GPU debug layer settings
TARGET_SDK = 29

IS_WIN = platform.system() == "Windows"
EXE = ".exe" if IS_WIN else ""


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def newest(paths):
    """The highest version among directories named by a version number."""
    def key(p):
        parts = []
        for x in os.path.basename(p).replace("-", ".").split("."):
            parts.append(int(x) if x.isdigit() else -1)
        return parts
    return max(paths, key=key) if paths else None


def find_sdk(explicit):
    candidates = [explicit, os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT")]
    if IS_WIN:
        candidates.append(os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk"))
    elif platform.system() == "Darwin":
        candidates.append(os.path.expanduser("~/Library/Android/sdk"))
    else:
        candidates += [os.path.expanduser("~/Android/Sdk"), "/opt/android-sdk"]
    for c in candidates:
        if c and os.path.isdir(os.path.join(c, "platform-tools")):
            return c
    return None


def find_ndk(explicit, sdk):
    candidates = [explicit, os.environ.get("ANDROID_NDK_HOME"), os.environ.get("ANDROID_NDK"), os.environ.get("ANDROID_NDK_ROOT")]
    if sdk:
        candidates.append(newest(glob.glob(os.path.join(sdk, "ndk", "*"))))
        candidates.append(os.path.join(sdk, "ndk-bundle"))
    for c in candidates:
        if c and os.path.isfile(os.path.join(c, "build", "cmake", "android.toolchain.cmake")):
            return c
    return None


def find_cmake_tools(sdk):
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if sdk and (not cmake or not ninja):
        d = newest(glob.glob(os.path.join(sdk, "cmake", "*")))
        if d:
            cmake = cmake or os.path.join(d, "bin", "cmake" + EXE)
            ninja = ninja or os.path.join(d, "bin", "ninja" + EXE)
    return cmake, ninja


def find_java():
    home = os.environ.get("JAVA_HOME")
    candidates = [os.path.join(home, "bin", "java" + EXE) if home else None, shutil.which("java")]
    if IS_WIN:
        candidates += [
            r"C:\Program Files\Android\Android Studio\jbr\bin\java.exe",
            *glob.glob(r"C:\Program Files\Unity\Hub\Editor\*\Editor\Data\PlaybackEngines\AndroidPlayer\OpenJDK\bin\java.exe"),
        ]
    elif platform.system() == "Darwin":
        candidates += [
            "/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin/java",
            *glob.glob("/Applications/Unity/Hub/Editor/*/PlaybackEngines/AndroidPlayer/OpenJDK/bin/java"),
        ]
    else:
        candidates += [
            "/opt/android-studio/jbr/bin/java",
            os.path.expanduser("~/android-studio/jbr/bin/java"),
            "/usr/lib/jvm/default-java/bin/java",
            *glob.glob(os.path.expanduser("~/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/OpenJDK/bin/java")),
        ]
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    return None


def run(cmd, **kw):
    print("+", " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def build_layer(abi, ndk, cmake, ninja, build_type):
    build_dir = os.path.join(OUT, abi)
    run([cmake, "-S", ROOT, "-B", build_dir, "-G", "Ninja",
         f"-DCMAKE_MAKE_PROGRAM={ninja}",
         f"-DCMAKE_TOOLCHAIN_FILE={os.path.join(ndk, 'build', 'cmake', 'android.toolchain.cmake')}",
         f"-DANDROID_ABI={abi}",
         f"-DANDROID_PLATFORM=android-{MIN_SDK}",
         "-DANDROID_STL=c++_static",
         f"-DCMAKE_BUILD_TYPE={build_type}",
         "-DVKINSP_BUILD_TESTS=OFF"])
    run([cmake, "--build", build_dir])
    built = os.path.join(build_dir, "bin", LAYER_LIB)
    if not os.path.isfile(built):
        die(f"{built} was not produced")
    lib_dir = os.path.join(OUT, "lib", abi)
    os.makedirs(lib_dir, exist_ok=True)
    shutil.copyfile(built, os.path.join(lib_dir, LAYER_LIB))
    print(f"layer: {os.path.join(lib_dir, LAYER_LIB)}")


def build_apk(abis, sdk):
    build_tools = newest(glob.glob(os.path.join(sdk, "build-tools", "*")))
    if not build_tools:
        die("no build-tools in the SDK (install one with sdkmanager or Android Studio)")
    aapt2 = os.path.join(build_tools, "aapt2" + EXE)
    zipalign = os.path.join(build_tools, "zipalign" + EXE)
    apksigner = os.path.join(build_tools, "lib", "apksigner.jar")
    for t in (aapt2, zipalign, apksigner):
        if not os.path.isfile(t):
            die(f"{t} not found")
    platform_dir = newest(glob.glob(os.path.join(sdk, "platforms", "android-*")))
    android_jar = os.path.join(platform_dir, "android.jar") if platform_dir else None
    if not android_jar or not os.path.isfile(android_jar):
        die("no platforms/android-*/android.jar in the SDK (install a platform with sdkmanager)")
    java = find_java()
    if not java:
        die("no Java runtime found for apksigner (set JAVA_HOME, or install Android Studio)")

    # The version name identifies the layer build: the inspector reinstalls the APK when the
    # installed version differs from the one it ships.
    digest = hashlib.sha1()
    libs = []
    for abi in abis:
        lib = os.path.join(OUT, "lib", abi, LAYER_LIB)
        if not os.path.isfile(lib):
            die(f"{lib} missing; build the layer first")
        libs.append((abi, lib))
        with open(lib, "rb") as f:
            digest.update(f.read())
    version_name = digest.hexdigest()[:12]
    version_code = int(time.time()) // 60

    work = os.path.join(OUT, "apk")
    os.makedirs(work, exist_ok=True)
    manifest = os.path.join(work, "AndroidManifest.xml")
    with open(manifest, "w", encoding="utf-8") as f:
        f.write(f'''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{APK_PACKAGE}"
    android:versionCode="{version_code}"
    android:versionName="{version_name}">
    <uses-sdk android:minSdkVersion="{MIN_SDK}" android:targetSdkVersion="{TARGET_SDK}"/>
    <!-- No code, no activity: the package only carries the layer library, which Android's
         loader finds through the gpu_debug_layer_app setting. extractNativeLibs keeps the
         library extracted on disk, where the loader's dlopen expects it. -->
    <application android:label="GPU Inspector Layer"
        android:hasCode="false"
        android:debuggable="true"
        android:forceQueryable="true"
        android:extractNativeLibs="true"/>
</manifest>
''')
    unaligned = os.path.join(work, "unaligned.apk")
    aligned = os.path.join(work, "aligned.apk")
    for f in (unaligned, aligned):
        if os.path.exists(f):
            os.remove(f)
    run([aapt2, "link", "-o", unaligned, "--manifest", manifest, "-I", android_jar,
         "--min-sdk-version", str(MIN_SDK), "--target-sdk-version", str(TARGET_SDK)])
    with zipfile.ZipFile(unaligned, "a", zipfile.ZIP_DEFLATED) as z:
        for abi, lib in libs:
            z.write(lib, f"lib/{abi}/{LAYER_LIB}")
    run([zipalign, "-f", "4", unaligned, aligned])

    keystore = os.path.join(os.path.expanduser("~"), ".android", "debug.keystore")
    if not os.path.isfile(keystore):
        keytool = os.path.join(os.path.dirname(java), "keytool" + EXE)
        if not os.path.isfile(keytool):
            die(f"{keystore} missing and no keytool next to {java} to create it")
        os.makedirs(os.path.dirname(keystore), exist_ok=True)
        run([keytool, "-genkeypair", "-keystore", keystore, "-alias", "androiddebugkey",
             "-storepass", "android", "-keypass", "android", "-keyalg", "RSA", "-keysize", "2048",
             "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US"])
    apk = os.path.join(OUT, APK_NAME)
    run([java, "-jar", apksigner, "sign", "--ks", keystore, "--ks-pass", "pass:android",
         "--ks-key-alias", "androiddebugkey", "--key-pass", "pass:android", "--out", apk, aligned])
    with open(apk + ".json", "w", encoding="utf-8") as f:
        json.dump({"package": APK_PACKAGE, "versionName": version_name, "versionCode": version_code,
                   "abis": abis, "minSdk": MIN_SDK}, f, indent=2)
    print(f"apk: {apk} (version {version_name}, {', '.join(abis)})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--abi", default="arm64-v8a", help="comma-separated ABIs (default: arm64-v8a)")
    ap.add_argument("--sdk", help="Android SDK directory")
    ap.add_argument("--ndk", help="Android NDK directory")
    ap.add_argument("--debug", action="store_true", help="Debug build")
    ap.add_argument("--no-apk", action="store_true", help="build the layer libraries only")
    args = ap.parse_args()

    abis = [a.strip() for a in args.abi.split(",") if a.strip()]
    sdk = find_sdk(args.sdk)
    ndk = find_ndk(args.ndk, sdk)
    if not ndk:
        die("Android NDK not found: set ANDROID_NDK_HOME or install one into the SDK (sdkmanager 'ndk;<version>')")
    cmake, ninja = find_cmake_tools(sdk)
    if not cmake or not os.path.isfile(cmake):
        die("cmake not found on PATH or in the SDK")
    if not ninja or not os.path.isfile(ninja):
        die("ninja not found on PATH or in the SDK")
    print(f"sdk: {sdk or '(none)'}\nndk: {ndk}")

    for abi in abis:
        build_layer(abi, ndk, cmake, ninja, "Debug" if args.debug else "Release")
    if not args.no_apk:
        if not sdk:
            die("Android SDK not found (needed for the APK); pass --no-apk to build only the libraries")
        build_apk(abis, sdk)


if __name__ == "__main__":
    main()
