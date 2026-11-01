"""mobile_installation_proxy 命令与状态解析。

传输层与 lockdown 相同：4 字节大端长度 + XML plist。
一条命令发出去后，设备会连发若干条状态 plist，最后以 ``Status=Complete`` 收尾。

来源:
  * libimobiledevice src/installation_proxy.c
    （instproxy_install / instproxy_upgrade / instproxy_receive_status_loop /
      instproxy_status_get_error）
  * pymobiledevice3 pymobiledevice3/services/installation_proxy.py
    （send_package / install_from_local / _watch_completion）
"""

from __future__ import annotations

from typing import Any

SERVICE_NAME = "com.apple.mobile.installation_proxy"

CMD_INSTALL = "Install"
CMD_UPGRADE = "Upgrade"
CMD_UNINSTALL = "Uninstall"
CMD_BROWSE = "Browse"
CMD_LOOKUP = "Lookup"
CMD_ARCHIVE = "Archive"
CMD_RESTORE = "Restore"
CMD_REMOVE_ARCHIVE = "RemoveArchive"
CMD_LOOKUP_ARCHIVES = "LookupArchives"
CMD_CHECK_CAPABILITIES_MATCH = "CheckCapabilitiesMatch"


class InstProxyError(Exception):
    def __init__(self, name: str, description: str | None = None,
                 detail: int | None = None, response: Any = None) -> None:
        super().__init__(name if not description else f"{name}: {description}")
        self.name = name
        self.description = description
        self.detail = detail
        self.response = response


def _package_command(cmd: str, package_path: str,
                     options: dict[str, Any] | None) -> dict[str, Any]:
    command: dict[str, Any] = {"Command": cmd}
    if options is not None:
        command["ClientOptions"] = dict(options)
    command["PackagePath"] = package_path
    return command


def install(package_path: str, options: dict[str, Any] | None = None) -> dict[str, Any]:
    return _package_command(CMD_INSTALL, package_path, options)


def upgrade(package_path: str, options: dict[str, Any] | None = None) -> dict[str, Any]:
    return _package_command(CMD_UPGRADE, package_path, options)


def uninstall(bundle_id: str, options: dict[str, Any] | None = None) -> dict[str, Any]:
    command: dict[str, Any] = {"Command": CMD_UNINSTALL,
                              "ApplicationIdentifier": bundle_id}
    if options is not None:
        command["ClientOptions"] = dict(options)
    return command


def browse(options: dict[str, Any] | None = None) -> dict[str, Any]:
    command: dict[str, Any] = {"Command": CMD_BROWSE}
    if options is not None:
        command["ClientOptions"] = dict(options)
    return command


def lookup(bundle_ids: list[str] | None = None,
           options: dict[str, Any] | None = None) -> dict[str, Any]:
    """Lookup 命令。

    注意 ``BundleIDs`` 在 **ClientOptions 里面**，不是顶层键
    （libimobiledevice `instproxy_lookup()` 把 BundleIDs 塞进 client_options 再作为
    ClientOptions 发送；pymobiledevice3 同样）。
    """
    command: dict[str, Any] = {"Command": CMD_LOOKUP}
    client_options: dict[str, Any] = dict(options) if options else {}
    if bundle_ids:
        client_options["BundleIDs"] = list(bundle_ids)
    if client_options:
        command["ClientOptions"] = client_options
    return command


def status_name(response: dict[str, Any]) -> str | None:
    value = response.get("Status")
    return value if isinstance(value, str) else None


def percent_complete(response: dict[str, Any]) -> int | None:
    value = response.get("PercentComplete")
    return value if isinstance(value, int) else None


def error_of(response: dict[str, Any]) -> InstProxyError | None:
    name = response.get("Error")
    if not isinstance(name, str):
        return None
    detail = response.get("ErrorDetail")
    return InstProxyError(
        name,
        response.get("ErrorDescription"),
        detail if isinstance(detail, int) else None,
        response,
    )


def is_complete(response: dict[str, Any]) -> bool:
    return status_name(response) == "Complete"


def check(response: dict[str, Any]) -> None:
    """状态消息里带 Error 就抛。"""
    err = error_of(response)
    if err is not None:
        raise err
