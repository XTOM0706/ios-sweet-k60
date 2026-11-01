"""framing.py / plistmsg.py 的单元测试。

    python3 -m unittest                    # 仓库根目录
    python3 tools/pcclient/test_framing.py # 单文件
"""

from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import framing
import plistmsg


class TestUsbmuxHeader(unittest.TestCase):
    def test_header_is_16_bytes_little_endian(self):
        hdr = framing.UsbmuxHeader(length=16, version=1, message=8, tag=7)
        blob = hdr.pack()
        self.assertEqual(len(blob), 16)
        self.assertEqual(blob, bytes.fromhex("10000000010000000800000007000000"))
        self.assertEqual(framing.UsbmuxHeader.unpack(blob), hdr)

    def test_length_includes_the_length_field(self):
        pkt = framing.UsbmuxPacket(framing.MUX_VERSION_PLIST, framing.MUX_MSG_PLIST,
                                   tag=1, payload=b"abc")
        blob = pkt.to_bytes()
        (declared,) = struct.unpack_from("<I", blob, 0)
        self.assertEqual(declared, len(blob))
        self.assertEqual(declared, 19)

    def test_short_header_raises(self):
        with self.assertRaises(framing.IncompleteMuxPacket) as ctx:
            framing.UsbmuxHeader.unpack(b"\x00" * 15)
        self.assertEqual(ctx.exception.needed, 16)

    def test_declared_length_below_header_raises(self):
        with self.assertRaises(framing.MuxProtocolError):
            framing.UsbmuxPacket.parse(bytes.fromhex("0f000000010000000800000001000000"))

    def test_incomplete_payload_reports_needed(self):
        blob = framing.list_devices(tag=1).to_bytes()
        with self.assertRaises(framing.IncompleteMuxPacket) as ctx:
            framing.UsbmuxPacket.parse(blob[:-1])
        self.assertEqual(ctx.exception.needed, len(blob))


class TestUsbmuxRequests(unittest.TestCase):
    def test_list_devices_shape(self):
        pkt = framing.list_devices(tag=1)
        self.assertEqual(pkt.version, framing.MUX_VERSION_PLIST)
        self.assertEqual(pkt.message, framing.MUX_MSG_PLIST)
        self.assertEqual(pkt.tag, 1)
        body = pkt.as_plist()
        self.assertEqual(body["MessageType"], "ListDevices")
        self.assertEqual(body["kLibUSBMuxVersion"], 3)
        self.assertIn("ClientVersionString", body)
        self.assertTrue(pkt.payload.startswith(b"<?xml"))

    def test_htons(self):
        self.assertEqual(framing.htons(62078), 32498)   # 0xf27e -> 0x7ef2
        self.assertEqual(framing.htons(32498), 62078)
        self.assertEqual(framing.htons(80), 20480)      # 0x0050 -> 0x5000
        self.assertEqual(framing.htons(0), 0)
        with self.assertRaises(ValueError):
            framing.htons(0x10000)

    def test_connect_plist_uses_network_order_port(self):
        pkt = framing.connect(device_id=3, port=framing.LOCKDOWN_PORT, tag=2)
        body = pkt.as_plist()
        self.assertEqual(body["MessageType"], "Connect")
        self.assertEqual(body["DeviceID"], 3)
        # 关键点：PortNumber 是 htons 之后的值，不是 62078
        self.assertEqual(body["PortNumber"], 32498)

    def test_connect_binary_golden_bytes(self):
        blob = framing.connect_binary(device_id=3, port=framing.LOCKDOWN_PORT, tag=5).to_bytes()
        self.assertEqual(
            blob.hex(),
            "18000000"      # length = 24
            "00000000"      # version = 0 (旧二进制协议)
            "02000000"      # message = CONNECT
            "05000000"      # tag = 5
            "03000000"      # device_id = 3 (LE)
            "f27e"          # port 62078 网络序
            "0000",         # reserved
        )
        self.assertEqual(len(blob), 24)

    def test_result_packet_parsing(self):
        pkt = framing.UsbmuxPacket(framing.MUX_VERSION_PLIST, framing.MUX_MSG_RESULT,
                                   tag=2, payload=struct.pack("<I", framing.MUX_RESULT_OK))
        self.assertEqual(pkt.as_result(), 0)
        self.assertEqual(framing.mux_result_name(0), "OK")
        self.assertEqual(framing.mux_result_name(3), "ConnectionRefused")
        self.assertEqual(framing.mux_result_name(99), "Unknown(99)")

    def test_result_requires_body(self):
        pkt = framing.UsbmuxPacket(framing.MUX_VERSION_PLIST, framing.MUX_MSG_RESULT, 1)
        with self.assertRaises(framing.MuxProtocolError):
            pkt.as_result()

    def test_parse_stream_splits_packets_and_keeps_tail(self):
        a = framing.list_devices(tag=1).to_bytes()
        b = framing.connect_binary(1, 62078, tag=2).to_bytes()
        packets, rest = framing.parse_stream(a + b + b[:5])
        self.assertEqual(len(packets), 2)
        self.assertEqual([p.tag for p in packets], [1, 2])
        self.assertEqual(rest, b[:5])


class TestPlistFraming(unittest.TestCase):
    def test_length_prefix_is_big_endian_and_excludes_itself(self):
        blob = plistmsg.encode({"Request": "QueryType"})
        (declared,) = struct.unpack_from(">I", blob, 0)
        self.assertEqual(declared, len(blob) - 4)
        self.assertTrue(blob[4:].startswith(b"<?xml"))

    def test_roundtrip(self):
        obj = {"Request": "GetValue", "Key": "ProductVersion", "Label": "pcclient"}
        decoded, used = plistmsg.decode_frame(plistmsg.encode(obj))
        self.assertEqual(decoded, obj)
        self.assertEqual(used, len(plistmsg.encode(obj)))

    def test_short_buffer_is_incomplete(self):
        blob = plistmsg.encode({"a": 1})
        for cut in (0, 1, 3, 4, len(blob) - 1):
            with self.assertRaises(plistmsg.IncompleteFrame):
                plistmsg.decode_frame(blob[:cut])

    def test_empty_payload_rejected(self):
        with self.assertRaises(plistmsg.PlistFrameError):
            plistmsg.decode_frame(struct.pack(">I", 0))

    def test_garbage_payload_rejected(self):
        with self.assertRaises(plistmsg.PlistFrameError):
            plistmsg.decode_frame(struct.pack(">I", 5) + b"hello")

    def test_oversized_length_rejected(self):
        with self.assertRaises(plistmsg.PlistFrameError):
            plistmsg.decode_frame(struct.pack(">I", plistmsg.MAX_PAYLOAD_SIZE + 1))

    def test_incremental_framer_handles_one_byte_at_a_time(self):
        a = plistmsg.encode({"Status": "CreatingStagingDirectory", "PercentComplete": 5})
        b = plistmsg.encode({"Status": "Complete"})
        framer = plistmsg.PlistFramer()
        out = []
        for i in range(len(a) + len(b)):
            out.extend(framer.feed((a + b)[i:i + 1]))
        self.assertEqual([m["Status"] for m in out],
                         ["CreatingStagingDirectory", "Complete"])
        self.assertEqual(framer.pending, b"")

    def test_incremental_framer_keeps_partial(self):
        blob = plistmsg.encode({"Status": "Complete"})
        framer = plistmsg.PlistFramer()
        self.assertEqual(framer.feed(blob[:10]), [])
        self.assertEqual(framer.pending, blob[:10])
        self.assertEqual(framer.feed(blob[10:]), [{"Status": "Complete"}])


class TestLockdown(unittest.TestCase):
    def test_get_value_request(self):
        req = framing.get_value("ProductVersion")
        self.assertEqual(req["Request"], "GetValue")
        self.assertEqual(req["Key"], "ProductVersion")
        self.assertEqual(req["Label"], "pcclient")
        self.assertNotIn("Domain", req)

    def test_get_value_with_domain(self):
        req = framing.get_value(None, domain="com.apple.mobile.battery")
        self.assertEqual(req["Domain"], "com.apple.mobile.battery")
        self.assertNotIn("Key", req)

    def test_start_session_request(self):
        req = framing.start_session("HOST-UUID", "BUID-1")
        self.assertEqual(req["Request"], "StartSession")
        self.assertEqual(req["HostID"], "HOST-UUID")
        self.assertEqual(req["SystemBUID"], "BUID-1")

    def test_start_service_request(self):
        req = framing.start_service(framing.SERVICE_AFC)
        self.assertEqual(req["Request"], "StartService")
        self.assertEqual(req["Service"], "com.apple.afc")
        self.assertNotIn("EscrowBag", req)
        req = framing.start_service(framing.SERVICE_AFC, escrow_bag=b"\x01\x02")
        self.assertEqual(req["EscrowBag"], b"\x01\x02")

    def test_check_result_success(self):
        framing.check_result({"Request": "GetValue", "Result": "Success", "Value": "16.7"})
        framing.check_result({"Type": "com.apple.mobile.lockdown"})  # 没有 Result 键

    def test_check_result_error(self):
        with self.assertRaises(framing.LockdownError) as ctx:
            framing.check_result({"Request": "StartSession", "Error": "InvalidHostID"})
        self.assertEqual(ctx.exception.name, "InvalidHostID")

    def test_check_result_failure(self):
        with self.assertRaises(framing.LockdownError):
            framing.check_result({"Request": "StartService", "Result": "Failure"})

    def test_service_descriptor(self):
        desc = framing.service_descriptor(
            {"Request": "StartService", "Result": "Success", "Port": 50000,
             "EnableServiceSSL": True},
            framing.SERVICE_AFC,
        )
        self.assertEqual(desc.port, 50000)
        self.assertTrue(desc.ssl)
        self.assertEqual(desc.name, "com.apple.afc")

    def test_service_descriptor_missing_port(self):
        with self.assertRaises(framing.LockdownError):
            framing.service_descriptor(
                {"Request": "StartService", "Result": "Success"}, framing.SERVICE_AFC)

    def test_encode_and_decode_roundtrip(self):
        req = framing.start_service(framing.SERVICE_INSTALLATION_PROXY)
        blob = framing.encode_request(req)
        decoded, used = framing.decode_response(blob)
        self.assertEqual(decoded, req)
        self.assertEqual(used, len(blob))

    def test_pair_is_explicitly_unsupported(self):
        with self.assertRaises(NotImplementedError):
            framing.pair()


if __name__ == "__main__":
    unittest.main(verbosity=2)
