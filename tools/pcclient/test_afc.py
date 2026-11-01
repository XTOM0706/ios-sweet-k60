"""afc.py 的单元测试。"""

from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import afc


class TestAfcHeader(unittest.TestCase):
    def test_header_layout(self):
        hdr = afc.AfcHeader(entire_length=48, this_length=48, packet_num=1,
                            operation=afc.AfcOp.FILE_OPEN)
        blob = hdr.pack()
        self.assertEqual(len(blob), 40)
        self.assertEqual(blob[:8], b"CFA6LPAA")
        self.assertEqual(struct.unpack_from("<Q", blob, 8)[0], 48)
        self.assertEqual(struct.unpack_from("<Q", blob, 32)[0], 0x0D)
        self.assertEqual(afc.AfcHeader.unpack(blob), hdr)

    def test_bad_magic(self):
        with self.assertRaises(afc.AfcProtocolError):
            afc.AfcHeader.unpack(b"XXXXXXXX" + b"\x00" * 32)

    def test_short_header(self):
        with self.assertRaises(afc.AfcProtocolError):
            afc.AfcHeader.unpack(b"CFA6LPAA" + b"\x00" * 31)


class TestAfcRequests(unittest.TestCase):
    def test_file_open(self):
        pkt = afc.file_open(1, "/a", afc.AfcFileMode.WRONLY)
        blob = pkt.to_bytes()
        self.assertEqual(blob[:8], b"CFA6LPAA")
        entire, this, num, op = struct.unpack_from("<QQQQ", blob, 8)
        self.assertEqual(op, 0x0D)
        self.assertEqual(num, 1)
        self.assertEqual(entire, 40 + 8 + 3)   # mode(8) + "/a\0"(3)
        self.assertEqual(this, entire)         # 没有尾随 bulk
        self.assertEqual(blob[40:48], struct.pack("<Q", 0x03))
        self.assertEqual(blob[48:], b"/a\x00")

    def test_file_write_splits_data_and_payload(self):
        pkt = afc.file_write(2, 0x1122334455667788, b"hello")
        blob = pkt.to_bytes()
        entire, this, num, op = struct.unpack_from("<QQQQ", blob, 8)
        self.assertEqual(op, 0x10)
        self.assertEqual(num, 2)
        self.assertEqual(this, 48)             # 40 + 8(handle)
        self.assertEqual(entire, 53)           # 48 + 5
        self.assertEqual(blob[40:48], bytes.fromhex("8877665544332211"))
        self.assertEqual(blob[48:], b"hello")

    def test_file_write_roundtrip(self):
        original = afc.file_write(7, 0xDEADBEEF, b"\x00\x01\x02")
        parsed, used = afc.AfcPacket.parse(original.to_bytes())
        self.assertEqual(used, len(original.to_bytes()))
        self.assertEqual(parsed.operation, afc.AfcOp.FILE_WRITE)
        self.assertEqual(parsed.packet_num, 7)
        self.assertEqual(parsed.data, struct.pack("<Q", 0xDEADBEEF))
        self.assertEqual(parsed.payload, b"\x00\x01\x02")

    def test_file_close(self):
        parsed, _ = afc.AfcPacket.parse(afc.file_close(3, 42).to_bytes())
        self.assertEqual(parsed.operation, afc.AfcOp.FILE_CLOSE)
        self.assertEqual(parsed.data, struct.pack("<Q", 42))

    def test_make_dir_and_rename(self):
        parsed, _ = afc.AfcPacket.parse(afc.make_dir(4, "/PublicStaging").to_bytes())
        self.assertEqual(parsed.operation, afc.AfcOp.MAKE_DIR)
        self.assertEqual(parsed.data, b"/PublicStaging\x00")

        parsed, _ = afc.AfcPacket.parse(afc.rename_path(5, "/a", "/b").to_bytes())
        self.assertEqual(parsed.data, b"/a\x00/b\x00")

    def test_truncate_layout(self):
        parsed, _ = afc.AfcPacket.parse(afc.truncate(6, "/a", 1024).to_bytes())
        self.assertEqual(parsed.operation, afc.AfcOp.TRUNCATE)
        self.assertEqual(parsed.data, b"/a\x00" + struct.pack("<Q", 1024))


class TestAfcResponses(unittest.TestCase):
    def test_status_packet(self):
        body = struct.pack("<Q", 0)
        pkt = afc.AfcPacket(afc.AfcOp.STATUS, body, packet_num=1)
        parsed, _ = afc.AfcPacket.parse(pkt.to_bytes())
        self.assertEqual(parsed.status(), 0)

    def test_status_with_trailing_path_is_ignored(self):
        body = struct.pack("<Q", afc.AfcError.OBJECT_NOT_FOUND) + b"/nope\x00"
        parsed, _ = afc.AfcPacket.parse(
            afc.AfcPacket(afc.AfcOp.STATUS, body, packet_num=1).to_bytes())
        self.assertEqual(parsed.status(), afc.AfcError.OBJECT_NOT_FOUND)

    def test_open_result_handle(self):
        pkt = afc.AfcPacket(afc.AfcOp.FILE_OPEN_RES, struct.pack("<Q", 0x1234))
        parsed, _ = afc.AfcPacket.parse(pkt.to_bytes())
        self.assertEqual(parsed.handle(), 0x1234)

    def test_status_without_body(self):
        with self.assertRaises(afc.AfcProtocolError):
            afc.AfcPacket(afc.AfcOp.STATUS, b"\x00\x00").status()

    def test_incomplete_packet(self):
        blob = afc.file_open(1, "/a").to_bytes()
        with self.assertRaises(afc.AfcProtocolError):
            afc.AfcPacket.parse(blob[:-1])

    def test_this_length_greater_than_entire_rejected(self):
        blob = bytearray(afc.file_open(1, "/a").to_bytes())
        struct.pack_into("<Q", blob, 16, 999)   # this_length
        with self.assertRaises(afc.AfcProtocolError):
            afc.AfcPacket.parse(bytes(blob))

    def test_split_cstrings(self):
        # libimobiledevice 的写法：NUL 分隔，条目数由长度决定，没有额外终止符
        self.assertEqual(afc.split_cstrings(b"a.txt\x00b.txt\x00"), ["a.txt", "b.txt"])
        # 多一个终止 NUL 也要能吃
        self.assertEqual(afc.split_cstrings(b"a.txt\x00b.txt\x00\x00"), ["a.txt", "b.txt"])
        self.assertEqual(afc.split_cstrings(b""), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
