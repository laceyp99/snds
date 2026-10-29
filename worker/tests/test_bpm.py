from __future__ import annotations

import numpy as np
import pytest

from audio_fixtures import drum_loop, synth_song
from snds_worker.bpm import BPM_MAX, BPM_MIN, analyze_bpm, estimate_tempo

SR = 44100


@pytest.mark.parametrize("bpm", [90.0, 120.0, 128.0, 140.0])
def test_drum_loops_within_one_percent(bpm: float) -> None:
    r = estimate_tempo(drum_loop(bpm, 30, seed=int(bpm * 10)).mean(axis=1), SR)
    assert r["status"] == "ok"
    assert abs(r["bpm"] / bpm - 1) < 0.01
    assert 0.0 <= r["confidence"] <= 1.0


@pytest.mark.parametrize("bpm", [70.0, 174.0])
def test_octave_ambiguous_tempos_offer_the_right_alternative(bpm: float) -> None:
    """The prior folds 70 -> 140 and 174 -> 87; the half/double alternative carries the truth
    with high relative salience, which is what the app's half/double correction uses."""
    r = estimate_tempo(drum_loop(bpm, 30, seed=int(bpm * 10)).mean(axis=1), SR)
    options = [r["bpm"]] + [a["bpm"] for a in (r["half"], r["double"]) if a]
    assert any(abs(o / bpm - 1) < 0.01 for o in options)
    alt = r["half"] if r["bpm"] > bpm else r["double"]
    assert alt["relative_salience"] > 0.8


def test_full_song_tempo() -> None:
    r = estimate_tempo(synth_song(30, bpm=128.0, seed=7).mean(axis=1), SR)
    assert abs(r["bpm"] - 128.0) < 0.5


def test_other_sample_rate() -> None:
    from scipy.signal import resample_poly

    x = resample_poly(drum_loop(120.0, 20).mean(axis=1), 160, 147)  # 44.1k -> 48k
    r = estimate_tempo(x, 48000)
    assert abs(r["bpm"] - 120.0) < 1.0


def test_short_and_silent_inputs() -> None:
    assert estimate_tempo(np.zeros(SR * 3, np.float32), SR)["status"] == "insufficient-audio"
    assert estimate_tempo(np.zeros(SR * 10, np.float32), SR)["status"] == "no-onsets"
    assert estimate_tempo(np.full(SR * 10, 0.5, np.float32), SR)["status"] == "no-onsets"


def test_alternatives_stay_in_range() -> None:
    r = estimate_tempo(drum_loop(140.0, 20).mean(axis=1), SR)
    assert r["double"] is None  # 280 is outside the range
    assert BPM_MIN <= r["half"]["bpm"] <= BPM_MAX


def test_analyze_prefers_drums_and_records_candidates() -> None:
    drums = drum_loop(128.0, 20).mean(axis=1)
    rng = np.random.default_rng(0)
    mix = drums + 0.05 * rng.standard_normal(drums.size).astype(np.float32)
    r = analyze_bpm(drums, mix, SR)
    assert r["status"] == "ok" and r["signal"] == "drums"
    assert abs(r["value"] - 128.0) < 0.5
    assert set(r["candidates"]) == {"drums", "mix"}
    assert r["range"] == [BPM_MIN, BPM_MAX]
    assert r["estimator"]["name"] == "snds-acf-tempo"


def test_analyze_falls_back_to_mix_when_drums_are_quiet() -> None:
    mix = drum_loop(120.0, 20).mean(axis=1)
    r = analyze_bpm(mix * 1e-3, mix, SR)
    assert r["signal"] == "mix"
    assert r["candidates"]["drums"]["status"] == "skipped-quiet"
    assert r["drums_relative_level_db"] < -30


def test_analyze_reports_unavailable() -> None:
    r = analyze_bpm(np.zeros(SR * 2, np.float32), np.zeros(SR * 2, np.float32), SR)
    assert r["status"] == "insufficient-audio" and r["value"] is None and r["signal"] is None
