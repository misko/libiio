# Cooperative GLRT admission: explicit opt-in

`IIOD_SCANNER_GLRT_COOPERATIVE_SKIPS` defaults to **OFF**. Enabling adaptive
hopping or capture protection does not enable it. It requires an explicitly
linked SDK, `IIOD_SCANNER_GLRT_CAPTURE_PROTECTION=ON`, and
`IIOD_SCANNER_GLRT_MODE=positive-only-v1` with the normal pinned thresholds.

The matching SDK must export `leo_scanner_glrt_enable_cooperative_skips`.
The provider calls it once during startup, before accepting IQ. Failure refuses
the open. Bundle publishers must give this policy a new configuration identity;
do not relabel an existing qualified bundle.

Intentional pressure/backlog admission skips supply healthy UNKNOWN feedback
to the adaptive policy. They are not positive detections or negative evidence,
and they do not renew activity/cooldown timestamps. Actual input, worker and
watchdog failures still fault and cannot be cleared by a later deliberate skip.
No request/result/hop wire layout changes. IQ still bypasses the numerical
worker; the provider classifies RX1 only while retaining both receivers.

## Bounded offline tests

- `python -m pytest tests/test_scanner_glrt_build_config.py`: default-off and
  prerequisite checks without radio access.
- Build `test_scanner_glrt_provider`, `test_spf_hop_adaptive_native`, and
  `test_spf_hop_adaptive_policy` with the actual SDK and isolated numerical
  worker. Supply explicit synthetic 120 ms CI16 pilot files via
  `SPF_ADAPTIVE_PILOT_{2500000,5000000}_{LOWER,UPPER}`.
- Test separate cooperative-OFF and cooperative-ON builds. Each provider run
  covers both rates, fixed/shadow/adaptive capture, cancellation, delayed
  events, exact gaps and terminal drain. The adaptive fixture additionally
  covers pressure, recovery, and a genuine failure after pressure.
- Pressure spans frames 60 through 95 (720 ms of synthetic device time).
  Cooperative runs require zero fault fallback before genuine failure,
  weighted choices, at least three skipped checks and subsequent numerical
  recovery. Legacy runs instead require latched round-robin fault fallback;
  they must not require weighted choices after that fault.
- Instrument provider, policy and SDK with ASan/UBSan. The separately spawned
  numerical worker remains unsanitized in this integration configuration;
  its own component tests cover it separately.

These are synthetic-IQ desktop tests. They do not prove ARM compute latency,
RF sensitivity, live duty, fair admission on saved RF, or deployment readiness.
The fixture's RX0 sentinel must remain byte-identical across every frame.

## Fair admission is a separate opt-in

`IIOD_SCANNER_GLRT_FAIR_ADMISSION` also defaults to OFF. It requires all the
cooperative prerequisites and cooperative skips itself. Its engineering
profile uses three occupied slots, 450 ms admission age, 500 ms watchdog,
four recovery blocks, a 120 ms pending-age bound and 2500 ms freshness trigger.
All values and this new policy must be bound to a new bundle identity. The
matching SDK and numerical worker use the private LP03 pool ABI; an older
worker is not interchangeable even though public wire layouts are unchanged.

The provider fixture checks enabled admission, at most one pending/running
request, complete terminal drain and pressure/fault behavior. To test real
overload, link a test-only dwell-delay wrapper into the numerical worker and
set `SPF_EXPECT_FAIR_SHEDDING=1`; ordinary no-pressure cases must then perform
some checks and intentionally shed others. This is synthetic pacing, not a
measurement of ARM runtime or live capture duty.
