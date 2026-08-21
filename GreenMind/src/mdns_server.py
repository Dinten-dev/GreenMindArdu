import socket
import uasyncio as asyncio
import select
import time

class MicroMDNS:
    def __init__(self, hostname, ip, port=80, service_type="_greenmind._tcp.local"):
        self.hostname = hostname + ".local"
        self.ip = ip
        self.port = port
        self.service_type = service_type
        self.sock = None

    def pack_name(self, name):
        parts = name.split('.')
        res = b""
        for p in parts:
            res += bytes([len(p)]) + p.encode()
        return res + b"\x00"

    def parse_name(self, data, offset):
        name = []
        while True:
            if offset >= len(data): break
            length = data[offset]
            if length == 0:
                offset += 1
                break
            if (length & 0xC0) == 0xC0:
                pointer = ((length & 0x3F) << 8) | data[offset + 1]
                name.append(self.parse_name(data, pointer)[0])
                offset += 2
                break
            offset += 1
            name.append(data[offset:offset + length].decode())
            offset += length
        return ".".join(name), offset

    def handle_query(self, data, addr, sock):
        if len(data) < 12: return
        transaction_id = data[:2]
        flags = data[2:4]
        # Check if query (flags=0)
        if (flags[0] & 0x80) != 0: return

        qdcount = (data[4] << 8) | data[5]
        if qdcount == 0: return

        offset = 12
        qname, offset = self.parse_name(data, offset)
        qtype = (data[offset] << 8) | data[offset+1]

        if qname == self.service_type or qname == self.hostname:
            self.send_response(transaction_id, qname, qtype, sock, addr)

    def send_response(self, transaction_id, qname, qtype, sock, addr):
        # Build A record and PTR/SRV records
        # Header (authoritative response)
        resp = transaction_id + b"\x84\x00\x00\x00\x00\x01\x00\x00\x00\x00"
        
        # Answer (simplified manual construction for _greenmind._tcp.local)
        name_packed = self.pack_name(qname)
        ip_bytes = bytes(map(int, self.ip.split('.')))
        
        if qtype == 12 or qtype == 255: # PTR or ANY for service type
            resp += self.pack_name(self.service_type)
            resp += b"\x00\x0c\x80\x01\x00\x00\x00\x78" # Type PTR, Class IN + Cache flush, TTL 120
            target_name = self.pack_name(self.hostname)
            resp += bytes([len(target_name)]) + target_name
        
        if qtype == 1 or qtype == 255: # A record
            resp += self.pack_name(self.hostname)
            resp += b"\x00\x01\x80\x01\x00\x00\x00\x78\x00\x04" # Type A, Class IN + Cache flush
            resp += ip_bytes

        if len(resp) > 12:
            try:
                sock.sendto(resp, addr)
            except Exception as e:
                print("mDNS send err:", e)

    async def run(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setblocking(False)
        self.sock.bind(('', 5353))
        
        # Join multicast group
        mcast = bytes([224, 0, 0, 251]) + bytes([0, 0, 0, 0])
        self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mcast)

        while True:
            r, _, _ = select.select([self.sock], [], [], 0.0)
            if r:
                data, addr = self.sock.recvfrom(1024)
                self.handle_query(data, addr, self.sock)
            await asyncio.sleep_ms(100)
