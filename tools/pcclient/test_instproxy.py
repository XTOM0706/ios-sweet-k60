"""instproxy.py 的单元测试。"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import instproxy
import plistmsg


class TestCommands(unittest.TestCase):
    def test_install(self):
        cmd = instproxy.install("/PublicStaging/a.ipa", {"PackageType": "Developer"})
        self.assertEqual(cmd["Command"], "Install")
        self.assertEqual(cmd["PackagePath"], "/PublicStaging/a.ipa")
        self.assertEqual(cmd["ClientOptions"], {"PackageType": "Developer"})

    def test_upgrade_without_options(self):
        cmd = instproxy.upgrade("/PublicStaging/a.ipa")
        self.assertEqual(cmd["Command"], "Upgrade")
        self.assertNotIn("ClientOptions", cmd)

    def test_uninstall_uses_application_identifier(self):
        cmd = instproxy.uninstall("com.example.app")
        self.assertEqual(cmd["Command"], "Uninstall")
        self.assertEqual(cmd["ApplicationIdentifier"], "com.example.app")

    def test_lookup_bundle_ids(self):
        cmd = instproxy.lookup(["com.a", "com.b"])
        # BundleIDs 必须嵌在 ClientOptions 里，不是顶层键
        self.assertNotIn("BundleIDs", cmd)
        self.assertEqual(cmd["ClientOptions"]["BundleIDs"], ["com.a", "com.b"])

    def test_lookup_without_options_has_no_client_options(self):
        cmd = instproxy.lookup()
        self.assertEqual(cmd, {"Command": "Lookup"})

    def test_options_are_copied_not_aliased(self):
        options = {"PackageType": "Developer"}
        cmd = instproxy.install("/x.ipa", options)
        options["PackageType"] = "Customer"
        self.assertEqual(cmd["ClientOptions"]["PackageType"], "Developer")

    def test_command_survives_plist_roundtrip(self):
        cmd = instproxy.install("/PublicStaging/a.ipa",
                                {"PackageType": "Developer", "IsUserInitiated": True})
        decoded, _ = plistmsg.decode_frame(plistmsg.encode(cmd))
        self.assertEqual(decoded, cmd)


class TestStatus(unittest.TestCase):
    def test_progress_message(self):
        msg = {"Status": "CreatingStagingDirectory", "PercentComplete": 5}
        self.assertEqual(instproxy.status_name(msg), "CreatingStagingDirectory")
        self.assertEqual(instproxy.percent_complete(msg), 5)
        self.assertFalse(instproxy.is_complete(msg))

    def test_complete_message(self):
        self.assertTrue(instproxy.is_complete({"Status": "Complete"}))

    def test_message_without_status(self):
        msg = {"CurrentList": [{"CFBundleIdentifier": "com.a"}], "CurrentAmount": 1}
        self.assertIsNone(instproxy.status_name(msg))
        self.assertIsNone(instproxy.percent_complete(msg))

    def test_error_detection(self):
        msg = {"Error": "ApplicationVerificationFailed",
               "ErrorDescription": "bad signature",
               "ErrorDetail": 3892346893}
        err = instproxy.error_of(msg)
        self.assertIsNotNone(err)
        assert err is not None
        self.assertEqual(err.name, "ApplicationVerificationFailed")
        self.assertEqual(err.description, "bad signature")
        self.assertEqual(err.detail, 3892346893)

    def test_check_raises(self):
        with self.assertRaises(instproxy.InstProxyError) as ctx:
            instproxy.check({"Error": "APIInternalError"})
        self.assertEqual(ctx.exception.name, "APIInternalError")

    def test_check_passes_on_progress(self):
        instproxy.check({"Status": "InstallingPackage", "PercentComplete": 60})


if __name__ == "__main__":
    unittest.main(verbosity=2)
