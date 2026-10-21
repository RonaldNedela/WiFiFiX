#!/usr/bin/env python3
import os
import subprocess
import shutil

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.path.join(BASE_DIR, "src")
BUILD_DIR = os.path.join(BASE_DIR, "build")

OUTPUT_ARM = os.path.join(BASE_DIR, "com.ronci.wififix_1.0.0_iphoneos-arm.deb")
OUTPUT_ARM64 = os.path.join(BASE_DIR, "com.ronci.wififix_1.0.0_iphoneos-arm64.deb")

os.makedirs(BUILD_DIR, exist_ok=True)

# 1. Compile arm64 (iOS 12+)
arm64_dylib = os.path.join(BUILD_DIR, "WiFiFiX_arm64.dylib")
cmd_arm64 = [
    "xcrun", "-sdk", "iphoneos", "clang",
    "-target", "arm64-apple-ios12.0",
    "-shared", "-O2",
    "-undefined", "dynamic_lookup",
    "-framework", "CoreFoundation",
    "-framework", "Foundation",
    "-install_name", "/Library/MobileSubstrate/DynamicLibraries/WiFiFiX.dylib",
    "-o", arm64_dylib,
    os.path.join(SRC_DIR, "Tweak.c")
]
print("Compiling arm64 (iOS 12+)...")
subprocess.check_call(cmd_arm64)

# 2. Compile arm64e (iOS 12+)
arm64e_dylib = os.path.join(BUILD_DIR, "WiFiFiX_arm64e.dylib")
cmd_arm64e = [
    "xcrun", "-sdk", "iphoneos", "clang",
    "-target", "arm64e-apple-ios12.0",
    "-shared", "-O2",
    "-undefined", "dynamic_lookup",
    "-framework", "CoreFoundation",
    "-framework", "Foundation",
    "-install_name", "/Library/MobileSubstrate/DynamicLibraries/WiFiFiX.dylib",
    "-o", arm64e_dylib,
    os.path.join(SRC_DIR, "Tweak.c")
]
print("Compiling arm64e (iOS 12+)...")
subprocess.check_call(cmd_arm64e)

# 3. Universal fat binary
fat_dylib = os.path.join(BUILD_DIR, "WiFiFiX.dylib")
print("Creating universal binary (lipo)...")
subprocess.check_call(["lipo", "-create", arm64_dylib, arm64e_dylib, "-output", fat_dylib])

# 4. Sign binary with ldid
print("Signing universal binary with ldid...")
subprocess.check_call(["/opt/homebrew/bin/ldid", "-S", fat_dylib])

# 5. Filter plist
plist_content = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Filter</key>
    <dict>
        <key>Executables</key>
        <array>
            <string>wifid</string>
        </array>
        <key>Bundles</key>
        <array>
            <string>com.apple.wifi.WiFiManager</string>
        </array>
    </dict>
</dict>
</plist>
"""

# Control file template
control_template = """Package: com.ronci.wififix
Name: WiFiFiX
Version: 1.0.0
Architecture: {arch}
Description: Fixes WPA2/WPA3 transitional Wi-Fi network connection failure on legacy iOS devices. Strips unhandled AKMs (like SAE=8) from RSN IE so wifid associates smoothly via standard WPA2-PSK instead of throwing -0xF3C error.
Maintainer: Ronald Nedela
Author: Ronald Nedela
Section: Tweaks
"""

postinst_content = """#!/bin/sh
if [ -x /usr/libexec/wifid ] || [ -x /usr/sbin/wifid ]; then
    killall wifid 2>/dev/null || true
fi
exit 0
"""

rc_content = """#!/bin/sh
if [ -x /usr/libexec/wifid ] || [ -x /usr/sbin/wifid ]; then
    killall wifid 2>/dev/null || true
fi
"""

def build_package(dest_deb, arch):
    pkg_dir = os.path.join(BUILD_DIR, f"pkg_{arch}")
    shutil.rmtree(pkg_dir, ignore_errors=True)

    if arch == "iphoneos-arm64":
        tweak_dir = os.path.join(pkg_dir, "var", "jb", "Library", "MobileSubstrate", "DynamicLibraries")
        rc_dir = os.path.join(pkg_dir, "var", "jb", "etc", "rc.d")
    else:
        tweak_dir = os.path.join(pkg_dir, "Library", "MobileSubstrate", "DynamicLibraries")
        rc_dir = os.path.join(pkg_dir, "etc", "rc.d")

    os.makedirs(tweak_dir, exist_ok=True)
    shutil.copy2(fat_dylib, os.path.join(tweak_dir, "WiFiFiX.dylib"))
    with open(os.path.join(tweak_dir, "WiFiFiX.plist"), "w") as f:
        f.write(plist_content)

    os.makedirs(rc_dir, exist_ok=True)
    rc_file = os.path.join(rc_dir, "wififix")
    with open(rc_file, "w") as f:
        f.write(rc_content)
    os.chmod(rc_file, 0o755)

    deb_dir = os.path.join(pkg_dir, "DEBIAN")
    os.makedirs(deb_dir, exist_ok=True)
    with open(os.path.join(deb_dir, "control"), "w") as f:
        f.write(control_template.format(arch=arch))
    postinst_file = os.path.join(deb_dir, "postinst")
    with open(postinst_file, "w") as f:
        f.write(postinst_content)
    os.chmod(postinst_file, 0o755)

    subprocess.check_call([
        "/opt/homebrew/bin/dpkg-deb",
        "-Zgzip",
        "--root-owner-group",
        "-b", pkg_dir,
        dest_deb
    ])
    print(f"[SUCCESS] Built: {dest_deb}")
    dl_path = os.path.join("/Users/Ronci/Downloads", os.path.basename(dest_deb))
    shutil.copy2(dest_deb, dl_path)
    print(f"Copied to Downloads: {dl_path}")

print("\nBuilding packages...")
build_package(OUTPUT_ARM, "iphoneos-arm")
build_package(OUTPUT_ARM64, "iphoneos-arm64")
