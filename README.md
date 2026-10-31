# ILDAWaveX16 V2
[![](https://stanleyprojects.com/projects/ildawavex16v2/thumbs/cover-text.jpg)](https://stanleyprojects.com/projects/ildawavex16v2)

**ILDAWaveX16 V2** is an all-in-one, dual-processor, high-resolution laser DAC platform built for makers and laser enthusiasts.

It combines **ILDA DB25 output**, **SD-card playback**, **Ethernet**, **Wi-Fi**, **USB**, **JST XH connectors**, and a flexible firmware architecture designed for experimenting with laser protocols and custom workflows.

The board is built around two processors:

- **ESP32-S3** - networking, web UI, SD-card playback, protocol parsing, configuration
- **RP2354** - real-time buffering, DAC timing, deterministic laser output

For full project documentation, visit: https://stanleyprojects.com/projects/ildawavex16v2

> [!WARNING]
> ILDAWaveX16 is a maker-friendly laser DAC platform, not a finished commercial laser product. The firmware is still under development and may contain bugs, so you should be comfortable flashing firmware, modifying code, and troubleshooting embedded hardware and software issues.

---

## Repository Contents

The firmware is split into separate projects:

```text
ILDAWaveX16/
├── firmware/
│   ├── ESP32S3/      # ESP32-S3 FW: networking, web UI, SD, protocols
│   └── RP2354/       # RP2354 FW: real-time DAC output engine
├── hardware/         # schematic, 3D model
├── docker/           # ESP and RP builder images
├── docs/             # build guides and hardware notes
├── tools/            # build, cleanup, and protocol-testing scripts
└── README.md
```

> [!NOTE]
> The ESP32-S3 firmware is also compatible with the original **ILDAWaveX16 V1** board. To build for V1, use `./tools/build-firmware.sh esp32 --board-version 1` or set `BOARD_VERSION` to `1` in `firmware/ESP32S3/main/CMakeLists.txt` for an IDE build.
> Performance on V1 is currently more limited than on V2, so please treat V1 support as experimental.
> V1 has the scanner rate limit but no colour shift, and its test patterns do not include Colour Shift.
> `python3 tools/build_all.py` builds the ESP32-S3 firmware for V1 and V2 and the RP2354 firmware from a scratch copy of the tree, so a change that breaks the V1 build shows up without a V1 board.

---

## Firmware Development Setup

### Docker builds

The Docker builders keep ESP-IDF, the Pico SDK, and both toolchains inside containers. With Docker installed, run these Bash commands from the repository root:

```bash
./tools/build-firmware.sh esp32   # ESP32-S3 firmware for V2
./tools/build-firmware.sh rp2354  # RP2354 firmware
```

Each command builds its image and firmware, then exports to `dist/esp32-v2/artifacts/` or `dist/rp2354/artifacts/`. Repeating a command replaces the previous artifacts only after a successful build. Use `./tools/clean-build.sh all` to remove generated firmware artifacts.

See the [ESP build guide](docs/building.md) and [RP build guide](docs/building-rp2354.md) for requirements, V1 builds, Windows PowerShell commands, caching, and artifact details. Browser flashing instructions are in the [ESP](#flashing-esp32-s3) and [RP](#flashing-rp2354) sections below.

### IDE builds

For development with host-installed SDKs:

- [Visual Studio Code](https://code.visualstudio.com/)
- [ESP-IDF Extension for VS Code](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension) for the ESP32-S3 firmware
- [Raspberry Pi Pico Extension for VS Code](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico) for the RP2354 firmware

The ESP32-S3 firmware is built with ESP-IDF.

The RP2354 firmware is built with the Raspberry Pi Pico SDK and produces a `.uf2` firmware file.

---

## Firmware Architecture

ILDAWaveX16 V2 uses a split architecture to keep the user-facing and real-time parts of the system separated.

The **ESP32-S3** handles:

- Web interface
- REST API
- Ethernet / Wi-Fi configuration
- microSD card access
- ILDA file playback
- Protocol parsing
- Test-pattern generation
- Stream conversion and packet forwarding to RP2354
- Runtime configuration and diagnostics

The **RP2354** handles:

- Real-time DAC sample output
- SPI packet receiving with PIO
- DMA-based payload handling
- Ring buffer management
- READY hardware flow control
- X/Y mirroring
- Underrun handling
- Safe blanking and hard-zero stop behavior

Incoming data from SD files, test patterns, or network protocols is converted on the ESP32-S3 into a unified internal sample format.

Each laser point is represented as an 8-channel signed 16-bit sample:

```text
X, Y, R, G, B, I, US1, US2
```

---

## Flashing Firmware

Both microcontrollers are connected to the same USB-C connector.

Depending on the USB-C cable orientation, the port connects either to the **ESP32-S3** or to the **RP2354**.

If the expected device does not appear on your computer, unplug the USB-C cable, rotate it 180 degrees, and plug it in again.

Each microcontroller must be flashed separately.

---

### Flashing ESP32-S3

If building from source, use the [ESP Docker builder](docs/building.md) or the ESP-IDF extension in VS Code.

Select the board version using the Docker builder's `--board-version` option or in `firmware/ESP32S3/main/CMakeLists.txt` for an IDE build.

**Released firmware:** Use the project's [release web installer](https://stanleyprojects.com/projects/ildawavex16v2/install/index.html), which provides preset firmware images.

**Local builds:** Use Espressif's [ESP Web Tool](https://espressif.github.io/esptool-js/) in Chrome or Edge:

1. Connect to the board's ESP USB connection, close other serial terminals, and connect to its serial port in the browser.
2. Open `flasher_args.json` from `dist/esp32-v2/artifacts/` (or `dist/esp32-v1/artifacts/` for V1).
3. Add a file row for every entry in `flash_files`, selecting the corresponding local binary and entering its hexadecimal address. Paths are relative to the artifact directory; the JSON itself is not a firmware file to upload.
4. Match the tool's flash mode, frequency, and size to `flash_settings`, then start programming and wait for completion. Reset the ESP if it does not restart automatically.

The current layout uses `bootloader/bootloader.bin` at `0x0`, `partition_table/partition-table.bin` at `0x8000`, and `ILDAWaveX16V2-ESP32S3.bin` at `0x10000`. Always use the values from your build's JSON rather than assuming these offsets. The application binary alone is not a complete factory image. A full-chip erase is not required by this procedure and would remove stored settings.

The ESP32-S3 should normally enter bootloader mode automatically. If flashing does not start:

1. Hold `ESP BOOT`
2. Briefly press and release `ESP RST`
3. Release `ESP BOOT` and try flashing again

---

### Flashing RP2354

Use a released `.uf2` or build with the [RP Docker builder](docs/building-rp2354.md) or Raspberry Pi Pico extension in VS Code. Docker builds export `dist/rp2354/artifacts/ILDAWaveX16V2-RP2350.uf2`.

Connect the board's RP USB connection to your computer with a data-capable cable, then enter bootloader mode:

1. Hold `RP BOOT`
2. Briefly press and release `RP RST` while holding `RP BOOT`
3. Release `RP BOOT`

The RP2354 should appear as a USB storage drive. Choose either flashing method:

**Browser:** Open the third-party [pico⚡flash](https://picoflash.org/) in Chrome or Edge. Click **Connect**, select the RP bootloader device, then select the `.uf2` in the **Flash** tab and click **Flash Device**. Keep USB connected until it reports success. If needed, click **Reboot** or press `RP RST` with `RP BOOT` released. See the tool's [platform requirements](https://github.com/piersfinlayson/picoflash).

**Mounted drive:** Copy the `.uf2` onto the RP storage drive. It should automatically unmount and reboot into the new firmware.

If WebUSB cannot connect, close other USB tools and re-enter the bootloader, or use the mounted-drive method. Use the original UF2 file; no manual address or binary conversion is needed.

After flashing, refresh the ESP web interface and check the RP connection and firmware version. Builds with the same version cannot be distinguished by that version alone.

For firmware that provides USB logs, open the [Web Serial terminal](https://googlechromelabs.github.io/serial-terminal/) in Chrome or Edge, select the RP serial port, and connect at 115200 baud with hardware flow control disabled. The RP must be running normally, with the flasher disconnected.

---

## Current Firmware Support

### SD Card Playback

The ESP32-S3 can play `.ild` files directly from microSD.

ILDA frames are parsed, converted into X/Y/color/intensity samples, and streamed to the RP2354 through the same SPI payload path used by live protocols.

The current implementation loads converted frames into PSRAM-backed memory, which makes playback fast and stable, but limits the maximum file size that can be played. The current limit is 89,600 points in total across all frames.

Before loading, the player scans the whole file to count its points. Files that exceed the limit, use an unsupported format, or are truncated are rejected before the current playback is stopped, so a bad file never interrupts output. Supported formats are 0, 1, 4 and 5. Some exporters write the closing header with the last frame's point count instead of zero; a header with no data after it is accepted as the end of the file.

A future improvement would be continuous batch reading, where only partial chunks of the ILDA file are read, converted, and streamed at a time. This would allow larger files to be played without loading the full converted output into memory.

The **SD playback** page in the web interface can:

- Browse folders on the card
- Preview an `.ild` file before playing it: frame count, point counts, formats, and whether it fits within the player limit
- Animate the preview at the current DAC point rate, with up to 256 points drawn per frame and up to 1024 frames
- Scale the projected X/Y size from 10% to 100% around the center, live during playback
- Play every `.ild` file in a folder, in name order, repeating until stopped. The operator sets how long single-frame files are held (default 5 s), how many times each animation plays (default 1), and whether subfolders are included. Files that fail the checks are skipped, and the output is blanked while the next file loads.

The same features are available through the REST API:

| Endpoint | Method | Description |
| --- | --- | --- |
| `/api/sd/list?path=<folder>` | GET | List a folder, relative to the card root |
| `/api/ilda/preview?path=<file>&frame=<n>` | GET | File summary plus one decimated frame |
| `/api/ilda/preview?path=<file>&all=1` | GET | File summary plus every frame, streamed |
| `/api/ilda/start?path=<file>&scale=<10-100>` | POST | Start playback at the given X/Y scale |
| `/api/ilda/scale?value=<10-100>` | POST | Change X/Y scale while playing |
| `/api/ilda/play_folder?path=<folder>&hold=<1-3600>&plays=<1-1000>&recursive=<0/1>&scale=<10-100>` | POST | Play a folder (hold in seconds) |
| `/api/ilda/stop` | POST | Stop playback |

Previews are served by a background task, so status and stop requests are never delayed by a large preview.

`/api/rp/status` reports the file being played in `ilda_file` (relative to the card root) and whether folder playback is active in `ilda_folder`.

---

### IDN / ILDA Digital Network

The firmware includes an experimental IDN / ILDA Digital Network implementation over UDP.

It supports IDN-Hello discovery, allowing compatible software to find the device on the network and identify it as an IDN-capable laser DAC.

Once connected, incoming IDN real-time stream data is parsed, converted into the internal 8-channel sample format, and forwarded to the RP2354 output engine.

If the stream pauses between packets, the scanners hold their last position with all color and intensity channels blanked, so the last colored point is never repeated.

IDN has no flow control: the sender streams at its own pace. The board queues 3,072 points (about 100 ms at 30 kpps) on the RP2354 before output starts, and that queue absorbs gaps in the sender's stream. A gap longer than the queue shows up as blanking. `IDN_FEED_PREBUFFER` in `ESP32S3/main/idn_protocol.c` trades latency against tolerance for sender hiccups.

Tested with Modulaser at 30 kpps over wired Ethernet: no dropped packets, and output keeps pace with the stream.

---

### Ether Dream

The firmware includes an Ether Dream compatible DAC server, allowing standard laser software to discover and stream point data to the board.

Implemented features include:

- UDP discovery
- TCP control
- TCP streaming
- Point-rate configuration, including rate changes during playback (`u`)
- Prepare / start / stop behavior
- Ether Dream point conversion into the internal 8-channel sample format

The client paces itself from the buffer fullness the board reports. The board keeps only a short cushion (3,072 points) on the RP2354 and holds the rest in its 8,192-point Ether Dream buffer, so the reported fullness covers everything not yet played.

How much is queued is the client's choice. If output blanks briefly while the client is busy, raise the client's buffer or latency setting for the DAC.

Tested with Modulaser over wired Ethernet: about 21,700 points/s get through at 22 kpps and about 29,500 at 30 kpps.

---

### Scanner Protection and Tuning

The board treats the scanners as the fragile part. From RP2354 firmware 0.2.0 and ESP32-S3 firmware 0.2.0:

- **Parked idle.** When stopped, X and Y sit at the scanner centre (0 V on the ILDA differential pairs) with every laser channel at 0 V. Earlier firmware parked at DAC code zero, which is -10 V differential, a full-scale corner.
- **Soft moves.** `STOP` ramps the scanners from their last position to the centre over 10 ms with the laser blanked; `START` ramps from the centre to the first point of the stream the same way, then plays the stream unchanged. No client sees retimed points; the stream starts 10 ms later.
- **Scanner rate limit.** Every requested rate, from any protocol or the web page, is clamped to a saved limit. The default is 30,000 pps, which any ILDA 30K scanner can do at the test-pattern angle; set it to your scanners' rating at the angle you use. The web page's **Scanner rating** menu fills in 12K, 20K, 30K, 40K or 50K; press **Save limit** to apply it. These ratings are at 8° optical, so use a lower limit for wider scans. The limit is saved on the ESP32-S3 and on the RP2354, which enforces it on its own. A new limit applies at once, also to a stream already playing: a lower one slows it, a higher one lets a clamped stream speed back up to the rate it asked for. Ether Dream clients are told the applied rate and see the limit as `max_point_rate`. The RP2354 writes the limit and the mirror settings to its flash only while output is stopped (a flash write pauses its output core); a change made while playing is written at the next stop.
- **Colour shift.** A colour/blanking delay in microseconds, applied on the RP2354 after the buffer, so it covers every source including SD playback. Positive values delay the colour channels behind the scanner position. It is applied in whole samples (one sample is 33 us at 30 kpps). To set it, press **Show pattern** on the Output Mapping card, which runs the **Colour Shift** test pattern: red, green and blue columns, each with two short white lines drawn left to right, one ending at the column above and one starting at it below. Step the shift on the Output Mapping card: if the upper line stops short and the lower one crosses, raise it; if the upper one crosses and the lower one leaves a gap, lower it. When both just touch, press **Save**, which stops the pattern and then saves. Changes made while output runs take effect at once, moving one sample at a time so the scanners never jump, but are only written to RP2354 flash while stopped.
- **Underrun hold.** When the buffer runs dry the scanners hold position and the laser blanks; the Ether Dream status reports the underflow flag.
- **Test patterns.** Switching patterns while one runs moves to the new pattern with the laser blanked. **Stop Stream** stops all output, like the top **STOP**, and parks the scanners.
- **Scanner load counters.** Per stream, the RP2354 counts the largest lit X/Y step and the number of lit and blanked steps of 1/8 full scale or more. A lit single-sample jump across the frame is a step input to the galvos; the counters show whether a source sends them.

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/api/rp/rate_limit?value=<1000-200000>` | POST | Set and save the scanner rate limit (pps) |
| `/api/rp/set_rate?value=<pps>` | POST | Request a rate; the applied rate is the request clamped to the limit |
| `/api/rp/color_shift?value=<-2000..2000>` | POST | Apply the colour shift in microseconds; saved at once while output is stopped |
| `/api/rp/color_shift?save=1` | POST | Save the applied colour shift; refused while output runs |

`/api/rp/status` adds `rate_limit`, `remote_max_rate`, `remote_requested_rate`, `remote_color_shift_us`, `remote_color_shift_samples`, `remote_color_shift_saved`, `remote_max_lit_step`, `remote_lit_big_steps`, `remote_blank_big_steps`, `remote_stale_packets` and per-protocol `stream_*_dropped` counters for points a protocol received but could not queue. Packets lost on the way to the RP2354 show up as `remote_seq_gaps` / `remote_seq_missing` (breaks in the packet sequence), `remote_rx_partial` (receives abandoned mid-packet), and the ESP32-S3's own `payload_ready_timeouts`, `payload_spi_errors` and `payload_ack_timeouts`.

`docs/hardware-testing.md` describes the test loop (build, flash, instrument, measure, watch, repeat, then a real client). `tools/hwtest.py` exercises these behaviours on a board (rate clamps, stop and cancel paths, underrun hold, overrun, throughput, IDN packet-size loss) and refuses to run while anything is outputting. `tools/ild_inspect.py` reports the largest lit and blanked steps in `.ild` files. `docs/galvo-safety-review.md` has the findings, measurements and the tuning roadmap, and `docs/galvo-safe-laser-dac-firmware.pdf` is the same review as a white paper with diagrams and charts; `docs/research/` has the background on the hardware, the protocols and ILDA tuning practice.

---

### RP2354 Diagnostics

When the USB-C cable is connected to the RP2354, it shows up as a USB serial port with a diagnostic log. Messages are prefixed with `[RP]`. Nothing is printed while the port is closed.

The log reports:

- Firmware version, whether the last reset came from the watchdog, and the buffer and mirror settings (printed once, when the port is first opened)
- Every command from the ESP32-S3: `START`, `STOP`, `CLEAR`, `RATE`, `LIMIT`, `SHIFT`, `BUFFER`, `MIRROR`
- Once per second while running: output mode, underruns, failsafe blanking, buffer fill, and the last commanded X/Y position
- Once per second, if any occurred: bad, dropped, or timed-out stream packets, and breaks in the packet sequence
- A `lit_jump` line for each lit step of 1/8 full scale or more, with its start and end position and where it fell in the 256-sample stream packet

The output engine is always in one of these modes:

| Mode | Meaning |
| --- | --- |
| `PARKED` | Stopped; scanners at centre, laser channels at 0 V |
| `START_BLANK` | Started; scanners centred and laser blanked, then ramped to the first point when it arrives |
| `STREAMING` | Playing received points |
| `UNDERRUN_BLANK` | Buffer ran dry; scanners hold position, laser blanked |
| `FAILSAFE_BLANK` | No points for 20 ms; blanking is re-asserted every 5 ms until data returns |

Two GPIOs on the breakout nets mirror this state for a scope or logic analyzer:

| Pin | Net | High when |
| --- | --- | --- |
| GPIO10 | `RP_IO10` | Output engine is started |
| GPIO11 | `RP_IO11` | Output is blanked or stopped (any mode other than `STREAMING`) |

---

### USB Protocol Experiments

Both the ESP32-S3 and RP2354 have native USB, so the board can also be used for USB-based applications.

As a proof of concept, a Helios DAC USB protocol implementation was tested and ran reliably directly on the RP2354. Due to licensing concerns, this implementation is not included in the public firmware. However, the test showed that the hardware is suitable for custom USB protocol development, including USB streaming as an alternative to network streaming.

---

## Hardware Documentation

The schematic is published for reference, repair, education, and firmware development.

PCB manufacturing files, production files, and full hardware source files are not published. The ILDAWaveX16 name, branding, documentation, artwork, and released design files may not be used for commercial manufacturing or resale without permission.

For fully open-source hardware, see the previous ILDAWaveX16 generation with complete KiCad source files:

https://stanleyprojects.com/projects/ildawavex16

---

## Firmware License

The firmware source code is released under the **GPLv3** license.

You may study, modify, and redistribute the firmware under the terms of the GPLv3 license.

Firmware derivatives must remain open under the same license.

---

## Community

Developers are encouraged to study, modify, optimize, and adapt the firmware for new protocols and workflows.

The community has already done a great job helping shape the project, so join the [Discord server](https://discord.gg/B33ecuS3Sw), share your ideas, and build something cool with us.
