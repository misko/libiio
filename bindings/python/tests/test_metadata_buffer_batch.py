"""MetadataBuffer batching contract tests."""

from ctypes import c_size_t, memmove, sizeof
import struct
from types import SimpleNamespace

import pytest

import iio


class FakeDevice:
    _device = object()
    sample_size = 8


class BurstFakeDevice(FakeDevice):
    _ctx = SimpleNamespace(
        attrs={
            "iio,buffer-ddr-burst": "1",
            "iio,buffer-ddr-burst-max-iq-bytes": "200000000",
        }
    )


class RingFakeDevice(BurstFakeDevice):
    _ctx = SimpleNamespace(
        attrs={
            **BurstFakeDevice._ctx.attrs,
            "iio,buffer-ddr-ring": "1",
            "iio,buffer-ddr-ring-max-iq-bytes": "200000000",
        }
    )


def test_metadata_batch_attestation_and_cleanup(monkeypatch):
    created = object()
    configured = []
    destroyed = []
    monkeypatch.setattr(iio, "_create_buffer_with_metadata", lambda *args: created)
    monkeypatch.setattr(
        iio,
        "_buffer_set_metadata_batch_size",
        lambda buffer, frames: configured.append((buffer, frames)),
    )
    monkeypatch.setattr(iio, "_buffer_destroy", destroyed.append)

    buffer = iio.MetadataBuffer(
        FakeDevice(),
        65536,
        b"request",
        metadata_capacity=65536,
        batch_frames=64,
    )
    assert configured == [(created, 64)]
    assert buffer.batch_frames == 64
    assert buffer.batch_cache_bytes == 64 * (
        65536 * 8 + 65536 + 2 * sizeof(c_size_t)
    )
    buffer.close()
    assert destroyed == [created]


def test_batch_one_preserves_uncached_behavior(monkeypatch):
    monkeypatch.setattr(iio, "_create_buffer_with_metadata", lambda *args: object())
    monkeypatch.setattr(iio, "_buffer_set_metadata_batch_size", lambda *args: None)
    monkeypatch.setattr(iio, "_buffer_destroy", lambda *args: None)

    buffer = iio.MetadataBuffer(
        FakeDevice(), 1, b"request", metadata_capacity=64 * 1024 * 1024
    )
    assert buffer.batch_frames == 1
    assert buffer.batch_cache_bytes == 0
    assert buffer.ddr_burst_enabled is False
    assert buffer.ddr_burst_requested_bytes == 0
    assert buffer.ddr_burst_admitted_bytes == 0
    assert buffer.ddr_burst_frames == 0
    assert buffer.ddr_ring_enabled is False
    assert buffer.ddr_ring_requested_bytes == 0
    assert buffer.ddr_ring_admitted_bytes == 0
    assert buffer.ddr_ring_capacity_frames == 0
    assert buffer.ddr_ring_capture_frames == 0
    assert buffer.ddr_ring_continuous is False
    buffer.close()


def test_device_ddr_burst_appends_versioned_request(monkeypatch):
    created = object()
    calls = []

    def create(*args):
        calls.append(args)
        return created

    monkeypatch.setattr(iio, "_create_buffer_with_metadata", create)
    monkeypatch.setattr(iio, "_buffer_set_metadata_batch_size", lambda *args: None)
    monkeypatch.setattr(iio, "_buffer_destroy", lambda *args: None)

    buffer = iio.MetadataBuffer(
        BurstFakeDevice(), 1024, b"provider", ddr_burst_bytes=32769
    )
    assert buffer.ddr_burst_enabled is True
    assert buffer.ddr_burst_requested_bytes == 32769
    assert buffer.ddr_burst_admitted_bytes == 32768
    assert buffer.ddr_burst_frames == 4
    _, samples, storage, request_bytes = calls[0]
    assert samples == 1024
    assert request_bytes == len(b"provider") + 32
    assert storage.raw[: len(b"provider")] == b"provider"
    assert struct.unpack("<IHHIIQQ", storage.raw[-32:]) == (
        0x42524653,
        1,
        32,
        1,
        0,
        32769,
        0,
    )
    buffer.close()


@pytest.mark.parametrize("ddr_burst_bytes", [True, 1.5, "8192"])
def test_device_ddr_burst_type_is_exact(monkeypatch, ddr_burst_bytes):
    monkeypatch.setattr(
        iio,
        "_create_buffer_with_metadata",
        lambda *args: pytest.fail("validation must happen before buffer creation"),
    )
    with pytest.raises(TypeError):
        iio.MetadataBuffer(
            BurstFakeDevice(), 1024, b"provider", ddr_burst_bytes=ddr_burst_bytes
        )


@pytest.mark.parametrize("ddr_burst_bytes", [-1, 8191, 200000001])
def test_device_ddr_burst_bounds_precede_creation(monkeypatch, ddr_burst_bytes):
    monkeypatch.setattr(
        iio,
        "_create_buffer_with_metadata",
        lambda *args: pytest.fail("validation must happen before buffer creation"),
    )
    with pytest.raises(ValueError):
        iio.MetadataBuffer(
            BurstFakeDevice(), 1024, b"provider", ddr_burst_bytes=ddr_burst_bytes
        )


def test_device_ddr_burst_requires_capability_and_single_host_batch(monkeypatch):
    monkeypatch.setattr(
        iio,
        "_create_buffer_with_metadata",
        lambda *args: pytest.fail("validation must happen before buffer creation"),
    )
    with pytest.raises(OSError, match="does not advertise"):
        iio.MetadataBuffer(FakeDevice(), 1024, b"provider", ddr_burst_bytes=8192)
    with pytest.raises(ValueError, match="batch_frames=1"):
        iio.MetadataBuffer(
            BurstFakeDevice(),
            1024,
            b"provider",
            batch_frames=2,
            ddr_burst_bytes=8192,
        )


@pytest.mark.parametrize(
    ("continuous", "capture_frames", "expected_flags"),
    [(False, 17, 1), (True, 0, 2)],
)
def test_device_ddr_ring_appends_versioned_request(
    monkeypatch, continuous, capture_frames, expected_flags
):
    calls = []
    monkeypatch.setattr(
        iio, "_create_buffer_with_metadata", lambda *args: calls.append(args) or object()
    )
    monkeypatch.setattr(iio, "_buffer_set_metadata_batch_size", lambda *args: None)
    monkeypatch.setattr(iio, "_buffer_destroy", lambda *args: None)

    buffer = iio.MetadataBuffer(
        RingFakeDevice(),
        1024,
        b"provider",
        ddr_ring_bytes=32769,
        ddr_ring_frames=capture_frames,
        ddr_ring_continuous=continuous,
    )
    assert buffer.ddr_ring_enabled is True
    assert buffer.ddr_ring_requested_bytes == 32769
    assert buffer.ddr_ring_admitted_bytes == 32768
    assert buffer.ddr_ring_capacity_frames == 4
    assert buffer.ddr_ring_capture_frames == capture_frames
    assert buffer.ddr_ring_continuous is continuous
    _, _, storage, request_bytes = calls[0]
    assert request_bytes == len(b"provider") + 48
    assert struct.unpack("<IHHIIQQQQ", storage.raw[-48:]) == (
        0x52524653,
        1,
        48,
        1,
        expected_flags,
        32769,
        capture_frames,
        0,
        0,
    )
    buffer.close()


def test_device_ddr_ring_status_decodes_atomic_snapshot(monkeypatch):
    wire = struct.pack(
        "<IHHIIIi13Q",
        0x53524653,
        1,
        128,
        3,
        0,
        1,
        0,
        32769,
        32768,
        17,
        9,
        7,
        4,
        2,
        1,
        3,
        123456,
        0,
        0,
        0,
    )
    monkeypatch.setattr(iio, "_create_buffer_with_metadata", lambda *args: object())
    monkeypatch.setattr(iio, "_buffer_set_metadata_batch_size", lambda *args: None)
    monkeypatch.setattr(iio, "_buffer_destroy", lambda *args: None)
    monkeypatch.setattr(
        iio,
        "_buffer_get_metadata_status",
        lambda _buffer, destination, _capacity: memmove(destination, wire, len(wire))
        and len(wire),
    )
    buffer = iio.MetadataBuffer(
        RingFakeDevice(), 1024, b"provider", ddr_ring_bytes=32769,
        ddr_ring_frames=17
    )
    status = buffer.ddr_ring_status()
    assert status["version"] == 1
    assert status["state"] == "draining"
    assert status["produced_frames"] == 9
    assert status["consumed_frames"] == 7
    assert status["high_water_frames"] == 4
    assert status["wrap_count"] == 2
    assert status["last_contiguous_sample_sequence"] == 123456
    assert status["first_unavailable_sample_sequence"] is None
    assert status["failure_frame_index"] is None
    assert status["failure_sample_sequence"] is None
    buffer.close()


def test_device_ddr_ring_status_v2_decodes_typed_failure(monkeypatch):
    wire = struct.pack(
        "<IHHIIIi13Q",
        0x53524653,
        2,
        128,
        5,
        9,
        0xC,
        -84,
        32769,
        32768,
        17,
        7,
        7,
        4,
        2,
        1,
        1,
        0,
        0,
        0,
        123456,
    )
    monkeypatch.setattr(iio, "_create_buffer_with_metadata", lambda *args: object())
    monkeypatch.setattr(iio, "_buffer_set_metadata_batch_size", lambda *args: None)
    monkeypatch.setattr(iio, "_buffer_destroy", lambda *args: None)
    monkeypatch.setattr(
        iio,
        "_buffer_get_metadata_status",
        lambda _buffer, destination, _capacity: memmove(destination, wire, len(wire))
        and len(wire),
    )
    buffer = iio.MetadataBuffer(
        RingFakeDevice(), 1024, b"provider", ddr_ring_bytes=32769,
        ddr_ring_frames=17
    )
    status = buffer.ddr_ring_status()
    assert status["version"] == 2
    assert status["state"] == "failed"
    assert status["terminal_reason"] == "gain_event_gap"
    assert status["failure_frame_index"] == 0
    assert status["failure_sample_sequence"] == 123456
    buffer.close()


@pytest.mark.parametrize(
    "version,reason,produced,consumed,high_water",
    [
        (1, 9, 7, 7, 4),
        (2, 9, 7, 8, 4),
        (2, 9, 7, 7, 8),
    ],
)
def test_device_ddr_ring_status_rejects_hostile_relations(
    monkeypatch, version, reason, produced, consumed, high_water
):
    wire = struct.pack(
        "<IHHIIIi13Q",
        0x53524653,
        version,
        128,
        5,
        reason,
        0,
        -84,
        32769,
        32768,
        17,
        produced,
        consumed,
        high_water,
        2,
        1,
        1,
        0,
        0,
        0,
        0,
    )
    monkeypatch.setattr(iio, "_create_buffer_with_metadata", lambda *args: object())
    monkeypatch.setattr(iio, "_buffer_set_metadata_batch_size", lambda *args: None)
    monkeypatch.setattr(iio, "_buffer_destroy", lambda *args: None)
    monkeypatch.setattr(
        iio,
        "_buffer_get_metadata_status",
        lambda _buffer, destination, _capacity: memmove(destination, wire, len(wire))
        and len(wire),
    )
    buffer = iio.MetadataBuffer(
        RingFakeDevice(), 1024, b"provider", ddr_ring_bytes=32769,
        ddr_ring_frames=17
    )
    with pytest.raises(OSError):
        buffer.ddr_ring_status()
    buffer.close()


@pytest.mark.parametrize(
    "kwargs",
    [
        {"ddr_ring_bytes": 8192},
        {"ddr_ring_bytes": 8192, "ddr_ring_frames": 1, "ddr_ring_continuous": True},
        {"ddr_ring_bytes": 0, "ddr_ring_frames": 1},
        {"ddr_ring_bytes": 8192, "ddr_ring_frames": 1, "ddr_burst_bytes": 8192},
        {"ddr_ring_bytes": 8192, "ddr_ring_frames": 1, "batch_frames": 2},
    ],
)
def test_device_ddr_ring_rejects_ambiguous_modes(monkeypatch, kwargs):
    monkeypatch.setattr(
        iio,
        "_create_buffer_with_metadata",
        lambda *args: pytest.fail("validation must happen before buffer creation"),
    )
    with pytest.raises(ValueError):
        iio.MetadataBuffer(RingFakeDevice(), 1024, b"provider", **kwargs)


def test_device_ddr_ring_requires_capability(monkeypatch):
    monkeypatch.setattr(
        iio,
        "_create_buffer_with_metadata",
        lambda *args: pytest.fail("validation must happen before buffer creation"),
    )
    with pytest.raises(OSError, match="does not advertise"):
        iio.MetadataBuffer(
            BurstFakeDevice(), 1024, b"provider", ddr_ring_bytes=8192,
            ddr_ring_frames=1
        )


@pytest.mark.parametrize("batch_frames", [True, 1.5, "2"])
def test_batch_frame_type_is_exact(monkeypatch, batch_frames):
    monkeypatch.setattr(
        iio,
        "_create_buffer_with_metadata",
        lambda *args: pytest.fail("validation must happen before buffer creation"),
    )
    with pytest.raises(TypeError):
        iio.MetadataBuffer(FakeDevice(), 1, b"request", batch_frames=batch_frames)


@pytest.mark.parametrize("batch_frames", [0, 65])
def test_batch_frame_bounds(monkeypatch, batch_frames):
    monkeypatch.setattr(
        iio,
        "_create_buffer_with_metadata",
        lambda *args: pytest.fail("validation must happen before buffer creation"),
    )
    with pytest.raises(ValueError):
        iio.MetadataBuffer(FakeDevice(), 1, b"request", batch_frames=batch_frames)


def test_batch_cache_bound_precedes_creation(monkeypatch):
    monkeypatch.setattr(
        iio,
        "_create_buffer_with_metadata",
        lambda *args: pytest.fail("validation must happen before buffer creation"),
    )
    with pytest.raises(ValueError, match="64 MiB"):
        iio.MetadataBuffer(FakeDevice(), 131072, b"request", batch_frames=64)


def test_setter_failure_destroys_created_buffer(monkeypatch):
    created = object()
    destroyed = []
    monkeypatch.setattr(iio, "_create_buffer_with_metadata", lambda *args: created)

    def fail_setter(*args):
        raise OSError("unsupported")

    monkeypatch.setattr(iio, "_buffer_set_metadata_batch_size", fail_setter)
    monkeypatch.setattr(iio, "_buffer_destroy", destroyed.append)

    with pytest.raises(OSError):
        iio.MetadataBuffer(FakeDevice(), 1, b"request", batch_frames=2)
    assert destroyed == [created]
