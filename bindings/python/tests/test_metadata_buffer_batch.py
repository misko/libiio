"""MetadataBuffer batching contract tests."""

from ctypes import c_size_t, sizeof
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
            "iio,buffer-ddr-burst-max-iq-bytes": "300000000",
        }
    )


class SingleRxBurstFakeDevice(BurstFakeDevice):
    sample_size = 4


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


@pytest.mark.parametrize("ddr_burst_bytes", [250_000_000, 300_000_000])
def test_device_ddr_burst_accepts_capacity_candidate_sizes(monkeypatch, ddr_burst_bytes):
    monkeypatch.setattr(iio, "_create_buffer_with_metadata", lambda *args: object())
    monkeypatch.setattr(iio, "_buffer_set_metadata_batch_size", lambda *args: None)
    monkeypatch.setattr(iio, "_buffer_destroy", lambda *args: None)

    buffer = iio.MetadataBuffer(
        SingleRxBurstFakeDevice(),
        500_000,
        b"provider",
        ddr_burst_bytes=ddr_burst_bytes,
    )
    assert buffer.ddr_burst_requested_bytes == ddr_burst_bytes
    assert buffer.ddr_burst_admitted_bytes == ddr_burst_bytes
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


@pytest.mark.parametrize("ddr_burst_bytes", [-1, 8191, 300000001])
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
