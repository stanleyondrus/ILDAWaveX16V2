#!/usr/bin/env python3
"""IDN-Hello / IDN-RT continuous-graphics sender for ILDAWave DACs.

This script has the same drawing commands as ilw_sender.py, but streams points
as IDN-Stream continuous-graphics wave samples (service mode 0x01) inside
IDN-RT packets on UDP port 7255.

Quick start:

  Draw a circle for 5 seconds at 30 kpps:
    python idn_sender.py --host 192.168.1.50 --pps 30000 circle --seconds 5

  Draw a square with 64 points per packet:
    python idn_sender.py --host 192.168.1.50 --pps 30000 --packet-points 64 square --seconds 10

  Stream an ILDA file at 30 FPS and loop it:
    python idn_sender.py --host 192.168.1.50 --pps 30000 ild show.ild --fps 30 --loop

  Discovery and reachability:
    python idn_sender.py --host 192.168.1.50 scan
    python idn_sender.py --host 192.168.1.50 svcmap
    python idn_sender.py --host 192.168.1.50 ping --size 32

What goes on the wire:
  - The first packet routes the channel (Routing flag, service ID, mode 0x01)
    and carries the full sample dictionary; the unchanged configuration is
    repeated so that configured packets are never more than 200 ms of stream
    time apart (IDN-Stream 2.2).
  - Samples are 8 octets, declared by the dictionary
    X(0x4200 0x4010) Y(0x4210 0x4010) R(0x527E) G(0x5214) B(0x51CC) I(0x5C10).
  - Every wave chunk carries 20 or more samples; with the default 1472-octet
    UDP payload budget a configured packet holds at most 179 samples.
  - Timestamps and durations come from one cumulative integer timeline, and no
    packet is sent before its timestamp (IDN-Hello 6).
  - The stream is closed (IDN-RT graceful close) over the same socket when the
    command ends. There is no separate "stop" command: IDN-RT identifies a
    connection by its source port, so a second process cannot close this one.

This tool follows the specifications as read by this project; it has not been
certified against them and is not a conformance claim.
"""

from __future__ import annotations

import argparse
import itertools
import random
import socket
import struct
import time
from dataclasses import dataclass
from typing import Callable, Iterable, Iterator, List, Optional, Sequence, Tuple

from ilw_sender import (
    LaserPoint,
    blank_point,
    clamp_i16,
    make_circle,
    make_sine,
    make_square,
    normalize_ilda_points,
    resample_points,
    load_ilda,
    u16_to_u8,
    wait_until_precise,
)


IDN_PORT = 7255

IDNCMD_PING_REQ = 0x08
IDNCMD_PING_RESP = 0x09
IDNCMD_SCAN_REQ = 0x10
IDNCMD_SCAN_RESP = 0x11
IDNCMD_SVCMAP_REQ = 0x12
IDNCMD_SVCMAP_RESP = 0x13
IDNCMD_RT_CHANNEL = 0x40
IDNCMD_RT_CHANNEL_ACK = 0x41
IDNCMD_RT_CLOSE = 0x44
IDNCMD_RT_CLOSE_ACK = 0x45
IDNCMD_RT_ABORT = 0x46
IDNCMD_RT_ACK = 0x47

CHUNK_VOID = 0x00
CHUNK_WAVE = 0x01

MODE_GRAPHIC_CONTINUOUS = 0x01

CNL_CHANNEL_MSG = 0x80
CNL_CCLF = 0x40
CFL_ROUTING = 0x01
CFL_CLOSE = 0x02

HELLO_HEADER_BYTES = 4
CHANNEL_HEADER_BYTES = 8
CONFIG_HEADER_BYTES = 4
WAVE_HEADER_BYTES = 4

# Tag dictionary for the 8-octet X_hi X_lo Y_hi Y_lo R G B I sample.
XYRGBI_TAGS = (0x4200, 0x4010, 0x4210, 0x4010, 0x527E, 0x5214, 0x51CC, 0x5C10)
SAMPLE_BYTES = 8

MIN_WAVE_SAMPLES = 20
MAX_MESSAGE_BYTES = 0xFF00
MAX_UDP_PAYLOAD = 65507
DEFAULT_UDP_BUDGET = 1472
DEFAULT_CONFIG_INTERVAL_US = 200_000
MAX_CONFIG_INTERVAL_US = 250_000
DEFAULT_MAX_LATE_S = 0.1
MAX_PPS = 20_000_000

ACK_RESULTS = {
    0x00: "message received and passed to the session",
    0xEB: "empty close without established connection",
    0xEC: "all sessions occupied",
    0xED: "client group excluded",
    0xEE: "invalid packet payload",
    0xEF: "other processing error",
}
ACK_INPUT_FLAGS = ((15, "IRAErr"), (13, "CCErr"), (11, "BPErr"), (10, "LAErr"), (9, "OVErr"), (8, "MVErr"),
                   (6, "SeqErr:missing"), (5, "SeqErr:duplicate"), (4, "SeqErr:not-monotonic"),
                   (3, "Order"), (0, "New"))
ACK_PIPELINE_FLAGS = ((15, "IAPErr"), (14, "DVIErr"), (13, "PVLErr"), (12, "RGUErr"), (11, "MCLErr"),
                      (10, "CTYErr"), (9, "DCMErr"), (8, "CKTErr"), (7, "CFGErr"), (6, "FRGErr"),
                      (5, "BSYErr"), (4, "SMErr"), (1, "CnlClsd"), (0, "CnlRted"))
ACK_STATUS_FLAGS = ((2, "SNMsg"), (1, "DOBuf"), (0, "SOCnl"))

SCAN_STRUCT_BYTES = 40
SVCMAP_HEADER_BYTES = 4
SVCMAP_ENTRY_BYTES = 24
ACK_STRUCT_BYTES = 12


class IDNResponseError(ValueError):
    """A response datagram failed validation. `reason` is a short code."""

    def __init__(self, reason: str, detail: str = ""):
        super().__init__(f"{reason}: {detail}" if detail else reason)
        self.reason = reason


class StreamStateError(RuntimeError):
    pass


class StreamLateError(RuntimeError):
    def __init__(self, late_s: float, limit_s: float):
        super().__init__(f"sender fell {late_s * 1000:.1f} ms behind its timeline (limit {limit_s * 1000:.1f} ms); "
                         "the stream was closed")
        self.late_s = late_s


def _check_int(name: str, v, lo: int, hi: int) -> int:
    if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
        raise ValueError(f"{name} must be an integer in {lo}..{hi}, got {v!r}")
    return v


# -- pure packet builders ----------------------------------------------------

def build_hello_header(cmd: int, group: int, seq: int) -> bytes:
    _check_int("command", cmd, 0, 0xFF)
    _check_int("client group", group, 0, 15)
    _check_int("sequence", seq, 0, 0xFFFF)
    return struct.pack(">BBH", cmd, group, seq)


def build_config_header(scwc: int, sdm: int = 0, close: bool = False, routing: bool = True,
                        service_id: int = 0, service_mode: int = MODE_GRAPHIC_CONTINUOUS) -> bytes:
    _check_int("SCWC", scwc, 0, 0xFF)
    _check_int("SDM", sdm, 0, 3)
    _check_int("service ID", service_id, 0, 0xFF)
    _check_int("service mode", service_mode, 0, 0xFF)
    if not routing and (scwc or sdm or service_id or service_mode):
        raise ValueError("without Routing, SCWC, SDM, service ID and mode must be 0 (IDN-Stream 2.2)")
    cfl = (sdm << 4) | (CFL_CLOSE if close else 0) | (CFL_ROUTING if routing else 0)
    return struct.pack(">BBBB", scwc, cfl, service_id, service_mode)


def build_channel_message(channel: int, chunk_type: int, timestamp: int, config: bytes = b"", data: bytes = b"") -> bytes:
    """IDN-Stream channel message. `config` is the configuration header plus service words."""
    _check_int("channel", channel, 0, 63)
    _check_int("chunk type", chunk_type, 0, 0xBF)
    _check_int("timestamp", timestamp, 0, 0xFFFFFFFF)
    if len(config) % 4:
        raise ValueError("channel configuration must be a whole number of 32-bit words")
    total = CHANNEL_HEADER_BYTES + len(config) + len(data)
    if total > MAX_MESSAGE_BYTES:
        raise ValueError(f"message of {total} octets exceeds the IDN-Stream maximum 0x{MAX_MESSAGE_BYTES:04X}")
    cnl = CNL_CHANNEL_MSG | (CNL_CCLF if config else 0) | channel
    return struct.pack(">HBBI", total, cnl, chunk_type, timestamp) + config + data


def build_wave_chunk(scm: int, duration_us: int, samples: bytes, sample_bytes: int = SAMPLE_BYTES) -> bytes:
    _check_int("SCM", scm, 0, 3)
    _check_int("duration", duration_us, 1, 0xFFFFFF)
    if len(samples) % sample_bytes:
        raise ValueError("sample data is not a whole number of samples")
    if len(samples) // sample_bytes < MIN_WAVE_SAMPLES:
        raise ValueError(f"a wave chunk needs at least {MIN_WAVE_SAMPLES} samples (IDN-Stream 6.1)")
    return struct.pack(">I", (scm << 28) | duration_us) + samples


def xyrgbi_dictionary() -> bytes:
    return b"".join(struct.pack(">H", t) for t in XYRGBI_TAGS)


def encode_sample(p: LaserPoint) -> bytes:
    """One point in the declared 8-octet X_hi X_lo Y_hi Y_lo R G B I layout."""
    p = p.with_blanking_applied()
    return struct.pack(">hhBBBB", clamp_i16(p.x), clamp_i16(p.y),
                       u16_to_u8(p.r), u16_to_u8(p.g), u16_to_u8(p.b), u16_to_u8(p.i))


def build_close_packet(group: int, seq: int, ack: bool = False, payload: bytes = b"") -> bytes:
    return build_hello_header(IDNCMD_RT_CLOSE_ACK if ack else IDNCMD_RT_CLOSE, group, seq) + payload


def build_abort_packet(group: int, seq: int) -> bytes:
    return build_hello_header(IDNCMD_RT_ABORT, group, seq)


# -- sizing, batching and timeline (pure) -------------------------------------

def packet_capacity(budget: int, sample_bytes: int = SAMPLE_BYTES, dictionary_bytes: int = 16,
                    with_config: bool = True) -> int:
    """Samples that fit one IDN-RT datagram of at most `budget` UDP payload octets."""
    _check_int("UDP payload budget", budget, 1, MAX_UDP_PAYLOAD)
    usable = min(budget, HELLO_HEADER_BYTES + MAX_MESSAGE_BYTES)
    overhead = HELLO_HEADER_BYTES + CHANNEL_HEADER_BYTES + WAVE_HEADER_BYTES
    if with_config:
        overhead += CONFIG_HEADER_BYTES + dictionary_bytes
    n = (usable - overhead) // sample_bytes
    if n < MIN_WAVE_SAMPLES:
        raise ValueError(f"a {budget}-octet budget cannot carry a {MIN_WAVE_SAMPLES}-sample wave chunk")
    return n


def plan_batches(total: int, preferred: int, capacity: int, minimum: int = MIN_WAVE_SAMPLES) -> List[int]:
    """Split `total` finite samples into batches of `preferred`, every batch >= minimum.

    A short final remainder takes samples from the batch before it, so 65 at 64
    becomes 45 + 20. If that would leave the donor too small the two merge,
    provided the merged batch fits `capacity`. Samples are never dropped or repeated.
    """
    if not minimum <= preferred <= capacity:
        raise ValueError(f"preferred batch {preferred} must be within {minimum}..{capacity}")
    if total < minimum:
        raise ValueError(f"{total} samples is below the {minimum}-sample minimum chunk")
    full, rem = divmod(total, preferred)
    if rem == 0:
        return [preferred] * full
    if rem >= minimum or full == 0:
        return [preferred] * full + [rem]
    donor = preferred + rem - minimum
    if donor >= minimum:
        return [preferred] * (full - 1) + [donor, minimum]
    if preferred + rem <= capacity:
        return [preferred] * (full - 1) + [preferred + rem]
    raise ValueError(f"cannot split {total} samples into batches of {minimum}..{capacity}")


def iter_batches(points: Iterable[LaserPoint], preferred: int, capacity: int,
                 minimum: int = MIN_WAVE_SAMPLES) -> Iterator[List[LaserPoint]]:
    """Batches across an unbounded or finite iterable; the finite tail uses plan_batches."""
    it = iter(points)
    buf: List[LaserPoint] = []
    lookahead = 2 * preferred
    while True:
        while len(buf) < lookahead:
            try:
                buf.append(next(it))
            except StopIteration:
                for n in plan_batches(len(buf), preferred, capacity, minimum) if buf else []:
                    yield buf[:n]
                    buf = buf[n:]
                return
        yield buf[:preferred]
        buf = buf[preferred:]


def boundary_us(samples: int, pps: int) -> int:
    """Stream-time offset of sample `samples` on the cumulative integer timeline."""
    return samples * 1_000_000 // pps


# -- response parsing (pure) --------------------------------------------------

def parse_hello_header(data: bytes, expect_cmd: int, expect_group: int, expect_seq: int) -> Tuple[int, int, int]:
    if len(data) < HELLO_HEADER_BYTES:
        raise IDNResponseError("short", f"{len(data)} octets")
    cmd, flags, seq = struct.unpack_from(">BBH", data)
    if cmd != expect_cmd:
        raise IDNResponseError("command", f"0x{cmd:02X}, expected 0x{expect_cmd:02X}")
    if flags & 0x0F != expect_group:
        raise IDNResponseError("group", f"{flags & 0x0F}, expected {expect_group}")
    if seq != expect_seq:
        raise IDNResponseError("sequence", f"0x{seq:04X}, expected 0x{expect_seq:04X}")
    return cmd, flags, seq


def _utf8_field(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace")


def _flag_names(value: int, table) -> List[str]:
    return [name for bit, name in table if value & (1 << bit)]


def parse_ping_response(data: bytes, payload: bytes, group: int, seq: int) -> None:
    parse_hello_header(data, IDNCMD_PING_RESP, group, seq)
    if data[HELLO_HEADER_BYTES:] != payload:
        raise IDNResponseError("echo-mismatch", f"{len(data) - HELLO_HEADER_BYTES} of {len(payload)} payload octets, or content differs")


def parse_scan_response(data: bytes, group: int, seq: int) -> dict:
    parse_hello_header(data, IDNCMD_SCAN_RESP, group, seq)
    body = data[HELLO_HEADER_BYTES:]
    if not body:
        raise IDNResponseError("truncated", "no scan response header")
    size = body[0]
    if size < SCAN_STRUCT_BYTES:
        raise IDNResponseError("struct-size", f"{size}, need {SCAN_STRUCT_BYTES}")
    if len(body) < size:
        raise IDNResponseError("truncated", f"{len(body)} of {size} octets")
    version, status = body[1], body[2]
    uid = body[4:20]
    uid_len = uid[0]
    if uid_len > 15:
        raise IDNResponseError("unit-id", f"length {uid_len}")
    unit_id = "" if uid_len == 0 else f"{uid[1]:02X}-" + uid[2:1 + uid_len].hex().upper()
    return {"struct_size": size, "protocol_version": f"{version >> 4}.{version & 0x0F}", "status": status,
            "realtime": bool(status & 0x01), "malfunction": bool(status & 0x80), "offline": bool(status & 0x40),
            "excluded": bool(status & 0x20), "occupied": bool(status & 0x10),
            "unit_id": unit_id, "host_name": _utf8_field(body[20:40])}


def parse_svcmap_response(data: bytes, group: int, seq: int) -> dict:
    parse_hello_header(data, IDNCMD_SVCMAP_RESP, group, seq)
    body = data[HELLO_HEADER_BYTES:]
    if len(body) < SVCMAP_HEADER_BYTES:
        raise IDNResponseError("truncated", "no service map header")
    size, entry, relays, services = struct.unpack_from(">BBBB", body)
    if size < SVCMAP_HEADER_BYTES:
        raise IDNResponseError("struct-size", f"header {size}")
    if entry < SVCMAP_ENTRY_BYTES:
        raise IDNResponseError("struct-size", f"entry {entry}")
    need = size + (relays + services) * entry
    if len(body) < need:
        raise IDNResponseError("truncated", f"{len(body)} of {need} octets")
    rows = [body[size + k * entry: size + (k + 1) * entry] for k in range(relays + services)]
    out = {"relays": [], "services": []}
    for r in rows[:relays]:
        out["relays"].append({"relay_number": r[3], "name": _utf8_field(r[4:24])})
    known = {r["relay_number"] for r in out["relays"]}
    for r in rows[relays:]:
        if r[3] != 0 and r[3] not in known:
            raise IDNResponseError("relay-number", f"service {r[0]} refers to relay {r[3]}")
        out["services"].append({"service_id": r[0], "service_type": r[1], "default": bool(r[2] & 0x01),
                                "relay_number": r[3], "name": _utf8_field(r[4:24])})
    return out


def parse_ack(data: bytes, group: int, seq: int) -> dict:
    parse_hello_header(data, IDNCMD_RT_ACK, group, seq)
    body = data[HELLO_HEADER_BYTES:]
    if not body:
        raise IDNResponseError("truncated", "no acknowledgement header")
    size = body[0]
    if size < ACK_STRUCT_BYTES:
        raise IDNResponseError("struct-size", f"{size}, need {ACK_STRUCT_BYTES}")
    if len(body) < size:
        raise IDNResponseError("truncated", f"{len(body)} of {size} octets")
    _, result, inflags, pflags, status, quality, latency = struct.unpack_from(">BBHHBBI", body)
    return {"struct_size": size, "result_code": result, "input_events": _flag_names(inflags, ACK_INPUT_FLAGS),
            "pipeline_events": _flag_names(pflags, ACK_PIPELINE_FLAGS),
            "status": _flag_names(status, ACK_STATUS_FLAGS), "link_quality": quality, "latency_us": latency}


def _receive_matching(sock, clock, timeout: float, accept: Callable[[bytes, tuple], Optional[object]],
                      collect: bool = False) -> list:
    """Receive until `accept` returns a value (or until timeout when collecting)."""
    got = []
    deadline = clock() + timeout
    while True:
        remaining = deadline - clock()
        if remaining <= 0:
            return got
        sock.settimeout(remaining)
        try:
            data, src = sock.recvfrom(65535)
        except (socket.timeout, BlockingIOError):
            return got
        value = accept(data, src)
        if value is not None:
            got.append(value)
            if not collect:
                return got


# -- streaming ----------------------------------------------------------------

@dataclass(frozen=True)
class StreamConfig:
    """One channel's fixed routing, layout and timing for a run."""

    pps: int
    packet_points: int = 64
    channel: int = 0
    service_id: int = 0
    service_mode: int = MODE_GRAPHIC_CONTINUOUS
    sdm: int = 0
    group: int = 0
    budget: int = DEFAULT_UDP_BUDGET
    config_interval_us: int = DEFAULT_CONFIG_INTERVAL_US
    max_late_s: float = DEFAULT_MAX_LATE_S

    def __post_init__(self):
        _check_int("pps", self.pps, 1, MAX_PPS)
        _check_int("channel", self.channel, 0, 63)
        _check_int("service ID", self.service_id, 0, 0xFF)
        if self.service_mode != MODE_GRAPHIC_CONTINUOUS:
            raise ValueError("only service mode 0x01 (continuous graphic) is implemented by this sender")
        _check_int("SDM", self.sdm, 0, 3)
        _check_int("client group", self.group, 0, 15)
        _check_int("config interval (us)", self.config_interval_us, 1, MAX_CONFIG_INTERVAL_US)
        if not self.max_late_s > 0:
            raise ValueError("max_late_s must be > 0")
        cap = self.capacity
        _check_int("packet points", self.packet_points, MIN_WAVE_SAMPLES, cap)
        if self.cadence_limit < MIN_WAVE_SAMPLES:
            raise ValueError(f"{self.pps} pps is too slow: a {MIN_WAVE_SAMPLES}-sample chunk lasts longer than the "
                             f"{self.config_interval_us // 1000} ms configuration interval")

    @property
    def capacity(self) -> int:
        """Byte-budget limit, assuming every packet may carry the configuration."""
        return packet_capacity(self.budget, SAMPLE_BYTES, 2 * len(XYRGBI_TAGS), with_config=True)

    @property
    def cadence_limit(self) -> int:
        """Most samples whose duration still fits within one configuration interval."""
        return self.config_interval_us * self.pps // 1_000_000

    @property
    def max_batch(self) -> int:
        return min(self.capacity, self.cadence_limit)

    @property
    def effective_packet_points(self) -> int:
        return min(self.packet_points, self.max_batch)

    def config_block(self) -> bytes:
        tags = xyrgbi_dictionary()
        return build_config_header(len(tags) // 4, self.sdm, False, True, self.service_id, self.service_mode) + tags


class IDNStream:
    """Explicit state for one IDN-RT link carrying one continuous-graphics channel.

    The socket is shared with close/abort so they reach the same connection.
    `clock` is a monotonic seconds counter and `sleep_until(deadline)` waits on
    it; both are injectable so pacing can be tested without real time.
    """

    IDLE = "idle"
    STREAMING = "streaming"
    CLOSED = "closed"

    def __init__(self, sock, addr, config: StreamConfig, first_seq: Optional[int] = None,
                 ts_base: Optional[int] = None, clock: Callable[[], float] = time.perf_counter,
                 sleep_until: Callable[[float], None] = wait_until_precise, lead_in: bool = True,
                 restart_on_late: bool = False):
        self.sock = sock
        self.addr = addr
        self.config = config
        self.clock = clock
        self.sleep_until = sleep_until
        self.lead_in = lead_in
        self.restart_on_late = restart_on_late
        self.seq = random.randrange(0x10000) if first_seq is None else _check_int("first sequence", first_seq, 0, 0xFFFF)
        self._ts_base_request = None if ts_base is None else _check_int("timestamp base", ts_base, 0, 0xFFFFFFFF)
        self._config_bytes = config.config_block()
        self.state = self.IDLE
        self.stats = {"packets": 0, "samples": 0, "config_packets": 0, "epochs": 0, "max_late_s": 0.0}
        self._reset_epoch()

    def _reset_epoch(self) -> None:
        self.origin: Optional[float] = None
        self.ts_base: Optional[int] = self._ts_base_request
        self.epoch_samples = 0
        self.last_config_offset_us: Optional[int] = None

    def _start_epoch(self, now: float) -> None:
        self.origin = now
        if self.ts_base is None:
            self.ts_base = int(now * 1_000_000) & 0xFFFFFFFF
        self.stats["epochs"] += 1

    def _wait_until(self, deadline: float) -> None:
        while self.clock() < deadline:
            self.sleep_until(deadline)

    def _config_due(self, offset_us: int, duration_us: int) -> bool:
        if self.last_config_offset_us is None:
            return True
        return offset_us + duration_us - self.last_config_offset_us > self.config.config_interval_us

    def next_timestamp(self) -> int:
        base = 0 if self.ts_base is None else self.ts_base
        return (base + boundary_us(self.epoch_samples, self.config.pps)) & 0xFFFFFFFF

    def send_batch(self, points: Sequence[LaserPoint]) -> None:
        """Send one wave chunk at its timeline position, waiting for its timestamp."""
        if self.state == self.CLOSED:
            raise StreamStateError("stream is closed; create a new stream to start again")
        cfg = self.config
        _check_int("batch size", len(points), MIN_WAVE_SAMPLES, cfg.max_batch)
        samples = b"".join(encode_sample(p) for p in points)
        if self.origin is None:
            self._start_epoch(self.clock())
        offset = boundary_us(self.epoch_samples, cfg.pps)
        duration = boundary_us(self.epoch_samples + len(points), cfg.pps) - offset
        deadline = self.origin + offset / 1_000_000
        self._wait_until(deadline)
        late = self.clock() - deadline
        if late > cfg.max_late_s:
            self._handle_late(late)
            return self.send_batch(points)
        with_config = self._config_due(offset, duration)
        msg = build_channel_message(cfg.channel, CHUNK_WAVE, (self.ts_base + offset) & 0xFFFFFFFF,
                                    self._config_bytes if with_config else b"",
                                    build_wave_chunk(cfg.sdm, duration, samples))
        self.sock.sendto(build_hello_header(IDNCMD_RT_CHANNEL, cfg.group, self.seq) + msg, self.addr)
        # Only a completed send advances the stream.
        self.seq = (self.seq + 1) & 0xFFFF
        self.epoch_samples += len(points)
        if with_config:
            self.last_config_offset_us = offset
            self.stats["config_packets"] += 1
        self.state = self.STREAMING
        self.stats["packets"] += 1
        self.stats["samples"] += len(points)
        self.stats["max_late_s"] = max(self.stats["max_late_s"], late)

    def _handle_late(self, late: float) -> None:
        now = self.clock()
        old_origin, old_base = self.origin, self.ts_base
        self.close()
        if not self.restart_on_late:
            raise StreamLateError(late, self.config.max_late_s)
        # A new connection and epoch: routing, configuration and pacing start
        # over from now; timestamps keep following real time.
        self.state = self.IDLE
        self._reset_epoch()
        self.ts_base = (old_base + int((now - old_origin) * 1_000_000)) & 0xFFFFFFFF
        self._start_epoch(now)

    def stream(self, points: Iterable[LaserPoint], seconds: Optional[float] = None) -> dict:
        """Send `points` as one continuous wave; stop after `seconds` of stream time if given."""
        src: Iterable[LaserPoint] = points
        if self.lead_in and self.state == self.IDLE and self.epoch_samples == 0:
            # IDN-Stream 3.2: the first sample after opening SHOULD be the blanked origin.
            src = itertools.chain([blank_point(0, 0)], src)
        cfg = self.config
        for batch in iter_batches(src, cfg.effective_packet_points, cfg.max_batch):
            if seconds is not None and boundary_us(self.epoch_samples, cfg.pps) >= seconds * 1_000_000:
                break
            self.send_batch(batch)
        return dict(self.stats)

    def stream_points(self, points: Sequence[LaserPoint], seconds: Optional[float] = None, loop: bool = True) -> dict:
        if not points:
            raise ValueError("No points to stream")
        return self.stream(itertools.cycle(points) if loop else points, seconds)

    def close(self, ack: bool = False, attempts: int = 3, timeout: float = 0.25) -> Optional[dict]:
        """Graceful IDN-RT close on this link. With ack, wait for (and return) the ACK.

        An unanswered close is repeated with a new sequence number: it carries
        no samples, so repeating it cannot duplicate content. Result 0xEB means
        the connection was already gone, which also counts as closed.
        """
        if self.state != self.STREAMING:
            self.state = self.CLOSED
            return None
        reply = None
        for _ in range(attempts if ack else 1):
            seq = self.seq
            self.sock.sendto(build_close_packet(self.config.group, seq, ack), self.addr)
            self.seq = (self.seq + 1) & 0xFFFF
            self.state = self.CLOSED
            if not ack:
                break
            got = _receive_matching(self.sock, self.clock, timeout, self._ack_matcher(seq))
            if got:
                reply = got[0]
                break
        return reply

    def abort(self) -> None:
        """IDN-RT abort: the receiver resets the session immediately."""
        if self.state == self.STREAMING:
            self.sock.sendto(build_abort_packet(self.config.group, self.seq), self.addr)
            self.seq = (self.seq + 1) & 0xFFFF
        self.state = self.CLOSED

    def _ack_matcher(self, seq: int):
        def accept(data, src):
            if src[:2] != self.addr[:2]:
                return None
            try:
                return parse_ack(data, self.config.group, seq)
            except IDNResponseError as exc:
                if exc.reason in ("short", "command", "group", "sequence"):
                    return None
                raise
        return accept


class IDNSender:
    """Management requests and stream creation for one IDN-Hello server."""

    def __init__(self, host: str, port: int = IDN_PORT, bind: Optional[str] = None, quiet: bool = False,
                 group: int = 0, broadcast: bool = False, sock=None,
                 clock: Callable[[], float] = time.perf_counter,
                 sleep_until: Callable[[float], None] = wait_until_precise):
        self.addr = (host, port)
        self.quiet = quiet
        self.group = _check_int("client group", group, 0, 15)
        self.broadcast = broadcast
        self.clock = clock
        self.sleep_until = sleep_until
        if sock is None:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
            except OSError:
                pass
            if broadcast:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            if bind:
                sock.bind((bind, 0))
        self.sock = sock
        # Management transactions have their own identifiers: they never touch
        # a stream's RT sequence.
        self.mgmt_seq = random.randrange(0x10000)

    def close_socket(self) -> None:
        self.sock.close()

    def _next_mgmt_seq(self) -> int:
        seq = self.mgmt_seq
        self.mgmt_seq = (self.mgmt_seq + 1) & 0xFFFF
        return seq

    def _transaction(self, cmd: int, payload: bytes, parse, timeout: float, collect: bool = False) -> list:
        seq = self._next_mgmt_seq()
        self.sock.sendto(build_hello_header(cmd, self.group, seq) + payload, self.addr)

        def accept(data, src):
            if not self.broadcast and src[:2] != self.addr[:2]:
                return None
            try:
                return src, parse(data, self.group, seq)
            except IDNResponseError as exc:
                if exc.reason in ("short", "command", "group", "sequence"):
                    return None
                raise
        return _receive_matching(self.sock, self.clock, timeout, accept, collect)

    def ping(self, payload: bytes = b"", timeout: float = 1.0) -> Optional[float]:
        """Round trip in seconds for an exact echo, or None on timeout."""
        t0 = self.clock()
        got = self._transaction(IDNCMD_PING_REQ, payload,
                                lambda d, g, s: parse_ping_response(d, payload, g, s) or True, timeout)
        return (self.clock() - t0) if got else None

    def scan(self, timeout: float = 1.0) -> List[dict]:
        return [dict(r, source=f"{src[0]}:{src[1]}")
                for src, r in self._transaction(IDNCMD_SCAN_REQ, b"", parse_scan_response, timeout, collect=self.broadcast)]

    def svcmap(self, timeout: float = 1.0) -> Optional[dict]:
        got = self._transaction(IDNCMD_SVCMAP_REQ, b"", parse_svcmap_response, timeout)
        return got[0][1] if got else None

    def open_stream(self, pps: int, packet_points: int = 64, first_seq: Optional[int] = None,
                    ts_base: Optional[int] = None, lead_in: bool = True, restart_on_late: bool = False,
                    **config) -> IDNStream:
        cfg = StreamConfig(pps=pps, packet_points=packet_points, group=self.group, **config)
        return IDNStream(self.sock, self.addr, cfg, first_seq=first_seq, ts_base=ts_base, clock=self.clock,
                         sleep_until=self.sleep_until, lead_in=lead_in, restart_on_late=restart_on_late)


def ilda_points(path, pps, fps=30.0, loop=False, scale=1.0) -> Iterator[LaserPoint]:
    """All frames of an ILDA file as one continuous point sequence."""
    frames = load_ilda(path)
    if not frames:
        raise RuntimeError(f"No supported ILDA frames found in {path}")
    frame_points = max(1, int(round(pps / fps))) if fps > 0 else 0
    prepared = []
    for f in frames:
        pts = normalize_ilda_points(f.points, scale=scale)
        prepared.append(resample_points(pts, frame_points) if frame_points > 0 else pts)
    while True:
        for pts in prepared:
            yield from pts
        if not loop:
            return


def stream_ilda_file(stream: IDNStream, path, pps, fps=30.0, loop=False, scale=1.0) -> dict:
    """One schedule across ILDA frames: frame ends never produce short packets."""
    return stream.stream(ilda_points(path, pps, fps, loop, scale))


def _int_range(lo: int, hi: int):
    def parse(text):
        try:
            v = int(text, 0)
        except ValueError:
            raise argparse.ArgumentTypeError(f"not an integer: {text!r}")
        if not lo <= v <= hi:
            raise argparse.ArgumentTypeError(f"must be in {lo}..{hi}, got {v}")
        return v
    return parse


def build_arg_parser():
    examples = """examples:
  python idn_sender.py --host 192.168.1.50 circle --seconds 5
  python idn_sender.py --host 192.168.1.50 --packet-points 64 square --seconds 10
  python idn_sender.py --host 192.168.1.50 --pps 30000 ild show.ild --fps 30 --loop
  python idn_sender.py --host 192.168.1.50 --channel 3 --service-id 1 sine --seconds 5
  python idn_sender.py --host 192.168.1.50 scan
  python idn_sender.py --host 255.255.255.255 --broadcast scan
  python idn_sender.py --host 192.168.1.50 ping --size 64

Every drawing command closes its IDN-RT connection over its own socket when it
ends (including Ctrl-C). There is no separate stop command: another process
would be a different connection. To stop a stream started elsewhere, stop that
sender or use the board's web UI.
"""
    p = argparse.ArgumentParser(
        description="IDN-Hello / IDN-RT continuous-graphics sender with ILW-style test commands",
        epilog=examples,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--host", required=True, help="DAC IP address (or a broadcast address with --broadcast)")
    p.add_argument("--port", type=_int_range(1, 65535), default=IDN_PORT, help=f"UDP port, default {IDN_PORT}")
    p.add_argument("--bind", default=None, help="Optional local source IP to bind before sending")
    p.add_argument("--group", type=_int_range(0, 15), default=0, help="IDN-Hello client group 0..15. Default: 0")
    p.add_argument("--broadcast", action="store_true", help="allow a broadcast --host and accept scan replies from any unit")
    p.add_argument("--timeout", type=float, default=1.0, help="reply wait for scan/svcmap/ping in seconds. Default: 1.0")
    p.add_argument("--quiet", action="store_true")

    st = p.add_argument_group("streaming (square, circle, sine, ild)")
    st.add_argument("--pps", type=_int_range(1, MAX_PPS), default=30000, help="Point output rate. Default: 30000")
    st.add_argument("--packet-points", type=_int_range(MIN_WAVE_SAMPLES, MAX_UDP_PAYLOAD), default=64,
                    help=f"Nominal points per packet, {MIN_WAVE_SAMPLES}..capacity (179 at the default budget). Default: 64")
    st.add_argument("--channel", type=_int_range(0, 63), default=0, help="IDN-Stream channel ID 0..63. Default: 0")
    st.add_argument("--service-id", type=_int_range(0, 255), default=0,
                    help="Service to route to; 0 = the receiver's default service for the mode. Default: 0")
    st.add_argument("--budget", type=_int_range(1, MAX_UDP_PAYLOAD), default=DEFAULT_UDP_BUDGET,
                    help=f"Max UDP payload octets per packet. Default: {DEFAULT_UDP_BUDGET} (Ethernet MTU 1500 - IP/UDP headers)")
    st.add_argument("--max-late-ms", type=float, default=DEFAULT_MAX_LATE_S * 1000,
                    help="Close the stream if the sender falls this far behind its timeline. Default: 100")
    st.add_argument("--restart-on-late", action="store_true",
                    help="After falling behind, reopen with fresh routing and timing instead of exiting")
    st.add_argument("--no-lead-in", action="store_true",
                    help="Do not prepend the blanked origin sample (IDN-Stream 3.2 recommends it)")

    sub = p.add_subparsers(dest="cmd", required=True)
    sq = sub.add_parser("square", help="draw a square test pattern")
    sq.add_argument("--seconds", type=float, default=5.0)
    sq.add_argument("--size", type=int, default=22000)
    sq.add_argument("--points-per-side", type=int, default=80)

    ci = sub.add_parser("circle", help="draw a color circle test pattern")
    ci.add_argument("--seconds", type=float, default=5.0)
    ci.add_argument("--radius", type=int, default=22000)
    ci.add_argument("--points", type=int, default=360)

    si = sub.add_parser("sine", help="draw a sine-wave test pattern")
    si.add_argument("--seconds", type=float, default=5.0)
    si.add_argument("--width", type=int, default=26000)
    si.add_argument("--height", type=int, default=16000)
    si.add_argument("--points", type=int, default=400)
    si.add_argument("--cycles", type=float, default=2.0)

    ild = sub.add_parser("ild", help="stream an ILDA file as one continuous wave")
    ild.add_argument("path")
    ild.add_argument("--fps", type=float, default=30.0)
    ild.add_argument("--loop", action="store_true")
    ild.add_argument("--scale", type=float, default=1.0)

    sub.add_parser("scan", help="send a scan request and print each unit that answers")
    sub.add_parser("svcmap", help="request and print the service map")
    pg = sub.add_parser("ping", help="send a ping and check for an exact echo")
    pg.add_argument("--size", type=_int_range(0, MAX_UDP_PAYLOAD - HELLO_HEADER_BYTES), default=16,
                    help="payload octets. Default: 16")
    return p


def main():
    parser = build_arg_parser()
    args = parser.parse_args()
    sender = IDNSender(args.host, args.port, bind=args.bind, quiet=args.quiet, group=args.group,
                       broadcast=args.broadcast)
    say = (lambda *a: None) if args.quiet else print
    try:
        if args.cmd == "scan":
            units = sender.scan(args.timeout)
            for u in units:
                flags = [k for k in ("realtime", "malfunction", "offline", "excluded", "occupied") if u[k]]
                say(f"{u['source']}  {u['host_name']!r}  unit {u['unit_id'] or '?'}  "
                    f"IDN-Hello {u['protocol_version']}  [{', '.join(flags)}]")
            if not units:
                say("no scan response")
            return 0 if units else 1
        if args.cmd == "svcmap":
            m = sender.svcmap(args.timeout)
            if m is None:
                say("no service map response")
                return 1
            for r in m["relays"]:
                say(f"relay {r['relay_number']}: {r['name']!r}")
            for s in m["services"]:
                say(f"service {s['service_id']}: type 0x{s['service_type']:02X} {s['name']!r}"
                    + (" (default)" if s["default"] else "") + (f" via relay {s['relay_number']}" if s["relay_number"] else ""))
            return 0
        if args.cmd == "ping":
            payload = bytes(random.randrange(256) for _ in range(args.size))
            rtt = sender.ping(payload, args.timeout)
            say("no ping response" if rtt is None else f"exact echo of {args.size} octets in {rtt * 1000:.2f} ms")
            return 0 if rtt is not None else 1

        try:
            stream = sender.open_stream(args.pps, args.packet_points, channel=args.channel, service_id=args.service_id,
                                        budget=args.budget, max_late_s=args.max_late_ms / 1000.0,
                                        lead_in=not args.no_lead_in, restart_on_late=args.restart_on_late)
        except ValueError as exc:
            parser.error(str(exc))
        t0 = time.perf_counter()
        try:
            if args.cmd == "square":
                stream.stream_points(make_square(args.size, args.points_per_side), args.seconds)
            elif args.cmd == "circle":
                stream.stream_points(make_circle(args.radius, args.points), args.seconds)
            elif args.cmd == "sine":
                stream.stream_points(make_sine(args.width, args.height, args.points, args.cycles), args.seconds)
            elif args.cmd == "ild":
                stream_ilda_file(stream, args.path, args.pps, args.fps, args.loop, args.scale)
            else:
                raise RuntimeError("Unknown command")
        finally:
            stream.close()
            elapsed = max(0.001, time.perf_counter() - t0)
            s = stream.stats
            say(f"Streamed {s['samples']} points in {s['packets']} packets over {elapsed:.2f}s "
                f"({s['samples'] / elapsed:.0f} pps, {s['config_packets']} configured, "
                f"max lateness {s['max_late_s'] * 1000:.1f} ms)")
        return 0
    finally:
        sender.close_socket()


if __name__ == "__main__":
    raise SystemExit(main())
