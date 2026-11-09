# IDN fixtures

Literal, spec-derived IDN-Hello / IDN-RT / IDN-Stream datagrams and expected
outcomes. They are the test oracle for the Python sender (`tests/idn/test_sender.py`)
and for the native firmware tests under `tests/idn/native/`.

## Provenance and independence

- Bytes and expected interpretations were laid out by hand from the field
  tables of **IDN-Stream Revision 002 (July 2025)** and the **IDN-Hello draft
  2022-03-27**, not produced by `tools/idn_sender.py` or the firmware. Every
  file names both documents under `sources`; every segment and fixture cites
  section and page (`"Stream 2.2 p10"`, `"Hello 6.3.3 p25"`).
- The PDFs are not in the repository (redistribution not confirmed). Nothing
  here reads them at test time.
- Malformed cases are single-feature variations: `variation_of` names the
  valid fixture and `varied` says what changed.
- `"interpretation": "normative"` means the outcome follows directly from a
  SHALL/SHOULD in the cited text. `"project"` marks this project's reading of
  something the drafts leave open; its `note` says why. Do not treat project
  outcomes as the standard. In particular, whether malformed packets get a
  silent discard or an ACK result 0xEE is plan 03's decision gate; fixtures
  only assert "no output, no session" for those.
- Passing these fixtures is not a conformance claim.

## Files

| File | Contents |
|---|---|
| `hello-management.json` | ping, scan, service map requests and responses (valid and broken), short datagram |
| `rt-ack.json` | IDN-RT acknowledgements (success, 0xEB, event flags, broken) |
| `rt-stream-valid.json` | empty RT packet, routing-only Void, first data message with the 8-octet XYRGBI dictionary, follow-on data, channel close, connection close, abort, layout variants, channel 63, SDM/SCM 0..3 |
| `rt-stream-invalid.json` | truncations, size mismatches, bad content ID, oversized SCWC, sample remainder, zero duration, 19 samples, bad/unsupported modes and chunk types, dictionary errors, unsupported features |
| `rt-sequences.json` | multi-datagram cases: sequence wrap/duplicate/gap/reorder, timestamps, close and timeout, ports, client groups, SCM mismatch, replacement config, ACK accumulation |
| `sender.json` | what the sender must emit for given parameters and input points |
| `batching.json` | tables for batch planning, per-packet capacity and cumulative timing |

## Schema (`idn-fixtures/1`)

Each file: `{schema, family, description, sources, fixtures: [...]}`. Each fixture
has `id` (unique across files), `kind`, `summary`, `refs`, `interpretation`, and
(except tables) `expect`.

- `kind: "datagram"` — one UDP payload. `direction` is `client-to-server` or
  `server-to-client`. `length` is the octet count; `segments` is the payload in
  order, one entry per field:
  `{"offset": 4, "field": "msg.total_size", "hex": "00c0", "value": "192 octets", "ref": "Stream 2.1 p7-8"}`.
  The datagram is the concatenation of all `hex` strings.
- `kind: "sequence"` — `steps`, each a datagram-like object with `label`,
  `source` (`"ip:port"`), `at_us` (receive time from the first step) and
  `segments`/`length`; `expect` describes the receiver's end state.
- `kind: "sender-sequence"` — `sender` holds the parameters and input
  (`input_points` as `[x, y, r16, g16, b16, i16]`, or `input_blank_count`);
  `packets` lists the expected datagrams in order, either `{"ref": <datagram id>}`
  or literal header `segments` with the full `datagram_length` (sample octets
  covered elsewhere). `send_at_s` is the earliest legal send time from the
  stream origin.
- `kind: "table"` — `cases` of plain inputs and expected values.

`expect.outcome` vocabulary: `accept`, `respond`, `discard` (malformed: silent,
no session, no timeout refresh), `reject` (recognized but refused; no output),
`unsupported` (valid per spec, not implemented; no output — `if_implemented`
says what changes once it is), `ignore`, `parse_ok` / `parse_error` (client-side
response parsing, with `reason`), `match` / `match-prefix` (sender output).
Decoded sample expectations use signed 16-bit `x`/`y` and raw 8-bit color octets
unless the field name says otherwise.

`tools/test_idn.py` checks all of this before any test runs: schema keys, hex
syntax, contiguous offsets, literal lengths, unique ids, references, and a
`note` on every project interpretation.

## Using fixtures from native (C) tests

`tools/test_idn.py` writes `idn_fixtures.h` into `<build>/generated/` and
passes these cache variables to every `tests/idn/native/<name>/CMakeLists.txt`:

| Variable | Value |
|---|---|
| `IDN_FIXTURE_HEADER_DIR` | directory containing the generated `idn_fixtures.h` |
| `IDN_FIXTURES_DIR` | this directory (for tests that read the JSON themselves) |
| `IDN_REPO_ROOT` | repository root (to reach `firmware/ESP32S3/main/*.c`) |

The header has one array per datagram and per sequence step, named from the
fixture id with `-` replaced by `_` (sequence steps get `__<n>`):

```c
#include "idn_fixtures.h"   /* target_include_directories(t PRIVATE ${IDN_FIXTURE_HEADER_DIR}) */

decode(idn_fx_rt_first_config_wave_xyrgbi_20, IDN_FX_RT_FIRST_CONFIG_WAVE_XYRGBI_20_LEN);
for (size_t i = 0; i < IDN_FIXTURE_COUNT; i++)          /* name, family, outcome, data, len */
    if (!strcmp(idn_fixtures[i].outcome, "discard")) expect_discard(idn_fixtures[i].data, idn_fixtures[i].len);
```

Expected values beyond `outcome` stay in the JSON; mirror the ones a test
asserts as literals next to the test, citing the fixture id. To generate the
header without running anything else:
`python tools/test_idn.py --emit-c-header path/to/idn_fixtures.h`.
A native target that needs the header should fail clearly when run some other way:
`if(NOT IDN_FIXTURE_HEADER_DIR) message(FATAL_ERROR "configure via tools/test_idn.py") endif()`.
