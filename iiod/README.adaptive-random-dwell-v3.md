# Adaptive random-dwell protocol v3

`SCANCAPS3 96` returns the existing 96-byte capability record with wire version
3, rate mask bits for 2.5 and 10 MS/s, RX mask 3, and dwell bounds 120–360 ms.
V1 `SCANCAPS` and v2 `SCANCAPS2` retain their published behavior.

A v3 setup keeps the existing 352-byte layout. `dwell_ms` is the scan's active
base and must be 120, 240, or 360. Before an ACTIVE result has been applied for
a target, and after a QUIET result or activity decay, that target receives a
120 ms probe. An applied ACTIVE result gives subsequent selected visits for
that target the active base. Selection weights and revisit deadlines remain the
existing policy; dwell duration never multiplies the weight.

Visit records keep the existing 160-byte layout and carry wire version 3. The
authoritative duration is `valid_end - valid_start`; IQ reservation and byte
accounting use that exact interval. Feedback, acknowledgements, terminal
records, and counter observations remain v1. Firmware without `SCANCAPS3`
cannot admit a v3 setup, so clients must fail closed instead of falling back to
a fixed-duration protocol.
