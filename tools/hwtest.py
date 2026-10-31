#!/usr/bin/env python3
"""Hardware behaviour tests for the ILDAWaveX16 V2 board.

Each test drives the board through one of the repo's protocol senders and
measures the outcome from /api/rp/status (RP2354 state read over UART) and the
Ether Dream status replies. The laser IS driven: run only with the beam on a
backstop or the laser off.

  python3 tools/hwtest.py --host ildawavex16v2.local list
  python3 tools/hwtest.py --host ildawavex16v2.local idle rate-request
  python3 tools/hwtest.py --host ildawavex16v2.local --repeat 20 ed-stop-cycle
  python3 tools/hwtest.py --host ildawavex16v2.local all

Every test refuses to start while anything is already outputting. Results are
printed as tables and appended as JSON lines to logs/hwtest-YYYYMMDD.jsonl.

ILW is left out on purpose: an ILW CONFIG rewrites the RP's buffer watermarks
in flash whenever they differ from the ILW constants.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from etherdream_sender import EtherDreamSender, POINT_STRUCT, recv_exact  # noqa: E402
from idn_sender import IDNSender  # noqa: E402
from ilw_sender import LaserPoint, make_circle, wait_until_precise  # noqa: E402

API_TIMEOUT_S = 4.0
ED_STATUS = struct.Struct("<BBBBHHHHII")
ED_PB_FLAG_UNDERFLOW = 1 << 1


# -- status API --------------------------------------------------------------

def api(host: str, path: str, method: str = "GET") -> str:
    req = urllib.request.Request(f"http://{host}{path}", method=method,
                                 data=b"" if method == "POST" else None)
    with urllib.request.urlopen(req, timeout=API_TIMEOUT_S) as r:
        return r.read().decode()


def status(host: str, refresh: bool = False) -> dict:
    """Board status. refresh=True forces a fresh STATUS read from the RP first."""
    if refresh:
        # A failed read still answers HTTP 200, with "ERR ..." as the body.
        reply = api(host, "/api/scan/refresh", "POST").strip()
        if not reply.startswith("OK"):
            raise RuntimeError(f"status refresh failed: {reply}")
    return json.loads(api(host, "/api/rp/status"))


def require_idle(host: str) -> dict:
    try:
        s = status(host, refresh=True)
    except (OSError, RuntimeError) as exc:
        raise SystemExit(f"refusing to test: cannot read the board state ({exc})")
    busy = [k for k in ("remote_running", "ilda_running", "test_stream_running") if s.get(k)]
    if busy:
        raise SystemExit(f"refusing to test: output already active ({', '.join(busy)})")
    return s


def wait_for(pred, timeout_s: float, poll_s: float = 0.05):
    """Poll pred() until truthy. Returns seconds waited, or None on timeout."""
    t0 = time.perf_counter()
    while True:
        if pred():
            return time.perf_counter() - t0
        if time.perf_counter() - t0 > timeout_s:
            return None
        time.sleep(poll_s)


def wait_stopped(host: str, timeout_s: float = 4.0):
    """Seconds until the RP reports not running with an empty buffer."""
    def done():
        s = status(host, refresh=True)
        return (not s["remote_running"]) and s["remote_buffer_fill_samples"] == 0
    return wait_for(done, timeout_s, poll_s=0.0)


class Sampler(threading.Thread):
    """Sample the cached status once a second and keep per-second rates."""

    def __init__(self, host: str, interval_s: float = 1.0):
        super().__init__(daemon=True)
        self.host = host
        self.interval_s = interval_s
        self.rows: list[dict] = []
        self._stop_evt = threading.Event()

    def run(self):
        prev = status(self.host)
        prev_t = time.perf_counter()
        while not self._stop_evt.wait(self.interval_s):
            try:
                cur = status(self.host)
            except OSError:
                continue
            t = time.perf_counter()
            dt = t - prev_t
            self.rows.append({
                "t": round(t, 2),
                "running": cur["remote_running"],
                "rate": cur["remote_sample_rate"],
                "fill": cur["remote_buffer_fill_samples"],
                "ed_pps": round((cur["stream_etherdream_samples"] - prev["stream_etherdream_samples"]) / dt),
                "idn_pps": round((cur["stream_idn_samples"] - prev["stream_idn_samples"]) / dt),
                "underruns_ps": round((cur["remote_underruns"] - prev["remote_underruns"]) / dt),
                "dropped": cur["remote_stream_dropped_packets"] - prev["remote_stream_dropped_packets"],
                "rx_timeouts": cur["remote_stream_rx_timeouts"] - prev["remote_stream_rx_timeouts"],
            })
            prev, prev_t = cur, t

    def stop(self):
        self._stop_evt.set()
        self.join(timeout=3)


# -- Ether Dream helpers -----------------------------------------------------

def ed_status(raw: bytes) -> dict:
    f = ED_STATUS.unpack_from(raw, 2)
    return {"le_state": f[1], "pb_state": f[2], "le_flags": f[4], "pb_flags": f[5],
            "fullness": f[7], "point_rate": f[8], "point_count": f[9]}


def ed_connect(host: str, tries: int = 24) -> EtherDreamSender:
    """Connect, retrying while the board finishes closing the previous client."""
    last = None
    for _ in range(tries):
        s = EtherDreamSender(host, quiet=True, timeout=5.0)
        try:
            s.connect()
            return s
        except (ConnectionError, OSError) as exc:
            last = exc
            s.close()
            time.sleep(0.25)
    raise ConnectionError(f"Ether Dream connect failed: {last}")


def ed_prefill(s: EtherDreamSender, points, count: int, pkt: int = 256) -> int:
    """Prepare and queue `count` points without Begin.

    Returns the index of the next point, for ed_feed(start=...): restarting
    the drawing at point 0 instead puts a jump in the stream.
    """
    s.stop()
    s.prepare()
    sent, idx = 0, 0
    while sent < count:
        batch = []
        for _ in range(min(pkt, count - sent)):
            batch.append(points[idx])
            idx = (idx + 1) % len(points)
        s.send_points_retry_full(batch)
        sent += len(batch)
    return idx


def ed_update_rate(s: EtherDreamSender, rate: int) -> bytes:
    _, _, raw = s.command(b"u" + struct.pack("<HI", 0, rate))
    return raw


def ed_build_data(s: EtherDreamSender, points) -> bytes:
    out = bytearray(b"d" + struct.pack("<H", len(points)))
    for p in points:
        out += POINT_STRUCT.pack(*s.encode_point(p))
    return bytes(out)


def ed_feed(s: EtherDreamSender, points, rate: int, seconds: float,
            target_fullness: int = 4000, pkt: int = 300, depth: int = 2, on_rate=None,
            start: int = 0) -> dict:
    """Keep the DAC's reported fullness near target for `seconds`.

    `depth` data commands may be in flight before the first ACK is read
    (Ether Dream allows pipelining). on_rate(elapsed_s) may return a new
    rate to apply with 'u'. Points are taken from index `start` on. Returns
    client-side statistics, including the per-command round trip (send to
    ACK) in ms, and "next_idx" for a following call.
    """
    idx = start
    t0 = time.perf_counter()
    end = t0 + seconds
    sent = 0
    inflight: list[tuple[float, int]] = []
    rtts: list[float] = []
    fullness_min, fullness_max = 1 << 30, 0
    last_rate = rate
    last_fullness = 0

    def read_ack():
        nonlocal fullness_min, fullness_max, sent, last_fullness
        t_sent, n = inflight.pop(0)
        raw = recv_exact(s.sock, 22)
        rtts.append((time.perf_counter() - t_sent) * 1000.0)
        if raw[0:1] != b"a":
            raise RuntimeError(f"data NAK {raw[0:1]!r} (fullness pacing too loose for pipelining)")
        sent += n
        st = ed_status(raw)
        last_fullness = st["fullness"]
        fullness_min = min(fullness_min, st["fullness"])
        fullness_max = max(fullness_max, st["fullness"])

    while time.perf_counter() < end:
        if on_rate:
            new_rate = on_rate(time.perf_counter() - t0)
            if new_rate and new_rate != last_rate:
                while inflight:
                    read_ack()
                ed_update_rate(s, new_rate)
                last_rate = new_rate
        batch = []
        for _ in range(pkt):
            batch.append(points[idx])
            idx = (idx + 1) % len(points)
        s.sock.sendall(ed_build_data(s, batch))
        inflight.append((time.perf_counter(), len(batch)))
        if len(inflight) >= depth:
            read_ack()
        if last_fullness > target_fullness:
            time.sleep((last_fullness - target_fullness) / last_rate)
    while inflight:
        read_ack()
    elapsed = time.perf_counter() - t0
    rtts.sort()
    return {"sent": sent, "client_pps": round(sent / elapsed), "depth": depth, "pkt": pkt,
            "rtt_ms_med": round(rtts[len(rtts) // 2], 1) if rtts else None,
            "rtt_ms_p95": round(rtts[int(len(rtts) * 0.95)], 1) if rtts else None,
            "rtt_ms_max": round(rtts[-1], 1) if rtts else None,
            "fullness_min": fullness_min, "fullness_max": fullness_max, "next_idx": idx}


# -- IDN helpers -------------------------------------------------------------

def idn_close(s: IDNSender) -> None:
    """Close the session. UDP: during a flood one close can be lost, so send three."""
    for _ in range(3):
        s.stop()
        time.sleep(0.02)


def idn_send_burst(s: IDNSender, points, count: int, rate: int, pkt: int = 64) -> int:
    sent, idx = 0, 0
    while sent < count:
        batch = []
        for _ in range(min(pkt, count - sent)):
            batch.append(points[idx])
            idx = (idx + 1) % len(points)
        s.send_points_packet(batch, rate)
        sent += len(batch)
    return sent


def idn_feed(s: IDNSender, points, rate: int, seconds: float, pace_factor: float = 1.0,
             pkt: int = 64) -> int:
    """Send packets declaring `rate`, but deliver them pace_factor times faster."""
    idx = 0
    sent = 0
    interval = pkt / rate / pace_factor
    next_send = time.perf_counter()
    end = next_send + seconds
    while time.perf_counter() < end:
        batch = []
        for _ in range(pkt):
            batch.append(points[idx])
            idx = (idx + 1) % len(points)
        s.send_points_packet(batch, rate)
        sent += pkt
        next_send += interval
        if next_send - time.perf_counter() < -0.25:
            next_send = time.perf_counter()
        wait_until_precise(next_send)
    return {"sent": sent, "client_pps": round(sent / (time.perf_counter() - (end - seconds)))}


# -- test framework ----------------------------------------------------------

class Result:
    def __init__(self, name: str):
        self.name = name
        self.rows: list[dict] = []
        self.checks: list[tuple[str, bool]] = []
        self.notes: list[str] = []

    def check(self, what: str, ok: bool):
        self.checks.append((what, bool(ok)))

    @property
    def passed(self) -> bool:
        return all(ok for _, ok in self.checks)


def print_table(rows: list[dict]):
    if not rows:
        return
    cols = list(rows[0].keys())
    widths = {c: max(len(c), *(len(str(r.get(c, ""))) for r in rows)) for c in cols}
    print("  " + "  ".join(c.rjust(widths[c]) for c in cols))
    for r in rows:
        print("  " + "  ".join(str(r.get(c, "")).rjust(widths[c]) for c in cols))


def report(res: Result, log_path: str):
    print(f"\n== {res.name}: {'PASS' if res.passed else 'FAIL'}")
    print_table(res.rows)
    for what, ok in res.checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {what}")
    for n in res.notes:
        print(f"  note: {n}")
    with open(log_path, "a") as f:
        f.write(json.dumps({"time": time.strftime("%Y-%m-%dT%H:%M:%S"), "test": res.name,
                            "pass": res.passed, "checks": res.checks, "rows": res.rows,
                            "notes": res.notes}) + "\n")


# -- tests -------------------------------------------------------------------

def t_idle(ctx) -> Result:
    """Read-only: board reachable, nothing running, RP initialised."""
    res = Result("idle")
    s = status(ctx.host, refresh=True)
    keep = ("esp_fw", "rp_fw", "initialized", "remote_running", "remote_sample_rate",
            "remote_buffer_fill_samples", "remote_buffer_size_samples",
            "remote_buffer_high_water_samples", "remote_buffer_low_water_samples",
            "remote_underruns", "remote_stream_dropped_packets", "remote_mirror_x", "remote_mirror_y")
    res.rows = [{k: s.get(k) for k in keep}]
    res.check("RP initialised", s["initialized"])
    res.check("output stopped", not s["remote_running"])
    res.check("buffer empty", s["remote_buffer_fill_samples"] == 0)
    return res


def t_rate_request(ctx) -> Result:
    """Ask the web API for rates above the galvo rating; see what the RP applies."""
    res = Result("rate-request")
    require_idle(ctx.host)
    for want in (30000, 50000, 60000, 100000, 200000):
        api(ctx.host, f"/api/rp/set_rate?value={want}", "POST")
        got = status(ctx.host, refresh=True)["remote_sample_rate"]
        res.rows.append({"requested": want, "applied": got, "within_galvo_max": got <= ctx.galvo_max})
    api(ctx.host, "/api/rp/set_rate?value=30000", "POST")
    res.check(f"every applied rate <= galvo max {ctx.galvo_max}",
              all(r["within_galvo_max"] for r in res.rows))
    res.check("restored to 30000", status(ctx.host, refresh=True)["remote_sample_rate"] == 30000)
    return res


def t_ed_begin_rate(ctx) -> Result:
    """Ether Dream Begin at 100 kpps with a blanked, small circle; what rate runs?"""
    res = Result("ed-begin-rate")
    require_idle(ctx.host)
    pts = [LaserPoint(p.x, p.y, 0, 0, 0, 0, blanked=True) for p in make_circle(3000, 256)]
    s = ed_connect(ctx.host)
    try:
        ed_prefill(s, pts, 1024)
        s.begin(100000, 0)
        _, _, raw = s.command(b"?")
        ed = ed_status(raw)
        time.sleep(0.15)
        st = status(ctx.host, refresh=True)
        res.rows.append({"begin_rate": 100000, "ed_reported_rate": ed["point_rate"],
                         "rp_rate": st["remote_sample_rate"], "rp_running": st["remote_running"]})
        s.command(b"s")
    finally:
        s.close()
    api(ctx.host, "/api/rp/set_rate?value=30000", "POST")
    stopped = wait_stopped(ctx.host)
    res.check(f"RP rate <= galvo max {ctx.galvo_max}", res.rows[0]["rp_rate"] <= ctx.galvo_max)
    res.check("client told the rate the RP really runs", res.rows[0]["ed_reported_rate"] == res.rows[0]["rp_rate"])
    res.check("stopped and empty afterwards", stopped is not None)
    return res


def t_ed_pacing(ctx) -> Result:
    """Ether Dream round trip per data command vs packet size (depth 1, 30 kpps)."""
    res = Result("ed-pacing")
    require_idle(ctx.host)
    pts = make_circle(8000, 360)
    s = ed_connect(ctx.host)
    try:
        idx = ed_prefill(s, pts, 3072)
        s.begin(30000, 0)
        for pkt in (100, 300, 600, 1000):
            feed = ed_feed(s, pts, 30000, 2.5, pkt=pkt, depth=1, start=idx)
            idx = feed.pop("next_idx")
            feed["sync_pps_limit"] = round(pkt * 1000.0 / feed["rtt_ms_med"]) if feed["rtt_ms_med"] else None
            res.rows.append(feed)
        s.command(b"s")
    finally:
        s.close()
    stopped = wait_stopped(ctx.host)
    res.check("stopped and empty afterwards", stopped is not None)
    res.notes.append("sync_pps_limit = points a one-command-at-a-time client could sustain")
    return res


def t_cancel_before_start(ctx) -> Result:
    """Queue points, cancel before the RP starts; the RP buffer must end empty."""
    res = Result("cancel-before-start")
    require_idle(ctx.host)
    pts = make_circle(8000, 360)
    for i in range(ctx.repeat):
        row = {"run": i + 1}
        s = ed_connect(ctx.host)
        try:
            ed_prefill(s, pts, 1024)
            s.begin(30000, 0)
            s.command(b"s")
        finally:
            s.close()
        time.sleep(0.4)
        st = status(ctx.host, refresh=True)
        row.update(proto="etherdream", running=st["remote_running"], fill=st["remote_buffer_fill_samples"])
        res.rows.append(row)

        row = {"run": i + 1}
        idn = IDNSender(ctx.host, quiet=True)
        try:
            idn_send_burst(idn, pts, 1024, 30000)
            idn_close(idn)
        finally:
            idn.close_socket()
        time.sleep(0.4)
        st = status(ctx.host, refresh=True)
        row.update(proto="idn", running=st["remote_running"], fill=st["remote_buffer_fill_samples"])
        res.rows.append(row)
    for proto in ("etherdream", "idn"):
        rows = [r for r in res.rows if r["proto"] == proto]
        clean = sum(1 for r in rows if not r["running"] and r["fill"] == 0)
        res.check(f"{proto}: {clean}/{len(rows)} runs left the RP stopped and empty", clean == len(rows))
    return res


def t_ed_stop_cycle(ctx) -> Result:
    """Start a short stream, stop it: time to running and time to stopped+empty."""
    res = Result("ed-stop-cycle")
    require_idle(ctx.host)
    pts = make_circle(8000, 360)
    base = status(ctx.host, refresh=True)
    for i in range(ctx.repeat):
        row = {"run": i + 1}
        s = ed_connect(ctx.host)
        try:
            ed_prefill(s, pts, 3072)
            t0 = time.perf_counter()
            s.begin(30000, 0)
            started = wait_for(lambda: status(ctx.host)["remote_running"], 3.0, poll_s=0.01)
            row["start_ms"] = None if started is None else round(started * 1000)
            time.sleep(0.3)
            s.command(b"s")
            t1 = time.perf_counter()
        finally:
            s.close()
        stopped = wait_stopped(ctx.host)
        row["stop_ms"] = None if stopped is None else round((time.perf_counter() - t1) * 1000)
        st = status(ctx.host, refresh=True)
        row["fill_after"] = st["remote_buffer_fill_samples"]
        row["running_after"] = st["remote_running"]
        row["dropped"] = st["remote_stream_dropped_packets"] - base["remote_stream_dropped_packets"]
        res.rows.append(row)
    clean = sum(1 for r in res.rows if not r["running_after"] and r["fill_after"] == 0)
    res.check(f"{clean}/{len(res.rows)} cycles ended stopped and empty", clean == len(res.rows))
    res.check("no RP packet drops", all(r["dropped"] == 0 for r in res.rows))
    res.check("every stop observed within 4 s", all(r["stop_ms"] is not None for r in res.rows))
    return res


def t_ed_stop_residue(ctx) -> Result:
    """Stop with the ESP32 send queue full: no stale packet may remain on the RP."""
    res = Result("ed-stop-residue")
    require_idle(ctx.host)
    pts = make_circle(12000, 480)
    for i in range(ctx.repeat):
        s = ed_connect(ctx.host)
        try:
            idx = ed_prefill(s, pts, 3072)
            s.begin(30000, 0)
            ed_feed(s, pts, 30000, 1.5, target_fullness=6000, start=idx)
            s.command(b"s")
        finally:
            s.close()
        time.sleep(0.4)
        st = status(ctx.host, refresh=True)
        res.rows.append({"run": i + 1, "running": st["remote_running"], "fill": st["remote_buffer_fill_samples"]})
        if st["remote_buffer_fill_samples"]:
            api(ctx.host, "/api/rp/stop", "POST")   # clear it so the next START cannot play it
            time.sleep(0.5)
    clean = sum(1 for r in res.rows if not r["running"] and r["fill"] == 0)
    res.check(f"{clean}/{len(res.rows)} stops left the RP buffer empty", clean == len(res.rows))
    return res


def t_ed_underrun_hold(ctx) -> Result:
    """Starve a running stream for 1 s: output must stay running, no drops."""
    res = Result("ed-underrun-hold")
    require_idle(ctx.host)
    pts = make_circle(8000, 360)
    base = status(ctx.host, refresh=True)
    s = ed_connect(ctx.host)
    try:
        ed_prefill(s, pts, 1024)
        s.begin(30000, 0)
        time.sleep(1.0)
        st = status(ctx.host, refresh=True)
        _, _, raw = s.command(b"?")
        ed = ed_status(raw)
        under = st["remote_underruns"] - base["remote_underruns"]
        res.rows.append({"running": st["remote_running"], "fill": st["remote_buffer_fill_samples"],
                         "underrun_samples_1s": under,
                         "dropped": st["remote_stream_dropped_packets"] - base["remote_stream_dropped_packets"],
                         "ed_pb_state": ed["pb_state"],
                         "ed_underflow_flag": bool(ed["pb_flags"] & ED_PB_FLAG_UNDERFLOW),
                         "ed_fullness": ed["fullness"]})
        s.command(b"s")
    finally:
        s.close()
    stopped = wait_stopped(ctx.host)
    r = res.rows[0]
    res.check("still running while starved (holds position, laser blanked)", r["running"])
    res.check("underrun samples in 1 s between 20k and 32k", 20000 <= r["underrun_samples_1s"] <= 32000)
    res.check("no packet drops", r["dropped"] == 0)
    res.check("stopped and empty afterwards", stopped is not None)
    if not r["ed_underflow_flag"]:
        res.notes.append("Ether Dream playback_flags underflow bit is not set during an underrun")
    return res


def t_ed_rate_change(ctx) -> Result:
    """Change the rate mid-stream with 'u': 20k -> 30k -> 20k, 2 s each."""
    res = Result("ed-rate-change")
    require_idle(ctx.host)
    pts = make_circle(12000, 480)
    phases = [20000, 30000, 20000]

    def on_rate(elapsed):
        return phases[min(int(elapsed // 2.0), len(phases) - 1)]

    sampler = Sampler(ctx.host)
    s = ed_connect(ctx.host)
    try:
        idx = ed_prefill(s, pts, 3072)
        s.begin(phases[0], 0)
        sampler.start()
        feed = ed_feed(s, pts, phases[0], 6.0, on_rate=on_rate, start=idx)
        feed.pop("next_idx")
        s.command(b"s")
    finally:
        sampler.stop()
        s.close()
    stopped = wait_stopped(ctx.host)
    res.rows = sampler.rows
    res.notes.append(f"client: {feed}")
    rates_seen = sorted({r["rate"] for r in res.rows})
    res.check(f"RP applied both rates (seen {rates_seen})", {20000, 30000} <= set(rates_seen))
    res.check("no packet drops", all(r["dropped"] == 0 for r in res.rows))
    res.check("underruns/s stayed below 1000 after the first second",
              all(r["underruns_ps"] < 1000 for r in res.rows[1:]))
    res.check("stopped and empty afterwards", stopped is not None)
    return res


def t_idn_timeout(ctx) -> Result:
    """Send 1 s of IDN points, then go silent without closing: when does output stop?"""
    res = Result("idn-timeout")
    require_idle(ctx.host)
    pts = make_circle(8000, 360)
    idn = IDNSender(ctx.host, quiet=True)
    try:
        idn_feed(idn, pts, 30000, 1.0)
        t0 = time.perf_counter()
        stopped = wait_for(lambda: not status(ctx.host, refresh=True)["remote_running"], 15.0, poll_s=0.2)
    finally:
        idn.close_socket()
    st = status(ctx.host, refresh=True)
    res.rows.append({"silence_to_stop_s": None if stopped is None else round(stopped, 2),
                     "fill_after": st["remote_buffer_fill_samples"], "running_after": st["remote_running"]})
    res.check("output stopped after the sender went silent", stopped is not None)
    res.check("buffer empty afterwards", st["remote_buffer_fill_samples"] == 0)
    return res


def t_idn_overrun(ctx) -> Result:
    """IDN sender declares 30 kpps but delivers 2x faster for 3 s (no flow control)."""
    res = Result("idn-overrun")
    require_idle(ctx.host)
    pts = make_circle(8000, 360)
    base = status(ctx.host, refresh=True)
    sampler = Sampler(ctx.host)
    idn = IDNSender(ctx.host, quiet=True)
    try:
        sampler.start()
        feed = idn_feed(idn, pts, 30000, 3.0, pace_factor=2.0)
        sent = feed["sent"]
        idn.stop()
    finally:
        sampler.stop()
        idn.close_socket()
    stopped = wait_stopped(ctx.host, timeout_s=6.0)
    end = status(ctx.host, refresh=True)
    res.rows = sampler.rows
    fed = end["stream_idn_samples"] - base["stream_idn_samples"]
    counted = end.get("stream_idn_dropped", 0) - base.get("stream_idn_dropped", 0)
    res.notes.append(f"client sent {sent} points; {fed} reached the RP; "
                     f"{sent - fed} were dropped on the ESP32; status counted {counted}")
    # Points still in the ESP32 ring (up to 8191) when the session closed were
    # discarded with it, not dropped as overrun, so they are not counted.
    loss = sent - fed
    res.check("ESP32 drop counter accounts for the loss (allowing the 8191-sample ring)",
              counted <= loss and loss - counted <= 8191 + 0.05 * loss)
    res.check("RP rate stayed at the declared 30000", all(r["rate"] == 30000 for r in res.rows))
    res.check("no RP-side packet drops", all(r["dropped"] == 0 for r in res.rows))
    res.check("stopped and empty afterwards", stopped is not None)
    return res


def t_idn_packet_size(ctx) -> Result:
    """IDN loss on the ESP32 vs points per packet (64/120/180) at 30 and 50 kpps."""
    res = Result("idn-packet-size")
    require_idle(ctx.host)
    for rate, radius in ((30000, 12000), (50000, 8000)):
        if rate > ctx.galvo_max:
            continue
        pts = make_circle(radius, 480)
        for pkt in (64, 120, 180):
            before = status(ctx.host, refresh=True)
            sampler = Sampler(ctx.host)
            idn = IDNSender(ctx.host, quiet=True)
            try:
                sampler.start()
                feed = idn_feed(idn, pts, rate, 5.0, pkt=pkt)
                idn_close(idn)
            finally:
                sampler.stop()
                idn.close_socket()
            stopped = wait_stopped(ctx.host, timeout_s=8.0)
            after = status(ctx.host, refresh=True)
            reached = after["stream_idn_samples"] - before["stream_idn_samples"]
            rows = sampler.rows[1:]
            res.rows.append({"rate": rate, "pkt": pkt, "bytes": 4 + 8 + 4 + pkt * 8,
                             "packets_ps": round(rate / pkt), "client_pps": feed["client_pps"],
                             "sent": feed["sent"], "reached_rp": reached,
                             "lost_pct": round(100.0 * (feed["sent"] - reached) / feed["sent"], 1),
                             "underruns_ps_max": max((r["underruns_ps"] for r in rows), default=None),
                             "stop_s": None if stopped is None else round(stopped, 2)})
            time.sleep(0.5)
    for r in res.rows:
        res.check(f"{r['rate']} pps, {r['pkt']}/packet: loss {r['lost_pct']}% <= 1%", r["lost_pct"] <= 1.0)
    return res


def t_throughput(ctx) -> Result:
    """Circle for 8 s per protocol and rate; smaller radius at higher rates."""
    res = Result("throughput")
    require_idle(ctx.host)
    matrix = [(30000, 22000), (40000, 12000), (50000, 8000)]
    seconds = 8.0
    for rate, radius in matrix:
        if rate > ctx.galvo_max:
            res.notes.append(f"skipped {rate} pps: above galvo max {ctx.galvo_max}")
            continue
        pts = make_circle(radius, 480)
        for proto in ("etherdream", "idn"):
            sampler = Sampler(ctx.host)
            before = status(ctx.host, refresh=True)
            if proto == "etherdream":
                s = ed_connect(ctx.host)
                try:
                    idx = ed_prefill(s, pts, 3072)
                    s.begin(rate, 0)
                    sampler.start()
                    feed = ed_feed(s, pts, rate, seconds, start=idx)
                    feed.pop("next_idx")
                    s.command(b"s")
                finally:
                    sampler.stop()
                    s.close()
            else:
                idn = IDNSender(ctx.host, quiet=True)
                try:
                    sampler.start()
                    feed = idn_feed(idn, pts, rate, seconds)
                    idn_close(idn)
                finally:
                    sampler.stop()
                    idn.close_socket()
            stopped = wait_stopped(ctx.host, timeout_s=6.0)
            after = status(ctx.host, refresh=True)
            reached = after[f"stream_{proto}_samples"] - before[f"stream_{proto}_samples"]
            rows = sampler.rows[1:]  # skip the start-up second
            key = "ed_pps" if proto == "etherdream" else "idn_pps"
            arriving = [r[key] for r in rows]
            res.rows.append({"proto": proto, "rate": rate, "radius": radius,
                             "pps_min": min(arriving) if arriving else None,
                             "pps_avg": round(sum(arriving) / len(arriving)) if arriving else None,
                             "underruns_ps_max": max(r["underruns_ps"] for r in rows) if rows else None,
                             "fill_min": min(r["fill"] for r in rows) if rows else None,
                             "fill_max": max(r["fill"] for r in rows) if rows else None,
                             "dropped": sum(r["dropped"] for r in rows),
                             "client_pps": feed["client_pps"],
                             "sent": feed["sent"], "reached_rp": reached,
                             "lost_on_esp": feed["sent"] - reached,
                             "stopped_ok": stopped is not None})
            time.sleep(0.5)
    for r in res.rows:
        # The board is judged against what the client delivered: the Python
        # Ether Dream client tops out near 30 kpps (see ed-pacing).
        target = min(r["rate"], r["client_pps"])
        res.check(f"{r['proto']} {r['rate']}: arriving >= 97% of min(rate, client {r['client_pps']}) ({r['pps_avg']})",
                  r["pps_avg"] is not None and r["pps_avg"] >= 0.97 * target)
        res.check(f"{r['proto']} {r['rate']}: no drops, stopped cleanly",
                  r["dropped"] == 0 and r["stopped_ok"])
    return res


TESTS = {
    "idle": t_idle,
    "rate-request": t_rate_request,
    "ed-begin-rate": t_ed_begin_rate,
    "ed-pacing": t_ed_pacing,
    "cancel-before-start": t_cancel_before_start,
    "ed-stop-cycle": t_ed_stop_cycle,
    "ed-stop-residue": t_ed_stop_residue,
    "ed-underrun-hold": t_ed_underrun_hold,
    "ed-rate-change": t_ed_rate_change,
    "idn-timeout": t_idn_timeout,
    "idn-overrun": t_idn_overrun,
    "idn-packet-size": t_idn_packet_size,
    "throughput": t_throughput,
}


class Ctx:
    def __init__(self, host, galvo_max, repeat):
        self.host = host
        self.galvo_max = galvo_max
        self.repeat = repeat


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="ildawavex16v2.local")
    ap.add_argument("--galvo-max", type=int, default=None,
                    help="galvo rating in pps (default: the board's scanner limit; may not exceed it)")
    ap.add_argument("--repeat", type=int, default=20, help="runs for the repeated tests (default 20)")
    ap.add_argument("--log", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs",
                                                  time.strftime("hwtest-%Y%m%d.jsonl")))
    ap.add_argument("tests", nargs="+", help="test names, or 'all' / 'list'")
    args = ap.parse_args()

    if args.tests == ["list"]:
        for name, fn in TESTS.items():
            print(f"{name:22s} {fn.__doc__.strip().splitlines()[0]}")
        return 0

    names = list(TESTS) if args.tests == ["all"] else args.tests
    unknown = [n for n in names if n not in TESTS]
    if unknown:
        raise SystemExit(f"unknown tests: {unknown}; see 'list'")

    # Test rates come from the board's own limit, so a 30k board is not
    # sent 50k and then marked as losing data.
    board_limit = int(require_idle(args.host)["rate_limit"])
    galvo_max = board_limit if args.galvo_max is None else args.galvo_max
    if galvo_max > board_limit:
        raise SystemExit(f"--galvo-max {galvo_max} is above the board's scanner limit {board_limit}; "
                         "raise the limit in the web UI first if the scanners are rated for it")
    print(f"galvo max {galvo_max} pps (board scanner limit {board_limit})")

    ctx = Ctx(args.host, galvo_max, args.repeat)
    os.makedirs(os.path.dirname(args.log), exist_ok=True)
    failed = []
    for name in names:
        print(f"\n-- running {name} ...", flush=True)
        try:
            res = TESTS[name](ctx)
        except Exception as exc:  # report and move on, but leave output stopped
            res = Result(name)
            res.check(f"raised {type(exc).__name__}: {exc}", False)
            try:
                api(ctx.host, "/api/rp/stop", "POST")
            except OSError:
                pass
        report(res, args.log)
        if not res.passed:
            failed.append(name)
    print(f"\n{len(names) - len(failed)}/{len(names)} tests passed" + (f"; failed: {failed}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
