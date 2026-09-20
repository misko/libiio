# Adaptive counter observations (issue 107)

This is an additive, opt-in control protocol. Existing SCANCAPS, OPENM,
READSCAN, feedback, visit and terminal records are unchanged.

`SCANTIMECAPS\n` returns `1\n` for this protocol. Older daemons may reject the
command; clients must preserve IQ and report unavailable timing. Capability
means the command is understood, not that a particular radio has a UTC source.

`SCANTIME <device> 48\n` followed by the query below returns `128\n` and a
response, or a negative errno line. Send it on a separate connection while
READSCAN is active. `EAGAIN` means not rebased to the first DMA epoch yet or
scheduler contention; `ESTALE` means session/generation mismatch; `ESHUTDOWN`
means the capture has finished. All control references use the existing
provider-lifetime lock. Snapshot failures do not fault or cancel acquisition.

All integers are little endian. Both records start with magic, u16 version=1,
u16 byte count, u32 features=0xff, u32 flags=1. The final u32 is standard CRC32
over every preceding byte. Unused fields must be zero. These are new record
types, not reinterpretations of published v1 scan records.

| Offset | Query, 48 bytes | Response, 128 bytes |
|---:|---|---|
| 0 | magic `SPTQ` | magic `SPTA` |
| 16 | u64 nonzero request ID | echoed request ID |
| 24 | u64 nonzero session | echoed session |
| 32 | u64 nonzero generation | echoed generation |
| 40 | u32 reserved | 16 raw boot UUID bytes, textual byte order |
| 44 | CRC32 | continuation of boot UUID |
| 56 | — | u64 provider clock epoch (creation monotonic ns) |
| 64 | — | u64 counter in the IQ metadata epoch |
| 72 | — | u64 device CLOCK_MONOTONIC before snapshot ioctl |
| 80 | — | u64 device CLOCK_MONOTONIC after snapshot ioctl |
| 88 | — | u32 sample rate Hz |
| 92 | — | u32 extended counter width, 64 |
| 96 | — | u64 maximum snapshot age ns; UINT64_MAX = unknown |
| 104 | — | 20 zero reserved bytes |
| 124 | — | CRC32 |

The read is coherent and anchored to a full DMA timestamp after the first DMA
block. A delta of half the low-word range or more is rejected. The query does
not update scheduler counter state. The provider lock prevents competing
reads, rate/session changes or teardown during the observation.

The current kernel scan-snapshot ABI does **not** attest snapshot age. Therefore
this implementation returns UINT64_MAX, not a guessed FPGA latency. A host may
use independently validated bounds for the exact radio boot, retaining their
reference in its evidence. No firmware UTC qualification is advertised.
Device monotonic timestamps do not synchronize device and host clocks.

Host consumers must include full request/response bracketing, coherent-register
age, host UTC synchronization error, sample-clock rate error and IQ acquisition
delay. Do not infer symmetric latency, average away systematic delay, or label
realtime/monotonic stability as UTC accuracy. Across reboot or clock epoch
changes, discard the mapping and require fresh qualification.

Tests: `test_spf_scan_protocol`, `test_spf_scan_time` (actual provider with only
the owner ioctl substituted), `test_spf_scan_radio`, `test_spf_scan_session`,
and `test_metadata_drain_server`. `test_spf_scan_protocol --time-golden FILE`
exports a C-encoded response for cross-language decoding.
