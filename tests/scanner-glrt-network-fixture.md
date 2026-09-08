# Scanner GLRT loopback integration fixture

`test_scanner_glrt_network_server` is a hardware-free test executable, never a
deployable radio daemon. Build it with `WITH_TESTS`, `WITH_XML_BACKEND`, the real
SPF metadata provider, and the explicitly configured scanner GLRT SDK/worker
and templates. It reuses the existing provider fixture's hardware substitutions.

The executable takes `RATE DELAY MODE`, where RATE is 2500000 or 5000000,
DELAY is 0..2 initial refills before delivering hardware events, and MODE is
`normal`, `worker-failure` (injected SDK failure), or `missing-final`.
It binds only 127.0.0.1 on an ephemeral port, prints the port, and serves the
actual iiOD lexer/parser, OPENM/READBUFM/status/cancel/drain/CLOSE operations.
The original SPF provider, hop state machine, GLRT SDK and child worker run.
Only hardware-facing buffer, gain, RSSI, register and hop-device operations
are substituted. A 90-second watchdog and 20,000-refill bound limit the fixture.

Each refill contains synthetic dual-CI16 with RX0=(30000,-20000), RX1=(0,0),
and source counters above 2^53. The sample clock is accelerated, not paced like
DMA. Hardware events have a synthetic seven-sample transition plus the
requested guard. They are not real radio timings. Stream generation 100 and
test-only algorithm/configuration identifiers permit repeatable byte comparisons.
No context naming a local, USB, or network radio is opened.

On SIGTERM the server shuts down its clients, checks buffer creation/destruction
balance, and prints JSON counts. Every actual metadata-only drain is checked
not to cause another refill. The Python integration tests additionally kill
the fixture's own discovered worker to exercise real process-death handling.

The matching Leo test lane is `tests/radio/test_scanner_glrt_network.py` in
the `codex/arm-single-rx-presence` worktree. It requires:

- `SCANNER_GLRT_NETWORK_SERVER`: this executable's absolute path;
- `LD_LIBRARY_PATH`: the matching libiio build;
- `PYTHONPATH`: the matching binding, Leo source/tests, and PPU source;
- optionally `SCANNER_GLRT_SERVER_LIBRARY_PATH`: a separately instrumented
  server libiio build, while the Python client uses the ordinary library.

The lane is explicitly marked `libiio_integration` and fails if its declared
native dependencies are missing. It exercises public `metadata_status_raw`
and `drain_metadata` APIs; it does not access Python binding-private pointers.

Short tests check all edges, delayed events, unchanged IQ and inner legacy
bytes, cancellation, actual worker death and missing FINAL. Full 300-second
**counter-span** tests use the concrete PPU backend and public radio-factory
port, substituting only radio-control/priming operations. They stream/discard
IQ incrementally, rather than retaining gigabytes in the test process.

Passing this fixture does not qualify original archived arrival replay, RF
tuning/restoration, ARM runtime, detector sensitivity/specificity, direct-async
transport modes, or unchanged live duty. The current mode remains unqualified
evidence, not a deployed positive/negative classifier.
