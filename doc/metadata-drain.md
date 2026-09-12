# Metadata-only result drain, version 1

This opt-in network extension retrieves delayed provider results on the owning
OPENM connection after IQ is consumed. It does not initiate acquisition,
create/refill a receive buffer, or change the most recent IQ/metadata refill.
It is transport infrastructure for an asynchronous scanner classifier, **not
an enabled classifier or a claim of unchanged live RF duty**.

## Discovery and wire contract

The daemon advertises `iio,buffer-metadata-drain=1` when built with a metadata
provider. This advertises the operation, not the availability of a classifier.
The provider must separately opt in for the exact opaque OPENM request. A NULL
optional `iiod_buffer_burst_plan.drain_metadata` callback preserves legacy
sessions, which return `-ENODATA`. Existing providers need no new link symbol.
The existing `iio,buffer-metadata=3` capability and IQ framing are unchanged.

Request: `DRAINBUFM <device-id> <capacity>\r\n`.

Capacity is decimal, 1..65536 bytes. A response is either a strictly parsed
positive decimal length followed by exactly that many opaque bytes, or one
negative errno line without a payload. There is no IQ length, channel mask,
trailing newline or fabricated IQ block in a successful drain response. An
empty blob is invalid. Completion semantics belong to the provider schema;
the planned GLRT provider uses its versioned DRAIN/FINAL envelope.

| Response | Meaning |
| --- | --- |
| Positive length and bytes | One complete provider record/frame |
| `-EAGAIN` | Work pending; retry later within the caller's bounded deadline |
| `-EBUSY` | Capture still active or client has unread IQ responses |
| `-ENODATA` | Session opted out, provider no longer exists, or results exhausted |
| `-ENOSPC` | Buffer too small; provider must not consume the result |
| `-ENOSYS` | Client backend/server does not support this operation |

The protocol uses Linux negative errno values in [-4095, -1]. The client
rejects oversized, zero, malformed, truncated and narrowing-overflow replies.
After framing failure the network backend invalidates the stream, so remaining
payload cannot be misread as another response. A complete provider errno does
not invalidate the connection. As with existing transport operations, failed
delivery is not acknowledged or automatically replayed; the provider's result
sequence/final accounting must expose any missing evidence to the host.

## Ownership and lifecycle

`iio_buffer_drain_metadata(buf, output, capacity)` requires a metadata-enabled
buffer. It refuses unread direct-async responses, unread host-cached frames and
failed batches. API-v10 adds an optional callback at the end of the backend
operations; old backends are rejected before that field is accessed. Only the
network backend implements it. Its per-device lock serializes socket commands,
and a missing/unsupported advertised version sends no new command.

iiOD resolves the device only in the requesting parser's open-session list.
It pins the provider under the existing teardown lock and releases that lock
before sending the bounded response. The callback must serialize its result
consumer, reject active capture, check capacity before consuming, and never
wait for detector computation. This API does not stop RF by itself.

Finish capture and consume queued IQ, then drain, then CLOSE. Destructive
buffer cancellation or connection loss can prevent a final drain; hosts must
report incomplete evidence, not complete classification. A provider must emit
an explicit final marker, not infer completion from `-ENODATA` alone.

Direct-async currently retains the provider until close. Sealed-burst and
ordinary DDR-ring teardown can free it earlier. A future provider must reject
unsupported modes at negotiation or implement/test a distinct retained-results
lifetime; this transport change does not make those lifecycles equivalent.

Python `MetadataBuffer.drain_metadata(capacity=65536)` returns bytes without
updating `metadata` or refilling IQ. The native symbol is optional at import;
an older library raises `OSError(ENOSYS)` only when drain is requested. Calls on
one buffer must be serialized. Polling/deadlines belong to the session owner,
not a loop hidden inside libiio.

## Hardware-free verification

Configure an isolated desktop build with CMake, libxml2, bison and flex. No
installed runtime is overwritten. One tested configuration is:

```sh
cmake -S SOURCE -B BUILD -DCMAKE_BUILD_TYPE=Debug \
  -DWITH_USB_BACKEND=OFF -DWITH_SERIAL_BACKEND=OFF -DHAVE_DNS_SD=OFF \
  -DWITH_AIO=OFF -DWITH_IIOD_USBD=OFF -DWITH_IIOD_SERIAL=OFF \
  -DWITH_ZSTD=OFF -DWITH_TESTS=ON
cmake --build BUILD -j4
BUILD/tests/test_direct_async_transport
BUILD/iiod/test_metadata_drain_server
LD_LIBRARY_PATH=BUILD PYTHONPATH=bindings/python python -m pytest -q bindings/python/tests
```

- `test_direct_async_transport`: real buffer/client code with a mocked byte
  transport; pending/cached/failed gates, old backend, late results, malformed
  lengths and unchanged IQ. Ordinary refill still rejects zero IQ.
- `test_metadata_drain_server`: actual generated lexer/parser, handler and
  client with an XML-only device and synthetic already-open provider. Checks
  ownership, opt-out, teardown, busy/pending/exhaustion, small capacity and
  malformed requests. It never opens a receive buffer.
- `test_metadata_drain_network.py`: actual Python/C/network stack against a
  loopback-only peer. Checks capability fallback, one OPENM, unchanged refill
  data, retry/final/error replies, maximum binary payload and poisoned streams.
  This peer is not an emulator of acquisition or the GLRT provider.

Additional C regressions cover metadata batches, DMA block leases, command
batches, hop protocol/session/scheduler and worker affinity. Sanitized desktop
builds exercise the server/client fixture and core transport with ASan, UBSan
and leak detection. Python network tests can use that instrumented library
with the ASan runtime preloaded; Python-process leak detection is disabled for
that separate check. No hardware, RF, PostgreSQL or archival writes are needed.
