import io
import json

import pytest

from snds_worker import jobs_protocol as jp

SHA = "ab" * 32


def request(**overrides):
    base = {
        "protocol": 1,
        "type": "prepare",
        "jobId": "job-1",
        "source": {"path": "C:\\Music\\song.wav", "sizeBytes": 1234, "sha256": SHA},
        "jobDir": "C:\\snds\\jobs\\job-1",
        "cacheDir": f"C:\\snds\\cache\\{SHA}-1234",
        "compute": {"preference": "auto"},
    }
    base.update(overrides)
    return base


def parse(payload):
    return jp.parse_prepare_request(json.dumps(payload))


def test_valid_request_without_artwork():
    parsed = parse(request())
    assert parsed.job_id == "job-1"
    assert parsed.source == jp.SourceInfo("C:\\Music\\song.wav", 1234, SHA)
    assert parsed.compute_preference == "auto"
    assert parsed.artwork is None


def test_valid_request_with_artwork_and_extra_fields():
    parsed = parse(
        request(
            artwork={"path": "C:\\snds\\jobs\\job-1\\art.png", "origin": "embedded"},
            futureField=[1, 2],
        )
    )
    assert parsed.artwork == jp.ArtworkInfo("C:\\snds\\jobs\\job-1\\art.png", "embedded")


def test_artwork_null_is_allowed():
    assert parse(request(artwork=None)).artwork is None


def test_non_ascii_paths_round_trip_through_ascii_json():
    path = "C:\\M\u00fasic\\\u00dcn\u00efc\u00f8d\u00e9 \u66f2 \U0001f3b5.wav"
    payload = request(source={"path": path, "sizeBytes": 1, "sha256": SHA})
    line = json.dumps(payload)
    assert line.isascii()
    assert jp.parse_prepare_request(line).source.path == path
    # A raw UTF-8 line (bytes) parses too.
    raw = json.dumps(payload, ensure_ascii=False)
    assert jp.parse_prepare_request(raw.encode("utf-8")).source.path == path


@pytest.mark.parametrize("line", ["", "   ", "{not json", "[1, 2]", '"text"', "null"])
def test_malformed_requests_exit_10(line):
    with pytest.raises(jp.RequestError) as caught:
        jp.parse_prepare_request(line)
    assert caught.value.exit_code == jp.EXIT_INVALID_REQUEST
    assert caught.value.code == "invalid_request"


def test_invalid_utf8_bytes_exit_10():
    with pytest.raises(jp.RequestError) as caught:
        jp.parse_prepare_request(b"\xff\xfe{}")
    assert caught.value.exit_code == 10


def test_unsupported_protocol_exits_11_and_lists_supported():
    with pytest.raises(jp.RequestError) as caught:
        parse(request(protocol=2))
    error = caught.value
    assert error.exit_code == jp.EXIT_UNSUPPORTED_PROTOCOL
    event = error.to_failure()
    assert event["code"] == "unsupported_protocol_version"
    assert event["supportedProtocols"] == [1]
    assert event["retryable"] is False


@pytest.mark.parametrize("version", [None, "1", 1.0, True, [1]])
def test_missing_or_non_integer_protocol_is_invalid(version):
    payload = request()
    if version is None:
        del payload["protocol"]
    else:
        payload["protocol"] = version
    with pytest.raises(jp.RequestError) as caught:
        parse(payload)
    assert caught.value.exit_code == 10


def _without(payload, *path):
    node = payload
    for key in path[:-1]:
        node = node[key]
    del node[path[-1]]
    return payload


@pytest.mark.parametrize(
    "mutate",
    [
        lambda p: p.update(type="job"),
        lambda p: _without(p, "type"),
        lambda p: _without(p, "jobId"),
        lambda p: p.update(jobId="../escape"),
        lambda p: p.update(jobId=""),
        lambda p: p.update(jobId="x" * 65),
        lambda p: _without(p, "source"),
        lambda p: p.update(source="C:\\a.wav"),
        lambda p: _without(p, "source", "path"),
        lambda p: p["source"].update(path="relative\\song.wav"),
        lambda p: p["source"].update(path="\\rooted\\a.wav"),
        lambda p: p["source"].update(sizeBytes=-1),
        lambda p: p["source"].update(sizeBytes="12"),
        lambda p: p["source"].update(sizeBytes=True),
        lambda p: p["source"].update(sha256="AB" * 32),
        lambda p: p["source"].update(sha256="ab" * 31),
        lambda p: _without(p, "jobDir"),
        lambda p: p.update(cacheDir="cache\\x"),
        lambda p: _without(p, "compute"),
        lambda p: p["compute"].update(preference="cuda"),
        lambda p: p.update(artwork="art.png"),
        lambda p: p.update(artwork={"path": "C:\\a.png", "origin": "web"}),
        lambda p: p.update(artwork={"origin": "file"}),
    ],
)
def test_invalid_fields_exit_10(mutate):
    payload = request()
    mutate(payload)
    with pytest.raises(jp.RequestError) as caught:
        parse(payload)
    assert caught.value.exit_code == jp.EXIT_INVALID_REQUEST


def test_cpu_preference_is_valid():
    assert parse(request(compute={"preference": "cpu"})).compute_preference == "cpu"


def test_cancel_line_detection():
    assert jp.is_cancel_line('{"protocol":1,"type":"cancel"}')
    assert jp.is_cancel_line(b'{"type":"cancel"}')
    assert not jp.is_cancel_line("garbage")
    assert not jp.is_cancel_line('{"type":"ping"}')
    assert not jp.is_cancel_line("[1]")


def test_every_event_builder_sets_protocol_and_type():
    events = [
        jp.accepted("job-1", "cuda"),
        jp.stage("decoding"),
        jp.progress("separating", 0.5, 12.0),
        jp.cancelled("user"),
        jp.failure(
            "cuda_failed", "boom", details="trace", retryable=True, suggested_action="retry_cpu"
        ),
        jp.result("C:\\c", {"bpm": 120.0}),
    ]
    assert [e["type"] for e in events] == [
        "accepted",
        "stage",
        "progress",
        "cancelled",
        "failure",
        "result",
    ]
    assert all(e["protocol"] == 1 for e in events)
    assert events[0] == {"protocol": 1, "type": "accepted", "jobId": "job-1", "device": "cuda"}


def test_stage_index_and_count_follow_the_fixed_order():
    assert jp.STAGES == (
        "decoding",
        "separating",
        "normalizing",
        "analyzing_bpm",
        "validating",
        "finalizing",
    )
    event = jp.stage("analyzing_bpm")
    assert (event["index"], event["count"]) == (4, 6)
    with pytest.raises(ValueError):
        jp.stage("mixing")


def test_progress_clamps_and_omits_missing_eta():
    assert "etaSeconds" not in jp.progress("decoding", 0.25)
    assert jp.progress("decoding", 1.7)["fraction"] == 1.0
    assert jp.progress("decoding", -3)["fraction"] == 0.0
    assert jp.progress("decoding", 0.5, 4)["etaSeconds"] == 4.0
    with pytest.raises(ValueError):
        jp.progress("nope", 0.1)


def test_failure_shape():
    event = jp.failure("out_of_memory", "No memory", retryable=True, suggested_action="retry_cpu")
    assert event["retryable"] is True
    assert event["suggestedAction"] == "retry_cpu"
    assert "details" not in event
    assert jp.failure("internal_error", "x")["retryable"] is False


def test_exit_codes_match_terminal_events():
    assert jp.exit_code_for(jp.result("C:\\c", {})) == 0
    assert jp.exit_code_for(jp.cancelled()) == 12
    assert jp.exit_code_for(jp.failure("separation_failed", "x")) == 13
    assert jp.exit_code_for(jp.failure("invalid_request", "x")) == 10
    assert jp.exit_code_for(jp.failure("unsupported_protocol_version", "x")) == 11
    assert jp.exit_code_for(jp.failure("internal_error", "x")) == 14
    with pytest.raises(ValueError):
        jp.exit_code_for(jp.stage("decoding"))
    assert jp.EXIT_KILLED_BY_APP == 99


def test_failure_codes_are_unique_snake_case():
    assert len(set(jp.FAILURE_CODES)) == len(jp.FAILURE_CODES)
    assert all(code.replace("_", "").isalpha() and code.islower() for code in jp.FAILURE_CODES)


def test_writer_emits_ascii_ndjson_with_contiguous_seq():
    stream = io.StringIO()
    writer = jp.EventWriter(stream)
    writer.emit(jp.accepted("job-1", "cpu"))
    writer.emit(jp.result("C:\\M\u00fasic\\\u66f2", {"title": "\U0001f3b5"}))
    raw = stream.getvalue()
    assert raw.isascii()
    lines = raw.splitlines()
    assert len(lines) == 2
    decoded = [json.loads(line) for line in lines]
    assert [d["seq"] for d in decoded] == [1, 2]
    assert decoded[1]["cacheDir"] == "C:\\M\u00fasic\\\u66f2"
    assert decoded[1]["summary"]["title"] == "\U0001f3b5"


def test_writer_allows_exactly_one_terminal_event():
    stream = io.StringIO()
    writer = jp.EventWriter(stream)
    assert writer.emit(jp.cancelled())
    assert writer.terminal_sent
    assert not writer.emit(jp.progress("decoding", 0.1))
    assert not writer.emit(jp.result("C:\\c", {}))
    assert len(stream.getvalue().splitlines()) == 1


def test_writer_flushes_every_line():
    flushes = []

    class Recorder(io.StringIO):
        def flush(self):
            flushes.append(self.tell())
            super().flush()

    writer = jp.EventWriter(Recorder())
    writer.emit(jp.stage("decoding"))
    writer.emit(jp.progress("decoding", 0.5))
    assert len(flushes) == 2
    assert flushes[0] < flushes[1]


def test_writer_rejects_nan():
    writer = jp.EventWriter(io.StringIO())
    with pytest.raises(ValueError):
        writer.emit(jp.result("C:\\c", {"x": float("nan")}))


def test_open_stdout_writes_to_a_private_duplicate(capfd):
    writer = jp.EventWriter.open_stdout()
    writer.emit(jp.accepted("j", "cpu"))
    out = capfd.readouterr().out
    assert json.loads(out.splitlines()[0])["type"] == "accepted"


def test_unc_source_paths_are_accepted():
    payload = request()
    unc = "\\\\server\\share\\a.wav"
    payload["source"]["path"] = unc
    assert parse(payload).source.path == unc
