"""Free Riders Recompiled's own online service: lobbies and the network
between the consoles, in place of Xbox LIVE (src/live_client.cpp).

Each game that connects becomes a console on a virtual network: an address
10.77.x.y, a MAC address and a machine id. The title's sockets are virtual
(src/live_imports.cpp); every datagram it sends goes through this server's UDP
port, which hands it to the console that owns the destination address, so two
games on one PC, or behind NAT, reach each other without opening ports.

Control is one TCP connection per console: a line per request, answered by an
"OK ..." or "ERR ..." line, any further lines, and a line ".". Binary values
are hex.

  HELLO <xuid> <gamertag>             -> OK <console> <ip> <mac> <machine>
  CREATE <flags> <public> <private> <props> <contexts>
                                      -> OK <session> <key> <nonce>
  SEARCH <max>                        -> OK <n>, then n lines
      <session> <key> <host ip> <host mac> <host machine> <flags>
      <public> <private> <filled public> <filled private> <props> <contexts>
  SESSION <session>                   -> OK, then that line, then members
  JOIN <session> <xuid> [private]...  -> OK
  LEAVE <session> <xuid>...           -> OK
  MODIFY <session> <flags> <public> <private> -> OK
  STATE <session> <lobby|ingame|ended> -> OK
  DELETE <session>                    -> OK
  QOSSET <session> <data>             -> OK
  QOSGET <session>                    -> OK <data>
  PEER <ip>                           -> OK <mac> <machine>

UDP, both ways (big-endian):
  0x00 console:u32                         a console's UDP address
  0x01 dst:u32 dport:u16 src:u32 sport:u16 payload

Usage: python scripts/live_server.py [--port 47800] [--host 0.0.0.0]
"""
import argparse
import asyncio
import logging
import secrets
import struct

log = logging.getLogger('live')


class Console:
    def __init__(self, number, xuid, name, writer):
        self.number = number
        self.ip = (10 << 24) | (77 << 16) | number
        self.mac = bytes([0x02, 0x46, 0x52, 0x00, number >> 8 & 0xFF, number & 0xFF])
        self.machine = 0xFA00000000000000 | number
        self.xuid = xuid
        self.name = name
        self.writer = writer
        self.udp = None


class Session:
    def __init__(self, host, flags, public, private, props, contexts):
        self.id = secrets.token_bytes(8)
        self.key = secrets.token_bytes(16)
        self.nonce = secrets.token_bytes(8)
        self.host = host
        self.flags = flags
        self.public = public
        self.private = private
        self.props = props
        self.contexts = contexts
        self.members = {}  # xuid -> private slot
        self.state = 'lobby'
        self.qos = b''

    def filled(self, private):
        return sum(1 for p in self.members.values() if p == private)

    def line(self):
        host = self.host
        return ' '.join([self.id.hex(), self.key.hex(), '%08x' % host.ip, host.mac.hex(), '%016x' % host.machine,
                         '%x' % self.flags, str(self.public), str(self.private), str(self.filled(False)),
                         str(self.filled(True)), self.props.hex() or '-', self.contexts.hex() or '-'])


class Service:
    def __init__(self):
        self.consoles = {}  # number -> Console
        self.by_ip = {}
        self.sessions = {}  # id -> Session
        self.next_number = 1
        self.transport = None

    # --- control -------------------------------------------------------
    async def control(self, reader, writer):
        console = None
        peer = writer.get_extra_info('peername')
        try:
            while line := await reader.readline():
                words = line.decode('utf-8', 'replace').split()
                if not words:
                    continue
                try:
                    if words[0] == 'HELLO':
                        console = self.hello(words, writer)
                        answer = ['OK %d %08x %s %016x' % (console.number, console.ip, console.mac.hex(), console.machine)]
                    elif console is None:
                        answer = ['ERR hello first']
                    else:
                        answer = self.request(console, words)
                except (IndexError, ValueError, KeyError) as error:
                    answer = ['ERR %s' % (error or type(error).__name__)]
                if answer[0].startswith('ERR'):
                    log.info('%s: %s -> %s', console.name if console else peer, ' '.join(words), answer[0])
                writer.write(('\n'.join(answer) + '\n.\n').encode())
                await writer.drain()
        except ConnectionError:
            pass
        finally:
            if console:
                self.goodbye(console)
            writer.close()

    def hello(self, words, writer):
        number = self.next_number
        self.next_number += 1
        console = Console(number, int(words[1], 16), ' '.join(words[2:]) or 'Player', writer)
        self.consoles[number] = console
        self.by_ip[console.ip] = console
        log.info('console %d %s xuid %016x at 10.77.%d.%d', number, console.name, console.xuid,
                 console.ip >> 8 & 0xFF, console.ip & 0xFF)
        return console

    def goodbye(self, console):
        log.info('console %d %s left', console.number, console.name)
        self.consoles.pop(console.number, None)
        self.by_ip.pop(console.ip, None)
        for sid in [sid for sid, s in self.sessions.items() if s.host is console]:
            log.info('session %s closed with its host', sid.hex())
            del self.sessions[sid]
        for session in self.sessions.values():
            session.members.pop(console.xuid, None)

    def session(self, text):
        return self.sessions[bytes.fromhex(text)]

    def request(self, console, words):
        verb = words[0]
        if verb == 'CREATE':
            props = b'' if words[4] == '-' else bytes.fromhex(words[4])
            contexts = b'' if words[5] == '-' else bytes.fromhex(words[5])
            session = Session(console, int(words[1], 16), int(words[2]), int(words[3]), props, contexts)
            self.sessions[session.id] = session
            log.info('session %s by %s: flags %x, %d public, %d private', session.id.hex(), console.name,
                     session.flags, session.public, session.private)
            return ['OK %s %s %s' % (session.id.hex(), session.key.hex(), session.nonce.hex())]
        if verb == 'SEARCH':
            limit = int(words[1])
            found = [s for s in self.sessions.values()
                     if s.host is not console and s.state == 'lobby' and s.filled(False) < s.public][:limit]
            return ['OK %d' % len(found)] + [s.line() for s in found]
        if verb == 'SESSION':
            session = self.session(words[1])
            members = ['%016x %d' % (xuid, private) for xuid, private in session.members.items()]
            return ['OK %d' % len(members), session.line()] + members
        if verb == 'JOIN':
            session = self.session(words[1])
            for word in words[2:]:
                xuid, _, private = word.partition(':')
                session.members[int(xuid, 16)] = private == '1'
            log.info('session %s: %d members', session.id.hex(), len(session.members))
            return ['OK']
        if verb == 'LEAVE':
            session = self.session(words[1])
            for word in words[2:]:
                session.members.pop(int(word, 16), None)
            return ['OK']
        if verb == 'MODIFY':
            session = self.session(words[1])
            session.flags, session.public, session.private = int(words[2], 16), int(words[3]), int(words[4])
            return ['OK']
        if verb == 'STATE':
            session = self.session(words[1])
            session.state = words[2]
            log.info('session %s: %s', session.id.hex(), session.state)
            return ['OK']
        if verb == 'DELETE':
            session = self.sessions.pop(bytes.fromhex(words[1]), None)
            if session:
                log.info('session %s deleted', session.id.hex())
            return ['OK']
        if verb == 'QOSSET':
            self.session(words[1]).qos = bytes.fromhex(words[2]) if len(words) > 2 and words[2] != '-' else b''
            return ['OK']
        if verb == 'QOSGET':
            return ['OK %s' % (self.session(words[1]).qos.hex() or '-')]
        if verb == 'PEER':
            peer = self.by_ip[int(words[1], 16)]
            return ['OK %s %016x' % (peer.mac.hex(), peer.machine)]
        return ['ERR unknown request']

    # --- datagrams -----------------------------------------------------
    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, address):
        if data[:1] == b'\x00' and len(data) >= 5:
            console = self.consoles.get(struct.unpack_from('>I', data, 1)[0])
            if console and console.udp != address:
                console.udp = address
                log.info('console %d datagrams from %s:%d', console.number, *address[:2])
            return
        if data[:1] == b'\x01' and len(data) >= 13:
            destination = self.by_ip.get(struct.unpack_from('>I', data, 1)[0])
            if destination and destination.udp:
                self.transport.sendto(data, destination.udp)

    def error_received(self, error):
        log.debug('udp: %s', error)

    def connection_lost(self, error):
        pass


async def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=47800)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')
    service = Service()
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(lambda: service, local_addr=(args.host, args.port))
    server = await asyncio.start_server(service.control, args.host, args.port)
    log.info('listening on %s:%d (TCP and UDP)', args.host, args.port)
    async with server:
        await server.serve_forever()


if __name__ == '__main__':
    asyncio.run(main())
