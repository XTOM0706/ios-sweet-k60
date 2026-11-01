"""lockdown / installation_proxy 的 plist 报文封装。

线上格式（每条消息一帧）::

    +-------------------+--------------------------------+
    | u32 大端 length   |  length 字节的 plist 负载       |
    +-------------------+--------------------------------+

* length 只计负载，**不含** 这 4 字节自身（与 usbmux 的 length 语义不同）。
* 负载是 UTF-8 的 XML plist；设备回包同样是 XML plist。
* 没有 magic、没有版本号、没有请求号：请求与响应靠「一问一答」的顺序配对。

来源（均为开源实现，非苹果文档）:
  * libimobiledevice src/property_list_service.c, internal_plist_send():
        nlen = htobe32(length); service_send(parent, &nlen, 4, &bytes);
    https://github.com/libimobiledevice/libimobiledevice/blob/master/src/property_list_service.c
  * pymobiledevice3 pymobiledevice3/service_connection.py, build_plist():
        message = struct.pack(">L", len(payload)) + payload
    https://github.com/doronz88/pymobiledevice3/blob/master/pymobiledevice3/service_connection.py
"""

from __future__ import annotations

import plistlib
import struct
from typing import Any

LENGTH_PREFIX_SIZE = 4
#: 单帧负载上限，纯粹是本实现的防御性上限（设备不会发这么大的 plist）
MAX_PAYLOAD_SIZE = 8 * 1024 * 1024


class PlistFrameError(ValueError):
    """帧格式非法（长度前缀 < 4、超过上限、plist 解析失败）。"""


class IncompleteFrame(PlistFrameError):
    """缓冲区里的字节还不够一帧，继续收。"""

    def __init__(self, needed: int, have: int) -> None:
        super().__init__(f"incomplete plist frame: need {needed} bytes, have {have}")
        self.needed = needed
        self.have = have


def encode(obj: Any, *, fmt: plistlib.PlistFormat = plistlib.FMT_XML) -> bytes:
    """把一个 dict 序列化成完整的一帧（4 字节大端长度 + plist 负载）。"""
    payload = plistlib.dumps(obj, fmt=fmt)
    if len(payload) > MAX_PAYLOAD_SIZE:
        raise PlistFrameError(f"payload too large: {len(payload)}")
    return struct.pack(">I", len(payload)) + payload


def decode_frame(buf: bytes) -> tuple[Any, int]:
    """从缓冲区头部解出一帧。

    返回 ``(plist 对象, 消耗的字节数)``；数据不够时抛 :class:`IncompleteFrame`。
    """
    if len(buf) < LENGTH_PREFIX_SIZE:
        raise IncompleteFrame(LENGTH_PREFIX_SIZE, len(buf))
    (length,) = struct.unpack_from(">I", buf, 0)
    if length > MAX_PAYLOAD_SIZE:
        raise PlistFrameError(f"declared payload length {length} exceeds limit")
    total = LENGTH_PREFIX_SIZE + length
    if len(buf) < total:
        raise IncompleteFrame(total, len(buf))
    payload = bytes(buf[LENGTH_PREFIX_SIZE:total])
    if not payload:
        raise PlistFrameError("empty plist payload")
    try:
        return plistlib.loads(payload), total
    except Exception as exc:  # plistlib raises several unrelated types
        raise PlistFrameError(f"bad plist payload: {exc}") from exc


class PlistFramer:
    """增量解析器：把 socket 上读到的任意长度 chunk 喂进来，吐出完整消息。"""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, chunk: bytes) -> list[Any]:
        self._buf.extend(chunk)
        out: list[Any] = []
        while True:
            try:
                obj, used = decode_frame(bytes(self._buf))
            except IncompleteFrame:
                return out
            del self._buf[:used]
            out.append(obj)

    @property
    def pending(self) -> bytes:
        """还没凑成一帧的残余字节（调试用）。"""
        return bytes(self._buf)
