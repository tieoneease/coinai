"""Microphone sources (-> MIC frames, stereo s16 16 kHz) and speaker level metering."""

from __future__ import annotations

import collections
import logging
import math
import pathlib

import numpy as np

log = logging.getLogger('coinsim.audio')

RATE = 16000
CHUNK = 320  # 20 ms
NOISE_FLOOR_DBFS = -65.0
FULL_SCALE = 32767.0
SOURCES = ('silence', 'tone', 'file', 'browser')
AUDIO_EXTS = {'.ogg', '.opus', '.wav', '.mp3', '.flac', '.m4a', '.aac', '.webm'}


def dbfs(samples: np.ndarray) -> float:
    if samples.size == 0:
        return -120.0
    rms = math.sqrt(float(np.mean(np.square(samples.astype(np.float64)))))
    return 20 * math.log10(rms / FULL_SCALE) if rms > 0 else -120.0


def decode_file(path: str | pathlib.Path) -> np.ndarray:
    """Decode any audio file `av` can read into mono s16 at 16 kHz."""
    import av  # local import: keep start-up fast

    chunks = []
    with av.open(str(path)) as container:
        stream = container.streams.audio[0]
        resampler = av.AudioResampler(format='s16', layout='mono', rate=RATE)
        for frame in container.decode(stream):
            for f in resampler.resample(frame):
                chunks.append(f.to_ndarray().reshape(-1))
        for f in resampler.resample(None):
            chunks.append(f.to_ndarray().reshape(-1))
    if not chunks:
        return np.zeros(0, dtype=np.int16)
    return np.concatenate(chunks).astype(np.int16)


def to_stereo_bytes(mono: np.ndarray) -> bytes:
    m = mono.astype('<i2')
    return np.column_stack((m, m)).reshape(-1).tobytes()


class MicSource:
    """Produces mono s16 chunks for the selected source. Stereo duplication happens at send time."""

    def __init__(self) -> None:
        self.kind = 'silence'
        self.tone_hz = 1000.0
        self.tone_dbfs = -20.0
        self.file_path: str | None = None
        self.file_label: str | None = None
        self.file_pcm: np.ndarray | None = None
        self.loop = False
        self.file_pos = 0
        self._phase = 0.0
        self._browser = collections.deque()  # np.int16 arrays
        self._browser_len = 0
        self._rng = np.random.default_rng()
        self.browser_last_rx = 0.0

    # ---------------------------------------------------------------- control
    def rewind(self) -> None:
        """Called when the coin opens its microphone: a file starts from the beginning."""
        self.file_pos = 0
        self._phase = 0.0

    def set_file(self, path: str, label: str, pcm: np.ndarray) -> None:
        self.file_path = path
        self.file_label = label
        self.file_pcm = pcm
        self.file_pos = 0

    def push_browser(self, pcm: np.ndarray, now: float) -> None:
        self.browser_last_rx = now
        self._browser.append(pcm)
        self._browser_len += pcm.size
        while self._browser_len > RATE:  # keep at most 1 s
            old = self._browser.popleft()
            self._browser_len -= old.size

    # ---------------------------------------------------------------- generation
    def _noise(self, n: int) -> np.ndarray:
        sigma = FULL_SCALE * 10 ** (NOISE_FLOOR_DBFS / 20)
        return self._rng.normal(0.0, sigma, n)

    def read(self, n: int = CHUNK) -> np.ndarray:
        if self.kind == 'tone':
            amp = FULL_SCALE * math.sqrt(2) * 10 ** (self.tone_dbfs / 20)
            t = self._phase + np.arange(n) * (2 * math.pi * self.tone_hz / RATE)
            self._phase = float((self._phase + n * 2 * math.pi * self.tone_hz / RATE) % (2 * math.pi))
            out = amp * np.sin(t) + self._noise(n)
        elif self.kind == 'file' and self.file_pcm is not None and self.file_pcm.size:
            pcm = self.file_pcm
            out = np.zeros(n)
            filled = 0
            while filled < n:
                if self.file_pos >= pcm.size:
                    if not self.loop:
                        break
                    self.file_pos = 0
                take = min(n - filled, pcm.size - self.file_pos)
                out[filled:filled + take] = pcm[self.file_pos:self.file_pos + take]
                self.file_pos += take
                filled += take
            out[filled:] = self._noise(n - filled)
        elif self.kind == 'browser':
            out = np.zeros(n)
            filled = 0
            while filled < n and self._browser:
                head = self._browser[0]
                take = min(n - filled, head.size)
                out[filled:filled + take] = head[:take]
                if take == head.size:
                    self._browser.popleft()
                else:
                    self._browser[0] = head[take:]
                self._browser_len -= take
                filled += take
            out[filled:] = self._noise(n - filled)
        else:
            out = self._noise(n)
        return np.clip(np.round(out), -32768, 32767).astype(np.int16)

    def snapshot(self) -> dict:
        dur = None
        if self.file_pcm is not None:
            dur = round(self.file_pcm.size / RATE, 2)
        return {
            'source': self.kind,
            'tone_hz': self.tone_hz,
            'tone_dbfs': self.tone_dbfs,
            'file': self.file_label,
            'file_duration_s': dur,
            'file_pos_s': round(self.file_pos / RATE, 2),
            'loop': self.loop,
            'browser_buffer_ms': int(self._browser_len * 1000 / RATE),
        }
