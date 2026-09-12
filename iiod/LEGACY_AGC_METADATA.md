# Metadata without tandem ownership

This extension is based on v0.49's libiio commit
`5cb2389719d46d12463daa0371d1fda19eb25fa7`. It keeps the ABI-3 transport and
metadata-v6 layout. Existing 104-byte tandem requests retain their behavior.

The iiOD context advertises `iio,buffer-metadata-legacy-agc=1`. Clients must
check that capability before sending the new 16-byte little-endian request:

| Offset | Type | Value |
| --- | --- | --- |
| 0 | 4 bytes | ASCII `SPFL` |
| 4 | u16 | version 1 |
| 6 | u16 | size 16 |
| 8 | u32 | gain-observation interval, at least 1024 samples |
| 12 | u16 | observation capacity, 1–64 |
| 14 | u16 | reserved, zero |

The request leaves the AD9361 gain modes and gains unchanged. It never opens
or acquires the tandem kernel device. Timestamp setup, gain/RSSI/temperature
sampling, exact gap reporting, and buffer cleanup remain active. The ordinary
metadata transport is supported; this request has no DDR-burst/ring trailer.

Legacy records have feature mask `0xEF7`: neither tandem ownership nor FPGA
gain events is claimed. Event count/capacity are zero. Tandem extension fields
are zero except temperature; `TANDEM_VALID` is clear. Unequal RX gains are valid.
Clients must preserve the distinction between interval-bounded gain
observations and exact tandem transition events.

Build with `build_spf_gain_mode_provider.sh`, passing a compatible Buildroot
SDK toolchain file, metadata source commit
`3294365ff44da26b261be4a2ccb241b7896d23ad`, kernel UAPI from
`7176508dd84bde78c62d8790bbd17957fdda12d7`, and an output directory.
`tests/test_spf_tandem_metadata.c` covers legacy request rejection, truthful
ownership/event flags, unequal gain observations, and the existing tandem
serialization. The SPF gain-mode hardware suite exercises the actual provider
lifecycle and host rollback together.
