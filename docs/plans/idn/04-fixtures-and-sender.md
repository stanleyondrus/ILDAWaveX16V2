# IDN plan 04: Independent fixtures and a compliant continuous sender

Status: in progress on branch `idn/04-fixtures-and-sender`. Increments 1, 3, 4 and 5 are implemented and pass host tests; increment 6 is code-only (hardware helpers corrected, nothing run on hardware); increment 2 continues alongside plans 01–03. See [Implementation status](#implementation-status).

## Purpose

Build an independent protocol oracle and correct the Python continuous-stream sender. Our current sender omits routing/configuration and the receiver guesses its layout; a successful round trip between them therefore does not demonstrate interoperability.

This plan begins before firmware changes in [plan 01](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/01-safe-decoding.md), evolves with [state/ownership](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/02-protocol-state.md), and verifies [timing/diagnostics](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/03-timing-and-diagnostics.md). Do not postpone all testing until the other plans are complete.

The sender target is a well-defined continuous-graphics producer using standard tags, not a full authoring application or a new implementation of discrete/effects/audio modes. The firmware remains a partial laser-projector service until the deferred modes are addressed.

## Source hierarchy and test independence

Primary references:

- [Stream rev002](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/ILDA-IDN-Stream-rev002.pdf), especially 2.1, 2.2, 3.4, 6.1, and 6.7.
- [Hello draft](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/IDN-Hello-2022-03-27-draft.pdf), especially 2.1, 6, and 7.
- [Existing hardware-test procedure](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/hardware-testing.md).
- [Protocol research](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/research/protocols.md) as supporting context, not a replacement for the specifications.

The PDFs are presently untracked local inputs. Decide whether distribution rights permit adding them to the repository; otherwise retain document identifiers, page references, and official source locations in fixture provenance. Do not make automated tests depend on a live PDF download or parsing a PDF at runtime.

Rules for independence:

1. Author fixture bytes and expected interpretation from the specification, not by calling the production sender.
2. Store literal expected outputs. Tests should not reproduce the decoder algorithm to derive expected results.
3. Sender tests compare to fixed byte fixtures; receiver tests decode those same externally specified fixtures. A sender-to-receiver round trip is supplemental, never the sole oracle.
4. Keep malformed cases as deliberate single-feature variations of annotated valid packets.
5. Record the source section, byte offsets, and intended outcome for each fixture.
6. Where the draft is ambiguous, label the project's interpretation and resolve it before asserting a behavior as normative. In particular, use plan 03's malformed-packet/0xEE decision gate.

Use Python's standard library (`unittest`, `unittest.mock`, `struct`, `json`) and a small native C harness. No new third-party test framework is required. Mock network/time/output boundaries in unit tests; hardware tests remain opt-in and separate.

## Proposed file organization

- Existing sender: [C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tools/idn_sender.py](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tools/idn_sender.py).
- Existing point/file utilities: [C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tools/ilw_sender.py](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tools/ilw_sender.py). Reuse confirmed point types; do not duplicate ILDA parsing just for IDN.
- Existing live tests: [C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tools/hwtest.py](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tools/hwtest.py).
- Proposed literal fixtures and provenance: `C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tests/idn/fixtures/`.
- Proposed Python sender tests: `C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tests/idn/test_sender.py`.
- Proposed native harness: `C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tests/idn/native/` with a minimal CMake target compiling the actual pure-C decoder/session/timing modules.
- Proposed test runner: `C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tools/test_idn.py`, reporting host checks separately from firmware builds and hardware runs.

These are proposed paths, not existing test commands. Keep the harness small: adapters for time, output requests, and events are justified; reimplementing an ESP runtime is not. Build artifacts belong outside source/fixtures. Native sanitizers are optional when supported by the available compiler; unavailability must be reported rather than silently presented as a sanitizer pass.

## Fixture families

### Valid packets and descriptors

- Hello ping/scan/service-map headers with fixed expected response structure sizes and sequence/group copying.
- Empty RT channel request; routing-only Void message; initial route/config/wave message; repeated configuration; channel close; connection close; abort.
- Standard XYRGB layout, explicitly declared eight-byte XYRGBI layout, reordered BGR, green-only, precision, NOP/Void padding, and scales.
- All channel IDs through boundary cases 0 and 63; service ID 0 default and explicit ID 1.
- SDM/SCM combinations 0 through 3.

Example dictionary for the existing eight-byte sample representation:

| Sample bytes | Descriptor tags |
|---|---|
| X high, X low | 0x4200, 0x4010 |
| Y high, Y low | 0x4210, 0x4010 |
| R, G, B, intensity | 0x527E, 0x5214, 0x51CC, 0x5C10 |

Eight tags consume 16 configuration bytes, so SCWC is 4. A first message uses the channel-message and configuration bits, Routing set, service ID 1 (or explicitly selected default 0), mode 0x01, and matching SDM/SCM. The complete fixture must contain at least 20 samples. This table is for fixture review; store the final packet bytes and expected size literally when implementing.

### Invalid input and unsupported features

- Datagram shorter than four bytes, truncated stream/config/chunk headers, declared-size mismatch, invalid content ID, oversized SCWC, bad suffix length, sample remainder, and zero duration.
- Under-minimum wave count, over-limit stride, invalid precision placement, missing mandatory descriptors, conflicting descriptors, and unknown/reserved categories.
- Sample-shutter, dynamic wavelength, and multi-head cases: expected success only when implemented; otherwise a precise unsupported result with no output.
- Invalid service/mode, data on closed channel, SCM mismatch, and replacement configuration failure.
- Large valid ping, legal reassembled UDP payload beyond 1500 bytes, and truncated datagram detection. Unsupported large data modes must not corrupt the receive parser.

### Stateful and timing sequences

- Sequence wrap, duplicate, gap, delayed older packet, and an interleaved management transaction.
- Timestamp wrap, equal legal Void timestamps, exact wave continuity, and a missing interval.
- Last-channel close while connection remains live; empty keepalives; timeout; empty close without connection.
- Two source ports on one host, two local destination endpoints, client-group exclusion, and resource exhaustion.
- Cancellation at each asynchronous boundary; next owner must not receive stale samples or rate commands.
- ACK accumulation, per-channel scope, static CnlClsd, and event occurrence during response send.

Prefer a compact matrix with meaningful representative values over thousands of trivial permutations. Add fuzz/property checks only as supplemental parser robustness coverage, not as a substitute for explicit semantic expectations.

## Sender state and API changes

Separate transport transaction state from a streaming channel's state:

- One persistent socket/local endpoint for a stream and its close/abort commands.
- Independent RT sequence and management transaction identifiers.
- Configured client group, channel ID, service ID, mode, descriptor bytes, and SDM.
- A monotonic stream origin, cumulative authored sample count, duration accumulator, and last configuration transmission time.
- Explicit idle/streaming/closing/closed state; beginning a new stream establishes fresh routing/timing intentionally.

Validate inputs instead of masking invalid channel/group values into another valid identifier. Reject nonpositive rates, impossible payload sizes/durations, and invalid configuration rather than clamping arbitrary values into plausible packets.

Keep packet construction pure and separate from transmission/pacing. A pure builder may be tested without a socket, while high-level streaming owns timing and state progression. Do not advance sequence/timestamps/config-sent state as though a send succeeded after a local send exception.

## Configuration lifecycle

1. Send Routing plus the full descriptor dictionary on the first data message (or an explicit preceding routing-only message with correct timing).
2. Repeat the unchanged configuration at a selected cadence of 200 ms, within the spec's 200/250 ms recovery expectation.
3. Track repetition by elapsed stream/monotonic time, not a fixed packet count that changes meaning with PPS and packet size.
4. Repeat on a data packet when practical; use a correctly timestamped Void/config message during an intentional idle period.
5. Do not change semantic configuration faster than 100 ms per channel. Repetition is not a change.
6. Change SDM when a configuration changes data structure/interpretation; put the same value into SCM on matching chunks.
7. During a stall exceeding link lifetime, do not assume prior routing remains. Establish a new explicit streaming epoch/configuration after recovery rather than burst stale samples.

Packet duration also constrains repetition: at low PPS, a maximum-size packet may cover more than the selected configuration interval. Cap sample count by both byte budget and configuration cadence, while preserving the 20-sample minimum. Reject an unsupported low-rate/cadence combination explicitly or schedule valid timestamped configuration-only messages; do not miss the cadence simply because the sender is sleeping for a long packet.

The basic sender can keep one fixed layout/SDM per run initially. Tests must still exercise the builder's matching bits and repeated configuration; dynamic layout changes are not required merely to correct ordinary streaming.

## Packet sizing and minimum sample count

Default IPv4/ordinary Ethernet budget: 1472 bytes of UDP payload, including the four-byte Hello header. Make the budget configurable for other paths; do not equate 1500-byte Ethernet MTU with 1500 bytes of application payload.

For eight-byte samples and this 16-byte dictionary:

- Without configuration: 4 Hello + 8 message + 4 wave = 16 overhead bytes; at most 182 samples in 1472 bytes.
- With configuration: add 4 configuration-header + 16 dictionary bytes = 36 overhead bytes; at most 179 samples.

Therefore the current 180-sample setting cannot be retained unchanged for configured packets under this budget. Prefer a uniform conservative maximum of 179 for this layout or calculate each packet's capacity explicitly. Recalculate capacity whenever layout/configuration changes. Also enforce Stream's message maximum 0xFF00 and the transport's actual maximum.

Every wave packet contains at least 20 samples. For finite input:

- Repartition a short final tail across earlier packets when possible: 65 points with a nominal 64 maximum can become 45 + 20, not 64 + 1.
- Do not append visible repeats or silently drop authored samples to meet the minimum.
- For a standalone input shorter than 20, return a clear error or use a separately requested documented padding policy; default to error.
- Continuous ILDA playback can carry remaining samples across source-frame boundaries because it is one wave stream. Do not emit invalid tiny packets merely because a source frame ended.

Test counts 19, 20, 39, 40, 64, 65, 179, 180, and final tails with fixed expected batch sizes. Distinguish a producer's nominal packet preference from the actual capacity limit.

## Accurate timestamps and pacing

Use one monotonic origin and integer/rational cumulative arithmetic for both packet timestamps and send deadlines. Do not repeatedly round an isolated packet duration and let error accumulate.

For fixed PPS, compute quantized cumulative boundaries from total sample count. Each chunk duration is the difference between consecutive boundaries; its timestamp is the prior boundary offset modulo 2^32. This ensures exact on-wire continuity while distributing fractional microseconds across chunks. Preserve the full-width schedule internally across wire timestamp wraps.

Wait until the corresponding timestamp deadline before transmission. Sending slightly late is allowed; sending ahead is not. Rate estimation and production scheduling must use the same cumulative timeline. Tests use a fake monotonic clock rather than wall-clock sleeps.

After a short final batch, advance by that batch's actual sample duration, not a full preferred-packet interval. Keep one schedule across ILDA source frames instead of resetting `next_send` for every frame.

Late sender policy:

- Small lateness may be sent late without changing the already-authored timeline.
- Define a finite lateness/backlog threshold; do not burst indefinitely to catch up or send future timestamps early.
- Beyond the threshold, close/abort the epoch and explicitly restart/configure if requested. Do not silently reset the wall-clock schedule while keeping an unrelated packet timeline.
- The intentional overload mode in hardware tests is a negative test, not compliant sender behavior. Label it accordingly and keep it out of normal streaming code paths.

First sample should be origin with optical signals off as recommended by Stream 3.2. Decide whether generated test patterns add an explicit blanked lead-in or require callers to provide one. If inserted, account for it as real samples/time and document the artwork change.

## Close, abort, and management UX

Close uses the same socket as streaming. Replace the claim that a new `stop` process can close another process's established connection. Either remove that misleading command, describe its limited behavior accurately, or implement an explicit local control mechanism in a separate future feature. Do not preserve IP-only server identity to make it work.

Provide separate close and abort methods with correct commands and semantics. An empty repeated close-with-ACK may return 0xEB after the connection is gone; handle this intentionally. Never blindly resend a lost sample packet with a new sequence number to obtain an ACK, since that duplicates content.

Optional ACK requests should confirm routing and expose diagnostics without turning the simplex stream into an unbounded blocking loop. Validate response source, command, group, sequence, struct size, and available length. Interpret pipeline errors separately from result code. An ACK does not prove optical completion.

Implement `scan` and `svcmap` response handling so commands print useful results rather than send and immediately exit. Ping verifies exact echo. Bound receive deadlines; do not let unrelated datagrams satisfy a pending transaction. Expose client group and validate 0..15. Use management identifiers independently of RT sequence.

## Native/Python test execution design

The runner should:

1. Validate fixture schema and literal lengths before running production code.
2. Run Python standard-library tests for packet building, batching, pacing, and response parsing.
3. Configure/build the small native target with available host C compiler and run it against actual firmware modules.
4. Report unsupported optional compiler features explicitly and fail on test/compiler errors.
5. Leave fixtures/source unchanged and avoid network/hardware access by default.

Pure tests have explicit inputs and expected outputs. Use fake clocks/output sinks only where state transitions require them. Do not write tests whose sole purpose is checking Python `struct`, sockets, or standard collection behavior.

The existing [all-target build runner](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/tools/build_all.py) invokes Bash for ESP builds and assumes configurable SDK paths. Document prerequisites for Windows/WSL/container use; do not claim the host-native tests automatically prove embedded compilation. Keep ESP V1, ESP V2, and RP2354 builds as separate required checks.

## Hardware test corrections

Update the existing IDN hardware helpers to use the corrected sender. Preserve deliberately malformed/flooding traffic only in clearly marked negative tests.

- Replace the permissive 15-second timeout pass condition with a one-second protocol expectation plus a justified observation/scheduling tolerance. Measure connection release and optical stop separately.
- Verify real route acceptance, descriptor interpretation, SCM recovery, and ACK state—not just whether sample counters rise.
- Compare queued, consumed, and discarded counters using the definitions from plan 03.
- Test competing protocols, web Stop during prebuffer/streaming, same-host different ports, and reconfiguration after timeout.
- Capture byte-level traffic from at least one independent client. Review licensing before copying third-party fixtures/code.
- Use an independent reference receiver or reviewed packet dissector to validate the Python sender separately from this firmware.
- Keep hardware tests opt-in with an explicit target and idle-state checks. Start with lasers disabled and use the existing controlled test procedure before optical observations.

Do not automatically run laser-driving tests as part of a unit-test command. Report skipped hardware checks plainly.

## Delivery increments

1. Fixture provenance, valid open/config/data packet, and a minimal native/Python runner. Demonstrate existing defects with small failing semantic cases.
2. Decoder/state/timing fixture families in step with plans 01–03.
3. Pure sender builders and explicit stream configuration/state.
4. Budget-aware batching, cumulative timestamps, pacing, and finite-tail handling.
5. Correct close/abort and useful management responses; update CLI help/examples.
6. Correct live tests and run independent interoperability checks.

Do not land a large permanent set of unexplained expected failures. Use focused reproductions during development and require affected tests to pass when each implementation increment lands.

## Implementation status

Run with `python tools/test_idn.py` (fixtures, Python, native; no network or hardware).

- **Fixtures** — `tests/idn/fixtures/*.json` (format in its `README.md`): 90 fixtures, 101 literal datagrams across management, ACK, valid/invalid stream, stateful sequences, sender output and tables. Each segment is annotated with field, value and section/page; project interpretations carry a note. PDFs are not committed; provenance records document identifiers and pages.
- **Runner** — `tools/test_idn.py` validates fixture schema and lengths, runs `tests/idn` unittest, then configures/builds/ctests every `tests/idn/native/*/CMakeLists.txt` separately under `build/idn-tests/` (Ninja or MinGW Makefiles; `--generator`, `--cc`). It generates `idn_fixtures.h` for native targets and reports sanitizer support as unsupported rather than passed. No native targets exist on this branch yet.
- **Sender** — `tools/idn_sender.py`: pure builders; `StreamConfig` (validated, no masking); `IDNStream` with explicit idle/streaming/closed state, routing + full dictionary on the first packet, configuration whenever the gap would otherwise exceed 200 ms of stream time, SDM = SCM, 1472-octet budget (179 samples), batch cap by cadence at low rates, ≥20 samples with tail repartition, cumulative integer timestamps with wrap, fake-clock pacing that never sends early, 100 ms lateness limit (close and raise, or `--restart-on-late` for a fresh epoch), state unchanged after a failed send, close/abort/close-with-ACK (0xEB = already closed) on the stream socket. Management (`ping`, `scan`, `svcmap`) uses its own sequence numbers and validates source, command, group, sequence, struct sizes and lengths. The misleading `stop` command was removed.
- **Decisions** — default service ID 0 (receiver's default for the mode; 1 available via `--service-id`); a blanked origin lead-in sample is prepended by default (`--no-lead-in`); 65 @ 64 → 45 + 20, and a donor that would drop below 20 merges with the remainder when within capacity.
- **Hardware helpers** — `tools/hwtest.py` uses `IDNStream`; `idn-overrun` is a labelled negative test using the pure builders; `idn-timeout` checks output stop and link release (0xEB probe) separately against 1 s + 0.5 s; packet sizes are 64/120/179. Not run: the current firmware's 6 s link timeout is expected to fail `idn-timeout` until plan 02 lands.
- **Not done** — ACK requests on data packets, Void keepalives during intentional idle, dynamic layout changes, independent receiver/dissector and third-party client captures, firmware builds (no ESP-IDF/pico-sdk here).

## Acceptance criteria

- [x] Literal spec-derived fixtures exist independently of sender output.
- [ ] Actual firmware decoder/session/timing code is executed in host tests. (Runner and fixture header ready; modules arrive with plans 01–03.)
- [x] First packet routes/configures; unchanged configuration repeats on time.
- [x] SDM/SCM agree and sample bytes match the declared dictionary.
- [x] All ordinary wave packets meet sample-count, size, and duration bounds.
- [x] Cumulative timestamp continuity survives variable batches and wrap.
- [x] Pacing never intentionally sends ahead; late recovery is bounded and explicit.
- [x] Management traffic does not create RT sequence gaps.
- [x] Close/abort use the established socket, and CLI behavior is accurate.
- [x] Hardware timeout checks cannot pass merely because output eventually stopped within 15 seconds. (Code only; not run on hardware.)
- [ ] All-target builds and independent-client/receiver checks are reported separately from unit tests. (The runner states they were not run; none were performed.)
- [ ] Documentation distinguishes verified behavior, unsupported capabilities, and unrun hardware checks. (Done for fixtures, sender help and this plan; top-level README not updated.)
