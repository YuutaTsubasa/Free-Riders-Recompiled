"""Free Riders Recompiled's own online service: lobbies and the network
between the consoles, in place of Xbox LIVE (src/live_client.cpp).

Each game that connects becomes a console on a virtual network: an address
10.77.x.y, a MAC address and a machine id. The title's sockets are virtual
(src/live_imports.cpp); every datagram it sends goes through this server's UDP
port, which hands it to the console that owns the destination address, so two
games on one PC, or behind NAT, reach each other without opening ports.

The game can run the same service itself (src/live_service.cpp, the launcher's
"Host the server"); the two speak the same protocol and must stay alike.

Control is one TCP connection per console: a line per request, answered by an
"OK ..." or "ERR ..." line, any further lines, and a line ".". Binary values
are hex.

  PING                                -> OK freeriders <version> [password]
      (allowed before HELLO; a check that the server is there, as the
      launcher's Test connection does, which does not make a console.
      "password": this server needs PASS before HELLO)
  PASS <password as hex>              -> OK | ERR wrong password
      (before HELLO; the room password's UTF-8 bytes. A wrong one closes the
      connection. A server without a password answers OK.)
  HELLO <xuid> <gamertag>             -> OK <console> <ip> <mac> <machine> <token>
      (ERR password required, when a password is set and PASS did not come
      first; ERR server full)
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
  0x00 console:u32 [token:8 bytes]         a console's UDP address; the token
                                           is HELLO's (games before it send
                                           none, accepted only without a
                                           password); repeated as a keep-alive
  0x01 dst:u32 dport:u16 src:u32 sport:u16 payload
      relayed only from a console's registered UDP address, with its own
      address as src

The room password is sent as plain hex over TCP: it keeps strangers out of a
room, it is not account security.

Usage: python server/live_server.py [--port 47800] [--host 0.0.0.0]
Settings for a public server come from the environment (server/README.md):
SFR_LIVE_PASSWORD, SFR_LIVE_MAX_CONSOLES, SFR_LIVE_MAX_SESSIONS, ...
"""
import argparse
import asyncio
import hmac
import logging
import os
import secrets
import socket
import struct
import sys
import time

log = logging.getLogger('live')

# What PING answers (src/live_connect.h, live_protocol_version).
PROTOCOL_VERSION = 1
DEFAULT_PORT = 47800
# Console numbers fill the low 16 bits of 10.77.x.y.
LAST_CONSOLE_NUMBER = 0xFFFE


class Settings:
    """What a server operator may change. The defaults suit a server on the
    Internet for a group of friends; the environment (SFR_LIVE_*) or the
    command line changes them."""

    def __init__(self, **values):
        self.host = '0.0.0.0'
        self.udp_host = None  # the UDP socket's address, when not host's (Fly.io: fly-global-services)
        self.port = DEFAULT_PORT
        self.password = ''
        self.max_consoles = 64
        self.max_sessions = 32
        self.sessions_per_console = 4
        self.idle_timeout = 900.0    # s without a request or a datagram from a console; 0: never
        self.hello_timeout = 30.0    # s a connection may stay without saying HELLO
        self.max_line = 16384        # bytes in one request line
        self.requests_per_second = 100.0  # per connection, with twice that as a burst
        self.log_level = 'info'
        for name, value in values.items():
            if not hasattr(self, name):
                raise TypeError('unknown setting %s' % name)
            setattr(self, name, value)

    @property
    def max_connections(self):
        # Room for checks (PING) and games saying HELLO next to a full server.
        return self.max_consoles + 16

    @classmethod
    def from_environment(cls, environment=os.environ):
        settings = cls()
        numbers = {
            'SFR_LIVE_PORT': ('port', int),
            'SFR_LIVE_MAX_CONSOLES': ('max_consoles', int),
            'SFR_LIVE_MAX_SESSIONS': ('max_sessions', int),
            'SFR_LIVE_SESSIONS_PER_CONSOLE': ('sessions_per_console', int),
            'SFR_LIVE_IDLE_TIMEOUT': ('idle_timeout', float),
            'SFR_LIVE_HELLO_TIMEOUT': ('hello_timeout', float),
            'SFR_LIVE_MAX_LINE': ('max_line', int),
            'SFR_LIVE_MAX_REQUESTS': ('requests_per_second', float),
        }
        for variable, (name, kind) in numbers.items():
            text = environment.get(variable, '').strip()
            if text:
                try:
                    value = kind(text)
                except ValueError:
                    raise SystemExit('%s=%r is not a number' % (variable, text))
                if value < 0:
                    raise SystemExit('%s must not be negative' % variable)
                setattr(settings, name, value)
        if environment.get('SFR_LIVE_BIND', '').strip():
            settings.host = environment['SFR_LIVE_BIND'].strip()
        if environment.get('SFR_LIVE_UDP_BIND', '').strip():
            settings.udp_host = environment['SFR_LIVE_UDP_BIND'].strip()
        if environment.get('SFR_LIVE_LOG_LEVEL', '').strip():
            settings.log_level = environment['SFR_LIVE_LOG_LEVEL'].strip().lower()
        settings.password = environment.get('SFR_LIVE_PASSWORD', '').strip()  # as the launcher keeps it
        if not settings.password and environment.get('SFR_LIVE_PASSWORD_FILE', '').strip():
            with open(environment['SFR_LIVE_PASSWORD_FILE'].strip(), encoding='utf-8') as file:
                settings.password = file.read().strip()
        return settings


class Console:
    def __init__(self, number, xuid, name, writer, peer):
        self.number = number
        self.ip = (10 << 24) | (77 << 16) | number
        self.ip_bytes = struct.pack('>I', self.ip)
        self.mac = bytes([0x02, 0x46, 0x52, 0x00, number >> 8 & 0xFF, number & 0xFF])
        self.machine = 0xFA00000000000000 | number
        self.xuid = xuid
        self.name = name
        self.writer = writer
        self.peer = peer
        self.udp = None
        # Proves a UDP registration comes from this game: an address alone
        # (the console number) could be guessed by anyone.
        self.token = secrets.token_bytes(8)
        self.token_used = False
        self.last_seen = time.monotonic()


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


class Refused(Exception):
    """A request that ends the connection after its answer."""


def hex_bytes(text, limit):
    data = b'' if text == '-' else bytes.fromhex(text)
    if len(data) > limit:
        raise ValueError('too long')
    return data


def slot_count(text):
    value = int(text)
    if not 0 <= value <= 64:
        raise ValueError('slots')
    return value


class Service:
    def __init__(self, settings=None):
        self.settings = settings or Settings()
        self.password = self.settings.password.encode('utf-8')
        self.consoles = {}  # number -> Console
        self.by_ip = {}
        self.by_udp = {}    # UDP address -> Console
        self.sessions = {}  # id -> Session
        self.next_number = 1
        self.connections = 0
        self.transport = None

    # --- control -------------------------------------------------------
    def new_number(self):
        for _ in range(LAST_CONSOLE_NUMBER):
            number = self.next_number
            self.next_number = number % LAST_CONSOLE_NUMBER + 1
            if number not in self.consoles:
                return number
        raise Refused('server full')

    async def control(self, reader, writer):
        peer = writer.get_extra_info('peername')
        where = '%s:%d' % peer[:2] if peer else '?'
        if self.connections >= self.settings.max_connections:
            log.warning('%s: refused, %d connections already', where, self.connections)
            writer.close()
            return
        self.connections += 1
        keep_alive(writer.get_extra_info('socket'))
        console = None
        signed = not self.password
        rate = self.settings.requests_per_second
        allowance, checked = rate * 2, time.monotonic()
        try:
            while True:
                line = await self.read_line(reader, console)
                if line is None:
                    break
                words = line.decode('utf-8', 'replace').split()
                if not words:
                    continue
                if rate > 0:
                    now = time.monotonic()
                    allowance = min(rate * 2, allowance + (now - checked) * rate)
                    checked = now
                    if allowance < 1:
                        log.warning('%s: too many requests, closed', console.name if console else where)
                        await self.answer(writer, ['ERR too many requests'])
                        break
                    allowance -= 1
                if console:
                    console.last_seen = time.monotonic()
                closing = False
                try:
                    if words[0] == 'PING':
                        answer = ['OK freeriders %d%s' % (PROTOCOL_VERSION, ' password' if self.password else '')]
                    elif words[0] == 'PASS':
                        if self.check_password(words):
                            signed = True
                            answer = ['OK']
                        else:
                            log.warning('%s: wrong password', where)
                            answer, closing = ['ERR wrong password'], True
                    elif words[0] == 'HELLO':
                        if console:
                            answer = ['ERR hello again']
                        elif not signed:
                            answer = ['ERR password required']
                        else:
                            console = self.hello(words, writer, where)
                            answer = ['OK %d %08x %s %016x %s' % (console.number, console.ip, console.mac.hex(),
                                                                  console.machine, console.token.hex())]
                    elif console is None:
                        answer = ['ERR hello first']
                    else:
                        answer = self.request(console, words)
                except Refused as error:
                    answer = ['ERR %s' % error]
                except (IndexError, ValueError, KeyError) as error:
                    answer = ['ERR %s' % (error or type(error).__name__)]
                if answer[0].startswith('ERR'):
                    # Never the password: PASS is logged by its verb alone.
                    shown = 'PASS' if words[0] == 'PASS' else ' '.join(words)[:200]
                    log.info('%s: %s -> %s', console.name if console else where, shown, answer[0])
                await self.answer(writer, answer)
                if closing:
                    break
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            self.connections -= 1
            if console:
                self.goodbye(console)
            writer.close()

    async def read_line(self, reader, console):
        """The next request line, or None when the connection should end:
        closed, a line past the limit, or silent for too long."""
        while True:
            if console is None:
                wait = self.settings.hello_timeout or None
            elif self.settings.idle_timeout:
                # Datagrams count as signs of life: a racing game asks nothing
                # over TCP for minutes.
                wait = self.settings.idle_timeout - (time.monotonic() - console.last_seen)
                if wait <= 0:
                    log.info('console %d %s: nothing for %.0f s, closed', console.number, console.name,
                             self.settings.idle_timeout)
                    return None
            else:
                wait = None
            try:
                line = await asyncio.wait_for(reader.readline(), wait)
            except asyncio.TimeoutError:
                if console is None:
                    return None
                continue  # look at last_seen again
            except (ValueError, asyncio.LimitOverrunError):
                log.warning('%s: a line longer than %d bytes, closed', console.name if console else 'connection',
                            self.settings.max_line)
                return None
            return line or None

    @staticmethod
    async def answer(writer, lines):
        writer.write(('\n'.join(lines) + '\n.\n').encode())
        await writer.drain()

    def check_password(self, words):
        try:
            given = bytes.fromhex(words[1]) if len(words) > 1 else b''
        except ValueError:
            given = b''
        if not self.password:
            return True
        return hmac.compare_digest(given, self.password)

    def hello(self, words, writer, where):
        if len(self.consoles) >= self.settings.max_consoles:
            raise Refused('server full')
        xuid = int(words[1], 16)
        number = self.new_number()
        console = Console(number, xuid, ' '.join(words[2:])[:15] or 'Player', writer, where)
        self.consoles[number] = console
        self.by_ip[console.ip] = console
        log.info('console %d %s xuid %016x at 10.77.%d.%d from %s (%d online)', number, console.name, console.xuid,
                 console.ip >> 8 & 0xFF, console.ip & 0xFF, where, len(self.consoles))
        return console

    def goodbye(self, console):
        log.info('console %d %s left', console.number, console.name)
        self.consoles.pop(console.number, None)
        self.by_ip.pop(console.ip, None)
        if console.udp is not None and self.by_udp.get(console.udp) is console:
            del self.by_udp[console.udp]
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
            if len(self.sessions) >= self.settings.max_sessions:
                raise Refused('too many sessions')
            if sum(1 for s in self.sessions.values() if s.host is console) >= self.settings.sessions_per_console:
                raise Refused('too many sessions')
            session = Session(console, int(words[1], 16), slot_count(words[2]), slot_count(words[3]),
                              hex_bytes(words[4], 4096), hex_bytes(words[5], 1024))
            self.sessions[session.id] = session
            log.info('session %s by %s: flags %x, %d public, %d private', session.id.hex(), console.name,
                     session.flags, session.public, session.private)
            return ['OK %s %s %s' % (session.id.hex(), session.key.hex(), session.nonce.hex())]
        if verb == 'SEARCH':
            limit = min(int(words[1]), 50)
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
                xuid = int(xuid, 16)
                if xuid not in session.members and len(session.members) >= 64:
                    raise Refused('session full')
                session.members[xuid] = private == '1'
            log.info('session %s: %d members', session.id.hex(), len(session.members))
            return ['OK']
        if verb == 'LEAVE':
            session = self.session(words[1])
            for word in words[2:]:
                session.members.pop(int(word, 16), None)
            return ['OK']
        if verb == 'MODIFY':
            session = self.session(words[1])
            session.flags, session.public, session.private = int(words[2], 16), slot_count(words[3]), slot_count(words[4])
            return ['OK']
        if verb == 'STATE':
            session = self.session(words[1])
            session.state = words[2][:16]
            log.info('session %s: %s', session.id.hex(), session.state)
            return ['OK']
        if verb == 'DELETE':
            session = self.sessions.pop(bytes.fromhex(words[1]), None)
            if session:
                log.info('session %s deleted', session.id.hex())
            return ['OK']
        if verb == 'QOSSET':
            self.session(words[1]).qos = hex_bytes(words[2], 1024) if len(words) > 2 else b''
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
            if not console:
                return
            if len(data) >= 13:
                if not hmac.compare_digest(data[5:13], console.token):
                    log.debug('udp: a wrong token for console %d from %s:%d', console.number, *address[:2])
                    return
                console.token_used = True
            elif self.password or console.token_used:
                # A game from before the token: only where no password is
                # asked, and never in place of a game that has one.
                return
            console.last_seen = time.monotonic()
            if console.udp != address:
                if console.udp is not None and self.by_udp.get(console.udp) is console:
                    del self.by_udp[console.udp]
                console.udp = address
                self.by_udp[address] = console
                log.info('console %d datagrams from %s:%d', console.number, *address[:2])
            return
        if data[:1] == b'\x01' and len(data) >= 13:
            sender = self.by_udp.get(address)
            # Only a console's own datagrams, from its own address.
            if sender is None or data[7:11] != sender.ip_bytes:
                return
            sender.last_seen = time.monotonic()
            destination = self.by_ip.get(struct.unpack_from('>I', data, 1)[0])
            if destination and destination.udp:
                self.transport.sendto(data, destination.udp)

    def error_received(self, error):
        log.debug('udp: %s', error)

    def connection_lost(self, error):
        pass


def keep_alive(sock):
    """TCP keep-alive, so a game that vanished (a phone that lost its network)
    is noticed in minutes, not hours."""
    if sock is None:
        return
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        for name, value in (('TCP_KEEPIDLE', 60), ('TCP_KEEPINTVL', 15), ('TCP_KEEPCNT', 4)):
            if hasattr(socket, name):
                sock.setsockopt(socket.IPPROTO_TCP, getattr(socket, name), value)
    except OSError:
        pass


def check(address, timeout=3.0):
    """0 when a Free Riders server answers PING at host[:port] (the image's
    health check), 1 otherwise."""
    host, _, port = address.rpartition(':') if ':' in address else (address, '', str(DEFAULT_PORT))
    try:
        with socket.create_connection((host, int(port)), timeout=timeout) as connection:
            connection.sendall(b'PING\n')
            answer = b''
            while b'\n' not in answer:
                got = connection.recv(256)
                if not got:
                    break
                answer += got
    except OSError as error:
        print('no answer: %s' % error)
        return 1
    first = answer.split(b'\n', 1)[0].decode('utf-8', 'replace')
    print(first)
    return 0 if first.startswith('OK freeriders ') else 1


def check_relay(address, password='', timeout=3.0):
    """0 when a datagram goes from one test console to another through the
    server at host[:port], over UDP (the race's path, which a firewall may
    block while TCP works); 1 otherwise. Joins as two consoles named "Check"
    for a moment."""
    host, _, port = address.rpartition(':') if ':' in address else (address, '', str(DEFAULT_PORT))
    port = int(port)
    try:
        target = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_DGRAM)[0][4]
        consoles = []
        for xuid in (0x0009FFFFFFFF0001, 0x0009FFFFFFFF0002):
            control = socket.create_connection((host, port), timeout=timeout)
            reader = control.makefile('rb')

            def ask(line, control=control, reader=reader):
                control.sendall((line + '\n').encode())
                lines = []
                while (got := reader.readline().decode('utf-8', 'replace').rstrip('\r\n')) != '.':
                    if not got and not lines:
                        raise OSError('connection closed')
                    lines.append(got)
                return lines

            if password:
                answer = ask('PASS ' + password.encode('utf-8').hex())
                if answer[:1] == ['ERR wrong password']:
                    print('wrong password')
                    return 1
            words = ask('HELLO %016x Check' % xuid)[0].split()
            if words[0] != 'OK' or len(words) < 6:
                print('HELLO: %s' % ' '.join(words))
                return 1
            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.settimeout(timeout)
            udp.connect(target)
            consoles.append((control, udp, int(words[1]), int(words[2], 16), bytes.fromhex(words[5])))
        for _ in range(3):
            for _, udp, number, _, token in consoles:
                udp.send(b'\x00' + struct.pack('>I', number) + token)
            time.sleep(0.2)
            (_, sender, _, source, _), (_, receiver, _, destination, _) = consoles
            sent = b'\x01' + struct.pack('>IHIH', destination, 1000, source, 1000) + b'free riders'
            sender.send(sent)
            try:
                if receiver.recv(2048) == sent:
                    print('OK: a datagram went through the server over UDP')
                    return 0
            except socket.timeout:
                pass
        print('no datagram came back over UDP: is UDP port %d open?' % port)
        return 1
    except (OSError, IndexError, ValueError) as error:
        print('no answer: %s' % error)
        return 1


async def serve(settings):
    service = Service(settings)
    loop = asyncio.get_running_loop()
    udp_host = settings.udp_host or settings.host
    await loop.create_datagram_endpoint(lambda: service, local_addr=(udp_host, settings.port))
    server = await asyncio.start_server(service.control, settings.host, settings.port, limit=settings.max_line)
    log.info('listening on %s:%d TCP, %s:%d UDP; password %s; up to %d consoles, %d sessions',
             settings.host, settings.port, udp_host, settings.port, 'on' if settings.password else 'off',
             settings.max_consoles, settings.max_sessions)
    async with server:
        await server.serve_forever()


def main(arguments=None):
    settings = Settings.from_environment()
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--host', default=settings.host, help='TCP (and UDP) address to listen on (SFR_LIVE_BIND)')
    parser.add_argument('--udp-host', default=settings.udp_host,
                        help='UDP address, when not --host (SFR_LIVE_UDP_BIND; fly-global-services on Fly.io)')
    parser.add_argument('--port', type=int, default=settings.port, help='TCP and UDP port (SFR_LIVE_PORT)')
    parser.add_argument('--check', metavar='HOST[:PORT]', help='ask a server PING and exit 0 when it answers')
    parser.add_argument('--check-relay', metavar='HOST[:PORT]',
                        help='send a datagram between two test consoles through a server (UDP); '
                             'the password comes from SFR_LIVE_PASSWORD')
    args = parser.parse_args(arguments)
    if args.check:
        return check(args.check)
    if args.check_relay:
        return check_relay(args.check_relay, settings.password)
    settings.host, settings.udp_host, settings.port = args.host, args.udp_host, args.port
    level = getattr(logging, settings.log_level.upper(), None)
    if not isinstance(level, int):
        raise SystemExit('SFR_LIVE_LOG_LEVEL=%s: use debug, info, warning or error' % settings.log_level)
    logging.basicConfig(level=level, format='%(asctime)s %(message)s')
    try:
        asyncio.run(serve(settings))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
