# USB gadget 到 usbmux：现状、差距与分期

日期：2026-11-01
对象：K60（Redmi K60 / mondrian / SM8475），内核 xnu-8796 + `xnu/patches/`
参考：`~/hackios/refs/data-a-live`（pandora，小米 17 Pro / SM8850）、
`~/hackios/refs/UniXNU-pong`（另一块 SM8550 级板子的设计稿）

本文只做评估，不改代码、不 commit、不碰设备。

---

## 0. 结论

**能延伸，而且起点比 README 写的高；但在 K60 上第一步仍然是"把链路点亮"，不是写 mux。**

三句话：

1. pandora 已经有一份**在真机上跑通的 DWC3 设备模式实现**：CDC-ACM 串口，
   180 秒 1.5 MB 内核日志能实时到 PC，且可做到与写盘副本逐字节一致
   （usb4 只少重启前 1 ms 的两行，usb5 完全一致；`PLAN.md:587-588`）。
   另外还留着一套更早的 CDC-ECM（在 sweet 上 `ping` 通，`NOTES.md:32`）。
   控制器、ep0、端点、DMA、事件轮询这一层不用从零写。
2. 从"枚举成串口/网卡"到"macOS 认成 iPhone 并跑 usbmux"，差的**不是控制器驱动**，
   而是三件事：Apple 描述符（`0xFF/0xFE/0x02` 的 bulk 接口）、设备侧 mux 传输、
   配对信任。其中 mux 有可能直接复用 iOS 自带的 `usbmuxd` + `AppleUSBDeviceMux`。
3. 我们 K60 上这套代码**从没上机**：`0143` 因为 PHY 地址不对把探测关掉，
   `0148` 改了地址但没有任何启动记录。所以"内核里没有 USB 数据通道"在 K60 上
   目前仍然成立——**代码在，链路不在**。

量级：到"macOS 枚举成 Apple 设备 + usbmux 列出设备"约 **4–7 人周**（复用
`AppleUSBDeviceMux`）或 **7–10 人周**（自己写 mux）；到 Finder/爱思可用再加
**4–7 人周**，主要卡在配对（没有 SEP）。

### 0.1 我们缺的 5 件事

| # | 缺口 | 现状 | 要补什么 | 证据 |
| --- | --- | --- | --- | --- |
| 1 | **K60 板级点亮** | 代码在，0 次上机；探测被 `0143` 默认关掉 | 先只读 probe 不崩，再 `sweetusb=2` 枚举；核对 GCC 时钟/PD、eUSB2 PHY、VBUS、SMMU 路由 | `xnu/patches/0143-*.patch`、`0148-*.patch` |
| 2 | **Apple 描述符** | `1209:0001`（ECM）/ `1209:316d`（ACM），CDC 类 | `05AC:12A8`、Apple 字符串、`0xFF/0xFE/0x02` 接口 + 一对 bulk 端点 | `0075-sweet-usb-ecm.patch`、`qcomusb/soc/Sm8850.h:24-27` |
| 3 | **设备侧 mux 传输** | 完全没有 | 复用 iOS 的 `AppleUSBDeviceMux`（需 IOKit nub），或自写 mux 帧 + 最小 TCP 状态机 | `root-hfsx.img` 内 `usbmuxd` 字符串；usbmuxd `src/device.c` |
| 4 | **端点/事件/DMA 工程化** | 每端点 1 个 TRB、事件中断屏蔽、单线程单 CPU 轮询 | 多 TRB 环、ZLP、CLEAR_HALT、流控、事件中断、按方向 cache 维护 | `0073`、`qcomusb/usb/Dwc3.c:524-538`、`wsl/pandora_usb.c:117,159-164,190` |
| 5 | **配对与 lockdown 信任** | 无 SEP/keybag | pair record 桩：软件证书 + 永远接受；不行就自写瘦客户端 | `docs/PC-TOOL-INSTALL-EVALUATION.md:45-56` |

### 0.2 第一个可上机验证的里程碑

**`fastboot boot` 一版 `sweetusbprobe=1 sweetusb=2` 的内核，PC 上 `lsusb` 出现
`1209:0001`，`tools/sweet-usbnet.sh` 能 `ping 192.168.7.2`。**

验收标准（全过才算过）：

- `dmesg` 出现 `cdc_ether` 绑定，接口 MAC 为 `02:00:5e:77:ee:01`；
- `ping 192.168.7.2` 1000 包 0 丢，往返稳定在毫秒级；
- 连续拔插 10 次，每次都能重新枚举；
- 串口日志里 `SWEET USB: running (DSTS 0x...)` 的 DSTS 显示 High-Speed、
  设备模式、`DEVCTRLHLT` 已清。

这一步不碰 Apple 描述符、不碰 mux，只证明**时钟/PD、DWC3 设备模式、VBUS
override、ep0 枚举、bulk 端点、DMA 一致性、SMMU 路由**这条全链路在我们板上成立。
后面所有工作都建立在它上面。失败时按 `0143` 的做法逐段二分（先只读 probe，
再单独开 gadget）。

---

## 1. 先校正三个前提

1. **pandora 的 USB 不止"正在做串口"。** README（`README.md:25`）写"数据功能还没做"，
   但 `PLAN.md:585-591` 的 usb1–usb11 记录显示串口已经验收：usb4 传了 1,525,912 字节，
   只少重启前 1 ms 的两行；usb5 的 1,544,638 字节完全一致。仓库里还有一套更早的
   CDC-ECM（`0075-0081`），在 sweet 上 `ping` 通。
2. **我们不是"零代码"。** `k60-repo-full` 里同样有 `0072-0081` 全套（gadget +
   ECM + UDP 日志 + link state）。它们在 sweet（SM7150，6 核）上验过；
   K60 上因为 PHY 基址不同被 `0143` 关掉，`0148` 只改了基址和两路 RPMh 电，
   没有上机记录。`NOTES.md:30-34` 的 `ping` 成功记录属于 sweet，不能算到 K60 头上。
3. **iOS 侧的 mux 协议层是现成的。** `iosroot/root-hfsx.img` 里同时有：
   - `usbmuxd` 二进制：字符串包含 `AppleUSBDeviceMux`、
     `Failed to create IOKit matching dictionary for 'AppleUSBDeviceMux'`、
     `IOUSBDeviceInterface`、`USBDeviceFunction`、`IOUSBDeviceControllerForceOffBus`、
     `we failed to create an IOUSBDeviceController`；
   - `/etc/group` 里的 `_usbmuxd:*:213:`。

   这说明设备侧 `usbmuxd` 通过 IOKit 找 `AppleUSBDeviceMux` 服务、用
   `IOUSBDeviceController` 拿 USB 通道，**mux/TCP 那两千行不用我们写**——前提是
   这个驱动在 iPhone X 内核缓存里、且能在我们的 xnu 上加载（离线可查，见 §8）。

---

## 2. pandora 做到哪

### 2.1 控制器与 PHY

- **Synopsys DWC3** @ `0xa600000`，USB 2.0 设备模式（高速）。
  不是 QUSB2 控制器、也不是 DWC3-QMP（QMP 是 USB3/DP combo PHY，这里没用）。
- Qualcomm `dwc3-msm` 外壳：QSCRATCH @ `0xa6f8000`（`0xf8800` 偏移）。
- PHY：SM8850 是 **eUSB2** @ `0x88e3000`。pandora 的模块**完全不碰 PHY**
  （`Sm8850.h:64` 写明"没有 prepare 阶段，`QUD_PHY_BASE` 保持 0"），
  全靠 bootloader 交接时已经配好。
- 交接状态（usb1 实测）：5 个 USB 时钟全开、`USB30_PRIM` GDSC 开、复位没按住、
  DWC3 在设备模式且已停（`RUN_STOP=0`、`DEVCTRLHLT=1`）。见 `qcomusb/README.md:27-31`。

证据：`wsl/qcomusb/soc/Sm8850.h:29-35,40-48,64`；`wsl/pandora_usb.c:51-55`；
`PLAN.md:585`。

### 2.2 两套实现，不要混

| | 自有 gadget（`bsd/dev/sweet_usb.c`） | 引入模块（`bsd/dev/qcomusb`） |
| --- | --- | --- |
| 补丁 | `0072-0081` | `0248-0253`（0249 是整包导入） |
| 来源 | 自己写，ep0 状态机照 Linux `ep0.c` | m1n1 的 DWC3 + nabu-usb-cdc 的模块，改 5 处 |
| class | CDC-ECM：2 接口，bulk `0x81`/`0x01`，中断 `0x82` | CDC-ACM：2 接口，中断 `0x81`，bulk `0x02`/`0x82` |
| 宿主表现 | Linux `cdc_ether`、macOS 自带驱动 | Windows `usbser`（COM8） |
| 附加 | ifnet `ue0` 192.168.7.2/24，UDP 5140 实时日志 | 控制通道 hello/ping、重启前 drain |
| 验证 | sweet 上 ping 通（`NOTES.md:32`） | pandora usb4/usb5 日志逐字节一致（`PLAN.md:587-588`） |
| 在 pandora | 编进去了（PANDORA 配置同时定义 SWEET），但寄存器偏移是 sweet 的，没有 pandora 验收记录 | 线上路径（`HANDOFF.md:85` 的启动参数） |
| 规模 | ~900 行 | 5,834 行（`Dwc3.c` 647，`QcomXnuUsbDebug.c` 1551） |

### 2.3 做了哪几步

1. **只读探测**（`0072` / `0248`）：先读 GCC 的 USB 时钟 CBCR、`BCR`/`GDSCR`，
   确认时钟和电源域开着、复位没按住，才去读 DWC3 的 identity/global/device/event
   寄存器和 QSCRATCH；两次间隔 100 ms 看状态有没有动。写操作一个都没有。
   （`wsl/pandora_usb.c:84-115`）
2. **ep0 枚举**（`0073`）：`DCTL` 软复位 → 自己的事件缓冲区（`GEVNTADR/SIZ`，
   中断屏蔽、轮询）→ `DEPSTARTCFG` → `SETEPCONFIG` → `SETTRANSFRESOURCE` →
   setup TRB → `RUN_STOP`。控制传输是一个 5 状态机（SETUP/DATA/两个状态等待/STATUS），
   照 Linux `ep0.c`。
3. **VBUS override**（`0074`）：光重启控制器不会 connect，因为 Qualcomm 外壳要
   软件置 VBUS 有效。写 QSCRATCH `HS_PHY_CTRL` 的 bit20（`UTMI_OTG_VBUS_VALID`）
   和 bit28（`SW_SESSVLD_SEL`）、`SS_PHY_CTRL` 的 bit24（`LANE0_PWR_PRESENT`），
   等价于 Linux `dwc3_qcom_vbus_override_enable`。
4. **数据端点 + ECM**（`0075`）：物理端点 `EP_RX=2`（OUT ep1）、`EP_TX=3`（IN ep1）、
   `EP_INT=5`（IN ep2），`DALEPENA` 打开；描述符变成 2 接口的 CDC-ECM；
   建 ifnet `ue0`（`IFNET_INIT_LEGACY`）、`proto_plumb(PF_INET)`、用内核 socket
   给自己配 192.168.7.2/24；`SET_INTERFACE(1,1)` 时发 `NETWORK_CONNECTION`。
5. **收发修正**（`0076`）：`mbuf_allocpacket` 的第一个 mbuf 长度为 0 会触发
   "Failed mbuf validity check: len -14"，改成一个 cluster。
6. **实时日志 + link state**（`0077`、`0081`）：每 100 ms 把 console ring 的新字节
   用 UDP 发到 192.168.7.1:5140；补 `SIOCGIFMEDIA` 和 `KEV_DL_LINK_ON`，
   否则 configd 不认这个接口。
7. **换模块**（`0249-0253`）：导入 `qcomusb`，DMA 走固件留下的直通上下文
   （bank 3，物理地址）；`0250/0251` 把三个寄存器窗口改成
   `ml_io_map_unmappable`（原来的 `ml_io_map` 8 MiB 专用区会被占满，触摸驱动
   随后要 128 KiB 就分配失败）；`0253` 在重启前把剩下的日志 drain 掉；
   `pandora_usb.c` 的线程每 5 ms 把 console ring 喂给模块。

### 2.4 日志与验收

- 内核日志关键字：`PANDORA USB:`（`pandora_usb.c`）和 `SWEET USB:`（sweet_usb.c）。
  `qcom_usb_debug_get_status` 会打印 phase/error/core/qcom/DMA/vbus/phy 等字段
  （`pandora_usb.c:143-153`）。
- 验收记录在 `PLAN.md:585-591`（usb1–usb11）。仓库引用的 `device/logs/usb*.txt`
  没有随归档发布，只有 PLAN 里的转述。
- 主机侧抓取：`tools/usbserial_capture.ps1`（Windows，pandora）；
  我们的对应脚本是 `tools/sweet-usbnet.sh`（Linux）。

### 2.5 TODO / 边界

- **轮询、单 CPU、中断屏蔽**：线程绑在 boot CPU（`pandora_usb.c:159-164`），
  每 5 ms 一轮（`:117,190`）；事件中断位一直掩着（`qcomusb/usb/Dwc3.c:524-538`）。
  吞吐和延迟都撑不住 mux 的持续双向流量。
- **端点**：每个端点一个 TRB，DMA arena 只有 32 KiB（`Dwc3.h:8`），没有多 TRB 环、
  没有流控、没有 ZLP、没有 halt 恢复。
- **只有 ACM/ECM**：没有第二个功能，没有 Apple 接口，没有 mux。
- **PHY 不编程**（`Sm8850.h:64`）：完全吃 bootloader 状态；换板子/换启动方式就可能不成立。
- **枚举等待为 0**（`Sm8850.h:59`）：因为是在系统跑起来后从线程里启动。
- **重启前 drain 最多 2 秒**（`0253`）是 workaround，不是正常关闭流程。
- **没有 suspend/resume、没有 host 模式**；耳机那条路要 host 模式（`PLAN.md:712`）。
- **只在 `fastboot boot` 的交接状态下测过**；从开机菜单正常进 iOS 没测
  （`qcomusb/README.md:31`）。

---

## 3. 我们的 K60 现状

- 板：Redmi K60 / mondrian / SM8475，8 核（`tools/k60-build-boot.sh:56` 的 `cpus=8`）。
- 代码：`xnu/patches/0072-0081` 全套在：
  - `0072` 只读探测（GCC 7 个时钟 + GDSC + DWC3 + QUSB2）；
  - `0073` ep0 枚举（软复位、事件缓冲区、`DEPSTARTCFG`、setup TRB、`RUN_STOP`）；
  - `0074` VBUS override；
  - `0075` 数据端点 + CDC-ECM + ifnet `ue0`；
  - `0076` mbuf 修正；
  - `0077` UDP 实时日志；
  - `0081` link state。
- 但：
  - `0143` 把 `sweet_usb_probe()` 改成只在 `sweetusbprobe=1` 时调用。原因写得很清楚：
    探测读的是 SM7150 的地址（DWC3 `0xa600000`、QUSB2 `0x88e2000`），mondrian 的
    eUSB2 PHY 在 `0x88e3000`，探测会 data abort 打死启动。
  - `0148` 把 `QUSB2_PA` 改成 `0x88e3000`，并把 eUSB2 的两路 RPMh 电
    （`ldob5` 879 mV、`ldoc10` 1.2 V）挂到触摸驱动的电源路径里（因为从 USB 探测里
    调 `rh_lookup` 会崩）。
  - 之后**没有任何 K60 USB 启动记录**。文档里搜不到 `sweetusb`/`usbnet`/
    `192.168.7` 的 K60 实测。
- 结论：我们有"半成品 gadget 源码"，K60 上的链路是 **0**。今天 K60 的实际水平是
  **一个默认不执行的只读探测 + 一套没上过机的 gadget**。`0143` 的注释还提供了一条
  有用信息：探测是在读到 PHY 才崩的，说明 GCC/DWC3 那一段在 mondrian 上至少
  没有立刻挂——但这不是验收。

### 3.1 和 sweet 的差异（P0 要逐项确认）

| 项 | sweet（验过） | K60/mondrian（未验） | 风险 |
| --- | --- | --- | --- |
| PHY | QUSB2 `0x88e2000` | eUSB2 `0x88e3000`（`0148`） | 中：eUSB2 初始化/调优和 QUSB2 不同 |
| GCC USB 时钟偏移 | `0x0f010` 等（`0072`） | 未核对 `gcc-mondrian`/SM8475 | 中：读错只是日志错，写错会挂 |
| DWC3 基址 | `0xa600000` | 同 | 低 |
| QSCRATCH | `0xa6f8800` | 未确认 | 中：VBUS override 不生效就不 connect |
| SMMU | ECM 能跑，说明流是直通 | 未确认 apps SMMU 的 USB stream | 高：翻译开着会 DMA fault |
| 角色/交接 | ABL 留设备模式、halted | 未确认 | 中：可能要自己做 PHY 上电和角色切换 |
| 中断号 | 未用中断 | 未接 | 低（首版继续轮询） |

---

## 4. 到"被 macOS 认成 iPhone、承载 usbmux"的差距

### 4.1 USB 描述符

| 字段 | 现在（ECM / ACM） | 目标（iPhone X 形态） |
| --- | --- | --- |
| `idVendor:idProduct` | `1209:0001` / `1209:316d`（pid.codes 测试号） | `05AC:12A8` |
| `iManufacturer` / `iProduct` | `ios-sweet` / `Sweet iOS ...` | `Apple Inc.` / `iPhone` |
| `bDeviceClass` | 2（CDC） | 0（按接口定义，待真机确认） |
| 配置 | 1 份，只有 HS | 需要 HS + FS 两份 + device qualifier（`qcomusb/usb/Dwc3.c:65-68` 已有写法） |
| 接口 | CDC comm + data | 至少 1 个 `0xFF/0xFE/0x02`，带 1 个 bulk IN + 1 个 bulk OUT |
| 序列号 | `0001` / `QcomXnuUsbDebug` | 真机形态的 40 位 UDID 风格字符串（host 侧会缓存/去重） |

**必须先拿到一份真机 iPhone X 的完整描述符**（`lsusb -v` 或 `system_profiler
SPUSBDataType` + `ioreg -l`）。公开资料只够定性：
- `05ac:12a8`、Product `iPhone`、Manufacturer `Apple Inc.`；
- 网络共享（ipheth）会切到另一个配置（公开 dmesg 里是 `1-3:4.2`，
  即配置 4 / 接口 2）；
- usbmux 接口按 `(bInterfaceClass=0xff, bInterfaceSubClass=0xfe,
  bInterfaceProtocol=0x02)` 匹配，取其中一 IN 一 OUT 两个 bulk 端点。

我们可以用自己 IPSW 里的 iPhone X 身份，但**描述符抓取需要一台真机或一份公开的
完整 dump**，这是 P1 的输入，不是能拍脑袋写的东西。

### 4.2 class 选型

- **usbmux 不是 ACM。** ACM 是串口，ECM 是网卡；usbmux 走的是 Apple 私有的
  vendor-specific bulk 接口。想要"插上就被 macOS 认成 iPhone"，必须做这个接口。
- ECM 仍然有用：它是**另一条路**（§6 B 路线），可以让 macOS 的 usbmuxd 以
  "网络设备"发现我们，但不是"USB 认成 iPhone"。

### 4.3 设备侧协议层

有两个方案，差别很大：

**方案 A：复用 iOS 自带的 `AppleUSBDeviceMux`**

iOS 的 `usbmuxd` 负责 mux 复用和 TCP 状态机，我们只要给它一个 USB 通道。
从字符串看，它：
- 匹配 IOKit 服务 `AppleUSBDeviceMux`；
- 使用 `IOUSBDeviceController` / `USBDeviceFunction` / `IOUSBDeviceInterface`；
- 有 `local_worker`（unix socket）和 `remote_worker`（网络）；
- 会 `notify configd to enable USB`。

我们要做的是让 DWC3 gadget 发布一个 `USBDeviceFunction` nub，让
`AppleUSBDeviceMux` 匹配上。**前提**：该驱动在 iPhone X 内核缓存里，
且能按 `tools/mkprelink.py` 那套链进我们的内核（pandora 用它链了 11 个原版驱动）。
这可以离线验证：解 iPhone X kernelcache，列出 kext 和 `Info.plist` 的匹配字典。
如果可行，P2 从 3–4 人周降到 1–2 人周。

**方案 B：自己实现 mux 传输**

usbmuxd 的 USB 侧帧格式（`usbmuxd/src/device.c`）：

| 字段 | 长度 | 说明 |
| --- | --- | --- |
| `protocol` | 4 | 0=VERSION，1=CONTROL，2=SETUP，6=TCP |
| `length` | 4 | 含头总长 |
| `magic` | 4 | 仅 version>=2：`0xfeedface` |
| `tx_seq` | 2 | 仅 version>=2 |
| `rx_seq` | 2 | 仅 version>=2 |

- version<2 时头只有 8 字节（`protocol+length`）；version>=2 是 16 字节。
- VERSION 的载荷是 3 个 u32：`major/minor/padding`。
- TCP 的载荷是标准 20 字节 `tcphdr` + 数据；设备侧要实现 SYN/ACK/RST、
  序号、窗口、重传、MTU 和 ZLP。
- 连接最终落到本机端口（`62078` lockdownd、AFC、installation_proxy 等）。

**别和 `docs/PROTOCOL-SPEC.md` 的 16 字节头混了**：那份是
「PC 进程 → 宿主 usbmuxd」的客户端 API（`length/version/message/tag`，
plist 消息体，`tools/pcclient/framing.py` 已实现并有单测）；USB 线上是
「宿主 usbmuxd → 设备」的 `mux_header` + TCP 隧道。两层都有，但位置和字段不同。
客户端那层已经写完，正好可以在 G4 之后直接拿来验证。

这部分是真正的工程量：约 1,500–2,500 行 C，还要处理背压、短包、零包、
断线重连。好消息是协议有开源实现可对照，且内核里已有 BSD socket
（ECM 的 `sock_socket`/`sock_ioctl` 就是现成例子，`0075`）。

**建议**：先花 2–3 天做方案 A 的离线可行性验证；不可行再走 B。

### 4.4 配对与 lockdown

- `usbmux` 只负责"把 TCP 连接送到设备的某个端口"。macOS 要"信任"设备，
  需要 host 侧有一份 pair record（`/var/db/lockdown`），设备侧要能完成
  Pair/PairRecord 握手并派生 TLS 会话密钥。
- 我们没有 SEP，keybag 是桩（`docs/PC-TOOL-INSTALL-EVALUATION.md:9-10,45-48`）。
  可以做"永远接受 + 软件证书"的桩：如果 host 只校验 pair record 自洽，
  就能过；如果 Finder/iTunes 还校验设备证书链或 activation，就可能永远不认。
- 报文级细节（Pair/ValidatePair 能跳过多少、错误标识符）已经整理在
  `docs/PROTOCOL-SPEC.md:439-510`；PC 侧编解码原型在 `tools/pcclient/`，
  60 个单测全绿。**缺的只是设备侧的 keybag/证书桩**，不是协议理解。
- 因此**必须把"usbmux 能列设备"和"Finder 能认设备"分成两个里程碑**，
  前者不依赖配对，后者依赖。

### 4.5 六道门

| 门 | 内容 | 怎么验 | 依赖 |
| --- | --- | --- | --- |
| G1 | 设备模式跑起来、能枚举 | `lsusb` 看到任何 VID/PID | P0 |
| G2 | 被识别成 Apple | `system_profiler`/`ioreg` 显示 `Apple Inc. iPhone` | P1 |
| G3 | mux 接口被 host 认领 | macOS 出现 `0xFF/0xFE/0x02` 接口，usbmuxd 有匹配日志 | P1 |
| G4 | mux 握手 + TCP | host `usbmuxd` 收到 VERSION 应答，`idevice_id -l` 列出设备 | P2 |
| G5 | lockdown 应答 + 配对 | `ideviceinfo` 能过 62078；`idevicepair pair` 生成记录 | P3 |
| G6 | Finder/爱思可用 | Finder 侧栏出现设备，或爱思能读设备信息 | P4 |

---

## 5. 内核要补的清单

| 模块 | 现在有什么 | 还要补什么 | 证据 |
| --- | --- | --- | --- |
| ep0 / 标准请求 | `GET_DESCRIPTOR`、`SET_ADDRESS`、`SET_CONFIGURATION`、`GET/SET_INTERFACE`、`GET_STATUS`、class 请求放行 | `CLEAR_FEATURE(HALT)` 真正清 halt（现在 0075 是空分支）、`SET_FEATURE(TEST_MODE)`、配置值一致性 | `0075`、`qcomusb/usb/Dwc3.c:278-336` |
| 端点管理 | `DEPSTARTCFG`、每端点 `SETEPCONFIG`/`SETTRANSFRESOURCE`、`DALEPENA` | 多 TRB 环（link TRB）、burst/fifo 按带宽、`SetStall/ClearStall`、ZLP、短包语义 | `0073`、`0075`、`qcomusb/usb/Dwc3.c:199-231,268-276` |
| 事件 | 4 KiB 事件环 + 轮询，中断位掩着 | GIC 中断注册（ADT 里加 SPI，参考 `UniXNU-pong/docs/USB-LOGGING-PLAN.zh.md:47`）、事件类型全覆盖（XferNotReady/InProgress/Complete/CmdComplete）、溢出恢复 | `0073` 的 `GEVNTSIZ`、`qcomusb/usb/Dwc3.c:524-538` |
| DMA | `kmem_alloc_contig` + `pmap_find_phys`，物理地址给控制器 | 32 位可寻址检查、按方向 clean/invalidate、IOVA/SMMU 映射（或确认直通）、跨页/对齐约束 | `0073` 的 `kmem_alloc_contig`/`pmap_find_phys` 段 |
| PHY | 完全不编程，吃 bootloader | K60 的 eUSB2 初始化/调优（预加重、disconnect 阈值）、两路 RPMh 电（`0148` 只加了电，没上机） | `0148`、`qcomusb/soc/Sm8850.h:64` |
| 时钟/电源域 | 只读检查；`0074` 只做 VBUS | 自己上电（GCC 5 个 CBCR、BCR、GDSCR）、runtime PM、suspend/resume | `0072`、`0074` |
| 角色/充电共存 | VBUS override | Type-C 角色检测、与 iOS 的 SOCCP 充电驱动共存、拔插/重枚举 | `0074`、`PLAN.md:568` |
| IOKit 集成 | 无 | `IOUSBDeviceController` provider + `USBDeviceFunction` nub（方案 A），或自写 mux 的设备节点（方案 B） | iOS `usbmuxd` 字符串 |
| 速度/配置 | 单 config，HS | FS 描述符、other-speed config、qualifier（`qcomusb` 已有） | `qcomusb/usb/Dwc3.c:65-68` |

---

## 6. 两条路线

| | A. 原生 mux（目标） | B. ECM + 网络 usbmux（退路） |
| --- | --- | --- |
| USB 形态 | `05AC:12A8` + `0xFF/0xFE/0x02` bulk 接口 | CDC-ECM 网卡（已有） |
| 设备侧 mux | 复用 `AppleUSBDeviceMux` 或自写 | 不需要：走 TCP 62078 |
| host 发现 | USB 枚举 | Bonjour `_apple-mobdev2._tcp`，usbmuxd 的 `ConnectionType=Network` |
| 复用现有代码 | 端点/ep0/DMA 层 | `0075-0081` 几乎全复用 |
| 能达到 | Finder/爱思/idevice 全家桶 | `idevice_id -l -n`、自写客户端；Finder 需要先 Wi-Fi 同步配对 |
| 风险 | 描述符/AppleUSBDeviceMux/配对 | configd 不认 ifnet、Bonjour/配对、不是"USB 认成 iPhone" |

macOS 的 usbmuxd 确实支持网络设备（设备列表里有 `ConnectionType: Network`、
`EscapedFullServiceName`、`NetworkAddress`），所以 B 路线在协议上成立；
但 Finder/iTunes 的"网络设备"依赖之前用 USB 建立的 Wi-Fi 同步配对，
对我们是鸡生蛋。**B 只作为工具链退路，不作为目标。**

两条路线的 PC 侧客户端可以共用：`tools/pcclient/` 已经把
usbmux/lockdown/AFC/installation_proxy 的编解码做完（`docs/PROTOCOL-SPEC.md`），
所以 B 路线一旦通了传输，就能立刻端到端跑安装流程，不用再等客户端开发。

---

## 7. 分期与人周

| 阶段 | 产出 | 人周 | 依赖 | 主要风险 |
| --- | --- | --- | --- | --- |
| **P0 板级点亮** | `sweetusbprobe=1` 不崩；`sweetusb=2` 枚举 + `ping 192.168.7.2` | 1–2 | 无 | eUSB2 PHY、GCC 偏移、VBUS、SMMU 直通 |
| **P1 Apple 身份** | `05AC:12A8`、Apple 字符串、`0xFF/0xFE/0x02` 接口出现在 host | 0.5–1 | P0；真机描述符 dump | 描述符细节/多配置 |
| **P2 mux 传输** | A：`AppleUSBDeviceMux` 匹配并跑通 VERSION；B：`idevice_id -l` 列出设备 | 1–2（A）/ 3–4（B） | P1；离线 kext 验证 | kext 不可用、TCP 状态机 |
| **P3 配对** | `ideviceinfo` 过 62078；`idevicepair pair` 成功 | 2–3 | P2 | 无 SEP、证书校验 |
| **P4 macOS 端到端** | Finder 侧栏/爱思读设备 | 2–4 | P3 | Apple 的额外校验 |

注：P3/P4 的 PC 侧协议已经做完（`docs/PROTOCOL-SPEC.md` + `tools/pcclient/`），
这里的估时全部是**设备侧**（keybag 桩、服务端口、端到端联调）。

汇总：

- **到 G4（usbmux 列设备）**：4–7 人周（A）/ 7–10 人周（B）。
- **到 G5（能配对、能读设备信息）**：再加 2–3 人周。
- **到 G6（Finder/爱思）**：再加 2–4 人周，合计 **8–14 人周**。

如果 P0 就卡住（eUSB2 交接状态和 sweet 不同），后面全部顺延；所以 P0 的时间盒
建议设为 2 周，超时就先做只读探测 + 手动 PHY 初始化，不要直接冲 mux。

---

## 8. 必须上机才能验的检查点

按顺序，前一个不过不做后一个：

1. **只读探测不崩**：`sweetusbprobe=1` 启动，打印 GCC 5/7 个时钟、`GDSCR`、
   DWC3 `GSNPSID/GCTL/DSTS`；不重启。证明地址和门控对。
2. **设备模式启动**：`sweetusb=2`，日志出现 `SWEET USB: running (DSTS 0x...)`，
   `DSTS` 是 High-Speed、`DEVCTRLHLT` 已清。
3. **枚举**：PC `lsusb` 看到 `1209:0001`；`dmesg` 里 `cdc_ether` 绑定。
4. **bulk 往返**：`ping 192.168.7.2` 1000 包 0 丢；这同时验证 RX/TX 两个方向、
   DMA 一致性和 SMMU 路由。
5. **拔插**：10 次拔插每次都能重新枚举；这是 VBUS/角色/重连逻辑的验收。
6. **Apple 身份**：`system_profiler SPUSBDataType` 显示 `Apple Inc. iPhone`；
   `ioreg` 里能看到 `0xFF/0xFE/0x02` 的接口和 bulk 端点。
7. **mux 握手**：host `usbmuxd` 日志出现 VERSION 交互；`idevice_id -l` 列出设备。
8. **lockdown**：`ideviceinfo` 能连上 62078 并收到协议应答（哪怕配对失败）。
9. **配对**：`idevicepair pair` 返回成功，host 侧生成 pair record。
10. **稳定性**：30 分钟持续 mux 流量 + 同时充电 + 一次休眠唤醒，不掉线、不丢事件。

可以离线先做的（不占设备）：

- 解 iPhone X kernelcache，确认 `AppleUSBDeviceMux` 是否存在、匹配字典是什么；
- 用一台真 iPhone X 抓完整 USB 描述符（或找公开 dump 交叉验证）；
- 在 host 上抓一次真机 usbmux 的 VERSION/TCP 帧，作为自写 mux 的对照样本；
- 用 QEMU 跑内核改动（`docs/QEMU_TESTING.md`），只验证不崩，不验证 USB。

---

## 9. 风险清单

| 风险 | 概率 | 影响 | 缓解 |
| --- | --- | --- | --- |
| K60 eUSB2 交接状态和 sweet 不同，P0 直接卡住 | 中 | 高（阻塞全部） | 先只读探测；必要时写最小 PHY 初始化，参考 SM8475 下游内核 |
| `AppleUSBDeviceMux` 不在 iPhone X 内核缓存/不能加载 | 中 | 中（+2–3 人周） | 先离线查；不可用就走自写 mux |
| macOS 需要更多 Apple 私有描述符/OS descriptor | 中 | 中 | 用真机 dump 逐字段对齐；先保证 interface 匹配 |
| 无 SEP，Finder/iTunes 永远不认 | 中高 | 高（G6 可能不可达） | 把 G5 当目标；G6 用自写瘦客户端兜底 |
| apps SMMU 对 USB stream 开着翻译 | 中 | 高（DMA fault） | 复刻 pandora 的"继承固件直通上下文"；不要重置共享 SMMU |
| 与 SOCCP 充电/角色检测冲突 | 中 | 中（插拔掉线） | VBUS override + 角色状态机；先在只充电场景外测 |
| 单 CPU 5 ms 轮询撑不住 mux | 高 | 中（吞吐/延迟差） | 事件中断化 + 多 TRB 环；P2 之前做完 |
| 冒用 Apple VID/PID 的合规问题 | — | 低（本地实验） | 只在本机实验镜像使用，不发布 |

---

## 10. 证据索引

pandora（`~/hackios/refs/data-a-live`）：

- `README.md:25` — "USB 目前只用来充电，数据功能还没做"（已过时）
- `PLAN.md:585-591` — usb1–usb11 实测记录（枚举、1.5 MB 日志、逐字节一致）
- `PLAN.md:580` — 原生 USB/iTunes 的战略方向（把控制器交给 iOS 原生驱动）
- `PLAN.md:712` — 主机模式（耳机）还没做
- `HANDOFF.md:85` — 线上启动参数 `pandora_usb=2 qcom-usb-debug=1`
- `wsl/pandora_usb.c:1-25` — 控制器/交接状态说明
- `wsl/pandora_usb.c:51-60` — GCC/DWC3/QSCRATCH 基址、时钟偏移
- `wsl/pandora_usb.c:84-115` — 只读探测（两次 100 ms 对比）
- `wsl/pandora_usb.c:117,155-197` — 5 ms 轮询、绑 boot CPU、喂日志
- `wsl/pandora_usb.c:209-241` — 重启前 final drain
- `wsl/qcomusb/README.md:1-31` — 模块来源、5 处改动、本机交接状态
- `wsl/qcomusb/soc/Sm8850.h:24-27` — VID/PID `1209:316d`
- `wsl/qcomusb/soc/Sm8850.h:29-55` — GCC/DWC3/SMMU 基址、直通上下文
- `wsl/qcomusb/soc/Sm8850.h:59,64` — 枚举等待 0、PHY 不编程
- `wsl/qcomusb/usb/Dwc3.c:9-11` — 端点号（NOTIFY 3 / OUT 4 / IN 5）
- `wsl/qcomusb/usb/Dwc3.c:50-68` — CDC-ACM 描述符（HS/FS/qualifier）
- `wsl/qcomusb/usb/Dwc3.c:199-231,268-276` — 端点配置、SET_CONFIGURATION
- `wsl/qcomusb/usb/Dwc3.c:472-560` — 初始化/启动（事件中断掩着）
- `wsl/qcomusb/usb/Dwc3.c:562-600` — 轮询、队列、读
- `wsl/qcomusb/kernel/QcomXnuUsbDebug.c:671-680` — map/init
- `wsl/qcomusb/kernel/QcomXnuUsbDebug.c:1012-1030` — poll_owned
- `wsl/qcomusb/kernel/QcomXnuUsbDebug.c:1403-1440` — write/poll/final drain
- `xnu/patches/0072-0081` — 自写 gadget/ECM/UDP 日志/link state
- `xnu/patches/0248-0253` — 只读探测、qcomusb 导入、映射修复、final drain

我们（`~/hackios/k60-repo-full`）：

- `xnu/patches/0072-sweet-usb-probe.patch` — 只读探测（GCC 7 时钟、DWC3、QUSB2）
- `xnu/patches/0073-sweet-usb-device-ep0.patch` — ep0 枚举、事件环、DMA
- `xnu/patches/0074-sweet-usb-vbus-override.patch` — QSCRATCH VBUS override
- `xnu/patches/0075-sweet-usb-ecm.patch` — 数据端点、CDC-ECM 描述符、ifnet `ue0`
- `xnu/patches/0076-sweet-usb-rx-mbuf.patch` — mbuf 修正
- `xnu/patches/0077-sweet-usb-live-log.patch` — UDP 5140 实时日志
- `xnu/patches/0081-sweet-usb-link-state.patch` — `SIOCGIFMEDIA` + `KEV_DL_LINK_ON`
- `xnu/patches/0143-k60-usb-probe-opt-in.patch` — K60 上默认关掉探测（PHY 地址）
- `xnu/patches/0148-k60-usb-rails-via-touch.patch` — PHY 改 `0x88e3000`、加两路 RPMh 电
- `tools/sweet-usbnet.sh:1-23` — 主机侧 192.168.7.1/24 + UDP 日志
- `NOTES.md:30-34` — sweet 上 USB 网络/日志的实测（**不是 K60**）
- `docs/PC-TOOL-INSTALL-EVALUATION.md:7-13,45-56` — 配对/通道缺件与路线选择
- `docs/PROTOCOL-SPEC.md:68-262,439-510` — usbmux 客户端头、lockdown 配对能跳过多少
- `tools/pcclient/framing.py:110-227` — usbmux 客户端报文与 Connect 编解码原型
- `tools/k60-build-boot.sh:56` — K60 启动参数（`cpus=8` 等）

iOS 侧证据（`~/hackios/iosroot/root-hfsx.img`）：

- 偏移 `12056629308` 附近 — `usbmuxd` 二进制：`AppleUSBDeviceMux`、
  `IOUSBDeviceInterface`、`USBDeviceFunction`、`IOUSBDeviceController`
- 偏移 `11399562419` 附近 — `/etc/group` 的 `_usbmuxd:*:213:`

外部协议参考：

- usbmuxd `src/device.c`（mux 头、VERSION/SETUP/TCP、`0xfeedface`）：
  <https://github.com/libimobiledevice/usbmuxd/blob/master/src/device.c>
- usbmux 客户端 16 字节头（length/version/message/tag）：
  <https://deepwiki.com/libimobiledevice/libusbmuxd/2.3-protocol-layer>
- usbmux 接口匹配 `0xFF/0xFE/0x02` + 一 IN 一 OUT bulk：
  <https://cdn.jsdelivr.net/npm/@limrun/ui@0.13.9/src/core/device-install/operations/webusb.ts>
- iPhone `05ac:12a8` / `iPhone` / ipheth 在 4.2（dmesg 实例）：
  <https://discussion.fedoraproject.org/t/iphone-usb-tethering-doesnt-work/133172>
- macOS usbmuxd 支持网络设备（`ConnectionType: Network`）：
  <https://jkcoxson.com/blog/usbmuxd>
- pymobiledevice3：usbmuxd 同时支持 USB 与 Wi-Fi：
  <https://doronz88.github.io/pymobiledevice3/internals/idevice-protocol-layers/>

---

## 附：一句话给下一轮

**先用 `sweetusbprobe=1 sweetusb=2` 把 K60 的 DWC3 设备模式点亮（G1），
同时离线确认 iPhone X 内核缓存里有没有 `AppleUSBDeviceMux`；这两件事决定
后面是 4 人周还是 10 人周。**
