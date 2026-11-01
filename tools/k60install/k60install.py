#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""k60install —— 宿主侧 IPA → "待推送到设备" 目录树 生成器。

对应 docs/INSTALLER-PLAN.md 的里程碑:

  I2  给一个 .ipa，产出容器目录树 + metadata plist + uicache 输入
  I3  --sign 调 ldid，把 entitlements.xml 签进 app 里所有 Mach-O

本程序**只写文件**: 不联网、不碰设备、不写 /etc、不需要 root。
设备那一半由产物里的 install.sh 完成（它才是要拷到设备上跑的）。

产物 (out/<bundleid>/):

    Bundle/<Name>.app/...     Payload 里解出来的 app（权限位尽量保留）
    Bundle/.com.apple.mobile_container_manager.metadata.plist
                              Bundle 容器 metadata（ContentClass=1）
    Data/Documents/           ┐
    Data/Library/Preferences/ ├ 数据容器骨架（拷进 Data/<UUID>/ 里）
    Data/tmp/                 ┘
    metadata.plist            Data 容器 metadata（install.sh 改名拷入）
    install.sh                设备端安装脚本（支持 --dry-run 空跑）
    manifest.json             给上层脚本/CI 用的机器可读摘要

用法:

    python3 k60install.py test.ipa
    python3 k60install.py test.ipa -o out --uuid 6F9C...-... --force
    python3 k60install.py test.ipa --sign                    # 找 ldid
    python3 k60install.py test.ipa --sign --ldid /var/k60bootstrap/usr/bin/ldid
    python3 k60install.py test.ipa --sign --verify           # 签完回读 entitlements
    python3 k60install.py test.ipa --json                    # stdout 只出 JSON

退出码: 0 成功 / 1 运行错误 / 2 输入有问题 / 3 找不到 ldid。
"""

from __future__ import annotations

import argparse
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import uuid as uuidlib
import zipfile
from pathlib import Path

# ---------------------------------------------------------------------------
# 常量：真机容器布局
# ---------------------------------------------------------------------------

# 设备上的两棵容器树（rootless 风格也仍然用这两个绝对路径，容器管理是内核/守护进程的事）
BUNDLE_ROOT = "/private/var/containers/Bundle/Application"
DATA_ROOT = "/private/var/mobile/Containers/Data/Application"

MCM_METADATA_NAME = ".com.apple.mobile_container_manager.metadata.plist"

# MCMContainerClass: 真机 dump 里 Bundle 容器是 1、App Data 容器是 2、App Group 是 7。
# 见 docs/INSTALLER-PLAN.md §2.1；上机后要拿一个系统 App 的容器对照一遍。
MCM_CLASS_BUNDLE = 1
MCM_CLASS_DATA = 2

MCM_SCHEMA_VERSION = 1
MCM_PERSONA = 501          # 单用户设备上的 mobile persona
MOBILE_UID = 501
MOBILE_GID = 501

# uicache / ldid 在设备上（Procursus bootstrap 装到 /var/k60bootstrap）的常见位置
UICACHE_CANDIDATES = (
    "/var/k60bootstrap/usr/bin/uicache",
    "/usr/bin/uicache",
    "/var/jb/usr/bin/uicache",
    "/usr/local/bin/uicache",
)

LDID_CANDIDATES = (
    "/var/k60bootstrap/usr/bin/ldid",
    "/var/jb/usr/bin/ldid",
    "/usr/local/bin/ldid",
    "/usr/bin/ldid",
)

HERE = Path(__file__).resolve().parent
DEFAULT_ENTITLEMENTS = HERE / "entitlements.xml"


class InstallError(Exception):
    """输入/环境问题；main() 转成退出码 2（或 3，ldid 缺失时）。"""

    def __init__(self, message: str, code: int = 2):
        super().__init__(message)
        self.code = code


# ---------------------------------------------------------------------------
# Mach-O：识别 + 回读 embedded entitlements
# ---------------------------------------------------------------------------

MH_MAGIC = 0xFEEDFACE
MH_CIGAM = 0xCEFAEDFE
MH_MAGIC_64 = 0xFEEDFACF
MH_CIGAM_64 = 0xCFFAEDFE
FAT_MAGIC = 0xCAFEBABE
FAT_CIGAM = 0xBEBAFECA

LC_CODE_SIGNATURE = 0x1D
CSMAGIC_EMBEDDED_SIGNATURE = 0xFADE0CC0
CSMAGIC_EMBEDDED_ENTITLEMENTS = 0xFADE7171
CSSLOT_ENTITLEMENTS = 5


def is_macho(path: Path) -> bool:
    """按魔数判断是不是 Mach-O（thin 或 fat）。"""
    try:
        with open(path, "rb") as fp:
            head = fp.read(4)
    except OSError:
        return False
    if len(head) < 4:
        return False
    return struct.unpack(">I", head)[0] in (
        MH_MAGIC, MH_CIGAM, MH_MAGIC_64, MH_CIGAM_64, FAT_MAGIC, FAT_CIGAM,
    )


def _macho_slices(data: bytes) -> list[bytes]:
    """把 fat 拆成若干 thin 切片；thin 原样返回。"""
    if len(data) < 8:
        return []
    be = struct.unpack(">I", data[:4])[0]
    if be not in (FAT_MAGIC, FAT_CIGAM):
        return [data]
    endian = ">" if be == FAT_MAGIC else "<"
    nfat = struct.unpack(endian + "I", data[4:8])[0]
    out = []
    for i in range(min(nfat, 64)):
        base = 8 + i * 20
        if base + 20 > len(data):
            break
        # cputype, cpusubtype, offset, size, align
        _, _, offset, size, _ = struct.unpack(endian + "IIIII", data[base:base + 20])
        if offset + size > len(data) or size == 0:
            continue
        out.append(data[offset:offset + size])
    return out


def _cs_entitlements(sl: bytes) -> str | None:
    if len(sl) < 28:
        return None
    magic = struct.unpack("<I", sl[:4])[0]
    if magic == MH_MAGIC:
        endian, is64 = "<", False
    elif magic == MH_CIGAM:
        endian, is64 = ">", False
    elif magic == MH_MAGIC_64:
        endian, is64 = "<", True
    elif magic == MH_CIGAM_64:
        endian, is64 = ">", True
    else:
        return None
    ncmds = struct.unpack(endian + "I", sl[16:20])[0]
    off = 32 if is64 else 28
    for _ in range(min(ncmds, 4096)):
        if off + 8 > len(sl):
            return None
        cmd, cmdsize = struct.unpack(endian + "II", sl[off:off + 8])
        if cmdsize < 8:
            return None
        if cmd == LC_CODE_SIGNATURE:
            if off + 16 > len(sl):
                return None
            dataoff, datasize = struct.unpack(endian + "II", sl[off + 8:off + 16])
            return _superblob_entitlements(sl[dataoff:dataoff + datasize])
        off += cmdsize
    return None


def _superblob_entitlements(blob: bytes) -> str | None:
    if len(blob) < 12:
        return None
    magic, _length, count = struct.unpack(">III", blob[:12])
    if magic != CSMAGIC_EMBEDDED_SIGNATURE:
        return None
    for i in range(min(count, 64)):
        entry = 12 + i * 8
        if entry + 8 > len(blob):
            break
        slot, off = struct.unpack(">II", blob[entry:entry + 8])
        if slot != CSSLOT_ENTITLEMENTS or off + 8 > len(blob):
            continue
        bmagic, blen = struct.unpack(">II", blob[off:off + 8])
        if bmagic != CSMAGIC_EMBEDDED_ENTITLEMENTS or blen < 8:
            continue
        return blob[off + 8:off + blen].decode("utf-8", "replace").rstrip("\x00")
    return None


def read_entitlements(path: Path) -> str | None:
    """从已签名的 Mach-O 里把 entitlements XML 抠出来（替代 macOS 的 codesign -d）。

    等价于 `codesign -d --entitlements :- <binary>`，但纯标准库、Linux 可跑。
    """
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    for sl in _macho_slices(data):
        ents = _cs_entitlements(sl)
        if ents:
            return ents
    return None


# ---------------------------------------------------------------------------
# IPA 解析
# ---------------------------------------------------------------------------

def _safe_relpath(name: str) -> str:
    """zip 里的相对路径 → 安全相对路径（拒绝绝对路径与 .. 穿越）。"""
    p = name.replace("\\", "/")
    if p.startswith("/") or re.match(r"^[A-Za-z]:", p):
        raise InstallError(f"IPA 里有绝对路径条目，拒绝处理: {name!r}")
    parts = []
    for seg in p.split("/"):
        if seg in ("", "."):
            continue
        if seg == "..":
            raise InstallError(f"IPA 里有路径穿越条目，拒绝处理: {name!r}")
        parts.append(seg)
    return "/".join(parts)


def find_app_bundle(zf: zipfile.ZipFile) -> str:
    """返回 Payload 下唯一的 .app 目录名（不含尾部 /）。"""
    tops: list[str] = []
    for info in zf.infolist():
        name = info.filename
        if not name.startswith("Payload/"):
            continue
        rest = name[len("Payload/"):]
        first = rest.split("/", 1)[0]
        if first and first.endswith(".app") and first not in tops:
            tops.append(first)
    if not tops:
        raise InstallError("IPA 里找不到 Payload/*.app（不是有效的 IPA，或者被加密了）")
    if len(tops) > 1:
        raise InstallError(
            "IPA 里有多个 app bundle，不知道怎么选: " + ", ".join(sorted(tops))
        )
    return tops[0]


def read_info_plist(zf: zipfile.ZipFile, app_name: str) -> dict:
    member = f"Payload/{app_name}/Info.plist"
    try:
        raw = zf.read(member)
    except KeyError:
        raise InstallError(f"IPA 里没有 {member}") from None
    try:
        info = plistlib.loads(raw)
    except Exception as exc:  # plistlib 的异常类型随版本变
        raise InstallError(f"{member} 解析失败: {exc}") from exc
    if not isinstance(info, dict):
        raise InstallError(f"{member} 不是字典")
    return info


def bundle_info(info: dict) -> dict:
    """从 Info.plist 里取安装需要的那几项。"""
    bundle_id = info.get("CFBundleIdentifier")
    if not isinstance(bundle_id, str) or not bundle_id.strip():
        raise InstallError("Info.plist 里没有 CFBundleIdentifier")
    bundle_id = bundle_id.strip()
    # 只允许安全字符：bundle id 会被写进 metadata.plist，也会被拼进 shell 脚本
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", bundle_id):
        raise InstallError(f"CFBundleIdentifier 含非法字符: {bundle_id!r}")

    exec_name = info.get("CFBundleExecutable")
    if not isinstance(exec_name, str) or not exec_name:
        raise InstallError(f"{bundle_id}: Info.plist 里没有 CFBundleExecutable")
    if "/" in exec_name or exec_name in (".", ".."):
        raise InstallError(f"{bundle_id}: CFBundleExecutable 非法: {exec_name!r}")

    return {
        "bundle_id": bundle_id,
        "executable": exec_name,
        "display_name": str(info.get("CFBundleDisplayName") or info.get("CFBundleName") or exec_name),
        "short_version": str(info.get("CFBundleShortVersionString") or ""),
        "build_version": str(info.get("CFBundleVersion") or ""),
        "minimum_os": str(info.get("MinimumOSVersion") or ""),
    }


def extract_app(zf: zipfile.ZipFile, app_name: str, dest: Path) -> int:
    """把 Payload/<app_name>/ 解到 dest（dest 就是 .app 本身）。返回文件数。"""
    prefix = f"Payload/{app_name}/"
    count = 0
    dest.mkdir(parents=True, exist_ok=True)
    for info in zf.infolist():
        name = info.filename
        if not name.startswith(prefix) or name.endswith("/"):
            if name.startswith(prefix) and name.endswith("/"):
                rel = _safe_relpath(name[len(prefix):])
                if rel:
                    (dest / rel).mkdir(parents=True, exist_ok=True)
            continue
        rel = _safe_relpath(name[len(prefix):])
        if not rel:
            continue
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        mode = (info.external_attr >> 16) & 0xFFFF
        if (mode & 0o170000) == 0o120000:  # 符号链接：内容是链接目标
            link_target = zf.read(info).decode("utf-8", "surrogateescape")
            if target.is_symlink() or target.exists():
                target.unlink()
            os.symlink(link_target, target)
            count += 1
            continue
        with zf.open(info) as src, open(target, "wb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
        perm = mode & 0o7777
        if not perm:
            # 有些打包工具不写权限位：可执行位靠 zip 里的名字猜不出来，
            # 先给 0644，签名/上机前由 install.sh 的 chmod 兜底（见下文）。
            perm = 0o644
        os.chmod(target, perm)
        count += 1
    return count


# ---------------------------------------------------------------------------
# metadata plist
# ---------------------------------------------------------------------------

def build_data_metadata(bundle_id: str, container_uuid: str) -> dict:
    """App Data 容器的 metadata（真机 dump 的字段子集）。"""
    return {
        "MCMMetadataIdentifier": bundle_id,
        "MCMMetadataUUID": container_uuid,
        "MCMMetadataContentClass": MCM_CLASS_DATA,
        "MCMMetadataSchemaVersion": MCM_SCHEMA_VERSION,
        "MCMMetadataPersona": MCM_PERSONA,
        "MCMMetadataInfo": {
            "com.apple.MobileInstallation.ContentProtectionClass": -1,
        },
    }


def build_bundle_metadata(bundle_id: str, container_uuid: str) -> dict:
    """Bundle 容器的 metadata（ContentClass=1）。"""
    return {
        "MCMMetadataIdentifier": bundle_id,
        "MCMMetadataUUID": container_uuid,
        "MCMMetadataContentClass": MCM_CLASS_BUNDLE,
        "MCMMetadataSchemaVersion": MCM_SCHEMA_VERSION,
        "MCMMetadataPersona": MCM_PERSONA,
    }


def write_plist(path: Path, obj: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as fp:
        plistlib.dump(obj, fp, fmt=plistlib.FMT_XML, sort_keys=True)


# ---------------------------------------------------------------------------
# 签名（I3）
# ---------------------------------------------------------------------------

def find_ldid(explicit: str | None = None) -> str | None:
    """按 --ldid > $K60_LDID > $LDID > PATH > 常见 bootstrap 路径 找 ldid。"""
    cands: list[str] = []
    if explicit:
        cands.append(explicit)
    for env in ("K60_LDID", "LDID"):
        if os.environ.get(env):
            cands.append(os.environ[env])
    which = shutil.which("ldid")
    if which:
        cands.append(which)
    cands.extend(LDID_CANDIDATES)
    for cand in cands:
        if cand and os.path.isfile(cand) and os.access(cand, os.X_OK):
            return os.path.abspath(cand)
    return None


LDID_HELP = """\
找不到 ldid，--sign 没法执行。装一个再回来（三种任选）:

  Arch Linux (AUR):     yay -S ldid          # 或 paru -S ldid
  Procursus 现成二进制:  从 bootstrap 包里取 usr/bin/ldid，例如
                        tools/bootstrap/build_bootstrap.sh 产出的
                        build/k60bootstrap-*.tar.zst 里的 usr/bin/ldid
  自己编译:              git clone https://github.com/ProcursusTeam/ldid
                        cd ldid && make

装好后任选一种告诉本程序:
  k60install.py app.ipa --sign --ldid /path/to/ldid
  export K60_LDID=/path/to/ldid && k60install.py app.ipa --sign
"""


def iter_macho_files(app_dir: Path) -> list[Path]:
    out: list[Path] = []
    for root, dirs, files in os.walk(app_dir):
        dirs.sort()
        for name in sorted(files):
            p = Path(root) / name
            if p.is_symlink():
                continue
            if is_macho(p):
                out.append(p)
    return out


def sign_binaries(app_dir: Path, ldid: str, entitlements: Path,
                  *, dry_run: bool = False) -> list[Path]:
    """给 app 里每个 Mach-O 签 entitlements。返回被签的文件列表。"""
    binaries = iter_macho_files(app_dir)
    if not binaries:
        raise InstallError(f"{app_dir.name} 里没有找到任何 Mach-O（主程序是不是没解出来？）")
    if dry_run:
        return binaries
    for binary in binaries:
        cmd = [ldid, f"-S{entitlements}", str(binary)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise InstallError(
                "ldid 签名失败: {}\n  rc={}\n  stdout={}\n  stderr={}".format(
                    " ".join(cmd), proc.returncode,
                    proc.stdout.strip(), proc.stderr.strip(),
                )
            )
    return binaries


# ---------------------------------------------------------------------------
# install.sh（设备端）
# ---------------------------------------------------------------------------

_INSTALL_SH_BODY = r"""
SRC_DIR=$(cd "$(dirname "$0")" && pwd)

BUNDLE_ROOT=${K60_BUNDLE_ROOT:-__BUNDLE_ROOT__}
DATA_ROOT=${K60_DATA_ROOT:-__DATA_ROOT__}
UICACHE=${K60_UICACHE:-}
MOBILE_UID=${K60_UID:-__UID__}
MOBILE_GID=${K60_GID:-__GID__}
MCM_META=__MCM_META__

BUNDLE_DEST=$BUNDLE_ROOT/$CONTAINER_UUID
APP_PATH=$BUNDLE_DEST/$APP_NAME
DATA_DEST=$DATA_ROOT/$CONTAINER_UUID

DRY_RUN=0
FORCE=0
for arg in "$@"; do
	case $arg in
	--dry-run) DRY_RUN=1 ;;
	--force) FORCE=1 ;;
	-h|--help) sed -n '1,30p' "$0"; exit 0 ;;
	*) echo "k60install: 未知参数 $arg" >&2; exit 2 ;;
	esac
done

say() { printf 'k60install: %s\n' "$*"; }
die() { printf 'k60install: 错误: %s\n' "$*" >&2; exit 1; }
run() {
	if [ "$DRY_RUN" = 1 ]; then
		say "would run: $*"
	else
		"$@"
	fi
}

# 0. 先确认源树在（这是宿主打包出来的东西，缺了说明推错目录）
[ -d "$SRC_DIR/Bundle/$APP_NAME" ] || die "缺少 $SRC_DIR/Bundle/$APP_NAME"
[ -f "$SRC_DIR/metadata.plist" ] || die "缺少 $SRC_DIR/metadata.plist"

# 1. 目标是否已存在
if [ "$FORCE" != 1 ] && [ -e "$BUNDLE_DEST" ]; then
	die "$BUNDLE_DEST 已存在（换 --uuid 重打包，或加 --force 覆盖）"
fi

# 2. Bundle 容器
run mkdir -p "$BUNDLE_DEST"
run cp -a "$SRC_DIR/Bundle/$APP_NAME" "$BUNDLE_DEST/"
run cp -a "$SRC_DIR/Bundle/$MCM_META" "$BUNDLE_DEST/$MCM_META"

# 3. Data 容器骨架 + metadata
run mkdir -p "$DATA_DEST/Documents" "$DATA_DEST/Library/Preferences" "$DATA_DEST/Library/Caches" "$DATA_DEST/tmp"
run cp -a "$SRC_DIR/Data/." "$DATA_DEST/"
run cp -a "$SRC_DIR/metadata.plist" "$DATA_DEST/$MCM_META"

# 4. 属主（容器必须归 mobile，否则 App 一写就 EPERM）
if [ "$DRY_RUN" = 1 ]; then
	say "would run: chown -R $MOBILE_UID:$MOBILE_GID $BUNDLE_DEST $DATA_DEST"
	say "would run: chmod 755 $BUNDLE_DEST/$APP_NAME/$EXECUTABLE"
elif [ "$(id -u)" = 0 ]; then
	run chown -R "$MOBILE_UID:$MOBILE_GID" "$BUNDLE_DEST" "$DATA_DEST"
	run chmod 755 "$BUNDLE_DEST/$APP_NAME"
	run chmod 755 "$BUNDLE_DEST/$APP_NAME/$EXECUTABLE"
else
	say "警告: 当前不是 root，跳过 chown；属主不对时 App 会写不进自己的容器"
fi

# 5. 刷新 LaunchServices / 图标缓存
if [ -z "$UICACHE" ]; then
	for cand in __UICACHE_CANDIDATES__; do
		if [ -x "$cand" ]; then UICACHE=$cand; break; fi
	done
fi
if [ -n "$UICACHE" ] && [ -x "$UICACHE" ]; then
	say "uicache: $UICACHE"
	if [ "$DRY_RUN" = 1 ]; then
		say "would run: $UICACHE -p $APP_PATH"
	else
		"$UICACHE" -p "$APP_PATH" || "$UICACHE" -a || die "uicache 失败"
	fi
else
	say "警告: 没找到 uicache，图标不会出现"
	say "      找过: ${K60_UICACHE:-<K60_UICACHE 未设>} 以及 __UICACHE_CANDIDATES__"
	say "      先把 bootstrap 装到 /var/k60bootstrap，或用 K60_UICACHE=/path/to/uicache 指过去"
fi

if [ "${K60_RESPRING:-0}" = 1 ]; then
	run killall -9 SpringBoard lsd 2>/dev/null || true
fi

say "bundle id : $BUNDLE_ID"
say "container : $CONTAINER_UUID"
say "app       : $APP_PATH"
say "data      : $DATA_DEST"
if [ "$DRY_RUN" = 1 ]; then
	say "(dry-run：什么都没写)"
fi
exit 0
"""


def render_install_sh(*, bundle_id: str, app_name: str, container_uuid: str,
                      executable: str) -> str:
    header = (
        "#!/bin/sh\n"
        "# 由 tools/k60install/k60install.py 生成 —— 设备端安装脚本。\n"
        "#\n"
        "# 前置（上机时按这个清单核）:\n"
        "#   1. 在设备上以 root 跑（容器属主/权限要改）: sh install.sh\n"
        "#   2. uicache: 默认找 /var/k60bootstrap/usr/bin/uicache，其次\n"
        "#      /usr/bin/uicache、/var/jb/usr/bin/uicache；也可 K60_UICACHE=... 指定。\n"
        "#   3. /private/var/containers/Bundle/Application 与\n"
        "#      /private/var/mobile/Containers/Data/Application 必须存在（镜像里是空的）。\n"
        "#   4. 只建目录 + 写 metadata，不叫 installd；amfid 若被桩掉就不需要真签名。\n"
        "#\n"
        "# 用法: sh install.sh [--dry-run] [--force]\n"
        "#       K60_RESPRING=1 会额外 killall -9 SpringBoard lsd。\n"
        "\n"
        "set -eu\n"
        "\n"
        f"BUNDLE_ID={_shquote(bundle_id)}\n"
        f"APP_NAME={_shquote(app_name)}\n"
        f"CONTAINER_UUID={_shquote(container_uuid)}\n"
        f"EXECUTABLE={_shquote(executable)}\n"
    )
    body = (
        _INSTALL_SH_BODY
        .replace("__BUNDLE_ROOT__", BUNDLE_ROOT)
        .replace("__DATA_ROOT__", DATA_ROOT)
        .replace("__MCM_META__", MCM_METADATA_NAME)
        .replace("__UICACHE_CANDIDATES__", " ".join(UICACHE_CANDIDATES))
        .replace("__UID__", str(MOBILE_UID))
        .replace("__GID__", str(MOBILE_GID))
    )
    return header + body


def _shquote(s: str) -> str:
    return "'" + s.replace("'", "'\\''") + "'"


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def build(ipa: Path, out_root: Path, *, container_uuid: str | None = None,
          bundle_id_override: str | None = None, force: bool = False,
          sign: bool = False, ldid: str | None = None,
          entitlements: Path | None = None, verify: bool = False,
          quiet: bool = False) -> dict:
    """核心：IPA → out/<bundleid>/ 目录树。返回 manifest 字典。"""
    ipa = Path(ipa)
    if not ipa.is_file():
        raise InstallError(f"找不到 IPA: {ipa}")
    if not zipfile.is_zipfile(ipa):
        raise InstallError(f"不是 zip/IPA: {ipa}")

    ents_path = Path(entitlements) if entitlements else DEFAULT_ENTITLEMENTS
    if sign:
        if not ents_path.is_file():
            raise InstallError(f"entitlements 模板不存在: {ents_path}")
        try:
            plistlib.loads(ents_path.read_bytes())
        except Exception as exc:
            raise InstallError(f"entitlements 模板不是合法 plist: {exc}") from exc
        found = find_ldid(ldid)
        if not found:
            raise InstallError(LDID_HELP, code=3)
        ldid_path = found
    else:
        ldid_path = None

    with zipfile.ZipFile(ipa) as zf:
        app_name = find_app_bundle(zf)
        info = read_info_plist(zf, app_name)
        meta = bundle_info(info)
        bundle_id = bundle_id_override or meta["bundle_id"]
        if bundle_id_override:
            if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", bundle_id_override):
                raise InstallError(f"--bundle-id 非法: {bundle_id_override!r}")
            bundle_id = bundle_id_override

        if container_uuid:
            try:
                container_uuid = str(uuidlib.UUID(container_uuid)).upper()
            except ValueError:
                raise InstallError(f"--uuid 不是合法 UUID: {container_uuid}") from None
        else:
            container_uuid = str(uuidlib.uuid4()).upper()

    out_dir = out_root / bundle_id
    if out_dir.exists():
        if not force:
            raise InstallError(f"{out_dir} 已存在（要覆盖加 --force）")
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    try:
        return _materialise(
            ipa=ipa, out_dir=out_dir, app_name=app_name, meta=meta,
            bundle_id=bundle_id, container_uuid=container_uuid,
            sign=sign, ldid_path=ldid_path, ents_path=ents_path,
            verify=verify, quiet=quiet,
        )
    except BaseException:
        # 半成品会挡住下一次运行（除非 --force），失败就清干净
        shutil.rmtree(out_dir, ignore_errors=True)
        raise


def _materialise(*, ipa: Path, out_dir: Path, app_name: str, meta: dict,
                 bundle_id: str, container_uuid: str, sign: bool,
                 ldid_path: str | None, ents_path: Path, verify: bool,
                 quiet: bool) -> dict:
    """解包 + 写 plist + 签名 + 生成 install.sh；出错由 build() 负责清目录。"""
    bundle_dir = out_dir / "Bundle"
    app_dest = bundle_dir / app_name
    with zipfile.ZipFile(ipa) as zf:
        nfiles = extract_app(zf, app_name, app_dest)

    # 权限兜底：主程序必须可执行
    exec_path = app_dest / meta["executable"]
    if exec_path.is_file():
        os.chmod(exec_path, os.stat(exec_path).st_mode | 0o111)
    else:
        raise InstallError(
            f"CFBundleExecutable={meta['executable']} 指向的文件不在 app 里: {exec_path}"
        )

    # Data 容器骨架
    data_dir = out_dir / "Data"
    for sub in ("Documents", "Library/Preferences", "Library/Caches", "tmp"):
        (data_dir / sub).mkdir(parents=True, exist_ok=True)

    # metadata：数据容器放顶层 metadata.plist，Bundle 容器跟着 app 一起走
    write_plist(out_dir / "metadata.plist", build_data_metadata(bundle_id, container_uuid))
    write_plist(bundle_dir / MCM_METADATA_NAME,
                build_bundle_metadata(bundle_id, container_uuid))

    # 签名（I3）
    signed: list[str] = []
    if sign:
        signed = [str(p.relative_to(app_dest))
                  for p in sign_binaries(app_dest, ldid_path, ents_path.resolve())]

    # 回读 entitlements（替代 codesign -d）
    verified: dict[str, str] = {}
    if verify:
        for rel in (signed or [str(p.relative_to(app_dest)) for p in iter_macho_files(app_dest)]):
            ents = read_entitlements(app_dest / rel)
            if ents:
                verified[rel] = ents

    # install.sh
    sh_path = out_dir / "install.sh"
    sh_path.write_text(render_install_sh(
        bundle_id=bundle_id, app_name=app_name,
        container_uuid=container_uuid, executable=meta["executable"],
    ), encoding="utf-8")
    os.chmod(sh_path, 0o755)

    manifest = {
        "bundle_id": bundle_id,
        "app_name": app_name,
        "display_name": meta["display_name"],
        "executable": meta["executable"],
        "version": meta["short_version"],
        "build": meta["build_version"],
        "minimum_os": meta["minimum_os"],
        "container_uuid": container_uuid,
        "bundle_container": f"{BUNDLE_ROOT}/{container_uuid}",
        "app_path": f"{BUNDLE_ROOT}/{container_uuid}/{app_name}",
        "data_container": f"{DATA_ROOT}/{container_uuid}",
        "out_dir": str(out_dir),
        "files_extracted": nfiles,
        "signed": bool(sign),
        "ldid": ldid_path,
        "entitlements": str(ents_path.resolve()) if sign else None,
        "verified_entitlements": verified,
        "source_ipa": str(ipa),
    }
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    if not quiet:
        print(f"bundle id : {bundle_id}")
        print(f"app       : {app_name}  ({meta['display_name']} {meta['short_version']})")
        print(f"uuid      : {container_uuid}")
        print(f"out       : {out_dir}  ({nfiles} 个文件)")
        print(f"app path  : {manifest['app_path']}")
        print(f"data path : {manifest['data_container']}")
        if sign:
            print(f"ldid      : {ldid_path}")
            print(f"signed    : {len(signed)} 个 Mach-O  ({ents_path})")
        if verify:
            if verified:
                for rel, ents in verified.items():
                    keys = sorted(plistlib.loads(ents.encode()).keys()) if ents else []
                    print(f"verify    : {rel}  entitlements {len(keys)} 项: {', '.join(keys)}")
            else:
                print("verify    : 没读到 entitlements（未签名，或签名里没有 entitlement blob）")
        print(f"next      : 把 {out_dir} 推到设备后跑  sh install.sh --dry-run  先看一眼")
    return manifest


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="k60install.py",
        description="宿主侧 IPA → 设备容器目录树（INSTALLER-PLAN I2/I3）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("ipa", help="要处理的 .ipa")
    ap.add_argument("-o", "--out", default="out", help="输出根目录（默认 ./out）")
    ap.add_argument("--uuid", help="指定容器 UUID（默认随机 v4）")
    ap.add_argument("--bundle-id", help="覆盖 Info.plist 里的 CFBundleIdentifier")
    ap.add_argument("--force", action="store_true", help="输出目录已存在时覆盖")
    ap.add_argument("--sign", action="store_true", help="调 ldid 签 entitlements（I3）")
    ap.add_argument("--ldid", help="ldid 路径（默认: $K60_LDID / $LDID / PATH / bootstrap）")
    ap.add_argument("--entitlements", help=f"entitlements 模板（默认 {DEFAULT_ENTITLEMENTS.name}）")
    ap.add_argument("--verify", action="store_true",
                    help="签完回读 embedded entitlements（Linux 上代替 codesign -d）")
    ap.add_argument("--json", action="store_true", help="stdout 只输出 manifest JSON")
    ap.add_argument("-q", "--quiet", action="store_true", help="不打印摘要")
    args = ap.parse_args(argv)

    try:
        manifest = build(
            Path(args.ipa), Path(args.out),
            container_uuid=args.uuid,
            bundle_id_override=args.bundle_id,
            force=args.force,
            sign=args.sign,
            ldid=args.ldid,
            entitlements=Path(args.entitlements) if args.entitlements else None,
            verify=args.verify,
            quiet=args.quiet or args.json,
        )
    except InstallError as exc:
        msg = str(exc)
        sys.stderr.write(msg if msg.endswith("\n") else msg + "\n")
        return exc.code
    except OSError as exc:
        print(f"k60install: {exc}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(manifest, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
