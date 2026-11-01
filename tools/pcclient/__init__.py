"""pcclient - 宿主侧 iOS 协议编解码原型（不连接设备）。

模块:
    framing   usbmux 帧 + lockdown 请求/响应编解码
    plistmsg  lockdown / installation_proxy 的 plist 帧（4 字节大端长度前缀）
    afc       AFC 40 字节头 + 各 opcode 请求体
    instproxy mobile_installation_proxy 命令与状态

只做编解码，不打开 socket。`python3 tools/pcclient/selftest.py --selftest`
会把每个协议的示例帧打印成十六进制。
"""
