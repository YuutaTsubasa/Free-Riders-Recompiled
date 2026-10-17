import asyncio
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import live_server


class LiveServerTest(unittest.TestCase):
    def exchange(self, lines):
        """Each request's answer lines, from one connection to a server on the loopback."""
        async def run():
            service = live_server.Service()
            server = await asyncio.start_server(service.control, '127.0.0.1', 0)
            port = server.sockets[0].getsockname()[1]
            reader, writer = await asyncio.open_connection('127.0.0.1', port)
            answers = []
            for line in lines:
                writer.write((line + '\n').encode())
                await writer.drain()
                answer = []
                while (got := (await reader.readline()).decode().rstrip('\n')) != '.':
                    answer.append(got)
                answers.append(answer)
            writer.close()
            server.close()
            await server.wait_closed()
            return answers, service
        return asyncio.run(run())

    def test_ping_before_hello_makes_no_console(self):
        answers, service = self.exchange(['PING', 'SEARCH 4'])
        self.assertEqual(answers[0], ['OK freeriders %d' % live_server.PROTOCOL_VERSION])
        self.assertEqual(answers[1], ['ERR hello first'])
        self.assertEqual(service.consoles, {})

    def test_hello_after_ping_joins_as_before(self):
        answers, _ = self.exchange(['PING', 'HELLO 0009000000001234 Rider', 'SEARCH 4'])
        self.assertEqual(answers[1], ['OK 1 0a4d0001 024652000001 fa00000000000001'])
        self.assertEqual(answers[2], ['OK 0'])


if __name__ == '__main__':
    unittest.main()
