"""
Builds the OpenGL ES phone test application (test/android_gles_triangle: a NativeActivity drawing with
an OpenGL ES 3.2 context) and packages it as a debuggable APK, so the OpenGL ES plugin's layer can be
loaded into it:

    python tools/build_android_gles_triangle.py   # -> build/android/android_gles_triangle.apk
    adb install -r build/android/android_gles_triangle.apk

Needs what tools/build_android.py needs (SDK with build-tools and a platform, NDK, cmake, ninja, Java).
The package is com.brendanduncan.androidglestriangle ("GLES Triangle").
"""
import argparse
import glob
import os
import sys
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_android as ba  # noqa: E402

ROOT = ba.ROOT
OUT = os.path.join(ROOT, "build", "android")
SRC = os.path.join(ROOT, "test", "android_gles_triangle")
LIB = "android_gles_triangle"
PACKAGE = "com.brendanduncan.androidglestriangle"
LABEL = "GLES Triangle"
APK_NAME = "android_gles_triangle.apk"
MIN_SDK = 29   # Android 10: the first release that loads OpenGL ES layers
TARGET_SDK = 33


def build(abi, ndk, cmake, ninja, debug):
    build_dir = os.path.join(OUT, LIB, abi)
    ba.run([cmake, "-S", SRC, "-B", build_dir, "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}",
            f"-DCMAKE_TOOLCHAIN_FILE={os.path.join(ndk, 'build', 'cmake', 'android.toolchain.cmake')}",
            f"-DANDROID_ABI={abi}",
            f"-DANDROID_PLATFORM=android-{MIN_SDK}",
            "-DANDROID_STL=c++_static",
            f"-DCMAKE_BUILD_TYPE={'Debug' if debug else 'RelWithDebInfo'}"])
    ba.run([cmake, "--build", build_dir])
    lib = os.path.join(build_dir, f"lib{LIB}.so")
    if not os.path.isfile(lib):
        ba.die(f"{lib} was not produced")
    return lib


def package(libs, sdk):
    build_tools = ba.newest(glob.glob(os.path.join(sdk, "build-tools", "*")))
    aapt2 = os.path.join(build_tools, "aapt2" + ba.EXE)
    zipalign = os.path.join(build_tools, "zipalign" + ba.EXE)
    apksigner = os.path.join(build_tools, "lib", "apksigner.jar")
    platform_dir = ba.newest(glob.glob(os.path.join(sdk, "platforms", "android-*")))
    android_jar = os.path.join(platform_dir, "android.jar")
    java = ba.find_java()
    if not java:
        ba.die("no Java runtime found for apksigner")
    work = os.path.join(OUT, LIB, "apk")
    os.makedirs(work, exist_ok=True)
    manifest = os.path.join(work, "AndroidManifest.xml")
    version_code = int(time.time()) // 60
    with open(manifest, "w", encoding="utf-8") as f:
        f.write(f'''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{PACKAGE}"
    android:versionCode="{version_code}"
    android:versionName="1.0">
    <uses-sdk android:minSdkVersion="{MIN_SDK}" android:targetSdkVersion="{TARGET_SDK}"/>
    <uses-feature android:glEsVersion="0x00030002" android:required="true"/>
    <!-- debuggable: Android loads GPU debug layers (the inspector's) only into debuggable applications. -->
    <application android:label="{LABEL}"
        android:hasCode="false"
        android:debuggable="true"
        android:extractNativeLibs="true">
        <activity android:name="android.app.NativeActivity"
            android:label="{LABEL}"
            android:exported="true"
            android:launchMode="singleTask"
            android:configChanges="screenSize|screenLayout|orientation|keyboardHidden|keyboard|navigation|uiMode|density">
            <meta-data android:name="android.app.lib_name" android:value="{LIB}"/>
            <intent-filter>
                <action android:name="android.intent.action.MAIN"/>
                <category android:name="android.intent.category.LAUNCHER"/>
            </intent-filter>
        </activity>
    </application>
</manifest>
''')
    unaligned = os.path.join(work, "unaligned.apk")
    aligned = os.path.join(work, "aligned.apk")
    for p in (unaligned, aligned):
        if os.path.exists(p):
            os.remove(p)
    ba.run([aapt2, "link", "-o", unaligned, "--manifest", manifest, "-I", android_jar,
            "--min-sdk-version", str(MIN_SDK), "--target-sdk-version", str(TARGET_SDK)])
    with zipfile.ZipFile(unaligned, "a", zipfile.ZIP_DEFLATED) as z:
        for abi, lib in libs:
            z.write(lib, f"lib/{abi}/lib{LIB}.so")
    ba.run([zipalign, "-f", "4", unaligned, aligned])
    keystore = os.path.join(os.path.expanduser("~"), ".android", "debug.keystore")
    apk = os.path.join(OUT, APK_NAME)
    ba.run([java, "-jar", apksigner, "sign", "--ks", keystore, "--ks-pass", "pass:android",
            "--ks-key-alias", "androiddebugkey", "--key-pass", "pass:android", "--out", apk, aligned])
    print(f"apk: {apk} ({PACKAGE})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--abi", default="arm64-v8a")
    ap.add_argument("--sdk")
    ap.add_argument("--ndk")
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()
    sdk = ba.find_sdk(args.sdk)
    ndk = ba.find_ndk(args.ndk, sdk)
    if not sdk or not ndk:
        ba.die("Android SDK / NDK not found")
    cmake, ninja = ba.find_cmake_tools(sdk)
    libs = [(abi, build(abi, ndk, cmake, ninja, args.debug)) for abi in [a.strip() for a in args.abi.split(",") if a.strip()]]
    package(libs, sdk)


if __name__ == "__main__":
    main()
