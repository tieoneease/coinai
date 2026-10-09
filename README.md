# coinai

Push-to-talk voice capture: a Waveshare ESP32-S3-Touch-AMOLED-1.75C records whenever PWR is held, and an Android app delivers the clips to SamBot. The plan is in [PLAN.md](PLAN.md), the board facts in [HARDWARE.md](HARDWARE.md), and the device ↔ app contract in [PROTOCOL.md](PROTOCOL.md).

## Toolchain on this PC

All of it is installed per user, with no admin rights:

| Tool | Where |
|---|---|
| ESP-IDF v5.5.5 (EIM 0.20.0) | `C:\esp\v5.5.5\esp-idf`; tools in `C:\Espressif\tools` |
| esptool 5.5.0 (standalone venv) | `C:\Users\Tanker\tools\esptool-venv` |
| Python 3.13 | `%LOCALAPPDATA%\Programs\Python\Python313` |
| JDK 21 (Corretto) | `C:\Users\Tanker\tools\jdk-21` |
| Android SDK (platform 36, build-tools 36.0.0, platform-tools) | `%LOCALAPPDATA%\Android\Sdk` |

GitHub release downloads are slow from this machine (about 80 KB/s), while git clones are fast. EIM therefore pulls tools through `-m https://dl.espressif.com/github_assets`.

## Firmware

From Git Bash at the repo root:

```bash
scripts/idf firmware build
scripts/idf firmware -p COM9 flash        # app-flash for app-only updates
PY=/c/Users/Tanker/tools/esptool-venv/Scripts/python.exe
$PY scripts/serlog.py COM9 10 --reset     # boot log; the port follows USB re-enumeration
$PY scripts/serlog.py COM9 5 --send pmic --send i2cscan --send imu --send stats
$PY scripts/fbdump.py COM9 evidence/screen.png   # what LVGL rendered, CRC-checked
```

- `scripts/idf` wraps EIM's PowerShell activation script. It removes `MSYSTEM` first, because `idf.py` silently does nothing (and exits 0) when it sees Git Bash.
- COM9 is the coin. COM8 is an unrelated USB serial device (VID 1D50, PID 615E); never flash it.
- Recovery: the full factory image is in `backup/` (see HARDWARE.md, "Factory state").
