"""Level and hum analysis of coin clips (Ogg Opus), for comparing recording conditions.

Usage: python scripts/cliplevels.py a.ogg [b.ogg ...]
Prints the 100 ms RMS distribution in dBFS and, over the quietest second, the level at 50 Hz
and its harmonics (the 20 ms fast-advertising interval would show there) next to
off-harmonic reference frequencies.
"""
import sys

import av
import numpy as np


def decode(path):
    with av.open(path) as f:
        frames = [fr.to_ndarray().astype(np.float32).reshape(-1) for fr in f.decode(audio=0)]
        rate = f.streams.audio[0].codec_context.sample_rate
    x = np.concatenate(frames)
    if x.dtype.kind != "f" or np.abs(x).max() > 1.5:
        x = x / 32768.0
    return x, rate


def tone_db(sig, rate, hz):
    n = np.arange(len(sig))
    amp = 2 * np.abs(np.dot(sig, np.exp(-2j * np.pi * hz * n / rate))) / len(sig)
    return 20 * np.log10(amp + 1e-12)


def main():
    harmonics = [50, 100, 150, 200, 250, 300, 400, 500]
    refs = [75, 125, 175, 225, 275, 325, 450, 550]
    for path in sys.argv[1:]:
        x, rate = decode(path)
        win = rate // 10
        rms = np.sqrt([np.mean(x[i:i + win] ** 2) for i in range(0, len(x) - win + 1, win)])
        db = 20 * np.log10(rms + 1e-9)
        k = min(range(3, len(rms) - 9), key=lambda i: np.sum(rms[i:i + 10] ** 2))
        seg = x[k * win:(k + 10) * win]
        quiet = 10 * np.log10(np.mean(seg ** 2) + 1e-18)
        h = np.mean([tone_db(seg, rate, f) for f in harmonics])
        r = np.mean([tone_db(seg, rate, f) for f in refs])
        print(f"{path}: {len(x) / rate:.2f} s at {rate} Hz; 100 ms RMS dBFS min {db.min():.1f} "
              f"p10 {np.percentile(db, 10):.1f} median {np.median(db):.1f} max {db.max():.1f}")
        print(f"   quietest second at {k / 10:.1f} s: {quiet:.1f} dBFS; 50 Hz harmonics avg {h:.1f} dB, "
              f"off-harmonic refs avg {r:.1f} dB ({h - r:+.1f} dB)")
        print("   " + "  ".join(f"{f}:{tone_db(seg, rate, f):.0f}" for f in harmonics))


if __name__ == "__main__":
    main()
