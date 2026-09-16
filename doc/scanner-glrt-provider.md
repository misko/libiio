# Opt-in scanner GLRT metadata provider

Implementation checkpoint, 2026-09-08. This is an experimental userspace
extension, not a stock libiio feature, a calibrated classifier, or a deployed
scanner. It changes no FPGA, kernel, or flashed firmware.

The SPF provider can now consume the public acquisition SDK from Leo revision
`73141b0f5279733a7baf755660639a1bba087bde`. It does not import that component's
private worker/pool structures or link detector numerics into iiOD. The SDK
owns the bounded history, isolated worker, result accounting, and LGC1 codec.

## Negotiation and ownership

The build is disabled unless `IIOD_SCANNER_GLRT_LIBRARY` names a built SDK.
It additionally requires the SPF persistent-hop provider and:

- `IIOD_SCANNER_GLRT_INCLUDE_DIR`: public `scanner_glrt.h` directory.
- `IIOD_SCANNER_GLRT_WORKER_PATH`: absolute, trusted runtime executable path.
- `IIOD_SCANNER_GLRT_TEMPLATES_2500000` / `..._5000000`: trusted template paths.
- `IIOD_SCANNER_GLRT_ALGORITHM_SHA256` / `..._CONFIGURATION_SHA256`: nonzero
  lowercase identities from the reviewed release manifest.

These identities are configured, not inferred by hashing the files at OPENM.
The release/install preflight must verify actual SDK, worker, and template
hashes against that manifest. Test identities are not release identities.
The SDK opens regular files owned by root/current euid, rejects writable-to-
others files and final symlinks, and passes pinned descriptors to the worker.

Capable builds advertise `iio,buffer-scanner-glrt=1`, the two identity attributes,
and `iio,buffer-scanner-glrt-mode=unqualified-evidence`. Transport drain support
alone does not imply scanner support. Nothing changes for a legacy request.

LGO1 explicitly wraps the original OPENM request. Its 96-byte little-endian
header is: magic/version/header length (8 bytes), total/legacy lengths (8),
nonzero generation (8), RX=1 and flags=0 (8), algorithm/configuration SHA-256
(64), followed by unchanged legacy bytes. Maximum request size is 4096 bytes.
No filesystem path comes from the client. Malformed requests, stale identities,
wrong RX/rate/dwell/bandwidth/profile geometry, and non-hop modes are rejected.

The current mode is bounded to 300 seconds, 120 ms valid visits, 2.5/5 MS/s,
dual-RX recording with RX1-only detection, and the exact CH1L..CH4L/CH1U..CH4U
pilot-centred profile mapping after the documented 9.75 GHz LNB conversion.
Other capture modes are not implicitly opted into classifier ownership.

## Capture and terminal delivery

Allocation and worker startup finish before buffer creation. Accepted HOPS
events retain their original sample intervals, even when reported after IQ.
The collector copies only RX1 into bounded history/worker slots and never
holds a DMA buffer while computing. Acquisition does not wait for GLRT.

Each IQ frame carries an LGC1 envelope around the unchanged V6/HOPS bytes, plus
up to four completed, ordered earlier-dwell results. The output reservation is
the existing maximum metadata size plus 704 bytes. Exact-gap inspection and
rebasing operate on the inner V6 header and preserve HOPS and classifier bytes.

Active-capture drain attempts return EBUSY without taking the collector lock.
The provider reserves each accepted carrier while holding its hop-state lock;
completion/cancellation cannot publish FINAL in the gap before that carrier
is wrapped. After the last carrier is encoded, the same-session
`iio_buffer_drain_metadata()` operation delivers pending results and one FINAL.
It never fabricates IQ or refills a receive buffer. See [drain protocol](metadata-drain.md).

Worker overload, invalid input, or failure becomes explicit unavailable
evidence. It does not discard IQ or enable a no-signal policy. Host-side
expected dwell inventory and transport sequence accounting are still required;
loss of a carrier or connection is not successful complete classification.

## Capture-protection opt-in (2026-09-09)

`IIOD_SCANNER_GLRT_CAPTURE_PROTECTION=ON` requires the updated acquisition SDK.
It is off by default and does not change the existing OPENM, LGC1, observation,
fixed-hop or adaptive-hop layouts. Bind the enabled setting and the following
engineering profile into the reviewed release's configuration identity:

| Control | Initial value |
| --- | ---: |
| Admission occupancy limit, including a partially collected dwell | 2 of 3 slots |
| Oldest submitted job age that prevents new admission | 250 ms |
| Outstanding-job watchdog | 500 ms |
| Metadata/GLRT callback budget | 80% of one source block period |
| Healthy block observations required to resume | 4 consecutive |

The complete metadata-format callback, including GLRT collection/framing, is
timed with a monotonic clock. Crossing its budget or observing missing source
samples suspends subsequent checks. This is a measurement after the callback,
not prediction or prevention of the first overrun, and excludes network-send,
refill, DMA/IRQ and other acquisition work. After suspension the SDK omits its
advisory RX1 history copy; the original dual-RX IQ still belongs to acquisition.
Interrupted checks never resume from stale or partially retained samples.

Queue/age shedding uses the existing `worker_busy` unavailable reason. Capture
pressure uses `incomplete_search`, zero search coverage and an unhealthy UNKNOWN
observation. Runtime counters distinguish these causes without adding new wire
reasons. Already submitted results can finish normally. Skips are never a
negative detection, never refresh the last-positive time, and do not override
the three-miss/two-second policy. Three consecutive unhealthy observations still
latch uniform scanning for the rest of the capture, even if GLRT later resumes.

The SDK checks deadlines on acquisition/result/drain calls. A timed-out owned
worker is signalled without waiting in those paths and disabled for this session;
final reaping remains in off-capture close. No arbitrary process is signalled,
and no restart is attempted on capture. Completed results are harvested before
the outstanding-job watchdog is evaluated. This is not a separate watchdog
thread, CPU reservation or an unconditional live-duty guarantee.

The component fixture tests both rates, explicit off/on builds, callback-budget
boundaries, sample-gap pressure, four-block recovery and an intentionally slow
metadata callback. The latter preserves the complete original IQ and legacy
metadata while recording the interrupted check as unavailable. These are
userspace/synthetic-IQ checks; fresh ARM/live-duty qualification remains required.

## Tests and remaining gates

With the configured SDK/worker/templates, build and run
`test_scanner_glrt_provider`. This hardware-free fixture compiles the actual
provider and hop engine; only IIO/gain/RSSI/device operations are substituted.
It exercises both rates, real isolated numerical work, legacy byte comparison,
delayed events, exact-gap rebasing, capacity rejection before input consumption,
injected classification failure, final draining, and completion/cancellation
ordering. It passes with ASan/UBSan/leak detection. The daemon also builds with
the feature disabled and cross-compiles for ARMv7 with the feature enabled.

This fixture is not an original-arrival 300-second replay, a real DMA/IRQ/network
load test, or a combined real-provider/network/production-host test. Those gates,
production host integration, runtime artifact qualification, the unresolved
5 MS/s CPU/startup tails, classifier specificity, and authorized same-duty live
verification remain open. No deployment or new RF is implied by this document.
