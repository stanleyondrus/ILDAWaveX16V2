"""Sender and response-parser tests against the literal fixtures in tests/idn/fixtures.

Standard library only. Expected bytes always come from the fixture files, never
from the sender itself. Run through tools/test_idn.py, or directly:

  python -m unittest discover -s tests/idn
"""

import glob
import io
import json
import os
import socket
import sys
import unittest
from contextlib import redirect_stderr

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools"))

import idn_sender as idn  # noqa: E402
from ilw_sender import LaserPoint  # noqa: E402

ADDR = ("192.0.2.50", idn.IDN_PORT)


def _load():
    by_id = {}
    for path in glob.glob(os.path.join(HERE, "fixtures", "*.json")):
        with open(path, encoding="utf-8") as f:
            for fx in json.load(f)["fixtures"]:
                by_id[fx["id"]] = fx
    return by_id


FX = _load()


def literal(obj) -> bytes:
    return bytes.fromhex("".join(s["hex"] for s in obj["segments"]))


def fx_bytes(fid: str) -> bytes:
    return literal(FX[fid])


class FakeClock:
    """Monotonic seconds; sleep_until jumps to the deadline (or short of it)."""

    def __init__(self, start=100.0, undershoot=0.0):
        self.now = start
        self.undershoot = undershoot
        self.sleeps = 0

    def __call__(self):
        return self.now

    def sleep_until(self, deadline):
        self.sleeps += 1
        target = deadline - self.undershoot if self.undershoot and self.sleeps % 2 else deadline
        self.now = max(self.now, target)


class FakeSocket:
    def __init__(self, clock=None, replies=None, fail_sends=0):
        self.sent = []
        self.clock = clock
        self.replies = list(replies or [])
        self.fail_sends = fail_sends
        self.timeouts = []

    def sendto(self, data, addr):
        if self.fail_sends:
            self.fail_sends -= 1
            raise OSError("simulated send failure")
        self.sent.append((bytes(data), addr, self.clock() if self.clock else None))
        return len(data)

    def settimeout(self, t):
        self.timeouts.append(t)

    def recvfrom(self, n):
        if self.replies:
            return self.replies.pop(0)
        if self.clock:
            self.clock.now += self.timeouts[-1] if self.timeouts else 10.0
        raise socket.timeout()

    def close(self):
        pass


def make_stream(sender_params, clock=None, sock=None, **kw):
    clock = clock or FakeClock()
    sock = sock or FakeSocket(clock)
    cfg = idn.StreamConfig(pps=sender_params["pps"], packet_points=sender_params["packet_points"],
                           channel=sender_params["channel"], service_id=sender_params["service_id"],
                           sdm=sender_params["sdm"], group=sender_params["group"], budget=sender_params["budget"],
                           **kw.pop("config", {}))
    st = idn.IDNStream(sock, ADDR, cfg, first_seq=sender_params["first_seq"], ts_base=sender_params["ts_base"],
                       clock=clock, sleep_until=clock.sleep_until, lead_in=sender_params["lead_in"], **kw)
    return st, sock, clock


def sender_input(params):
    if "input_points" in params:
        return [LaserPoint(*p) for p in params["input_points"]]
    return [LaserPoint(0, 0, 0, 0, 0, 0) for _ in range(params["input_blank_count"])]


def run_sender_fixture(fid):
    f = FX[fid]
    p = f["sender"]
    st, sock, clock = make_stream(p)
    origin = clock.now
    st.stream_points(sender_input(p), loop=False)
    if p.get("close", True):
        st.close()
    return f, sock, origin


class SenderBytes(unittest.TestCase):
    """Whole-packet and header-prefix comparisons with literal fixtures."""

    def check_sequence(self, fid):
        f, sock, origin = run_sender_fixture(fid)
        self.assertEqual(len(sock.sent), len(f["packets"]), "packet count")
        for k, (want, (got, addr, t)) in enumerate(zip(f["packets"], sock.sent)):
            self.assertEqual(addr, ADDR)
            if "ref" in want:
                self.assertEqual(got.hex(), fx_bytes(want["ref"]).hex(), f"packet {k} ({want['ref']})")
            else:
                prefix = literal(want)
                self.assertEqual(len(got), want["datagram_length"], f"packet {k} length")
                self.assertEqual(got[:len(prefix)].hex(), prefix.hex(), f"packet {k} header")
            if want["send_at_s"] is not None:
                self.assertGreaterEqual(t - origin, want["send_at_s"] - 1e-12, f"packet {k} sent early")
                self.assertAlmostEqual(t - origin, want["send_at_s"], places=9, msg=f"packet {k} send time")

    def test_first_and_second_packet_and_close(self):
        self.check_sequence("sender-xyrgbi-40-at-30k")

    def test_explicit_service_1(self):
        self.check_sequence("sender-xyrgbi-20-svc1")

    def test_config_repeats_every_200ms_with_sdm_scm(self):
        self.check_sequence("sender-config-cadence-1k-50")

    def test_timestamp_wrap(self):
        self.check_sequence("sender-timestamp-wrap")

    def test_finite_tail_is_repartitioned(self):
        self.check_sequence("sender-tail-65-at-64")

    def test_lead_in_is_blanked_origin_and_counted(self):
        p = dict(FX["sender-xyrgbi-40-at-30k"]["sender"], lead_in=True)
        st, sock, _ = make_stream(p)
        pts = sender_input(p)[1:]  # drop the fixture's own origin sample; the lead-in replaces it
        st.stream_points(pts, loop=False)
        self.assertEqual(sock.sent[0][0].hex(), fx_bytes("rt-first-config-wave-xyrgbi-20").hex())
        self.assertEqual(st.stats["samples"], 40)


class Batching(unittest.TestCase):
    def test_plan_batches_table(self):
        for c in FX["batching-plan"]["cases"]:
            with self.subTest(c=c):
                if "expect_error" in c:
                    with self.assertRaises(ValueError):
                        idn.plan_batches(c["total"], c["preferred"], c["capacity"], c["minimum"])
                else:
                    got = idn.plan_batches(c["total"], c["preferred"], c["capacity"], c["minimum"])
                    self.assertEqual(got, c["expect"])

    def test_capacity_table(self):
        for c in FX["packet-capacity"]["cases"]:
            with self.subTest(c=c):
                if "expect_error" in c:
                    with self.assertRaises(ValueError):
                        idn.packet_capacity(c["budget"], c["sample_bytes"], c["dictionary_bytes"])
                    continue
                self.assertEqual(idn.packet_capacity(c["budget"], c["sample_bytes"], c["dictionary_bytes"], True),
                                 c["expect_with_config"])
                self.assertEqual(idn.packet_capacity(c["budget"], c["sample_bytes"], c["dictionary_bytes"], False),
                                 c["expect_without_config"])

    def test_cumulative_timing_table(self):
        for c in FX["cumulative-timing"]["cases"]:
            with self.subTest(c=c):
                n, offs, durs = 0, [], []
                for b in c["batches"]:
                    offs.append(idn.boundary_us(n, c["pps"]))
                    durs.append(idn.boundary_us(n + b, c["pps"]) - offs[-1])
                    n += b
                self.assertEqual(offs, c["expect_offsets_us"])
                self.assertEqual(durs, c["expect_durations_us"])

    def test_max_size_packet_fits_budget(self):
        p = dict(FX["sender-xyrgbi-40-at-30k"]["sender"], packet_points=179)
        st, sock, _ = make_stream(p)
        st.stream_points([LaserPoint(0, 0)] * 358, loop=False)
        self.assertEqual([len(d) for d, _, _ in sock.sent], [1468, 1448])
        with self.assertRaises(ValueError):
            make_stream(dict(p, packet_points=180))

    def test_ilda_frame_ends_do_not_make_short_packets(self):
        p = dict(FX["sender-xyrgbi-40-at-30k"]["sender"], packet_points=64)
        st, sock, _ = make_stream(p)
        frames = [[LaserPoint(0, 0)] * 30, [LaserPoint(0, 0)] * 30, [LaserPoint(0, 0)] * 30]  # 90 points
        st.stream(pt for fr in frames for pt in fr)
        counts = [(len(d) - (36 if d[6] & 0x40 else 16)) // 8 for d, _, _ in sock.sent]
        self.assertEqual(counts, [64, 26])

    def test_below_minimum_finite_input_is_an_error(self):
        st, sock, _ = make_stream(FX["sender-xyrgbi-40-at-30k"]["sender"])
        with self.assertRaises(ValueError):
            st.stream_points([LaserPoint(0, 0)] * 19, loop=False)
        self.assertEqual(sock.sent, [])


class Pacing(unittest.TestCase):
    P = FX["sender-xyrgbi-40-at-30k"]["sender"]

    def test_never_sends_early_even_if_sleep_returns_early(self):
        clock = FakeClock(undershoot=0.0004)
        st, sock, _ = make_stream(dict(self.P, packet_points=20), clock=clock)
        origin = clock.now
        st.stream_points([LaserPoint(0, 0)] * 200, loop=False)
        n = 0
        for data, _, t in sock.sent:
            due = origin + idn.boundary_us(n, self.P["pps"]) / 1e6
            self.assertGreaterEqual(t, due)
            n += 20

    def test_small_lateness_keeps_the_authored_timeline(self):
        clock = FakeClock()
        st, sock, _ = make_stream(self.P, clock=clock)
        st.send_batch([LaserPoint(0, 0)] * 20)
        clock.now += 0.05  # 50 ms late, under the 100 ms limit
        st.send_batch([LaserPoint(0, 0)] * 20)
        self.assertEqual(sock.sent[1][0][8:12].hex(), "0000029a")  # timestamp 666 us, not wall time

    def test_too_late_closes_and_raises(self):
        clock = FakeClock()
        st, sock, _ = make_stream(self.P, clock=clock)
        st.send_batch([LaserPoint(0, 0)] * 20)
        clock.now += 0.5
        with self.assertRaises(idn.StreamLateError):
            st.send_batch([LaserPoint(0, 0)] * 20)
        self.assertEqual(sock.sent[-1][0].hex(), "44001235")
        self.assertEqual(st.state, st.CLOSED)
        with self.assertRaises(idn.StreamStateError):
            st.send_batch([LaserPoint(0, 0)] * 20)

    def test_too_late_restart_opens_a_fresh_epoch(self):
        clock = FakeClock()
        st, sock, _ = make_stream(self.P, clock=clock, restart_on_late=True)
        st.send_batch([LaserPoint(0, 0)] * 20)
        clock.now += 0.5
        st.send_batch([LaserPoint(0, 0)] * 20)
        close, reopen = sock.sent[1][0], sock.sent[2][0]
        self.assertEqual(close.hex(), "44001235")
        self.assertEqual(reopen[0:4].hex(), "40001236")
        self.assertEqual(reopen[6], 0xC0, "new epoch routes and configures again")
        # Timestamps follow real time across the epochs: 0 + 500 ms.
        self.assertEqual(int.from_bytes(reopen[8:12], "big"), 500000)
        self.assertEqual(st.stats["epochs"], 2)

    def test_failed_send_does_not_advance_state(self):
        clock = FakeClock()
        sock = FakeSocket(clock, fail_sends=1)
        st, _, _ = make_stream(self.P, clock=clock, sock=sock)
        pts = sender_input(self.P)
        with self.assertRaises(OSError):
            st.send_batch(pts[:20])
        self.assertEqual((st.seq, st.epoch_samples, st.state), (0x1234, 0, st.IDLE))
        st.send_batch(pts[:20])
        self.assertEqual(sock.sent[0][0].hex(), fx_bytes("rt-first-config-wave-xyrgbi-20").hex())


class CloseAbort(unittest.TestCase):
    P = FX["sender-xyrgbi-40-at-30k"]["sender"]

    def opened(self, replies=None):
        """A stream that has sent the two packets of sender-xyrgbi-40-at-30k."""
        clock = FakeClock()
        sock = FakeSocket(clock, replies)
        st, _, _ = make_stream(self.P, clock=clock, sock=sock)
        pts = sender_input(self.P)
        st.send_batch(pts[:20])
        st.send_batch(pts[20:40])
        return st, sock

    def test_close_on_same_socket(self):
        st, sock = self.opened()
        st.close()
        self.assertEqual(sock.sent[-1][0], fx_bytes("rt-connection-close-empty"))
        self.assertEqual(st.state, st.CLOSED)
        st.close()
        self.assertEqual(len(sock.sent), 3, "closing twice sends nothing more")

    def test_abort(self):
        st, sock = self.opened()
        st.abort()
        self.assertEqual(sock.sent[-1][0], fx_bytes("rt-abort"))
        self.assertEqual(st.state, st.CLOSED)

    def test_never_sent_stream_sends_no_close(self):
        sock = FakeSocket()
        st, _, _ = make_stream(self.P, sock=sock)
        st.close()
        st.abort()
        self.assertEqual(sock.sent, [])

    def test_close_with_ack_accepts_0xeb(self):
        eb = bytearray(fx_bytes("ack-eb-close-without-connection"))
        eb[2:4] = (0x1236).to_bytes(2, "big")
        stray = (bytes(eb), ("192.0.2.99", idn.IDN_PORT))
        st, sock = self.opened(replies=[stray, (bytes(eb), ADDR)])
        ack = st.close(ack=True)
        self.assertEqual(sock.sent[-1][0], fx_bytes("rt-connection-close-ack-empty"))
        self.assertEqual(ack["result_code"], 0xEB)
        self.assertEqual(st.state, st.CLOSED)

    def test_unanswered_close_repeats_without_data(self):
        st, sock = self.opened()
        self.assertIsNone(st.close(ack=True, attempts=3, timeout=0.1))
        self.assertEqual([d.hex() for d, _, _ in sock.sent[2:]], ["45001236", "45001237", "45001238"])


class Management(unittest.TestCase):
    def sender(self, replies=(), group=0):
        clock = FakeClock()
        sock = FakeSocket(clock, list(replies))
        s = idn.IDNSender(ADDR[0], group=group, sock=sock, clock=clock, sleep_until=clock.sleep_until)
        return s, sock

    def test_response_fixtures(self):
        parsers = {"scan-req": idn.parse_scan_response, "svcmap-req": idn.parse_svcmap_response}
        for fid, f in FX.items():
            if f["kind"] != "datagram" or f["direction"] != "server-to-client":
                continue
            exp = f["expect"]
            data = literal(f)
            with self.subTest(fixture=fid):
                if "request" in exp:
                    req = fx_bytes(exp["request"])
                    group, seq = req[1] & 0x0F, int.from_bytes(req[2:4], "big")
                    if exp["request"].startswith("ping"):
                        call = lambda: idn.parse_ping_response(data, req[4:], group, seq)  # noqa: E731
                    else:
                        call = lambda: parsers[exp["request"]](data, group, seq)  # noqa: E731
                else:
                    call = lambda: idn.parse_ack(data, 0, exp["request_sequence"])  # noqa: E731
                if exp["outcome"] == "parse_ok":
                    got = call()
                    if "parsed" in exp:
                        self.assertEqual(got, exp["parsed"])
                else:
                    with self.assertRaises(idn.IDNResponseError) as cm:
                        call()
                    self.assertEqual(cm.exception.reason, exp["reason"])

    def test_requests_match_fixture_bytes(self):
        s, sock = self.sender()
        s.mgmt_seq = 0x0007
        s.scan(timeout=0.1)
        self.assertEqual(sock.sent[0][0], fx_bytes("scan-req"))
        s.mgmt_seq = 0x0009
        s.svcmap(timeout=0.1)
        self.assertEqual(sock.sent[1][0], fx_bytes("svcmap-req"))
        s.group, s.mgmt_seq = 3, 0x1234
        s.ping(bytes.fromhex("deadbeef"), timeout=0.1)
        self.assertEqual(sock.sent[2][0], fx_bytes("ping-req-payload"))

    def test_transaction_ignores_unrelated_datagrams(self):
        good = fx_bytes("scan-resp-valid")
        replies = [(good, ("192.0.2.99", 7255)),             # other host
                   (fx_bytes("scan-resp-wrong-sequence"), ADDR),
                   (fx_bytes("scan-resp-wrong-group"), ADDR),
                   (fx_bytes("ping-resp-empty"), ADDR),      # other command
                   (good, ADDR)]
        s, _ = self.sender(replies)
        s.mgmt_seq = 0x0007
        units = s.scan(timeout=1.0)
        self.assertEqual(len(units), 1)
        self.assertEqual(units[0]["unit_id"], "01-123456789ABC")
        self.assertEqual(units[0]["source"], f"{ADDR[0]}:{ADDR[1]}")

    def test_ping_exact_echo_and_timeout(self):
        s, _ = self.sender([(fx_bytes("ping-resp-payload"), ADDR)], group=3)
        s.mgmt_seq = 0x1234
        self.assertIsNotNone(s.ping(bytes.fromhex("deadbeef"), timeout=1.0))
        s, _ = self.sender([(fx_bytes("ping-resp-short-echo"), ADDR)], group=3)
        s.mgmt_seq = 0x1234
        with self.assertRaises(idn.IDNResponseError):
            s.ping(bytes.fromhex("deadbeef"), timeout=1.0)
        s, _ = self.sender([])
        self.assertIsNone(s.ping(b"", timeout=0.2))

    def test_management_does_not_touch_rt_sequence(self):
        clock = FakeClock()
        sock = FakeSocket(clock)
        s = idn.IDNSender(ADDR[0], sock=sock, clock=clock, sleep_until=clock.sleep_until)
        p = FX["sender-xyrgbi-40-at-30k"]["sender"]
        st = s.open_stream(p["pps"], p["packet_points"], first_seq=p["first_seq"], ts_base=0, lead_in=False)
        pts = sender_input(p)
        st.send_batch(pts[:20])
        s.ping(b"", timeout=0.05)
        st.send_batch(pts[20:40])
        rt = [d for d, _, _ in sock.sent if d[0] == idn.IDNCMD_RT_CHANNEL]
        self.assertEqual([d.hex() for d in rt], [fx_bytes("rt-first-config-wave-xyrgbi-20").hex(),
                                                 fx_bytes("rt-second-wave-xyrgbi-20").hex()])


class Validation(unittest.TestCase):
    def test_rejects_instead_of_masking(self):
        bad = [dict(channel=64), dict(channel=-1), dict(group=16), dict(sdm=4), dict(service_id=256),
               dict(pps=0), dict(pps=-1), dict(packet_points=19), dict(packet_points=180), dict(budget=100),
               dict(service_mode=2), dict(config_interval_us=0), dict(config_interval_us=300000),
               dict(max_late_s=0), dict(pps=50)]
        for kw in bad:
            with self.subTest(kw=kw):
                args = dict(pps=30000, packet_points=64)
                args.update(kw)
                with self.assertRaises(ValueError):
                    idn.StreamConfig(**args)
        with self.assertRaises(ValueError):
            idn.build_hello_header(0x40, 16, 0)
        with self.assertRaises(ValueError):
            idn.build_channel_message(64, 1, 0)
        with self.assertRaises(ValueError):
            idn.build_wave_chunk(0, 0, bytes(160))
        with self.assertRaises(ValueError):
            idn.build_config_header(4, routing=False, service_id=1)
        with self.assertRaises(ValueError):
            idn.IDNSender(ADDR[0], group=16, sock=FakeSocket())

    def test_low_rate_caps_batch_by_config_cadence(self):
        cfg = idn.StreamConfig(pps=200, packet_points=64)
        self.assertEqual(cfg.effective_packet_points, 40)  # 200 ms at 200 pps

    def test_builders_reproduce_routing_void_and_channel_close(self):
        tags = idn.xyrgbi_dictionary()
        void = idn.build_hello_header(0x40, 0, 1) + idn.build_channel_message(
            0, idn.CHUNK_VOID, 0, idn.build_config_header(4, service_id=1) + tags)
        self.assertEqual(void, fx_bytes("rt-routing-void-xyrgbi"))
        close = idn.build_hello_header(0x40, 0, 0x1236) + idn.build_channel_message(
            0, idn.CHUNK_VOID, 1333, idn.build_config_header(0, close=True, routing=False, service_mode=0))
        self.assertEqual(close, fx_bytes("rt-close-channel-void"))


class Cli(unittest.TestCase):
    def test_help_lists_real_commands_only(self):
        text = idn.build_arg_parser().format_help()
        for cmd in ("square", "circle", "sine", "ild", "scan", "svcmap", "ping"):
            self.assertIn(cmd, text)
        self.assertNotRegex(text, r"\{[^}]*\bstop\b")
        self.assertIn("--channel", text)
        self.assertIn("--group", text)

    def test_cli_rejects_out_of_range_ids(self):
        for argv in (["--host", "x", "--channel", "64", "circle"], ["--host", "x", "--group", "16", "scan"],
                     ["--host", "x", "--packet-points", "19", "circle"], ["--host", "x", "stop"]):
            with self.subTest(argv=argv), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    idn.build_arg_parser().parse_args(argv)


if __name__ == "__main__":
    unittest.main()
