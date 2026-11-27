# Continuous 24-target RX scanner, protocol 6

Protocol 6 is additive. `SCANCAPS5`, `SCANCAPS5M`, the 352-byte legacy
setups, 160-byte visits and existing finite modes retain their wire bytes.
Negotiate `SCANCAPS6 96` before changing radio settings. The command probes
the additive kernel profile-bank capability and rejects older kernels.

The first mode uses 10 MS/s, 8 MHz bandwidth, simultaneous RX1/RX2, 20 ms
valid windows and at least 20 ms guard after confirmed recall completion.
There are 24 logical targets and eight physical Fast Lock slots. Before
streaming, the host calibrates each target and saves its complete 16-byte
profile plus reflected CRC-32. It uploads these images with the setup.

All integers are little endian. Headers retain the established magic,
feature mask `0xff`, length and CRC conventions; their version is 6.

| Record | Bytes | Layout |
| --- | ---: | --- |
| Capabilities | 96 | Existing fields; targets=24, resident slots=8, rate mask=1, dwell=20, duration=0 |
| Setup | 1120 | Existing 144-byte prefix; 24 target rows of 40 bytes; reserved bytes 1104–1115; CRC at 1116 |
| Target row | 40 | Logical label u32, physical slot u32, requested IF u64, weight=1 u32, original CRC u32, image[16] |
| Visit | 224 | Existing fields through flags at 136; extension below; CRC at 220 |
| Control | 48 | Existing identity and flags; version 6 |
| Status | 128 | Existing fields; sweep divides logical visits by 24; target may be `UINT32_MAX` before acquisition |
| Terminal | 128 | Existing fields and accounting; version 6 |

The visit extension is `load_after` u64 at 140, `recall_before` u64 at 148,
sweep u64 at 156, slot generation u64 at 164, verified IF u64 at 172,
original profile CRC u32 at 180, RX mask u32 at 184, and plan digest[32] at
188. The existing CRC at 132 is the post-recall image CRC, which can differ
because of the legitimate AD9361 ALC adjustment. Requested IF remains at 80.
Verified IF allows the kernel's two-Hz representation tolerance.

Use `OPENM` with a 1120-byte request and the existing `READSCAN` stream.
Control verbs are `SCANSTATUS6 <device> 48` and `SCANSTOP6 <device> 48`.
Their request binds the session and generation and cannot control a v5
session. Each visit carries that identity and its immutable plan digest.

The kernel owns an immutable copy of all 24 profiles and preloads targets
0–7 at slot generation 1. Visit `n` selects target `n % 24`, sweep `n / 24`
and slot `target % 8`. The radio adapter sends the expected generation and
requested IF with every checked replacement/recall. It verifies the returned
logical identity, original CRC, selected IF, generation and ordered source
counters before publishing the mapping. Loading, readback, recall, PLL lock,
STOP and release use the existing owner serialization.

The acquisition ledger copies each receipt before queueing its IQ. Reading
an old window after slot reuse therefore retains its original slot generation
and frequency identity. Failed transitions return explicit cancelled visits;
queue pressure fails the session with an accounted whole-visit outcome.
Cancellation can retain partial IQ and its exact counter support. Restoration
and terminal accounting follow the established continuous-session lifecycle.

`test_spf_position_scan` exercises 10,000 ordered visits, multiple counter
wraps, 25 delayed readers, image CRC validation, stale identity/generation,
partial-load and lock errors, cancellation during guard and dwell, queue
pressure and restoration. Run alongside the seven existing scan/queue/time
suites in native and ASan/UBSan builds. An optional directory argument emits
C codec fixtures for independent host decoding; those fixtures are simulated
protocol evidence, not hardware or RF qualification.
