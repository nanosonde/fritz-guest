#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded RFC 3931 guest probe; no interfaces or routes configured.

Default mode sends DISCOVER only. --cross-tunnel temporarily accepts and
releases two DHCP leases in userspace, without assigning guest addresses.

Run with CAP_NET_RAW/root on a LAN that passes IP protocol 115:
    sudo python3 tools/fritz_guest_probe.py
Use --self-test for offline packet checks. This is not a persistent VPN client.
"""

import argparse
import ipaddress
import json
import secrets
import select
import socket
import struct
import subprocess
import sys
import time


MESSAGES = {1: "SCCRQ", 2: "SCCRP", 3: "SCCCN", 4: "StopCCN",
            6: "HELLO", 10: "ICRQ", 11: "ICRP", 12: "ICCN",
            14: "CDN", 16: "SLI", 20: "ACK"}
KNOWN_AVPS = {0, 1, 2, 3, 4, 5, 7, 8, 10, 15, 19, 24, 25, 34, 36,
              59, 60, 61, 62, 63, 64, 65, 66, 68, 69, 70, 71, 72, 73,
              74, 75}


def integer(value, width=4):
    return value.to_bytes(width, "big")


def avp(attribute, value, mandatory=True):
    if len(value) > 1017:
        raise ValueError("AVP too large")
    return struct.pack("!HHH", len(value) + 6 | (0x8000 if mandatory else 0),
                       0, attribute) + value


def control(connection, sequence, acknowledgement, body):
    return struct.pack("!IHHIHH", 0, 0xC803, 12 + len(body), connection,
                       sequence, acknowledgement) + body


def parse_control(payload):
    if len(payload) < 16:
        raise ValueError("Truncated control header")
    session, flags, length, connection, sequence, acknowledgement = struct.unpack(
        "!IHHIHH", payload[:16])
    if session or flags != 0xC803 or length < 12 or length + 4 != len(payload):
        raise ValueError("Invalid control header/length")
    fields = {}
    cursor = 16
    while cursor < len(payload):
        if cursor + 6 > len(payload):
            raise ValueError("Truncated AVP header")
        bits, vendor, attribute = struct.unpack("!HHH", payload[cursor:cursor + 6])
        size = bits & 0x3FF
        if size < 6 or cursor + size > len(payload) or bits & 0x4000:
            raise ValueError("Invalid or hidden AVP")
        if (vendor != 0 or attribute not in KNOWN_AVPS) and bits & 0x8000:
            if (vendor, attribute) != (872, 1):
                raise ValueError(f"Unsupported mandatory AVP {vendor}/{attribute}")
        key = attribute if vendor == 0 else (vendor, attribute)
        if key in fields:
            raise ValueError(f"Duplicate AVP {key}")
        if cursor == 16 and key != 0:
            raise ValueError("Message Type must be the first AVP")
        fields[key] = payload[cursor + 6:cursor + size]
        cursor += size
    message = field_integer(fields, 0, 2) if fields else 20
    return connection, sequence, acknowledgement, message, fields


def field_integer(fields, attribute, width):
    value = fields.get(attribute)
    if value is None or len(value) != width:
        raise ValueError(f"Missing or invalid AVP {attribute}")
    return int.from_bytes(value, "big")


def checksum(packet):
    padded = packet + b"\0" * (len(packet) % 2)
    total = sum(struct.unpack(f"!{len(padded) // 2}H", padded))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def udp_frame(mac, destination_mac, source, destination, payload, source_port=49001, destination_port=49002):
    udp = struct.pack("!HHHH", source_port, destination_port, 8 + len(payload), 0) + payload
    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0,
                         64, 17, 0, socket.inet_aton(source), socket.inet_aton(destination))
    header = header[:10] + integer(checksum(header), 2) + header[12:]
    return destination_mac + mac + b"\x08\x00" + header + udp


def dhcp_discover(mac, transaction):
    bootp = struct.pack("!BBBBIHH", 1, 1, 6, 0, transaction, 0, 0x8000)
    bootp += bytes(16) + mac + bytes(10 + 64 + 128)
    bootp += b"\x63\x82\x53\x63\x35\x01\x01\x3d\x07\x01" + mac
    bootp += b"\x37\x03\x01\x03\x06\xff"
    bootp = bootp.ljust(300, b"\0")
    return udp_frame(mac, b"\xff" * 6, "0.0.0.0", "255.255.255.255", bootp, 68, 67)


def dhcp_select(mac, transaction, address, server, release=False, server_mac=None):
    bootp = bytearray(dhcp_discover(mac, transaction)[42:282])
    options = b"\x35\x01" + (b"\x07" if release else b"\x03")
    options += b"\x36\x04" + socket.inet_aton(server) + b"\x3d\x07\x01" + mac
    if release:
        bootp[10:12] = bytes(2)
        bootp[12:16] = socket.inet_aton(address)
    else:
        options += b"\x32\x04" + socket.inet_aton(address)
    payload = (bytes(bootp) + options + b"\xff").ljust(300, b"\0")
    return udp_frame(mac, server_mac if release else b"\xff" * 6,
                     address if release else "0.0.0.0",
                     server if release else "255.255.255.255", payload, 68, 67)


def dhcp_reply(frame, transaction, mac, network):
    if len(frame) < 282 or frame[12:14] != b"\x08\x00" or frame[23] != 17:
        return None
    header_length = (frame[14] & 15) * 4
    if header_length < 20:
        return None
    udp = frame[14 + header_length:]
    if len(udp) < 248 or udp[:4] != struct.pack("!HH", 67, 68):
        return None
    bootp = udp[8:int.from_bytes(udp[4:6], "big")]
    if (len(bootp) < 240 or bootp[:3] != b"\x02\x01\x06"
            or bootp[4:8] != integer(transaction) or bootp[28:34] != mac
            or bootp[236:240] != b"\x63\x82\x53\x63"):
        return None
    options = {}
    cursor = 240
    while cursor < len(bootp):
        kind = bootp[cursor]
        cursor += 1
        if kind == 255:
            break
        if kind == 0:
            continue
        if cursor >= len(bootp):
            return None
        size = bootp[cursor]
        cursor += 1
        if cursor + size > len(bootp):
            return None
        options[kind] = bootp[cursor:cursor + size]
        cursor += size
    if options.get(53) not in (b"\x02", b"\x05") or len(options.get(54, b"")) != 4:
        return None
    address = ipaddress.IPv4Address(bootp[16:20])
    if address not in network:
        return None
    return options[53][0], str(address), socket.inet_ntoa(options[54]), frame[6:12]


def arp_frame(mac, address, peer_mac, peer_address, reply=False):
    destination = peer_mac if reply else b"\xff" * 6
    payload = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2 if reply else 1)
    payload += mac + socket.inet_aton(address)
    payload += (peer_mac if reply else bytes(6)) + socket.inet_aton(peer_address)
    return (destination + mac + b"\x08\x06" + payload).ljust(60, b"\0")


def describe_frame(frame, network, transaction, mac):
    if len(frame) < 14:
        return None
    if frame[12:14] == b"\x08\x06" and len(frame) >= 42:
        if frame[14:20] != b"\x00\x01\x08\x00\x06\x04":
            return None
        source = ipaddress.IPv4Address(frame[28:32])
        target = ipaddress.IPv4Address(frame[38:42])
        if source in network or target in network:
            return f"Guest ARP: {source} -> {target}"
    if frame[12:14] != b"\x08\x00" or len(frame) < 34:
        return None
    packet = frame[14:]
    header_length = (packet[0] & 15) * 4
    total_length = int.from_bytes(packet[2:4], "big")
    if packet[0] >> 4 != 4 or header_length < 20 or not header_length <= total_length <= len(packet):
        return None
    packet = packet[:total_length]
    source = ipaddress.IPv4Address(packet[12:16])
    target = ipaddress.IPv4Address(packet[16:20])
    if packet[9] == 17 and not int.from_bytes(packet[6:8], "big") & 0x3FFF:
        udp = packet[header_length:]
        if len(udp) >= 8:
            source_port, target_port, udp_length, _ = struct.unpack("!HHHH", udp[:8])
            bootp = udp[8:udp_length]
            if (source_port, target_port) == (67, 68) and len(bootp) >= 240:
                if (bootp[0] == 2 and bootp[4:8] == integer(transaction)
                        and bootp[28:34] == mac and bootp[236:240] == b"\x63\x82\x53\x63"):
                    offered = ipaddress.IPv4Address(bootp[16:20])
                    if offered in network:
                        return f"Matching DHCP reply from {source}: offered guest address {offered}"
    if source in network or target in network:
        return f"Guest IPv4: {source} -> {target}, protocol {packet[9]}, {len(packet)} bytes"
    return None


class Probe:
    def __init__(self, server, local, timeout, router_id=None):
        self.server = server
        self.local = local
        self.router_id = router_id if router_id is not None else int.from_bytes(socket.inet_aton(local), "big")
        self.timeout = timeout
        self.connection = secrets.randbelow(0xFFFFFFFF) + 1
        self.session = secrets.randbelow(0xFFFFFFFF) + 1
        self.peer_connection = 0
        self.peer_session = 0
        self.sequence = 0
        self.expected = 0
        self.pending = None
        self.closed = False
        self.data_packets = 0
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_RAW, 115)
        self.socket.bind((local, 0))
        self.socket.setblocking(False)

    def send_control(self, message, attributes=b""):
        body = avp(0, integer(message, 2)) + attributes
        sequence = self.sequence
        if message != 20:
            if self.pending is not None:
                raise RuntimeError("Previous control message is not acknowledged")
            self.pending = (sequence, body)
            self.sequence = (self.sequence + 1) & 0xFFFF
        self.socket.sendto(control(self.peer_connection, sequence, self.expected, body),
                           (self.server, 0))
        print(f"TX {MESSAGES[message]} Ns={sequence} Nr={self.expected}", flush=True)

    def receive(self, timeout):
        if not select.select([self.socket], [], [], max(0, timeout))[0]:
            return None
        packet, address = self.socket.recvfrom(65535)
        if address[0] != self.server or len(packet) < 20:
            return None
        header_length = (packet[0] & 15) * 4
        if header_length < 20 or len(packet) < header_length + 4:
            return None
        payload = packet[header_length:]
        session = int.from_bytes(payload[:4], "big")
        if session:
            if session == self.session and len(payload) >= 8:
                self.data_packets += 1
                return ("data", payload[8:])
            return None
        if len(payload) < 16:
            return None
        connection = int.from_bytes(payload[8:12], "big")
        if connection != self.connection and not (connection == 0 and not self.peer_connection):
            return None
        _, sequence, acknowledgement, message, fields = parse_control(payload)
        if ((self.sequence - acknowledgement) & 0xFFFF) >= 0x8000:
            raise ValueError("Peer acknowledged an unsent message")
        if self.pending and acknowledgement == self.sequence:
            self.pending = None
        print(f"RX {MESSAGES.get(message, message)} Ns={sequence} Nr={acknowledgement}", flush=True)
        if message == 20:
            return ("control", message, fields)
        if sequence != self.expected:
            self.send_control(20)
            return None
        self.expected = (self.expected + 1) & 0xFFFF
        if message == 2:
            self.peer_connection = field_integer(fields, 61, 4)
            if not self.peer_connection:
                raise ValueError("Peer supplied zero connection ID")
        self.send_control(20)
        if message == 4:
            self.closed = True
        if message in (4, 14):
            raise RuntimeError(f"Peer rejected/closed tunnel: {MESSAGES[message]}, Result Code AVP={fields.get(1, b'').hex()}")
        if 73 in fields or 59 in fields:
            raise ValueError("Peer requests authentication/integrity checking; unsupported by this probe")
        if message not in (2, 6, 11, 16):
            raise ValueError(f"Unexpected control message {message}")
        if message == 16:
            if field_integer(fields, 64, 4) != self.session:
                raise ValueError("SLI addresses a different session")
            if 71 in fields and not field_integer(fields, 71, 2) & 1:
                raise RuntimeError("Peer marked guest circuit inactive")
        return ("control", message, fields)

    def exchange(self, message, attributes=b"", reply=None):
        self.send_control(message, attributes)
        return self.wait(message, reply)

    def wait(self, message, reply=None):
        deadline = time.monotonic() + self.timeout
        interval = 1.0
        retry = time.monotonic() + interval
        response = None
        while time.monotonic() < deadline:
            event = self.receive(min(deadline, retry) - time.monotonic())
            if event and event[0] == "control" and event[1] == reply:
                response = event[2]
            if self.pending is None and (reply is None or response is not None):
                return response
            if time.monotonic() >= retry:
                if self.pending:
                    sequence, body = self.pending
                    self.socket.sendto(control(self.peer_connection, sequence, self.expected, body),
                                       (self.server, 0))
                    print(f"Retransmit {MESSAGES[message]}", flush=True)
                interval = min(interval * 2, 8)
                retry = time.monotonic() + interval
        raise TimeoutError(f"No completed {MESSAGES[message]} exchange within {self.timeout}s; reachability, filtering or interoperability remains unresolved")

    def establish(self):
        response = self.exchange(1, avp(7, b"fritz-guest-probe")
                                 + avp(60, integer(self.router_id))
                                 + avp(61, integer(self.connection))
                                 + avp(62, integer(5, 2))
                                 + avp(10, integer(1, 2)), reply=2)
        capabilities = response.get(62, b"")
        if not response.get(7) or len(response.get(60, b"")) != 4:
            raise ValueError("Missing SCCRP host name/router ID")
        if len(capabilities) % 2 or integer(5, 2) not in [capabilities[index:index + 2] for index in range(0, len(capabilities), 2)]:
            raise ValueError("Server does not advertise Ethernet pseudowires")
        self.exchange(3)
        response = self.exchange(10, avp(63, integer(self.session))
                                 + avp(64, integer(0))
                                 + avp(15, integer(int(time.time()) & 0xFFFFFFFF), False)
                                 + avp(68, integer(5, 2))
                                 + avp(66, b"fritz.box:guest")
                                 + avp(71, integer(3, 2))
                                 + avp(69, integer(1, 2))
                                 + avp(70, integer(0, 2)), reply=11)
        self.peer_session = field_integer(response, 63, 4)
        if not self.peer_session or field_integer(response, 64, 4) != self.session:
            raise ValueError("Invalid ICRP session IDs")
        if not field_integer(response, 71, 2) & 1:
            raise ValueError("Server guest circuit is not active")
        if response.get(65, b"") or response.get(69) != integer(1, 2):
            raise ValueError("Only zero cookies and default L2 sublayer are supported")
        if response.get(70, integer(0, 2)) != integer(0, 2):
            raise ValueError("Server requires data sequencing")
        if response.get((872, 1), integer(0, 2)) != integer(0, 2):
            raise ValueError("Nonzero AVM offset is unsupported")
        self.exchange(12, avp(63, integer(self.session))
                      + avp(64, integer(self.peer_session))
                      + avp(71, integer(3, 2)))
        print(f"Session established: local={self.session}, peer={self.peer_session}", flush=True)

    def observe(self, network, seconds):
        mac = b"\x02" + secrets.token_bytes(5)
        transaction = secrets.randbits(32)
        frame = dhcp_discover(mac, transaction)
        self.socket.sendto(integer(self.peer_session) + bytes(4) + frame, (self.server, 0))
        print(f"Sent DHCP DISCOVER, MAC={mac.hex(':')}, xid={transaction:08x}; observing {seconds}s", flush=True)
        deadline = time.monotonic() + seconds
        next_discover = time.monotonic() + 10
        matches = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_discover:
                self.socket.sendto(integer(self.peer_session) + bytes(4) + frame, (self.server, 0))
                print("Retry DHCP DISCOVER after bridge forwarding delay", flush=True)
                next_discover = time.monotonic() + 10
            event = self.receive(min(1, deadline - time.monotonic()))
            if event and event[0] == "data":
                received_frame = event[1]
                if len(received_frame) >= 14:
                    print(f"RX Ethernet: src={received_frame[6:12].hex(':')} dst={received_frame[:6].hex(':')} "
                          f"type/length=0x{received_frame[12:14].hex()} length={len(received_frame)}", flush=True)
                description = describe_frame(event[1], network, transaction, mac)
                if description:
                    matches += 1
                    print(description, flush=True)
        print(f"Received {self.data_packets} session data packets; {matches} guest-subnet observations", flush=True)
        return 0 if matches else 2

    def close(self):
        try:
            if self.peer_connection and not self.closed:
                if self.pending:
                    self.wait(int.from_bytes(self.pending[1][6:8], "big"))
                self.exchange(4, avp(1, integer(1, 2)) + avp(61, integer(self.connection)))
        except (OSError, ValueError, RuntimeError, TimeoutError) as error:
            print(f"Cleanup: {error}; peer may retain state until timeout", file=sys.stderr)
        finally:
            self.socket.close()


def cross_tunnel(server, local, second_local, timeout, network):
    probes = []
    macs = [b"\x02" + secrets.token_bytes(5) for _ in range(2)]
    transactions = [secrets.randbits(32) for _ in range(2)]
    ready = [False, False]
    offers = [None, None]
    leases = [None, None]
    received = set()
    token = secrets.token_bytes(16)
    frames = {}
    for sender in range(2):
        for kind in ("broadcast", "unicast"):
            destination = b"\xff" * 6 if kind == "broadcast" else macs[1 - sender]
            frame = destination + macs[sender] + b"\x88\xb5"
            frame += b"fritz-guest-cross-tunnel:" + token + integer(sender, 1) + kind.encode("ascii")
            frames[sender, kind] = frame.ljust(60, b"\0")
    try:
        for index in range(2):
            source = local if index == 0 else second_local
            probe = Probe(server, source, timeout, secrets.randbelow(0xFFFFFFFF) + 1)
            probes.append(probe)
            print(f"Establish endpoint {'AB'[index]}, source={source}, MAC={macs[index].hex(':')}", flush=True)
            probe.establish()
        if probes[0].peer_session == probes[1].peer_session:
            raise RuntimeError("Server returned the same session ID; endpoints are not independent")
        print(f"Two distinct server sessions: A={probes[0].peer_session}, B={probes[1].peer_session}", flush=True)
        deadline = time.monotonic() + 50
        next_send = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_send:
                for index, probe in enumerate(probes):
                    discover = dhcp_discover(macs[index], transactions[index])
                    if not leases[index]:
                        if offers[index]:
                            _, address, dhcp_server, _ = offers[index]
                            discover = dhcp_select(macs[index], transactions[index], address, dhcp_server)
                        probe.socket.sendto(integer(probe.peer_session) + bytes(4) + discover, (server, 0))
                    for kind in ("broadcast", "unicast"):
                        probe.socket.sendto(integer(probe.peer_session) + bytes(4) + frames[index, kind], (server, 0))
                    if all(leases):
                        address = leases[index][1]
                        peer_address = leases[1 - index][1]
                        frames[index, "ipv4"] = udp_frame(macs[index], macs[1 - index], address,
                                                         peer_address, b"fritz-guest-udp:" + token + integer(index, 1))
                        request = arp_frame(macs[index], address, macs[1 - index], peer_address)
                        for test_frame in (request, frames[index, "ipv4"]):
                            probe.socket.sendto(integer(probe.peer_session) + bytes(4) + test_frame, (server, 0))
                print("Sent DHCP and tagged broadcast/unicast frames from both endpoints", flush=True)
                next_send = time.monotonic() + 10
            for index, probe in enumerate(probes):
                event = probe.receive(0.05)
                if not event or event[0] != "data":
                    continue
                frame = event[1]
                dhcp = dhcp_reply(frame, transactions[index], macs[index], network)
                if dhcp:
                    ready[index] = True
                    if dhcp[0] == 2 and not leases[index]:
                        offers[index] = dhcp
                        request = dhcp_select(macs[index], transactions[index], dhcp[1], dhcp[2])
                        probe.socket.sendto(integer(probe.peer_session) + bytes(4) + request, (server, 0))
                    if dhcp[0] == 5 and offers[index] and dhcp[1:3] == offers[index][1:3]:
                        leases[index] = dhcp
                        print(f"Endpoint {'AB'[index]} DHCP ACK: {dhcp[1]}", flush=True)
                        if all(leases) and leases[0][1] == leases[1][1]:
                            raise RuntimeError("DHCP gave both endpoints the same address")
                        next_send = 0
                if all(leases):
                    peer_index = 1 - index
                    request = arp_frame(macs[peer_index], leases[peer_index][1], macs[index], leases[index][1])
                    reply = arp_frame(macs[peer_index], leases[peer_index][1], macs[index], leases[index][1], True)
                    if frame[:42] == request[:42]:
                        response = arp_frame(macs[index], leases[index][1], macs[peer_index], leases[peer_index][1], True)
                        probe.socket.sendto(integer(probe.peer_session) + bytes(4) + response, (server, 0))
                    if frame[:42] == reply[:42]:
                        if (index, "arp") not in received:
                            print(f"CONFIRMED endpoint {'AB'[index]} resolved {'AB'[peer_index]} by ARP", flush=True)
                        received.add((index, "arp"))
                for kind in ("broadcast", "unicast", "ipv4"):
                    expected_frame = frames.get((1 - index, kind))
                    if expected_frame is None:
                        continue
                    if frame[:len(expected_frame)] == expected_frame:
                        key = (1 - index, kind)
                        if key not in received:
                            print(f"CONFIRMED {'AB'[1 - index]} -> {'AB'[index]} {kind} across separate tunnels", flush=True)
                        received.add(key)
            if all(leases) and len(received) == 8:
                break
        print(f"Guest DHCP verified: A={ready[0]}, B={ready[1]}", flush=True)
        for sender in range(2):
            for kind in ("broadcast", "unicast", "arp", "ipv4"):
                print(f"{'AB'[sender]} -> {'AB'[1 - sender]} {kind}: "
                      f"{'RECEIVED' if (sender, kind) in received else 'NOT OBSERVED'}", flush=True)
        if not all(ready):
            print("INCONCLUSIVE: both endpoints were not independently verified by guest DHCP", flush=True)
            return 2
        if all(leases) and len(received) == 8:
            print("Cross-tunnel Ethernet, ARP resolution and guest IPv4 UDP work in both directions", flush=True)
            return 0
        print("Missing frames do not alone prove isolation; packet-type filtering or endpoint identity may matter", flush=True)
        return 2
    finally:
        for index, probe in enumerate(probes):
            if leases[index] and not probe.closed:
                _, address, dhcp_server, server_mac = leases[index]
                try:
                    release = dhcp_select(macs[index], transactions[index], address, dhcp_server,
                                          release=True, server_mac=server_mac)
                    probe.socket.sendto(integer(probe.peer_session) + bytes(4) + release, (server, 0))
                    print(f"Sent DHCP RELEASE for endpoint {'AB'[index]} ({address})", flush=True)
                except OSError as error:
                    print(f"DHCP release failed: {error}", file=sys.stderr)
        for probe in reversed(probes):
            probe.close()


def self_test():
    from unittest.mock import Mock, patch

    payload = control(123, 2, 3, avp(0, integer(11, 2)) + avp(63, integer(456)))
    assert parse_control(payload) == (123, 2, 3, 11, {0: integer(11, 2), 63: integer(456)})
    assert parse_control(control(123, 2, 3, b""))[3] == 20
    for malformed in (payload[:-1], payload + b"\0", bytes(16),
                      control(123, 0, 0, avp(0, integer(2, 2)) + avp(999, b""))):
        try:
            parse_control(malformed)
        except ValueError:
            pass
        else:
            raise AssertionError("Malformed packet accepted")
    mac = bytes.fromhex("020000000001")
    frame = dhcp_discover(mac, 123)
    assert checksum(frame[14:34]) == 0
    assert frame[42:46] == b"\x01\x01\x06\x00"
    assert frame[46:50] == integer(123)
    reply = bytearray(frame)
    reply[26:30] = socket.inet_aton("192.168.179.1")
    reply[34:38] = struct.pack("!HH", 67, 68)
    reply[42] = 2
    reply[58:62] = socket.inet_aton("192.168.179.123")
    reply[284] = 2
    reply[285:292] = b"\x36\x04" + socket.inet_aton("192.168.179.1") + b"\xff"
    network = ipaddress.IPv4Network("192.168.179.0/24")
    assert "Matching DHCP reply" in describe_frame(bytes(reply), network, 123, mac)
    assert "Matching DHCP reply" not in describe_frame(bytes(reply), network, 124, mac)
    assert describe_frame(frame, network, 123, mac) is None
    assert dhcp_reply(bytes(reply), 123, mac, network)[:3] == (2, "192.168.179.123", "192.168.179.1")
    assert dhcp_reply(bytes(reply), 124, mac, network) is None
    request = dhcp_select(mac, 123, "192.168.179.123", "192.168.179.1")
    assert checksum(request[14:34]) == 0 and request[282:285] == b"\x35\x01\x03"
    release = dhcp_select(mac, 123, "192.168.179.123", "192.168.179.1", True, mac)
    assert release[54:58] == socket.inet_aton("192.168.179.123")
    assert release[282:285] == b"\x35\x01\x07"
    assert arp_frame(mac, "192.168.179.123", mac, "192.168.179.124")[20:22] == b"\x00\x01"
    assert arp_frame(mac, "192.168.179.123", mac, "192.168.179.124", True)[20:22] == b"\x00\x02"
    with patch.object(socket, "socket") as socket_factory, patch.object(select, "select") as ready:
        transport = Mock()
        socket_factory.return_value = transport
        ready.return_value = ([transport], [], [])
        probe = Probe("192.168.178.1", "192.168.178.2", 1)
        peer_connection = 789
        peer_session = 987
        responses = [
            control(probe.connection, 0, 1, avp(0, integer(2, 2))
                    + avp(7, b"test-server") + avp(60, integer(1))
                    + avp(61, integer(peer_connection)) + avp(62, integer(5, 2))),
            control(probe.connection, 1, 2, avp(0, integer(20, 2))),
            control(probe.connection, 1, 3, avp(0, integer(11, 2))
                    + avp(63, integer(peer_session)) + avp(64, integer(probe.session))
                    + avp(71, integer(3, 2)) + avp(69, integer(1, 2))),
            control(probe.connection, 2, 4, avp(0, integer(20, 2))),
            control(probe.connection, 2, 5, avp(0, integer(20, 2))),
        ]
        transport.recvfrom.side_effect = [
            (b"\x45" + bytes(19) + response, (probe.server, 0))
            for response in responses
        ]
        probe.establish()
        assert probe.peer_session == peer_session and probe.expected == 2
        probe.close()
        transport.close.assert_called_once()
        sent = [parse_control(call.args[0]) for call in transport.sendto.call_args_list]
        assert [packet[3] for packet in sent] == [1, 20, 3, 10, 20, 12, 4]
        assert sent[3][4][66] == b"fritz.box:guest"
        assert sent[0][0] == 0 and all(packet[0] == peer_connection for packet in sent[1:])
    print("Offline framing, AVP rejection, DHCP matching, handshake and teardown checks passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="192.168.178.1")
    parser.add_argument("--guest-network", default="192.168.179.0/24")
    parser.add_argument("--timeout", type=int, default=12)
    parser.add_argument("--observe", type=int, default=45)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--cross-tunnel", action="store_true",
                        help="Test Ethernet, ARP and IPv4 forwarding with temporary DHCP leases (up to 50s)")
    parser.add_argument("--second-local", help="Already assigned second LAN source IPv4 address for --cross-tunnel")
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
        return 0
    server = str(ipaddress.IPv4Address(arguments.server))
    network = ipaddress.IPv4Network(arguments.guest_network)
    if not 1 <= arguments.timeout <= 60 or not 1 <= arguments.observe <= 60:
        parser.error("timeout and observe must be 1..60s")
    route = json.loads(subprocess.check_output(["ip", "-j", "route", "get", server]))[0]
    local = route["prefsrc"]
    print(f"Route: {local} via {route.get('gateway', 'direct LAN')} dev {route['dev']}", flush=True)
    if ipaddress.IPv4Address(local) not in ipaddress.IPv4Network("192.168.178.0/24"):
        print("Warning: not directly on the FRITZ!Box home subnet; NAT/routing may block protocol 115", flush=True)
    if arguments.cross_tunnel:
        if not arguments.second_local:
            parser.error("--cross-tunnel requires --second-local: the server may replace sessions from the same IP")
        second_local = str(ipaddress.IPv4Address(arguments.second_local))
        if second_local == local or second_local == server:
            parser.error("--second-local must differ from the first endpoint and server")
        return cross_tunnel(server, local, second_local, arguments.timeout, network)
    probe = Probe(server, local, arguments.timeout)
    try:
        probe.establish()
        return probe.observe(network, arguments.observe)
    finally:
        probe.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.CalledProcessError, KeyError) as error:
        print(f"Probe failed: {error}", file=sys.stderr)
        sys.exit(1)