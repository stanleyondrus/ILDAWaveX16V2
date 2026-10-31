# "Standard ILDA tuning" for a laser DAC: research notes

Compiled 2026-10-23 for the ILDAWaveX16 V2 firmware (ESP32-S3 network/UI MCU + RP2354 DAC-timing MCU).

Legend:
- **[V]** fetched and read the cited page or document directly.
- **[S]** only seen through a search-engine summary of the cited page (page itself refused the fetcher). Treat the wording as approximate.
- **[U]** unverified: single informal source, or my own inference. Said so inline.

Primary documents used (the ILDA Test Pattern, ISP99, ILDA safety guide 7b, a PT-30K scanner datasheet and the
Mach-DSP manual) were read from local copies that are not in the repository; each is cited by its public URL.

---

## 1. The ILDA Test Pattern

### 1.1 The document

- **"ILDA Test Pattern", ILDA Technical Committee, Revision 002, October 1995.** Written by Casey Stack and Bob Mueller (Laser Fantasy International, 1992), Casey Stack (Lightspeed Design, 1995), Scott Huggins (LFI, 1995); prepared by Kelly and Frank Plughoff (Full Spectrum Lasers, 1995). 12 pages: pattern, key, tuning procedure. [V] https://www.ilda.com/resources/StandardsDocs/ILDA_TestPattern95_rev002.pdf
- It is listed as a current ILDA technical standard alongside the ILDA Standard Projector (ISP, Rev. 002, July 1999), the IDTF file format (Rev. 011, 2014) and IDN-Stream (Rev. 002, July 2025). [V] https://www.ilda.com/technical.htm
- The companion standard **ISP-30K** (inside the ISP document) says: "The ILDA Standard Projector shall have image response as defined by the ILDA Test Pattern properly displayed at 30,000 points per second." [V] https://www.ilda.com/resources/StandardsDocs/ILDA_ISP99_rev002.pdf (page 6)
- The pattern document explicitly limits its own scope: "The point output rate associated with this ILDA Standard defines only the rate which must be used during scanner adjustment. This standard does not effect the point rate at which subsequent images may be generated or reproduced." [V] (test pattern doc, p. 2)

### 1.2 Where to get the .ild file

- ILDA's own zip: "zip file containing 11 sample and test files ... Includes 12K and 30K test patterns." [V] https://www.ilda.com/resources/Tech/ILDA-FILE-FORMAT-FILES.zip (linked from https://www.ilda.com/technical.htm)
- Forum copies exist (`ildatsta.ild`, ~11 KB, 1995) but a forum post warns "there are 12K, 30K and DUAL ILDA test patterns, so check which one you have"; the 30K pattern is "the one with a small ilda pattern inside a large box". [S] https://www.photonlexicon.com/forums/showthread.php/4454-Ilda-Test-Pattern?p=49529 and https://laserpointerforums.com/threads/ilda-test-pattern-downloads.55891/
- The 1995 document anticipates the boxed variant: "Newer test patterns may appear with ILDA test pattern small inside a larger box. In this case, adjust the larger box to the maximum screen size." [V] (p. 8)
- There is a browser-based "virtual" ILDA test pattern (useful for checking a renderer, not a scanner). [S] https://observablehq.com/@tomlarkworthy/ilda-virtual-laser-test-pattern

### 1.3 What is in the pattern (Figure 2 key, p. 3) [V]

| Key | Element | Tests |
|---|---|---|
| A1 | Circle inside the inner square (circle touches at four points) | **Correct scanner speed** (servo gain) |
| A2 | Corners of the inner square (dots at the corners) | **Correct damping** (over/undershoot) |
| A3 | Circle roundness/phase | **Same scanner speed** on X and Y |
| A4 | "Y" label at top, "X" label at right | **Inverts** |
| A5 | Outer box | **Amplitude** (maximum scan area) |
| A6 | Centre cross "+" | **Centering** (DC offset) |
| B1 | Two offset horizontal lines above the square with a small vertical tick | **Blanking speed** (convergence point vs tick) |
| B2 | One horizontal line with a blanked gap around a tick | **Blanking damping** (gap symmetric about tick) |
| B3/B4 | Row of six dots of decreasing intensity (lower left), plus the grey ramp bars under the square | **Blanking amplitude** and **DC offset** (fifth dot barely visible, sixth fully blanked) |
| text | "12/30K" lower left, "ILDA" lower right | Speed label; the height of "ILDA" is the ruler for the overshoot step |

Note on the pattern: "The 12/30K indicates the output speed for this test pattern in points per second. Your image source must output points at 12,000 or 30,000 points per second (+/- 1%) for this test pattern to work." [V] (p. 3)

There is **no separate "DAC delay / small circles" element** in the 1995 ILDA document. The colour/blanking latency check is the B1/B2 line pair; the six dots check blanking level, not latency. Vendors' modern "colour shift" test frames (Pangolin's "ILDA 30k Test (scan rate and color shift)", Showeditor's "BlankShift_EachSide" triangle, Modulaser's red-reference-line colour-shift step) are their own figures. [V] https://wiki.pangolin.com/doku.php?id=quickshow:projector_settings ; [V] https://www.showeditor.com/en/tutorials-faq/tutorials/248-laser-output-quality-optimization.html ; [V] https://modulaser.app/docs/guides/improve-laser-output

### 1.4 The tuning procedure (pp. 4–10) [V]

Safety preamble: "Galvanometer and motor scanners can be easily damaged through careless or rough adjustment ... Always watch the output carefully, when images begin to break-up this is a warning that harm may be near. **Never allow scanners to oscillate!**"

1. **Display** at 12,000 or 30,000 pps. "Increase the size of the test pattern until the inner circle begins to distort. Decrease the test pattern 10% from this size. To use the test pattern correctly, it must be scanned small to prevent distortion of the circle. **For 30K tuning this about 8 degrees optical or less. For 12K tuning this can typically be as large as 15 degrees optical.**"
2. **Inverts**: Y at top, X at right; "invert the signal at the driver card or at the source as appropriate."
3. **Y damping** (lower-right corner): raise low-frequency damping until a small overshoot appears whose length is "approximately 2/3 the height of the word 'ILDA'"; trim high-frequency damping in both directions to find the *shortest* overshoot; then lower LF damping to "tuck" the overshoot back into the corner dot. "Use caution not to create undershoot which is very difficult to see." Single-damping amps: do everything with LF damping.
4. **Y servo gain**: circle top/bottom inside the square → increase servo gain; outside → decrease; then redo step 3.
5. **X damping**, 6. **X servo gain**: same, on the upper-right corner / left-right extent. Goal: "the circle should touch the square at four points and be perfectly round." If it touches but is not round, iterate damping/gain; if phase (ellipse tilt) is wrong, iterate X then Y.
7. **Image size**: "Reset image signal size to normal at the signal source. Adjust X and Y image amplitudes to fill screen ... The outside edge of the test pattern is the maximum scan area."
8. **DC offset**: centre the cross; "Make only minor adjustments with DC offsets. Larger adjustments should be made by physically rotating the scanners in their mounts."
   Tips: "Any time you adjust servo gain, the damping(s) must be adjusted as well. The inverse of this is also true."
9. **Blanking damping** (B2): gap evenly spaced either side of the tick.
10. **Blanking servo gain** (B1): convergence to the right of the tick → increase blanking servo gain; to the left → decrease.
11. **Blanking gain and DC offset**: "fifth dot is barely visible and the sixth dot is entirely blanked."
12. **AO (non-mechanical) blanking**: "For AO systems without a blanking filter, **a blanking shift of positive five** will roughly emulate the blanking response defined by the ILDA Test Pattern. A blanking shift will not however, produce fading or blended lines or colors." (This is the origin of the "colour shift ≈ 4–5 points at 30K" folklore.)

Meaning of the symptoms, restated by vendors:
- "When the blue circle is just touching the midpoint of the green square's sides, this is the correct, ILDA-compliant speed." "At too-fast speeds, the blue circle is inside the green square." "At too-slow speeds the blue circle is outside the green square." [V] https://pangolinlegacy.com/ILDAtest.html
- LC MAX tips page says the same three states in one line each. [V] https://wiki.pangolin.com/doku.php?id=tools%3Alcmax%3Atips_scan_speeds_and_lc_max
- PT-30K manual: circle larger than square = "Driver is set too fast or output speed of computer is too slow"; circle smaller = "Driver is too slow or output speed of computer is too high. Readjust driver or slow down computer." [V] https://www.showlasershop.eu/images/PT-30K.pdf

### 1.5 Scanner speed rating methodology, "fast" patterns

- The rating "NK" means: the scanner shows the ILDA Test Pattern correctly at N×1000 pps **at ≤8° optical**. "'ILDA 60K pps' implies the scanner can accurately reproduce all parts of the pattern at 60,000 points per second at 8 degrees or less." A bare "60K pps" claim "only means something can be shown at some angle at 60,000 points per second, which the page calls meaningless on its own." "It is impossible to scan both fast and wide." At 8° the pattern width is "roughly 1/7th of the scanner-to-screen distance." Pangolin rates its "TrueK 50" scanners at 7°. [V] https://pangolinlegacy.com/ILDAtest.html
- Pangolin also admits the circle-in-square test "measures performance at only one point on the scanner's speed curve" and that new tunings "may need new test patterns"; so far "scanner curves have been similar at 12K, 30K, and 36K." [V] same page
- There is **no ILDA-published 50K/60K pattern**; the 1995 pattern is simply played at the higher rate. Vendors quote "50K at 4–5°" (PT-30K manual: "driving speed of up to 50k with the ILDA test pattern, displayed at an angle of 4-5°"). [V] PT-30K manual
- Historic context (LaserFX "Scanner wars" pages, mirrored by Lanling): at 12K the G-120 limit is 40° p-p ("Tune to about 10% under this ... roughly 35 degrees"); at 30K the CT-6800/G-120 limit "is about 25 degrees". [V] https://www.lanlingtech.com/news/15.html (a near-verbatim copy of LaserFX's "Using the ILDA Standard Test Pattern"; LaserFX itself refused connections: [U] https://www.laserfx.com/Backstage.LaserFX.com/Systems/Scanning3.html)
- Cambridge 6215H + 673XX driver: "solid 30K performance with scan angles roughly double what was previously possible with 6800 and 6210 scanners"; with the 671XX HP driver "60K at roughly the same angle as a 6800 or 6210 tuned to 30K" ("twice the angle or twice the speed"). [V] http://www.pangolinlegacy.com/laserists%20articles/HTML/Cambridge_Scanner_Makes_Performance_Breakthrough.html
- ScannerMAX application notes describe a "30K tuning" as 2.5 kHz closed-loop small-signal bandwidth and a "60K tuning" as 5 kHz. [S] https://download.scannermax.com/pdf/S1B_PlusSign.pdf and related notes (not fetched).

---

## 2. Scanner-amp tuning vs software/DAC tuning

### 2.1 What the amp pots do

PT-30K (GT-Tech / SpaceLas-style analog driver) legend [V] https://www.showlasershop.eu/images/PT-30K.pdf:
- **Servo (Servo-Gain)**: "Power of the feedback signal for the internal PID controller."
- **LFD (Low Frequency Damping)**: "Corrects overshoots."
- **HFD (High Frequency Damping)**: "Corrects undershoots."
- **Size**: "Increases or decreases input sensitivity of the complete driver. Does not change driver settings."
- **Offset**: "Electrical offset of the driver, not the galvo! Is adjusted in factory."
- **Position Scale** and **Linearity**: "factory Setting." (7 pots total, "4 of them are necessary for the user. 3 of them should not be changed".)
- Cold-start procedure: Size, Servo, Damping, HFD fully CCW; LFD 5 turns CW; run the pattern "at approx. 20kpps"; open servo until the mirror centres (+2 turns); open Size until it moves; raise servo until overshoot, correct with LFD; reduce undershoot with HFD; "Increase speed of test patterns and readjust."

Mach-DSP (ScannerMAX digital driver) [V] https://www.edmundoptics.com/ViewDocument/ScannerMAXManual.pdf:
- Five fundamental gains summed to drive the coil: **Servo Gain (Error-Proportional)** "provides current to the scanner in proportion to the difference between the commanded and actual position ... This allows quick acceleration"; **Error Integrator**; **Position-Proportional**; **Low Frequency Damping** = velocity "derived through differentiated position ... controls the deceleration ... when overshoots are happening on steps of all amplitudes, increasing this control may help"; **High Frequency Damping** = velocity "derived through integrated current ... typically applicable only to fast-changing signals" (unneeded below ~2 kHz bandwidth); "both ... must be used (normally not in equal proportions) to achieve critical damping."
- PD is "the fastest type of servo configuration ... usually used for laser displays."
- **Input Scale** maps ±1 V to ±10 V differential input to any angle "up to the maximum provided by the scanner ... greater than 100 degrees optical peak-to-peak"; recommends ±10 V differential input to use the 16-bit ADC range.
- Step response: "Rise times can be minimized by increasing the servo gain control. Typically the smoothness of settling can be improved by increasing the Low-Frequency Damping and High-Frequency Damping."
- The driver has digital outputs for "an over-temperature or over-position fault" and a "Settled" indicator; board should run ≤50 °C.

Forum corroboration: "more gain increases overshoot"; HFD "when decreased it causes ringing". [S] https://photonlexicon.com/forums/showthread.php/26040-Any-help-for-figuring-out-how-to-tune-a-WonSung-galvo-set

**Takeaway:** the amp pots set the servo's dynamics (gain, two damping terms), the physical angle per volt (size/position scale), centring (offset) and linearity. None of these should be emulated in the DAC. The DAC's job is to send a signal the tuned servo can follow: the right point rate, size, inverts, latency compensation and point density.

### 2.2 What is done in the DAC/software, with vendor parameter names and defaults

| Function | Pangolin QuickShow / BEYOND | Modulaser | TouchDesigner Laser CHOP | OpenLase (libol) | laser-dac-rs / Helios | Showeditor (Laserworld) |
|---|---|---|---|---|---|---|
| **Scan rate** | "Default sample rate" slider bounded by Minimum/Maximum sliders; recommends max = manufacturer rating, e.g. "Scan Speed 30kpps"; "a high sample rate may damage the scanners" [V] QS, BEYOND wiki | PPS; "20k–30k is safe for most galvos"; "40k+ needs a scanner rated for that speed at 8° ILDA"; "<20k shows visible flicker"; Helios caps at 65,535 [V] | `outputrate`, default 48000, typical 10k–96k [V] | `rate` (int) [V] | Helios 7–65,535 pps USB, 7–100,000 via IDN [V] | "Set the desired scan rate first, before making any output optimizations" [V] |
| **Colour/blanking shift** (colour vs position latency) | "Color shift", positive only; "Most lasers that run at 30kpps will use a color shift of 4, 40kpps a shift of 5, and 20kpps of 3" (units not stated on page; vendor usage is *points*) [V] | "Color shift" 0–500 µs, typical 50–200 µs, "Start around 100 µs" [V] | `colordelay` in ms [V] | none (left to DAC) | laser-dac-rs has a colour delay that also covers transition points [V] | per-colour shift, tuned with "BlankShift_EachSide" triangle figure [V] |
| **Blanking dwell (lead-in/lead-out)** | "Anchors": visible line begin/end, blanked line begin/end; Track settings: beginning/ending anchors (no defaults published) [V] | "Lit hold before travel 100 µs, Blank hold before travel 0 µs, Blank hold after travel 600 µs, Lit hold after travel 65 µs" (defaults) [V] | `preblankon`/`postblankon`/`preblankoff`/`postblankoff` in ms; `starthold` ms [V] | `start_wait`, `end_wait` (blank points at ends of a jump), `start_dwell`, `end_dwell` (lit repeats); simple.c uses start_dwell 3, end_dwell 3 [V] | laser-dac-rs default transition: ~100 µs end dwell, quintic-eased blank transit (0–64 points, scaled by L∞ distance), ~400 µs start dwell [V] | "Line Start" etc., start at minimum and raise [V] |
| **Corner dwell / anchor points** | "Angle: linear angle to anchor relation – number of anchor points in a 180 degree turn", or custom table for 100/40/60…180° turns; "angle optimization works for visible lines only" [V] | "Corner settle (µs) ... Scales automatically with turn angle"; 0 disables [V] | Min hold at 180° (straight), max hold at 0° (reversal), custom lookup; `NumRepeatPoints = HoldAdd + HoldLookupFactor × H` [V] | `corner_dwell` repeats when cos(angle) ≤ `curve_angle`, else `curve_dwell`; simple.c: corner_dwell 8, curve_angle cos(30°) [V] | n/a | "Corner-Points" [V] |
| **Interpolation / max step** | "Visible lines spacing", "Blanked lines spacing"; points added when neighbours ≥1.5 spaces apart; count = round((A−B)/Spacing); optional nonlinear spacing for blanked lines [V] | "Drawing speed" u/s presets Detailed 100 / Balanced 200 / Fast 400 (max lit traversal velocity); "Travel speed" (blanked) Gentle 600 (default) / Balanced 800 / Aggressive 1500; "Motion response" 30/50/70 % [V] | `stepsize` (lit) and `bstepsize` (blanked) max distance per sample [V] | `on_speed`, `off_speed` = max distance per point; points = ceil(dist/speed) with Chebyshev distance; simple.c on_speed 0.02, off_speed 0.1 [V] | n/a | "Max.Distance ... Leave the settings for Max.Distance unchanged!"; "Extra-Points" [V] |
| **Min points per frame / flicker floor** | "Minimum number of points" default 200; FB3 hardware minimum 128; sometimes 500/700 [V] | optical refresh = PPS / points; badge amber <35 fps, red <20 fps [V] | n/a | `max_framelen` resamples a too-long frame, never dropping blank points [V] | Helios: "Frames should be 10 milliseconds long at an absolute minimum, but 20-40ms is recommended" [V] | n/a |
| **Size / scale** | Size & position in %, "100×100" full; "~95% ... for beam show content and 50% size for graphic content" [V] | projection map or scale knob; "reducing the scan area improves line accuracy" [V] | `xscale`, `yscale`, `rotate` [V] | `olSetScissor` [V] | n/a | n/a |
| **Invert / swap** | "Invert X, Invert Y, and Swap XZ" checkboxes (BEYOND) [V] | n/a on fetched pages | `swap` toggle [V] | n/a | n/a | n/a |
| **Wide-angle derating** | "Wide angle compensation" checkbox, recommended on, "may slow scans beyond 50% size" [V] | n/a | n/a | n/a | n/a | n/a |
| **Colour levels** | TTL / analog linear / analog log; min & max voltage per channel [V] | TTL/Analog profile; min voltage (diode threshold), max voltage (white balance), gamma default 2.2 per channel [V] | red/green/blue scale [V] | `RENDER_GRAYSCALE` flag [V] | n/a | colour mapping |
| **Geometric correction** | in Projection Zones (keystone etc.), not detailed on fetched pages [V] | projection map [V] | via SOP transforms | n/a | n/a | n/a |
| **Safety zones / BAM** | Beam Attenuation Map 64×64, 0–100 % per cell; projection zones [V] | "define safe projection zones" (procedural, not enforced: "Modulaser is creative software, not a safety device") [V] | n/a | n/a | n/a | n/a |
| **Dot/stationary cap** | n/a on fetched pages | "Dot dwell (DAC samples) ... A safety cap on stationary beams limits the effective length" [V] | n/a | n/a | laser-dac-rs `IdlePolicy::Blank` default, `Park{x,y}`; startup blank 1 ms [V] | n/a |

Sources for the table: [V] https://wiki.pangolin.com/doku.php?id=quickshow:projector_settings ; [V] https://wiki.pangolin.com/doku.php?id=beyond%3Aprojector_settings-new ; [V] https://modulaser.app/docs/guides/improve-laser-output ; [V] https://modulaser.app/docs/outputs/helios-dac ; [V] https://docs.derivative.ca/Laser_CHOP ; [V] https://raw.githubusercontent.com/marcan/openlase/master/include/libol.h ; [V] https://raw.githubusercontent.com/marcan/openlase/master/libol/libol.c ; [S→V] https://github.com/marcan/openlase/blob/master/examples/simple.c (values from search summary) ; [V] https://github.com/ModulaserApp/laser-dac-rs ; [V] https://raw.githubusercontent.com/Grix/helios_dac/master/sdk/cpp/HeliosDac.h ; [V] https://www.showeditor.com/en/tutorials-faq/tutorials/248-laser-output-quality-optimization.html

LSX and LaserBoy: I could not fetch primary documentation. LaserBoy's optimiser is described on its forum as governed by "max lit distance, max blank distance, max dwell" / "maximum distance between lit points" and "maximum distance between dark points", with dwell derived from the approach/departure angle of each vertex triple. [S] https://laserboy.org/forum/index.php?topic=121.0 ; [S] https://www.photonlexicon.com/forums/showthread.php/22910-Laserboy-Optimize-or-Minimize . LSX: only its colour mapping/linearity is described on the vendor page. [S] https://innolasers.com/shop/index.php?id_product=18&controller=product

Tuning order recommended by Modulaser's wizard: colour shift first, then voltages and gamma, then motion (PPS, scan preset, dwells), then "Review the full ILDA test pattern." [V] https://modulaser.app/docs/guides/improve-laser-output. Showeditor: set all optimisation options to minimum, raise one at a time on the "Ilda12K" figure, "Listen to the scanners, and if they tend to make a screaming noise, reduce the value that caused it." "Be careful: Wrong settings can destroy your galvo scanner system!" [V]

---

## 3. Point-optimisation algorithms (concrete code and formulas)

### 3.1 OpenLase libol (the clearest open implementation) [V] https://raw.githubusercontent.com/marcan/openlase/master/libol/libol.c

Coordinates are normalised (the header documents nothing; the −1..1 convention is inferred from the scissor API and examples [U]).

- **Segment interpolation (lit):** in `line_to`:
  ```c
  float distance = fmaxf(fabsf(dx),fabsf(dy));     // Chebyshev distance
  int points = ceilf(distance/params.on_speed);
  for (i=1; i<=points; i++) { ... }
  ```
  So `on_speed` is "max step per point as a fraction of the coordinate range". simple.c's `on_speed = 0.02` gives ≥100 points across a full-width (2.0) line; `off_speed = 0.1` gives 20 blank points for a full-width jump.
- **Corner dwell:** `get_dwell(x,y)` computes the normalised dot product of the incoming and outgoing direction; `if (dot > params.curve_angle) return curve_dwell; else return corner_dwell;` (zero-length → corner_dwell). `line_to` then repeats the last point `dwell` times before drawing the new segment. simple.c: `corner_dwell = 8`, `curve_angle = cos(30°)`, i.e. any turn sharper than 30° gets 8 repeated points; gentler turns get `curve_dwell`.
- **Start/end dwell:** first point of an object repeated `start_dwell` times (`point_to`); in `olEnd` the last point is repeated `end_dwell` times. simple.c: 3 and 3.
- **Blanking jump between objects** (`render_object`): if the jump exceeds `snap`, emit `end_wait` blank points at the old position, `ceil(distance/off_speed)` interpolated blank points, then `start_wait` blank points at the new position. Jumps shorter than `snap` get no blanking.
- **Frame length cap:** `max_framelen` resamples lit points (`delta = count/out_count`) but "Blank points are never skipped during resampling."
- Flags: `RENDER_GRAYSCALE`, `RENDER_NOREORDER` (object reordering for shorter jumps), `RENDER_NOREVERSE` (allow drawing an object backwards), `RENDER_CULLDARK`.

### 3.2 TouchDesigner Laser CHOP (time-based parameters) [V] https://docs.derivative.ca/Laser_CHOP

- Corner repeats: `NumRepeatPoints = HoldAdd + HoldLookupFactor × H`, where H is interpolated (linear by default, or a lookup CHOP) between **Minimum Corner Hold** at 180° (straight) and **Maximum Corner Hold** at 0° (full reversal).
- Separate **Step Size** (lit) and **Blanking Step Size** (blanked) as maximum distance per sample.
- Blanking in milliseconds: pre/post blanking-on delay (wait before/after turning colour *off*), pre/post blanking-off delay (before/after turning colour *on*), plus **Color Delay** (ms) and **Closed Shape Overlap** (ms) for closed paths.

### 3.3 Pangolin spacing rule [V] https://wiki.pangolin.com/doku.php?id=beyond%3Aprojector_settings-new

- Extra points are added to a visible line when the gap is ≥1.5 spaces; "Point count is the rounded value of (A−B)/Spacing." Blanked lines have their own spacing and an optional nonlinear (eased) distribution. Anchors are added "where visible and blanked segments meet, not between two visible lines or two blanked lines" (from the same wiki, via search summary [S]). Angle optimisation: "number of anchor points in a 180 degree turn", linear in angle, or a custom per-angle table.

### 3.4 Modulaser (velocity-planned, time-based) [V] https://modulaser.app/docs/guides/improve-laser-output

- Lit path planned by **Drawing speed** (max lit velocity, laser units/s: 100/200/400 presets) and **Motion response** %; "Straight segments get wide point spacing at cruise speed; curves and corners get dense clusters." Corner settle in µs scales with turn angle. Blank travel by **Travel speed** 600/800/1500 u/s. Dwells in µs converted to samples at the current PPS (defaults 100/0/600/65 µs). Colour shift 0–500 µs.

### 3.5 laser-dac-rs frame transitions [V] https://github.com/ModulaserApp/laser-dac-rs

- Between frames: end dwell ≈100 µs at the source, a quintic-eased blanked transit of 0–64 points scaled by L∞ distance, start dwell ≈400 µs at the destination; "Dwell durations are converted to point counts based on PPS." `TransitionPlan::Coalesce` for closed shapes avoids a visible halt. On frame-swap DACs (Helios 4095 points) the transition prefix is trimmed first; authored content is never dropped.

### 3.6 Converting between "points" and "time"

All of the above agree implicitly: physical scanner latency and settling are in **microseconds**, so parameters expressed in points scale with PPS. Pangolin's folk rule (3 points @20K, 4 @30K, 5 @40K) is ≈130–150 µs; Modulaser's "typical 50–200 µs, start at 100 µs" matches; ILDA's "blanking shift of positive five" at 30K is ≈167 µs. A firmware should store latency-type settings in µs (or samples at a reference rate) and convert to samples at the active rate.

### 3.7 Historical caution about optimisation and scan rate

LaserFX/Lanling: pick one standard speed and stick to it; content authored for 30K tuned scanners can be "projected at a slower speed than the tuning speed, for example 8K instead of 30K, to avoid too many points at sharp angles and corners" (1990s advice; modern software re-optimises instead). [V] https://www.lanlingtech.com/news/15.html

---

## 4. Galvo protection practice

### 4.1 Speed vs angle vs heat

- PT-30K published derating table (±24 V, 5×10 mm mirror) [V] https://www.showlasershop.eu/images/PT-30K.pdf:

  | Optical deflection | Speed (ILDA pattern shown correctly) |
  |---|---|
  | 20° | >30,000 pps |
  | 15° | >32,000 pps |
  | 10° | >35,000 pps |
  | 8° | >40,000 pps |
  | 5° | >50,000 pps |

  Also: "the galvos are not set for maximum speed at small scan angle, but set to a usable angle of 20° at 30Kpps"; "If the galvos gets very hot (above 45 °C / 115 °F), switch off the system"; "maximum current per phase should be in average below 700mA"; "Do NOT run them without heatsink!!"
- Forum rule of thumb: speed costs heat and angle, "square" relationship; tuning to 40K instead of 30K ≈ (40/30)² = 1.8× heat and angle ≈60 % of the 30K angle; a driver that hits its thermal budget enters "power limiting" and shrinks the image. [S] https://www.photonlexicon.com/forums/showthread.php/4662-What-Makes-Galvo-s-Faster (not fetched) — [U] indicative only.
- ScannerMAX: overheating is "the most common way conventional scanners are destroyed" and heat is "typically the limiting factor on scan speed." [S] https://scannermax.com/blogs/education/stronger-cooler-faster-explained (429 on fetch).
- Pangolin: "power limiting ... the servo driver reduces the size so that no overheating will occur"; the 6215H coil "virtually eliminated [power limiting] for normal light show content." [V] http://www.pangolinlegacy.com/laserists%20articles/HTML/Cambridge_Scanner_Makes_Performance_Breakthrough.html
- ScannerMAX step data: Saturn 5B 24° optical step in 520 µs at ±24 V; Mach-DSP peak coil voltage ~20 V caps faster steps. [S] https://download.scannermax.com/pdf/Saturn5B_StepTimes.pdf
- Pangolin QuickShow: "most scanners can do their rated scan speeds at full size for beam show content and about half size (50%) for graphic content"; the Minimum/Maximum sample-rate sliders "act as a hard ceiling, since a high sample rate may damage the scanners in your projector." [S/V] https://wiki.pangolin.com/doku.php?id=quickshow:projector_settings
- The ILDA document itself: "when images begin to break-up this is a warning that harm may be near. Never allow scanners to oscillate!" [V]

I found **no manufacturer-published universal curve** like "30K at 8°, 20K at 20°, 10K at 40°"; the PT-30K table above is the only concrete one fetched, and it is for one product. Treat any derating curve in firmware as user-editable, seeded from the user's own scanner datasheet.

### 4.2 Content hazards

- Square waves / high-frequency content: the risk is sustained high acceleration at a fixed frequency (raster-like turnarounds, beam "fans" at high rate), which drives coil heating; patents and ScannerMAX notes discuss turnaround acceleration and heat; Showeditor's practical test is audible: "if they tend to make a screaming noise, reduce the value". [V] Showeditor; [S] https://www.photonlexicon.com/forums/showthread.php/4662-What-Makes-Galvo-s-Faster
- "Overscan": not found as a defined term in any fetched standard. In vendor usage it means driving beyond the rated angle for the speed; the ILDA procedure's "10% under distortion" margin is the standardised equivalent. [U]
- "DC hold": not found as a defined term. The closest standard behaviours are Ether Dream's Idle state (outputs at 0 V) and laser-dac-rs's `IdlePolicy::Blank` (blanked points at origin) vs `Park{x,y}`. [V] https://ether-dream.com/protocol.html ; [V] https://github.com/ModulaserApp/laser-dac-rs

### 4.3 Typical DAC/projector safety behaviours observed in the field

| Behaviour | Where seen |
|---|---|
| Blank (laser off) when the frame/stream ends with no successor | Helios `HELIOS_FLAGS_SINGLE_MODE`: "If a new frame has not arrived before the currently playing one is done, the laser will turn off." Default without the flag is to loop the last frame. [V] HeliosDac.h |
| Blank on empty frame / on disconnect | laser-dac-rs: empty frame "sends a single blanked point at origin"; Modulaser disarms Helios on disconnect: "moves the beam to its park position, blanks it, and closes the shutter." [V] |
| Idle outputs at centre | Ether Dream Idle: "Outputs sit at 0v"; shutter controlled by the data source. [V] https://ether-dream.com/protocol.html |
| E-stop state latched, cleared explicitly | Ether Dream: E-stop by input, network command (byte 0x00 or 0xFF), **or any unrecognised command**, or over-temperature; cleared by 'c' which NAKs if the cause persists. Underflow and E-stop flags recorded in `playback_flags`. [V] |
| Hard ceiling on sample rate | Pangolin min/max sliders; Helios caps at 65,535 and "frames play slower than Modulaser planned them" above it; laser-dac-rs rejects out-of-range `set_pps` rather than clamping. [V] |
| Stationary-beam cap | Modulaser "Dot dwell ... A safety cap on stationary beams limits the effective length." [V] |
| Startup blanking | laser-dac-rs default 1 ms startup blank. [V] |
| Projector-side interlock loop | ISP-DB25 Interlock A/B: "If this loop is broken, it is required that projectors close a shutter or turn off the laser." Shutter pin: "0 volts shall close the shutter ... +5 volts shall fully open." [V] ISP p. 11 |
| Scan-fail in the amp | ILDA: "Usually these are built into a scanner amplifier"; Mach-DSP exposes over-position/over-temperature fault outputs. [V] |

---

## 5. Laser output safety relevant to idle/stop behaviour

### 5.1 ILDA guidance (hardware list and audience-scanning pages)

- ILDA Hardware Safety List Rev. 7 (2005, based on IEC 60825-1 ed. 1.2) Table 1, for Class 3B/4 display projectors: protective housing; external interlock to activate emission ("Remote Interlock between Laser Projector and the Control System"); keyswitch "shall not be removable when in the ON position"; laser emission indicator ("fail-safe or redundant"); mechanical shutter / beam stop; **scan-fail device for audience scanning**; Class 4: manual reset "following an interruption to emission caused by mains power or interlock loss" (mains interruption longer than 5 s). [V] https://www.ilda.com/resources/ILDA-safety7b.pdf
- Scanning safeguard, quoted from IEC 60825-1 §4.10 (ed. 1.2): "Laser products intended to emit scanned radiation, and classified on this basis, shall not, as a result of scan failure or of variation in either scan velocity or amplitude, permit human access to laser radiation in excess of the AEL for the assigned class." [V] same PDF. In the 2014 edition the clause is numbered **6.11 "Scanning safeguard"** (6.10 is viewing optics) according to a third-party EN 60825-1:2014 test report. [S] https://cdn1.midocean.com/document/test-performance/mo8097-test-performance.pdf — [U] clause number unverified against the standard itself.
- Scan-fail timing: "An ideal scan fail device should trigger in 1 millisecond. Many devices are in the 20 millisecond range" (acceptable, not ideal). Must also catch the two-dot case: "The scanner has not stopped, but this can be unsafe -- each dot has 1/2 the total laser power." "Continuous scanning shows MUST be well-designed so that the beam is constantly moving. There should never be just a 'dot' of light, or a few dots, in the audience." When shrinking a shape to a point, "fade the beam out during the shrink." [V] https://www.ilda.com/audiencescanningtips.htm
- Requirements to rely on scanning as a safety factor: "a working, valid scan fail detection system such as Pangolin's PASS" that will "shut off the beam within at least 10 milliseconds and preferably within 1 msec", a show with no hot spots, continuous operator monitoring with an E-stop. MPE figures at the closest point of human exposure: static beam 2.5 mW/cm² (¼-s MPE), continuously scanned 10 mW/cm², ILDA "Level 2/10x" proposal 100 mW/cm² (not law anywhere). "Some laser software automatically adds blanking points when the beam is a dot." [V] https://www.ilda.com/audiencescanningsafety.htm
- "NEVER scan audiences with pulsed lasers"; FDA variance required in the US for shows over 5 mW. [V] audiencescanningtips

### 5.2 Regulatory framing (summary only)

- US: 21 CFR 1040.11(c) limits laser products promoted for demonstration to Class IIIa; Class IIIb/IV light-show projectors and shows require an FDA variance; variance conditions include a scanning safeguard that "detects scanner motion directly and reacts quickly enough to keep emissions under the limit", trained operators, audience-scanning limits, a 3.0 m height threshold. [S] https://www.fda.gov/media/72256/download ; https://downloads.regulations.gov/FDA-2021-V-0986-0001/attachment_1.pdf ; https://www.fda.gov/regulatory-information/search-fda-guidance-documents/responsibilities-laser-light-show-projector-manufacturers-dealers-and-distributors-laser-notice-51 (eCFR itself blocked the fetcher).
- ILDA notes FDA Laser Notice 50 lets IEC-compliant products be treated as compliant with Part 1040. [V] ILDA-safety7b.pdf
- Modulaser's stance as a software vendor: "Modulaser is creative software, not a safety device"; "Use and test projector interlocks and scan-fail protection before operation"; "Keep a physical emergency stop within reach and test it at low power"; "Modulaser prohibits audience scanning, even where local law may permit it." [V] https://modulaser.app/docs/laser-safety

### 5.3 What this implies for a DAC firmware (hobbyist board, safe defaults)

The DAC is not the classified product, but it is the only element that knows whether valid data is flowing. Safe defaults that every fetched source either requires or practises:
1. **Blank when there is nothing valid to play**: end of stream, underrun, host disconnect, invalid/unknown command, startup. Never loop the last frame by default (Helios loops by default; Ether Dream, laser-dac-rs and Modulaser do not).
2. **Latch an E-stop/fault state** that must be explicitly cleared; report "underflow" vs "stopped" vs "e-stop" in status (Ether Dream model).
3. **Do not emit a lit stationary beam**: cap how long the output may stay lit while position change stays below a threshold; blank, do not merely dim.
4. **Treat the shutter/interlock as the hard stop** and the blanking channels as the fast soft stop; the ISP shutter is 0 V = closed.
5. **Clamp the sample rate** to a user-set ceiling and refuse (not silently clamp) out-of-range requests from clients, logging them.
6. Keep all of this on the MCU that owns the DAC timing, so it works regardless of what the network side does.

---

## (a) Proposed, prioritised "standard ILDA tuning" feature list

Board split assumed: **ESP** = ESP32-S3 (network, SD, web UI, protocol receivers, test streams); **RP** = RP2354 (ring buffer, DAC timing, 20 ms blank-on-stop). Settings persist on the ESP and are pushed to the RP over UART at boot and on change; avoid RP flash writes (the existing `BUFFER` command already shows the cost).

### Tier 1: needed before any tuning can be trusted

| # | Feature | Parameter (UI name) | Unit | Range | Default | MCU | Why / source |
|---|---|---|---|---|---|---|---|
| 1 | ILDA Test Pattern generator in the Test Stream section, rate-locked | `test_pattern = ilda_12k / ilda_30k`; `test_rate` | pps | 12,000 or 30,000 ±1 % (also free 1k–100k for "fast" checks) | 30,000 | ESP generates; RP plays at exact rate | The pattern only works at 12k/30k ±1 % [V ILDA p.3]; generate from geometry so no .ild licensing/file issues; include the boxed variant option. |
| 2 | Hard sample-rate ceiling | `max_pps` | pps | 1,000–100,000 | 30,000 | **RP enforces**, ESP mirrors in UI and refuses client RATE requests above it | Pangolin max slider "hard ceiling ... may damage the scanners" [V]; Helios/laser-dac-rs reject/clamp [V]. Reject, log, keep old rate. |
| 3 | Size / scale, X/Y invert, X/Y swap, X/Y offset | `size_x`, `size_y` (%), `invert_x`, `invert_y`, `swap_xy`, `offset_x`, `offset_y` | %, bool, % of full scale | 1–100 %, ±50 % | 100/100, off, off, 0/0 | **RP** (per-sample, applies to every source identically) | ILDA steps 2, 7, 8 and A4/A5/A6 [V]; Pangolin "95 % beams / 50 % graphics" [V]; BEYOND invert/swap [V]. Offset only for fine centring ("Make only minor adjustments with DC offsets" [V]). |
| 4 | Colour/blanking shift (delay colour vs XY) | `color_shift_us` (show derived samples) | µs (stored), samples at active rate (applied) | −500…+500 µs (positive = colour delayed) | 130 µs (≈4 samples @30K) | **RP** (sample delay line on R/G/B/I channels) | Pangolin 3/4/5 points @20/30/40K [V]; Modulaser 50–200 µs, start 100 [V]; ILDA "+5 shift" for AO [V]. Must sit after the ring buffer so it applies to all protocols. |
| 5 | Blank-on-everything idle policy | `idle_policy = blank_hold / blank_center`; `idle_center_ramp_ms` | enum, ms | ramp 0–500 | `blank_center`, 50 ms ramp | **RP** | Ether Dream idle at 0 V [V]; laser-dac-rs Blank@origin default [V]; Modulaser park on disarm [V]. Blank first, then glide to centre to avoid a step. Already have 20 ms blank-on-stop; keep ≤20 ms. |
| 6 | Status fields for safety state | `stopped_reason = stop / underflow / estop / fault / no_signal`, `estop_latched` | enum | – | – | RP reports, ESP exposes in `/api/rp/status` | Ether Dream `playback_flags` + `light_engine_state` [V]. |

### Tier 2: the "projector settings" page proper

| # | Feature | Parameter | Unit | Range | Default | MCU | Source |
|---|---|---|---|---|---|---|---|
| 7 | Per-channel colour delay (fine) | `color_shift_r/g/b_us` added to #4 | µs | ±200 | 0 | RP | Showeditor per-colour shift [V]; Modulaser wizard colour-shift step [V]. |
| 8 | Colour mapping | `color_mode = ttl / linear / log`; `color_min_v[ch]`, `color_max_v[ch]`; `gamma[ch]` | V, – | 0–5 V; γ 1.0–3.0 | linear, 0 V, 5 V, 2.2 | RP | Pangolin TTL/linear/log + min/max V [V]; Modulaser diode threshold/white balance/gamma 2.2 [V]. Note ISP colour is 0–5 V differential [V]. |
| 9 | Stationary-beam (scan-fail-lite) guard | `beam_still_max_ms`; `beam_still_threshold` | ms; % of full scale | 1–100 ms; 0.1–5 % | 20 ms; 0.5 % | **RP** (monitors output XY while any colour > 0) | ILDA: ideal 1 ms, 20 ms common/acceptable, catch two-dot case [V]; Modulaser dot-dwell cap [V]. Document that this is *not* a certified scan-fail device (that lives in the amp). Make it switchable off only with an explicit "I know" toggle. |
| 10 | Loss-of-signal / watchdog | `no_data_blank_ms` | ms | 10–1000 | 100 | RP (SPI silence) and ESP (socket/stream death → STOP) | Helios single-mode "laser will turn off" [V]; Modulaser disarm on disconnect [V]. |
| 11 | Minimum points per frame / flicker floor (for frame-based sources: ILDA player, test streams) | `min_frame_points` | points | 0–2000 | 200 | ESP (pads frames by repeating the last blanked point or resampling) | Pangolin default 200 (FB3 min 128) [V]; Helios "≥10 ms, 20–40 ms recommended" [V]. |
| 12 | Scan-rate derating by size ("wide-angle compensation") | `max_pps_full_size`, `size_for_max_pps` (%) or a 3-point table angle→pps | pps, % | – | off (table empty) | ESP computes effective ceiling; RP enforces resulting `max_pps` | Pangolin wide-angle compensation beyond 50 % size [V]; PT-30K table 20°/30k…5°/50k [V]. Keep user-editable; no universal curve exists. |

### Tier 3: point optimisation (only for sources the ESP itself produces: ILDA files from SD, test patterns, future local shapes). **Never re-time Ether Dream/IDN/ILW streams**: their clients already optimised and pace by point count (`buffer_fullness`).

| # | Feature | Parameter | Unit | Range | Default | MCU | Source |
|---|---|---|---|---|---|---|---|
| 13 | Lit-segment interpolation | `max_lit_step` | % of full scale per sample | 0.1–10 % | 1 % (≈OpenLase on_speed 0.02 of a 2.0 range) | ESP | OpenLase `ceil(dist/on_speed)`, Chebyshev [V]; Pangolin `round((A−B)/spacing)` [V]; TD `stepsize` [V]. |
| 14 | Blanked-jump interpolation | `max_blank_step` | % | 0.5–50 % | 5 % (OpenLase 0.1/2.0) | ESP | OpenLase off_speed [V]; Pangolin blanked spacing with optional nonlinear easing [V]; laser-dac-rs quintic 0–64 pts [V]. |
| 15 | Corner dwell by angle | `corner_dwell_max` (at 0°/reversal), `corner_dwell_min` (at 180°/straight), `corner_angle_threshold` | samples (or µs), degrees | 0–20; 0–180° | 8 / 0 / 30° | ESP | OpenLase corner_dwell 8 @ >30° [V]; TD linear min(180°)→max(0°) [V]; Pangolin "anchors per 180° turn" [V]. Formula: `repeats = round(min + (max−min) × (180°−θ)/180°)` for θ < threshold else `min`. |
| 16 | Lit start/end dwell and blank lead-in/out | `lit_hold_before_blank_us`, `blank_hold_before_travel_us`, `blank_hold_after_travel_us`, `lit_hold_after_travel_us` | µs | 0–1000 | 100 / 0 / 600 / 65 | ESP | Modulaser defaults [V]; OpenLase start/end_dwell 3, start/end_wait [V]; TD pre/post blanking delays [V]. |
| 17 | Frame-length cap | `max_frame_points` | points | 500–10,000 | 4,000 | ESP | OpenLase `max_framelen` never drops blank points [V]; Helios 4095 [V]. |

### Tier 4: nice-to-have

| # | Feature | Parameter | MCU | Source |
|---|---|---|---|---|
| 18 | Colour-shift test figure (two lines switching colour at a reference tick; Showeditor-style triangle) | `test_pattern = color_shift` | ESP | Showeditor BlankShift_EachSide [V]; Modulaser wizard step 2 [V]; ILDA B1/B2 [V]. |
| 19 | Horizon clip / simple safety zone | `blank_below_y` (% of full scale), `zone_mode = blank / clamp` | RP | ILDA "beams above audience eye level", BAM concept [V]. Blank, don't clamp, by default. |
| 20 | Keystone / 4-corner geometric correction | 4 corner offsets | RP (per-sample bilinear) or ESP | Pangolin projection zones [V]. Lowest priority for a hobbyist DAC. |
| 21 | Soft-start / ramped size on START | `start_ramp_ms` | RP | No standard; inferred from laser-dac-rs startup blank 1 ms and idle glide [U]. Blank during ramp. |
| 22 | Scanner thermal/duty hint | running RMS of |Δposition| × rate as a "scanner load" gauge in the UI | RP computes, ESP shows | PT-30K 700 mA / 45 °C limits, "square" heat law [V/S]. Informational only. |

---

## (b) The 10 most decision-relevant facts

1. **The test pattern is defined only at 12,000 or 30,000 pps ±1 %** and "defines only the rate which must be used during scanner adjustment"; it says nothing about the rate shows must use. A firmware test-pattern generator must hit the rate exactly and should offer both. [V] ILDA Test Pattern p. 2–3.
2. **Correct tuning = circle touches the inner square at four midpoints and is round; tune at ≤8° optical for 30K (≤15° for 12K), sized 10 % under where the circle starts to distort.** Circle inside the square = too fast (or servo gain too low); outside = too slow. [V] ILDA p. 4–7; [V] pangolinlegacy.
3. **"NK" scanner ratings mean the pattern is correct at N kpps at ≤8°**; speed and angle trade off (one vendor table: 20°→30k, 15°→32k, 10°→35k, 8°→40k, 5°→50k), so a single `max_pps` without size context under-protects at full size and over-restricts at small size. [V] pangolinlegacy; [V] PT-30K.
4. **Servo gain, LF/HF damping, size, offset, linearity live in the amp; the DAC should not try to emulate them.** The DAC's levers are rate, size, inverts, colour latency and point density. [V] PT-30K legend; [V] Mach-DSP manual.
5. **Colour/blanking latency is a time, not a point count**: vendors converge on ≈100–170 µs (Pangolin 4 points @30K, Modulaser "start around 100 µs", ILDA "+5" @30K). Store it in µs and convert to samples at the active rate on the DAC-timing MCU, after the ring buffer, so every protocol gets it. [V] Pangolin wiki; [V] Modulaser; [V] ILDA p. 10.
6. **Point optimisation is simple and well documented in OpenLase**: `points = ceil(Chebyshev_dist / max_step)`, corner repeats when `cos θ ≤ cos(threshold)` (default 8 repeats, 30°), start/end dwell 3, blank jumps padded with `end_wait`/`start_wait`. TouchDesigner adds the angle-linear form `repeats = add + factor × lerp(minHold@180°, maxHold@0°)`. [V] libol.c; [V] Laser CHOP.
7. **Do not optimise or re-time network streams.** Ether Dream clients pace from `buffer_fullness`, which must count exactly what they sent; IDN/ILW have no flow control. Optimisation belongs only to frames the ESP itself originates (ILDA files, test patterns). [V] laser-dac-rs trims only its own transition prefix, never authored content; this firmware's Ether Dream pacing (`etherdream_protocol.c`).
8. **Safe idle is "blank, then park at centre," and stream end without a successor should blank, not loop.** Ether Dream Idle outputs 0 V; laser-dac-rs defaults to blanked points at origin; Helios only blanks at frame end if `SINGLE_MODE` is set (its default loops). [V] ether-dream.com; [V] laser-dac-rs; [V] HeliosDac.h.
9. **ILDA's scan-fail numbers: ideal 1 ms, 20 ms is common and acceptable, ≤10 ms to rely on scanning for safety; it must also catch a beam alternating between two dots.** A DAC-side stationary-beam guard with a ~20 ms window is in line with practice but is not a substitute for the amp's scan-fail device; say so in the UI. [V] ilda.com audience scanning pages.
10. **The standards' hardware model for the projector**: ISP shutter 0 V = closed/+5 V = open; interlock loop broken → "close a shutter or turn off the laser"; IEC 60825-1 scanning safeguard forbids access above the AEL "as a result of scan failure or of variation in either scan velocity or amplitude"; Class 4 needs manual reset after interlock/mains loss. Firmware should latch faults and require an explicit clear (Ether Dream's 'c' model). [V] ISP p. 11; [V] ILDA-safety7b; [V] ether-dream.com.

---

## Fetch log (for provenance)

Fetched and read: ilda.com technical.htm; ILDA_TestPattern95_rev002.pdf (rendered 12 pages); ILDA_ISP99_rev002.pdf; ILDA-safety7b.pdf; audiencescanningsafety.htm; audiencescanningtips.htm; pangolinlegacy.com/ILDAtest.html; pangolinlegacy Cambridge 6215 article; wiki.pangolin.com quickshow:projector_settings, beyond:projector_settings-new, lcmax tips; modulaser.app improve-laser-output, outputs/helios-dac, laser-safety; marcan/openlase libol.h and libol.c; ModulaserApp/laser-dac-rs README; Volst/laser-dac README; Grix/helios_dac README and sdk/cpp/HeliosDac.h; ether-dream.com/protocol.html; docs.derivative.ca Laser_CHOP; showeditor.com optimisation tutorial; showlasershop.eu PT-30K.pdf; edmundoptics ScannerMAXManual.pdf (Mach-DSP); lanlingtech.com/news/15.html.

Refused or rate-limited (facts used only via search summaries, marked [S]): pangolin.com blog (429), nfllaser.com (429), scannermax.com blog (429), starshinelights.com (429), laserfx.com (ECONNREFUSED), photonlexicon.com (ECONNREFUSED), laserboy.org forum (503), bitlasers.com (DNS), ecfr.gov (bot wall), web.archive.org (blocked by tool). Not fetched: Sino-Galvo/RayScan datasheets (search found no 8°/kpps ratings for them), Cambridge 6215H datasheet.
