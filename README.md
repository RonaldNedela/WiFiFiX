# WiFiFiX

**WPA2/WPA3 Mixed Network Association Fix for Legacy iOS Devices**  
*Resolves 802.11 association failures on modern Wi-Fi routers broadcasting WPA3-Transition / SAE suites on iOS 12 and legacy kernels.*

![iOS 12.0 - 14.8.1](https://img.shields.io/badge/iOS-12.0--14.8.1-blue?style=flat-square)
![Architecture](https://img.shields.io/badge/Architecture-arm64%20%7C%20arm64e-success?style=flat-square)
![Jailbreak](https://img.shields.io/badge/Jailbreak-Rootless%20%26%20Rootful-orange?style=flat-square)
![License](https://img.shields.io/badge/License-MIT-lightgrey?style=flat-square)

---

## Problem Description: The Transitional WPA3 Incompatibility

Modern wireless access points and home mesh routers (e.g., Wi-Fi 6 / 6E / 7 units from Asus, Netgear, TP-Link, AVM Fritz!, and ISP gateways) default to **WPA2/WPA3 Mixed (Transitional) Mode**. In this configuration, beacons and probe responses broadcast an RSN (Robust Security Network) Information Element (IE 48) containing multiple Authentication and Key Management (AKM) suites:
- AKM 2: `00-0F-AC:2` (WPA2-PSK)
- AKM 8: `00-0F-AC:8` (WPA3-SAE / Simultaneous Authentication of Equals)

### Root Cause in Legacy iOS `wifid`
On legacy iOS versions (notably iOS 12.x on iPhone 5s, iPhone 6, iPhone 6s, iPhone 7, iPad Air, etc.):
1. **Unimplemented AKM Parsing**: The system wireless daemon (`/usr/libexec/wifid` or `/usr/sbin/wifid`) parses the incoming beacon's RSN IE during association negotiation.
2. **Rejection Cascade**: When `wifid` encounters AKM 8 (SAE), it fails to fall back to AKM 2. The internal authentication selector parser aborts with status code `-0xF3C` (`0xFFFFF0C4` / `kWiFiReturnError`).
3. **User-Facing Symptom**: The device displays "Unable to join the network" (*"Nem sikerült csatlakozni a hálózathoz"*), even when the password is 100% correct, forcing network administrators to downgrade whole networks to legacy WPA2-only.

---

## Technical Solution: Dynamic In-Memory RSN Filter

WiFiFiX hooks the private `findRSN_IE_Parser` entry point inside `wifid` and `WiFiManager` via dynamic symbol pattern recognition.

```
[ AP Beacon: AKM 2 (WPA2) + AKM 8 (SAE) ] 
                   |
                   v
[ WiFiFiX: Intercepts RSN IE in wifid memory ]
                   |
                   v  (Strips unhandled AKM 8 from selector buffer)
                   v
[ wifid associates smoothly via standard WPA2-PSK ]
```

### Architectural Details
- **Dynamic Binary Scanner**: Uses custom Mach-O segment inspection (`disasm.h`, `finder.h`, `macho.h`) to locate RSN Information Element parsing logic across iOS binaries without relying on fragile hardcoded offsets.
- **Selective Suite Stripping**: Inspects `IE_KEY_RSN_AUTHSELS` in the parsed beacon dictionary. Any AKM value greater than standard WPA2 (e.g., AKM 8 / SAE) is stripped from the active candidate list.
- **Graceful PSK Fallback**: `wifid` sees only standard WPA2-PSK (AKM 2) and completes the 4-way EAPOL handshake instantly.
- **Zero Overhead**: The filter executes exclusively during initial 802.11 association and roaming handoffs. Once connected, throughput and radio performance are 100% native.

---

## Features

- **Automated Daemon Management**: Installs `/etc/rc.d/wififix` and executes a clean restart of `wifid` upon installation, enabling the fix immediately without requiring a full device reboot.
- **Universal Architecture**: Compiled for both `arm64` and `arm64e` (A7 through A12+ chipsets).
- **Dual Package Support**: Distributed in both classic Rootful (`iphoneos-arm`) and modern Rootless (`iphoneos-arm64`) Debian archive formats.
- **Persistent Operation**: Runs transparently in the background with zero user intervention or configuration required.

---

## Performance and Resource Optimization

- **Zero-Allocation Fast Path**: Wi-Fi beacons and probe responses are inspected in a single non-allocating pass. Over 99% of networks utilize standard WPA2 AKMs that require no modification; in these cases, `filterAuthSels` immediately returns without allocating any `CFMutableArrayRef` heap memory, eliminating garbage collection and heap fragmentation in `wifid`.
- **Silent Image Scanning**: Image enumeration during constructor initialization suppresses false-positive diagnostic logs for non-networking system libraries, keeping daemon startup instantaneous and free of syslog overhead.

---

## Installation

### Requirements
- **Device**: iPhone 5s, 6, 6 Plus, 6s, 6s Plus, 7, 7 Plus, SE, iPad mini 2/3/4, iPad Air 1/2, iPad Pro.
- **Firmware**: iOS 12.0 through 14.8.1 (and legacy iOS firmware down to iOS 6.0).
- **Jailbreak**: Checkra1n, Amethyst, unc0ver, Taurine, palera1n, Chimera.

### Package Deployment
1. Download the Debian package matching your jailbreak bootstrap:
   - **Rootful (checkra1n / unc0ver / Amethyst rootful):**  
     `com.ronci.wififix_1.0.0_iphoneos-arm.deb`
   - **Rootless (palera1n rootless / modern bootstrap):**  
     `com.ronci.wififix_1.0.0_iphoneos-arm64.deb`
2. Open the package in **Filza**, **Sileo**, or **Zebra** and tap **Install**.
3. Wi-Fi daemon will automatically reload. Reconnect to your WPA2/WPA3 network immediately.

---

## Compilation from Source

Prerequisites: macOS with Xcode Command Line Tools and `ldid`.

```bash
git clone git@github.com:RonaldNedela/WiFiFiX.git
cd WiFiFiX
python3 pack_deb.py
```

The script:
1. Compiles `WiFiFiX_arm64.dylib` and `WiFiFiX_arm64e.dylib` targeting `arm64-apple-ios12.0`.
2. Assembles a universal fat binary via `lipo`.
3. Ad-hoc codesigns the binary using `ldid -S`.
4. Assembles both rootless and rootful Debian packages to `com.ronci.wififix_1.0.0_iphoneos-arm.deb` and `com.ronci.wififix_1.0.0_iphoneos-arm64.deb`.

---

## Repository Structure

```
WiFiFiX/
├── pack_deb.py                     # Build pipeline and dual-package assembler
├── README.md                       # Technical documentation
├── com.ronci.wififix_1.0.0_iphoneos-arm.deb   # Pre-compiled rootful package
├── com.ronci.wififix_1.0.0_iphoneos-arm64.deb # Pre-compiled rootless package
└── src/
    ├── Tweak.c                     # Core association interception and RSN filtering
    ├── finder.h                    # In-memory pattern scanning and disassembly matcher
    ├── disasm.h                    # Instruction decoding utility for ARM64
    └── macho.h                     # Dynamic Mach-O header parsing
```

---

## Author

- **Developer**: Ronald Nedela ([@RonaldNedela](https://github.com/RonaldNedela))

---

## License

This project is licensed under the MIT License.
