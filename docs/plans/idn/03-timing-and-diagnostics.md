# IDN plan 03: Timing, rate boundaries, and truthful diagnostics

Status: partly implemented on branch `idn/03-timing-and-diagnostics` (host tests only; no ESP/RP build or hardware run). Numerical policies and hardware timing still require validation.

## Goal

Preserve the meaning of timestamped continuous samples through buffering and make protocol responses describe what actually happened. Distinguish successful packet reception, session acceptance, downstream queueing, hardware consumption, and estimated optical presentation.

Dependencies: [safe decoder](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/01-safe-decoding.md), [state/ownership](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/02-protocol-state.md), and [independent fixtures/sender](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/04-fixtures-and-sender.md).

The decoder returns typed failures; the session owns state and output tokens; this plan owns sequence/timeline evaluation, scheduling metadata, ACK event scope, and discovery snapshots. Implement these contracts together, without making separate incompatible copies of channel state.

## Specification anchors

Sources: [Stream](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/ILDA-IDN-Stream-rev002.pdf) and [Hello](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/IDN-Hello-2022-03-27-draft.pdf).

| Topic | Required behavior / qualification | Source |
|---|---|---|
| Message timestamps | 32-bit microseconds, monotonic with wrap; equal values allowed | Stream 2.1 |
| Wave continuity | Previous timestamp plus duration equals next wave timestamp | Stream 6.1 |
| RT send time | May arrive/send late; producer must not send ahead of its timestamp | Hello 6 |
| Sequence | Increment by one; server tracks first sequence; duplicate rejection SHOULD | Hello 6 |
| ACK result | Packet reception/connection result, not physical-render success | Hello 6.3.3 |
| Input/pipeline events | Correct scope and reset-after-report behavior | Hello 6.3.3 |
| CnlClsd | Static closed indication, not merely a one-shot close event | Hello 6.3.3, p. 30 |
| Status | Session messages, device buffers, open channels refer to this connection | Hello 6.3.3 |
| Latency / quality | Zero is unknown; do not fabricate measurements | Hello 6.3.3 |
| Discovery occupation | OCPD when no session capacity remains | Hello 4.1.2 |

Hello 6.1 says malformed packets SHALL be silently discarded, while 6.3.3 defines result 0xEE for invalid packet payload. Treat this as a specification interpretation gate, not permission to invent an unqualified rule. Define and document which failures prevent recognizing a valid request and which recognized ACK requests receive 0xEE. Check reference behavior before finalizing fixtures. In every interpretation, malformed input must not allocate a session or refresh its timeout before validation.

## Affected implementation

- [IDN receiver](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/idn_protocol.c): currently ignores timestamps/sequence state and changes hardware rate at receive time.
- [Stream counters](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/protocol_stream_stats.c): currently aggregate handed-off samples, not end-to-end playback.
- [V2 manager](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/rp_manager.c) and [interface](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/rp_manager.h): queue completion, token checking, and rate boundaries.
- [V1 output](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/rp_manager_v1.c): time accumulator, hysteresis, deliberate periodic yield gaps.
- [RP output](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/RP2354/src/output_engine.c), [DAC timing](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/RP2354/src/dac80508.c), [SPI receive](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/RP2354/src/stream_rx.c), and [UART status](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/RP2354/src/rp_uart.c): actual scheduling and downstream state.
- [Web status](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/web_server.c) and [stats tool](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tools/stream_stats.py): accurately labeled diagnostics, separate from IDN wire ACKs.

Proposed pure-C timing module: `C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/idn_timing.c` with matching header. Keep serial-number arithmetic, continuity, and deterministic rate calculations testable without real time or hardware.

## Sequence and timestamp tracking

### Sequence policy

Use unsigned modular arithmetic for the 16-bit RT sequence. The first accepted packet establishes a baseline. Track only RT traffic for this link; management requests must not advance the RT baseline.

Initial project policy: bounded duplicate/recent-sequence recognition with no general packet-reordering queue. Drop recognized duplicates; do not append them twice. Recognize forward gaps and older/reordered packets without interpreting ordinary wrap as a loss of 65,535 packets. Document the comparison window and ambiguous half-range handling.

Report the observed Type 1/2/3 SeqErr bits according to the draft. Avoid counting the same missing interval twice when a delayed older packet arrives. Define whether a rejected duplicate refreshes link liveness; choose one policy consistently with session acceptance, and test it.

### Timestamp policy

- Maintain session input chronology independently from per-channel wave continuity.
- Unwrap 32-bit microsecond timestamps into a wider internal timeline using a documented half-range/window assumption.
- Equal session timestamps are legal. For wave data, equality alone does not satisfy continuity if the prior duration is nonzero.
- Exclude sequel-fragment timestamp fields from clock calculations; rejecting unsupported fragments must not corrupt the baseline.
- Configuration-only/Void messages participate in session chronology, but do not advance the expected end of the last wave chunk on that channel.
- Reset timing state on the appropriate new connection/route generation, not on every repeated configuration.

Keep exact wire continuity distinct from output clock estimation. Quantized durations may alternate while forming an exactly continuous wire timeline. Do not relax the equality check to hide a sender bug.

Initial discontinuity policy: report CTYErr and end the affected continuous output epoch with blanked recovery; do not silently join nonadjacent lit points. Require a defined reconfiguration/recovery boundary before resuming. This is a project policy, not a claim that the standard mandates one particular recovery algorithm. Document how a subsequent repeated valid configuration can recover after packet loss without permanent lockout.

## Preserve metadata through the pipeline

Replace the anonymous sample-only IDN ring with bounded chunk/work descriptors referencing bounded sample storage. Each accepted unit retains:

- Connection/channel/route generation and output-owner token.
- Unwrapped timestamp and duration, sample count, and exact inferred rate ratio.
- Decoder/configuration identity, relevant control metadata, and completion boundary.
- A clear lifecycle: validated, session-queued, device-queued, consumed/discarded.

These are conceptual fields, not a required packed wire structure. Budget memory explicitly against ESP internal RAM/PSRAM. No per-sample allocation or lazy processing is needed; validate and enqueue eagerly in bounded blocks.

The current 256-sample SPI packets can span IDN chunks. An adapter must retain the location of chunk/rate/close boundaries rather than erase them. Partial-packet padding may be an intentional end-of-stream action, but must not be inserted between otherwise gapless chunks. Count padding separately from authored samples.

## Rate policy and boundary design

The scanner safety limit remains authoritative on both processors. Never raise it automatically to satisfy an incoming stream or a conformance test.

Required acceptance rules:

1. Reject zero/invalid duration and impossible sample-rate values before sample enqueue.
2. Do not accept an unsupported rate while silently playing at a previous rate.
3. Do not slow an above-limit stream and rely on overflow to discard excess artwork. Report range/unsupported-feature failure and apply defined blanked recovery.
4. Keep requested rational rate, selected realizable output rate, and configured limit distinct in diagnostics.
5. Replace fixed 100/500 pps hysteresis with an explicit clock/rate-estimation policy. Small valid changes cannot be ignored forever.

Duration quantization must not turn an intended 30,000 pps stream into constant rejection at a 30,000 pps limit: 64 points over 2,133 microseconds implies about 30,004.7 pps, while a neighboring 2,134-microsecond chunk compensates. Define a bounded estimation window/quantization envelope from sample count and timestamp resolution, keep the actual hardware rate at or below the safety limit, and reject sustained above-limit demand. Record rounding uncertainty separately from permission to exceed the limit. Test both alternating quantization and genuine 30,100/50,000 pps demand.

### Output-rate boundary gate

Changing a UART RATE immediately on receipt retimes all older queued samples. Do not retain that behavior.

Preferred target: chunk-aware scheduling carries a rate/epoch boundary to the output consumer, applied only after the previous epoch's samples finish. This may require a versioned ESP-to-RP control/packet extension and equivalent V1 scheduling.

Before implementing a transport extension, specify:

- Version/capability negotiation; never reinterpret old reserved bytes silently.
- Exact sample index at which a new rate becomes active.
- Ordering with SPI data, UART control, STOP, and CLEAR.
- Queue accounting, acknowledgments, rollover, and fault behavior.
- Old-RP compatibility: fail closed or operate a documented fixed-rate subset, not immediate unsequenced rate changes.

Bounded first-release fallback: permit one stable rate per continuous epoch and reject genuine in-stream rate changes until a safe scheduled implementation exists. Quantization compensation is not a genuine rate change. This fallback is an explicit feature limitation. Never approximate a seamless rate boundary by draining everything, pausing unpredictably, and pretending continuity was preserved.

### Output clock validation

Measure real sample intervals and sustained consumption, not just configured rate fields. Include RP PIO divider quantization, DMA handoff, low-rate scheduling, and V1 periodic yield/blanking gaps. Determine whether V1 can meet the supported rate envelope without its current injected gaps; otherwise narrow and report its supported capability.

Use monotonic clocks. Startup prebuffer and safe preroll add latency; preserve them where needed and include them in estimates. Define a bounded jitter buffer target and overflow policy. Prefer recovery that bounds stale playback over retaining arbitrarily old data while dropping every new chunk; do not manipulate a live SPSC consumer index unsafely to implement dropping.

## ACK contract

Result code and event flags are different layers. A syntactically accepted message may legitimately return result 0x00 with configuration or routing errors in pipeline flags. Do not redefine 0x00 as successful laser output.

### Result codes

| Code | Meaning/action |
|---|---|
| 0x00 | Valid accepted request/message handed to the session, as applicable |
| 0xEB | Empty close without established connection |
| 0xEC | No session available for new connection |
| 0xED | Client group excluded |
| 0xEE | Invalid payload where the agreed framing/ACK interpretation permits response |
| 0xEF | Other processing failure at the appropriate transport/session boundary |

### Event recording

Input events are connection/session scoped: New, sequence flags, ordering, late/before-process arrival, overrun, input validation, closed-channel data, and resource errors.

Pipeline events are channel scoped: routing, static closed state, busy/invalid service, configuration errors, SCM mismatch, continuity, chunk type, minimum length, range/unsupported features, payload decoding, fragments, device errors, and internal processing errors.

Maintain latched events since the last report, not only failures of the current packet. Snapshot atomically with concurrent producers. On send success, clear only the reported event generation; a new occurrence during send must remain latched. Do not globally clear another channel's events. Recompute CnlClsd from state because it is static. Empty RT ACK requests have no referenced channel: reset/unknown channel-specific fields as the draft specifies.

Send ACKs promptly after bounded acceptance/snapshot work. Do not block ACK generation on a UART STATUS round trip, a prebuffer fill, or motor parking. Asynchronous pipeline state may lag; report it honestly.

### Status and estimates

- SOCnl: actual open-channel state associated with this connection.
- SNMsg: retained session input work for this connection, not arbitrary global samples.
- DOBuf: retained downstream data; include ESP payload queues, in-flight work, RP buffers, and relevant delayed output state until an immutability/completion point is known.
- Latency: zero when unknown. When implemented, estimate this channel's first payload presentation from timestamps, queued work, current rates, preroll, and output delay. Label internal versus measured estimates.
- Link quality: start unknown. Add a bounded estimator only with documented input metrics and tests; no fabricated excellent value.

An enqueued SPI packet is not proof of hardware consumption. Add generation-scoped consumed/discarded accounting or completion markers as needed for truthful DOBuf. A stale periodic status sample must not report drained when a new packet is still in flight.

## Discovery and application diagnostics

Build scan status from a consistent state snapshot: RT support, occupied session capacity, exclusion for the querying group, and offline/malfunction only when supported by known device state. Distinguish a busy service from all sessions being allocated. Keep protocol version 0.1 for this draft.

Preserve service-map sizes, ID 1, laser type 0x80, and DSID for the default route. Do not invent per-mode capability bits in the fixed discovery structure. Document unsupported modes separately.

Add useful counters to the existing status API without confusing them with ACK flags: received datagrams, malformed payloads, accepted samples, session/device queued samples, rendered/consumed samples where measurable, discarded samples by reason, sequence loss/duplicates, continuity failures, decoder errors, and cancellation faults. Existing `stream_idn_samples` semantics must be documented or versioned if changed; do not silently rename queue acceptance as rendering.

## Implementation increments

1. Pure sequence/timestamp/continuity helpers with literal wrap and loss fixtures.
2. Typed diagnostic events and scoped snapshot/report lifecycle.
3. Chunk metadata and owner-generation propagation through IDN buffering.
4. Stable-rate/quantization/limit policy; then scheduled rate boundary support if the transport gate is satisfied.
5. ACK serialization, scan state, and clearly labeled application counters.
6. Measured clock/jitter/overflow validation, including V1 limitations and RP completion accuracy.

## Acceptance matrix

| Scenario | Expected result |
|---|---|
| Sequence 65535 then 0 | No false sequence loss |
| Duplicate last accepted packet | No duplicate output; duplicate event reported |
| Gap then delayed old packet | No giant modulo missing count or replayed old artwork |
| Timestamp `0xFFFFFFF0` plus duration 32, next `0x10` | Continuous across wrap |
| Equal timestamps on legal Void messages | Accepted chronology |
| Wrong next wave timestamp | CTYErr and defined recovery, no silent lit splice |
| Repeated identical configuration | No false route restart or clock reset |
| Alternating quantized 30k durations at 30k safety limit | Stable bounded clock; no sustained above-limit output |
| Genuine unsupported/above-limit rate | No enqueue at old/slowed rate; correct diagnostics |
| Queued old-rate samples followed by rate change | Old samples retain old timing, or change is explicitly rejected |
| ACK after multiple events | All scoped latched events present, no loss during snapshot |
| Empty ACK on live connection | Connection status correct; no invented channel fields |
| Drained session with device data still pending | SNMsg may clear; DOBuf remains set |
| New data races a status snapshot | No premature drained/available claim |
| Scan while only session is held | OCPD set |

Hardware tests must correlate packet capture, ESP/RP counters, and scope/logic-analyzer sample timing. Perform rate-envelope tests on a disconnected optical/scanner load where appropriate; never infer a scanner can tolerate 100 kpps because the protocol requires device capability.

## Done criteria

- [x] Every accepted wave chunk has checked chronology and per-channel continuity. (ESP receiver; host-checked only.)
- [x] Sequence and timestamp wraps do not create false loss or stale replay.
- [ ] Queued samples retain timing/owner boundaries; unsupported transitions are rejected explicitly. (Rejection done; chunk descriptors/owner tokens not done.)
- [x] Rate quantization is handled without weakening hardware safety limits.
- [ ] ACK result/events/status match their defined scopes and survive concurrent updates. (Done except 0xED/XCLD, which need plan 02's group mask, and DOBuf completion accounting.)
- [x] Unknown latency/quality stay unknown until evidence supports estimates.
- [x] Discovery and application counters no longer claim unavailable knowledge.
- [ ] All-target builds, host fixtures, and measured timing tests are recorded. (Host tests only.)

## Implementation record (branch `idn/03-timing-and-diagnostics`)

Increments 1, 2, 4 (fallback only) and 5 are implemented; 3 and 6 are not.

- Pure module `firmware/ESP32S3/main/idn_timing.c/.h`, tested by `tests/idn/native/timing/` (CMake/CTest, host gcc, `-Wall -Wextra -Werror`). Its structs are small and meant to be embedded in plan 02's session/channel state.
- Integrated into `idn_protocol.c` with the current (pre-plan-02) session model. `web_server.c` reports `idn_*` counters.

Decisions taken (project policy, not normative):

| Topic | Decision |
|---|---|
| Malformed vs 0xEE | Framing failures (short header, declared size, truncated config header) never open/refresh a session. 0x40/0x44: silent drop. 0x41/0x45: 0xEE with all other fields zero. Not yet checked against a reference implementation. |
| Sequence | 32-entry window, serial arithmetic. Forward 1..0x7FFF accepted; 0x8000 is treated as older. Duplicates, late (unseen, in window) and stale packets are dropped, not reordered, and do not refresh the link. Type 1 on every irregularity; type 2 duplicates; type 3 when unseen numbers leave the window, so a late packet is never also counted lost. Four consecutive stale numbers re-baseline. ACK result: 0x00 for a duplicate (first copy was passed on), 0xEF for late/stale. |
| Timestamps | Unwrapped by signed 32-bit difference (forward up to 2^31 - 1 us). Earlier: dropped with BPErr. Eight consecutive earlier, non-decreasing timestamps re-base the timeline and end all wave epochs. Fragments (0xC0+) are excluded. A new source port from the session's host restarts sequence/timestamp timelines and reports New. |
| Continuity | Exact equality. On a gap/overlap: CTYErr, drop, 16 blanked samples at the last point, then drop wave data until a routing configuration (an identical repeat suffices). An auto-opened channel (legacy, no config) is closed instead and re-opens on the next chunk. A new epoch after earlier output starts with 16 blanked samples at its first point. Recovery samples are counted separately. |
| Rate | Envelope of +/-1 us per chunk (covers cumulative, per-chunk-rounding and truncating senders; window halved past ~16.8 s). Reject zero duration, < 1 kHz, epoch lower bound above `min(ESP limit, RP limit)`, or a chunk inconsistent with the epoch or the locked rate (RGUErr). At a 30 k limit, 64-sample chunks of 2133/2134 us (including the in-repo sender's constant 2133 us) are accepted and played at 30,000; 30,050, 30,100 and 50,000 pps are rejected. The cost: an accepted stream may be slowed by up to its own quantization (1 us per chunk), and smaller genuine changes are not detectable. No RATE at receive time: the feed applies the rate once before START, locked for the session. |
| ACK | Input latch per session, pipeline latch per channel, cleared after a successful send only, generation-checked. CnlClsd from state; CnlRted on first route or mode change. Empty RT ACK: pipeline 0. SNMsg = ESP ring non-empty; DOBuf set from the first delivered packet until the feed stops (conservative, never "drained" early). Latency and link quality 0. |
| Scan | OCPD while the single session is held; MALFN/Offline/XCLD never set. |

Not done: ESP-to-RP rate-boundary transport, chunk descriptors and owner tokens, measured DOBuf/latency, link quality estimator, V1 yield-gap analysis, overflow policy (ring-full drops still drop the newest samples, now with OVErr), and every hardware/scope measurement.
