"""AFC（Apple File Conduit）帧编解码。

帧头固定 40 字节::

     0  char[8] magic = "CFA6LPAA"
     8  u64 entire_length   整包长度 = 40 + data + payload
    16  u64 this_length     头 + 紧跟的 data 段长度 = 40 + len(data)
    24  u64 packet_num      请求序号，设备原样回带
    32  u64 operation       opcode，见 AfcOp

``entire_length - this_length`` 的差额就是尾随的 bulk payload（FileWrite 的数据块），
设备可以分多次 write 发出来。

来源:
  * libimobiledevice src/afc.h（AFC_MAGIC / AFCPacket / opcode 枚举）
    https://github.com/libimobiledevice/libimobiledevice/blob/master/src/afc.h
  * libimobiledevice src/afc.c（afc_dispatch_packet / afc_receive_data）
  * pymobiledevice3 pymobiledevice3/services/afc.py（AfcHeader, byte_order="<"）
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum

AFC_MAGIC = b"CFA6LPAA"
AFC_HEADER_SIZE = 40


class AfcOp(IntEnum):
    INVALID = 0x00
    STATUS = 0x01
    DATA = 0x02
    READ_DIR = 0x03
    READ_FILE = 0x04
    WRITE_FILE = 0x05
    WRITE_PART = 0x06
    TRUNCATE = 0x07
    REMOVE_PATH = 0x08
    MAKE_DIR = 0x09
    GET_FILE_INFO = 0x0A
    GET_DEVINFO = 0x0B
    WRITE_FILE_ATOM = 0x0C
    FILE_OPEN = 0x0D
    FILE_OPEN_RES = 0x0E
    FILE_READ = 0x0F
    FILE_WRITE = 0x10
    FILE_SEEK = 0x11
    FILE_TELL = 0x12
    FILE_TELL_RES = 0x13
    FILE_CLOSE = 0x14
    FILE_SET_SIZE = 0x15
    GET_CON_INFO = 0x16
    SET_CON_OPTIONS = 0x17
    RENAME_PATH = 0x18
    SET_FS_BS = 0x19
    SET_SOCKET_BS = 0x1A
    FILE_LOCK = 0x1B
    MAKE_LINK = 0x1C
    GET_FILE_HASH = 0x1D
    SET_FILE_MOD_TIME = 0x1E
    GET_FILE_HASH_RANGE = 0x1F
    FILE_SET_IMMUTABLE_HINT = 0x20
    GET_SIZE_OF_PATH_CONTENTS = 0x21
    REMOVE_PATH_AND_CONTENTS = 0x22
    DIR_OPEN = 0x23
    DIR_OPEN_RESULT = 0x24
    DIR_READ = 0x25
    DIR_CLOSE = 0x26
    FILE_READ_OFFSET = 0x27
    FILE_WRITE_OFFSET = 0x28


class AfcFileMode(IntEnum):
    RDONLY = 0x01     # r
    RW = 0x02         # r+
    WRONLY = 0x03     # w  (O_WRONLY|O_CREAT|O_TRUNC)
    WR = 0x04         # w+
    APPEND = 0x05     # a
    RDAPPEND = 0x06   # a+


class AfcError(IntEnum):
    SUCCESS = 0
    UNKNOWN_ERROR = 1
    OP_HEADER_INVALID = 2
    NO_RESOURCES = 3
    READ_ERROR = 4
    WRITE_ERROR = 5
    UNKNOWN_PACKET_TYPE = 6
    INVALID_ARG = 7
    OBJECT_NOT_FOUND = 8
    OBJECT_IS_DIR = 9
    PERM_DENIED = 10
    SERVICE_NOT_CONNECTED = 11
    OP_TIMEOUT = 12
    TOO_MUCH_DATA = 13
    END_OF_DATA = 14
    OP_NOT_SUPPORTED = 15
    OBJECT_EXISTS = 16
    OBJECT_BUSY = 17
    NO_SPACE_LEFT = 18
    OP_WOULD_BLOCK = 19
    IO_ERROR = 20
    OP_INTERRUPTED = 21
    OP_IN_PROGRESS = 22
    INTERNAL_ERROR = 23
    MUX_ERROR = 30
    NO_MEM = 31
    NOT_ENOUGH_DATA = 32
    DIR_NOT_EMPTY = 33
    SSL_ERROR = 34


class AfcProtocolError(ValueError):
    pass


def _cstr(text: str) -> bytes:
    """AFC 的路径都是 NUL 结尾的 UTF-8。"""
    return text.encode("utf-8") + b"\x00"


@dataclass(frozen=True)
class AfcHeader:
    entire_length: int
    this_length: int
    packet_num: int
    operation: int

    def pack(self) -> bytes:
        return AFC_MAGIC + struct.pack("<QQQQ", self.entire_length, self.this_length,
                                       self.packet_num, self.operation)

    @classmethod
    def unpack(cls, buf: bytes) -> "AfcHeader":
        if len(buf) < AFC_HEADER_SIZE:
            raise AfcProtocolError(f"need {AFC_HEADER_SIZE} bytes, have {len(buf)}")
        if buf[:8] != AFC_MAGIC:
            raise AfcProtocolError(f"bad magic: {buf[:8]!r}")
        entire, this, num, op = struct.unpack_from("<QQQQ", buf, 8)
        return cls(entire, this, num, op)


@dataclass(frozen=True)
class AfcPacket:
    """一包 = 头 + data（紧跟头）+ payload（尾随 bulk）。"""

    operation: int
    data: bytes = b""
    payload: bytes = b""
    packet_num: int = 0

    def to_bytes(self) -> bytes:
        entire = AFC_HEADER_SIZE + len(self.data) + len(self.payload)
        this = AFC_HEADER_SIZE + len(self.data)
        header = AfcHeader(entire, this, self.packet_num, self.operation)
        return header.pack() + self.data + self.payload

    @classmethod
    def parse(cls, buf: bytes) -> tuple["AfcPacket", int]:
        header = AfcHeader.unpack(buf)
        if header.entire_length < AFC_HEADER_SIZE:
            raise AfcProtocolError(f"entire_length {header.entire_length} < header")
        if header.this_length < AFC_HEADER_SIZE:
            raise AfcProtocolError(f"this_length {header.this_length} < header")
        if header.this_length > header.entire_length:
            raise AfcProtocolError("this_length > entire_length")
        if len(buf) < header.entire_length:
            raise AfcProtocolError(
                f"incomplete: need {header.entire_length}, have {len(buf)}"
            )
        data_end = header.this_length
        return (
            cls(
                operation=header.operation,
                data=bytes(buf[AFC_HEADER_SIZE:data_end]),
                payload=bytes(buf[data_end:header.entire_length]),
                packet_num=header.packet_num,
            ),
            header.entire_length,
        )

    def status(self) -> int:
        """STATUS 包体前 8 字节 = 状态码；后面可能还跟一个路径字符串（待实测）。"""
        if len(self.data) < 8:
            raise AfcProtocolError("status packet without 8-byte status code")
        return struct.unpack_from("<Q", self.data, 0)[0]

    def handle(self) -> int:
        """FILE_OPEN_RES 包体前 8 字节 = 文件句柄。"""
        if len(self.data) < 8:
            raise AfcProtocolError("open-result packet without 8-byte handle")
        return struct.unpack_from("<Q", self.data, 0)[0]


# ---------------------------------------------------------------- 请求构造


def file_open(packet_num: int, path: str, mode: AfcFileMode = AfcFileMode.RDONLY) -> AfcPacket:
    """FileRefOpen：``u64 mode | path\\0``。"""
    return AfcPacket(AfcOp.FILE_OPEN, struct.pack("<Q", int(mode)) + _cstr(path),
                     packet_num=packet_num)


def file_write(packet_num: int, handle: int, chunk: bytes) -> AfcPacket:
    """FileRefWrite：data = ``u64 handle``，payload = 数据块。

    this_length 恒为 48（40 + 8），entire_length = 48 + len(chunk)。
    """
    return AfcPacket(AfcOp.FILE_WRITE, struct.pack("<Q", handle), chunk, packet_num)


def file_read(packet_num: int, handle: int, length: int) -> AfcPacket:
    """FileRefRead：``u64 handle | u64 length``。"""
    return AfcPacket(AfcOp.FILE_READ, struct.pack("<QQ", handle, length),
                     packet_num=packet_num)


def file_close(packet_num: int, handle: int) -> AfcPacket:
    return AfcPacket(AfcOp.FILE_CLOSE, struct.pack("<Q", handle), packet_num=packet_num)


def file_seek(packet_num: int, handle: int, offset: int, whence: int) -> AfcPacket:
    """whence: 0=SET 1=CUR 2=END。"""
    return AfcPacket(AfcOp.FILE_SEEK,
                     struct.pack("<QQQ", handle, offset & 0xFFFFFFFFFFFFFFFF, whence),
                     packet_num=packet_num)


def make_dir(packet_num: int, path: str) -> AfcPacket:
    return AfcPacket(AfcOp.MAKE_DIR, _cstr(path), packet_num=packet_num)


def read_dir(packet_num: int, path: str) -> AfcPacket:
    return AfcPacket(AfcOp.READ_DIR, _cstr(path), packet_num=packet_num)


def get_file_info(packet_num: int, path: str) -> AfcPacket:
    return AfcPacket(AfcOp.GET_FILE_INFO, _cstr(path), packet_num=packet_num)


def remove_path(packet_num: int, path: str) -> AfcPacket:
    return AfcPacket(AfcOp.REMOVE_PATH, _cstr(path), packet_num=packet_num)


def rename_path(packet_num: int, src: str, dst: str) -> AfcPacket:
    """RenamePath：``src\\0 dst\\0``。"""
    return AfcPacket(AfcOp.RENAME_PATH, _cstr(src) + _cstr(dst), packet_num=packet_num)


def truncate(packet_num: int, path: str, size: int) -> AfcPacket:
    return AfcPacket(AfcOp.TRUNCATE, _cstr(path) + struct.pack("<Q", size),
                     packet_num=packet_num)


def get_device_info(packet_num: int) -> AfcPacket:
    return AfcPacket(AfcOp.GET_DEVINFO, b"", packet_num=packet_num)


def split_cstrings(blob: bytes) -> list[str]:
    """把 AFC 返回的「一串 NUL 结尾字符串」切成列表（ReadDir / GetFileInfo 用）。

    列表结束由帧长度隐含：libimobiledevice 数 payload 里 NUL 的个数当作条目数
    （src/afc.c make_strings_list / count_nullspaces），并不会多读一个终止 NUL。
    这里对「末尾多一个 NUL」的写法也容忍（空串条目直接丢弃），两种都能吃。
    """
    if not blob:
        return []
    return [p.decode("utf-8", "replace") for p in blob.split(b"\x00") if p]
