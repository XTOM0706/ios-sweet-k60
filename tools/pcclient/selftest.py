#!/usr/bin/env python3
"""--selftest：把四个协议的示例帧打印成十六进制，不连任何设备。

    python3 tools/pcclient/selftest.py --selftest
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import afc
import framing
import instproxy
import plistmsg


def hexdump(data: bytes, width: int = 16) -> str:
    lines = []
    for off in range(0, len(data), width):
        chunk = data[off:off + width]
        hexpart = " ".join(f"{b:02x}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{off:04x}  {hexpart:<{width * 3}}  {text}")
    return "\n".join(lines)


def section(title: str, blob: bytes, note: str = "") -> None:
    print(f"\n=== {title} ({len(blob)} bytes) ===")
    if note:
        print(f"# {note}")
    print(hexdump(blob))


def main(argv: list[str]) -> int:
    if "--selftest" not in argv:
        print(__doc__)
        return 2

    print("pcclient selftest - 仅编解码，不打开 socket")

    # 1. usbmux: ListDevices
    pkt = framing.list_devices(tag=1)
    section("usbmux ListDevices", pkt.to_bytes(),
            "16 字节小端头 + plist 负载")

    # 2. usbmux: Connect (plist)
    pkt = framing.connect(device_id=3, port=framing.LOCKDOWN_PORT, tag=2)
    section("usbmux Connect -> lockdownd:62078", pkt.to_bytes(),
            f"PortNumber = htons(62078) = {framing.htons(62078)}")

    # 3. usbmux: Connect (旧二进制协议)
    pkt = framing.connect_binary(device_id=3, port=framing.LOCKDOWN_PORT, tag=3)
    section("usbmux Connect (binary, version=0)", pkt.to_bytes(),
            "u32 device_id(LE) + u16 port(网络序) + u16 0")

    # 4. lockdown: GetValue
    req = framing.get_value("ProductVersion")
    blob = framing.encode_request(req)
    section("lockdown GetValue ProductVersion", blob,
            "4 字节大端长度 + XML plist")

    # 5. lockdown: StartSession / StartService
    section("lockdown StartSession",
            framing.encode_request(
                framing.start_session("AABBCCDD-1122-3344-5566-778899AABBCC")))
    section("lockdown StartService com.apple.afc",
            framing.encode_request(framing.start_service(framing.SERVICE_AFC)))

    # 6. AFC: FileOpen / FileWrite / FileClose
    section("AFC FileOpen /PublicStaging/a.ipa (mode=w)",
            afc.file_open(1, "/PublicStaging/a.ipa", afc.AfcFileMode.WRONLY).to_bytes(),
            "40 字节头 + u64 mode + path NUL")
    section("AFC FileWrite handle=1 chunk=5 bytes",
            afc.file_write(2, 1, b"hello").to_bytes(),
            "this_length=48, entire_length=53")
    section("AFC FileClose handle=1", afc.file_close(3, 1).to_bytes())

    # 7. installation_proxy: Install
    cmd = instproxy.install("/PublicStaging/a.ipa",
                            {"PackageType": "Developer", "IsUserInitiated": True})
    section("installation_proxy Install", plistmsg.encode(cmd))

    # 8. 解析方向：一个模拟的 Status 响应
    resp = plistmsg.encode({"Status": "CreatingStagingDirectory", "PercentComplete": 5})
    framer = plistmsg.PlistFramer()
    framer.feed(resp[:7])
    print("\n=== 增量解析 ===")
    print(f"喂 7 字节后：{framer.feed(b'')!r}（还没凑成一帧）")
    msgs = framer.feed(resp[7:])
    print(f"补齐后：{msgs}  -> percent={instproxy.percent_complete(msgs[0])}")

    # 9. 解析方向：usbmux Result
    result = framing.UsbmuxPacket(framing.MUX_VERSION_PLIST, framing.MUX_MSG_RESULT, 2,
                                  b"\x00\x00\x00\x00")
    print(f"\nusbmux Connect 成功回包解析："
          f"result={framing.mux_result_name(result.as_result())}")

    print("\nselftest OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
