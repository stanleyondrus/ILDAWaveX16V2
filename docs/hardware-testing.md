# Hardware test protocol

How changes to output, timing or protocol code are tested on a board. It is a loop: each step narrows what the next one looks for. The lost-SPI-packet fix (F16 in `docs/galvo-safety-review.md`) was found and proved this way. A camera showed lit chords, the counters showed lost packets, a position log tied them to packet boundaries, a 20-run soak proved the fix, and the owner confirmed it with a real client.

## Safety first

- Run tests with the laser off or the beam on a backstop. `tools/hwtest.py` drives the laser.
- Flash only while output is stopped: `remote_running` is `false` in `GET /api/rp/status`. Some clients keep a session open after "stop" and keep sending blanked points, so check that the per-protocol sample counters (`stream_etherdream_samples`, `stream_idn_samples`) have stopped rising too.
- Set the scanner rate limit to the galvos' rating before testing a slower set. Several tests ask for 30–50 kpps.

## The loop

1. **Build every target.** `python3 tools/build_all.py` builds ESP32-S3 V1, ESP32-S3 V2 and RP2354 from a scratch copy and fails on errors or warnings. Before a pull request, build each commit on its own (for example in a `git worktree`), so no commit breaks a target.
2. **Flash** both chips, with output stopped (see above). Log both consoles while you work. `tools/serial_logger.py` opens the ESP32-S3 port without resetting it and reconnects on its own.
3. **Instrument before fixing.** If a symptom has no counter, add one first, flash, and reproduce. Change behaviour only once the numbers show the mechanism. Existing instruments:
   - `/api/rp/status`: drop counters per protocol, `remote_seq_gaps`, `remote_rx_partial`, `payload_*` send failures, and the step counters (`remote_max_lit_step`, `remote_lit_big_steps`, `remote_blank_big_steps`). `POST /api/scan/refresh` first forces a fresh read from the RP.
   - The RP2354 USB console: once a second, the output mode, underruns and fill; a `lit_jump` line for each lit step of 1/8 full scale or more, with its position in the 256-sample stream packet.
4. **Measure with the tools.**
   - `tools/hwtest.py --host <board> all` runs 13 behaviour tests. Each refuses to start while anything is outputting, and appends JSON lines to `logs/`.
   - `tools/stream_stats.py` prints points/s per protocol, underruns/s and RP fill from the status API.
   - `tools/ild_inspect.py` reports the step inputs inside `.ild` files.
5. **Watch.** A camera on the backstop records what the beam actually draws. Grab frames when something looks wrong, and compare them with the counters for the same seconds. Some faults (a lit streak during a blanked jump) show on camera before any counter moves.
6. **Repeat.** A fix to a timing race is not proven by one pass. Run the relevant tests about 20 times and report the pass count and the counters per run.
7. **Real client, real eyes, last.** Run the client people actually use (for example Modulaser) over each protocol, with the laser on, while logging the counters. Commit once that run is clean. Use one commit per fix, with the measured before and after in the commit message.

Keep the raw tables, console logs and recordings in `logs/` (git ignores it). The commit messages and docs quote the numbers.
