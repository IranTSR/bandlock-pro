<p align="center">
  <h1 align="center">📡 BandLock Pro</h1>
  <p align="center">
    <strong>LTE/5G Band Locker & Signal Monitor for Qualcomm Snapdragon</strong>
  </p>
  <p align="center">
    A native QRTR/QMI tool to lock LTE/NR bands, monitor real-time signal strength, and scan nearby cell towers — bypassing Android's privacy restrictions.
  </p>
  <p align="center">
    <a href="#features">Features</a> •
    <a href="#architecture">Architecture</a> •
    <a href="#supported-bands">Supported Bands</a> •
    <a href="#building">Building</a> •
    <a href="#usage">Usage</a> •
    <a href="#screenshots">Screenshots</a>
  </p>
</p>

---

## ⚠️ Disclaimer

> This tool requires **root access** and communicates directly with the Qualcomm modem via QRTR sockets. Use at your own risk. Incorrect band configuration may cause temporary loss of cellular connectivity. Only tested on **Snapdragon 695 (X51 modem)** — behavior may differ on other chipsets.

## Features

- 🔒 **Band Locking** — Lock to specific LTE bands (B1, B3, B7, B8, B28, B40) or carrier aggregation combos
- 📊 **Real-time Signal Monitoring** — RSRP, RSRQ, SNR, PCI, and EARFCN from the modem
- 📡 **Neighbor Cell Scanner** — Discover nearby towers with PCI/EARFCN/RSRP data, bypassing Android 11+ privacy masking
- 🔓 **Current Lock State** — Always shows which bands are currently locked when you reopen the app
- 🔄 **Swipe to Refresh** — Pull down to force update signal data
- 📱 **Native Android APK** — No Termux needed; clean Kotlin/Compose app with NDK-compiled binary
- 🌐 **Web Dashboard** — Alternative glassmorphism web UI via Python server

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                  BandLock Pro APK                     │
│  ┌─────────────────┐   ┌──────────────────────────┐ │
│  │  MainActivity.kt │   │  activity_main.xml (UI) │ │
│  │  (libsu root)    │   │  - Lock State Card      │ │
│  │  - Band Lock     │   │  - Signal Metrics       │ │
│  │  - Signal Poll   │   │  - Neighbor Towers      │ │
│  │  - Lock State    │   │  - Band Buttons         │ │
│  └────────┬─────────┘   └──────────────────────────┘ │
│           │ Root Shell                                │
│  ┌────────▼─────────┐                                │
│  │  libqmi_tool.so  │  ← NDK-compiled C binary      │
│  │  (qmi_tool.c)    │                                │
│  └────────┬─────────┘                                │
└───────────┼─────────────────────────────────────────┘
            │ AF_QIPCRTR socket
┌───────────▼─────────────────────────────────────────┐
│        Qualcomm Modem (via QRTR/QMI)                │
│  ┌─────────────┐  ┌──────────────┐  ┌────────────┐ │
│  │ NAS Service  │  │ SET_SYS_SEL  │  │ CELL_LOC   │ │
│  │ (Band Pref)  │  │ (Band Lock)  │  │ (Towers)   │ │
│  └─────────────┘  └──────────────┘  └────────────┘ │
└─────────────────────────────────────────────────────┘
```

### How It Works

1. Opens an `AF_QIPCRTR` kernel socket (no `/dev/diag` needed)
2. Discovers the NAS (Network Access Service) via QRTR service lookup
3. Sends raw QMI messages to query signal, lock bands, and read cell info
4. Parses TLV responses to extract PCI, EARFCN, RSRP, RSRQ from modem hardware
5. Bypasses Android 11+ privacy restrictions that hide cell tower data from apps

## Supported Bands

| Band | Frequency | Bitmask | Notes |
|------|-----------|---------|-------|
| B1   | 2100 MHz  | `0x1`   | Primary LTE |
| B3   | 1800 MHz  | `0x4`   | Most common |
| B7   | 2600 MHz  | `0x40`  | High speed |
| B8   | 900 MHz   | `0x80`  | Indoor/rural |
| B28  | 700 MHz   | `0x8000000` | 5G NSA anchor |
| B40  | 2300 MHz  | `0x8000000000` | TDD |

### Combo Presets

| Combo | Bitmask | Use Case |
|-------|---------|----------|
| B1+B3 | `0x5` | Balanced |
| B3+B7 | `0x44` | Speed |
| B3+B40 | `0x8000000004` | TDD Combo |
| B1+B3+B7 | `0x45` | Max Speed 3CA |
| B1+B3+B28 | `0x8000005` | 5G Anchor 3CA |

## Building

### Prerequisites

- Android Studio with NDK installed
- Java 21+
- Root access on device (KernelSU or Magisk)
- Qualcomm Snapdragon chipset (tested on SD695)

### Android APK

```bash
# Clone the repo
git clone https://github.com/Apihplays/bandlock-pro.git
cd bandlock-pro

# Build with Gradle
cd BandLockPro
./gradlew assembleDebug

# Install via ADB
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### CLI Binary (Termux)

```bash
# Push source to device
adb push native/qmi_tool.c /data/local/tmp/
adb push native/qmi_common.h /data/local/tmp/

# Compile in Termux
clang -o qmi_tool qmi_tool.c -Wall -O2

# Test connection
su -c ./qmi_tool test
```

### Web Dashboard

```bash
# Start the server (in Termux)
cd web/
python server.py

# Open browser at http://localhost:8080
```

## Usage

### CLI Examples

```bash
# Test modem connection
su -c ./qmi_tool test

# Lock to Band 3 only
su -c ./qmi_tool band_lock 0x4

# Lock to B1 + B3 + B28 (5G Anchor combo)
su -c ./qmi_tool band_lock 0x8000005

# Check current band preferences
su -c ./qmi_tool get_pref

# Get serving cell & neighbor tower info
su -c ./qmi_tool cell_info

# Unlock all bands
su -c ./qmi_tool unlock
```

### QMI Output Examples

```json
// Band Lock
{"result":"OK","lte_bands":"0x4","locked_bands":["B3"]}

// Cell Info
{"cells":[{"type":"serving","pci":123,"earfcn":1350},{"type":"neighbor","pci":456,"earfcn":1350,"rsrp":-980,"rsrq":-110}]}

// Get Preferences
{"preferences":{"mode":"0x005c","lte_bands":"0x0011e7ffffdf3fff"}}
```

## Project Structure

```
.
├── native/                    # C source for QMI/QRTR binary
│   ├── qmi_tool.c            # Main tool (band lock, cell info, get_pref)
│   ├── qmi_common.h          # QMI protocol definitions & TLV helpers
│   └── build.sh              # Build script for Termux
│
├── BandLockPro/               # Android Studio project
│   └── app/src/main/
│       ├── cpp/               # NDK build (CMake)
│       │   ├── CMakeLists.txt
│       │   ├── qmi_tool.c    # Synced from native/
│       │   └── qmi_common.h
│       ├── java/.../
│       │   └── MainActivity.kt  # App logic with libsu
│       └── res/layout/
│           └── activity_main.xml # Glassmorphism UI
│
├── web/                       # Web dashboard
│   ├── server.py             # Python API bridge
│   └── index.html            # Glassmorphism dashboard
│
├── compile_in_termux.bat      # Windows → Termux auto-deploy
└── run_web.bat                # Launch web dashboard
```

## Technical Deep Dive

### Privacy Bypass

Android 11+ hides PCI and EARFCN from `TelephonyManager` for privacy. BandLock Pro bypasses this by reading **directly from the Qualcomm modem** via QMI protocol:

- **TLV 0x13**: LTE intra-frequency cell info (serving PCI + neighbors)
- **TLV 0x14**: LTE inter-frequency neighbor cells
- **TLV 0x1E**: NR5G serving cell info

### Modem Quirks (SD695 / X51)

- Rejects `CHANGE_DURATION` TLV — only permanent lock mode works
- Rejects `NR5G_BAND_PREF` TLV — NR5G band control not supported on this chipset
- Requires `getsockname()` to resolve local QRTR node ID before binding

## Tested On

| Device | Chipset | Modem | Root | Status |
|--------|---------|-------|------|--------|
| Redmi Note 11 Pro 5G (veux) | Snapdragon 695 | X51 | KernelSU | ✅ Working |

## License

MIT License — see [LICENSE](LICENSE) for details.

## Credits

- Built by [@Apihplays](https://github.com/Apihplays)
- QMI/QRTR protocol references from Qualcomm open-source kernel drivers
- Root access via [libsu](https://github.com/topjohnwu/libsu) by topjohnwu
