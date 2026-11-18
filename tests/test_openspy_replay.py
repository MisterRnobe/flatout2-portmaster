"""Replay must preserve a split request and a fragmented/timeout-ended response."""
import importlib.util
from pathlib import Path
import socket
import tempfile
import unittest
from unittest import mock
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('replay',ROOT/'scripts/replay_openspy.py')
replay=importlib.util.module_from_spec(spec);spec.loader.exec_module(replay)
class ReplayTest(unittest.TestCase):
    def test_split_stream_and_offline_mode(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);session=root/'work/macos/network-trace/123';session.mkdir(parents=True)
            (session/'0000100c-send-002.bin').write_bytes(b'pc\0query')
            (session/'0000100c-send-000.bin').write_bytes(b'flatout2')
            (session/'0000100c-recv-003.bin').write_bytes(b'game-reply')
            with mock.patch.object(replay,'ROOT',root),mock.patch('sys.argv',['replay','--offline']),mock.patch.object(replay.socket,'create_connection') as connect:
                self.assertEqual(replay.main(),0);connect.assert_not_called()
            class Connection:
                parts=[b'one',b'two']
                sent=None
                def __enter__(self):return self
                def __exit__(self,*args):pass
                def sendall(self,data):self.sent=data
                def recv(self,size):
                    if self.parts:return self.parts.pop(0)
                    raise socket.timeout()
            connection=Connection()
            with mock.patch.object(replay,'ROOT',root),mock.patch('sys.argv',['replay']),mock.patch.object(replay.socket,'create_connection',return_value=connection):
                self.assertEqual(replay.main(),0)
            self.assertEqual(connection.sent,b'flatout2pc\0query')
            self.assertEqual((session/'0000100c-replay-response.bin').read_bytes(),b'onetwo')
if __name__=='__main__':unittest.main()
