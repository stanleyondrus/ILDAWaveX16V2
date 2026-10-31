# ILDAWaveX16 V2 hardware research for firmware review

Compiled 2026-10-23. Sources are cited per fact. "Schematic" means
`hardware/ILDAWaveX16_V2_REV_00_SCH.pdf` (rev REV_00, dated 2026-05-01,
KiCad 10.0.1, one A3 sheet); parts of it are named by area (DAC, peripheral,
power, RP2354 GPIO, ESP32-S3, Ethernet). Downloaded documents (DAC80508
datasheet SLASEL1D, ILDA ISP99, ILDA Test Pattern 1995, RP2350 datasheet, and
the j4cDAC, Helios and OpenLase sources) were read from local copies that are
not in the repository; each is cited by name, with its public URL.

Fetch budget used: 18 WebFetch, 15 WebSearch, 7 curl downloads.
Unreachable from this machine: laserfx.com and photonlexicon.com (connection
refused / timeout), pangolin.com (HTTP 429 twice), baiyi-light.com DT50K page
(404), lh-laser.com (403). Facts from those sites are marked as coming via
search-result snippets.

---

## 1. The board's analog output chain (from the schematic)

### 1.1 DAC

- **U8 = DAC80508**, 8-channel, 16-bit, WQFN-16 pinout matches the datasheet
  (SDI 14, CS 12, SCLK 13, SDO/ALARM 15, REF 1, VDD 7, VIO 16, GND 6, EP 17).
  VIO = +3V3, VDD = +5V (C13 100 nF + C14 1 µF). Schematic, DAC area.
- **Reference:** REF pin 1 is wired to the board net **+2.5V** with C19 1 µF to
  GND. No external reference IC exists on the sheet, so the +2.5V net is the
  DAC's **internal 2.5 V reference output**, and it is reused as the op-amp
  bias for the X/Y level shifters (R11, R22). Schematic, DAC area. The
  datasheet confirms the internal reference is 2.5 V, ±5 mV initial accuracy,
  2 ppm/°C typical (DAC80508): DAC80508 datasheet features list; TI datasheet
  https://www.ti.com/lit/ds/symlink/dac80508.pdf
- **The schematic does not say which variant is fitted** (printed only as
  "DAC80508"). The datasheet's orderable parts are DAC80508Z (reset to zero
  scale) and DAC80508M (reset to midscale), plus ZC/MC with a CLR pin instead of
  SDO. Because this board uses pin 15 as SDO (DAC_MISO), it is a Z or M part,
  not a ZC/MC. **Which of Z or M is fitted is unverified** (no BOM in the repo;
  only the STEP and the PDF are in `hardware/`). Source: DAC80508 datasheet
  "Device Comparison Table"; https://www.ti.com/lit/ds/symlink/dac80508.pdf
- Channel assignment, schematic vs firmware: schematic OUT0 → USR2+,
  OUT1 → USR1+, OUT2 → Int+, OUT3 → B+, OUT4 → G+, OUT5 → R+, OUT6 → X (via
  op-amps), OUT7 → Y (via op-amps). Firmware defines `DAC_CH_X 6, DAC_CH_Y 7,
  DAC_CH_R 5, DAC_CH_G 4, DAC_CH_B 3, DAC_CH_I 2, DAC_CH_U1 1, DAC_CH_U2 0`,
  which match the schematic exactly. Schematic, DAC area; repo
  `firmware/RP2354/src/dac80508.c` lines 22–27.

### 1.2 Colour / intensity / user channels (single-ended)

- OUT0..OUT5 go through **150 Ω series resistors (R31–R36) straight to the
  DB-25** nets USR2+, USR1+, Int+, B+, G+, R+. No op-amp, no filter capacitor,
  no inverted "−" drive. Schematic, DAC area.
- Full-scale is set by the DAC gain: with internal 2.5 V reference and gain 2
  (BUFF-GAIN = 1, REF-DIV = 0) the range is **0 to 2×VREF = 0–5 V**, which is
  the ILDA colour range. Datasheet Table 1 and "Voltage range" spec:
  DAC80508 datasheet; https://www.ti.com/lit/ds/symlink/dac80508.pdf
- The firmware writes `GAIN = 0x00FF` (all BUFF-GAIN bits set, REFDIV-EN
  clear) at init, i.e. 0–5 V on every channel. Repo
  `firmware/RP2354/src/dac80508.c` line 414; `include/project_config.h`
  `DAC_GAIN_2X_ALL_CHANNELS 0x00FF`.
- The DAC output buffer can drive 2 kΩ ‖ 10 nF, 20 mA with 0.5 V headroom; the
  150 Ω series resistor is only short-circuit/ESD protection, not a filter.
  DAC80508 datasheet "Output Amplifiers", "High Drive Capability".
- ILDA R−/G−/B−/Int− pins (18, 19, 20, 16) and USR1−/USR2− (21, 22) are
  **tied to GND on the board**, so the colour "differential" pairs are
  single-ended 0–5 V against ground. Schematic, peripheral area. (ISP-DB25
  allows this: "When compared to the ground level of the differential driver,
  the voltage level of the normal signal line shall be +2.5V and the voltage
  level of the inverted signal shall be −2.5V" is the full-differential case;
  the standard only requires the differential voltage to be 0..+5 V.)
  ISP99 lines 269–315;
  https://www.ilda.com/resources/StandardsDocs/ILDA_ISP99_rev002.pdf

### 1.3 X and Y (differential, through LM324)

Topology for X (Y is identical with U3B/U3C and R21–R26, R29, R30):

- **U3A (LM324QT)**: non-inverting input from OUT6 through R12 10 k with
  R13 20 k to GND (divider 2/3). Inverting input from the **+2.5V** net through
  R11 10 k, feedback R10 20 k. Transfer function:
  `Vx+ = (1 + 20k/10k) × (2/3 × Vout6) − (20k/10k) × 2.5 V = 2·Vout6 − 5 V`.
  So **Vout6 = 0 V → X+ = −5 V; Vout6 = 2.5 V (code 0x8000) → X+ = 0 V;
  Vout6 = 5 V → X+ = +5 V**. Schematic, DAC area.
- **U3D**: unity-gain inverter (R8 10 k in, R9 10 k feedback, + input to GND)
  producing **X− = −X+**. Both legs leave through **150 Ω series resistors
  (R27, R28)** to DB-25 pins 1 and 14. Schematic, DAC area.
- Result: ±5 V per leg, **±10 V differential**, exactly the ISP-DB25
  requirement ("the voltage level of the normal signal line shall be +5V and
  the voltage level of the inverted signal shall be −5V"). ISP99 lines
  239–267.
- **No analog filtering or slew limiting** on X/Y other than the LM324's own
  limits: no capacitors in the feedback network, no RC on the outputs.
  Schematic, DAC area.
- **Op-amp rails:** LM324 V+ = VDD (nominally **+8 V**), V− = VSS (nominally
  **−8 V**), decoupled by C3/C5 100 nF. Schematic, DAC and power areas.
- **LM324 dynamic limits** (TI datasheet): slew rate **0.5 V/µs typical** at
  unity gain, unity-gain bandwidth 1.2 MHz.
  https://www.ti.com/document-viewer/LM324B/datasheet/GUID-D481D949-AB5C-40D5-A39B-62D2CD7680EF
  Consequence: a full −5 V → +5 V step on one leg takes ≈20 µs to slew in U3A
  and a further ≈20 µs in U3D, i.e. the op-amps, not the DAC (1.8 V/µs,
  5 µs settling), set the analog step response. At 50 kpps a sample is 20 µs,
  so a full-scale jump cannot complete within one sample. (Derived from the
  two cited slew rates; the 0.5 V/µs is "typical" at ±15 V, ±8 V rails may be
  slightly slower — unverified.)
- The LM324 output can swing to roughly V+ − 1.5 V; with +8 V that is about
  +6.5 V, so ±5 V is within range but with little margin if VDD sags
  (unverified for this exact part/rail; standard LM324 characteristic).

### 1.4 Power rails (relevant to analog behaviour)

- **VDD (+8 V nominal, "MAX 17V")**: either from **U6 TLV61048 boost** from
  VBUS (USB) with R14 1 M / R18 100 k feedback giving +8 V, through D6, or from
  **+V_EXT (J3 pin 1) via fuse F2 0.5 A and D5**. Schematic, power area.
- **VSS (−8 V nominal)**: from a **charge pump hung on the boost converter's
  switch node** (R19 1 R, C16 2.2 µF, D3, D4, C17 2.2 µF) through D8, or from
  **−V_EXT (J3 pin 3) via F3 0.5 A and D7**. The negative rail therefore has
  low current capability and exists only while the boost converter switches
  or an external negative supply is present. Schematic, power area.
  (Exact current capability unverified.)
- **+5 V** (DAC VDD, shutter pull-up, LED) from **U5 TPS62160 buck** off VDD;
  **+3V3** from **U4 NCP167BMX330** LDO off +5 V. Schematic, power area.
- Sequencing note (derived): the DAC (+5 V) and op-amps (±8 V) are powered
  from the same chain, so while VSS is absent the LM324 has no negative rail
  and X±/Y± are clamped near 0 V rather than at −5 V.

### 1.5 Shutter

- **ILDA pin 13 "Shutter"** is driven by Q1 **AP3404 N-MOSFET**: gate from net
  **ShutterL** through R15 100 Ω, **R16 10 k gate pull-down**; drain pulled to
  **+5 V through R17 10 k**, and the drain goes through R20 150 Ω to the
  Shutter net (DB-25 pin 13 and J12 pin 1). Schematic, peripheral area.
- Logic: **ShutterL low (or floating/hi-Z) → MOSFET off → Shutter pin = +5 V
  = shutter OPEN**. ShutterL high → MOSFET on → Shutter pin ≈ 0 V = shutter
  CLOSED. (Polarity per ISP-DB25: "A voltage level of 0 volts shall close the
  shutter... A voltage level of +5 volts shall fully open the shutter."
  ISP99 lines 320–327.)
- **ShutterL is ESP32-S3 GPIO40** (MTDO/GPIO40/CLK_OUT2). Schematic,
  ESP32-S3 area. The V2 firmware does not drive it; grep of
  `firmware/ESP32S3/main` finds shutter GPIO handling only in the legacy
  `rp_manager_v1.c`. **So on the V2 build the shutter line idles at +5 V
  (open) at all times, including boot and stop.** The projector's own
  blanking therefore depends entirely on the colour channels being 0 V.
- Source impedance: 10 k pull-up + 150 Ω series; a projector input that sinks
  current will pull it below 5 V (unverified projector loads).

### 1.6 Interlock

- **DB-25 pins 4 (Interlock A) and 17 (Interlock B) are shorted together on
  the board** with no firmware control. Schematic, peripheral area. The
  standard says these are "tied together at the signal source. If this loop is
  broken, it is required that projectors close a shutter or turn off the laser".
  ISP99 lines 329–343. The board can therefore never open the
  projector interlock loop as a fault response.

### 1.7 DB-25 (J2, X0325FT7B2) pin assignment as wired

| Pin | Net on board | ISP-DB25 name |
|---|---|---|
| 1 | X+ | X+ |
| 2 | Y+ | Y+ |
| 3 | Int+ (DAC OUT2 via 150 Ω) | Intensity+ |
| 4 | tied to 17 | Interlock A |
| 5 | R+ (OUT5) | R+ |
| 6 | G+ (OUT4) | G+ |
| 7 | B+ (OUT3) | B+ |
| 8 | USR1+ (OUT1) | User 1+ |
| 9 | USR2+ (OUT0) | User 2+ |
| 10, 11 | GND | User 3+, User 4+ |
| 12 | NC | Projector return |
| 13 | Shutter | Shutter |
| 14 | X− | X− |
| 15 | Y− | Y− |
| 16 | GND | Intensity− |
| 17 | tied to 4 | Interlock B |
| 18, 19, 20 | GND | R−, G−, B− |
| 21, 22, 23, 24 | GND | User 1−..4− |
| 25, shell | GND | Ground |

Schematic, peripheral area; names from ISP99 lines 187–215
(https://www.ilda.com/resources/StandardsDocs/ILDA_ISP99_rev002.pdf).
Also on the board: 3-pin headers J4 (X+/GND/X−), J5 (Y+/GND/Y−), 2-pin J6–J12
(R, G, B, Int, USR1, USR2, Shutter against GND), J3 (±V_EXT in). Schematic
peripheral area.

### 1.8 Digital side

- **RP2354A (U11, QFN-60)**: GPIO0 DAC_MISO, GPIO1 DAC_CS, GPIO2 DAC_SCK,
  GPIO3 DAC_MOSI; GPIO4/5 RP_UART_TX/RX; GPIO12 RP_SPI0_MOSI, GPIO13
  RP_SPI0_CS, GPIO14 RP_SPI0_SCK, GPIO15 RP_SPI0_MISO (the ESP→RP stream
  link); GPIO10/11/16 to header J15. Schematic, RP2354 GPIO area. This matches
  `stream_rx.c` (stream SPI on GPIO12–15).
- **ESP32-S3-MINI-1-N4R2 (U1)**: GPIO10–13 (FSPI CS/MOSI/CLK/MISO) drive
  RP_SPI0_CS/MOSI/SCK/MISO. **R43–R46 (0 Ω) that would also connect the ESP to
  DAC_CS/MOSI/SCK/MISO are marked DNP** (crossed out), so the DAC is wired to
  the RP only. GPIO1/2/4 → J15; GPIO9 RP_UART_TX, GPIO10 RP_UART_RX (as seen
  on the ESP side: pins 9/10 of the module carry RP_UART_TX/RX); GPIO18 SD_CS,
  GPIO19 ETH_CS, GPIO20–22 ESP_MOSI/SCK/MISO (shared with W5500 and SD);
  GPIO38 SD_DET; GPIO40 ShutterL; GPIO41 ETH_RST; GPIO42 ETH_INT. Schematic
  ESP32-S3 area.
- ESP master clock for the RP payload link: `RP_SPI_PAYLOAD_SPEED_HZ
  32000000` (32 MHz). Repo `firmware/ESP32S3/main/app_config.h` line 102.
- **RP2350 SPI slave limit:** "To ensure correct device operation, SSPCLK must
  be at least 12 times faster than the maximum expected frequency of SSPCLKIN
  ... In slave mode, the same maximum SSPCLK frequency of 150 MHz can achieve a
  peak bit rate of 150 / 12 = 12.5Mb/s." RP2350 datasheet §12.3.4.4,
  RP2350 datasheet; https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf
  A 32 MHz master clock would exceed what the PL022 slave is specified for,
  **but the RP firmware receives the stream with a PIO state machine
  (`spi_payload_rx_program`) and DMA, not the SSP block**, so the /12 limit
  does not apply; the PIO limit is the system clock. Repo
  `firmware/RP2354/src/stream_rx.c` lines 93–156.
- **W5500 (U9)**: SCS = ETH_CS, SCLK/MISO/MOSI shared ESP bus, INT = ETH_INT
  (pulled up by R37 4.7 k), RST = ETH_RST, PMODE0–2 all +3V3 (auto-negotiation,
  all capable), 25 MHz crystal, magnetics in RJ45 J14 RB1-1D6B8K1A. Schematic
  Ethernet area. ESP drives it at 60 MHz SPI (`net_manager.c` line 855).
  WIZnet: "SPI supports 80 MHz speed", "Internal 32Kbytes Memory for TX/RX
  Buffers", "16KB TX/RX Socket buffers". https://wiznet.io/products/ethernet-chips/w5500
  and the datasheet https://www.mouser.com/datasheet/2/443/Wiznet_W5500_DS_1 0 5-1212109.pdf
  A secondary source says the guaranteed minimum SCLK is 33.3 MHz with 80 MHz
  being the design target (via search snippet of
  https://forums.parallax.com/uploads/attachments/50345/100990.pdf — unverified).
- **D1** is a WS2812-type RGB LED on ESP GPIO7 ("LED"); microSD J13 on the
  shared SPI bus with SD_CS. Schematic, peripheral area, overview.

---

## 2. DAC80508 behaviour (TI datasheet SLASEL1D; https://www.ti.com/lit/ds/symlink/dac80508.pdf)

### 2.1 SPI timing
- Write: **fSCLK max 50 MHz**, SCLK high/low 9 ns, SDI setup 5 ns, hold 10 ns,
  CS-to-SCLK setup 13 ns, SCLK-falling-to-CS-rising 10 ns, **CS high time
  15 ns min**. (Table 7, "Programming Timing Requirements".)
- Read / daisy-chain: fSCLK max 18 MHz (FSDO = 0) or 25 MHz (FSDO = 1) at
  VIO ≥ 2.7 V.
- Each access is one 24-bit frame: 4 address bits + 16 data bits (register
  map Table 8). Commands take effect on the CS rising edge.
- The repo's PIO program uses 1 + 1 + 24×3 + 1 = 75 PIO cycles per 24-bit word
  (`DAC_PIO_CYCLES_PER_WORD`), one word per active channel plus one TRIGGER
  word per sample. Repo `firmware/RP2354/src/dac80508.c` lines 32–36, 110–127.

### 2.2 Update rate, settling, glitch
- "In both update modes a minimum wait time of **1 µs** is required between
  DAC output updates." (§8.3.1.3.1.) That is the datasheet's only update-rate
  figure; the 50 MHz SPI gives ≈0.5 µs per 24-bit word, so with 9 words per
  sample the interface itself allows ≈200 ksps for 8 channels + LDAC.
- **Settling time 5 µs** (¼→¾ scale to ±2 LSB, VDD 5.5 V, gain 2);
  **slew rate 1.8 V/µs**; **power-up time 12 µs** (from per-channel
  power-down to full scale).
- **Code-change glitch impulse 4 nV·s** (1 LSB around the major carry);
  channel-to-channel AC crosstalk 0.2 nV·s; digital feedthrough 0.1 nV·s;
  **power-up glitch magnitude 25 mV** (code = zero scale, CLOAD 50 pF).
- Output noise 14 µVpp (0.1–10 Hz, midscale), 78 nV/√Hz at 1 kHz.
- Max capacitive load 2 nF unloaded / 10 nF with 2 kΩ.

### 2.3 Simultaneous update, LDAC, broadcast
- Each channel has a SYNC-EN bit (SYNC register 0x02, reset 0xFF00). With
  SYNC-EN = 0 (asynchronous) the output updates on the CS rising edge of the
  data write; with SYNC-EN = 1 (synchronous) it updates only on an **LDAC
  trigger, which is the LDAC bit (bit 4, value 0x0010) in the TRIGGER register
  0x05** — there is no LDAC pin on this part. (§8.3.1.3.1; Table 14.)
- **Broadcast**: writing the BRDCAST register 0x06 loads the same 16-bit value
  into every channel whose DACx-BRDCAST-EN bit (SYNC register bits 15:8,
  default 1) is set, "on CS rising edge independently of their synchronous
  mode configuration". (§8.3.1.3.2.) This is a one-frame way to force all
  outputs to a known code.
- There is no multi-device LDAC pin; "Daisy Chain Operation" is via SDO.
- The repo writes `SYNC = 0xFFFF` (all channels synchronous and broadcast-
  enabled) and ends every sample batch with `TRIGGER = 0x0010`, so all
  channels of a sample update together. Repo `dac80508.c` lines 126, 148,
  172, 415.

### 2.4 Power-on and reset
- "The POR causes all registers to initialize to their default values and
  communication with the device is valid only after a **250 µs** power-on-
  reset delay. The default value for all DACs in the DACx0508Z devices is
  **zero-code** and **midscale-code** for the DACx0508M ones. Each DAC channel
  remains at the power-up voltage until a valid command is written to it."
  (§8.3.3.1.)
- POR only occurs if VDD or VIO fall below their low thresholds (VDD < 0.7 V,
  VIO < 0.7 V per Figures 62/63) for at least 100 µs; between the low and high
  thresholds (VDD 0.7–2.2 V, VIO 0.7–1.2 V) reset is undefined. (§8.3.3.1.)
- **Software reset**: write **0x000A** (code 1010) to SOFT-RESET[3:0] in the
  TRIGGER register; "A software reset initiates a POR event", i.e. all
  registers go back to defaults, **including GAIN** (so a Z part returns to
  gain 1 = 0–2.5 V and zero code; an M part to gain 2 and midscale). (§8.3.3.2,
  Table 13: BUFFx-GAIN default 0 for Z, 1 for M.)
- The repo's init does `TRIGGER = 0x000A`, waits 10 ms, then writes GAIN and
  SYNC, then "Boot safety: all physical DAC outputs raw 0x0000" via
  `dac_write_all_zero()`. Repo `dac80508.c` lines 411–421.
- **Consequence for this board** (derived from §1.3): raw code 0x0000 on
  OUT6/OUT7 is 0 V at the DAC, which the LM324 stage turns into **X+ = Y+ =
  −5 V, X− = Y− = +5 V — a full-scale corner, not centre**. Centre is code
  0x8000 (2.5 V). So: a Z-variant DAC parks the galvos in the corner from
  power-up until the first centre write; an M variant parks them at centre;
  and the firmware's own "hard zero" state (boot, stop) writes 0x0000 to X
  and Y and therefore drives the scanners to the corner while blanking
  colours. The firmware's underrun path (`dac_build_words_for_blanked_xy`)
  keeps X/Y at the last position or centre and only zeroes colours, which is
  consistent with the comment "signed 0 maps to raw DAC 0x8000". Repo
  `dac80508.c` lines 130–172; `output_engine.c` lines 99–160.
- The DAC itself has no fault/limit outputs other than REF-ALARM on SDO/ALARM
  (CONFIG register ALM-EN), which would need a 10 kΩ pull-up (pin table note).

---

## 3. Galvanometer scanners: the "DT50K" set and galvo failure modes

### 3.1 Identity
- "DT50K"/"DT-50K"/"DT50" is a widely resold Shenzhen-made 50 kpps show galvo
  set. One listing names the maker as "shenzhen Huatelai Photo-Electric
  Co.,Ltd" (sold as LH-DT50K); another seller's URL slug calls it the "Dragon
  Tiger" scanner. **The original manufacturer is unverified**; it is not a
  Sino-Galvo catalogue product as far as these searches show. Via search
  snippets: https://lh-laser.com/index.php?product_id=225&route=product%2Fproduct ,
  https://www.baiyi-light.com/DT50KDragonTigerScanner (both unreachable for
  direct fetch), https://sinogalvo.com/
- A compatible 50K unit is advertised as "compatible with DT40K systems", so
  DT40K/DT50K are one family. Via search snippet:
  https://www.aliexpress.com/i/3256807270407794.html

### 3.2 Specifications (vendor data, no independent datasheet found)
From the Teclulu 50 kpps listing (SKU THYXGT50, specs match the DT50K
mirror sizes):
https://www.teclulu.com/products/50k-pps-high-speed-galvo-scanner-laser-show-rgb-laser-scanner-large-mirrors
- "Speed: 50Kpps@±8°(ILDA Test Frame)"; table: 20° → >35 kpps, 15° →
  >35 kpps, 10° → >40 kpps, 8° → >45 kpps, 5° → >50 kpps.
- "Deflection angle: max 70° optical (factory calibration @±25°)";
  "Scanner Angle: ±35°" (mechanical).
- "Input: Analog ±5V"; "Power Supply: ±24VDC @1A each".
- "Mirror Aperture: X: 8.5X8.5X1mm, Y: 8X13X1mm"; dielectric coating,
  ">99.3% @45° incidence (400nm-700nm)".
- No bandwidth, step response, linearity, drift or protection figures given.

Other DT50K listings (via search snippets): "35-40Kpps ILDA at 15°,
42K-45Kpps at 8°, 50Kpps at 6°"; "60° max optical"; "±24V, 2–3A" (AliExpress
https://www.aliexpress.com/i/3256808628278522.html ; Alibaba
https://www.alibaba.com/product-detail/15k-20k-25k-30k-40k-50kpps_62564338204.html).
Note the vendors disagree on whether 50 kpps is at 5°, 6° or 8°; treat the
honest figure as **~45 kpps at 8° ILDA, 50 kpps only at ≤6°**.

A comparable Chinese 50K set reviewed on laserpointerforums (Laserworld
LW-50K) measured with the shipped 5×11×0.6 mm mirrors: **30° → 17–19 kpps,
15° → 25–27 kpps, 8° → 40 kpps, 5° → 37–45 kpps**; "at 40kpps the scanners and
drivers become warm"; each driver has "its own safety fuse" that disconnects
power on over-current; "No distortions or over or undershoots are seen when
running the scanners on their rated output and scan angles."
https://laserpointerforums.com/threads/laserworld-lw-50k-galvo-set-review.37897/

A Chuyun "RD6215-50K" listing gives the drive-side numbers typical of these
amps: "Analog position input range: ±5V"; "Analog signal input impedance:
200K ±1% Ω"; "Position signal input proportional coefficient 0.5 V/°";
"Linearity 99.9%, range ±15°"; "Power supply voltage: +24V/3A, -24V/3A".
https://chuyunlaser.com/products/50k-high-performance-galvo-scanner-laser-scanning
(0.5 V/° means ±5 V ≈ ±10° mechanical ≈ ±20° optical = 40° optical full
scale at the ±5 V input — consistent with "factory calibration @±25°".
Derived; the exact calibration of a given DT50K is unverified.)

### 3.3 The kpps rating and the 8° convention
- ILDA Test Pattern standard: "Display the ILDA Test Pattern image at 12,000
  (12K) or 30,000 (30K) points per second ... it must be scanned small to
  prevent distortion of the circle. For 30K tuning this about **8 degrees
  optical or less**. For 12K tuning this can typically be as large as 15
  degrees optical." ILDA Test Pattern (1995) lines 78–84;
  https://www.ilda.com/resources/StandardsDocs/ILDA_TestPattern95_rev002.pdf
- Same document: "Use caution when adjusting scanners. Galvanometer and motor
  scanners can be easily damaged through careless or rough adjustment ...
  **Never allow scanners to oscillate!**" (lines 68–74).
- ISP-30K: "The ILDA Standard Projector shall have image response as defined
  by the ILDA Test Pattern properly displayed at 30,000 points per second."
  ISP99 lines 169–174.
- Photonlexicon definition (via search snippet, site unreachable): "Scanners
  are said to be 30kPPS if they can scan the ILDA test pattern (with no points
  added or subtracted) correctly at a DAC point rate of 30,000 points per
  second, typically at 8 degrees full scan angle."
  https://photonlexicon.com/forums/showthread.php/12333-quot-KPPS-quot-technical-definition-question
- Angle-vs-speed derating is the norm in vendor tables: a 30K set listed as
  "30 kpps at 20°, 35 kpps at 10°, 40 kpps at 8°, 45 kpps at 5°". Via search
  snippet: http://www.laser-parts.com/30kpps-laser-scanning-galvo-galvanometer-scanner-set-ilda.html

### 3.4 Galvo failure modes and protections (general)
- **Heat from fast large-angle drive**: Thorlabs galvo guide — "applications
  involving rapidly changing drive signals can create excess heat buildup,
  causing the galvo motor to fail or driver board thermal cutout to trip";
  "heatsinks are essential for preventing overheating". Via search snippet:
  https://manuals.plus/m/4848e52428da63b3d312aaaa0697610c5d7e1cb3a02d9c4bb6bb3bdc867652d3
- **Square waves**: a driver manual notes position overshoot "can grow over
  time when the scanner is driven with a large square wave, until a fault
  trips", linked to supply current limits, showing as ">1 kHz sinusoidal
  oscillation in coil current". The same manual: "the system has no
  protection against the galvanometer scanners overheating" and later firmware
  "disables the motors when persistent high current is detected". Via search
  snippet (Thorlabs GVS011/012 manual):
  https://manuals.plus/m/1ab62c9f5cfa5c7e8f26f9910a2b117c0431f80726892049c240a9221c8da69d_optim.pdf
- **Driver fault set** typical of servo amps: Citizen GVD0 lists "Overheat /
  Over Positioning / Over Current / Sensor Error" protections.
  https://ccj.citizen.co.jp/cms/ccj/cb/uploads/product/pdf/Galvano_GVD0_en.pdf
  Thorlabs fault table: "maximum scanner position exceeded" and "maximum peak
  current exceeded" → power amplifier off; "maximum position error exceeded"
  → loop gain reduced. (Thorlabs Scanning Galvo Systems User Guide, via
  search snippet, same manuals.plus URL as above.)
- **Resettable fuses**: "most galvo drivers (scanner amps) have a resettable
  polyswitch that will open during sustained high-current operation to protect
  the galvos. Turning the power off and back on again should reset the
  switch." Via search snippet:
  https://www.photonlexicon.com/forums/showthread.php/29329-Help-with-galvo-board-tuning-and-do-I-need-a-new-motor
- **Coil heating physics**: "heat in the galvo is directly proportional to
  coil resistance"; moving-magnet motors "prone to overheating" under "long
  periods of both high speed and wide-angle motion". Via search snippet:
  https://www.laserfocusworld.com/software-accessories/positioning-support-accessories/article/16547109/optical-scanning-scanning-advances-brighten-and-enhance-laser-light-shows
- **DC hold**: a stationary position is a constant coil current (small unless
  parked at an extreme angle); the risk of a parked beam is to the target,
  not the galvo. Thorlabs' "rapidly changing drive signals" wording implies
  steady-state dwell is the benign case. (Derived; no source gives a DC
  hold current limit for the DT50K — unverified.)
- **Abrupt far jumps**: the amplifier slews at its current limit and the
  closed loop overshoots/rings; the ILDA tuning document's "Never allow
  scanners to oscillate!" and the square-wave warnings above are the only
  quantified guidance found. No concrete "max step per sample" number was
  found for any show galvo — unverified / not published.
- **Operator rules of thumb** found: keep the test pattern at 8° for the rated
  speed and shrink the angle when raising speed (vendor derating tables, ILDA
  test pattern text); heat sink the drivers (LW-50K review); don't let the
  loop oscillate during tuning (ILDA). The specific phrases "never feed square
  waves at full scale" were not found verbatim in a fetchable source.
- Mirror fragility: "The small fragile mirrors break pretty easily when you're
  not careful." (LW-50K review, above.)

---

## 4. ILDA ISP-DB25 (ILDA Standard Projector, Rev 002, July 1999)

Source: ISP99; https://www.ilda.com/resources/StandardsDocs/ILDA_ISP99_rev002.pdf
(listed on https://www.ilda.com/technical.htm as the current standard; a 2024
draft "ILDA-Projector-Standard-2024-09-16-draft.docx" also exists there and was
not read).

- Pinout: see table in §1.7 (pins 1 X+, 2 Y+, 3 Intensity+, 4 Interlock A,
  5 R+, 6 G+, 7 B+, 8–11 User 1–4 +, 12 Projector return signal, 13 Shutter,
  14 X−, 15 Y−, 16 Intensity−, 17 Interlock B, 18 R−, 19 G−, 20 B−, 21–24
  User 1–4 −, 25 Ground). "Signal sources ... shall use a DB-25 female.
  Projectors shall use a DB-25 male."
- Required signals: "X, Y, R, G, B, Shutter, Ground and Interlock loop."
- X/Y: "bipolar analog signal whose voltage range is 10V peak-to-peak
  differential. A differential voltage level of -10V shall deflect the [beam
  to the left / bottom] ... +10V ... [right / top]. A differential voltage
  level of 0V shall place the beam in the center of the screen ... When
  compared to the ground level of the differential driver, the voltage level
  of the normal signal line shall be +5V and the voltage level of the inverted
  signal shall be -5V."
- R/G/B/Intensity: "unipolar analog signal whose voltage range is 5V
  peak-to-peak differential. A differential voltage level of 0V shall produce
  no [colour] output. A differential voltage level of +5V shall produce
  maximum output." Intensity: "0V indicates zero intensity and that the beam
  is fully blanked. +5V indicates full intensity." (Rev 002 changelog:
  "Changed polarity of blanking signal within ISP-DB25.")
- Shutter: "unipolar analog signal whose voltage range is 5 volts peak. The
  shutter signal is referenced to the Ground pin. A voltage level of 0 volts
  shall close the shutter so that no light is emitted ... +5 volts shall fully
  open the shutter."
- Interlock A/B: "intended to keep the projector from outputting light if
  inadvertently connected to non-laser signal sources ... These signals are
  tied together at the signal source. If this loop is broken, it is required
  that projectors close a shutter or turn off the laser or take other actions
  that prevent light from being emitted."
- **What a source should output when idle/stopped/faulted: the standard does
  not say.** Nothing in ISP99 specifies idle X/Y position or colour levels for
  a stopped source; the only normative safety mechanisms are the shutter
  (0 V = closed) and the interlock loop. (Checked by grepping ISP99 for
  idle/standby/blank/centre.) Common practice from DAC firmwares is in §5.
- Pin 12 differs between sources: ISP99 says "Projector return signal",
  Pangolin's pinout marks it not connected (via search snippet,
  https://pangolin.com/blogs/education/ilda-laser-pinout — page itself
  rate-limited). This board leaves pin 12 unconnected.

---

## 5. How other DACs handle underrun / overrun at the output

### 5.1 Ether Dream (j4cDAC firmware, github.com/j4cbo/j4cDAC)
Source files: j4cDAC `dac.c`, j4cDAC `dac.h`, j4cDAC `lightengine.c` from
https://github.com/j4cbo/j4cDAC/blob/master/firmware/lib/dac.c ,
https://github.com/j4cbo/j4cDAC/blob/master/firmware/inc/dac.h ,
https://github.com/j4cbo/j4cDAC/blob/master/firmware/lib/lightengine.c
- Buffer: `DAC_BUFFER_POINTS 1800`, `DAC_MAX_POINT_RATE 100000`, rate-change
  queue of 200 entries.
- **Underrun**: in the per-point ISR, `if (produce == consume) { dac_stop_underflow(); return; }`.
  `dac_stop()` comment: "Stop the DAC, set all outputs to zero, and return to
  the idle state. This is triggered internally when the DAC has a buffer
  underrun, and may also be called externally at any time." It stops the PWM
  timer first "which could cause the DAC outputs to be left on", **closes the
  shutter** (`FIOCLR DAC_SHUTTER_PIN`), forces LDAC immediate, calls
  `hw_dac_zero_all_channels()`, sets state `DAC_IDLE`, sets
  `DAC_FLAG_STOP_UNDERFLOW`, resets colour delay lines.
  Note: the j4cDAC packs X as `(x + 0x8000) << 4`, i.e. its DAC is
  offset-binary with centre at 0x8000, so "zero all channels" on the Ether
  Dream means **DAC code 0 = centre/blank in its own bipolar output stage**
  (unverified how its analog stage maps code 0; the shutter closes regardless).
- **Restart required**: after a stop the host must send Prepare again
  (`dac_prepare()` requires `DAC_IDLE` and `LIGHTENGINE_READY`, clears the
  ring and flags). Protocol doc: playback states Idle ("No points may be added
  to the buffer") / Prepared / Playing; `playback_flags` bit 1 = "1 if the last
  stream ended with underflow, rather than a Stop command. Reset to zero by
  the Prepare command." https://ether-dream.com/protocol.html
- **Overrun**: "The DAC will reply with ACK if the incoming packet can fully
  fit in its buffer, or NAK - Full if it cannot." Nothing is dropped; the host
  paces from the reported `buffer_fullness`. `dac_request()` never lets the
  ring become completely full ("one word short of full").
  https://ether-dream.com/protocol.html
- **E-stop**: command 0x00/0xFF "causes the light engine to enter the E-Stop
  state ... Any unrecognized command will also be treated as E-stop";
  `le_estop()` calls `dac_stop(DAC_FLAG_STOP_ESTOP)`. Clear with 'c'.
  https://ether-dream.com/protocol.html ; j4cDAC `lightengine.c`
- Shutter is opened only in `DAC_PLAYING` (`shutter_set()`), closed on every
  stop. j4cDAC `dac.c` lines 575–592.
- No rate limiting or max-step-per-sample safety was found in dac.c.
- Field observation: "Ether Dream goes into a safety stop mode as soon as
  there is any error or missing data." Seb Lee-Delisle, 16 Jan 2025,
  https://forum.liberationlaser.com/t/important-notice-about-lasercube-network-protocol/847

### 5.2 Helios DAC (github.com/Grix/helios_dac)
Source: Helios `main.c` from
https://github.com/Grix/helios_dac/blob/master/firmware/AtmelStudio_helios/lasdac_mainfirmware/src/main.c ;
https://github.com/Grix/helios_dac/blob/master/sdk/cpp/HeliosDac.h
- Frame-based (max 4096 points per frame, `MAXSPEED 0xFFFF` pps, `MINSPEED 7`).
- At end of frame: if a new frame is ready, swap buffers; else if the frame is
  not flagged single-shot, **loop the frame**; else `stop_weak()`.
- `stop_weak()` "outputs a blanked and centered point and stops playback":
  disables SysTick, `playing = false`, and writes `0b0010 << 12` to the colour
  DAC ("blank all colors"). The delayed `stop()` path (TC0 timer) also centres
  XY: `posData = (0x800 << 16) | ((1 << 12) | 0x800); //center XY`.
- Default output at boot: `shutter_set(LOW)` then `stop_weak()`.
- `SetShutter`: "Value 1 = shutter open, value 0 = shutter closed"; "the
  library automatically opens the shutter when you write a frame."
- Overrun: USB frames are double-buffered; a frame that arrives while one is
  pending replaces the pending frame (newest wins) — inferred from the swap
  logic; "HELIOS_FLAGS_DONT_BLOCK ... before previous frame has completed
  transfer" error in the SDK.
- Network (IDN) Helios: "they always play the frame only once"; limits
  `HELIOS_MAX_POINTS_IDN 0x2000`, `HELIOS_MAX_PPS_IDN 100000`.

### 5.3 LaserCube (Wicked Lasers)
- "LaserCubes DO NOT BLANK THE LASER" on "a buffer under-run, lost packet, or
  corrupt or incorrect data"; they "just carry on from where [they] left off",
  so a live beam "can cross areas outside zones and cut across software
  masks". Author recommends physical masking and wired networking. Seb
  Lee-Delisle, 16 Jan 2025, protocol build 0.7.3.
  https://forum.liberationlaser.com/t/important-notice-about-lasercube-network-protocol/847
- A Rust crate `lasercube_core` documents the network protocol types; its
  underrun semantics were not fetched. https://docs.rs/lasercube-core

### 5.4 OpenLase (github.com/marcan/openlase)
Source: OpenLase `output.cpp` from
https://github.com/marcan/openlase/blob/master/output/output.cpp
- OpenLase is a JACK client: the output stage just scales X/Y by `scanSize`
  and applies a `SAFE` flag that forces 10 % size when `scanSize < 10`, and
  zeroes colours when `OUTPUT_ENABLE` is off. Underrun is JACK's xrun
  (silence = 0.0 = centre and black). No hold-last-point logic in the output
  stage.

### 5.5 laser-dac-rs (Modulaser) and this project
- `laser-dac-rs` "handles transition blanking and includes a final output
  filter that clamps coordinates to ±1.0 and blanks any out-of-range point";
  startup/disarm blanking; Helios capacity 4095 points. Via search snippet:
  https://github.com/ModulaserApp/laser-dac-rs
- ILDAWaveX16 V2's published behaviour: "During an active underrun, the
  firmware does not immediately force X/Y to zero; instead, it holds the last
  valid scanner position and blanks the laser channels. If output is stopped
  completely, all DAC channels are driven to a hard zero state." Via search
  snippet of https://stanleyprojects.com/projects/ildawavex16v2 (page is
  JS-rendered; direct fetch returned only the title). Matches
  `firmware/RP2354/src/dac80508.c` `dac_build_words_for_blanked_xy` /
  `dac_build_words_for_all_raw_zero`.

### 5.6 Pangolin FB3 / LaserBoy
- No public statement on underrun handling was found for FB3 (Edmund Optics
  product page only: https://www.edmundoptics.com/p/fb3-lasergalvo-control-hardware/47758/).
  LaserBoy is a frame editor, not a DAC. **Unverified / not found.**

---

## 6. Ten most decision-relevant facts for firmware authors

1. **Raw DAC code 0x0000 on X/Y is a full-scale corner, not centre.** The
   LM324 stage computes `X+ = 2·Vout − 5 V`, so 0 V from the DAC gives −5 V /
   +5 V on X+/X− (−10 V differential) on both axes. Centre is code 0x8000
   (2.5 V). The firmware's "hard zero" boot/stop state therefore slams the
   galvos into a corner; its underrun state (hold or centre + blank) does not.
   (Schematic, DAC area; `dac80508.c` lines 130–145, 418–421.)

2. **Power-up position depends on the unmarked DAC variant.** DAC80508Z resets
   to zero code (→ corner at full deflection); DAC80508M resets to midscale
   (→ centre). The soft reset 0x000A re-applies that default and also resets
   GAIN (Z: gain 1 = 0–2.5 V). Determine the fitted suffix from the chip
   marking and, if it is Z, write 0x8000 to X/Y (or BRDCAST 0x8000) as the
   very first command after the 250 µs POR delay. (DAC80508 datasheet §8.3.3.)

3. **The shutter line idles OPEN.** ESP GPIO40 (ShutterL) has a 10 k pull-down
   on the MOSFET gate; MOSFET off → pin 13 = +5 V = shutter open per ISP-DB25.
   With the V2 firmware not driving GPIO40, blanking relies solely on the
   colour DACs. Driving GPIO40 high closes the shutter. (peripheral area;
   ISP99 shutter clause.)

4. **The interlock loop (pins 4/17) is hard-wired closed on the PCB**, so the
   board has no way to assert the projector interlock as a fault response;
   the only hardware-level kill is the shutter pin. (peripheral area.)

5. **The op-amps, not the DAC, set the X/Y step response.** DAC: 1.8 V/µs,
   5 µs settling, 4 nV·s glitch. LM324: 0.5 V/µs typical — ≈20 µs per full
   10 V swing per stage, two stages in series for the inverted leg. There is
   no RC filtering; the analog chain will not soften a full-scale jump at
   50 kpps (20 µs/sample), so step limiting must be done in firmware if wanted.
   (TI DAC80508 and LM324 datasheets; schematic.)

6. **All eight channels can update simultaneously** via SYNC-EN + the LDAC
   bit (TRIGGER 0x0010), which the firmware already uses, and **BRDCAST
   (register 0x06) sets every enabled channel to one code in a single 24-bit
   frame** — the fastest way to force a known state. Minimum 1 µs between
   output updates; SPI write limit 50 MHz; CS high ≥ 15 ns. (DAC80508 datasheet.)

7. **Galvo ratings are at 8° optical on the ILDA test pattern; speed must be
   derated with angle.** DT50K-class sets: ~45 kpps at 8°, 50 kpps only at
   ≤5–6°, ~35 kpps at 15–20°, and a measured comparable set did 17–19 kpps at
   30°. Drive input ±5 V, ±24 V supplies, mirrors 8.5×8.5 / 8×13 mm. The
   ILDA tuning text: "Never allow scanners to oscillate!" (Teclulu, LPF
   review, ILDA Test Pattern 95.)

8. **Galvo damage mode is thermal, from sustained fast large-angle drive or
   square waves; protection is usually a polyswitch/fuse on the driver, not in
   the galvo.** A DC hold is benign for the galvo and dangerous for the target.
   No published "max step per sample" exists for show galvos; the practical
   rule is to keep points/s within the rating at the current angle.
   (Thorlabs guides, Citizen GVD0, photonlexicon via snippets.)

9. **Industry underrun behaviour: Ether Dream stops, zeroes outputs, closes
   the shutter and requires a new Prepare; Helios loops the frame or outputs a
   blanked, centred point; LaserCube keeps emitting (widely criticised).**
   On overrun Ether Dream refuses the write (NAK-Full) rather than dropping;
   Helios replaces the pending frame (newest wins). ISP-DB25 itself says
   nothing about idle output; blank + centre + shutter closed is the de-facto
   safe state. (j4cDAC dac.c, Helios main.c, Liberation Laser forum.)

10. **The RP2350's SSP block is specified for ≤ SSPCLK/12 in slave mode
    (12.5 Mb/s at 150 MHz clk_peri); the ESP32-S3 clocks the payload link at
    32 MHz, which is only legal because `stream_rx.c` receives with PIO + DMA,
    not the SSP.** Any future move of the stream to the hardware SPI slave, or
    of UART/command traffic onto an SSP slave, must respect the /12 rule.
    Also: W5500 SPI is 80 MHz-capable with 16 KB RX buffer (the reason for the
    11,520-byte TCP window the ESP32-S3 firmware sets). (RP2350 datasheet §12.3.4.4;
    `app_config.h`; `stream_rx.c`; WIZnet.)

---

## 7. What could not be found or verified

- The fitted DAC80508 suffix (Z vs M) — no BOM in the repo.
- The original manufacturer of the DT50K, and any datasheet with step
  response, small-signal bandwidth, coil thermal limits or RMS current limits
  for it. The Cambridge Technology 6240H manual found is a scanned PDF and
  was not OCR'd.
- Any numeric "max step per sample" or "angle × kpps" thermal derating curve
  for show galvos beyond vendor speed-vs-angle tables.
- Verbatim operator maxims ("never feed square waves at full scale") on
  Photonlexicon/laserfx — both sites refused connections from this machine.
- Pangolin FB3 underrun behaviour (not public).
- The LM324 slew rate at ±8 V rails specifically (datasheet value is at ±15 V).
- Current capability of the charge-pump −8 V rail.
- The exact mechanical/optical calibration (V/°) of a specific DT50K unit.
