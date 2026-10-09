# Continuous ordered scan protocol v5

This additive device mode recalls profiles 0 through 7 in order, then repeats
the same order until an explicit STOP or a visible acquisition/transport fault.
It does not restart finite campaigns, filter weighted visits, or accept host
commands for each target. Protocols v1 through v4 and SCANDIAG remain available.

## Geometry and counter guard

`SCANCAPS5` advertises the fixed v5 mode. `OPENM` receives the existing 352-byte
setup with version 5, duration zero, eight targets, profiles 0 through 7,
baseline weight one and maximum boost one. The only supported source rate is
2,500,000 samples/second and every complete visit has exactly 50,000 sample
times (20 ms). Select RX1 or paired RX1+RX2. Paired CI16 payloads contain
`[RX1 I, RX1 Q, RX2 I, RX2 Q]` for each sample time.

The transition budget is a positive guard in the reported device counter
domain. For each v5 recall the admitted start obeys:

```
valid_start = max(selection_counter, recall.counter_after) + guard_ticks
```

At the tested 20 ms guard, `guard_ticks` is 50,000. The full guard applies even
when the recall receipt contains equal before/after counters. Integer overflow
fails the session. Older modes retain their existing transition semantics.
This reported-counter invariant does not itself establish physical settling
time; the counter snapshot CDC and RF transition behavior need separate
qualification. The IQ window follows the guard rather than containing it.

## Visits and bounded state

`READSCAN` returns the existing 160-byte visit layout with version 5. The
64-bit visit sequence is global to the session, and target is `visit % 8`.
Complete records describe exactly 50,000 sample times. Fault/cancel records
retain the actual contiguous acquired prefix, with the exclusive end and IQ
byte count describing that prefix. Missing IQ is never padded or marked
complete. Kernel ownership, DMA capture and session generation remain
persistent across sweeps; host archive segment rotation requires no device
restart. Device policy history and pending visit state use bounded rings.

## Independent STATUS and STOP

`SCANSTATUS` and `SCANSTOP` accept a 48-byte binary control query on an
independent connection and return a 128-byte CRC-protected status receipt.
The query carries version 5, request identity, session and generation; stale
identities are rejected. Status exposes planned/delivered visits, active
counter bounds, pending visits, state, terminal error and restoration flags.

A graceful STOP finishes the admitted window and drains its real IQ before
the terminal record. A forced STOP interrupts capture and retains a genuine
partial prefix when available. Repeated v5 STOP requests are idempotent.
Queue pressure, DMA gaps, recall failure and transport failure terminate
visibly. The existing 128-byte terminal record retains its version-1 framing;
v5 graceful completion uses reason 2. A control acknowledgement is distinct
from receiving and accounting the drained terminal record.

Kernel ownership restoration and the public host preparation/restoration
receipt are separate responsibilities. Do not infer that the original host
LO/gain/layout/buffer settings were restored from a control acknowledgement.
SCANDIAG retains the v0.61 first-error and restoration-error evidence, including
absolute visit identity after continuous history slots have been reused.

## Validation and scope

The owned suites cover strict capability/setup/visit/control framing, CRC and
reserved fields; legacy modes; 20,000 ordered visits and low-counter wrap;
bounded queue leases; partial cancellation, gaps, pressure, final drain and
the minimum reported-counter guard. An integration regression crosses history
reuse before recall/restoration failures and checks that a later producer
failure cannot replace the first diagnostic error.

Release qualification must use the exact installed ARM daemon and native
library, preserve the selected kernel/FPGA image, and retain START/STATUS/STOP,
exact IQ support and restoration evidence. Bounded hardware passes do not
claim indefinite reliability, physical RF settling qualification or support
for higher rates. GLRT analysis remains a separate offline consumer of the
recorded 20 ms IQ windows. FPGA power acceleration is not part of this mode.
