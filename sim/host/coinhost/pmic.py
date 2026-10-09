"""AXP2101 PMIC model (sim/IPC.md "PMIC model") plus a simple Li-ion battery."""

from __future__ import annotations

import bisect

ADDR = 0x34

# The real coin's registers, 2026-10-09, USB powered (sim/IPC.md).
INITIAL_REGS = {
    0x00: 0x28, 0x01: 0x14, 0x03: 0x4A, 0x04: 0x00, 0x05: 0x00, 0x06: 0x00, 0x07: 0x00, 0x10: 0x34,
    0x12: 0x08, 0x13: 0x03, 0x14: 0x65, 0x15: 0x06, 0x16: 0x04, 0x17: 0x00, 0x18: 0x0A, 0x19: 0x06,
    0x20: 0x04, 0x21: 0x01, 0x22: 0x06, 0x23: 0x3F, 0x24: 0x00, 0x25: 0x18, 0x26: 0x08, 0x27: 0x14,
    0x30: 0x1F, 0x40: 0xFF, 0x41: 0xFC, 0x42: 0x5F, 0x50: 0x12, 0x61: 0x05, 0x62: 0x04, 0x63: 0x15,
    0x64: 0x03, 0x68: 0x01, 0x69: 0x01, 0x80: 0x0F, 0x81: 0x00, 0x82: 0x12, 0x83: 0x28, 0x84: 0x46,
    0x85: 0x64, 0x86: 0x00, 0x90: 0xFF, 0x91: 0x01, 0x92: 0x1C, 0x93: 0x1C, 0x94: 0x19, 0x95: 0x0D,
    0x96: 0x07, 0x97: 0x17, 0x98: 0x0E, 0x99: 0x00, 0x9A: 0x00,
}

REG_STATUS1 = 0x00
REG_STATUS2 = 0x01
REG_COMMON_CFG = 0x10
REG_PWROFF_EN = 0x22
REG_KEY_TIMING = 0x27
REG_VBAT_H = 0x34
REG_VBUS_H = 0x38
REG_VSYS_H = 0x3A
REG_TDIE_H = 0x3C
REG_ICC_CHG = 0x62
REG_DC_ONOFF = 0x80
REG_DCDC1_VOL = 0x82
REG_LDO_ONOFF0 = 0x90
REG_ALDO1_VOL = 0x92
REG_BAT_PERCENT = 0xA4

ON_TIME_S = (0.128, 0.512, 1.0, 2.0)
OFF_TIME_S = (4.0, 6.0, 8.0, 10.0)

# Charger state, STATUS2 bits 2:0, as XPowersAXP2101.hpp (xpowers_chg_status_t) and firmware/main/pmic.c
# name them (the real register dump has 0x01 = 0x14: standby, done).
CHG_TRICKLE, CHG_PRECHARGE, CHG_CC, CHG_CV, CHG_DONE, CHG_STOP = range(6)
CHG_NAMES = ('trickle', 'precharge', 'constant current', 'constant voltage', 'done', 'not charging')

# Simple Li-ion open-circuit curve: (percent, volts).
CURVE = [(0, 3.00), (5, 3.45), (10, 3.60), (20, 3.70), (30, 3.75), (40, 3.79), (50, 3.83),
         (60, 3.88), (70, 3.94), (80, 4.01), (90, 4.08), (100, 4.20)]
_PCTS = [p for p, _ in CURVE]
_VOLTS = [v for _, v in CURVE]


def pct_to_volts(pct: float) -> float:
    pct = max(0.0, min(100.0, pct))
    i = bisect.bisect_right(_PCTS, pct)
    if i >= len(CURVE):
        return _VOLTS[-1]
    p0, v0 = CURVE[i - 1]
    p1, v1 = CURVE[i]
    return v0 + (v1 - v0) * (pct - p0) / (p1 - p0)


def volts_to_pct(volts: float) -> float:
    if volts <= _VOLTS[0]:
        return 0.0
    if volts >= _VOLTS[-1]:
        return 100.0
    i = bisect.bisect_right(_VOLTS, volts)
    p0, v0 = CURVE[i - 1]
    p1, v1 = CURVE[i]
    return p0 + (p1 - p0) * (volts - v0) / (v1 - v0)


def charge_current_ma(reg: int) -> int:
    code = reg & 0x1F
    return code * 25 if code <= 8 else 200 + (code - 8) * 100


class Axp2101:
    """Register file with live registers. The supervisor owns power-state decisions; this class reports
    them through `events` returned from i2c()/write()."""

    def __init__(self) -> None:
        self.regs = bytearray(256)
        self.battery_present = True
        self.usb = True
        self._battery_pct = 100.0  # authoritative state of charge
        self.empty_mv: int | None = None  # VBAT below the curve (under 3.0 V) when the battery is flat
        self.die_temp_c = 30.0
        self.capacity_mah = 400.0
        self._ptr = 0
        self.reset_registers()

    # ------------------------------------------------------------------ state
    def reset_registers(self) -> None:
        self.regs[:] = bytes(256)
        for reg, val in INITIAL_REGS.items():
            self.regs[reg] = val
        self._ptr = 0

    @property
    def battery_pct(self) -> float:
        return self._battery_pct

    @battery_pct.setter
    def battery_pct(self, pct: float) -> None:
        self._battery_pct = max(0.0, min(100.0, float(pct)))
        if self._battery_pct > 0:
            self.empty_mv = None

    @property
    def vbat_mv(self) -> int:
        if not self.battery_present:
            return 0
        if self.empty_mv is not None and self._battery_pct <= 0:
            return self.empty_mv
        return int(round(pct_to_volts(self._battery_pct) * 1000))

    @property
    def vbus_mv(self) -> int:
        return 5000 if self.usb else 0

    @property
    def vsys_mv(self) -> int:
        return max(self.vbus_mv - 100, self.vbat_mv)

    @property
    def power_ok(self) -> bool:
        """Whether the rails can be up: USB, or a battery at or above 3.0 V."""
        return self.usb or (self.battery_present and self.vbat_mv >= 3000)

    @property
    def charging(self) -> bool:
        return self.usb and self.battery_present and self.battery_pct < 100.0

    @property
    def charger_state(self) -> int:
        if not (self.usb and self.battery_present):
            return CHG_STOP
        if self.battery_pct >= 100.0:
            return CHG_DONE
        v = self.vbat_mv
        if v < 3000:
            return CHG_PRECHARGE
        return CHG_CC if v < 4100 else CHG_CV

    @property
    def long_press_off_enabled(self) -> bool:
        return bool(self.regs[REG_PWROFF_EN] & 0x02)

    @property
    def on_time_s(self) -> float:
        return ON_TIME_S[self.regs[REG_KEY_TIMING] & 3]

    @property
    def off_time_s(self) -> float:
        return OFF_TIME_S[(self.regs[REG_KEY_TIMING] >> 2) & 3]

    @property
    def dcdc1_on(self) -> bool:
        return bool(self.regs[REG_DC_ONOFF] & 0x01)

    @property
    def aldo1_on(self) -> bool:
        return bool(self.regs[REG_LDO_ONOFF0] & 0x01)

    @property
    def dcdc1_mv(self) -> int:
        return 1500 + (self.regs[REG_DCDC1_VOL] & 0x1F) * 100

    @property
    def aldo1_mv(self) -> int:
        return 500 + (self.regs[REG_ALDO1_VOL] & 0x1F) * 100

    def set_battery_volts(self, volts: float) -> None:
        self.battery_pct = volts_to_pct(volts)
        if volts < 3.0:
            self.empty_mv = max(2500, int(round(volts * 1000)))

    # ------------------------------------------------------------------ registers
    def read(self, reg: int) -> int:
        reg &= 0xFF
        if reg == REG_STATUS1:
            v = self.regs[reg] & ~(0x20 | 0x08)
            if self.usb:
                v |= 0x20
            if self.battery_present:
                v |= 0x08
            return v
        if reg == REG_STATUS2:
            v = self.regs[reg] & 0x18  # keep bit 4 (power on) and bit 3 as found
            if self.charging:
                v |= 0x01 << 5
            elif self.battery_present and not self.usb:
                v |= 0x02 << 5
            return v | self.charger_state
        if reg in (REG_VBAT_H, REG_VBAT_H + 1):
            return self._adc(reg - REG_VBAT_H, self.vbat_mv)
        if reg in (REG_VBUS_H, REG_VBUS_H + 1):
            return self._adc(reg - REG_VBUS_H, self.vbus_mv)
        if reg in (REG_VSYS_H, REG_VSYS_H + 1):
            return self._adc(reg - REG_VSYS_H, self.vsys_mv)
        if reg in (REG_TDIE_H, REG_TDIE_H + 1):
            raw = int(round(7274 - (self.die_temp_c - 22.0) * 20))
            return self._adc(reg - REG_TDIE_H, max(0, min(0x3FFF, raw)))
        if reg == REG_BAT_PERCENT:
            return int(round(self.battery_pct)) if self.battery_present else 0
        return self.regs[reg]

    @staticmethod
    def _adc(low: int, value: int) -> int:
        return value & 0xFF if low else (value >> 8) & 0x3F

    def write(self, reg: int, val: int) -> list[str]:
        reg &= 0xFF
        val &= 0xFF
        events: list[str] = []
        if reg == REG_COMMON_CFG and val & 0x01:
            events.append('poweroff')
            val &= ~0x01  # the bit self-clears: the system is off now
        old = self.regs[reg]
        self.regs[reg] = val
        if old != val:
            events.append(f'reg:{reg:02x}')
        return events

    def i2c(self, wbytes: bytes, rlen: int) -> tuple[bytes, list[str]]:
        """One transaction: wbytes[0] sets the register pointer, the rest are writes with
        auto-increment; then rlen bytes are read from the pointer, auto-incrementing."""
        events: list[str] = []
        if wbytes:
            self._ptr = wbytes[0]
            for i, b in enumerate(wbytes[1:]):
                events += self.write(self._ptr + i, b)
        out = bytes(self.read((self._ptr + i) & 0xFF) for i in range(rlen))
        return out, events

    # ------------------------------------------------------------------ battery simulation
    def step(self, dt_s: float, load_ma: float) -> None:
        """Advance the battery by dt_s seconds of (possibly accelerated) time."""
        if not self.battery_present or dt_s <= 0:
            return
        if self.usb:
            if self.battery_pct >= 100.0:
                return
            current = charge_current_ma(self.regs[REG_ICC_CHG])
            if self.charger_state == CHG_CV:  # taper towards full
                current *= max(0.1, (100.0 - self.battery_pct) / 12.0)
            self.battery_pct = min(100.0, self.battery_pct + current * dt_s / 3600.0 / self.capacity_mah * 100.0)
        else:
            self.battery_pct = max(0.0, self.battery_pct - load_ma * dt_s / 3600.0 / self.capacity_mah * 100.0)
            if self.battery_pct <= 0 and self.empty_mv is None:
                self.empty_mv = 2950  # flat: the cell sags below the 3.0 V cut-off

    def snapshot(self) -> dict:
        return {
            'battery_present': self.battery_present,
            'usb': self.usb,
            'battery_pct': round(self.battery_pct, 2),
            'vbat_mv': self.vbat_mv,
            'vbus_mv': self.vbus_mv,
            'vsys_mv': self.vsys_mv,
            'die_temp_c': self.die_temp_c,
            'charging': self.charging,
            'charger_state': CHG_NAMES[self.charger_state],
            'status1': self.read(REG_STATUS1),
            'status2': self.read(REG_STATUS2),
            'percent_reg': self.read(REG_BAT_PERCENT),
            'dcdc1_on': self.dcdc1_on,
            'dcdc1_mv': self.dcdc1_mv,
            'aldo1_on': self.aldo1_on,
            'aldo1_mv': self.aldo1_mv,
            'charge_current_ma': charge_current_ma(self.regs[REG_ICC_CHG]),
            'long_press_off': self.long_press_off_enabled,
            'on_time_ms': int(self.on_time_s * 1000),
            'off_time_s': self.off_time_s,
            'capacity_mah': self.capacity_mah,
            'power_ok': self.power_ok,
        }
