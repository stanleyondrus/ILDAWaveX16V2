# IDN plan 01: Safe decoding and output semantics

Status: software increments 1-3 and the rejection half of increment 4 implemented on branch `idn/01-safe-decoding` and host-tested; lifecycle shutter work, plan 02/03 integration and all firmware builds and hardware validation remain. No conformance is claimed.

## Implementation status (branch idn/01-safe-decoding)

Implemented:

- Pure-C decoder `firmware/ESP32S3/main/idn_decoder.c/.h`: transactional layout compilation with typed results (malformed / unsupported / invalid config / invalid samples / resource, plus reason and offending tag), whole-chunk validation, and sample decoding. The header records every policy choice below.
- `idn_protocol.c`: default layout, auto-open and SCWC shortening removed. A truncated configuration invalidates the channel's decoder; a rejected replacement leaves the channel without a decoder; wave chunks are checked as a whole before rate update or enqueue, and are queued whole or dropped (counted).
- Unassigned outputs (U1/U2, and I when no intensity line) are `INT16_MIN` (raw DAC 0); previously U1/U2 were signed 0 (mid-scale).

Policy decisions:

- Unknown identifiers in descriptor categories 4/5 consume one octet (plus any precision octets) and are ignored; unknown identifiers in modifier categories 0/1 are rejected. X/Y with nonzero parameter, Z and Beam-Brush are consumed and ignored.
- Intensity-only layouts are not accepted (X, Y and one colour are mandatory).
- Colour lines: 638/532/460 nm on R/G/B (ISP-DB25 defaults, 3.4.10); nearest match, tie to the shorter wavelength; two tags on one line are rejected (no combining policy). U1/U2 are not offered as colour lines, so the full ISP-DB25 dictionary with optional U1-U3 colours is rejected.
- Absent intensity = largest decoded colour (after CSCL). ISCL applies only to explicit intensity. Scales truncate (right shift by 2 x scale).
- Space Modifier: only "projector specific" and "no transformation" accepted for each field; reserved values invalid.
- Hint reserved low bits are ignored, not rejected.
- Limits: dictionary 1020 bytes, stride 32 octets, 4 octets per value.

Shutter gate: not implemented. V2 GPIO40 polarity comes from schematic reading only and has not been measured, so no V2 shutter control was added. V1 keeps its existing open-on-start / close-on-stop behaviour on GPIO8 (also unmeasured). `0x4101` is rejected.

Tests: `tests/idn/native/decoder/` (CTest target `idn_decoder`) compiles the real module with `-Wall -Wextra -Werror` and covers the acceptance table at the decoder level. The DAC-code check mirrors the RP/V1 `signed_to_dac()` formula; those static functions are not compiled by the test. "Old decoder not reused" is tested at the decoder contract; the `idn_protocol.c` integration was only syntax-checked against host stubs.

Not done: ESP V1/V2 and RP2354 builds (toolchains unavailable), hardware checks, lifecycle shutter on both boards, plan 02 channel states/SDM, plan 03 event mapping of decoder results, plan 04 fixtures (tests here embed their own spec-derived bytes).

## Purpose and scope

Make malformed, unsupported, or misinterpreted IDN data incapable of silently becoming a different laser waveform. Establish a bounded, testable decoder and correct optical-control semantics before expanding service support.

This is the first of four linked plans:

1. Safe decoding: this document.
2. [Protocol state and ownership](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/02-protocol-state.md).
3. [Timing and diagnostics](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/03-timing-and-diagnostics.md).
4. [Independent fixtures and compliant sender](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/plans/idn/04-fixtures-and-sender.md).

Start the fixture work in plan 04 alongside this plan, not after firmware changes. Plan 01 defines decoder results; plan 02 owns channel transitions; plan 03 maps results into protocol diagnostics. None of these plans alone establishes full standard-laser-projector conformance: discrete graphics and effects remain a later workstream.

## Evidence and requirements

Primary sources:

- [IDN-Stream rev002](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/ILDA-IDN-Stream-rev002.pdf).
- [IDN-Hello draft](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/IDN-Hello-2022-03-27-draft.pdf).
- [Hardware research, especially section 1.5](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/docs/research/hardware.md).

| Requirement | Source | Current failure |
|---|---|---|
| Configuration declares every sample octet | Stream 2.2, 3.4, 6.1 | Private eight-byte fallback replaces absent/bad dictionaries |
| Unknown category/subcategory combinations abort decoding | Stream 3.4 | Unknown tags are silently skipped |
| Precision extends the preceding descriptor, most significant octet first | Stream 3.4.5 | Third coordinate octet overwrites the second |
| Static color tags encode wavelength in nm | Stream 3.4.8, 3.4.10 | Firmware compares against nm/4 ranges |
| Hint controls shutter and separate color/intensity scales | Stream 3.4.6 | Hint bytes are ignored |
| Explicit intensity is a separate legacy signal | Stream 3.4.9 | Explicit zero is replaced by maximum RGB |
| Unknown payload structure must not be guessed | Stream 3.4; Hello 6.3.3 | Failed parsing installs a different decoder |
| Wave chunks contain at least 20 whole samples | Stream 6.1 | Short chunks and trailing sample fractions are accepted |

Conformance language matters: distinguish SHALL requirements above from implementation limits and project safety policy below. Unsupported physical features can justify rejecting or deliberately adapting a stream; they do not justify miscounting its sample stride.

## Files and responsibility boundaries

Existing files to change during implementation:

- [IDN receiver](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/idn_protocol.c): remove implicit layouts; integrate bounded parsing and decoder results.
- [ESP component build](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/CMakeLists.txt): register extracted modules.
- [Output interface](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/rp_manager.h), [V2 manager](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/rp_manager.c), and [V1 manager](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/rp_manager_v1.c): board-specific shutter control and ownership-aware output integration.
- [Board configuration](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/app_config.h): explicit board-specific shutter pin/polarity once verified.
- [RP output engine](C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/RP2354/src/output_engine.c): preserve final blanking after color-delay processing if sample-level gating is implemented there.

Proposed new pure-C module: `C:/Users/zubair/Documents/GitHub/ILDAWaveX16V2/firmware/ESP32S3/main/idn_decoder.c` and matching header. It must not depend on sockets, FreeRTOS, clocks, or a live DAC. Exact exported names are implementation details, but the following contracts are required.

## Decoder contracts

### Transactional layout compilation

Input: bounded dictionary bytes, declared length, and explicit hardware capabilities. Output: either an immutable valid layout or a structured error. Do not mutate the active layout while parsing.

The compiled layout must describe:

- Exact sample stride, including ignored but structurally known descriptors.
- Descriptor kind, byte offset, precision width, and scan-head association.
- Presence of intensity and hint descriptors independently of sample values.
- Static wavelengths or validated dynamic-prefix relationships.
- Space-transform requirements and whether the target supports them.
- Checked resource requirements and a deterministic rejection reason.

No parser failure installs a default. No successful result may refer beyond the supplied bytes. No configuration is published until every tag, suffix, descriptor relationship, and resource limit has been checked.

### Bounded parsing rules

1. Validate outer message/configuration lengths before accessing the dictionary; plan 02 owns transport/session mutation ordering.
2. Require the complete declared SCWC region. Never shorten it to fit the received data.
3. Validate suffix counts before advancing. Use checked arithmetic for offset, count, and stride calculations.
4. Recognize category/subcategory combinations explicitly. Reserved or undefined combinations terminate compilation.
5. For a structurally known but unknown descriptor identifier, either consume its defined octet and deliberately ignore it or reject it. Record the choice; never accidentally shorten the stride.
6. Recognize NOP and valid Void padding. Do not treat all category-0 tags as Void.
7. Require standard-head X/Y and a valid draw-color configuration. Decide explicitly whether an intensity-only legacy compatibility profile is supported; do not silently relax the standard mandatory-color rule.
8. Reject duplicate/conflicting standard-head assignments unless an explicitly documented operation combines them.
9. Enforce limits on dictionary size, descriptor count, stride, and precision. Exceeding a limit rejects the entire configuration, not its tail.
10. Reject invalid precision placement, unsupported hint formats, and invalid wavelength-prefix relationships.

For a single-head device, initially reject Break/multi-head configurations with an unsupported-feature result. This is simpler and safer than ignoring the remainder while still accounting for its layout; it avoids later heads overwriting the first.

### Numeric conversion and color mapping

- Assemble signed coordinates using unsigned operations and explicit sign conversion. Avoid shifting negative signed integers.
- For coordinates wider than 16 bits, retain the most significant 16 bits with a documented truncation policy. For eight-bit coordinates, append low zero bits.
- Preserve supported 16-bit color/intensity precision. Convert the unsigned optical value into the existing offset signed representation only at the output boundary.
- For eight-bit optical values, document the left-aligned conversion dictated by the descriptor model. Do not casually change `value << 8` into full-range replication while fixing unrelated errors.
- Interpret `0x527E`, `0x5214`, and `0x51CC` as 638, 532, and 460 nm. Select the nearest configured available output wavelength; never assign colors according to descriptor encounter order.
- Define ties and multiple input lines mapping to the same physical output. If no tested color-combination policy exists, reject that conflicting layout rather than allowing last-descriptor-wins behavior.
- Either implement dynamic wavelength prefixes correctly or reject layouts using them. Do not infer their color from a zero static wavelength.
- Recognize space modifiers. Honor supported settings and reject incompatible requested transformations. Projector-specific/default settings must have a documented meaning.

### Intensity, auxiliary outputs, and hint scales

An explicitly supplied intensity value always wins, including zero. Only absent intensity may use a documented compatibility derivation from RGB. Intensity remains separate: setting it to zero does not, by itself, redefine every RGB sample value.

Default unassigned optical/auxiliary outputs to `INT16_MIN`, which converts to raw DAC zero. Preserve signed zero for X/Y origin. Test the actual downstream DAC-code conversion so a test cannot confuse signed zero with zero volts.

Apply CSCL to color lines and ISCL to the separate intensity line: division by 1, 4, 16, or 64. Define rounding once, use sufficiently wide intermediates, and apply each scale exactly once. Scaling precedes final output conversion/gating.

## Shutter implementation gate

The hardware research identifies V2 ShutterL as ESP32 GPIO40 with inverted electrical behavior: driving high closes the shutter; a floating/low line allows it to open. V1 currently uses GPIO8 with different control assumptions. Verify both against the board revision and with electrical measurements before assigning implementation-complete status.

Required lifecycle behavior:

- Initialize the controllable shutter closed as early as practical, before accepting producer traffic.
- Continuous-mode routing without samples leaves it closed.
- Only validated, authorized output can open it.
- Channel close, owner cancellation, timeout, abort, and output failure close it.
- Never claim software eliminates the V2 floating-pin boot/reset interval. Record any hardware pull-network change needed for a truly fail-closed reset state as separate hardware work.

Sample-level `0x4101` is not solved by toggling an ESP GPIO when its UDP packet arrives. The RP can be playing buffered older samples. The current eight-channel SPI payload does not carry synchronized shutter metadata, and the physical V2 shutter is on the ESP.

Implementation decision gate:

1. Establish immediate lifecycle shutter control on each board.
2. For the first strict continuous release, reject sample-controlled-shutter layouts unless a synchronized, measured implementation exists.
3. If implementing sample control, specify metadata transport, output-time scheduling, physical signal behavior, and final optical gating after color delay. RGB gating alone is not proof that the physical shutter signal follows the descriptor.
4. Do not accept-and-ignore `0x4101` as a compatibility shortcut. Advertise/document the remaining feature limitation and report it through plan 03.

This bounded initial rejection is a safe subset policy, not a claim to implement every standard tag.

## Chunk validation and failure behavior

Before writing any decoded samples, verify header presence, nonzero usable duration, exact sample-stride divisibility, minimum sample count, and all sample-dependent supported-feature constraints. Plan 03 decides rate acceptance and continuity; plan 02 decides routing and SCM/SDM acceptance.

If a sample-dependent failure could otherwise occur halfway through a chunk, validate first or decode into bounded staging storage and commit transactionally. Do not publish a partially interpreted chunk. Resource bounds must remain explicit for large datagrams.

Decoder results must distinguish malformed structure, unsupported feature, invalid configuration, invalid sample payload, and resource exhaustion. Return results to the session owner; do not change global channel state from inside the pure decoder. A rejected replacement configuration must not permit subsequent data to fall back to the old interpretation.

## Implementation increments

1. Extract the current pure parsing/conversion code behind the contract, with spec-authored fixtures available from plan 04.
2. Remove fallback/auto-layout behavior at the integration boundary; reject invalid dictionary lengths and categories.
3. Correct descriptor relationships, wavelength mapping, precision, intensity presence, scales, and unassigned outputs.
4. Add board lifecycle shutter handling; explicitly reject unsupported sample-shutter layouts until the gate above is satisfied.
5. Integrate typed results with plan 02 transitions and plan 03 event recording.
6. Update IDN capability comments and documentation. Do not change unrelated Ether Dream or ILW wire decoding as incidental cleanup.

Every increment must build ESP V1, ESP V2, and RP2354. Interface changes must include both manager implementations in the same increment.

## Focused acceptance cases

| Input | Expected result |
|---|---|
| Green-only standard wavelength with X/Y | Green output, not red |
| Same physical colors described in RGB versus BGR order | Same decoded physical output |
| Explicit intensity zero with nonzero RGB | Intensity remains raw zero |
| Absent intensity | Exactly the documented derivation policy |
| All declared optical signals zero; U1/U2 absent | Every optical/auxiliary DAC code is zero |
| X encoded as `12 34 56` with two precision tags | 16-bit X equals `0x1234`, not `0x1256` |
| Scale values 0, 1, 2, 3 | Independent color/intensity division by 1, 4, 16, 64 |
| Unknown category, truncated suffix, excessive stride | Whole configuration rejected; no output |
| Break introducing a second head | Unsupported result; no first/second-head mixture |
| Sample-shutter layout before synchronized support exists | Rejected, never silently played |
| Malformed final sample or fewer than 20 samples | Whole chunk rejected before enqueue |
| Good layout followed by rejected replacement | Old decoder not reused for new-format data |

Use table-driven pure-C tests with literal expected output. Hardware checks must separately measure shutter polarity, stop behavior, auxiliary voltages, and color-delay interaction, initially with the laser disabled.

## Completion checklist

- [x] No undeclared default layout remains in the IDN receive path. (host-tested decoder; integration syntax-checked only)
- [x] No unsupported layout can silently shorten sample stride or overwrite another head. (host-tested)
- [ ] Explicit blanking-related values survive decoding and downstream conversion. (decoding host-tested; downstream conversion mirrored, not measured)
- [ ] Lifecycle shutter behavior is implemented and measured on each supported board. (gated: polarity unmeasured)
- [x] Sample-controlled shutter support or explicit rejection is documented accurately. (rejected)
- [ ] Typed failures are observable without changing protocol state from the decoder. (typed results returned and logged; plan 03 event mapping pending)
- [ ] Independent fixtures, all-target builds, and controlled hardware results are recorded.
- [x] No full-conformance claim is made while unsupported modes/tags remain.
