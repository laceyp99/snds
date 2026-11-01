"""Generates testdata/tone-stereo-44100.mp3: 0.5 s of two synthetic sine tones (440 Hz left,
660 Hz right) with 20 ms fades, encoded by libsndfile's MP3 writer (LAME). No third-party audio.

Run from the repository root:
  worker\.venv\Scripts\python.exe -I testdata\generate_mp3_fixture.py
"""

import numpy as np
import soundfile as sf

sample_rate = 44100
frames = sample_rate // 2
t = np.arange(frames) / sample_rate
fade = np.minimum(1.0, np.minimum(t / 0.02, (t[-1] - t) / 0.02))
left = 0.4 * np.sin(2 * np.pi * 440 * t) * fade
right = 0.4 * np.sin(2 * np.pi * 660 * t) * fade
data = np.stack([left, right], axis=1).astype("float32")

sf.write("testdata/tone-stereo-44100.mp3", data, sample_rate, format="MP3", subtype="MPEG_LAYER_III")
info = sf.info("testdata/tone-stereo-44100.mp3")
print(f"wrote {info.frames} frames at {info.samplerate} Hz, {info.channels} channels")
