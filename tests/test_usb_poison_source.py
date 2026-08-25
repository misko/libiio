"""Static regressions for USB poisoned-pipe ownership ordering."""

from pathlib import Path


USB_SOURCE = (Path(__file__).parent.parent / "usb.c").read_text(encoding="utf-8")


def _function(name: str, following: str) -> str:
    start = USB_SOURCE.index(f"static int {name}(")
    end = USB_SOURCE.index(f"static int {following}(", start)
    return USB_SOURCE[start:end]


def _assert_fresh_pipe_reset(body: str, open_call: str) -> None:
    opened_check = body.index("if (pdata->opened)")
    pipe_open = body.index("usb_open_pipe(")
    poison_reset = body.index("usb_io_context_reset_cancelled(&pdata->io_ctx)")
    wire_open = body.index(open_call)
    assert opened_check < pipe_open < poison_reset < wire_open
    assert "pdata->io_ctx.cancelled = false" not in body


def test_ordinary_open_resets_poison_only_on_a_fresh_pipe() -> None:
    _assert_fresh_pipe_reset(
        _function("usb_open", "usb_open_with_metadata"),
        "iiod_client_open_unlocked(",
    )


def test_metadata_open_resets_poison_only_on_a_fresh_pipe() -> None:
    _assert_fresh_pipe_reset(
        _function("usb_open_with_metadata", "usb_close"),
        "iiod_client_open_with_metadata_unlocked(",
    )


def test_close_reads_poison_through_the_io_context_lock() -> None:
    start = USB_SOURCE.index("static int usb_close(")
    end = USB_SOURCE.index("static ssize_t usb_read(", start)
    close = USB_SOURCE[start:end]
    assert "usb_io_context_is_cancelled(&pdata->io_ctx)" in close
    assert "pdata->io_ctx.cancelled" not in close


def test_poison_accessors_hold_the_io_context_lock() -> None:
    read_start = USB_SOURCE.index("static bool usb_io_context_is_cancelled(")
    reset_start = USB_SOURCE.index("static void usb_io_context_reset_cancelled(")
    version_start = USB_SOURCE.index("static int usb_get_version(", reset_start)
    read = USB_SOURCE[read_start:reset_start]
    reset = USB_SOURCE[reset_start:version_start]
    assert read.index("iio_mutex_lock(io_ctx->lock)") < read.index(
        "cancelled = io_ctx->cancelled"
    ) < read.index("iio_mutex_unlock(io_ctx->lock)")
    assert reset.index("iio_mutex_lock(io_ctx->lock)") < reset.index(
        "io_ctx->cancelled = false"
    ) < reset.index("iio_mutex_unlock(io_ctx->lock)")
