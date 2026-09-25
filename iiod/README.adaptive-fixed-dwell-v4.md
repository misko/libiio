# Adaptive scan fixed-dwell protocol v4

`SCANCAPS4 96` returns the existing 96-byte capability record with wire
version 4. It advertises only dual-RX CI16 at 2.5 MS/s and a discrete dwell set
of 120, 240, or 360 ms. The minimum and maximum fields are 120 and 360; values
between them are not implicitly admitted.

A v4 setup keeps the existing 352-byte layout. It must request RX1+RX2,
2,500,000 sample periods per second, and exactly one of the three advertised
dwells. The daemon validates the complete setup and queue capacity before
acquiring the radio. Unsupported values fail explicitly and are never coerced.

The selected dwell is copied into immutable session state. Every visit in that
session has exactly `floor(source_rate_hz * dwell_ms / 1000)` sample periods,
regardless of UNKNOWN, ACTIVE, QUIET, expired feedback, weights, or repeated
selection of the same target. Feedback changes target scheduling only.
Transition intervals and an incomplete terminal tail remain outside delivered
IQ. Congestion skips a whole visit; a partial visit is never presented as
complete.

The setup and visit records carry version 4. Feedback, acknowledgements,
terminal records, and counter observations retain version 1. Firmware without
`SCANCAPS4` remains usable through the unchanged v1/v2 contracts; a host that
explicitly requests v4 must fail if v4 is unavailable and must not fall back to
the experimental activity-dependent v3 contract.
