#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""A host at the other end of Ethernet port 2's cable (`run --lan`).

    scripts/lan_host.py [-n NAME] [--client] [--for SECONDS]

It joins the LAN at devices/NAME/lan.sock (QEMU's stream netdev: each frame
after its length, 4 bytes big-endian) as a small host with an address of its
own; connecting is plugging the cable in, and quitting pulls it out. It says
what happens, a line each:

- by default it is the LAN's DHCP server and gateway, 192.168.77.1: it
  offers 192.168.77.10, answers ARP and ping for itself, and once the
  Flipper has the address pings it (Flipper OS a DHCP client on the port,
  as its Minimal profile is);
- with --client it is a DHCP client: it asks for an address, and pings the
  router it is given (Flipper OS the LAN's router, as its Router profile
  is), and answers ping for itself after.

Standard library only.
"""
import argparse
import pathlib
import random
import socket
import struct
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
GATEWAY, OFFER, MASK = "192.168.77.1", "192.168.77.10", "255.255.255.0"
BROADCAST = b"\xff" * 6
ARP, IPV4 = 0x0806, 0x0800
DHCP_COOKIE = b"\x63\x82\x53\x63"
DISCOVER, OFFERED, REQUEST, ACK = 1, 2, 3, 5


def say(text):
    print(text, flush=True)


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff


def ipv4(src, dst, proto, payload):
    head = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0, 0x4000,
                       64, proto, 0, socket.inet_aton(src),
                       socket.inet_aton(dst))
    return head[:10] + struct.pack("!H", checksum(head)) + head[12:] + payload


def udp(src, dst, sport, dport, payload):
    return ipv4(src, dst, 17, struct.pack("!HHHH", sport, dport,
                                          8 + len(payload), 0) + payload)


def icmp(src, dst, kind, ident, seq, data):
    body = struct.pack("!BBHHH", kind, 0, 0, ident, seq) + data
    body = body[:2] + struct.pack("!H", checksum(body)) + body[4:]
    return ipv4(src, dst, 1, body)


def dhcp(op, xid, mac, yiaddr, options, ciaddr="0.0.0.0"):
    opts = b"".join(bytes([k, len(v)]) + v for k, v in options) + b"\xff"
    return (struct.pack("!BBBBIHH4s4s4s4s", op, 1, 6, 0, xid, 0, 0x8000,
                        socket.inet_aton(ciaddr), socket.inet_aton(yiaddr),
                        b"\0" * 4, b"\0" * 4) + mac + b"\0" * 10 +
            b"\0" * 192 + DHCP_COOKIE + opts)


def dhcp_options(msg):
    out, i = {}, 240
    while i < len(msg) and msg[i] != 255:
        if msg[i] == 0:
            i += 1
            continue
        out[msg[i]] = msg[i + 2:i + 2 + msg[i + 1]]
        i += 2 + msg[i + 1]
    return out


def mac_text(mac):
    return ":".join(f"{b:02x}" for b in mac)


class Lan:
    """the cable's end: frames out, and in"""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(str(path))
        self.buf = b""
        self.mac = bytes([0x02, 0x4c, 0x41, 0x4e] +
                         [random.randrange(256) for _ in range(2)])

    def send(self, dst, kind, payload):
        frame = dst + self.mac + struct.pack("!H", kind) + payload
        frame += b"\0" * max(0, 60 - len(frame))
        self.s.sendall(struct.pack("!I", len(frame)) + frame)

    def recv(self, timeout):
        end = time.time() + timeout
        while True:
            if len(self.buf) >= 4:
                n = struct.unpack("!I", self.buf[:4])[0]
                if len(self.buf) >= 4 + n:
                    frame, self.buf = self.buf[4:4 + n], self.buf[4 + n:]
                    return frame
            left = end - time.time()
            if left <= 0:
                return None
            self.s.settimeout(left)
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                sys.exit("the emulator has gone")
            self.buf += chunk


class Host:
    """a host on the LAN: ARP and ping for its own address, and what the
    role adds"""

    def __init__(self, lan, ip):
        self.lan, self.ip = lan, ip
        self.macs = {}                  # address: MAC, as heard
        self.seq = 0

    def arp(self, oper, tha, tpa):
        self.lan.send(tha if oper == 2 else BROADCAST, ARP, struct.pack(
            "!HHBBH6s4s6s4s", 1, IPV4, 6, 4, oper, self.lan.mac,
            socket.inet_aton(self.ip), tha, socket.inet_aton(tpa)))

    def ping(self, to):
        mac = self.macs.get(to)
        if mac is None:
            self.arp(1, b"\0" * 6, to)
            return
        self.seq += 1
        self.lan.send(mac, IPV4, icmp(self.ip, to, 8, 0x4c41, self.seq,
                                      b"lan_host.py"))

    def handle(self, frame):
        """the frame's IPv4 packet if it is one for the role, else None"""
        kind = struct.unpack("!H", frame[12:14])[0]
        if kind == ARP and len(frame) >= 42:
            oper, sha, spa, tpa = struct.unpack("!6xH6s4s6x4s", frame[14:42])
            spa, tpa = socket.inet_ntoa(spa), socket.inet_ntoa(tpa)
            if spa != "0.0.0.0":
                self.macs[spa] = sha
            if oper == 1 and tpa == self.ip:
                self.arp(2, sha, spa)
            return None
        if kind != IPV4 or len(frame) < 34:
            return None
        pkt = frame[14:]
        proto, src, dst = pkt[9], socket.inet_ntoa(pkt[12:16]), \
            socket.inet_ntoa(pkt[16:20])
        body = pkt[(pkt[0] & 15) * 4:]
        if src != "0.0.0.0":
            self.macs.setdefault(src, frame[6:12])
        if proto == 1 and dst == self.ip and body[:1] == b"\x08":
            say(f"ping from {src}")
            self.lan.send(frame[6:12], IPV4,
                          icmp(self.ip, src, 0, *struct.unpack("!HH", body[4:8]),
                               body[8:]))
            return None
        if proto == 1 and dst == self.ip and body[:1] == b"\x00":
            if not getattr(self, "answered", False):
                say(f"ping answered by {src}")
                self.answered = True
            return None
        return pkt


def serve(lan, until):
    host = Host(lan, GATEWAY)
    lease = None                        # the Flipper's MAC, once acknowledged
    pinged = 0
    while time.time() < until:
        frame = lan.recv(0.5)
        if frame:
            pkt = host.handle(frame)
            if pkt and pkt[9] == 17:
                body = pkt[(pkt[0] & 15) * 4:]
                dport = struct.unpack("!H", body[2:4])[0]
                msg = body[8:]
                if dport == 67 and len(msg) >= 240 and \
                        msg[236:240] == DHCP_COOKIE:
                    opts = dhcp_options(msg)
                    xid, mac = struct.unpack("!I", msg[4:8])[0], msg[28:34]
                    what = opts.get(53, b"\0")[0]
                    reply = {DISCOVER: OFFERED, REQUEST: ACK}.get(what)
                    if reply:
                        options = [(53, bytes([reply])),
                                   (54, socket.inet_aton(GATEWAY)),
                                   (51, struct.pack("!I", 3600)),
                                   (1, socket.inet_aton(MASK)),
                                   (3, socket.inet_aton(GATEWAY))]
                        lan.send(BROADCAST, IPV4, udp(
                            GATEWAY, "255.255.255.255", 67, 68,
                            dhcp(2, xid, mac, OFFER, options)))
                        if reply == ACK:
                            say(f"DHCP: {OFFER} to {mac_text(mac)}")
                            host.macs[OFFER] = mac
                            lease = mac
        if lease and not getattr(host, "answered", False) and \
                time.time() - pinged > 2:
            host.ping(OFFER)
            pinged = time.time()


def client(lan, until):
    host = Host(lan, "0.0.0.0")
    xid = random.randrange(1 << 32)
    got, server, router = None, None, None
    asked = 0
    while time.time() < until and got is None:
        if time.time() - asked > 3:     # (again, if nothing came)
            if server:
                lan.send(BROADCAST, IPV4, udp("0.0.0.0", "255.255.255.255",
                         68, 67, dhcp(1, xid, lan.mac, "0.0.0.0", [
                             (53, bytes([REQUEST])), (50, socket.inet_aton(offer)),
                             (54, socket.inet_aton(server))])))
            else:
                lan.send(BROADCAST, IPV4, udp("0.0.0.0", "255.255.255.255",
                         68, 67, dhcp(1, xid, lan.mac, "0.0.0.0", [
                             (53, bytes([DISCOVER])), (55, bytes([1, 3, 6]))])))
            asked = time.time()
        frame = lan.recv(0.5)
        pkt = host.handle(frame) if frame else None
        if not pkt or pkt[9] != 17:
            continue
        body = pkt[(pkt[0] & 15) * 4:]
        msg = body[8:]
        if struct.unpack("!H", body[2:4])[0] != 68 or len(msg) < 240 or \
                struct.unpack("!I", msg[4:8])[0] != xid:
            continue
        opts = dhcp_options(msg)
        what = opts.get(53, b"\0")[0]
        if what == OFFERED and not server:
            offer = socket.inet_ntoa(msg[16:20])
            server = socket.inet_ntoa(opts.get(54, msg[20:24]))
            asked = 0
        elif what == ACK:
            got = socket.inet_ntoa(msg[16:20])
            router = socket.inet_ntoa(opts[3][:4]) if 3 in opts else server
    if got is None:
        say("DHCP: no address")
        return
    say(f"DHCP: got {got} from {server}, router {router}")
    host.ip = got
    pinged = 0
    while time.time() < until:
        if not getattr(host, "answered", False) and time.time() - pinged > 2:
            host.ping(router)
            pinged = time.time()
        frame = lan.recv(0.5)
        if frame:
            host.handle(frame)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-n", "--name", default="one", help="the device's name")
    ap.add_argument("--client", action="store_true",
                    help="a DHCP client, the Flipper the LAN's router")
    ap.add_argument("--for", dest="secs", type=float, default=0,
                    help="seconds, then quit (pull the cable out); 0: until "
                         "interrupted")
    args = ap.parse_args()
    path = ROOT / "devices" / args.name / "lan.sock"
    for _ in range(50):                 # (QEMU makes it as it starts)
        if path.exists():
            break
        time.sleep(0.2)
    else:
        sys.exit(f"no {path}: run --lan")
    lan = Lan(path)
    say(f"plugged in: {mac_text(lan.mac)}")
    until = time.time() + args.secs if args.secs else float("inf")
    try:
        (client if args.client else serve)(lan, until)
    except KeyboardInterrupt:
        pass
    say("pulled out")
    return 0


if __name__ == "__main__":
    sys.exit(main())
