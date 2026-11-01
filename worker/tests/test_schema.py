import copy

import pytest

from snds_worker import model, schema

SHA = "cd" * 32


def good():
    frames, rate = 441_000, 44_100
    return schema.build_song_metadata(
        source={
            "fileName": "\u00dcn\u00ef \u66f2.wav",
            "path": "C:\\Music\\\u00dcn\u00ef \u66f2.wav",
            "sizeBytes": 1234,
            "sha256": SHA,
            "container": "wav",
            "sampleRate": rate,
            "channels": 2,
            "frames": frames,
            "durationSeconds": 10.0,
        },
        stems={n: schema.stem_entry(frames, rate, 0.9, n) for n in schema.STEM_NAMES},
        bpm={
            "value": 120.0,
            "confidence": 0.8,
            "beats": 20,
            "alternatives": [60.0, 240.0],
            "analyzer": {"name": "beat_this", "version": "1.1.0", "checkpointSha256": SHA},
            "userOverride": None,
        },
        worker={"version": "0.0.1", "protocol": 1, "torch": "2.1.2+cpu", "device": "cpu"},
        prepared_at="2026-10-04T12:00:00Z",
    )


def test_cache_key_and_stem_files():
    assert schema.cache_key(SHA, 99) == f"{SHA}-99"
    assert schema.stem_file_name("drums") == "drums.wav"


def test_good_document_is_complete():
    assert schema.classify(good()) == (schema.COMPLETE, "")
    assert schema.validate_song_metadata(good()) == []


def test_stem_integrity_metadata_is_optional_for_legacy_packages():
    data = good()
    assert all(
        "sha256" not in entry and "sizeBytes" not in entry for entry in data["stems"].values()
    )
    assert schema.classify(data) == (schema.COMPLETE, "")
    for entry in data["stems"].values():
        entry.update(sha256=SHA, sizeBytes=1234)
    assert schema.classify(data) == (schema.COMPLETE, "")


@pytest.mark.parametrize(
    "integrity",
    [
        {"sha256": SHA},
        {"sizeBytes": 1234},
        {"sha256": None, "sizeBytes": 1234},
        {"sha256": SHA.upper(), "sizeBytes": 1234},
        {"sha256": "g" * 64, "sizeBytes": 1234},
        {"sha256": "a" * 63, "sizeBytes": 1234},
        {"sha256": SHA, "sizeBytes": None},
        {"sha256": SHA, "sizeBytes": 0},
        {"sha256": SHA, "sizeBytes": -1},
        {"sha256": SHA, "sizeBytes": True},
        {"sha256": SHA, "sizeBytes": 1.5},
        {"sha256": SHA, "sizeBytes": "1234"},
    ],
)
def test_malformed_or_partial_stem_integrity_metadata_is_corrupt(integrity):
    data = good()
    data["stems"]["drums"].update(integrity)
    status, reason = schema.classify(data)
    assert status == schema.CORRUPT
    assert "stems.drums" in reason


def test_builder_pins_model_and_defaults_artwork():
    data = good()
    assert data["model"]["signature"] == model.MODEL_SIGNATURE
    assert data["model"]["sha256"] == model.MODEL_SHA256
    assert data["artwork"] == {"file": None, "origin": "fallback"}
    assert data["cacheSchema"] == 1
    assert data["status"] == "complete"


def test_artwork_and_user_override_are_accepted():
    data = good()
    data["artwork"] = {"file": "artwork.png", "origin": "embedded"}
    data["bpm"]["userOverride"] = {"value": 60.0, "mode": "half"}
    assert schema.classify(data)[0] == schema.COMPLETE


def test_failed_bpm_detection_is_a_complete_package():
    data = good()
    data["bpm"].update(value=None, confidence=0, beats=2, alternatives=[], error="too few beats")
    assert schema.classify(data) == (schema.COMPLETE, "")
    data["bpm"]["userOverride"] = {"value": 100.0, "mode": "numeric"}
    assert schema.classify(data)[0] == schema.COMPLETE


def test_bpm_error_is_optional_when_detection_succeeded():
    data = good()
    assert "error" not in data["bpm"]
    data["bpm"]["error"] = None
    assert schema.classify(data)[0] == schema.COMPLETE


@pytest.mark.parametrize("schema_number", [0, 2, 99])
def test_other_schema_numbers_are_incompatible(schema_number):
    data = good()
    data["cacheSchema"] = schema_number
    status, reason = schema.classify(data)
    assert status == schema.INCOMPATIBLE
    assert "cacheSchema" in reason


@pytest.mark.parametrize("value", [None, "1", 1.5, True])
def test_bad_schema_value_is_corrupt(value):
    data = good()
    data["cacheSchema"] = value
    assert schema.classify(data)[0] == schema.CORRUPT


@pytest.mark.parametrize("data", [None, [], "x", 5])
def test_non_object_is_corrupt(data):
    assert schema.classify(data)[0] == schema.CORRUPT


@pytest.mark.parametrize("status", ["preparing", "", None, "Complete"])
def test_status_not_complete_is_corrupt(status):
    data = good()
    data["status"] = status
    assert schema.classify(data)[0] == schema.CORRUPT
    assert schema.validate_song_metadata(data)


@pytest.mark.parametrize("stem", schema.STEM_NAMES)
def test_missing_stem_is_corrupt(stem):
    data = good()
    del data["stems"][stem]
    status, reason = schema.classify(data)
    assert status == schema.CORRUPT
    assert stem in reason


def test_stem_frames_must_match_source():
    data = good()
    data["stems"]["bass"]["frames"] -= 1
    assert schema.classify(data)[0] == schema.CORRUPT


def test_stem_must_be_stereo_float32():
    data = good()
    data["stems"]["drums"]["channels"] = 1
    assert schema.classify(data)[0] == schema.CORRUPT
    data = good()
    data["stems"]["drums"]["sampleFormat"] = "int16"
    assert schema.classify(data)[0] == schema.CORRUPT


@pytest.mark.parametrize(
    "mutate",
    [
        lambda d: d["source"].pop("sha256"),
        lambda d: d["source"].update(sha256="XYZ"),
        lambda d: d["bpm"].update(value=0),
        lambda d: d["bpm"].update(value=None),
        lambda d: d["bpm"].update(value=None, error=""),
        lambda d: d["bpm"].update(error="stale reason"),
        lambda d: d["bpm"].update(confidence=1.5),
        lambda d: d["bpm"].update(alternatives="60"),
        lambda d: d["bpm"].update(userOverride={"value": 70, "mode": "triple"}),
        lambda d: d["bpm"].pop("analyzer"),
        lambda d: d.pop("artwork"),
        lambda d: d["artwork"].update(origin="web"),
        lambda d: d["artwork"].update(file="..\\evil.png"),
        lambda d: d["worker"].pop("torch"),
        lambda d: d.update(preparedAt="yesterday"),
        lambda d: d.update(preparedAt="2026-10-04T12:00:00+02:00"),
        lambda d: d.pop("model"),
    ],
)
def test_structural_problems_are_corrupt(mutate):
    data = good()
    mutate(data)
    assert schema.classify(data)[0] == schema.CORRUPT


def test_other_model_or_protocol_is_incompatible():
    data = good()
    data["model"]["signature"] = "deadbeef"
    assert schema.classify(data)[0] == schema.INCOMPATIBLE
    data = good()
    data["worker"]["protocol"] = 2
    assert schema.classify(data)[0] == schema.INCOMPATIBLE


def test_classify_does_not_mutate_input():
    data = good()
    snapshot = copy.deepcopy(data)
    schema.classify(data)
    assert data == snapshot
