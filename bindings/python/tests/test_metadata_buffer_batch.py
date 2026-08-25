"""MetadataBuffer batching contract tests."""

from ctypes import c_size_t, sizeof

import pytest

import iio


class FakeDevice:
    _device = object()
    sample_size = 8


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
    buffer.close()


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
