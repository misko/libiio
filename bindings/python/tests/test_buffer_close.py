"""Lifecycle regression tests for the Python buffer wrapper."""

import iio


def test_buffer_close_destroys_once_and_is_idempotent(monkeypatch):
    destroyed = []
    buffer = object.__new__(iio.Buffer)
    buffer._buffer = 0x1234

    monkeypatch.setattr(iio, "_buffer_destroy", destroyed.append)

    buffer.close()
    buffer.close()
    buffer.__del__()

    assert destroyed == [0x1234]
    assert buffer._buffer is None


def test_context_close_destroys_once_and_is_idempotent(monkeypatch):
    destroyed = []
    context = object.__new__(iio.Context)
    context._context = 0x5678

    monkeypatch.setattr(iio, "_destroy", destroyed.append)

    context.close()
    context.close()
    context.__del__()

    assert destroyed == [0x5678]
    assert context._context is None
