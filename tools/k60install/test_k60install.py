#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""k60install 的单元测试（纯标准库 unittest，不碰设备、不联网）。

    cd tools/k60install && python3 -m unittest -v

测试用的 IPA 是当场造的: 一个 zip，里面
    Payload/Demo.app/Info.plist              （真 Info.plist，XML/二进制都测）
    Payload/Demo.app/Demo                    （假 Mach-O，带 mach_header_64）
    Payload/Demo.app/Frameworks/libdemo.dylib（第二个假 Mach-O，验"签全部"）
    Payload/Demo.app/Assets.car              （非 Mach-O，验"别乱签"）

ldid 用测试里现写的假脚本顶替（记录参数 + 把 entitlements 真塞进 Mach-O），
所以 I3 的"签完能读回 entitlements"在没装 ldid 的机器上也能验。
"""

from __future__ import annotations

import hashlib
import json
import os
import plistlib
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid as uuidlib
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import k60install  # noqa: E402

K60INSTALL = HERE / "k60install.py"


# ---------------------------------------------------------------------------
# 造假 Mach-O / 假签名
# ---------------------------------------------------------------------------

def _superblob(ents_xml: bytes) -> bytes:
    """CS_SuperBlob 里塞一个 CSSLOT_ENTITLEMENTS 条目。"""
    blob = struct.pack(">II", 0xFADE7171, 8 + len(ents_xml)) + ents_xml
    index_off = 20  # 12 字节头 + 一条 8 字节索引
    total = index_off + len(blob)
    return (struct.pack(">III", 0xFADE0CC0, total, 1)
            + struct.pack(">II", 5, index_off)
            + blob)


def make_macho(ents_xml: bytes | None = None, *, is64: bool = True) -> bytes:
    """造一个最小 Mach-O；ents_xml 不为空时带 LC_CODE_SIGNATURE。"""
    cputype = 0x0100000C  # CPU_TYPE_ARM64
    if ents_xml is None:
        ncmds, sizeofcmds, body = 0, 0, b""
    else:
        blob = _superblob(ents_xml)
        header_size = 32 if is64 else 28
        sizeofcmds = 16
        ncmds = 1
        body = struct.pack("<IIII", 0x1D, 16, header_size + sizeofcmds, len(blob)) + blob
    if is64:
        header = struct.pack("<IiiIIIII", 0xFEEDFACF, cputype, 0, 2,
                             ncmds, sizeofcmds, 0, 0)
    else:
        header = struct.pack("<IiiIIII", 0xFEEDFACE, cputype, 0, 2,
                             ncmds, sizeofcmds, 0)
    return header + body


def make_fat(thin: bytes) -> bytes:
    """把 thin 包成 fat（大端头，1 个架构）。"""
    offset = 4096
    header = (struct.pack(">II", 0xCAFEBABE, 1)
              + struct.pack(">IIIII", 0x0100000C, 0, offset, len(thin), 12))
    return header + b"\x00" * (offset - len(header)) + thin


# ---------------------------------------------------------------------------
# 造 IPA
# ---------------------------------------------------------------------------

DEFAULT_INFO = {
    "CFBundleIdentifier": "com.example.demo",
    "CFBundleExecutable": "Demo",
    "CFBundleName": "Demo",
    "CFBundleDisplayName": "Demo App",
    "CFBundleShortVersionString": "1.2.3",
    "CFBundleVersion": "45",
    "MinimumOSVersion": "16.0",
    "CFBundlePackageType": "APPL",
}


def _add(zf: zipfile.ZipFile, name: str, data: bytes, mode: int = 0o644) -> None:
    zi = zipfile.ZipInfo(name, date_time=(2026, 11, 1, 0, 0, 0))
    zi.external_attr = (mode & 0xFFFF) << 16
    zi.compress_type = zipfile.ZIP_DEFLATED
    zf.writestr(zi, data)


def _add_symlink(zf: zipfile.ZipFile, name: str, target: str) -> None:
    zi = zipfile.ZipInfo(name, date_time=(2026, 11, 1, 0, 0, 0))
    zi.external_attr = (0o120777 & 0xFFFF) << 16
    zi.compress_type = zipfile.ZIP_STORED
    zf.writestr(zi, target.encode())


def build_ipa(path: Path, *, info: dict | None = None, binary_plist: bool = False,
              app_name: str = "Demo.app", machos: int = 2,
              binary_name: str | None = None,
              with_symlink: bool = False, extra: dict[str, bytes] | None = None,
              payload: bool = True) -> Path:
    info = dict(DEFAULT_INFO if info is None else info)
    base = f"Payload/{app_name}/" if payload else f"{app_name}/"
    fmt = plistlib.FMT_BINARY if binary_plist else plistlib.FMT_XML
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as zf:
        _add(zf, base + "Info.plist", plistlib.dumps(info, fmt=fmt))
        _add(zf, base + (binary_name or info.get("CFBundleExecutable", "Demo")),
             make_macho(), 0o755)
        if machos >= 2:
            _add(zf, base + "Frameworks/libdemo.dylib", make_macho(), 0o755)
        if machos >= 3:
            _add(zf, base + "PlugIns/Ext.appex/Ext", make_macho(), 0o755)
        _add(zf, base + "Assets.car", b"\x00\x01\x02not a macho")
        _add(zf, base + "en.lproj/Localizable.strings", b'"hi" = "hi";\n')
        if with_symlink:
            _add_symlink(zf, base + "Frameworks/Current", "libdemo.dylib")
        for name, data in (extra or {}).items():
            _add(zf, name, data)
    return path


FAKE_LDID = r'''#!/usr/bin/env python3
"""假的 ldid: 记录参数，并把 entitlements 真塞进目标 Mach-O（供 --verify 回读）。"""
import os, struct, sys

log = os.environ.get("FAKE_LDID_LOG")
if log:
    with open(log, "a") as fp:
        fp.write(" ".join(sys.argv[1:]) + "\n")

args = sys.argv[1:]
ents_path = None
target = None
for a in args:
    if a.startswith("-S"):
        ents_path = a[2:] or None
    elif not a.startswith("-"):
        target = a
if not target or not ents_path:
    sys.exit(9)
if not os.path.isfile(ents_path):
    sys.exit(8)

ents = open(ents_path, "rb").read()
blob = struct.pack(">II", 0xFADE7171, 8 + len(ents)) + ents
sb = struct.pack(">III", 0xFADE0CC0, 20 + len(blob), 1) + struct.pack(">II", 5, 20) + blob
header = struct.pack("<IiiIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, 1, 16, 0, 0)
lc = struct.pack("<IIII", 0x1D, 16, 48, len(sb))
mode = os.stat(target).st_mode
with open(target, "wb") as fp:
    fp.write(header + lc + sb)
os.chmod(target, mode)
'''


class Base(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="k60install-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.ipa = build_ipa(self.tmp / "in" / "Demo.ipa")
        self.out = self.tmp / "out"

    # --- helpers ---------------------------------------------------------
    def run_cli(self, *args: str, env: dict | None = None) -> subprocess.CompletedProcess:
        e = dict(os.environ)
        e.pop("K60_LDID", None)
        e.pop("LDID", None)
        if env:
            e.update(env)
        return subprocess.run(
            [sys.executable, str(K60INSTALL), *args],
            capture_output=True, text=True, env=e, timeout=120,
        )

    def build(self, **kw):
        kw.setdefault("out_root", self.out)
        kw.setdefault("quiet", True)
        return k60install.build(self.ipa, **kw)

    @property
    def out_dir(self) -> Path:
        return self.out / "com.example.demo"

    def write_fake_ldid(self) -> Path:
        p = self.tmp / "bin" / "ldid"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(FAKE_LDID, encoding="utf-8")
        p.chmod(0o755)
        return p


# ---------------------------------------------------------------------------
# I2: 目录结构 / plist / bundle id
# ---------------------------------------------------------------------------

class TestLayout(Base):
    def test_output_tree(self) -> None:
        self.build()
        o = self.out_dir
        self.assertTrue(o.is_dir(), "输出目录名应该是 bundle id")
        for rel in ("Bundle/Demo.app/Info.plist",
                    "Bundle/Demo.app/Demo",
                    "Bundle/Demo.app/Frameworks/libdemo.dylib",
                    "Bundle/Demo.app/Assets.car",
                    "Data/Documents",
                    "Data/Library/Preferences",
                    "Data/tmp",
                    "metadata.plist",
                    "install.sh",
                    "manifest.json"):
            self.assertTrue((o / rel).exists(), f"缺 {rel}")
        self.assertTrue((o / "Bundle" / k60install.MCM_METADATA_NAME).is_file())
        self.assertTrue((o / "Bundle/Demo.app/Demo").stat().st_mode & stat.S_IXUSR,
                        "主程序丢了可执行位")
        self.assertFalse(k60install.is_macho(o / "Bundle/Demo.app/Assets.car"))
        self.assertTrue(k60install.is_macho(o / "Bundle/Demo.app/Demo"))

    def test_data_skeleton_is_empty(self) -> None:
        self.build()
        self.assertEqual(list((self.out_dir / "Data/Documents").iterdir()), [])
        self.assertEqual(list((self.out_dir / "Data/tmp").iterdir()), [])

    def test_symlink_preserved(self) -> None:
        ipa = build_ipa(self.tmp / "in" / "Link.ipa", with_symlink=True)
        k60install.build(ipa, self.out, quiet=True)
        link = self.out / "com.example.demo/Bundle/Demo.app/Frameworks/Current"
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.readlink(link), "libdemo.dylib")

    def test_path_traversal_rejected(self) -> None:
        ipa = build_ipa(self.tmp / "in" / "Evil.ipa",
                        extra={"Payload/Demo.app/../../evil.txt": b"pwn"})
        with self.assertRaises(k60install.InstallError) as ctx:
            k60install.build(ipa, self.out, quiet=True)
        self.assertIn("穿越", str(ctx.exception))
        self.assertFalse((self.tmp / "evil.txt").exists())


class TestBundleId(Base):
    def test_bundle_id_from_xml_info_plist(self) -> None:
        m = self.build()
        self.assertEqual(m["bundle_id"], "com.example.demo")
        self.assertEqual(m["app_name"], "Demo.app")
        self.assertEqual(m["executable"], "Demo")
        self.assertEqual(m["display_name"], "Demo App")
        self.assertEqual(m["version"], "1.2.3")

    def test_bundle_id_from_binary_info_plist(self) -> None:
        ipa = build_ipa(self.tmp / "in" / "Bin.ipa",
                        info={**DEFAULT_INFO, "CFBundleIdentifier": "org.k60.binary"},
                        binary_plist=True)
        m = k60install.build(ipa, self.out, quiet=True)
        self.assertEqual(m["bundle_id"], "org.k60.binary")
        self.assertTrue((self.out / "org.k60.binary/metadata.plist").is_file())

    def test_bundle_id_override(self) -> None:
        m = self.build(bundle_id_override="com.example.renamed")
        self.assertEqual(m["bundle_id"], "com.example.renamed")
        self.assertTrue((self.out / "com.example.renamed").is_dir())

    def test_missing_bundle_id_errors(self) -> None:
        info = {k: v for k, v in DEFAULT_INFO.items() if k != "CFBundleIdentifier"}
        ipa = build_ipa(self.tmp / "in" / "NoId.ipa", info=info)
        with self.assertRaises(k60install.InstallError) as ctx:
            k60install.build(ipa, self.out, quiet=True)
        self.assertIn("CFBundleIdentifier", str(ctx.exception))

    def test_missing_executable_errors(self) -> None:
        # 二进制还叫 Demo，但 Info.plist 指到不存在的 Nope
        ipa = build_ipa(self.tmp / "in" / "NoExec.ipa",
                        info={**DEFAULT_INFO, "CFBundleExecutable": "Nope"},
                        binary_name="Demo")
        with self.assertRaises(k60install.InstallError) as ctx:
            k60install.build(ipa, self.out, quiet=True)
        self.assertIn("Nope", str(ctx.exception))

    def test_no_payload_errors(self) -> None:
        ipa = build_ipa(self.tmp / "in" / "Flat.ipa", payload=False)
        with self.assertRaises(k60install.InstallError) as ctx:
            k60install.build(ipa, self.out, quiet=True)
        self.assertIn("Payload", str(ctx.exception))

    def test_two_app_bundles_rejected(self) -> None:
        ipa = build_ipa(self.tmp / "in" / "Two.ipa",
                        extra={"Payload/Other.app/Info.plist":
                               plistlib.dumps({**DEFAULT_INFO,
                                               "CFBundleIdentifier": "com.example.other"})})
        with self.assertRaises(k60install.InstallError) as ctx:
            k60install.build(ipa, self.out, quiet=True)
        self.assertIn("多个", str(ctx.exception))

    def test_unsafe_bundle_id_rejected(self) -> None:
        info = {**DEFAULT_INFO, "CFBundleIdentifier": "com.example.x; rm -rf /"}
        ipa = build_ipa(self.tmp / "in" / "Semi.ipa", info=info)
        with self.assertRaises(k60install.InstallError) as ctx:
            k60install.build(ipa, self.out, quiet=True)
        self.assertIn("非法字符", str(ctx.exception))

    def test_existing_output_needs_force(self) -> None:
        self.build()
        with self.assertRaises(k60install.InstallError) as ctx:
            self.build()
        self.assertIn("--force", str(ctx.exception))
        self.build(force=True)  # 不抛就算过


class TestMetadata(Base):
    def test_data_metadata_fields(self) -> None:
        m = self.build(container_uuid="6f9c1b1e-1111-4222-8333-444455556666")
        meta = plistlib.loads((self.out_dir / "metadata.plist").read_bytes())
        self.assertEqual(meta["MCMMetadataIdentifier"], "com.example.demo")
        self.assertEqual(meta["MCMMetadataUUID"], "6F9C1B1E-1111-4222-8333-444455556666")
        self.assertEqual(meta["MCMMetadataContentClass"], k60install.MCM_CLASS_DATA)
        self.assertEqual(meta["MCMMetadataContentClass"], 2)
        self.assertEqual(meta["MCMMetadataSchemaVersion"], 1)
        self.assertEqual(meta["MCMMetadataPersona"], 501)
        self.assertEqual(
            meta["MCMMetadataInfo"]["com.apple.MobileInstallation.ContentProtectionClass"],
            -1)
        self.assertEqual(m["container_uuid"], "6F9C1B1E-1111-4222-8333-444455556666")

    def test_bundle_metadata_class(self) -> None:
        self.build(container_uuid="6f9c1b1e-1111-4222-8333-444455556666")
        meta = plistlib.loads(
            (self.out_dir / "Bundle" / k60install.MCM_METADATA_NAME).read_bytes())
        self.assertEqual(meta["MCMMetadataContentClass"], 1)
        self.assertEqual(meta["MCMMetadataIdentifier"], "com.example.demo")
        self.assertEqual(meta["MCMMetadataUUID"], "6F9C1B1E-1111-4222-8333-444455556666")

    def test_uuid_defaults_to_v4(self) -> None:
        m = self.build()
        u = uuidlib.UUID(m["container_uuid"])
        self.assertEqual(u.version, 4)
        self.assertEqual(str(u).upper(), m["container_uuid"])

    def test_uuid_override_normalised(self) -> None:
        m = self.build(container_uuid="6f9c1b1e-1111-4222-8333-444455556666")
        self.assertEqual(m["container_uuid"], "6F9C1B1E-1111-4222-8333-444455556666")
        self.assertEqual(m["bundle_container"],
                         "/private/var/containers/Bundle/Application/"
                         "6F9C1B1E-1111-4222-8333-444455556666")
        self.assertEqual(m["data_container"],
                         "/private/var/mobile/Containers/Data/Application/"
                         "6F9C1B1E-1111-4222-8333-444455556666")

    def test_bad_uuid_rejected(self) -> None:
        with self.assertRaises(k60install.InstallError) as ctx:
            self.build(container_uuid="not-a-uuid")
        self.assertIn("UUID", str(ctx.exception))

    def test_manifest_written(self) -> None:
        self.build()
        m = json.loads((self.out_dir / "manifest.json").read_text())
        self.assertEqual(m["bundle_id"], "com.example.demo")
        self.assertEqual(m["app_path"].rsplit("/", 1)[-1], "Demo.app")
        self.assertFalse(m["signed"])


# ---------------------------------------------------------------------------
# install.sh
# ---------------------------------------------------------------------------

class TestInstallSh(Base):
    def test_shell_syntax(self) -> None:
        self.build()
        if not shutil.which("sh"):
            self.skipTest("没有 sh")
        r = subprocess.run(["sh", "-n", str(self.out_dir / "install.sh")],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_contents(self) -> None:
        self.build(container_uuid="6f9c1b1e-1111-4222-8333-444455556666")
        text = (self.out_dir / "install.sh").read_text()
        self.assertIn("BUNDLE_ID='com.example.demo'", text)
        self.assertIn("APP_NAME='Demo.app'", text)
        self.assertIn("CONTAINER_UUID='6F9C1B1E-1111-4222-8333-444455556666'", text)
        self.assertIn("/private/var/containers/Bundle/Application", text)
        self.assertIn("/private/var/mobile/Containers/Data/Application", text)
        self.assertIn(k60install.MCM_METADATA_NAME, text)
        self.assertIn("/var/k60bootstrap/usr/bin/uicache", text)
        self.assertIn('"$UICACHE" -p "$APP_PATH"', text)
        self.assertTrue(os.access(self.out_dir / "install.sh", os.X_OK))

    def test_dry_run_writes_nothing(self) -> None:
        self.build()
        b_root = self.tmp / "device/Bundle/Application"
        d_root = self.tmp / "device/Data/Application"
        r = subprocess.run(
            ["sh", "install.sh", "--dry-run"],
            cwd=str(self.out_dir), capture_output=True, text=True,
            env={**os.environ, "K60_BUNDLE_ROOT": str(b_root),
                 "K60_DATA_ROOT": str(d_root)},
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("would run:", r.stdout)
        self.assertIn("dry-run", r.stdout)
        self.assertIn("uicache", r.stdout)   # 找不到 uicache 要有明确警告
        self.assertFalse(b_root.exists(), "dry-run 不该建目录")
        self.assertFalse(d_root.exists())

    def test_unknown_arg_fails(self) -> None:
        self.build()
        r = subprocess.run(["sh", "install.sh", "--nope"], cwd=str(self.out_dir),
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)


# ---------------------------------------------------------------------------
# I3: 签名 + 回读 entitlements
# ---------------------------------------------------------------------------

class TestSigning(Base):
    def test_missing_ldid_has_install_hint(self) -> None:
        from unittest import mock
        with mock.patch.object(k60install, "find_ldid", return_value=None):
            with self.assertRaises(k60install.InstallError) as ctx:
                self.build(sign=True)
        msg = str(ctx.exception)
        self.assertEqual(ctx.exception.code, 3)
        self.assertIn("ldid", msg)
        self.assertIn("yay -S ldid", msg)
        self.assertIn("K60_LDID", msg)
        self.assertFalse((self.out_dir / "Bundle").exists(), "缺 ldid 不该留下半成品")

    def test_cli_exit_code_3_without_ldid(self) -> None:
        r = self.run_cli(str(self.ipa), "-o", str(self.out),
                         "--sign", "--ldid", str(self.tmp / "nope/ldid"),
                         env={"PATH": "/nonexistent"})
        self.assertEqual(r.returncode, 3, r.stderr)
        self.assertIn("ldid", r.stderr)

    def test_sign_calls_ldid_for_every_macho(self) -> None:
        fake = self.write_fake_ldid()
        log = self.tmp / "ldid.log"
        os.environ["FAKE_LDID_LOG"] = str(log)
        self.addCleanup(os.environ.pop, "FAKE_LDID_LOG", None)
        ipa = build_ipa(self.tmp / "in" / "Three.ipa", machos=3)
        m = k60install.build(ipa, self.out, sign=True, ldid=str(fake), quiet=True)
        self.assertTrue(m["signed"])
        lines = log.read_text().strip().splitlines()
        self.assertEqual(len(lines), 3, lines)
        joined = "\n".join(lines)
        self.assertIn("Demo.app/Demo", joined)
        self.assertIn("Frameworks/libdemo.dylib", joined)
        self.assertIn("PlugIns/Ext.appex/Ext", joined)
        self.assertNotIn("Assets.car", joined)
        ents = str((HERE / "entitlements.xml").resolve())
        for line in lines:
            self.assertTrue(line.startswith(f"-S{ents} "), line)

    def test_verify_reads_entitlements_back(self) -> None:
        fake = self.write_fake_ldid()
        m = k60install.build(self.ipa, self.out, sign=True, ldid=str(fake),
                             verify=True, quiet=True)
        self.assertIn("Demo", m["verified_entitlements"])
        ents = plistlib.loads(m["verified_entitlements"]["Demo"].encode())
        self.assertIs(ents["platform-application"], True)
        self.assertIs(ents["get-task-allow"], True)
        raw = (self.out_dir / "Bundle/Demo.app/Demo").read_bytes()
        self.assertIn(struct.pack(">I", 0xFADE0CC0), raw)

    def test_cli_verify_prints_keys(self) -> None:
        fake = self.write_fake_ldid()
        r = self.run_cli(str(self.ipa), "-o", str(self.out), "--sign",
                         "--ldid", str(fake), "--verify")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("verify", r.stdout)
        self.assertIn("platform-application", r.stdout)

    def test_no_sign_leaves_binary_untouched(self) -> None:
        self.build()
        binary = self.out_dir / "Bundle/Demo.app/Demo"
        before = hashlib.sha256(binary.read_bytes()).hexdigest()
        self.assertIsNone(k60install.read_entitlements(binary))
        self.assertEqual(before, hashlib.sha256(binary.read_bytes()).hexdigest())

    def test_custom_entitlements_file(self) -> None:
        fake = self.write_fake_ldid()
        custom = self.tmp / "custom.xml"
        custom.write_bytes(plistlib.dumps({"get-task-allow": True},
                                          fmt=plistlib.FMT_XML))
        k60install.build(self.ipa, self.out, sign=True, ldid=str(fake),
                         entitlements=custom, verify=True, quiet=True)
        xml = k60install.read_entitlements(self.out_dir / "Bundle/Demo.app/Demo")
        self.assertEqual(list(plistlib.loads(xml.encode())), ["get-task-allow"])

    def test_broken_entitlements_rejected(self) -> None:
        bad = self.tmp / "bad.xml"
        bad.write_text("<plist><dict><key>oops</key>")
        fake = self.write_fake_ldid()
        with self.assertRaises(k60install.InstallError) as ctx:
            k60install.build(self.ipa, self.out, sign=True, ldid=str(fake),
                             entitlements=bad)
        self.assertIn("plist", str(ctx.exception))

    def test_ldid_failure_surfaces(self) -> None:
        bad_ldid = self.tmp / "bin" / "ldid-fail"
        bad_ldid.parent.mkdir(parents=True, exist_ok=True)
        bad_ldid.write_text("#!/bin/sh\necho 'boom' >&2\nexit 7\n")
        bad_ldid.chmod(0o755)
        with self.assertRaises(k60install.InstallError) as ctx:
            k60install.build(self.ipa, self.out, sign=True, ldid=str(bad_ldid), quiet=True)
        self.assertIn("rc=7", str(ctx.exception))
        self.assertIn("boom", str(ctx.exception))
        # 失败要把半成品清掉，否则下次得 --force
        self.assertFalse((self.out / "com.example.demo").exists())

    def test_find_ldid_prefers_explicit(self) -> None:
        fake = self.write_fake_ldid()
        self.assertEqual(k60install.find_ldid(str(fake)), str(fake.resolve()))

    def test_find_ldid_from_env(self) -> None:
        fake = self.write_fake_ldid()
        old = os.environ.get("K60_LDID")
        os.environ["K60_LDID"] = str(fake)
        try:
            self.assertEqual(k60install.find_ldid(), str(fake.resolve()))
        finally:
            if old is None:
                os.environ.pop("K60_LDID", None)
            else:
                os.environ["K60_LDID"] = old

    def test_entitlements_template_is_sane(self) -> None:
        ents = plistlib.loads((HERE / "entitlements.xml").read_bytes())
        self.assertIs(ents["platform-application"], True)
        self.assertIs(ents["get-task-allow"], True)
        self.assertIs(ents["com.apple.private.security.no-sandbox"], True)
        self.assertIn(
            "/", ents["com.apple.security.exception.files.absolute-path.read-write"])
        self.assertIn("kTCCServiceCamera", ents["com.apple.private.tcc.allow"])


# ---------------------------------------------------------------------------
# Mach-O entitlements 解析器（32 位 / fat / 无签名）
# ---------------------------------------------------------------------------

class TestReadEntitlements(Base):
    def _write(self, data: bytes) -> Path:
        p = self.tmp / "bin"
        p.write_bytes(data)
        return p

    def test_thin64(self) -> None:
        xml = plistlib.dumps({"a": 1}, fmt=plistlib.FMT_XML)
        p = self._write(make_macho(xml))
        self.assertEqual(k60install.read_entitlements(p).strip(), xml.decode().strip())

    def test_thin32(self) -> None:
        xml = plistlib.dumps({"b": True}, fmt=plistlib.FMT_XML)
        p = self._write(make_macho(xml, is64=False))
        self.assertIn("<key>b</key>", k60install.read_entitlements(p))

    def test_fat(self) -> None:
        xml = plistlib.dumps({"c": "d"}, fmt=plistlib.FMT_XML)
        p = self._write(make_fat(make_macho(xml)))
        self.assertIn("<key>c</key>", k60install.read_entitlements(p))

    def test_unsigned(self) -> None:
        p = self._write(make_macho())
        self.assertIsNone(k60install.read_entitlements(p))

    def test_garbage(self) -> None:
        p = self._write(b"\x00" * 64)
        self.assertIsNone(k60install.read_entitlements(p))
        self.assertFalse(k60install.is_macho(p))

    def test_is_macho_fat(self) -> None:
        p = self._write(make_fat(make_macho()))
        self.assertTrue(k60install.is_macho(p))


# ---------------------------------------------------------------------------
# CLI 行为
# ---------------------------------------------------------------------------

class TestCli(Base):
    def test_ok_and_summary(self) -> None:
        r = self.run_cli(str(self.ipa), "-o", str(self.out))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("com.example.demo", r.stdout)
        self.assertIn("install.sh --dry-run", r.stdout)

    def test_json_output_is_machine_readable(self) -> None:
        r = self.run_cli(str(self.ipa), "-o", str(self.out), "--json")
        self.assertEqual(r.returncode, 0, r.stderr)
        m = json.loads(r.stdout)
        self.assertEqual(m["bundle_id"], "com.example.demo")
        self.assertEqual(m["app_name"], "Demo.app")

    def test_missing_ipa(self) -> None:
        r = self.run_cli(str(self.tmp / "nope.ipa"), "-o", str(self.out))
        self.assertEqual(r.returncode, 2)
        self.assertIn("找不到 IPA", r.stderr)

    def test_not_a_zip(self) -> None:
        junk = self.tmp / "junk.ipa"
        junk.write_bytes(b"not a zip at all")
        r = self.run_cli(str(junk), "-o", str(self.out))
        self.assertEqual(r.returncode, 2)
        self.assertIn("zip", r.stderr)

    def test_quiet(self) -> None:
        r = self.run_cli(str(self.ipa), "-o", str(self.out), "-q")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
