# IDN plan 02: Protocol state, ownership, and shutdown

Status: partially implemented on branch `idn/02-protocol-state`, stacked on `idn/01-safe-decoding` (which is stacked on `idn/04-fixtures-and-sender`): host-tested state machine, integrated with the plan 01 decoder in the IDN server. Output lease across producers, local-endpoint identity, plan 03 integration, all-target builds, and hardware validation remain outstanding. See [Implementation status](#implementation-status).

## Objective and dependencies

Replace implicit auto-open behavior and reference-count-only output sharing with explicit connection, channel, service, and producer ownership. A packet must never gain access to laser output merely because it resembles sample data.

Companion plans: [safe decoding](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/01-safe-decoding.md), [timing and diagnostics](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/03-timing-and-diagnostics.md), and [fixtures/sender](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/04-fixtures-and-sender.md).

Plan 01 supplies transactional decoder results. This plan owns state transitions and resource release. Plan 03 supplies timeline validation, event accumulation, and ACK serialization. Plan 04 supplies independent state-machine scenarios from the start.

## Normative anchors

Sources: [Stream rev002](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/ILDA-IDN-Stream-rev002.pdf), [Hello draft](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/IDN-Hello-2022-03-27-draft.pdf).

| Requirement | Reference |
|---|---|
| Configuration required to open; routing precedes data; close follows data | Stream 2.2 |
| All 64 channel identifiers supported even when concurrent capacity is limited | Stream 2.1 |
| Continuous graphics is exclusive per service | Stream 3.2 |
| SCM mismatch discards configuration and voids channel until new configuration | Stream 6.7 |
| Correct connection identification includes client and server address/port | Hello 7.1 |
| Empty RT message establishes/refreshes link, not channel output | Hello 6.1, 6.2.1, 6.3.1 |
| Default one-second link timeout closes session/output | Hello 7.2 |
| Graceful close and abort have distinct command semantics | Hello 6.2.2, 6.2.3, 6.3.2 |
| Group requests are answered; excluded active sessions are closed | Hello 3.2 |

Keep mandatory behavior separate from project policy: single-session capacity, no reordering window, bounded shutdown budgets, and rejecting unsupported modes are chosen implementation limits, not restrictions imposed on every IDN device.

## Current integration points

- [IDN server](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/idn_protocol.c): IP-only session identity, auto-open, missing SCM, asynchronous feed stop plus immediate ring reset.
- [IDN public API](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/idn_protocol.h): currently exposes start only; needs controlled cancellation/status integration.
- [Manager interface](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/rp_manager.h), [V2](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/rp_manager.c), [V1](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/rp_manager_v1.c): shared samples, rates, queues, and force-stop behavior.
- [Web controls](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/web_server.c), [SD player](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/ilda_player.c), [Ether Dream](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/etherdream_protocol.c), and [ILW](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/ilw_protocol.c): all producers must participate in ownership/cancellation.
- [RP receiver](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/RP2354/src/stream_rx.c): retain and extend existing stale-packet protections, rather than replacing them with an ESP-only flag.

Proposed state-machine module: `C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/idn_session.c` and matching header. Prefer pure transition logic with injected clock/output actions; keep socket and FreeRTOS adaptation in the server.

## State model

### Connection/session

Keep one allocatable IDN session initially. Use FREE, ACTIVE, CLOSING, and FAULTED states. FAULTED is unavailable until resources are known quiescent; a timeout in a cleanup wait must not mark the device free.

Store peer address/port, local endpoint identity, client-group bookkeeping, last accepted RT activity time, connection generation, channel table, and diagnostics/timeline state from plan 03.

On a wildcard-bound multihomed UDP socket, source address/port alone is insufficient to distinguish all five-tuples. Investigate lwIP receive packet-info support or bind per-local-address sockets. Ensure replies use the matching local endpoint. Document this choice before calling identity complete; do not silently merge Ethernet/Wi-Fi destinations.

Management traffic does not acquire the output lease. Empty accepted RT traffic can allocate the session but must not start a feed task, set a sample rate, open a channel, or open the shutter.

### Channel

Use CLOSED, VOIDED, ROUTED_IDLE, and ROUTED_ACTIVE states, with independently represented service route, decoder validity, SDM value, and route generation. Do not reduce these distinctions to one `open` boolean.

- CLOSED: no active route. Non-void data without routing produces closed-channel error.
- VOIDED: invalid/busy route or invalidated configuration; no physical output. Preserve enough information to report routing failure consistently.
- ROUTED_IDLE: accepted route/configuration but no accepted output data yet.
- ROUTED_ACTIVE: accepted data is queued or playing for this route.

Support channel IDs 0 through 63 even when only one continuous physical route may be active. Service ID 0 resolves by mode to the default; ID 1 resolves to the advertised laser service. Other IDs do not fall through to laser output.

Implement the Void service mode for testing without acquiring the physical laser. Unsupported discrete/effects/audio/manufacturer-specific modes do not play wave data and report the appropriate mode/feature error. Rejecting these modes remains a documented conformance gap.

## Ordered packet processing

### Datagram reception boundary

Replace the fixed 1500-byte receive assumption with a bounded datagram strategy that preserves the original datagram length and detects truncation. Investigate lwIP facilities before selecting a full-size reusable buffer, a size-probe/receive sequence, or another supported approach. Budget the maximum legal IDN message (0xFF00 plus Hello framing), arbitrary legal ping payloads, concurrent management replies, and heap/PSRAM use explicitly. Never echo a truncated ping as if it were complete. Never dispatch a partial datagram after silently clamping its declaration. Packet-size compatibility and memory-exhaustion behavior need native and hardware tests; application-layer frame reassembly remains deferred with discrete graphics.

### Acceptance and dispatch

1. Validate datagram framing and the outer message boundary without mutating a session.
2. Identify command and endpoint, check client-group eligibility, and decide whether this command may establish a connection.
3. Apply sequence acceptance and input timeline policy from plan 03.
4. For a routing configuration, resolve service/mode and compile a candidate decoder with plan 01.
5. Acquire or transfer the exclusive service route transactionally. Repeated identical routing must not restart output or reset the timeline.
6. Store SDM with accepted service configuration; compare SCM before accepting the chunk.
7. Validate chunk type, payload, rate, and continuity, then enqueue an ownership-tagged unit of work.
8. Apply channel-close semantics after this message's data processing in the chosen pipeline order.
9. Execute connection-close semantics if requested, independently of whether any channels remain open.
10. Produce the appropriate ACK snapshot through plan 03, without claiming physical completion just because processing was queued.

When Routing is clear, ignore the service-routing fields as the specification requires; do not use them to overwrite the route or decoder. Specify the parser's handling of an invalid nonzero SCWC without Routing so it cannot shift the chunk boundary silently.

Failure of a replacement configuration must invalidate/quarantine that channel's old decoding state rather than decode the new message under stale assumptions. Identical repeated configuration is not a semantic change. Producers must respect the 100 ms minimum between changes; a consumer-side guard, if added, must distinguish repetition from change.

## Transition scenarios

| Event | Required state/output effect |
|---|---|
| Empty 0x40/0x41 on new eligible link | Allocate session only |
| Valid routing plus Void chunk | ROUTED_IDLE; shutter closed for continuous mode |
| First valid matching wave chunk | Acquire authorized output path; schedule samples |
| Non-void data on CLOSED channel | No auto-open; record CCErr |
| Second continuous channel requests same service | No mixed samples; report BSYErr for failed route |
| Unknown service/mode | No physical output; report SMErr |
| SCM differs from stored SDM | Invalidate decoder/route; record DCMErr; wait for configuration |
| Last channel closes | Keep connection ACTIVE; release channel resources safely |
| Empty close-with-ACK without connection | Return 0xEB; do not allocate session |
| Same IP, different source port | Distinct link; refuse if sole session unavailable |
| Valid abort on active link | Cancel output and reset channels; close connection |
| Default link timeout | Close session/output after one second of accepted RT inactivity |
| Management request while streaming | Respond without corrupting RT sequence/output ownership |

A message may route, carry data, and close the channel in one operation. Tests must cover it. Channel closure should be represented at the correct pipeline boundary; an empty channel table is not permission to tear down the connection or drop the just-accepted work.

## Exclusive output lease across the codebase

Introduce an owner kind, owner identifier, and monotonically advancing generation/token at the shared output boundary. Every enqueue, start, scheduled rate update, and release must validate the token. The physical laser has one owner at a time; reference counts alone cannot enforce this.

Suggested initial policy:

- Reject a competing normal producer while a lease is held; do not interleave.
- An explicit user takeover/Stop cancels the prior producer, waits for acknowledged quiescence, then allows a new lease.
- IDN may hold a transport session while the service is busy; report the failed route separately from inability to allocate a session.
- Rate and buffer-setting operations during output must follow owner-aware policy. They cannot secretly retime a different producer's queue.
- Cancellation invalidates tokens before new enqueues. Late work from old tasks must fail even if memory/queue slots have been recycled.

Migrate all producers in one coherent integration series: IDN, Ether Dream, ILW, SD playback, and test patterns. The web Stop path must notify/cancel IDN rather than merely stopping the hardware under its still-running task. Do not remove existing safety clamps or RP epoch checks.

## Synchronized shutdown

Required sequence:

1. Mark connection/owner closing and prevent new work for its generation.
2. Close lifecycle shutter and apply the selected optical-stop policy.
3. Signal feed cancellation with a notification/event; do not merely flip a volatile flag.
4. Interrupt bounded waits for queue slots/backpressure. Do not require a lock held by the caller for the worker to acknowledge cancellation.
5. Obtain feed-task quiescence. Only then reset its SPSC indices or reclaim decoder/queue storage.
6. Cancel/drain downstream work according to operation; synchronize in-flight SPI and RP output using existing stale-generation protection plus explicit completion.
7. Park/clear as required, confirm output state, and release the lease.
8. Reset channel/session state only after no old task can publish into it.

Never rearm a task still exiting after an arbitrary 300 ms wait. On a deadline failure, remain unavailable, keep blanking asserted, record a fault, and use a defined recovery path. Bound shutdown latency through measurements; do not present a polling interval as an exact physical stop guarantee.

Graceful connection close may drain output or move home under Hello. Initial policy: stop accepting work, move home with optical output blanked, discard remaining queued work under controlled ownership, and release after quiescence. Abort skips optional draining and initiates cancellation immediately. Abort is not a safety-rated emergency stop. A close ACK reports protocol reception/state, not an invented guarantee that motors have already settled.

## Client groups and timeout

- Default group mask: all sixteen groups enabled.
- Implement GET/SET mask requests, struct-size checking, response codes, and the draft's authentication field semantics.
- Ping, scan, service-map, and group-mask administration remain available as specified regardless of exclusion.
- Define how group changes on an existing tuple are tracked; a disabled group must never reach session processing. Excluding an active session's group initiates closure.
- Authentication here is limited cooperative access control, not a network security boundary. Do not log authentication bytes.
- Replace the six-second default with one second. Refresh only on accepted applicable RT traffic, not malformed packets or packets from another connection.
- Separate link liveness from any additional session/data watchdog. Empty RT packets keep the link allocated; they must not invent valid sample activity.

## Implementation sequence

1. Add pure state representation and table-driven transitions with a fake clock/output sink.
2. Implement framing-before-allocation, tuple identity, service/mode validation, and explicit no-auto-open.
3. Implement SCM/SDM and routing/close sequencing, including repeated configurations.
4. Introduce the shared lease/generation contract and migrate every producer and user stop/takeover path.
5. Replace asynchronous reset races with cancellation acknowledgment and bounded downstream cleanup.
6. Implement one-second timeout, abort, group mask operations, and fault recovery.
7. Integrate plan 03 snapshots and run all-target builds plus repeated hardware transitions.

## Tests and exit criteria

Host scenarios must use explicit input packets/events and expected states/actions: 64 IDs, default/invalid routes, repeated configs, matching/mismatching SDM, two clients on one host, two destination interfaces, empty keepalive, last-channel close, combined open/data/close, group exclusion, repeated abort/close, and failed decoder replacement.

Concurrency tests must force cancellation at: before ring read, during ring read, waiting for a payload slot, queued SPI work, in-flight SPI, immediately before START, and immediately after lease revocation. Assert no stale samples, starts, or rates occur after the next generation begins.

Hardware validation starts with the laser disabled. Repeat stop/restart/takeover scenarios at least 20 times, logging host/session generation, feed completion, RP fill, and shutter state. Verify timer expiration separately from optical blanking and physical parking.

- [x] No packet without a valid route/configuration produces samples. *(Routing plus a configuration accepted by `idn_decoder` is required; no default layout. Host-tested only.)*
- [ ] Connection identity does not merge independent ports/interfaces. *(Ports: done. Interfaces: local address not yet available, see below.)*
- [ ] Only one producer owns physical output at a time. *(Not started: no lease at the manager boundary.)*
- [x] Last-channel close does not implicitly close the connection. *(Host-tested only.)*
- [x] SCM mismatch cannot reuse stale decoder state. *(Host-tested only.)*
- [ ] Timeout, abort, exclusions, and user Stop cancel the producer and output consistently. *(IDN side implemented and host-tested; not verified on hardware; other producers unchanged.)*
- [ ] No ring reset races, stale generation writes, or unbounded cancellation waits remain. *(IDN ring reset now waits for feed exit; manager slot-generation race remains.)*
- [ ] V1, V2, and RP builds pass; transition tests and hardware logs are recorded. *(Host tests pass; no embedded build or hardware run.)*

## Implementation status

Done (branch `idn/02-protocol-state`):

- `firmware/ESP32S3/main/idn_session.c/.h`: pure state machine (FREE/ACTIVE/CLOSING/FAULTED; CLOSED/VOIDED/ROUTED_IDLE/ROUTED_ACTIVE) with injected decoder/output actions and optional plan 03 hooks (`rt_input`, `wave_timeline`, currently NULL).
- Framing before allocation; IP+port identity; 1 s link timeout refreshed only by accepted RT packets from the connection; 0xEB/0xEC/0xED/0xEE results; abort (0x46); group GET/SET (0x0C/0x0D) with exclusion closing the connection.
- Service ID 0/1 resolution; Void mode; other modes SMErr; exclusive continuous route with BSYErr; SDM stored at routing, SCM checked per chunk; exact repeat detection; failed replacement voids the old decoder; close after data; ACK events latched per connection/channel, CnlClsd static.
- `idn_protocol.c`: `idn_decoder` (plan 01) is the only decoder, called through the session hooks (`config_compile` -> `idn_decoder_compile`, `wave_check` -> `idn_decoder_check_wave_chunk`, enqueue -> `idn_decoder_decode`); its results map to CFGErr/IAPErr and MCLErr/PVLErr/RGUErr, rejections are logged once per reason, chunks are queued whole or not at all. A configuration whose SCWC region is cut short is malformed (0xEE, no link refresh) and, from the live connection, voids that channel's decoder. No auto-open; feed task started only on the first accepted wave chunk; cancellation via flag + wake semaphore, server waits for the feed's exit acknowledgment (1.5 s) before resetting the ring; a missed deadline leaves the session FAULTED until the feed reports exit; no 300 ms rearm. Datagrams over 1500 bytes are dropped, never truncated or partially echoed.
- Web Stop calls `idn_protocol_cancel_output()` before stopping the hardware.
- Host tests: `tests/idn/native/session/` (28 table scenarios + 64-channel and generation checks), run by plan 04's `tools/test_idn.py` alongside `native/decoder`.

Project policy choices (not spec requirements): total size must equal the payload; SCWC without Routing, and Void chunks with data, are malformed; malformed ACK requests get 0xEE without touching the session (plan 03 gate may change this); non-channel messages get MVErr; abort is honoured only from the connection's tuple; any excluded group seen on a connection closes it; group SET requires the 12-octet code to equal the configured one (default empty), never logged; close/abort/timeout/Stop discard queued output rather than drain; a changed valid configuration keeps the route and output; a compliant sender re-routes and resumes after a web Stop (no sticky stop).

Not done:

- Output lease/generation at `rp_manager` (V1 and V2) and migration of Ether Dream, ILW, SD, and test patterns. Known gap: `send_packet_payload()` stamps the slot generation after its free-slot wait, so a producer blocked across `payload_queue_reset()` can publish under the new generation. The IDN feed's wait inside `rp_manager_send_samples_packet()` cannot be interrupted, only bounded by the TX task's timeouts.
- Local-endpoint identity: the wildcard socket passes local address 0; needs `LWIP_NETBUF_RECVINFO`/`IP_PKTINFO` or per-interface sockets and matching reply source.
- Datagram strategy beyond 1500 bytes; lifecycle shutter (still open in plan 01); a host test of the `idn_protocol.c` adapter itself (session tests use a fake decoder/output sink); scan OCPD/XCLD and SNMsg/DOBuf/latency (plan 03; DOBuf currently reads 0, which is not truthful); sequence handling (plan 03).
- Concurrency tests at each cancellation boundary, ESP V1/V2/RP builds, and hardware runs.
