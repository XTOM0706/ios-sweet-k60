"""usbmux 帧 + lockdown 请求/响应编解码（仅编解码，不连设备）。

usbmux 帧（16 字节头，全部小端）::

    偏移  0  u32 length    整包长度，**含** 这 4 字节自身
          4  u32 version   0 = 旧二进制协议, 1 = plist 协议
          8  u32 message   消息类型，见 MUX_MSG_*
         12  u32 tag       请求号，响应原样回带

来源:
  * libusbmuxd src/usbmuxd-proto.h（struct usbmuxd_header）
    https://github.com/libimobiledevice/libusbmuxd/blob/master/src/usbmuxd-proto.h
  * libusbmuxd src/libusbmuxd.c（send_packet / receive_packet / create_plist_message）
  * pymobiledevice3 pymobiledevice3/usbmux.py（UsbmuxdHeader, byte_order="<"）
  * 旧文档（字段名 reserved/type）: https://wikee.iphone-dev.com/usb:usbmux
"""

from __future__ import annotations

import plistlib
import struct
from dataclasses import dataclass
from typing import Any

try:  # 作为包导入（python3 -m unittest / -m pcclient）
    from . import plistmsg
except ImportError:  # 作为顶层模块导入（python3 tools/pcclient/selftest.py）
    import plistmsg  # type: ignore[no-redef]

# ---------------------------------------------------------------- usbmux 常量

USBMUX_HEADER_SIZE = 16

MUX_VERSION_BINARY = 0  # 旧协议：定长二进制体
MUX_VERSION_PLIST = 1   # 新协议：plist 体

MUX_MSG_RESULT = 1
MUX_MSG_CONNECT = 2
MUX_MSG_LISTEN = 3
MUX_MSG_DEVICE_ADD = 4
MUX_MSG_DEVICE_REMOVE = 5
MUX_MSG_DEVICE_PAIRED = 6
MUX_MSG_PLIST = 8

MUX_MSG_NAMES = {
    MUX_MSG_RESULT: "Result",
    MUX_MSG_CONNECT: "Connect",
    MUX_MSG_LISTEN: "Listen",
    MUX_MSG_DEVICE_ADD: "DeviceAdd",
    MUX_MSG_DEVICE_REMOVE: "DeviceRemove",
    MUX_MSG_DEVICE_PAIRED: "DevicePaired",
    MUX_MSG_PLIST: "Plist",
}

MUX_RESULT_OK = 0
MUX_RESULT_BADCOMMAND = 1
MUX_RESULT_BADDEV = 2
MUX_RESULT_CONNREFUSED = 3
MUX_RESULT_NOSUCHSERVICE = 4
MUX_RESULT_BADVERSION = 6

MUX_RESULT_NAMES = {
    MUX_RESULT_OK: "OK",
    MUX_RESULT_BADCOMMAND: "BadCommand",
    MUX_RESULT_BADDEV: "BadDevice",
    MUX_RESULT_CONNREFUSED: "ConnectionRefused",
    MUX_RESULT_NOSUCHSERVICE: "NoSuchService",
    MUX_RESULT_BADVERSION: "BadVersion",
}

#: 设备上 lockdownd 监听的 TCP 端口；libimobiledevice 写作 0xf27e
LOCKDOWN_PORT = 62078

#: 客户端自报字段，usbmuxd 只是转发/记录，不做校验
DEFAULT_MUX_CLIENT_FIELDS: dict[str, Any] = {
    "ClientVersionString": "pcclient 0.1",
    "ProgName": "pcclient",
    "kLibUSBMuxVersion": 3,
}


class MuxProtocolError(ValueError):
    """usbmux 帧非法。"""


class IncompleteMuxPacket(MuxProtocolError):
    def __init__(self, needed: int, have: int) -> None:
        super().__init__(f"incomplete usbmux packet: need {needed} bytes, have {have}")
        self.needed = needed
        self.have = have


def htons(port: int) -> int:
    """把主机序端口换成网络序整数值。

    usbmux 的 Connect 要求端口以网络字节序出现：
      * 二进制协议：u16 字段直接放 htons(port) 的两个字节；
      * plist 协议：PortNumber 整数就是 htons(port) 的值（libusbmuxd 就是这么写的）。
    usbmuxd 收到后做 ntohs()，见 usbmuxd src/client.c。
    """
    if not 0 <= port <= 0xFFFF:
        raise ValueError(f"port out of range: {port}")
    return ((port & 0xFF) << 8) | ((port >> 8) & 0xFF)


# ---------------------------------------------------------------- usbmux 帧


@dataclass(frozen=True)
class UsbmuxHeader:
    length: int
    version: int
    message: int
    tag: int

    def pack(self) -> bytes:
        return struct.pack("<IIII", self.length, self.version, self.message, self.tag)

    @classmethod
    def unpack(cls, buf: bytes) -> "UsbmuxHeader":
        if len(buf) < USBMUX_HEADER_SIZE:
            raise IncompleteMuxPacket(USBMUX_HEADER_SIZE, len(buf))
        return cls(*struct.unpack_from("<IIII", buf, 0))


@dataclass(frozen=True)
class UsbmuxPacket:
    version: int
    message: int
    tag: int
    payload: bytes = b""

    def to_bytes(self) -> bytes:
        header = UsbmuxHeader(
            length=USBMUX_HEADER_SIZE + len(self.payload),
            version=self.version,
            message=self.message,
            tag=self.tag,
        )
        return header.pack() + self.payload

    @classmethod
    def parse(cls, buf: bytes) -> tuple["UsbmuxPacket", int]:
        """解出一包，返回 ``(packet, 消耗字节数)``。"""
        header = UsbmuxHeader.unpack(buf)
        if header.length < USBMUX_HEADER_SIZE:
            raise MuxProtocolError(f"declared length {header.length} < header size")
        if len(buf) < header.length:
            raise IncompleteMuxPacket(header.length, len(buf))
        payload = bytes(buf[USBMUX_HEADER_SIZE:header.length])
        return cls(header.version, header.message, header.tag, payload), header.length

    def as_plist(self) -> Any:
        """payload 按 XML plist 解析（version=1 的消息）。"""
        if not self.payload:
            raise MuxProtocolError("packet has no payload")
        return plistlib.loads(self.payload)

    def as_result(self) -> int:
        """MUX_MSG_RESULT 的 4 字节小端结果码。"""
        if self.message != MUX_MSG_RESULT:
            raise MuxProtocolError(f"not a Result packet: message={self.message}")
        if len(self.payload) < 4:
            raise MuxProtocolError("Result packet without 4-byte body")
        return struct.unpack_from("<I", self.payload, 0)[0]


def parse_stream(buf: bytes) -> tuple[list[UsbmuxPacket], bytes]:
    """把缓冲区里所有完整包切出来，返回 ``(包列表, 残余字节)``。"""
    out: list[UsbmuxPacket] = []
    off = 0
    while True:
        try:
            packet, used = UsbmuxPacket.parse(buf[off:])
        except IncompleteMuxPacket:
            return out, bytes(buf[off:])
        out.append(packet)
        off += used


# ---------------------------------------------------------------- usbmux 请求


def _plist_request(message_type: str, tag: int, **fields: Any) -> UsbmuxPacket:
    body: dict[str, Any] = dict(DEFAULT_MUX_CLIENT_FIELDS)
    body["MessageType"] = message_type
    body.update(fields)
    return UsbmuxPacket(MUX_VERSION_PLIST, MUX_MSG_PLIST, tag, plistlib.dumps(body))


def list_devices(tag: int = 1) -> UsbmuxPacket:
    """列设备。响应是 MessageType=Result + DeviceList 数组。"""
    return _plist_request("ListDevices", tag)


def read_buid(tag: int = 1) -> UsbmuxPacket:
    """读本机 SystemBUID（lockdown StartSession 需要）。响应带 BUID 键。"""
    return _plist_request("ReadBUID", tag)


def listen(tag: int = 1) -> UsbmuxPacket:
    """订阅设备插拔事件；之后会陆续收到 Attached / Detached。"""
    return _plist_request("Listen", tag)


def connect(device_id: int, port: int, tag: int = 1) -> UsbmuxPacket:
    """plist 版 Connect（version=1）。PortNumber 是 htons 后的整数。"""
    return _plist_request(
        "Connect",
        tag,
        DeviceID=device_id,
        PortNumber=htons(port),
    )


def connect_binary(device_id: int, port: int, tag: int = 1) -> UsbmuxPacket:
    """旧二进制版 Connect（version=0）。

    体布局::

        u32 device_id (LE) | u16 port (网络序) | u16 reserved = 0
    """
    body = struct.pack("<IHH", device_id, htons(port), 0)
    return UsbmuxPacket(MUX_VERSION_BINARY, MUX_MSG_CONNECT, tag, body)


def mux_result_name(code: int) -> str:
    return MUX_RESULT_NAMES.get(code, f"Unknown({code})")


# ---------------------------------------------------------------- lockdown

LOCKDOWN_LABEL = "pcclient"

SERVICE_AFC = "com.apple.afc"
SERVICE_HOUSE_ARREST = "com.apple.mobile.house_arrest"
SERVICE_INSTALLATION_PROXY = "com.apple.mobile.installation_proxy"
SERVICE_NOTIFICATION_PROXY = "com.apple.mobile.notification_proxy"
SERVICE_SPRINGBOARDSERVICE = "com.apple.springboard.services"


class LockdownError(Exception):
    """设备回了 ``Error``（或 Result != Success）。"""

    def __init__(self, name: str, description: str | None = None, response: Any = None) -> None:
        super().__init__(name if not description else f"{name}: {description}")
        self.name = name
        self.description = description
        self.response = response


def _lockdown_request(request: str, label: str | None, **fields: Any) -> dict[str, Any]:
    req: dict[str, Any] = {"Request": request}
    if label is not None:
        req["Label"] = label
    req.update(fields)
    return req


def query_type(label: str | None = LOCKDOWN_LABEL) -> dict[str, Any]:
    """探测对端是不是 lockdownd：响应 ``Type`` 应为 com.apple.mobile.lockdown。"""
    return _lockdown_request("QueryType", label)


def get_value(
    key: str | None = None,
    domain: str | None = None,
    label: str | None = LOCKDOWN_LABEL,
) -> dict[str, Any]:
    """读设备属性。key 为 None 表示整个 domain 的字典。"""
    fields: dict[str, Any] = {}
    if domain is not None:
        fields["Domain"] = domain
    if key is not None:
        fields["Key"] = key
    return _lockdown_request("GetValue", label, **fields)


def set_value(
    key: str,
    value: Any,
    domain: str | None = None,
    label: str | None = LOCKDOWN_LABEL,
) -> dict[str, Any]:
    fields: dict[str, Any] = {"Key": key, "Value": value}
    if domain is not None:
        fields["Domain"] = domain
    return _lockdown_request("SetValue", label, **fields)


def start_session(
    host_id: str,
    system_buid: str | None = None,
    label: str | None = LOCKDOWN_LABEL,
) -> dict[str, Any]:
    """开 session。

    HostID 必须与设备上已存的 pairing record 匹配，否则回 InvalidHostID。
    SystemBUID 来自宿主 ``usbmuxd ReadBUID``，设备侧一般不校验（待实测）。
    """
    fields: dict[str, Any] = {"HostID": host_id}
    if system_buid is not None:
        fields["SystemBUID"] = system_buid
    return _lockdown_request("StartSession", label, **fields)


def stop_session(session_id: str, label: str | None = LOCKDOWN_LABEL) -> dict[str, Any]:
    return _lockdown_request("StopSession", label, SessionID=session_id)


def start_service(
    service: str,
    label: str | None = LOCKDOWN_LABEL,
    escrow_bag: bytes | None = None,
) -> dict[str, Any]:
    """要一个服务端口。响应 ``Port`` + 可选 ``EnableServiceSSL``。"""
    fields: dict[str, Any] = {"Service": service}
    if escrow_bag is not None:
        fields["EscrowBag"] = escrow_bag
    return _lockdown_request("StartService", label, **fields)


def encode_request(request: dict[str, Any]) -> bytes:
    """lockdown 请求 -> 线上字节（4 字节大端长度 + XML plist）。"""
    return plistmsg.encode(request)


def decode_response(buf: bytes) -> tuple[dict[str, Any], int]:
    obj, used = plistmsg.decode_frame(buf)
    if not isinstance(obj, dict):
        raise plistmsg.PlistFrameError(f"lockdown response is not a dict: {type(obj).__name__}")
    return obj, used


def check_result(response: dict[str, Any], request: str | None = None) -> None:
    """按 libimobiledevice lockdown_check_result() 的规则判成败。

    * 有 ``Error`` 键 -> 抛 :class:`LockdownError`（值是字符串标识符，如 InvalidHostID）；
    * 有 ``Result`` 键且不是 "Success" -> 抛错；
    * 两者都没有（例如 QueryType）视为成功。
    """
    error = response.get("Error")
    if error is not None:
        raise LockdownError(str(error), response.get("ErrorDescription"), response)
    result = response.get("Result")
    if result is not None and result != "Success":
        raise LockdownError(str(result), None, response)


@dataclass(frozen=True)
class ServiceDescriptor:
    name: str
    port: int
    ssl: bool = False


def service_descriptor(response: dict[str, Any], service: str) -> ServiceDescriptor:
    """从 StartService 响应里取端口。缺 Port 视为协议错误。"""
    check_result(response, "StartService")
    port = response.get("Port")
    if not isinstance(port, int) or port <= 0:
        raise LockdownError("MissingPort", "StartService returned no usable Port", response)
    ssl = bool(response.get("EnableServiceSSL", False))
    return ServiceDescriptor(service, port, ssl)


def pair(**_kwargs: Any) -> dict[str, Any]:
    """配对（Pair/ValidatePair）本原型不实现。

    瘦客户端直接复用设备上已有的 pairing record（HostID + EscrowBag），
    不需要证书链校验。真要做完整 Pair，见 PROTOCOL-SPEC.md 的「可跳过部分」。
    """
    raise NotImplementedError(
        "Pair 未实现：本客户端假设设备侧已有本机的 pairing record（HostID）"
    )
