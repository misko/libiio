# Adaptive scan setup-validated rates

`SCANCAPS 96` retains the exact version-1 capability record and rate mask
`0x1f`. Version-1 Setup and Visit records still accept only the five published
rates. No existing bit, record size, feature field, or reserved byte changes.

`SCANCAPS2 96` opts into version-2 capabilities. The record remains 96 bytes;
offsets 80, 84, and 88 contain little-endian uint32 rate mode (1 means
setup-validated), minimum requested rate (520833), and maximum requested rate
(61440000). The legacy mask remains informative for version-1 clients.

Version-2 Setup and Visit records have the same layouts and CRC coverage as
version 1, with header version 2. Only these two rate-bearing session records
change version. Feedback, acknowledgments, terminal records, and the independent
counter-UTC query/response contract retain version 1. The session echoes its
Setup version in every Visit, including skipped and cancelled visits.

The bounds permit requests; they do not assert that every integer is realizable
by the active AD9361 clock/FIR configuration. The host prepares the radio and
checks exact rate readback before preparing Fast Lock profiles. The daemon
independently verifies rate and bandwidth, and kernel counter acquisition again
checks the actual sample clock. Errors never coerce a rate or start a session at
a replacement rate. Host preparation restores its saved settings on failure.

Durations and maximum-age budgets use `floor(rate_hz * milliseconds / 1000)`
with uint64 arithmetic. Each interval is at most one sample shorter than its
requested duration; maximum budgets are never rounded upward. The session end
is computed once from its source-counter origin. Adaptive revisits start from
actual source counters rather than an accumulated wall-time estimate. Valid
counter endpoints determine payload bytes (4 per single-RX sample period,
8 per dual-RX sample period) and retained duration.

Transport capacity is a separate constraint. A radio may accept a clock whose
continuous payload exceeds the link. Existing bounded queue admission must
classify unavailable visits explicitly; accepting setup is not a delivery-rate
guarantee.

Offline validation includes legacy codec/CRC tests, new capability and Setup
boundary tests, and session tests across both receiver layouts at 5, 7.5, 8,
12.345679, and 61.44 MS/s with exact Visit bytes and terminal accounting. Mock
driver rejection verifies the requested clock is passed unchanged and acquisition
failure is propagated. Existing queue overload and restoration suites remain
part of the regression gate.
