"""QMI8658 IMU register model and the motion model that feeds it."""

from __future__ import annotations

import math
import random
import struct

ADDR = 0x6B
WHO_AM_I = 0x05
REVISION = 0x7C

REG_WHO_AM_I = 0x00
REG_REVISION = 0x01
REG_CTRL1 = 0x02
REG_CTRL2 = 0x03
REG_CTRL3 = 0x04
REG_CTRL7 = 0x08
REG_CTRL9 = 0x0A
REG_STATUS0 = 0x2E
REG_TEMP_L = 0x33
REG_DATA_END = 0x40
REG_RESET_STATUS = 0x4D
REG_RESET = 0x60

ACCEL_LSB_PER_G = {0: 16384, 1: 8192, 2: 4096, 3: 2048}
GYRO_LSB_PER_DPS = {0: 2048, 1: 1024, 2: 512, 3: 256, 4: 128, 5: 64, 6: 32, 7: 16}


def _s16(v: float) -> int:
    return max(-32768, min(32767, int(round(v))))


class Qmi8658:
    """Registers: WHO_AM_I/revision, CTRL1-9 store writes, the data block 0x33-0x40 (temperature, accel XYZ,
    gyro XYZ, s16 LE) refreshed from the motion model while CTRL7 enables the sensor, reset 0x60 <- 0xB0.
    Register auto-increment and little-endian are always on (the firmware sets CTRL1 = 0x40 anyway)."""

    def __init__(self, motion: 'Motion | None' = None) -> None:
        self.motion = motion or Motion()
        self.regs = bytearray(128)
        self._ptr = 0
        self.reset()

    def reset(self) -> None:
        self.regs[:] = bytes(128)
        self.regs[REG_WHO_AM_I] = WHO_AM_I
        self.regs[REG_REVISION] = REVISION
        self.regs[REG_RESET_STATUS] = 0x80
        self._ptr = 0

    @property
    def accel_enabled(self) -> bool:
        return bool(self.regs[REG_CTRL7] & 0x01)

    @property
    def gyro_enabled(self) -> bool:
        return bool(self.regs[REG_CTRL7] & 0x02)

    @property
    def accel_lsb_per_g(self) -> int:
        return ACCEL_LSB_PER_G[(self.regs[REG_CTRL2] >> 4) & 0x07 & 3]

    @property
    def gyro_lsb_per_dps(self) -> int:
        return GYRO_LSB_PER_DPS[(self.regs[REG_CTRL3] >> 4) & 0x07]

    def refresh(self) -> None:
        """Latch the motion model into the data registers for the enabled sensors."""
        m = self.motion
        if self.accel_enabled or self.gyro_enabled:
            struct.pack_into('<h', self.regs, REG_TEMP_L, _s16(m.temp_c * 256))
        if self.accel_enabled:
            k = self.accel_lsb_per_g
            struct.pack_into('<hhh', self.regs, REG_TEMP_L + 2, *(_s16(a * k) for a in m.accel))
        if self.gyro_enabled:
            k = self.gyro_lsb_per_dps
            struct.pack_into('<hhh', self.regs, REG_TEMP_L + 8, *(_s16(g * k) for g in m.gyro))
        self.regs[REG_STATUS0] = (0x01 if self.accel_enabled else 0) | (0x02 if self.gyro_enabled else 0)

    def write(self, reg: int, val: int) -> None:
        reg &= 0x7F
        if REG_CTRL1 <= reg <= REG_CTRL9:
            self.regs[reg] = val & 0xFF
        elif reg == REG_RESET and val == 0xB0:
            self.reset()

    def i2c(self, wbytes: bytes, rlen: int) -> bytes:
        if wbytes:
            self._ptr = wbytes[0] & 0x7F
            for i, b in enumerate(wbytes[1:]):
                self.write(self._ptr + i, b)
        if rlen:
            end = self._ptr + rlen
            if self._ptr <= REG_DATA_END and end > REG_STATUS0:
                self.refresh()
            return bytes(self.regs[(self._ptr + i) & 0x7F] for i in range(rlen))
        return b''

    def data_values(self) -> dict:
        """The data registers as the firmware would scale them."""
        raw = struct.unpack_from('<7h', self.regs, REG_TEMP_L)
        ka, kg = self.accel_lsb_per_g, self.gyro_lsb_per_dps
        return {
            'temp_c': round(raw[0] / 256, 2),
            'accel_g': [round(v / ka, 4) for v in raw[1:4]],
            'gyro_dps': [round(v / kg, 2) for v in raw[4:7]],
            'ctrl7': self.regs[REG_CTRL7],
            'accel_on': self.accel_enabled,
            'gyro_on': self.gyro_enabled,
        }


class Motion:
    """Tilt from the joystick (pitch/roll up to +-60 deg) -> accel = rotated gravity; gyro = angular rate of the
    tilt changes (smoothed) plus a yaw-rate control; tap = short accel spike; shake = 1 s oscillation.
    Flat, face up: accel = (0, 0, +1 g)."""

    MAX_TILT = 60.0
    TILT_TAU = 0.06   # s: how fast the coin follows the stick
    GYRO_TAU = 0.03   # s: gyro smoothing

    def __init__(self, noise: bool = True) -> None:
        self.target_pitch = 0.0
        self.target_roll = 0.0
        self.pitch = 0.0
        self.roll = 0.0
        self.yaw_rate = 0.0
        self.temp_c = 27.5
        self.noise = noise
        self._rate = [0.0, 0.0]
        self.accel = (0.0, 0.0, 1.0)
        self.gyro = (0.0, 0.0, 0.0)
        self._t = 0.0
        self._tap_t = None
        self._shake_t = None
        self._update_outputs(0.0, 0.0)

    def set_stick(self, x: float, y: float) -> None:
        """x, y in [-1, 1]; screen coordinates (y down). Stick right rolls right, stick up pitches forward."""
        x = max(-1.0, min(1.0, x))
        y = max(-1.0, min(1.0, y))
        self.target_roll = x * self.MAX_TILT
        self.target_pitch = -y * self.MAX_TILT

    def set_tilt(self, pitch: float, roll: float) -> None:
        self.target_pitch = max(-90.0, min(90.0, pitch))
        self.target_roll = max(-90.0, min(90.0, roll))

    def tap(self) -> None:
        self._tap_t = self._t

    def shake(self) -> None:
        self._shake_t = self._t

    @property
    def shaking(self) -> bool:
        return self._shake_t is not None

    def step(self, dt: float) -> None:
        if dt <= 0:
            return
        self._t += dt
        a = 1.0 - math.exp(-dt / self.TILT_TAU)
        new_pitch = self.pitch + (self.target_pitch - self.pitch) * a
        new_roll = self.roll + (self.target_roll - self.roll) * a
        rp, rr = (new_pitch - self.pitch) / dt, (new_roll - self.roll) / dt
        self.pitch, self.roll = new_pitch, new_roll
        b = 1.0 - math.exp(-dt / self.GYRO_TAU)
        self._rate[0] += (rp - self._rate[0]) * b
        self._rate[1] += (rr - self._rate[1]) * b
        self._update_outputs(self._rate[0], self._rate[1])

    def _update_outputs(self, rate_pitch: float, rate_roll: float) -> None:
        p, r = math.radians(self.pitch), math.radians(self.roll)
        # Body frame specific force at rest: R^T * (0, 0, 1) with R = Rx(pitch) * Ry(roll).
        ax = -math.sin(r)
        ay = math.sin(p) * math.cos(r)
        az = math.cos(p) * math.cos(r)
        gx, gy, gz = rate_pitch, rate_roll, self.yaw_rate
        if self._tap_t is not None:
            t = self._t - self._tap_t
            if t < 0.04:
                s = math.sin(math.pi * t / 0.04)
                az += 2.5 * s
                ax += 0.4 * s
                gx += 40 * s
            else:
                self._tap_t = None
        if self._shake_t is not None:
            t = self._t - self._shake_t
            if t < 1.0:
                env = math.sin(math.pi * t)
                w = 2 * math.pi * 6.0
                ax += 1.6 * env * math.sin(w * t)
                ay += 0.5 * env * math.sin(w * t + 1.0)
                gz += 220 * env * math.cos(w * t)
                gx += 60 * env * math.cos(w * t + 0.5)
            else:
                self._shake_t = None
        if self.noise:
            ax += random.gauss(0, 0.002)
            ay += random.gauss(0, 0.002)
            az += random.gauss(0, 0.002)
            gx += random.gauss(0, 0.05)
            gy += random.gauss(0, 0.05)
            gz += random.gauss(0, 0.05)
        self.accel = (ax, ay, az)
        self.gyro = (gx, gy, gz)

    def snapshot(self) -> dict:
        return {
            'pitch': round(self.pitch, 2),
            'roll': round(self.roll, 2),
            'target_pitch': round(self.target_pitch, 2),
            'target_roll': round(self.target_roll, 2),
            'yaw_rate': self.yaw_rate,
            'accel_g': [round(v, 4) for v in self.accel],
            'gyro_dps': [round(v, 2) for v in self.gyro],
            'shaking': self.shaking,
        }
