import asyncio
import io
import logging
import struct
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server'))
import live_server


class Running:
    """A server on the loopback (TCP and UDP on one free port) for one test."""

    def __init__(self, **settings):
        self.settings = live_server.Settings(host='127.0.0.1', port=0, **settings)

    async def __aenter__(self):
        self.service = live_server.Service(self.settings)
        loop = asyncio.get_running_loop()
        # A free UDP port first, then TCP on the same number (Windows keeps
        # some ranges for itself, which differ between the two).
        for _ in range(20):
            self.udp, _ = await loop.create_datagram_endpoint(lambda: self.service, local_addr=('127.0.0.1', 0))
            self.port = self.udp.get_extra_info('sockname')[1]
            try:
                self.server = await asyncio.start_server(self.service.control, '127.0.0.1', self.port,
                                                         limit=self.settings.max_line)
                return self
            except OSError:
                self.udp.close()
        raise OSError('no port free for both TCP and UDP')

    async def __aexit__(self, *error):
        self.udp.close()
        self.server.close()
        await self.server.wait_closed()

    async def connect(self):
        return Client(*await asyncio.open_connection('127.0.0.1', self.port))


class Client:
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer

    async def ask(self, line):
        """The answer's lines, or None when the server closed the connection."""
        self.writer.write((line + '\n').encode())
        await self.writer.drain()
        answer = []
        while True:
            got = await asyncio.wait_for(self.reader.readline(), 5)
            if not got:
                return answer or None
            got = got.decode().rstrip('\n')
            if got == '.':
                return answer
            answer.append(got)

    async def closed(self):
        return await asyncio.wait_for(self.reader.read(), 5) == b''

    def close(self):
        self.writer.close()


class Datagrams(asyncio.DatagramProtocol):
    def __init__(self):
        self.received = asyncio.Queue()

    def datagram_received(self, data, address):
        self.received.put_nowait(data)


async def udp_socket(port):
    loop = asyncio.get_running_loop()
    transport, protocol = await loop.create_datagram_endpoint(Datagrams, remote_addr=('127.0.0.1', port))
    return transport, protocol


def relayed(to_ip, from_ip, payload):
    return b'\x01' + struct.pack('>IHIH', to_ip, 1000, from_ip, 1000) + payload


class LiveServerTest(unittest.TestCase):
    def setUp(self):
        live_server.log.setLevel(logging.ERROR)  # the refusals the tests cause are expected

    def tearDown(self):
        live_server.log.setLevel(logging.NOTSET)

    def run_async(self, test):
        return asyncio.run(asyncio.wait_for(test(), 20))

    def test_ping_before_hello_makes_no_console(self):
        async def test():
            async with Running() as server:
                client = await server.connect()
                self.assertEqual(await client.ask('PING'), ['OK freeriders %d' % live_server.PROTOCOL_VERSION])
                self.assertEqual(await client.ask('SEARCH 4'), ['ERR hello first'])
                self.assertEqual(server.service.consoles, {})
                client.close()
        self.run_async(test)

    def test_hello_after_ping_joins_as_before(self):
        async def test():
            async with Running() as server:
                client = await server.connect()
                await client.ask('PING')
                hello = (await client.ask('HELLO 0009000000001234 Rider'))[0].split()
                # The old fields first, so games from before the token read it.
                self.assertEqual(hello[:5], ['OK', '1', '0a4d0001', '024652000001', 'fa00000000000001'])
                self.assertEqual(len(hello[5]), 16)
                self.assertEqual(await client.ask('SEARCH 4'), ['OK 0'])
                client.close()
        self.run_async(test)

    def test_password_is_asked_for_and_checked(self):
        async def test():
            async with Running(password='ride ön') as server:
                client = await server.connect()
                self.assertEqual(await client.ask('PING'), ['OK freeriders %d password' % live_server.PROTOCOL_VERSION])
                # A game from before passwords says HELLO at once.
                self.assertEqual(await client.ask('HELLO 0009000000000001 Old'), ['ERR password required'])
                self.assertEqual(server.service.consoles, {})
                self.assertEqual(await client.ask('PASS ' + 'ride ön'.encode().hex()), ['OK'])
                hello = await client.ask('HELLO 0009000000000001 New')
                self.assertTrue(hello[0].startswith('OK 1 '))
                client.close()

                wrong = await server.connect()
                self.assertEqual(await wrong.ask('PASS ' + b'ride on'.hex()), ['ERR wrong password'])
                self.assertTrue(await wrong.closed(), 'a wrong password ends the connection')
                malformed = await server.connect()
                self.assertEqual(await malformed.ask('PASS zz'), ['ERR wrong password'])
                empty = await server.connect()
                self.assertEqual(await empty.ask('PASS'), ['ERR wrong password'])
                for each in (wrong, malformed, empty):
                    each.close()
        self.run_async(test)

    def test_the_password_never_reaches_the_log(self):
        async def test():
            async with Running(password='hunter2') as server:
                client = await server.connect()
                await client.ask('PASS ' + b'wrong'.hex())
                client.close()
                client = await server.connect()
                await client.ask('PASS ' + b'hunter2'.hex())
                await client.ask('HELLO 0009000000000001 Rider')
                await client.ask('PASS ' + b'hunter2'.hex() + ' x')
                client.close()
        stream = io.StringIO()
        handler = logging.StreamHandler(stream)
        live_server.log.addHandler(handler)
        live_server.log.setLevel(logging.DEBUG)
        try:
            self.run_async(test)
        finally:
            live_server.log.removeHandler(handler)
        text = stream.getvalue()
        self.assertIn('wrong password', text)
        for secret in ('hunter2', b'hunter2'.hex(), 'wrong', b'wrong'.hex()):
            self.assertNotIn(secret, text.replace('wrong password', ''))

    def test_a_server_without_password_takes_pass(self):
        async def test():
            async with Running() as server:
                client = await server.connect()
                self.assertEqual(await client.ask('PASS ' + b'anything'.hex()), ['OK'])
                self.assertTrue((await client.ask('HELLO 0009000000000001 Rider'))[0].startswith('OK 1 '))
                client.close()
        self.run_async(test)

    def test_limits(self):
        async def test():
            async with Running(max_consoles=1, max_sessions=1, max_line=256) as server:
                first = await server.connect()
                self.assertTrue((await first.ask('HELLO 0009000000000001 One'))[0].startswith('OK'))
                second = await server.connect()
                self.assertEqual(await second.ask('HELLO 0009000000000002 Two'), ['ERR server full'])
                self.assertTrue((await first.ask('CREATE 42f 7 1 - -'))[0].startswith('OK'))
                self.assertEqual(await first.ask('CREATE 42f 7 1 - -'), ['ERR too many sessions'])
                self.assertTrue((await first.ask('CREATE 42f 999 1 - -'))[0].startswith('ERR'))
                long = await server.connect()
                self.assertIsNone(await long.ask('PING ' + 'a' * 1000), 'a line past the limit closes')
                long.close()
                first.close()
                second.close()
        self.run_async(test)

    def test_request_rate(self):
        async def test():
            async with Running(requests_per_second=5) as server:
                client = await server.connect()
                answers = [await client.ask('PING') for _ in range(12)]
                self.assertIn(['ERR too many requests'], answers)
                self.assertTrue(await client.closed())
                client.close()
        self.run_async(test)

    def test_silent_connections_are_closed(self):
        async def test():
            async with Running(hello_timeout=0.3, idle_timeout=0.5) as server:
                silent = await server.connect()
                self.assertTrue(await silent.closed(), 'no HELLO in time')
                silent.close()
                console = await server.connect()
                await console.ask('HELLO 0009000000000001 Rider')
                self.assertEqual(len(server.service.consoles), 1)
                self.assertTrue(await console.closed(), 'nothing from the console in time')
                console.close()
                await asyncio.sleep(0.05)
                self.assertEqual(server.service.consoles, {})
        self.run_async(test)

    def test_datagrams_relay_between_registered_consoles(self):
        async def test():
            async with Running() as server:
                consoles = []
                for xuid in (1, 2):
                    client = await server.connect()
                    words = (await client.ask('HELLO %016x Rider%d' % (xuid, xuid)))[0].split()
                    transport, protocol = await udp_socket(server.port)
                    transport.sendto(b'\x00' + struct.pack('>I', int(words[1])) + bytes.fromhex(words[5]))
                    consoles.append((client, int(words[2], 16), transport, protocol))
                await asyncio.sleep(0.1)
                (_, ip1, udp1, _), (_, ip2, _, got2) = consoles
                udp1.sendto(relayed(ip2, ip1, b'hello'))
                self.assertEqual(await asyncio.wait_for(got2.received.get(), 2), relayed(ip2, ip1, b'hello'))
                # Not with another console's address as the sender.
                udp1.sendto(relayed(ip2, ip2, b'spoofed'))
                # Nor from an address that never registered.
                stranger, _ = await udp_socket(server.port)
                stranger.sendto(relayed(ip2, ip1, b'stranger'))
                # A registration with a wrong token moves nothing.
                stranger.sendto(b'\x00' + struct.pack('>I', 2) + bytes(8))
                stranger.sendto(b'\x00' + struct.pack('>I', 2))  # nor one without (console 2 has a token)
                await asyncio.sleep(0.2)
                self.assertTrue(got2.received.empty())
                udp1.sendto(relayed(ip2, ip1, b'again'))
                self.assertEqual(await asyncio.wait_for(got2.received.get(), 2), relayed(ip2, ip1, b'again'))
                for client, _, transport, _ in consoles:
                    client.close()
                    transport.close()
                stranger.close()
        self.run_async(test)

    def test_games_from_before_the_token_still_register(self):
        async def test():
            async with Running() as server:
                old = await server.connect()
                words = (await old.ask('HELLO 0009000000000001 Old'))[0].split()
                transport, _ = await udp_socket(server.port)
                transport.sendto(b'\x00' + struct.pack('>I', int(words[1])))
                await asyncio.sleep(0.1)
                self.assertIsNotNone(server.service.consoles[int(words[1])].udp)
                transport.close()
                old.close()
            async with Running(password='x') as server:
                client = await server.connect()
                await client.ask('PASS ' + b'x'.hex())
                words = (await client.ask('HELLO 0009000000000001 New'))[0].split()
                transport, _ = await udp_socket(server.port)
                transport.sendto(b'\x00' + struct.pack('>I', int(words[1])))
                await asyncio.sleep(0.1)
                self.assertIsNone(server.service.consoles[int(words[1])].udp, 'a password server wants the token')
                transport.close()
                client.close()
        self.run_async(test)

    def test_console_numbers_wrap_and_skip_ones_in_use(self):
        service = live_server.Service()
        service.consoles = {live_server.LAST_CONSOLE_NUMBER: object(), 1: object()}
        service.next_number = live_server.LAST_CONSOLE_NUMBER
        self.assertEqual(service.new_number(), 2)

    def test_settings_from_the_environment(self):
        settings = live_server.Settings.from_environment({
            'SFR_LIVE_PASSWORD': 'room', 'SFR_LIVE_MAX_CONSOLES': '8', 'SFR_LIVE_IDLE_TIMEOUT': '0',
            'SFR_LIVE_UDP_BIND': 'fly-global-services', 'SFR_LIVE_LOG_LEVEL': 'WARNING'})
        self.assertEqual((settings.password, settings.max_consoles, settings.idle_timeout, settings.udp_host,
                          settings.log_level), ('room', 8, 0.0, 'fly-global-services', 'warning'))
        with self.assertRaises(SystemExit):
            live_server.Settings.from_environment({'SFR_LIVE_MAX_SESSIONS': 'many'})

    def test_old_path_and_check_mode(self):
        """scripts/live_server.py still runs the server, and --check is the
        image's health check."""
        async def test():
            async with Running() as server:
                for script in (ROOT / 'server' / 'live_server.py', ROOT / 'scripts' / 'live_server.py'):
                    process = await asyncio.create_subprocess_exec(
                        sys.executable, str(script), '--check', '127.0.0.1:%d' % server.port,
                        stdout=subprocess.PIPE)
                    out, _ = await process.communicate()
                    self.assertEqual(process.returncode, 0, script)
                    self.assertTrue(out.startswith(b'OK freeriders'))
            process = await asyncio.create_subprocess_exec(
                sys.executable, str(ROOT / 'server' / 'live_server.py'), '--check', '127.0.0.1:%d' % server.port,
                stdout=subprocess.PIPE)
            await process.communicate()
            self.assertEqual(process.returncode, 1, 'nothing there any more')
        self.run_async(test)


if __name__ == '__main__':
    unittest.main()
