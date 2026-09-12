# SPFC1 physical single-receiver counter metadata

SPFC1 is an opt-in request over the existing ABI-3 generic metadata transport.
It does not change the SPFT tandem request, RadioMetadataV6, paired HOLD/AUTO,
or the exact-allocation direct-async contract. It supports ordinary and finite
direct-async RX without an additional RAM ring. Older providers reject SPFC1.

Discovery requires `iio,buffer-counter-metadata=1` and
`iio,buffer-counter-metadata-profile=ad9361:1r1t:rx0:manual:decimation1:spfc1`.
`iio,buffer-counter-metadata-topology-supported` distinguishes current physical
eligibility from implementation support. The kernel rechecks physical AD9361
1R1T, physical receiver 0, manual gain, sample rate, idle DMA and decimation 1
under configuration exclusion before timestamp or DMA setup. The receiver's
available sample-rate attribute is authoritative; RF bandwidth is independent.

All wire integers are little-endian. The 32-byte request is:

| Offset | Type | Value |
| --- | --- | --- |
| 0 | u32 | 0x43465053 (`SPFC`) |
| 4, 6 | u16, u16 | version 1, length 32 |
| 8 | u32 | required features exactly 7 |
| 12 | u32 | scan mask exactly 3 (I0/Q0) |
| 16 | u32 | requested/read-back sample rate in Hz |
| 20 | u32 | positive even complex samples per frame |
| 24, 28 | u32, u32 | zero |

The descriptor-owned kernel lease uses a distinct `ADI_RX_COUNTER_IOC_ACQUIRE`
on the existing exclusive `/dev/tandem-agc-events` endpoint. Sharing that
endpoint excludes paired ownership; the counter ioctl does not prepare paired
hardware, seed gains, arm pins, collect events, or heartbeat tandem. Closing the
descriptor restores the timestamp-control register and releases configuration
exclusion. PHY writes and ADC configuration writes are rejected while leased.
The provider unwinds the descriptor on rejected admission and normal cleanup.

The 80-byte frame is:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 | 0x31435053 (`SPC1`) |
| 4, 6 | u16, u16 | version 1, length 80 |
| 8 | u32 | features exactly 7: counter, canonical CI16, exact gaps |
| 12 | u32 | flags: sequence-valid bit 4, hardware-counter-valid bit 21; bits 11 and 23 iff missing samples > 0 |
| 16 | u64 | nonzero session stream ID |
| 24 | u64 | logical buffer sequence; advances by 1 + floor(gap / frame samples) |
| 32 | u64 | hardware sequence of the first returned complex sample |
| 40 | u64 | exact missing complex samples before this frame |
| 48 | u32 | complex samples per receiver |
| 52 | u32 | IQ payload bytes = 4 × sample count |
| 56 | u32 | scan mask 3 |
| 60 | u32 | attested sample rate in Hz |
| 64 | u32 | source counter correction: exactly 2 complex samples |
| 68, 72 | u32, u32 | zero |
| 76 | u32 | IEEE CRC32 of bytes 0..75 |

The DMA buffer has one eight-byte hardware timestamp followed by CI16 IQ.
`util_cpack2` presents two consecutive RX0 complex samples in each 64-bit word;
the concurrently captured counter has advanced twice from the first sample.
SPFC1 therefore subtracts two from the raw full-width counter, failing on
underflow. Its offset field makes this correction explicit. Existing paired
frames retain their original counter interpretation. Prefix removal returns
exactly I0,Q0,I0,Q0... with signed 16-bit little-endian components.

The exclusive end is first + sample count, checked for 64-bit overflow. No
claim is made about the interval preceding a new session. Within a session,
missing = first_current - end_previous; negative intervals are errors. When the
transport drops queued frames it rebases the missing count and CRC to the last
delivered frame. Flag bit 11 means a counter-observed missing interval, not an
independent ADC overload measurement. Gain, RSSI, tandem state and events are
absent and have no asserted validity bits.

Physical mode is distinct from the transfer mask: RX0-only transfer on a CMOS
2R2T PHY does not increase its 30.72 MS/s interface limit. Physical 1R1T permits
up to the driver's 61.44 MS/s limit; a 60 MS/s test needs an independently valid
analog RF bandwidth and does not imply sustained 240 MB/s Ethernet delivery.
