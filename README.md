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
$PY scripts/clipget.py COM9 3 clip3.ogg          # copy a clip off; checks transfer CRC and Ogg pages
```

Console commands (type `help` on the console for the full list):

| Command | What it does |
|---|---|
| `pmic`, `pmic reg <hex> [<hex>]`, `pmic chg <mA>` | PMIC status, raw registers, charge current |
| `i2cscan`, `imu`, `stats` | bus scan, one IMU sample, uptime/reset reason/heap |
| `rec <s>`, `clips`, `clip get <id>`, `clip rm <id\|all>` | record, list, dump, delete clips |
| `play <id>`, `tone [hz] [ms]`, `level`, `loopback [hz]` | speaker and microphone checks |
| `sleep [s] [rec]` | standby; wakes on PWR, or after `s` seconds (`rec`: the wake records like a PWR hold) |
| `ptttest [n]`, `pttlog [clear]`, `wake` | unattended standby→record latency trials and the latency records |
| `poweroff` | PMIC off; PWR turns the board back on |
| `fbdump`, `reboot` | screen dump, restart |

- `scripts/idf` wraps EIM's PowerShell activation script. It removes `MSYSTEM` first, because `idf.py` silently does nothing (and exits 0) when it sees Git Bash.
- COM9 is the coin. COM8 is an unrelated USB serial device (VID 1D50, PID 615E); never flash it.
- Recovery: the full factory image is in `backup/` (see HARDWARE.md, "Factory state").
