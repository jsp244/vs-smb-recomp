#!/usr/bin/env python3
"""android/build_apk.py - builds the Android app (arm64-v8a) straight from the SDK tools, no Gradle.

Needs: the Android SDK with a platform >= 34, build-tools, an NDK and CMake (all from the SDK
manager), a JDK (javac, keytool), and generated/ (make recomp with your own dump). Downloads the
SDL2 source once (its Java glue and native library). Output, both with arm64 (handhelds and
phones) and x86_64 (emulators, Chromebooks):
  dist/vssmb-android-handheld.apk  for gaming handhelds with built-in controls
  dist/vssmb-android-phone.apk     for phones: a translucent on-screen controller; its own app
                                   ("VS. Super Mario Bros. Touch"), so both can be installed
signed with a key kept in ~/.android/vssmb-release.keystore (made on first use; keep it, an
installed app can only be updated with the same key).

usage: python android/build_apk.py [--sdk DIR] [--jdk DIR] [--abi arm64-v8a,x86_64]
                                   [--edition handheld,phone] [--install [SERIAL]]
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "android")
BUILD = os.path.join(HERE, "build")
SDL_VERSION = "2.32.10"
SDL_URL = "https://github.com/libsdl-org/SDL/releases/download/release-%s/SDL2-%s.tar.gz" % (SDL_VERSION, SDL_VERSION)
ABIS = ["arm64-v8a", "x86_64"]
MIN_SDK = 24
TARGET_SDK = 34
EXE = ".exe" if os.name == "nt" else ""
PACKAGE = "org.vssmb.nativeport"
LABEL = "VS. Super Mario Bros."
# the phone edition is the same app with the on-screen controls (manifest meta-data, read by
# VSSMBActivity) under its own package name and label
EDITIONS = {
    "handheld": {"apk": "vssmb-android-handheld.apk", "package": PACKAGE, "label": LABEL, "touch": False},
    "phone": {"apk": "vssmb-android-phone.apk", "package": PACKAGE + ".touch", "label": LABEL + " Touch", "touch": True},
}
KEYSTORE = os.path.join(os.path.expanduser("~"), ".android", "vssmb-release.keystore")
KEY_ALIAS, KEY_PASS = "vssmb", "vssmbport"


def run(cmd, **kw):
    print("+ " + " ".join('"%s"' % c if " " in str(c) else str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def version_key(path):
    return [int(x) if x.isdigit() else x for x in re.split(r"[.\-_]", os.path.basename(path))]


def newest(pattern):
    found = sorted(glob.glob(pattern), key=version_key)
    if not found:
        raise SystemExit("not found: " + pattern)
    return found[-1]


def find_tools(args):
    sdk = args.sdk or os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT") or \
        os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "Android", "Sdk")
    t = {"sdk": sdk}
    t["ndk"] = newest(os.path.join(sdk, "ndk", "*"))
    cmake_bin = os.path.join(newest(os.path.join(sdk, "cmake", "*")), "bin")
    t["cmake"], t["ninja"] = os.path.join(cmake_bin, "cmake" + EXE), os.path.join(cmake_bin, "ninja" + EXE)
    bt = newest(os.path.join(sdk, "build-tools", "*"))
    t["aapt2"], t["zipalign"] = os.path.join(bt, "aapt2" + EXE), os.path.join(bt, "zipalign" + EXE)
    t["d8"], t["apksigner"] = os.path.join(bt, "lib", "d8.jar"), os.path.join(bt, "lib", "apksigner.jar")
    t["lambda_stubs"] = os.path.join(bt, "core-lambda-stubs.jar")   # android.jar has no LambdaMetafactory
    plats = [p for p in glob.glob(os.path.join(sdk, "platforms", "android-*")) if version_key(p)[-1] >= TARGET_SDK]
    if not plats:
        raise SystemExit("need platforms/android-%d or newer in the SDK" % TARGET_SDK)
    t["android_jar"] = os.path.join(sorted(plats, key=version_key)[-1], "android.jar")
    jdk = args.jdk or os.environ.get("JAVA_HOME")
    if not jdk:
        java = shutil.which("java")
        if not java:
            raise SystemExit("no JDK: install one or pass --jdk")
        jdk = os.path.dirname(os.path.dirname(os.path.realpath(java)))
    for tool in ("java", "javac", "keytool"):
        t[tool] = os.path.join(jdk, "bin", tool + EXE)
    t["strip"] = newest(os.path.join(t["ndk"], "toolchains", "llvm", "prebuilt", "*", "bin", "llvm-strip" + EXE))
    t["adb"] = os.path.join(sdk, "platform-tools", "adb" + EXE)
    for k, v in t.items():
        if k != "sdk" and not os.path.exists(v):
            raise SystemExit("missing %s: %s" % (k, v))
    return t


def sdl_source():
    deps = os.path.join(BUILD, "deps")
    src = os.path.join(deps, "SDL2-" + SDL_VERSION)
    if os.path.isdir(src):
        return src
    os.makedirs(deps, exist_ok=True)
    tgz = src + ".tar.gz"
    if not os.path.exists(tgz):
        print("downloading " + SDL_URL, flush=True)
        urllib.request.urlretrieve(SDL_URL, tgz)
    with tarfile.open(tgz) as tf:
        if sys.version_info >= (3, 12):
            tf.extractall(deps, filter="data")
        else:
            for m in tf.getmembers():
                if m.name.startswith("/") or ".." in m.name.split("/"):
                    raise SystemExit("unsafe path in the SDL archive: " + m.name)
            tf.extractall(deps)
    return src


def cm(path):
    """CMake reads backslashes in its variables as escapes"""
    return path.replace("\\", "/")


def build_native(t, sdl, abi):
    out = os.path.join(BUILD, "native-" + abi)
    run([t["cmake"], "-S", cm(HERE), "-B", cm(out), "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + cm(t["ninja"]),
         "-DCMAKE_TOOLCHAIN_FILE=" + cm(os.path.join(t["ndk"], "build", "cmake", "android.toolchain.cmake")),
         "-DANDROID_ABI=" + abi, "-DANDROID_PLATFORM=android-%d" % MIN_SDK, "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DSDL2_SOURCE_DIR=" + cm(sdl),
         "-DVSSMB_VERSION=" + version()[1], "-DVSSMB_REPO=" + repository()])
    run([t["cmake"], "--build", out])
    libs = {}
    for name in ("libSDL2.so", "libmain.so"):
        hits = glob.glob(os.path.join(out, "**", name), recursive=True)
        if not hits:
            raise SystemExit("not built: " + name)
        dst = os.path.join(BUILD, "lib", abi, name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        run([t["strip"], "--strip-unneeded", "-o", dst, hits[0]])
        libs["lib/%s/%s" % (abi, name)] = dst
    return libs


def build_java(t, sdl):
    classes = os.path.join(BUILD, "classes")
    shutil.rmtree(classes, ignore_errors=True)
    os.makedirs(classes)
    srcs = glob.glob(os.path.join(sdl, "android-project", "app", "src", "main", "java", "org", "libsdl", "app", "*.java"))
    srcs += glob.glob(os.path.join(HERE, "java", "**", "*.java"), recursive=True)
    boot = os.pathsep.join([t["android_jar"], t["lambda_stubs"]])
    run([t["javac"], "-nowarn", "-Xlint:-options", "-encoding", "UTF-8", "-source", "8", "-target", "8",
         "-bootclasspath", boot, "-classpath", boot, "-d", classes] + srcs)
    dex = os.path.join(BUILD, "dex")
    shutil.rmtree(dex, ignore_errors=True)
    os.makedirs(dex)
    class_files = glob.glob(os.path.join(classes, "**", "*.class"), recursive=True)
    run([t["java"], "-cp", t["d8"], "com.android.tools.r8.D8", "--release", "--min-api", str(MIN_SDK),
         "--lib", t["android_jar"], "--output", dex] + class_files)
    return os.path.join(dex, "classes.dex")


def build_icons():
    """android/res plus launcher icons drawn from the port's own artwork (tools/make_icon.py)"""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import make_icon
    res = os.path.join(BUILD, "res")
    shutil.rmtree(res, ignore_errors=True)
    shutil.copytree(os.path.join(HERE, "res"), res)          # styles (the app theme)
    for density, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        d = os.path.join(res, "mipmap-" + density)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "ic_launcher.png"), "wb") as f:
            f.write(make_icon.png(size, make_icon.render(size)))
    return res


def git(*args):
    return subprocess.check_output(["git"] + list(args), cwd=ROOT, text=True, stderr=subprocess.DEVNULL).strip()


def version():
    """(versionCode, versionName): the number of commits, and git describe (the newest v* tag, plus the
    commits after it), as the Makefile gives the desktop builds; VSSMB_VERSION in the environment
    sets the name instead"""
    try:
        count = int(git("rev-list", "--count", "HEAD"))
    except (OSError, subprocess.CalledProcessError, ValueError):
        count = 1
    name = os.environ.get("VSSMB_VERSION")
    if not name:
        try:
            name = git("describe", "--tags", "--match", "v[0-9]*", "--dirty")
        except (OSError, subprocess.CalledProcessError):
            name = "dev"
    return count, name[1:] if name[0] in "vV" and name[1:2].isdigit() else name


def repository():
    """owner/name of the GitHub repository whose releases the Updates tab checks: VSSMB_REPO in the
    environment, else the one this checkout came from (its origin remote), else "" """
    if "VSSMB_REPO" in os.environ:
        return os.environ["VSSMB_REPO"]
    try:
        m = re.search(r"github\.com[:/]+([^/]+/[^/]+?)(\.git)?$", git("remote", "get-url", "origin"))
    except (OSError, subprocess.CalledProcessError):
        return ""
    return m.group(1) if m else ""


def edition_manifest(edition):
    """AndroidManifest.xml with the edition's label, and for the phone edition the meta-data that turns
    the on-screen controls on"""
    e = EDITIONS[edition]
    with open(os.path.join(HERE, "AndroidManifest.xml"), encoding="utf-8") as f:
        m = f.read()
    if m.count('android:label="%s"' % LABEL) != 2 or m.count("        <activity\n") != 1:
        raise SystemExit("AndroidManifest.xml: labels or activity not where expected")
    m = m.replace('android:label="%s"' % LABEL, 'android:label="%s"' % e["label"])
    if e["touch"]:
        m = m.replace("        <activity\n", '        <meta-data android:name="org.vssmb.touch" android:value="true" />\n\n        <activity\n')
    out = os.path.join(BUILD, "AndroidManifest-%s.xml" % edition)
    with open(out, "w", encoding="utf-8") as f:
        f.write(m)
    return out


def package(t, libs, dex, res, edition):
    e = EDITIONS[edition]
    code, name = version()
    flat = os.path.join(BUILD, "res.zip")
    run([t["aapt2"], "compile", "--dir", res, "-o", flat])
    unaligned = os.path.join(BUILD, "unaligned.apk")
    rename = ["--rename-manifest-package", e["package"]] if e["package"] != PACKAGE else []
    run([t["aapt2"], "link", "-o", unaligned, "-I", t["android_jar"], "--manifest", edition_manifest(edition),
         "--min-sdk-version", str(MIN_SDK), "--target-sdk-version", str(TARGET_SDK),
         "--version-code", str(code), "--version-name", name, "--auto-add-overlay"] + rename + [flat])
    with zipfile.ZipFile(unaligned, "a", zipfile.ZIP_DEFLATED) as z:
        z.write(dex, "classes.dex")
        for arcname, path in libs.items():
            z.write(path, arcname)
        z.write(os.path.join(ROOT, "docs", "PLAYERS.md"), "assets/README.txt")   # the players' guide and licences, as the other packages carry it
    aligned = os.path.join(BUILD, "aligned.apk")
    run([t["zipalign"], "-p", "-f", "4", unaligned, aligned])
    if not os.path.exists(KEYSTORE):
        os.makedirs(os.path.dirname(KEYSTORE), exist_ok=True)
        run([t["keytool"], "-genkeypair", "-keystore", KEYSTORE, "-alias", KEY_ALIAS, "-keyalg", "RSA", "-keysize", "2048",
             "-validity", "10000", "-storepass", KEY_PASS, "-keypass", KEY_PASS, "-dname", "CN=VS. Super Mario Bros. native port"])
    out_apk = os.path.join(ROOT, "dist", e["apk"])
    os.makedirs(os.path.dirname(out_apk), exist_ok=True)
    run([t["java"], "-jar", t["apksigner"], "sign", "--ks", KEYSTORE, "--ks-key-alias", KEY_ALIAS,
         "--ks-pass", "pass:" + KEY_PASS, "--key-pass", "pass:" + KEY_PASS, "--out", out_apk, aligned])
    run([t["java"], "-jar", t["apksigner"], "verify", out_apk])
    print("apk: %s (%s, %s, version %s, %d bytes)" % (out_apk, e["label"], e["package"], name, os.path.getsize(out_apk)), flush=True)
    return out_apk


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sdk")
    ap.add_argument("--jdk")
    ap.add_argument("--abi", default=",".join(ABIS), help="comma-separated ABIs (default: %(default)s)")
    ap.add_argument("--edition", default=",".join(EDITIONS), help="comma-separated editions (default: %(default)s)")
    ap.add_argument("--install", nargs="?", const="", metavar="SERIAL", help="adb install afterwards (optionally to one device)")
    args = ap.parse_args()
    editions = [x.strip() for x in args.edition.split(",") if x.strip()]
    for x in editions:
        if x not in EDITIONS:
            raise SystemExit("unknown edition %s (handheld, phone)" % x)
    if not glob.glob(os.path.join(ROOT, "generated", "*.c")):
        raise SystemExit("no generated code: run 'make recomp' with your dump first")
    t = find_tools(args)
    sdl = sdl_source()
    libs = {}
    for abi in args.abi.split(","):
        libs.update(build_native(t, sdl, abi.strip()))
    dex = build_java(t, sdl)
    res = build_icons()
    apks = [package(t, libs, dex, res, x) for x in editions]
    if args.install is not None:
        for apk in apks:
            run([t["adb"]] + (["-s", args.install] if args.install else []) + ["install", "-r", apk])


if __name__ == "__main__":
    main()
