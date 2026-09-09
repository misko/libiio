# Adaptive userspace hop protocol, major 2

Implementation checkpoint, 2026-09-09. Provider OPENM, userspace factory and host
stream/lifecycle integration are implemented and tested offline. The build
option defaults OFF; nothing is deployed on a radio. No kernel, FPGA or flashed
firmware change. Durable application publication/UI and qualification remain.

## Boundaries

HOPR/HOPS/HOPT major 1 remain fixed-order and their wire codecs are unchanged.
Major 2 reuses numeric geometry validation through private scratch buffers, not
by relabeling adaptive captures as V1. V1 and V2 decoders reject the other major.
New codecs leave caller output unchanged on failure.

All integers are little-endian. Geometry uses the existing eight profiles;
actual visits need not cycle through all eight. A visit index is NOT a sweep.
Required features are `0x3f` (the previous `0x1f` plus variable-visit evidence).

| Record | Layout |
| --- | --- |
| HOPR V2 | 352 bytes: 288 bytes of existing geometry layout, followed by 64-byte policy |
| HOPS V2 | Existing 64-byte header layout, followed by 0–8 events of 144 bytes; maximum 1,216 bytes |
| HOPT V2 | Existing 160-byte terminal geometry layout, with version 2 and features `0x3f` |

HOPR offsets 4/6/8 hold version/record length/features, and offset 76 holds the
144-byte event size. HOPS offsets 4/8/12 hold version/record length/features.
HOPT offsets 4/8 hold version/features. Reserved fields stay zero.

## Policy, relative to HOPR offset 288

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u64 | Nonzero policy generation |
| 8 | u32 | Mode: 1 shadow, 2 adaptive |
| 12 | u32 | Uniform startup visits per target |
| 16 | u32 | Consecutive evaluated misses for demotion |
| 20 / 24 | u32 / u32 | Active / quiet weights |
| 28 | u32 | Cooldown in milliseconds |
| 32 / 36 | u32 / u32 | Maximum revisit / planning hop budget in milliseconds |
| 40 / 44 | u32 / u32 | Maximum result age in milliseconds / unhealthy-result limit |
| 48–63 | bytes | Reserved, zero |

The complete request must be retained, not just the generation number. OPENM
requires the policy generation to match the GLRT request generation and the
explicit positive-only detector profile. Generation alone is not a
configuration digest or authentication.

`IIOD_SCANNER_ADAPTIVE_HOP=ON` additionally requires the SDK, `positive-only-v1`
and `spf-hop-device-userspace.c`. Configuration rejects the unchanged kernel
provider. Only that opt-in build advertises `iio,buffer-adaptive-hop-{request,
event,status}=2`, modes `shadow,adaptive` and policy
`three-miss-two-second-v1`. The provider pins settings 3/3/3/1, 2000 ms cooldown,
3000 ms revisit, 160 ms hop budget, 1000 ms feedback age and health limit 3.
These are engineering policy settings, not detector quality qualification.

## Choice, relative to each event offset 80

The first 80 bytes have the existing individual event geometry layout. Unlike a
V1 session, V2 validates `from_profile` against the preceding ACTUAL target.

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u64 | Device-counter time of the selection |
| 8 | u64 | Last applied source visit, or UINT64_MAX for none |
| 16 | u64 | Proposed target's cooldown remaining, in samples |
| 24 | u64 | Policy generation |
| 32 | u32 | Proposed target |
| 36 | u32 | Reason: 0 warmup, 1 weighted, 2 exploration, 3 none active, 4 fault fallback |
| 40 / 44 | u32 / u32 | Active / quiet target masks |
| 48 | u32 | Proposed target's consecutive evaluated misses |
| 52 | u32 | Mode |
| 56–63 | bytes | Reserved, zero |

The device event's `to_profile` is always the actual target. In adaptive mode it
must equal the proposal. In shadow mode it must equal visit modulo eight and
may differ from the proposal. Credits, visit ages and detector source bindings
are committed to the actual target, never to a hypothetical capture.

## Execution and feedback

The userspace scheduler obtains a fresh counter after the prior dwell deadline,
asks its bounded policy port, performs the existing recall, validates its device
receipt, and commits the real guarded valid interval before publishing the
event. Finite capture, conservative invalid spans and restore lifecycle remain
shared with the existing implementation.

`spf-hop-adaptive-policy` owns the native policy and a 32-entry SPSC observation
queue. Only the acquisition owner calls `offer`; only the hop thread chooses
and commits. At most eight queued observations are considered per hop. Atomic
indices must be lock-free on the target or creation fails before acquisition.
Overflow/invalid feedback latches equal scanning. Errors returned by the
scheduler callback represent integrity/programming failures, not detector misses.

IQ counters supply a full epoch while the local register may begin in a low-32
epoch. The feedback adapter binds a single nonnegative whole-2^32 epoch offset
to an already committed visit and requires that same offset thereafter. It
validates session, generation, rate, RX, actual target and original interval.
It does not independently modulo-map every result. Decisions are re-anchored
to the full IQ epoch during session validation.

The queue stores observations, not IQ. It introduces no worker calls, IIO
operation, filesystem access, allocation or wait in the hop-policy callback.
Both owners must be stopped before destroying the policy. The provider must
join the scheduler before destroying its referenced policy context.

## Tests and remaining release work

`test_spf_hop_adaptive` checks both rates/modes, finite lifecycle, actual order,
counter wrap, malformed records, policy mismatch and legacy rejection.
`test_spf_hop_adaptive_native` runs the same scheduler/session with the real
native policy and causally timed synthetic observations. The separate policy
test covers every activity mask, fixed-shadow accounting, queue overflow,
source/epoch mismatch, latched fallback and two-thread queue handoff.

The adaptive provider fixture now exercises actual OPENM, SDK collection,
numerical worker, feedback and policy at both rates/modes with paced synthetic
pilots. Hardware recall receipts are substituted there; the actual threaded
scheduler remains independently tested. Worker failure latches equal scanning
without losing the recording or result inventory. The userspace factory unit
also validates the complete request and saved-profile CRCs before scheduler
creation, and tests physical-restore intent with all IO mocked.

The host's actual TCP tests cover V2 decoding, cross-frame actual-visit IQ,
terminal result drain, full 300 s counter spans and cancellation/restoration.
TCP IQ is synthetic zero RX1/constant RX0 and counters are accelerated: this is
not full positive-RF network/load qualification, ARM runtime, scientific quality
or live duty. Application publication/UI and authorized live qualification are
still required before enabling a production radio.
