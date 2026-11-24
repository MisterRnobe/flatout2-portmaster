import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('summary', ROOT/'scripts/summarize_network_trace.py')
summary = importlib.util.module_from_spec(spec); spec.loader.exec_module(summary)


class SummaryTest(unittest.TestCase):
    def test_split_stream_and_separate_qr(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp); (p/'qr2').mkdir()
            (p/'0000100c-send-001.bin').write_bytes(b'pc\0request')
            (p/'0000100c-send-000.bin').write_bytes(b'flatout2')
            (p/'0000100c-recv-000.bin').write_bytes(b'encrypted')
            (p/'qr2/00001060-tx-000.bin').write_bytes(b'\x03abcdgamename\0flatout2pc\0')
            (p/'qr2/00001060-rx-000.bin').write_bytes(b'\xfe\xfd\x01abcdchallenge')
            d = summary.summarize(p)
            self.assertEqual(len(d['master_streams']), 1)
            self.assertEqual(d['master_streams'][0]['game_identifiers'], ['flatout2pc'])
            self.assertEqual(d['master_streams'][0]['received_bytes'], 9)
            self.assertTrue(d['qr_master_reply_captured'])
            self.assertFalse(d['public_registration_verified'])
            self.assertEqual(d['qr_master_packets'][0]['type_hex'], '01')

    def test_decoded_empty_list_requires_schema_and_terminal(self):
        header = bytes(6)+bytes([2])+b'\0hostname\0\0numplayers\0'
        empty = summary.inspect_decoded_list(header+b'\0\0\xff\xff\xff\xff')
        self.assertTrue(empty['canonical_empty_list'])
        self.assertEqual(empty['fields'], ['hostname', 'numplayers'])
        self.assertFalse(summary.inspect_decoded_list(header+b'\0entry\0\xff\xff\xff\xff')['canonical_empty_list'])
        self.assertFalse(summary.inspect_decoded_list(header+b'\0\0\xff')['canonical_empty_list'])
        with self.assertRaises(ValueError):
            summary.inspect_decoded_list(b'short')

    def test_empty_and_orphan_reply(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            self.assertEqual(summary.summarize(p)['master_streams'], [])
            (p/'00001004-recv-000.bin').write_bytes(b'data')
            self.assertEqual(summary.summarize(p)['master_streams'][0]['status'], 'response-captured')


if __name__ == '__main__':
    unittest.main()
