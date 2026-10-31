# Laser DAC streaming protocols: reference notes for the ILDAWaveX16V2 firmware review

Compiled 2026-10-23 from primary specs and reference implementations. Every fact carries a source URL.
Items marked **[unverified]** were not confirmed against a primary source. Items marked **[local]** come from
this repository, not from the web.

Source PDFs (IDN-Stream rev 002, IDN-Hello 2022, IDTF rev 011, the IDN Wireshark dissector notes) were
read from local text extracts that are not in the repository; each is cited by its public URL.

Abbreviations: ED = Ether Dream; j4cDAC = the open-source firmware of the original Ether Dream (ED1);
IDN = ILDA Digital Network; IDTF = ILDA Image Data Transfer Format.

---

## 0. What "ILW" is **[local]**

ILW is this board's own UDP protocol ("ILW-NET v1 / ILW-UDP v1", `tools/ilw_sender.py`), not a public standard.
From `tools/ilw_sender.py` and `firmware/ESP32S3/main/ilw_protocol.c`:

- UDP, default port **45457**. Magic byte `0x49` ('I'). Packet types: CONFIG=1, POINTS=2, CONTROL=3. Version 1.
- CONFIG packet: `<BBBBBH` = magic, type, version, point format, flags, pps/100 (little-endian).
  Flags: OUTPUT_ENABLE 0x01, CLEAR_BUFFER 0x02, APPLY_DEFAULTS 0x04.
- POINTS packet: `<BBB` = magic, type, point count, then packed points. **No sequence number, no timestamp.**
- CONTROL packet: `<BBB` = magic, type, command. Commands: STOP 0, OUTPUT_ON 1, OUTPUT_OFF 2, CLEAR_BUFFER 3, PARK 4.
- Point formats: XY (4 B), XYRGB16 (10), XYRGBI16 (12), FULL16 (16), XYRGB8 (7), XYRGBI8 (8), FULL8 (10).
- Firmware constants: `ILW_MAX_PPS 200000`, `ILW_SAMPLE_RING_CAPACITY 16384`, `ILW_PREFILL_TARGET_US 30000`
  (4..12 packets), `ILW_BLANK_TIMEOUT_US 100000`, `ILW_DISABLE_TIMEOUT_US 500000`,
  `ILW_RP_BUFFER_HIGH_WATER_SAMPLES 15360`, `ILW_RP_BUFFER_LOW_WATER_SAMPLES 14848`.
- The sender docstring states the design intent: "The DAC owns the real output timing, buffering, underrun
  handling, and safety blanking."
- **Port clash note:** UDP 45457 is also the LaserCube/LaserDock *command* port (see section 4.3). LaserCube
  command bytes (0x77, 0x78, 0x80, 0x8a) do not equal the ILW magic 0x49, so a magic check rejects them, but
  LaserOS-style broadcast discovery on this port will reach the ILW socket.
  Source for LaserCube ports: https://raw.githubusercontent.com/nannou-org/lasercube/main/SPECIFICATION.md

---

## 1. Ether Dream protocol

Primary: https://ether-dream.com/protocol.html
Firmware (ED1, GPLv3): https://github.com/j4cbo/j4cDAC
Host library structs: https://raw.githubusercontent.com/j4cbo/etherdream-driver/master/protocol.h

### 1.1 Transport
- TCP port **7765**; the DAC accepts one control host at a time and rejects others while a connection is up.
  https://ether-dream.com/protocol.html
- UDP status broadcast **once per second** to the local broadcast address regardless of data source.
  https://ether-dream.com/protocol.html — port **7654** (`#define BROADCAST_PORT 7654`).
  https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/net/broadcast.c
- On TCP connect the DAC immediately sends a status reply "as if a ping had been received".
  https://ether-dream.com/protocol.html — j4cDAC: `send_resp(pcb, RESP_ACK, '?', 0)` in the accept callback.
  https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/net/point-stream.c
- Byte order: the protocol page says little-endian but also "most significant byte appears first"; the page is
  self-contradictory. All real implementations use **little-endian** (e.g. nannou `ether-dream`, Volst
  `writeUnsignedInt32`). https://ether-dream.com/protocol.html ;
  https://raw.githubusercontent.com/Volst/laser-dac/master/packages/ether-dream/src/EtherConn.ts
- Fields packed, no padding. https://ether-dream.com/protocol.html

### 1.2 Commands (byte, layout, rules)
Source unless noted: https://ether-dream.com/protocol.html ; j4cDAC dispatch:
https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/net/point-stream.c

| Cmd | Byte | Layout | Rules |
|---|---|---|---|
| Prepare | `'p'` 0x70 | 1 byte | Only if light engine Ready and playback Idle. Resets buffer and point_count to 0, clears underflow/e-stop playback flags. ACK else NAK-Invalid. j4cDAC `dac_prepare()` returns -1 unless `DAC_IDLE && LIGHTENGINE_READY`. |
| Begin | `'b'` 0x62 | `u8 cmd; u16 low_water_mark; u32 point_rate` (7 B) | ACK if Prepared and buffer non-empty, else NAK-Invalid. `low_water_mark` "currently unused" (j4cDAC: call commented out `// XXX`). j4cDAC NAKs if `point_rate > DAC_MAX_POINT_RATE`. |
| Update rate | `'u'` | same as `'b'` | j4cDAC only: `dac_set_rate()` without starting. Not on the protocol page. Modulaser notes a NAKed `'u'` must be tolerated. https://github.com/ModulaserApp/laser-dac-rs/pull/43 |
| Queue rate change | `'q'` (protocol.h comment says 0x74 — that is `'t'`; the ASCII `'q'` is 0x71) | `u8 cmd; u32 point_rate` (5 B) | NAK-Invalid unless Prepared/Playing; NAK-Full if rate FIFO full (j4cDAC `DAC_RATE_BUFFER_SIZE 200`, keeps one slot empty). Applied when a point with control bit 15 (`DAC_CTRL_RATE_CHANGE 0x8000`) is played. |
| Data | `'d'` 0x64 | `u8 cmd; u16 npoints; dac_point[]` | ACK if all points fit else NAK-Full. `npoints == 0` ACKed when Prepared/Playing. Page does not state result of non-zero data when Idle; j4cDAC: `dac_request()` returns -1 when idle, data is discarded and **NAK-Invalid** `'I'` is sent for `'d'`. nannou emulator also NAK_INVALID. https://raw.githubusercontent.com/nannou-org/ether-dream/master/dac-emulator/src/stream.rs |
| Stop | `'s'` 0x73 | 1 byte | ACK if Playing/Prepared else NAK-Invalid; goes to Idle. |
| E-stop | `0x00` or `0xFF` | 1 byte | Always ACK; enters E-Stop from any state. **Any unrecognised command byte is also treated as E-stop**; j4cDAC additionally *closes the connection* (`close_conn(... CONNCLOSED_UNKNOWNCMD ...)`). |
| Clear E-stop | `'c'` 0x63 | 1 byte | ACK if e-stop was from command; NAK-Invalid if not in e-stop; NAK-Stop-Condition `'!'` if cause still active. |
| Ping | `'?'` 0x3F | 1 byte | ACK; acts as keep-alive. |
| Version | `'v'` | 1 byte | j4cDAC sends the build string (not on protocol page). |
| `'I'`, `'P'`, `'D'` | | | j4cDAC plugin install/invoke/data-via-plugin (undocumented). |

Responses (https://raw.githubusercontent.com/j4cbo/etherdream-driver/master/protocol.h):
`RESP_ACK 'a'`, `RESP_NAK_FULL 'F'`, `RESP_NAK_INVL 'I'`, `RESP_NAK_ESTOP '!'`.
Each response is `dac_response { u8 response; u8 command; dac_status }` = **22 bytes**.

### 1.3 Structs
`dac_point` (18 bytes): `u16 control; i16 x; i16 y; u16 r,g,b,i,u1,u2`. Full-scale 65535; low bits ignored on
lower-resolution DACs. control bit 15 = rate change; other bits reserved (TTL outputs).
https://ether-dream.com/protocol.html

`dac_status` (**20 bytes**, not 22 — 22 is the response including the 2-byte prefix):
`u8 protocol; u8 light_engine_state; u8 playback_state; u8 source; u16 light_engine_flags; u16 playback_flags;
u16 source_flags; u16 buffer_fullness; u32 point_rate; u32 point_count`.
https://raw.githubusercontent.com/j4cbo/etherdream-driver/master/protocol.h
- `point_rate`: configured rate if Prepared/Playing else 0 (page); j4cDAC fills it **only when Playing**.
  https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/net/broadcast.c
- `point_count`: points emitted since Playing began; 0 when Prepared/Idle.
- `buffer_fullness`: points currently buffered (`dac_fullness() = produce - consume`, mod buffer).
  https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/lib/dac.c

`dac_broadcast` (36 bytes): `u8 mac_address[6]; u16 hw_revision; u16 sw_revision; u16 buffer_capacity;
u32 max_point_rate; dac_status status`. j4cDAC fills `buffer_capacity = DAC_BUFFER_POINTS - 1` (= **1799**),
`max_point_rate = DAC_MAX_POINT_RATE` (= **100000**), `sw_revision = 2`.
https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/net/broadcast.c ;
https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/inc/dac.h

States: light_engine 0 Ready, 1 Warmup, 2 Cooldown, 3 E-stop. playback 0 Idle (no points accepted, outputs 0 V),
1 Prepared (accepts points, output as Idle), 2 Playing. source 0 network, 1 SD ILDA, 2 abstract generator.
light_engine_flags: bit0 e-stop from packet/invalid cmd, bit1 projector e-stop input, bit2 e-stop input active,
bit3 overtemp e-stop, bit4 overtemp active, bit5 Ethernet link loss. playback_flags: bit0 shutter open,
bit1 underflow ended last stream, bit2 e-stop ended last stream (bits 1,2 cleared by Prepare).
https://ether-dream.com/protocol.html

### 1.4 Buffer, rate limits, underflow behaviour (ED1 firmware)
- `DAC_BUFFER_POINTS 1800` ("up to 60 ms at 30k, 45 ms at 40k, 36 ms at 50k, 30 ms at 60k").
  `dac_request()` reserves one slot, so usable = 1799 and advertised `buffer_capacity` = 1799.
  https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/lib/dac.c ;
  https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/inc/dac.h
- `DAC_MAX_POINT_RATE 100000` ("100kpps ought to be enough for anyone"); `dac_set_rate()` asserts
  `0 < pps <= 200000`. No minimum rate beyond > 0. Modulaser found **rate 0 hangs ED2 r331 firmware** and
  **rates above the advertised max are NAKed**. https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/inc/dac.h ;
  https://github.com/ModulaserApp/laser-dac-rs/pull/43
- **Underflow:** in the point ISR, `if (produce == consume) dac_stop_underflow()` →
  `dac_stop(DAC_FLAG_STOP_UNDERFLOW)`: stops the timer, closes the shutter, `hw_dac_zero_all_channels()`
  ("set all outputs to zero"), state → **DAC_IDLE**, flag bit1 set. So the real ED1 **does not hold the last
  point and does not go back to Prepared; it drops to Idle with all channels at zero** and the client must
  re-Prepare and re-Begin. https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/lib/dac.c
  (Whether "zero" means centre or corner is hardware-dependent and not shown in that file — [unverified].)
- `dac_stop()` is the common path for Stop, underflow, e-stop and source switch (`DAC_FLAG_STOP_ALL 0x0E`).
  https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/inc/dac.h
- `dac_start()` requires Prepared and non-zero pps; opens the shutter (`shutter_set(1)` only when Playing).
  https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/lib/dac.c
- Overflow protection: when `dac_request()` returns 0 the remaining points of that `'d'` are discarded and the
  command is NAKed (`DATA_ABORTING`). The whole packet is lost, not partially accepted.
  https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/net/point-stream.c
- No idle/keep-alive timeout exists in j4cDAC's point-stream code; e-stop on Ethernet link loss is a light-engine
  flag (bit 5) but `lightengine.c` is "basically a stub". https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/lib/lightengine.c
- ACKs can be deferred up to 24 deep (`PS_DEFERRED_ACK_MAX 24`) and are **dropped** beyond that
  ("!!! DROPPING ACK !!!"), which is why clients must not rely on 1:1 ACK accounting.
  https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/net/point-stream.c ; see also
  https://github.com/j4cbo/j4cDAC/issues/18
- E-stop: `le_estop()` sets flag bits, `le_state = LIGHTENGINE_ESTOP`, `dac_stop(DAC_FLAG_STOP_ESTOP)`;
  `le_estop_clear()` returns to Ready only when no flags remain; globals, so it persists across connections.
  https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/lib/lightengine.c

### 1.5 Hardware-verified ED2 behaviour (Modulaser, 2026)
From https://github.com/ModulaserApp/laser-dac-rs/pull/43 (ED2 firmware r331, hardware-verified):
- Rate 0 hangs the firmware; rate above advertised max is NAKed.
- All TCP clients share one playback state.
- **Stale `buffer_fullness` persists until the next Prepare** (an idle DAC can report non-zero fullness).
- Closing the TCP connection **stops playback on ED2**; **ED1 plays out its buffered points** instead.
- ED2/ED3/ED4 buffer capacity and max rate: not published in any source found. The protocol page only says
  "newer versions have a larger buffer". **[unverified]** Use the broadcast's `buffer_capacity`/`max_point_rate`.

### 1.6 How clients pace against `buffer_fullness`
The protocol gives no pacing rule; every client estimates fullness and tops up to a target.

| Client | Target / rule | Per-write | Begin condition | Source |
|---|---|---|---|---|
| libetherdream (j4cbo reference, Unix) | `expected = fullness + unacked - elapsed*pps`; `cap = 1700 - expected`; sleep `500 us + diff/pps` when `cap <= 40` | max **80** points | `fullness > 1600 && playback_state == 1`, `low_water_mark = 0`; sets rate per packet with control bit 15 | https://raw.githubusercontent.com/j4cbo/j4cDAC/master/driver/libetherdream/etherdream.c |
| etherdream-driver (j4cbo, portable/Windows EzAudDac-style) | `cap = 1600 - expected_fullness`; sleep `1 ms + diff/pps` | max 80 points; small frames duplicated to ~250 points; `pointrep = 16000/pps` | `start_if_ready()` | https://raw.githubusercontent.com/j4cbo/etherdream-driver/master/dac.cpp |
| Volst laser-dac (Node) | `N = 1799 - fullness`; send only if `N > 50`, else ping | N | begin after first data ACK; re-begin when `playback_flags == 2` (underrun) | https://raw.githubusercontent.com/Volst/laser-dac/master/packages/ether-dream/src/EtherConn.ts |
| nannou_laser (Rust) | `latency_points` default = points for 1/60 s ("~16 ms"), clamped to `buffer_capacity`; request `latency_points - fullness`, capped at `capacity - 1 - fullness` | | Prepare, queue `capacity` blank points, `begin(0, min(pps, max_point_rate))` | https://raw.githubusercontent.com/nannou-org/nannou_laser/master/src/stream/raw.rs |
| Modulaser laser-dac-rs | 80 ms target buffer, capped at 80 % of ring; writes admitted against decayed fullness; idle DAC's stale fullness treated as empty; 2 s stall timeout | | | https://github.com/ModulaserApp/laser-dac-rs/pull/43 |
| MadMapper | Paces on the fullness in the *last reply*; treats **any NAK as fatal** and reconnects; starts with `point_rate = -1` and sets the rate via the queue | | | https://github.com/naileclevrai/TheatricalExtraLights/pull/1 |
| TouchDesigner EtherDream CHOP | user "Queue Time" (samples/frames/seconds) sets the target queue | | | https://docs.derivative.ca/EtherDream_CHOP |
| tgreiser/etherdream (Go) | default 24000 pps; `Begin` lwm unused | | | https://github.com/tgreiser/etherdream |
| LaserOS / LSX / Beyond / OpenLase / LaserBoy | **[unverified]** No source found. ED vendor lists LSX, ILD SOS, LFI Player, "any EzAudDac-compatible" software; a 2021 Pangolin forum reply says Ether Dream is not compatible with BEYOND. OpenLase's tree has no Ether Dream output (it outputs via JACK). | | | https://ether-dream.com/ ; https://forums.pangolin.com/threads/how-can-i-connect-etherdream-dac.26093/latest ; https://github.com/marcan/openlase |

Latency conventions: libetherdream keeps ~1700 points queued (57 ms at 30 kpps); Modulaser 80 ms; nannou
~16 ms. The ED1 buffer itself is 1800 points. The j4cDAC header says frames "should be reasonably large (at
least 50-100 points)" to reduce overhead. https://raw.githubusercontent.com/j4cbo/j4cDAC/master/driver/libetherdream/etherdream.h

A DAC-side lesson from the TheatricalExtraLights emulator: a client pacing on stale fullness will overshoot;
they made storage 4x the advertised capacity, drop oldest on overshoot, and clamp reported fullness to the
advertised size so MadMapper never sees NAK-Full. https://github.com/naileclevrai/TheatricalExtraLights/pull/1

---

## 2. IDN (ILDA Digital Network)

Standards index: https://www.ilda.com/technical.htm
- IDN-Stream **Rev 002, July 2025** (current official): https://www.ilda.com/resources/StandardsDocs/ILDA-IDN-Stream-rev002.pdf
- IDN-Hello **draft 2022-03-27** (not yet official): https://www.ilda.com/resources/StandardsDocs/IDN-Hello-2022-03-27-draft.pdf
- IDN-File draft 2021-01-15: https://www.ilda.com/resources/Tech/IDN/IDN-File---2021-01-15-draft.pdf
- "Understanding IDN – Wireshark Traces" (Uni Bonn, 2021): https://ilda.com/resources/Tech/IDN/Understanding-IDN_Wireshark-Traces_v2021-05-26.pdf
- Reference code (MIT, Dirk Apitz/DexLogic): https://github.com/DexLogic/idn-C-helloScan ,
  https://github.com/DexLogic/idn-C-serverList , https://github.com/DexLogic/idn-C-idtfPlayer ;
  the same headers ship in the Helios SDK: https://raw.githubusercontent.com/Grix/helios_dac/master/sdk/cpp/idn/idn-stream.h

All multi-byte fields are **big-endian** (network order); 16/32-bit alignment is kept; unused bits SHALL be 0.
https://www.ilda.com/resources/StandardsDocs/ILDA-IDN-Stream-rev002.pdf (section 1.3, 1.4)

### 2.1 IDN-Hello (discovery + IDN-RT transport), UDP 7255
Source: IDN-Hello 2022-03-27 draft (above).
- Servers SHALL listen on **UDP 7255** and respond from the same port. Clients may use separate ports for
  admin and streaming; IDN-RT is connection-based on the 5-tuple, so a second client port = second session.
- Packet header, 4 octets: `u8 command; u8 flags (low nibble = client group 0..15); u16 sequence`. Packets
  smaller than 4 octets SHALL be discarded.
- Commands: 0x00 void; **0x08 ping req / 0x09 ping rsp** (payload echoed exactly); 0x0C/0x0D client-group
  mask; **0x10 scan req / 0x11 scan rsp; 0x12 service-map req / 0x13 service-map rsp**; 0x20–0x29 parameter
  req/rsp; **0x40 RT channel message; 0x41 RT channel message + ack request; 0x44 RT graceful close;
  0x45 close + ack; 0x46 RT abort; 0x47 RT acknowledge**.
- Request/response: server copies client's sequence and group into the response. For IDN-RT the client
  increments sequence by +1 per packet; server SHOULD discard duplicates, may check order.
- Scan response: `u8 structSize; u8 protocolVersion (major<<4|minor); u8 status; u8 0; u8 unitID[16];
  u8 hostName[20]`. Status bits: 7 MALFN, 6 Offline (bootup/overheat/E-Stop), 5 XCLD, 4 OCPD (no free
  sessions), 0 RT (offers IDN-RT). UnitID = length octet, category (0x01 = EUI-48 MAC, 6 octets), id octets,
  zero padded. Host name: non-terminated UTF-8, zero padded.
- Service map response header: `u8 structSize; u8 entrySize; u8 relayCount; u8 serviceCount`, then relay table
  then service table of entries `u8 serviceID (1..255 for services); u8 serviceType (0x80 laser projector);
  u8 flags (bit0 DSID default); u8 relayNumber (0 = root); char name[20]`.
- Service types (IDN-Stream 1.2): 0x00 void, 0x04 UART, 0x05 DMX512, 0x08 MIDI, **0x80 standard laser
  projector**, 0x84 audio.
- IDN-RT semantics: channel message (0x40) with empty payload just opens/refreshes the link; message payload is
  passed to the session. Graceful close (0x44): "closed gracefully by finishing output or moving to a home
  position and closing open channels". Abort (0x46): "reset immediately by aborting output or moving to a home
  position"; explicitly "not intended to be used for safety purposes".
- **Timestamps SHALL match the point in time of sending; producers may send past their timestamps but SHALL
  not send ahead of them.** Continuous producers "may have to wait until the next timestamp is reached".
- **Link timeout: "For safety reasons, by default, the link SHALL time out after 1 second and close the
  connection. This, in turn, SHALL close the session and stop any output."** Link activity = empty packets or
  IDN-Stream void channel messages. A *session* timeout may be shorter and "SHALL stop session processing and
  data output" while keeping the link.
- IP fragmentation SHOULD be avoided for continuous streams (loss of one fragment = larger discontinuity); OK
  for discrete streams.
- Acknowledge (0x47) header: `u8 structSize; u8 resultCode (0x00 ok, 0xEB, 0xEC sessions occupied,
  0xED group excluded, 0xEE invalid payload, 0xEF other); u16 inputEventFlags; u16 pipelineEventFlags;
  u8 statusFlags; u8 linkQuality (0 unknown,1 poor..255); u32 latency_us (0 unknown)`.
  Input events include BPErr (timestamp earlier than processed: messages dropped), LAErr (late arrival,
  "a lower level device underrun may have occurred"), OVErr (more messages than can be buffered: older dropped),
  SeqErr bits (not +1 / duplicate / missing), Order (reordered silently), New (new connection).
  Pipeline events include DVIErr (device over/underrun), MCLErr (chunk below minimum size/duration),
  CTYErr (continuity: timestamp + duration != next timestamp), DCMErr (SCM/SDM mismatch), FRGErr (fragment
  reassembly failed), BSYErr (exclusive mode busy), CnlClsd, CnlRted. Status flags: SNMsg (session queue has
  messages, "latency buffer for input jitter compensation"), DOBuf (device FIFOs still hold stream data), SOCnl.

### 2.2 IDN-Stream message formats
Source: IDN-Stream rev 002 (above), sections 2.1, 2.2, 3, 6.
- Generic IDN message: `u16 totalSize; u16 contentID; data`. Content IDs 0x8000–0xFFFF are channel messages.
- **Channel message header (8 octets):** `u16 totalSize (0x0008..0xFF00); u8 CNL; u8 chunkType;
  u32 timestamp (microseconds)`; optional channel configuration follows.
  CNL: bit7 = 1, bit6 = **CCLF** (config present for whole/first-fragment chunks; *last fragment* for sequel
  chunks), bits5..0 = **channel ID (0..63)**. Implementations "SHALL support the whole range of channel
  identifiers" even if they limit how many are open at once.
- **Timestamp is 32-bit microseconds** (not 48-bit). First timestamp arbitrary; thereafter monotonic
  (equal allowed); wraps handled by consumer. "The simplest way for assigning timestamps is to take the system
  time when sending the message." With buffering/clock manipulation the timestamp "can be used for the precise
  placement of output events". Sequel fragments (0xC0–0xFF) carry `firstTimestamp + fragmentNumber` instead,
  and must be excluded from clock calculations.
- Chunk types: 0x00 Void (no data; usable with config header to open/configure/close; "can be used in keep
  alive messages to prevent the automatic close of idle channels"); **0x01 Laser Projector Wave Samples;
  0x02 Frame Samples (entire); 0x03 Frame Samples (first fragment)**; 0x10 Octet Segment; 0x11 Octet String;
  0x18 Dimmer Levels; 0x20 Audio Wave Samples; **0xC0 Frame Samples (sequel fragment)**. 0xA0–0xBF /
  0xE0–0xFF reserved for session-assigned manufacturer types, disabled by default.
- **Channel configuration header (4 octets):** `u8 SCWC (count of 32-bit service-config words that follow);
  u8 CFL; u8 serviceID (0 = default service for that mode); u8 serviceMode`.
  CFL bits: 5..4 **SDM** (service data match), bit1 **Close** (close channel after processing this message),
  bit0 **Routing** (open/route channel before processing). Opening without data puts the channel in idle until
  data arrives. "It is absolutely valid to open a channel, pass data and close the channel with a single message."
- Config cadence: "on unreliable connections ... the configuration SHALL be present every 200ms or 250ms";
  "changes SHALL not be done faster than 100ms per channel"; consumers may binary-compare configs to skip
  re-setup. DexLogic idtfPlayer resends every 200 ms, the Helios IDN client every 250 ms, the Bonn traces show
  every ~250th 1 ms message. https://raw.githubusercontent.com/DexLogic/idn-C-idtfPlayer/master/src/main.c ;
  https://raw.githubusercontent.com/Grix/helios_dac/master/sdk/cpp/idn/idn.cpp
- Service modes: 0x00 Void (consumers SHALL implement for test, ignore data), **0x01 Laser Projector Graphic
  (Continuous), 0x02 Laser Projector Graphic (Discrete)**, 0x03/0x04 Effects cont/discrete, 0x05/0x06 DMX512,
  0x0C Audio. 0xC0–0xFF manufacturer-specific, disabled by default.
- Laser projector services **SHALL be able to play back at least 100 kHz sample rate** and SHALL implement
  both continuous and discrete graphic modes.

### 2.3 Continuous ("wave") vs discrete ("frame") graphic mode
Source: IDN-Stream rev 002 sections 3.2, 3.3, 6.1, 6.2.
- **Continuous (0x01):** exclusive on the service; "an uninterrupted sequence of samples". First sample after
  opening SHOULD be at origin with all colours off. **Shutter initially closed; SHALL not open until data is
  received; SHALL close when the channel is closed.** Accepts Wave Samples chunks only.
- **Wave Samples chunk:** header `u8 flags (bits5..4 SCM); u24 duration_us`, then samples. **At least 20
  samples per chunk.** "the timestamp of the current message plus the duration must be equal to the timestamp
  of the next wave sample message on a channel" — this is the continuity rule (CTYErr if violated).
  Sample frequency = count / duration; both may be adjusted to hit an exact rate. Recommended default for
  ISP-DB25 samplers: 100 kHz, 100 samples per message → one message per millisecond.
- **Discrete (0x02):** "Frames that started to draw must finish before the next frame is processed. Newer frames
  replace older ones and in case of an overrun ... older frames are dropped. In case frames arrive slower than
  being processed, the last frame is either repeated or the scanning system is to wait for the next frame
  depending on the single scan flag." Projector inserts hidden moves between end and next start point.
  **Shutter initially open with the first discrete channel and stays open while any is open; empty frames
  SHALL not close the shutter.** "The producer SHALL not make assumptions on durations or frame repeat rates."
- **Frame Samples chunk:** header `u8 flags (bits5..4 SCM, bit0 **Once**); u24 duration_us`, then samples. First
  sample is the invisible start point; duration excludes it (DexLogic computes `(sampleCnt-1)*1e6/scanSpeed`).
  Empty array = empty frame (voids repeat buffer); otherwise >= 2 samples. **Once:** "the frame SHALL be drawn
  exactly once. When done with the frame and no further frame is scheduled, the graphic scanners SHALL be moved
  to home position (0, 0) and SHALL wait for the next frame." Frames SHOULD rely on IP fragmentation before IDN
  application-layer fragmentation (messages up to 0xFF00 octets; Bonn trace shows a 2860-byte frame message
  IP-fragmented; DexLogic `MAX_IDN_MESSAGE_LEN 0xFF00`).
  https://ilda.com/resources/Tech/IDN/Understanding-IDN_Wireshark-Traces_v2021-05-26.pdf ;
  https://raw.githubusercontent.com/DexLogic/idn-C-idtfPlayer/master/src/main.c
- **Service configuration match (SCM/SDM):** on mismatch "the consumer SHALL discard its current service
  configuration, void the channel and wait for a new configuration."

### 2.4 Descriptor tags (dictionary), IDN-Stream section 3.4
Tags are 16-bit: nibbles Category.Subcategory.Identifier.Parameter. Categories 0/1 are modifiers (no sample
octet), 4/5 describe one sample octet each, in order. Mandatory: X, Y and at least one colour. Shutter status is
not required in the stream; derive it from channel open/close. Consumers "SHALL abort" on tags of an
unspecified category/subcategory.
- `0x0000` Void (align tag array to 32 bits; parameter = words to skip)
- `0x100*` Break (next scan head), `0x11**` Space modifier (GLIN/CLIN/CBAL/CTIM 2-bit fields)
- `0x4000` NOP, **`0x4010` Precision** (adds one octet of precision to the preceding descriptor; X+0x4010 = 16-bit)
- `0x4100` Hint fmt 0 (CSCL/ISCL colour and intensity scale bits), `0x4101` Hint fmt 1 (adds SHT shutter bit 0
  in the sample: 1 = open/+5 V, 0 = closed)
- **`0x420*` X, `0x421*` Y, `0x422*` Z** (left-aligned signed; -128..127 per octet; most negative = -10 V ISP)
- **`0x5www` Colour** with 10-bit wavelength in nm: `0x527E` red 638, `0x5214` green 532, `0x51CC` blue 460,
  `0x51BD` deep blue 445 (U1), `0x5241` yellow 577 (U2), `0x51E8` cyan 488 (U3); wavelength 0 ⇒ preceded by
  `0x5C00` Wavelength Prefix. Projector resolves wavelength mismatches by nearest match / colour transform.
- `0x5C10` Intensity (legacy ISP blanking line; 0 = fully blanked), `0x5C20` Beam-Brush, `0x4201` X-prime (U4).
- Default ISP-DB25 dictionary (3.4.10): `0x4101? 0x5C10? 0x527E 0x5214 0x51CC 0x51BD 0x5241 0x51E8 0x4200
  0x4010 0x4210 0x4010 0x4201 0x4010` (optional ones marked ?).
- Default IDTF (XYRGB) dictionary (3.4.11): `0x4200 0x4010 0x4210 0x4010 0x527E 0x5214 0x51CC` (+ `0x0000`
  pad; SCWC = 4). Blanking = all colours 0. A visual-linearity tag SHOULD be added for IDTF content.
  https://www.ilda.com/resources/StandardsDocs/ILDA-IDN-Stream-rev002.pdf

### 2.5 Reference sender behaviour (DexLogic idtfPlayer, Helios IDN client)
- idtfPlayer: discrete mode; defaults **30 fps, 30000 pps**, hold single-frame files 5 s with a void message
  every 100 ms; sends one frame per period with **no send-ahead** ("paced to the frame period since the previous
  frame's timestamp"); timestamps from a monotonic µs clock; `Once` flag only with `-jf` (jitter-free);
  fragments at 0xFF00 with first/sequel/last chunk types, sequel timestamps `+1`; close = void chunk with
  CFL Close then command 0x44. https://raw.githubusercontent.com/DexLogic/idn-C-idtfPlayer/master/src/main.c
- Helios SDK IDN client (continuous mode, wave chunks): flags underrun if
  `now > frameTimestamp + sampleCnt*1e6/scanSpeed + 100 ms`; rejects scan speed 0 and < 20 samples; config every
  250 ms; `HELIOS_MAX_PPS_IDN 100000`, `HELIOS_MAX_POINTS_IDN 0x2000`.
  https://raw.githubusercontent.com/Grix/helios_dac/master/sdk/cpp/idn/idn.cpp ;
  https://raw.githubusercontent.com/Grix/helios_dac/master/sdk/cpp/HeliosDac.h
- LaserCube's IDN implementation is limited to **35,000 pps** and omits DMX/audio. https://www.laseros.com/blog/etherdream-vs-idn-network-laser/

### 2.6 What IDN says about underrun / sender vanishing
- No "hold last point" rule exists. For **continuous** mode the only normative end-state is: shutter closes when
  the channel closes; a link timeout (default 1 s) closes the session and "stop[s] any output"; graceful close
  = finish output or move to home; abort = stop immediately or move to home. Late data is reported via LAErr /
  DVIErr but the spec does not say what to output in the gap (**[unverified]** — spec silent; blanking at (0,0)
  is consistent with the "home position" language and the "first sample SHOULD be at origin, colours off" rule).
- For **discrete** mode the rule is explicit: repeat the last frame unless `Once` is set, in which case go to
  home (0,0) and wait. Empty frames void the repeat buffer without closing the shutter.
  https://www.ilda.com/resources/StandardsDocs/ILDA-IDN-Stream-rev002.pdf ;
  https://www.ilda.com/resources/StandardsDocs/IDN-Hello-2022-03-27-draft.pdf

---

## 3. ILDA IDTF (Image Data Transfer Format) rev 011, 2014-11-16

Source: https://www.ilda.com/resources/StandardsDocs/ILDA_IDTF14_rev011.pdf (mirror:
https://k1.spdns.de/Develop/Projects/pico/Laseroids/ILDA%20files/ILDA_IDTF14_rev011.pdf)

- Scope: "frame exchange purposes only ... not currently concerned with display issues such as point output
  rate. Also, the format does not include show information such as timing of frames."
- **Big-endian** throughout. Point-oriented data "directly sent to the galvanometer scanners".
- Colours are **visually linear** (127 ≈ half brightness) and colour balanced.
- File = sections (header + records); EOF = a *frame* header (format 0, 1, 4 or 5) with Number of Records = 0.
- **Header, 32 bytes (1-based byte numbers in the spec):** 1–4 "ILDA"; 5–7 reserved (0, don't test); 8 format
  code; 9–16 frame/palette name (8 ASCII, stop at NUL); 17–24 company name; 25–26 number of records (u16;
  palettes 2..256); 27–28 frame/palette number (0..65534); 29–30 total frames (1..65535; 0 for palettes);
  31 projector number; 32 reserved.
- Formats: **0** 3D indexed (8 B: X,Y,Z i16; status u8; colour index u8); **1** 2D indexed (6 B: X,Y; status;
  index); **2** palette (3 B: R,G,B); **4** 3D true colour (10 B: X,Y,Z; status; **B,G,R** order); **5** 2D true
  colour (8 B: X,Y; status; B,G,R). Format 3 never approved. Readers SHALL read all five; files may mix formats.
- Coordinates: i16, -32768 extreme left/bottom/rear, +32767 extreme right/top/front.
- **Status byte: bit 7 = Last Point (0 on all but the last point), bit 6 = Blanking (1 = laser off).** Bits 0–5
  SHALL be 0, don't test. "the blanking bit takes precedence over the color ... If the blanking bit is set, all
  RGB values SHOULD be treated as zero." All writers SHALL write bit 6 even if they blank via palette.
- Palette: a format-2 section sets the palette for its projector number for all following frames; files often
  omit it, so readers initialise a default. Appendix A "Suggested Default Color Palette" (LFI/Aura): **64**
  entries, 0 = red 255,0,0; 16 = yellow; 24 = green; 31 = cyan; 40 = blue; ... used by "most ILDA files that do
  not contain a color palette, including the ILDA test pattern".
- **Player conventions (no timing in file):**
  - j4cDAC/Ether Dream SD player: uses the current DAC pps (not from file); optional fps limit computes
    `points_per_frame = pps / max_fps` and repeats each frame `round(points_needed / npoints)` times (min 1),
    carrying the remainder; palette formats blank on `color & 0x4000`, true-colour on `flags & 0x40`; last-point
    bit ignored; blanked points → r=g=b=i=0; default `ilda_palette_64`; format 2 rejected, zero-point frame = EOF.
    https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/file/ild-player.c
  - OpenLase `playilda`: default **30000 pps**, no fps; frame duration = points / pps; resamples to the JACK
    rate; `BLANK 0x40` zeroes RGB but still moves XY; `LAST 0x80` resets position. Only formats 0/1/2.
    https://raw.githubusercontent.com/marcan/openlase/master/tools/playilda.c
  - DexLogic idtfPlayer: **30 fps default (min 5), 30000 pps**, discrete IDN frames, 5 s hold for single frames.
    https://raw.githubusercontent.com/DexLogic/idn-C-idtfPlayer/master/src/main.c
  - Community practice: "the framerate is left as an exercise for the reader"; older files assume variable frame
    rate (frame time = points/pps), newer players use a fixed fps with repeats (at 30 kpps: 30 fps ≈ 1000
    points/frame, 15 fps ≈ 2000), or a variable rate with a minimum fps.
    https://photonlexicon.com/forums/showthread.php/1884-ILDA-show-frames-and-animation
  - IDN-Stream 3.4.11: IDTF movies map naturally to IDN discrete graphic mode.

---

## 4. Flow control and buffering across DACs

### 4.1 Ether Dream (pull by fullness)
See 1.4–1.6. Buffer 1800 points (ED1); clients keep ~1600–1700 points (or 16–80 ms) queued; the DAC exposes
`buffer_fullness` in every ACK and in the 1 Hz broadcast precisely because TCP gives the client no other view
of drain progress; `low_water_mark` was meant for DAC-initiated notifications but is unused.
https://ether-dream.com/protocol.html ; https://raw.githubusercontent.com/j4cbo/j4cDAC/master/driver/libetherdream/etherdream.c

### 4.2 Helios (USB, frame swap)
https://raw.githubusercontent.com/Grix/helios_dac/master/sdk/cpp/HeliosDac.h
- `HELIOS_MAX_POINTS 0xFFF` (4095) per frame, `HELIOS_MIN_PPS 7`, `HELIOS_MAX_PPS 0xFFFF` (65535) for the USB
  model; IDN variants 100000 pps / 0x2000 points.
- Flags: `START_IMMEDIATELY` (bit0, deprecated, not on network DACs), **`SINGLE_MODE`** (bit1: play once;
  "if no new frame arrives in time, the laser turns off"; default and recommended), `DONT_BLOCK` (bit2:
  `WriteFrame` returns before the USB transfer completes).
- Without SINGLE_MODE the frame **repeats until another frame is written**.
- "Frames should be 10 milliseconds long at an absolute minimum, but 20-40ms is recommended."
- "You MUST poll [GetStatus] until it returns true, before every call to WriteFrame*()"; the first poll may
  falsely report not-ready. `Stop()` stops output until a new frame is written and blocks 100 ms. Shutter
  opens automatically on write.

### 4.3 LaserCube / LaserDock (UDP, push with free-space feedback)
https://raw.githubusercontent.com/nannou-org/lasercube/main/SPECIFICATION.md (reverse-engineered, not
official) ; https://gist.github.com/s4y/0675595c2ff5734e927d68caf652e3af
- Ports: 45456 alive/ping, **45457 command**, 45458 data (UDP).
- Commands: 0x77 GET_FULL_INFO (64-byte reply: fw version, output enabled, current DAC rate u32 @10, max DAC
  rate u32 @14, **RX buffer free u16 @19, RX buffer size u16 @21**, battery, temp, connection type, serial, IP,
  model); 0x78 ENABLE_BUFFER_SIZE_RESPONSE_ON_DATA; **0x80 SET_OUTPUT** on/off; **0x8a
  GET_RINGBUFFER_EMPTY_SAMPLE_COUNT** (reply `[0x8a, pad, free u16 LE]`); 0xa9 SAMPLE_DATA
  `[0xa9, 0x00, msg_no, frame_no, points...]`, points 10 B = x,y,r,g,b u16 LE **12-bit** (centre 0x800),
  ~140 points per packet (< 1500-byte MTU). If enabled the cube answers each data packet with `[0xa9, free_lo,
  free_hi]`.
- Ring "typically around 6000 points"; pause when free space < ~5000; estimate drain from rate x time; send
  critical commands twice; "Include safety timeouts that disable the laser if communication is lost" (device-side
  timeout not documented — [unverified]).

### 4.4 IDN (push by timestamp)
No fullness feedback in the basic stream. The receiver is expected to keep a jitter buffer (SNMsg status flag,
"latency buffer for input jitter compensation") and schedule by timestamps; the acknowledged stream reports
estimated output latency in µs and link quality. Producers SHALL NOT send ahead of their timestamps, so the
receiver's buffer depth is set by its own fixed latency, not by the sender.
https://www.ilda.com/resources/StandardsDocs/IDN-Hello-2022-03-27-draft.pdf

### 4.5 Mid-stream rate change guidance
- Ether Dream: rate changes are queued (`'q'`) and take effect at the point flagged with control bit 15, so the
  change is sample-accurate; `'u'`/`'b'` change the rate immediately for everything still queued (Modulaser:
  "A 'u' rate change re-times points already queued"). Rate 0 hangs ED2; above max is NAKed.
  https://ether-dream.com/protocol.html ; https://github.com/ModulaserApp/laser-dac-rs/pull/43
- IDN continuous: rate is implicit per chunk (count/duration); a receiver must tolerate per-chunk variation
  within the continuity rule. Config changes SHALL be >= 100 ms apart.
  https://www.ilda.com/resources/StandardsDocs/ILDA-IDN-Stream-rev002.pdf
- Helios: pps is per frame; SDK clamps to 7..65535.
  https://raw.githubusercontent.com/Grix/helios_dac/master/sdk/cpp/HeliosDac.h

---

## 5. Stop, e-stop and abrupt stream end: what each protocol expects

| Situation | Ether Dream (ED1 firmware) | IDN | Helios | LaserCube |
|---|---|---|---|---|
| Normal stop | `'s'` → Idle; all channels zeroed, shutter closed. libetherdream `etherdream_stop` "stops as soon as the current frame is finished" (client drains first). ED2 also stops on TCP close; ED1 plays out. | Graceful close (0x44) or CFL Close: "finishing output or moving to a home position"; shutter closes with last continuous channel. | `Stop()`: output off until next frame. | `SET_OUTPUT 0`. |
| Buffer underrun | ISR calls `dac_stop(UNDERFLOW)`: **Idle, outputs zero, shutter closed, playback_flags bit1**; client must Prepare+Begin again (Volst re-begins on `playback_flags == 2`; j4cDAC's own SD player re-prepares). | Continuous: not specified beyond LAErr/DVIErr reporting; discrete: repeat last frame, or `Once` → home (0,0) and wait. | SINGLE_MODE: laser turns off; else frame repeats. | not documented. |
| Sender vanishes | No keep-alive timeout in j4cDAC; buffer simply underruns (ED1 plays out then zeroes). ED2 stops on TCP close. | **Link timeout 1 s default → session closed, output stopped**; session timeout may stop output sooner. | n/a (USB host) | client-side timeouts advised only. |
| E-stop | `0x00`/`0xFF`/unknown byte → light engine E-Stop, `dac_stop(ESTOP)`; persists until `'c'`; `'c'` NAK `'!'` while cause active. Status shows cause bits. | Abort (0x46) "not intended to be used for safety purposes". Scan response `Offline` bit covers E-Stop. | none in protocol | none documented |
| Output while idle | "outputs at 0 V" (protocol page); `hw_dac_zero_all_channels()`. | Home position (0,0); continuous mode first sample SHOULD be origin, colours off. | off | off |

Sources: https://ether-dream.com/protocol.html ; https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/lib/dac.c ;
https://raw.githubusercontent.com/j4cbo/j4cDAC/master/driver/libetherdream/etherdream.h ;
https://github.com/ModulaserApp/laser-dac-rs/pull/43 ; https://raw.githubusercontent.com/Volst/laser-dac/master/packages/ether-dream/src/EtherConn.ts ;
https://www.ilda.com/resources/StandardsDocs/ILDA-IDN-Stream-rev002.pdf ; https://www.ilda.com/resources/StandardsDocs/IDN-Hello-2022-03-27-draft.pdf ;
https://raw.githubusercontent.com/Grix/helios_dac/master/sdk/cpp/HeliosDac.h ; https://raw.githubusercontent.com/nannou-org/lasercube/main/SPECIFICATION.md

Point-rate limits: ED1 100 kpps advertised (`DAC_MAX_POINT_RATE`), firmware assert at 200 kpps; IDN projector
services SHALL do >= 100 kHz; Helios USB 65535, Helios IDN 100000; LaserCube IDN 35000; ILW_MAX_PPS 200000
**[local]**.

---

## 6. Ten facts most relevant to a DAC firmware author

1. **Ether Dream clients top up to a fixed fullness, not to "whatever fits".** Reference clients target 1600–1700
   of 1799 points (or 16–80 ms), write <= 80 points per `'d'`, and estimate drain as `fullness + unacked -
   elapsed*pps` between ACKs. `buffer_fullness` must therefore count every point not yet played, be monotone
   between ACKs, and stay <= `buffer_capacity`. https://raw.githubusercontent.com/j4cbo/j4cDAC/master/driver/libetherdream/etherdream.c
2. **A NAK is treated as fatal by some clients (MadMapper reconnects on any NAK).** Prefer dropping oldest and
   clamping reported fullness over NAK-Full; never NAK for transient reasons. https://github.com/naileclevrai/TheatricalExtraLights/pull/1
3. **Real ED1 underflow = Idle + all channels zero + shutter closed + playback_flags bit1; clients then
   re-Prepare/re-Begin.** Holding the last point or auto-resuming is non-standard; auto-resume would surprise
   clients that gate `'b'` on `playback_state == 1`. https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/lib/dac.c
4. **Any unknown ED command byte is an E-stop** (and j4cDAC drops the connection). A framing slip in the TCP
   parser therefore kills output; the parser must stay byte-exact (18-byte points, 3-byte `'d'` header, 7-byte
   `'b'`/`'u'`, 5-byte `'q'`). https://ether-dream.com/protocol.html
5. **`'b'`/`'u'` with rate 0 hangs ED2; rate > advertised max is NAKed; `point_rate` in status is 0 unless
   Playing; ED2's stale fullness persists until Prepare.** Clients have built workarounds for these, so matching
   them (NAK rate 0 and > max, reset fullness on `'p'`) avoids odd client behaviour. https://github.com/ModulaserApp/laser-dac-rs/pull/43
6. **IDN link timeout is 1 s by default and SHALL stop output; void channel messages (chunk 0x00) are the
   keep-alive; empty 0x40 packets keep the link but not necessarily the session.** https://www.ilda.com/resources/StandardsDocs/IDN-Hello-2022-03-27-draft.pdf
7. **IDN producers never send ahead of their timestamps and the continuous-mode rule is
   `timestamp + duration == next timestamp`.** The receiver's latency is its own choice (a jitter buffer); the
   sender's pacing is wall-clock, so a receiver that expects pre-buffering must build it from its own fixed
   delay, and should flag CTYErr/LAErr rather than stall. https://www.ilda.com/resources/StandardsDocs/ILDA-IDN-Stream-rev002.pdf
8. **IDN shutter semantics differ by mode:** continuous = closed until first data, closed on channel close;
   discrete = open while any discrete channel is open, empty frames don't close it, `Once` frames end at home
   (0,0). Config must be accepted every 200–250 ms without glitching, and SCM/SDM mismatch voids the channel.
   https://www.ilda.com/resources/StandardsDocs/ILDA-IDN-Stream-rev002.pdf
9. **IDN wave chunks must have >= 20 samples; frame messages are typically IP-fragmented (2.8 kB+) rather than
   IDN-fragmented; channel IDs 0..63 must all be accepted; 32-bit µs timestamps wrap every ~71.6 min.**
   https://www.ilda.com/resources/StandardsDocs/ILDA-IDN-Stream-rev002.pdf ; https://ilda.com/resources/Tech/IDN/Understanding-IDN_Wireshark-Traces_v2021-05-26.pdf
10. **IDTF has no timing; the blanking bit (status bit 6) overrides colour; true-colour records are B,G,R; the
    default palette is the 64-entry LFI/Aura table; EOF is a zero-record frame header.** Players pick pps
    (commonly 30000) and either variable frame time (points/pps) or a fixed fps with repeats (30 fps common).
    https://www.ilda.com/resources/StandardsDocs/ILDA_IDTF14_rev011.pdf ; https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/file/ild-player.c

---

## 7. Mismatch risks: these protocols vs a 16,384-sample RP ring plus a small network buffer

1. **Advertised capacity vs real depth (Ether Dream).** If the broadcast advertises e.g. 1799 while the real
   path holds up to 16,384 (RP) + ESP ring, clients pacing to 1600–1700 will keep only ~55 ms queued and the
   rest of the ring stays empty — fine. But if `buffer_fullness` only reports one stage (ESP *or* RP), the client's
   drain model breaks: reporting less than the true in-flight count makes libetherdream/MadMapper overshoot and
   trigger NAK-Full (fatal for MadMapper); reporting the RP-side fill with a 1 s cached STATUS makes the
   estimate jump. Fullness must be the sum of unplayed samples across both stages and refreshed per ACK
   (extrapolated between UART reads, as `rp_poll_task` does). Advertising a larger `buffer_capacity` increases
   client-side latency on clients that target a fraction of capacity (Modulaser 80 %, nannou clamps to it).
   https://raw.githubusercontent.com/j4cbo/j4cDAC/master/driver/libetherdream/etherdream.c ;
   https://github.com/ModulaserApp/laser-dac-rs/pull/43
2. **Latency of a deep ring at low rates.** 16,384 samples is 546 ms at 30 kpps and 1.6 s at 10 kpps. Ether
   Dream clients will not fill it (they cap by fullness), but IDN and ILW have no fullness feedback: whatever
   prebuffer the firmware chooses is pure latency, and IDN producers send at wall-clock pace so the ring never
   fills beyond the start prebuffer anyway. Keep the IDN/ILW prebuffer time-based (ms), not sample-based.
3. **Underflow semantics.** ED clients expect Idle + zero + flag on underrun and will re-Prepare; an RP that
   holds the last point or keeps `playback_state == 2` after a starve will confuse them (Volst waits for
   `playback_flags == 2` to re-begin). For IDN continuous mode an underrun should blank and the channel/session
   state should follow the 1 s link timeout, not the ring's emptiness. https://raw.githubusercontent.com/Volst/laser-dac/master/packages/ether-dream/src/EtherConn.ts
4. **Rate change granularity.** ED `'q'` + control bit 15 is sample-accurate at the point where the bit is set.
   With a 512-sample SPI packet and a ring of 16,384, a rate change applied "now" on the RP will re-time up to
   16k already-queued samples (the Modulaser `'u'` complaint); applying it at the right ring position requires
   carrying the rate change with the sample stream. IDN continuous streams can change rate per chunk (>= 20
   samples). https://github.com/ModulaserApp/laser-dac-rs/pull/43
5. **Stop/e-stop latency.** ED `'s'`, e-stop, and IDN abort/link-timeout all expect output to stop promptly.
   With a 16k ring that means CLEAR on the RP (which must wait for the in-flight SPI packet),
   not draining. The spec'd ED behaviour on `'s'` is immediate Idle, not "play out" — libetherdream drains on the
   client side before sending `'s'`. https://raw.githubusercontent.com/j4cbo/j4cDAC/master/firmware/lib/dac.c
6. **TCP window vs W5500 16 KB.** ED clients send at most 80 points (1.44 kB) per write and wait for ACKs, so the
   11,520-byte window is ample for the reference clients; a client that bursts a whole frame (Volst sends
   `1799 - fullness` points at once ≈ 32 kB) will be throttled by the window, which is correct but adds RTTs.
   https://raw.githubusercontent.com/Volst/laser-dac/master/packages/ether-dream/src/EtherConn.ts
7. **IDN fragmentation and MTU.** Discrete-mode frame messages are commonly > 1500 B and rely on IP
   reassembly (Bonn trace: 2860 B). lwIP on the ESP32 must have IP reassembly enabled with enough pbufs for a
   ~0xFF00-byte message or large frames are silently lost (FRGErr/overrun from the client's view). Continuous
   streams should stay under one MTU (100 samples x 10 B + 12 B headers ≈ 1 kB in the reference dictionary).
   https://ilda.com/resources/Tech/IDN/Understanding-IDN_Wireshark-Traces_v2021-05-26.pdf
8. **IDN timestamp wrap and reorder.** 32-bit µs wraps every 71.6 min; a receiver that compares timestamps
   without wrap arithmetic will drop the stream mid-show (BPErr). Sequel fragments carry `ts + n`, not time.
   https://www.ilda.com/resources/StandardsDocs/ILDA-IDN-Stream-rev002.pdf
9. **ILW shares UDP 45457 with LaserCube's command port.** LaserOS/LaserCube discovery broadcasts will land in
   the ILW socket; the 0x49 magic check must reject them and must not count them as "activity" for ILW timeouts.
   https://raw.githubusercontent.com/nannou-org/lasercube/main/SPECIFICATION.md ; **[local]** `ilw_protocol.c`
10. **ILW has no sequence numbers or timestamps**, so packet loss or reordering (plausible on Wi-Fi) is
    undetectable and appears as a glitch or an underrun; the only defence is the receive-side timeouts
    (blank at 100 ms, disable at 500 ms). Compare IDN's 1 s link timeout and sequence-error reporting.
    **[local]** `tools/ilw_sender.py`, `ilw_protocol.c`

---

## 8. Source list

Ether Dream
- https://ether-dream.com/protocol.html
- https://ether-dream.com/
- https://github.com/j4cbo/j4cDAC (files: firmware/net/point-stream.c, firmware/net/broadcast.c, firmware/lib/dac.c,
  firmware/inc/dac.h, firmware/lib/lightengine.c, firmware/file/ild-player.c, firmware/file/playback.c,
  driver/libetherdream/etherdream.c, driver/libetherdream/etherdream.h)
- https://github.com/j4cbo/etherdream-driver (protocol.h, dac.cpp)
- https://github.com/j4cbo/j4cDAC/issues/18
- https://github.com/ModulaserApp/laser-dac-rs/pull/43
- https://github.com/naileclevrai/TheatricalExtraLights/pull/1
- https://raw.githubusercontent.com/Volst/laser-dac/master/packages/ether-dream/src/EtherConn.ts
- https://raw.githubusercontent.com/nannou-org/nannou_laser/master/src/stream/raw.rs
- https://raw.githubusercontent.com/nannou-org/ether-dream/master/dac-emulator/src/stream.rs
- https://github.com/tgreiser/etherdream
- https://docs.derivative.ca/EtherDream_CHOP
- https://forums.pangolin.com/threads/how-can-i-connect-etherdream-dac.26093/latest

IDN
- https://www.ilda.com/technical.htm
- https://www.ilda.com/resources/StandardsDocs/ILDA-IDN-Stream-rev002.pdf
- https://www.ilda.com/resources/StandardsDocs/IDN-Hello-2022-03-27-draft.pdf
- https://www.ilda.com/resources/Tech/IDN/IDN-Hello---2020-11-24-draft.pdf
- https://ilda.com/resources/Tech/IDN/Understanding-IDN_Wireshark-Traces_v2021-05-26.pdf
- https://www.ieeelcn.org/prior/LCN42/lcn42demos/1570385454.pdf (IDN-Toolbox demo paper, background only)
- https://github.com/DexLogic/idn-C-idtfPlayer (src/main.c), https://github.com/DexLogic/idn-C-helloScan,
  https://github.com/DexLogic/idn-C-serverList
- https://raw.githubusercontent.com/Grix/helios_dac/master/sdk/cpp/idn/idn-stream.h
- https://raw.githubusercontent.com/Grix/helios_dac/master/sdk/cpp/idn/idn.cpp
- https://www.laseros.com/blog/etherdream-vs-idn-network-laser/

IDTF
- https://www.ilda.com/resources/StandardsDocs/ILDA_IDTF14_rev011.pdf
- https://raw.githubusercontent.com/marcan/openlase/master/tools/playilda.c
- https://photonlexicon.com/forums/showthread.php/1884-ILDA-show-frames-and-animation

Other DACs
- https://raw.githubusercontent.com/Grix/helios_dac/master/sdk/cpp/HeliosDac.h
- https://raw.githubusercontent.com/nannou-org/lasercube/main/SPECIFICATION.md
- https://gist.github.com/s4y/0675595c2ff5734e927d68caf652e3af
