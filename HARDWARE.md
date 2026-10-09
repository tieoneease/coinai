# Hardware reference: Waveshare ESP32-S3-Touch-AMOLED-1.75C

Board facts the firmware depends on, read on 2026-10-09 from the sources below. **verify** marks items still to be confirmed on the device (planned in PLAN.md phases 0–1).

Sources:
- Schematic `Schematic/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf` in `github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C` (byte-identical to the wiki copy). Crops in `hardware/`.
- BSP `waveshare/esp32_s3_touch_amoled_1_75c` v3.0.0: header `include/bsp/esp32_s3_touch_amoled_1_75c.h` and source.
- Waveshare examples `01_AXP2101`, `05_Spec_Analyzer`, and Arduino `03_LVGL_AXP2101_ADC_Data`.
- AXP2101 register meanings from XPowersLib `src/XPowersAXP2101.hpp` (maintainer driver; the X-Powers datasheet was not checked).

Do not reuse pinouts for the sibling ESP32-S3-Touch-AMOLED-1.75 (non-C). It has a TCA9554 expander, an RTC and a TF slot, and its LCD reset, touch reset and MCLK pins differ.

## SoC, memory, clocks, USB

- ESP32-S3R8: 8 MB octal PSRAM in package. External 32 MB flash in QIO mode (**verify** with `esptool flash_id`).
- 40 MHz crystal X1. **No 32.768 kHz crystal**: the layout shows only X1. As a result, a BLE connection held through light sleep must keep the main crystal running (see PLAN.md decision D3).
- Native USB-Serial/JTAG on GPIO19/20, which enumerates as VID 303A / PID 1001 (COM9 on the dev PC).
- No reset button. AXP2101 PWROK drives CHIP_PU. Test pads: TP11 "RST" (CHIP_PU) and TP1 GND.

## GPIO map

| GPIO | Net | Notes |
|---:|---|---|
| 0 | BOOT key (Key1), 10 kΩ pull-up, 100 nF | Strapping pin. Held low at chip reset or power-on → ROM download mode. RTC GPIO. |
| 1 | LCD_RESET | RTC GPIO (can be held through deep sleep). |
| 2 | TP_RESET | RTC GPIO. |
| 3 | SYS_OUT = PWR key state, **high while pressed** | Strapping pin (JTAG source; matters only if eFuse `STRAP_JTAG_SEL` is burned, **verify** with `espefuse summary`). RTC GPIO, so it can wake deep sleep. |
| 4–7 | QSPI SIO0–3 (CO5300) | |
| 8 | I2S DSDIN (to ES8311) | |
| 9 | I2S SCLK | Shared by ES8311 and ES7210. |
| 10 | I2S ASDOUT (from ES7210) | |
| 11 | TP_INT (CST9217) | RTC GPIO. |
| 12 | LCD_CS | |
| 13 | LCD_TE | Tearing-effect input. Wired but unused by the BSP. |
| 14 / 15 | I2C SCL / SDA | Shared by all onboard devices; also on pads. |
| 16 | I2S MCLK | |
| 17, 18 | Pads | Free. |
| 19 / 20 | USB D− / D+ | |
| 21 | QMI_INT1 (IMU) | From the schematic net label. Waveshare's pin table lists QMI_INT1 on GPIO38, which conflicts with the LCD clock and is taken as a table error. **verify**. RTC GPIO. |
| 38 | QSPI SCL (LCD clock) | |
| 39–42 | Pads | Free (also the JTAG pins). |
| 43 / 44 | U0TXD / U0RXD pads | |
| 45 | I2S LRCK | Strapping pin (VDD_SPI voltage). |
| 46 | PA_CTRL (NS4150B amplifier) | Strapping pin (boot mode / ROM log). |

Crops: `hardware/schematic-gpio-table.png`, `hardware/schematic-keys-imu.png`, `hardware/schematic-lcd-connector.png`.

## I2C devices (GPIO14 SCL / GPIO15 SDA)

| Address | Device |
|---:|---|
| 0x18 | ES8311 codec |
| 0x34 | AXP2101 PMIC |
| 0x40 | ES7210 4-channel mic ADC |
| 0x5A | CST9217 touch |
| 0x6B | QMI8658 IMU (address strap shown on the schematic) |

The C variant has no TCA9554 and no RTC chip. **verify** all of the above with an I2C scan.

## Buttons

From the schematic "KEYS" block (`hardware/schematic-keys-imu.png`):

- **BOOT (Key1):** shorts GPIO0 to GND.
- **PWR (Key2):** pulls the AXP2101 PWRON pin low through RP6 (510 Ω), which is the PMIC's power-on/off key and PEKEY interrupt source.
  - The same node drives the gate of T1 (BSS138) through R12 (1 kΩ).
  - T1's drain is SYS_OUT, with R9 (10 kΩ) pulled up to VCC3V3, and SYS_OUT is netted to GPIO3.
  - Released: PWRON is high, T1 conducts, SYS_OUT is low. Pressed: T1 is off, SYS_OUT is high.
- **Consequence:** whenever 3V3 is up and PWR is released, R9 draws about **0.33 mA** (3.3 V / 10 kΩ). That sets a floor under Standby current; Off (DCDC1 down) avoids it. **verify by measurement.**

## Power

- **AXP2101 used outputs:** only two (`hardware/schematic-axp2101*.png`).
  - DCDC1 → **VCC3V3**: ESP32, flash, the panel (FPC J3 pins 21–24 and 26), touch, IMU, codec digital, and the SYS_OUT pull-up.
  - ALDO1 → **A3V3**: audio analog (ES8311 AVDD, ES7210).
  - Everything else is unused. Waveshare's PMIC example turns all other rails off.
- **AXP IRQ:** pin 38 has only a 10 kΩ pull-up and is **not routed to the ESP32**, so PMIC events (charger, battery, key) must be polled over I2C. Waveshare's own example polls.
- **Panel power:** the panel has no rail of its own. It can only be put to sleep with the CO5300 sleep-in command, not power-gated.
- **Battery:** MX1.25 connector with a 10 kΩ NTC on TS.
  - **Capacity of the installed cell is unknown.** Waveshare doesn't publish the 1.75C battery's capacity. For comparison only, their 1.8-inch board's FAQ recommends 400 mAh and ships 350 mAh, in a different case.
  - **Charge current** must stay ≤ 1C of the installed cell. The AXP2101 offers 100, 125, 150, 175, 200, 300, 400, 500 … 1000 mA (XPowersLib enum). Waveshare's example programs 400 mA.
  - **No battery-current ADC.** XPowersLib exposes battery, VBUS and system voltage, the fuel-gauge percentage and temperature, but no current reading. Capacity can't be measured directly; estimate it by timing a full charge at a known constant current.

### AXP2101 registers the firmware will use

Meanings per XPowersLib; datasheet not checked.

| Reg | Use |
|---|---|
| 0x04–0x07 | Data buffer. Commonly used for flags that survive power-off while a battery is attached. **verify** |
| 0x20 | Power-on source (bit 0 = PWR key) |
| 0x21 | Power-off source |
| 0x22 | Bit 1: long-press shutdown enable. Bit 0: long-press action (0 = off, 1 = restart). |
| 0x26 | Sleep / wake-up control |
| 0x27 | [5:4] IRQ time 1/1.5/2/2.5 s · [3:2] OFF time 4/6/8/10 s · [1:0] ON time 128 ms/512 ms/1 s/2 s. Whether these survive Off is unknown (**verify**); it decides the from-Off latency. |
| 0x40–0x42 / 0x48–0x4A | IRQ enable / status (PEKEY short/long/edges, VBUS, battery, charge) |

## Display and touch

- **Panel:** CO5300, 466×466, 16-bit colour, over QSPI.
  - The driver component `esp_lcd_co5300` 2.2.0 defaults to a **40 MHz** QSPI clock. 80 MHz is untested on this panel.
  - The BSP sets a column gap of 6 and rounds dirty areas to 2-pixel alignment.
  - Brightness is set with a panel command; there is no backlight GPIO.
- **BSP display defaults:**
  - `esp_lvgl_adapter`, with 50-line double buffers in **PSRAM**.
  - Tearing avoidance is off (`TEAR_AVOID_MODE_NONE`).
- **Adapter options:** `esp_lvgl_adapter` 0.7.x offers `TEAR_AVOID_MODE_TE_SYNC` (TE-GPIO based, QSPI supported) and FPS statistics. The BSP pins `~0.6`; check whether that version has TE sync.

## Audio

- **ES7210 inputs:**
  - MIC1 and MIC2: onboard microphones at opposite edges of the round board.
  - MIC3: playback reference through the "AEC" network, for echo cancellation.
  - MIC4: not connected per the sibling board's docs (**verify**).
- **Output path:** ES8311 → NS4150B amplifier (PA_CTRL GPIO46) → speaker pads.
- **I2S pins:** MCLK 16, SCLK 9, LRCK 45, DOUT 8, DIN 10.
- **BSP audio:** the BSP's default I2S config is duplex mono at 22.05 kHz. Waveshare's spectrum example captures 16 kHz stereo (both mics).
