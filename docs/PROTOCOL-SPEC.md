# iOS 安装链路最小协议规格（usbmux / lockdown / AFC / installation_proxy）

面向 K60 iOS 移植的 App 安装器：一份「照着就能写代码」的报文级规格，配套
`tools/pcclient/` 里的纯宿主 Python 原型（只编解码，不连设备）。

> **诚实声明**：本轮工作**没有接触任何设备**，所以本文**没有任何一条字段是我们自己抓包实测的**。
> 所有内容来自三类来源，逐条标注：
>
> | 标记 | 含义 |
> | --- | --- |
> | ✅ **源码确认** | libimobiledevice / usbmuxd / pymobiledevice3 的代码里有明确的写入或读取语句，字段名、宽度、端序都能指到具体函数 |
> | 📄 **旧文档** | iPhone Dev Team 时代的 usbmux 说明页（已归档），与源码一致 |
> | 📋 **社区日志** | 别人在真机上跑出来的输出（安装进度字符串等），不是规范，但可交叉验证 |
> | 🧩 **推断** | 由实现行为外推（例如「设备大概会这样校验」），没有直接代码依据 |
> | ❓ **待实测** | 必须上机抓包才能定，编码时留好开关 |

---

## 0. 全局约定

### 0.1 端序速查（最容易踩的地方）

| 层 | 长度字段 | 端序 | 备注 |
| --- | --- | --- | --- |
| usbmux 头 | `u32 length` | **小端** | length **含**自身这 4 字节 |
| usbmux 二进制 Connect 体 | `u16 port` | **大端（网络序）** | 与同包内的 `u32 device_id`（小端）**不同** |
| usbmux plist Connect | `PortNumber` 整数 | 值是 `htons(port)` | 即端口做了字节交换，见 2.6 |
| lockdown / installation_proxy | `u32 length` | **大端** | length **不含**自身 4 字节 |
| AFC 头 | `u64 entire_length` / `u64 this_length` | **小端** | 无 4 字节长度前缀，靠 magic 定位 |

一句话：**usbmux 全是小端，但端口要按网络序放；lockdown 的长度前缀是大端；AFC 全小端。**

### 0.2 分层

```
PC 侧进程
  │  AF_UNIX /var/run/usbmuxd   (Windows: TCP 127.0.0.1:27015)
  │  ── usbmux 协议：16 字节头 ──────────────────────────────
  ▼
usbmuxd（宿主守护进程；把字节隧道到设备上的某个 TCP 端口）
  │
  ├─ Connect(DeviceID, 62078) ──► 设备 lockdownd
  │        └── lockdown 协议：4 字节大端长度 + XML plist
  │              ├─ StartService("com.apple.afc")                    → Port P_afc
  │              └─ StartService("com.apple.mobile.installation_proxy") → Port P_inst
  │
  ├─ Connect(DeviceID, P_afc)  ──► AFC      （40 字节头，二进制）
  └─ Connect(DeviceID, P_inst) ──► installation_proxy（又是 plist 帧）
```

**关键机制**：`Connect` 成功后，那条 usbmux 连接就退化成一条**裸 TCP 管道**。
从这一刻起，链路上不再有 usbmux 头，你写进去的字节原样到达目标服务的监听 socket。
所以一个安装器要开**三条** usbmux 连接（lockdownd 一条、AFC 一条、installation_proxy 一条）。

### 0.3 端口与服务名

| 名称 | 值 | 来源 |
| --- | --- | --- |
| lockdownd 端口 | `62078` (`0xf27e`) | ✅ `lockdown.c` 里 `struct lockdownd_service_descriptor .port = 0xf27e` |
| AFC 服务名 | `com.apple.afc` | ✅ `afc.h` `#define AFC_SERVICE_NAME "com.apple.afc"` |
| 安装服务名 | `com.apple.mobile.installation_proxy` | ✅ `installation_proxy.h` |
| 应用容器服务 | `com.apple.mobile.house_arrest` | ✅ `house_arrest.h` |
| 通知代理 | `com.apple.mobile.notification_proxy` | ✅ `notification_proxy.h` |
| 宿主 socket | `/var/run/usbmuxd`；Windows `127.0.0.1:27015` | ✅ `usbmuxd-proto.h` |

---

## 1. usbmux

### 1.1 帧头（16 字节，全小端）

| 偏移 | 大小 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | 4 | `length` | **整包**字节数，含这 4 字节自身。只有头时 = 16 |
| 4 | 4 | `version` | `0` = 旧二进制协议；`1` = plist 协议 |
| 8 | 4 | `message` | 消息类型，见 1.3 |
| 12 | 4 | `tag` | 请求号，响应原样回带；用于匹配请求/响应 |

✅ 源码确认：`libusbmuxd/src/usbmuxd-proto.h` 的 `struct usbmuxd_header`
（`length, version, message, tag`，`__attribute__((packed))`，直接整块 write，
所以在小端主机上就是小端）；pymobiledevice3 `UsbmuxdHeader(BinaryStruct, byte_order="<")`。
📄 旧文档同样写作 `u32 length / u32 reserved / u32 type / u32 tag`，全部 little-endian。

十六进制（ListDevices 请求的头 16 字节）：

```
0000  a5 01 00 00 01 00 00 00 08 00 00 00 01 00 00 00
      └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘
       0x1a5=421     version=1    message=8     tag=1
       整包 421 字节               PLIST
```

### 1.2 两个版本

| `version` | 负载 | 说明 |
| --- | --- | --- |
| `1` | XML plist | **推荐**。所有请求都是 `{"MessageType": "..."}` 字典 |
| `0` | 定长二进制 | 老协议，只有 CONNECT/LISTEN 等少数消息 |

✅ `libusbmuxd.c` 里 `proto_version` 初始为 1；一旦收到 `RESULT_BADVERSION (6)` 就降级为 0
（`if ((res == RESULT_BADVERSION) && (proto_version == 1)) proto_version = 0;`）。
我们的实现照抄这个策略：**先用 version=1，失败再降级**。

### 1.3 消息类型

| 值 | 名称 | 方向 | 用途 |
| --- | --- | --- | --- |
| 1 | `RESULT` | 设备→宿主 | 请求的通用应答，体是 `u32 result` |
| 2 | `CONNECT` | 宿主→设备 | 建立到设备某端口的隧道（二进制协议用） |
| 3 | `LISTEN` | 宿主→设备 | 订阅设备插拔事件（二进制协议用） |
| 4 | `DEVICE_ADD` | 设备→宿主 | 设备插入（体是 `usbmuxd_device_record`） |
| 5 | `DEVICE_REMOVE` | 设备→宿主 | 设备移除（体是 `u32 device_id`） |
| 6 | `DEVICE_PAIRED` | 设备→宿主 | 配对状态变化（仅 macOS 场景） |
| 8 | `PLIST` | 双向 | 负载是一个 plist，`MessageType` 键决定语义 |

✅ `usbmuxd-proto.h` 的 `enum usbmuxd_msgtype`；注释里 `//???` 的位置就是 7，未使用。

### 1.4 结果码（`RESULT` 消息体 / plist 里的 `Number`）

| 值 | 名称 | 典型触发 |
| --- | --- | --- |
| 0 | `OK` | 成功 |
| 1 | `BADCOMMAND` | `MessageType` 拼错 / 不支持 |
| 2 | `BADDEV` | `DeviceID` 不存在（拔线了、ID 用错） |
| 3 | `CONNREFUSED` | 目标端口没在监听（**lockdownd 没跑起来时最常见**） |
| 4 | `NOSUCHSERVICE` | 端口无对应服务（pymobiledevice3 补充，libusbmuxd 未定义） |
| 6 | `BADVERSION` | 协议版本不被接受 → 降级到 version=0 |

✅ `usbmuxd-proto.h` 定义了 0/1/2/3/6（中间两个值被注释成 `//???`）；
`NOSUCHSERVICE = 4` 来自 pymobiledevice3 `UsbmuxdResult`。

### 1.5 ListDevices（必须实现）

请求 plist（`MessageType` 必填，其余是自报信息，usbmuxd 只记录不校验）：

```xml
<dict>
    <key>ClientVersionString</key><string>pcclient 0.1</string>
    <key>MessageType</key><string>ListDevices</string>
    <key>ProgName</key><string>pcclient</string>
    <key>kLibUSBMuxVersion</key><integer>3</integer>
</dict>
```

✅ `libusbmuxd.c` 的 `create_plist_message()` 会塞 `BundleID`(macOS)、`ClientVersionString`、
`MessageType`、`ProgName`、`kLibUSBMuxVersion=3`；`send_list_devices_packet()` 只加 `MessageType`。
我们只发后 4 个字段就够（❓ `BundleID` 是否必需，待实测；libusbmuxd 在非 macOS 上也不发）。

响应（`message=8`，`MessageType="Result"`，带 `DeviceList`）：

```xml
<dict>
    <key>DeviceList</key>
    <array>
        <dict>
            <key>DeviceID</key><integer>3</integer>          <!-- 后续 Connect 用它 -->
            <key>MessageType</key><string>Attached</string>
            <key>Properties</key>
            <dict>
                <key>ConnectionSpeed</key><integer>480000000</integer>
                <key>ConnectionType</key><string>USB</string>
                <key>DeviceID</key><integer>3</integer>
                <key>LocationID</key><integer>0</integer>
                <key>ProductID</key><integer>4776</integer>   <!-- 0x12a8 = iPhone -->
                <key>SerialNumber</key><string>00008110-000A1B2C3D4E5F6G</string>
            </dict>
        </dict>
    </array>
    <key>MessageType</key><string>Result</string>
</dict>
```

🧩 `DeviceList` 里的每一项在源码里被当作「Attached 事件」复用同一个解析函数
（`libusbmuxd.c` 的 `receive_packet()` 把 `MessageType == "Attached"` 转成 `MESSAGE_DEVICE_ADD`），
所以 `MessageType` 的值是 `Attached` 而不是别的——这条是**从解析代码反推**的。

### 1.6 Connect（必须实现）

#### plist 版（`version=1`，推荐）

```xml
<dict>
    <key>DeviceID</key><integer>3</integer>
    <key>MessageType</key><string>Connect</string>
    <key>PortNumber</key><integer>32498</integer>   <!-- = htons(62078) -->
    ...
</dict>
```

> **`PortNumber` 是 `htons(port)` 之后的整数，不是端口本身。**
> 62078 = `0xf27e`，`htons` 后 = `0x7ef2` = **32498**。
> ✅ 依据：`libusbmuxd.c` `send_connect_packet()` 写的是
> `plist_dict_set_item(plist, "PortNumber", plist_new_uint(htons(port)))`；
> 服务端 `usbmuxd/src/client.c` 收到后 `ntohs(portnum)` 才用。
> 两边刚好抵消，但**中间这一层必须按网络序填**。

响应（`message=8`）：

```xml
<dict><key>MessageType</key><string>Result</string><key>Number</key><integer>0</integer></dict>
```

#### 二进制版（`version=0`）

`length=24`，体 8 字节：

```
0000  18 00 00 00 00 00 00 00 02 00 00 00 05 00 00 00
      └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘
      length=24   version=0    message=2      tag=5
0010  03 00 00 00 f2 7e 00 00
      └────┬────┘ └─┬─┘ └─┬─┘
      device_id=3  port    reserved=0
        (小端)   0xf27e
                (大端/网络序!)
```

响应是 `message=1` 的 `RESULT`，体 4 字节小端结果码：

```
0000  14 00 00 00 01 00 00 00 01 00 00 00 02 00 00 00
      length=20   version=1   message=1     tag=2
0010  00 00 00 00
      result=0 (OK)
```

✅ 二进制体布局见 `usbmuxd-proto.h` 的 `struct usbmuxd_connect_request`
（`u32 device_id; u16 port; u16 reserved;`）+ `libusbmuxd.c` 里 `conninfo.port = htons(port)`。

### 1.7 Listen / Attached / Detached（可选）

`{"MessageType": "Listen"}` → 回 `Result/0` 之后，同一条连接会持续推送
`{"MessageType":"Attached"|"Detached", "DeviceID":N, "Properties":{...}}`。
单设备安装器不需要它，**可以不做**；只要 `ListDevices` 里能挑到目标设备即可。

### 1.8 usbmux 最小实现清单

必须：

1. 打包/解包 16 字节头（`length` 含自身，小端）。
2. 发 `MessageType=ListDevices`，解析 `DeviceList[].DeviceID`。
3. 发 `MessageType=Connect`，`PortNumber = htons(port)`，判 `Number == 0`。
4. 解析 `Result` 的失败码，至少区分 `CONNREFUSED(3)` 与 `BADDEV(2)`。
5. `tag` 自增并校验回包 tag（不匹配说明读串包了，直接报错比继续读安全）。

可以回「不支持」：

* `Listen`（插拔事件）、`ReadBUID`（除非要填 `SystemBUID`，见 3.5）
* `ReadPairRecord` / `SavePairRecord` / `DeletePairRecord`（macOS 专用配对缓存接口，Linux/Windows 上 libusbmuxd 根本不用）
* `version=0` 的二进制路径（只有在 `BADVERSION` 时才需要）

### 1.9 常见错误

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `Number=3` CONNREFUSED | 设备侧 lockdownd 没起（我们的移植里 lockdownd 曾反复崩溃） | 先确认 62078 通；重试 3 次后放弃 |
| `Number=2` BADDEV | DeviceID 用了上一次的 / 设备重插后 ID 变了 | 重新 `ListDevices` |
| 读包卡死 | `length` 按「不含自身」实现 → 少读/多读 4 字节 | 严格按 1.1 的表 |
| 回包 tag 对不上 | 前一个请求的响应没读完 | 读满 `length` 再发下一个 |

---

## 2. lockdown（lockdownd）

### 2.1 帧格式

```
+-------------------+--------------------------------------+
| u32 大端 length   |  length 字节的 XML plist（UTF-8）     |
+-------------------+--------------------------------------+
```

* `length` **不含**自身 4 字节（和 usbmux 相反，别抄错）。
* 没有 magic、没有版本号、没有请求号：**一问一答**，请求与响应靠顺序配对。
* 响应永远是「一个 plist 字典」，失败时带 `Error` 键。

✅ `property_list_service.c`：
`nlen = htobe32(length); service_send(parent, &nlen, 4, &bytes); service_send(parent, content, length, &bytes);`
接收端 `pktlen = be32toh(pktlen);` 然后按 `pktlen` 循环读满。
✅ pymobiledevice3 `build_plist()`：`struct.pack(">L", len(payload)) + payload`。

十六进制（`GetValue ProductVersion` 的前 16 字节，整帧 333 字节）：

```
0000  00 00 01 49 3c 3f 78 6d 6c 20 76 65 72 73 69 6f
      └────┬────┘ └────┬────┘
      length=329      "<?xm"...  负载 329 字节 = 整帧 333 - 4
```

### 2.2 报文结构

请求（键都放在顶层，`Request` 必填）：

| 键 | 类型 | 说明 |
| --- | --- | --- |
| `Request` | string | 方法名：`QueryType` / `GetValue` / `SetValue` / `StartSession` / `StopSession` / `StartService` / `Pair` / `ValidatePair` / `Unpair` |
| `Label` | string | 客户端标识，回包里原样带回；建议填，不填也能用 |
| `Domain` | string | `GetValue`/`SetValue` 用，如 `com.apple.mobile.battery` |
| `Key` | string | `GetValue`/`SetValue` 用；省略表示取整个 domain |
| `Value` | any | `SetValue` 用 |
| `HostID` | string | `StartSession` 用 |
| `SystemBUID` | string | `StartSession` 用 |
| `SessionID` | string | `StopSession` 用 |
| `Service` | string | `StartService` 用 |
| `EscrowBag` | data | `StartService` 用（备份类服务需要；安装不需要） |

响应：

| 键 | 类型 | 说明 |
| --- | --- | --- |
| `Result` | string | `"Success"` / `"Failure"` |
| `Error` | string | 失败标识符，如 `InvalidHostID`（此时没有 `Result`） |
| `ErrorDescription` | string | 人类可读描述（可选） |
| `Value` | any | `GetValue` 的返回值 |
| `SessionID` | string | `StartSession` 返回 |
| `EnableSessionSSL` | bool | `StartSession` 返回，true 表示后续要走 TLS |
| `Port` | integer | `StartService` 返回 |
| `EnableServiceSSL` | bool | `StartService` 返回 |

✅ `lockdown.c` 的 `lockdown_check_result()` 逻辑：先看 `Error`，再看 `Result == "Success"`；
两者都没有就当成功（`QueryType` 的响应就是只有 `Type`）。

### 2.3 QueryType（建议实现，成本极低）

请求：`{"Label":"pcclient","Request":"QueryType"}`
响应：`{"Type":"com.apple.mobile.lockdown"}`（没有 `Result` 键）

✅ `lockdownd_query_type()` 只读 `Type`。用途：确认 62078 上真的是 lockdownd
（而不是连错端口后把 plist 喂给了别的服务）。

### 2.4 GetValue（必须实现）

请求（十六进制前 4 字节 + 完整 XML 如下）：

```xml
<dict>
    <key>Key</key><string>ProductVersion</string>
    <key>Label</key><string>pcclient</string>
    <key>Request</key><string>GetValue</string>
</dict>
```

响应：

```xml
<dict>
    <key>Key</key><string>ProductVersion</string>
    <key>Request</key><string>GetValue</string>
    <key>Result</key><string>Success</string>
    <key>Value</key><string>16.7.10</string>
</dict>
```

> 回包里的 `Request`/`Key` 是设备回带的（🧩 推断：方便宿主对账）。
> **解析时不要依赖它们**——`libimobiledevice` 只读 `Result` 和 `Value`。

安装器常用的 key：

| Key | 用途 | 依据 |
| --- | --- | --- |
| `ProductVersion` | 判断 iOS 版本，决定用哪套安装参数 | ✅ `lockdown.c` 里直接 `lockdownd_get_value(client, NULL, "ProductVersion", ...)` |
| `UniqueDeviceID` | 设备 UDID，也用来定位 pairing record 文件名 | ✅ `lockdown.c` |
| `DeviceClass` | `iPhone` / `iPad` 等，兼容性判断 | ✅ `lockdown.c` |
| `BuildVersion` | 具体固件版本 | ✅ pymobiledevice3 `lockdown.py` |
| `WiFiAddress` | 配对成功后写进本地 record | ✅ `lockdown.c`（`get wifi mac now` 那段） |
| `HardwareModel` | 机型标识（如 `D22AP`） | 🧩 常见键，本仓库引用的源码里没直接出现，**待实测** |
| `CPUArchitecture` | `arm64` / `arm64e` | 🧩 同上，**待实测** |

### 2.5 StartSession（必须实现）

```xml
<dict>
    <key>HostID</key><string>AABBCCDD-1122-3344-5566-778899AABBCC</string>
    <key>Label</key><string>pcclient</string>
    <key>Request</key><string>StartSession</string>
</dict>
```

（`SystemBUID` 可选，值来自 usbmux 的 `ReadBUID`。）

成功响应：

```xml
<dict>
    <key>EnableSessionSSL</key><false/>
    <key>Request</key><string>StartSession</string>
    <key>Result</key><string>Success</string>
    <key>SessionID</key><string>...</string>
</dict>
```

**`HostID` 从哪来**：设备上保存的 pairing record 里的 `HostID`。
设备侧按这个字符串查自己的配对记录，查不到就回 `{"Error":"InvalidHostID"}`。
✅ 依据：`lockdown.c` 的 `lockdownd_client_new_with_handshake()` 先从 record 里取 `host_id`，
再 `lockdownd_start_session(client, host_id, ...)`；`userpref.c` 的
`pair_record_get_host_id()` 读的就是 record 的 `HostID` 键。

**`SessionID` 有什么用**：只有 `StopSession` 需要。安装流程本身不校验它（🧩 推断：
设备只在 lockdownd 内部把 session 和连接绑定）。

**`EnableSessionSSL`**：真机上如果返回 `true`，后续所有 lockdownd 报文都必须走 TLS。
🧩 推断：已配对且已激活的设备通常返回 `false`；未配对/激活态可能返回 `true`。
**这条必须上机确认**（见第 6 节）。

### 2.6 StartService（必须实现）

```xml
<dict>
    <key>Label</key><string>pcclient</string>
    <key>Request</key><string>StartService</string>
    <key>Service</key><string>com.apple.afc</string>
</dict>
```

成功响应：

```xml
<dict>
    <key>EnableServiceSSL</key><false/>
    <key>Port</key><integer>50000</integer>
    <key>Request</key><string>StartService</string>
    <key>Result</key><string>Success</string>
    <key>Service</key><string>com.apple.afc</string>
</dict>
```

> **`Port` 是主机序整数（不是网络序！）**，和 usbmux 的 `PortNumber` **相反**。
> 拿到后直接 `Connect(DeviceID, Port)`，由 usbmux 层负责 `htons`。
> ✅ `lockdown.c` 的 `lockdownd_do_start_service()` 直接 `plist_get_uint_val(node, &port_value)`
> 就当端口用，中间没有 `ntohs`/`htons`。

失败：`{"Error":"InvalidService"}`（服务名拼错）或 `{"Error":"ServiceProhibited"}`（设备不允许）。

### 2.7 StopSession（可选）

`{"Label":..., "Request":"StopSession", "SessionID":"..."}` → `{"Result":"Success"}`。
进程直接关 socket 也能结束 session，**可以先不做**。

### 2.8 Pair / ValidatePair：能跳过多少

完整配对（我们**不做**）：

1. 宿主生成两对 RSA 2048 密钥：root keypair + host keypair，并各自自签一张证书
   （✅ `userpref.c` `pair_record_generate_keys_and_certs()`：`RSA_generate_key_ex(..., 2048, ...)`）。
2. 发 `{"Request":"Pair", "ProtocolVersion":"2", "PairRecord":{...}, "PairingOptions":{...}}`。
   线上 `PairRecord` 只有 5 个键：`DeviceCertificate`、`HostCertificate`、`HostID`、
   `RootCertificate`、`SystemBUID`
   （✅ `lockdownd_pair_record_to_plist()` 里正好写这 5 个）。
   本地 record 里的 `RootPrivateKey` / `HostPrivateKey` 在发送前被显式删掉
   （✅ `lockdownd_do_pair()` 里两行 `plist_dict_remove_item(..., USERPREF_*_PRIVATE_KEY_KEY)`）；
   `WiFiMACAddress` **不参与** Pair 请求，是配对成功后才由宿主 `GetValue("WiFiAddress")`
   补进本地 record 的（✅ 同上函数注释 `get wifi mac now ...`）。
3. 设备回 `{"Result":"Success", "EscrowBag":<data>}`。
4. 宿主把 `DeviceCertificate/HostCertificate/RootCertificate/RootPrivateKey/HostPrivateKey/
   HostID/EscrowBag/WiFiMACAddress/SystemBUID` 存成 `/var/lib/lockdown/<UDID>.plist`（Linux）或
   `%ProgramData%\Apple\Lockdown\<UDID>.plist`（Windows）
   （✅ 键名宏在 `userpref.h`，路径见 `userpref.c` 的 `USERPREF_CONFIG_DIR`）。
5. `ValidatePair` 只是拿已有 record 让设备确认「这个 host 我认识」。

**可以跳过的部分**（本项目的瘦客户端策略）：

| 步骤 | 能否跳过 | 说明 |
| --- | --- | --- |
| 生成密钥/证书 | ✅ 能 | 我们不做 TLS 客户端认证，也不需要签名 |
| 证书链校验 | ✅ 能 | 设备侧校验是它的自由，我们不校验证书 |
| `Pair` 本身 | ⚠️ 有条件 | **前提是设备上已有本机 HostID 的配对记录**。否则 `StartSession` 必回 `InvalidHostID` |
| `ValidatePair` | ✅ 能 | 只有 iOS < 7 才需要（✅ `lockdownd_client_new_with_handshake()` 里按 `device->version < 7.0.0` 分支） |
| `EscrowBag` | ✅ 能 | 只有备份/恢复类服务要求，安装链路不用 |

🧩 **现实路径**（待实测）：设备是我们自己刷的 iOS，root 可写，
可以直接把一份预生成的 `HostID`（以及配套 record）塞进设备
`/private/var/root/Library/Lockdown/pair_records/`，宿主这边只带同一个 `HostID` 字符串。
这样完全绕开证书。**这条是否被 lockdownd 接受，必须上机验证**。

### 2.9 lockdown 错误标识符（`Error` 字段的取值）

✅ 完整表在 `lockdown.c` 的 `lockdownd_error_str_map[]`，安装链路上会遇到的：

| `Error` | 含义 | 处理 |
| --- | --- | --- |
| `InvalidHostID` | HostID 不在设备的配对记录里 | 走 2.8 的预置 record，或完整 Pair |
| `MissingHostID` | 没发 `HostID` | 补字段 |
| `SessionActive` | 已经开过 session | 先 `StopSession` 或换连接 |
| `SessionInactive` / `InvalidSessionID` | `StopSession` 时 session 不对 | 忽略即可 |
| `InvalidService` | 服务名拼错 | 检查字符串 |
| `ServiceProhibited` | 设备策略禁止该服务 | 越狱/降级策略问题 |
| `MissingPairRecord` / `InvalidPairRecord` | 设备侧配对记录损坏 | 重新配对 |
| `PasswordProtected` | 设备有锁屏密码且未解锁 | 先解锁 |
| `PairingDialogResponsePending` | 设备上弹了「信任此电脑」没人点 | 需要人点确认 |
| `GetProhibited` / `SetProhibited` | 该 key 不允许读写 | 换 key |
| `InvalidResponse` | 报文结构不对（常见于少字段） | 对照 2.2 的键表 |

### 2.10 lockdown 最小实现清单

必须：

1. plist 帧编解码（4 字节大端长度，容忍 TCP 拆包/粘包）。
2. `QueryType`（1 次，确认端口对）。
3. `GetValue(ProductVersion)`、`GetValue(UniqueDeviceID)`。
4. `StartSession(HostID)`，判 `Error`。
5. `StartService("com.apple.afc")`、`StartService("com.apple.mobile.installation_proxy")`，取 `Port`。

可以回「不支持」：

* `Pair` / `ValidatePair` / `Unpair`（走 2.8 的预置 record）
* `SetValue` / `RemoveValue`（除非要改设备配置）
* `Activate` / `Deactivate`（激活是另一个子系统）
* `EscrowBag` 相关
* `EnableSessionSSL` / `EnableServiceSSL` 为 true 时的 TLS 升级 → 直接报错退出，别硬撑

### 2.11 常见错误

| 现象 | 原因 |
| --- | --- |
| 发完请求读不到响应 | 长度前缀写成小端（应为大端）；或忘了 `length` 不含自身 |
| 响应解析成乱码 | 端口连错（把 plist 发给了 AFC） |
| `InvalidHostID` | 见 2.8 |
| 卡在 `StartService` 不返回 | 设备侧对应服务没注册（我们的移植里 mobile_installation_proxy 可能没起） |

---

## 3. AFC（Apple File Conduit）

### 3.1 帧头（40 字节，全小端）

| 偏移 | 大小 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | 固定 ASCII `"CFA6LPAA"` |
| 8 | 8 | `entire_length` | 整包 = 40 + `data` + `payload` |
| 16 | 8 | `this_length` | 头 + 紧跟的 `data` 段 = 40 + `len(data)` |
| 24 | 8 | `packet_num` | 请求序号，设备**原样回带**；用于匹配响应 |
| 32 | 8 | `operation` | opcode，见 3.2 |

`entire_length > this_length` 时，差额部分是尾随的 bulk 数据（只有 FileWrite 用得上）。
接收方必须按 `entire_length` 读满，中间可能被拆成多次 read。

✅ `afc.h`：`#define AFC_MAGIC "CFA6LPAA"`、`typedef struct { char magic[8]; uint64_t entire_length, this_length, packet_num, operation; } AFCPacket;`
✅ `afc.c` `afc_dispatch_packet()`：
`entire_length = sizeof(AFCPacket) + data_length + payload_length; this_length = sizeof(AFCPacket) + data_length;`
✅ pymobiledevice3 `AfcHeader(BinaryStruct, byte_order="<")`，`AFC_HEADER_SIZE = 40`。

### 3.2 opcode

| 值 | 名称 | 必须? | 说明 |
| --- | --- | --- | --- |
| 0x01 | `STATUS` | ✅ 必须（收） | 通用应答：`u64 status` |
| 0x02 | `DATA` | ✅ 必须（收） | 数据应答（ReadDir / FileRead / GetFileInfo） |
| 0x03 | `READ_DIR` | 建议 | 列目录，请求 `path\0` |
| 0x05 | `WRITE_FILE` | 可选 | 一次性写整个文件 |
| 0x07 | `TRUNCATE` | 可选 | `path\0 + u64 size` |
| 0x08 | `REMOVE_PATH` | 建议 | 删文件/空目录 |
| 0x09 | `MAKE_DIR` | ✅ 必须 | 建目录 |
| 0x0A | `GET_FILE_INFO` | 建议 | `path\0` → DATA 里是键值串 |
| 0x0B | `GET_DEVINFO` | 可选 | 容量信息 |
| 0x0D | `FILE_OPEN` | ✅ 必须 | `u64 mode + path\0` |
| 0x0E | `FILE_OPEN_RES` | ✅ 必须（收） | 回 `u64 handle` |
| 0x0F | `FILE_READ` | 可选 | `u64 handle + u64 length` |
| 0x10 | `FILE_WRITE` | ✅ 必须 | `u64 handle` + 尾随数据 |
| 0x11 | `FILE_SEEK` | 可选 | `u64 handle + u64 offset + u64 whence` |
| 0x12 | `FILE_TELL` | 可选 | `u64 handle` |
| 0x13 | `FILE_TELL_RES` | 可选（收） | 回 `u64 pos` |
| 0x14 | `FILE_CLOSE` | ✅ 必须 | `u64 handle` |
| 0x15 | `FILE_SET_SIZE` | 可选 | ftruncate |
| 0x16 | `GET_CON_INFO` | 可选 | 连接信息 |
| 0x17 | `SET_CON_OPTIONS` | 可选 | 打开 Unicode 文件名等开关 |
| 0x18 | `RENAME_PATH` | 建议 | `src\0 dst\0` |
| 0x1B | `FILE_LOCK` | 可选 | flock |
| 0x1C | `MAKE_LINK` | 可选 | 硬/软链接 |
| 0x1E | `SET_FILE_MOD_TIME` | 可选 | `path\0 + u64 nsec` |
| 0x22 | `REMOVE_PATH_AND_CONTENTS` | 可选 | rm -rf（iOS 6+） |
| 0x27 | `FILE_READ_OFFSET` | 可选 | pread |
| 0x28 | `FILE_WRITE_OFFSET` | 可选 | pwrite |

✅ 完整枚举见 `afc.h`（`AFC_OP_INVALID` 到 `AFC_OP_FILE_WRITE_OFFSET`），
与 pymobiledevice3 `AfcOpcode` 逐条一致。
上表**省略**了安装器用不到的几个：`0x00 INVALID`、`0x04 READ_FILE`、`0x06 WRITE_PART`、
`0x0C WRITE_FILE_ATOM`、`0x19 SET_FS_BS`、`0x1A SET_SOCKET_BS`、`0x1D GET_FILE_HASH`、
`0x1F GET_FILE_HASH_RANGE`、`0x20 FILE_SET_IMMUTABLE_HINT`、`0x21 GET_SIZE_OF_PATH_CONTENTS`、
`0x23-0x26 DIR_OPEN/DIR_OPEN_RESULT/DIR_READ/DIR_CLOSE`。

### 3.3 FileOpen / FileWrite / FileClose（安装器的核心三件套）

#### FileRefOpen（0x0D）

`data = u64 mode | path + NUL`，`payload` 为空，所以 `entire_length == this_length`。

```
0000  43 46 41 36 4c 50 41 41 45 00 00 00 00 00 00 00
      └──────┬───────┘ └──────┬───────┘
         "CFA6LPAA"      entire_length = 0x45 = 69
0010  45 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00
      └──────┬───────┘ └──────┬───────┘
       this_length = 69    packet_num = 1
0020  0d 00 00 00 00 00 00 00 03 00 00 00 00 00 00 00
      └──────┬───────┘ └──────┬───────┘
     operation=0x0D        mode = 3 (w: O_WRONLY|O_CREAT|O_TRUNC)
0030  2f 50 75 62 6c 69 63 53 74 61 67 69 6e 67 2f 61
0040  2e 69 70 61 00
      "/PublicStaging/a.ipa" + NUL      ← 20 字符 + NUL = 21；40+8+21 = 69 ✓
```

文件模式（✅ `afc.h` `afc_file_mode_t`）：

| 值 | 含义 |
| --- | --- |
| 1 | `r` |
| 2 | `r+` |
| 3 | `w`（O_WRONLY\|O_CREAT\|O_TRUNC） |
| 4 | `w+` |
| 5 | `a` |
| 6 | `a+` |

响应是 `FILE_OPEN_RES`（0x0E），`data` 前 8 字节就是句柄：

```
0000  43 46 41 36 4c 50 41 41 30 00 00 00 00 00 00 00
0010  30 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00
0020  0e 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00
                                └──────┬───────┘
                                   handle = 1
```

#### FileRefWrite（0x10）

`data = u64 handle`（8 字节），`payload` = 数据块。**`this_length` 恒为 48**。

```
0000  43 46 41 36 4c 50 41 41 35 00 00 00 00 00 00 00
      "CFA6LPAA"                entire_length = 0x35 = 53
0010  30 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00
      this_length = 0x30 = 48   packet_num = 2
0020  10 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00
      operation=0x10            handle = 1
0030  68 65 6c 6c 6f
      "hello"                   ← 尾随 bulk，48 + 5 = 53 ✓
```

每包建议 ≤ 1 MiB（🧩 pymobiledevice3 的 `MAXIMUM_WRITE_SIZE` 是 1 MiB 量级；
苹果自家实现用 0x800000 = 8 MiB 的 socket 块大小，但**先按 1 MiB 保守走**）。

✅ `afc.c` `afc_file_write()`：`data_len = 8; *(uint64_t*)DATA_PTR = handle;
afc_dispatch_packet(AFC_OP_FILE_WRITE, data_len, data, length, ...)`。
✅ pymobiledevice3 `fwrite()` 显式传 `this_length=48`。

#### FileRefClose（0x14）

`data = u64 handle`，`entire_length == this_length == 48`：

```
0000  43 46 41 36 4c 50 41 41 30 00 00 00 00 00 00 00
0010  30 00 00 00 00 00 00 00 03 00 00 00 00 00 00 00
0020  14 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00
      operation=0x14            handle = 1
```

### 3.4 响应：STATUS 与 DATA

所有「无返回数据」的操作都回 `STATUS`，`data` 是 `u64 status`（0 = 成功）：

```
0000  43 46 41 36 4c 50 41 41 30 00 00 00 00 00 00 00
0010  30 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00
0020  01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
      operation=0x01            status = 0 (SUCCESS)
```

`READ_DIR` / `FILE_READ` / `GET_FILE_INFO` / `GET_DEVINFO` 回 `DATA`（0x02），
`data` 就是原始字节：

* `READ_DIR`：一串 NUL 结尾的文件名（含 `.` 和 `..`），条目数由长度决定
  （✅ `afc.c` `make_strings_list()` 用 NUL 计数）。❓ 是否额外多一个终止 NUL：两套开源实现
  处理不一致，我们的 `split_cstrings()` 两种都吃。
* `FILE_READ`：原始文件内容，可能少于请求长度（读到 EOF）。
* `GET_FILE_INFO`：`st_size\0 12345\0 st_ifmt\0 S_IFREG\0 ...` 交替的键值串。
* `GET_DEVINFO`：`Model\0 iPhone10,3\0 FSTotalBytes\0 ...\0`。

### 3.5 其余常用操作的数据布局

| 操作 | `data` 布局 |
| --- | --- |
| `MAKE_DIR` 0x09 | `path\0` |
| `READ_DIR` 0x03 | `path\0` |
| `GET_FILE_INFO` 0x0A | `path\0` |
| `REMOVE_PATH` 0x08 | `path\0` |
| `RENAME_PATH` 0x18 | `src\0 dst\0` |
| `TRUNCATE` 0x07 | `path\0 + u64 size` |
| `FILE_READ` 0x0F | `u64 handle + u64 length` |
| `FILE_SEEK` 0x11 | `u64 handle + u64 offset + u64 whence` |
| `FILE_SET_SIZE` 0x15 | `u64 handle + u64 size` |
| `SET_FILE_MOD_TIME` 0x1E | `path\0 + u64 mtime(ns)` |
| `GET_DEVINFO` 0x0B | 空 |

✅ 全部来自 `afc.c` 对应函数的 `data_len` 与 `memcpy`/`*(uint64_t*)` 写法。

### 3.6 AFC 错误码（`STATUS` 的 `u64 status`）

| 值 | 名称 | 典型场景 |
| --- | --- | --- |
| 0 | `SUCCESS` | 成功 |
| 1 | `UNKNOWN_ERROR` | 未分类 |
| 2 | `OP_HEADER_INVALID` | 头里的 magic/length 不对，或 `packet_num` 对不上 |
| 4 / 5 | `READ_ERROR` / `WRITE_ERROR` | 底层 IO 失败 |
| 6 | `UNKNOWN_PACKET_TYPE` | opcode 不认识 |
| 7 | `INVALID_ARG` | 参数不合法（句柄 0、路径空） |
| 8 | `OBJECT_NOT_FOUND` | 路径不存在（**最常见**） |
| 9 | `OBJECT_IS_DIR` | 想 open 一个目录 |
| 10 | `PERM_DENIED` | 沙盒/权限（非越狱设备上写 `/var` 必踩） |
| 12 | `OP_TIMEOUT` | 超时 |
| 13 | `TOO_MUCH_DATA` | 单包太大 |
| 14 | `END_OF_DATA` | 读越界 |
| 15 | `OP_NOT_SUPPORTED` | 设备不支持该 opcode |
| 16 | `OBJECT_EXISTS` | mkdir 已存在 |
| 18 | `NO_SPACE_LEFT` | 空间不足 |
| 20 | `IO_ERROR` | 设备侧 IO 错 |
| 33 | `DIR_NOT_EMPTY` | 删非空目录 |

✅ `afc.h` `afc_error_t` 全表。

### 3.7 AFC 最小实现清单

必须：

1. 40 字节头打包/解包，校验 magic。
2. `MAKE_DIR`、`FILE_OPEN`、`FILE_WRITE`（含尾随 payload 分段写）、`FILE_CLOSE`。
3. 收 `STATUS` 并判 `status == 0`；收 `FILE_OPEN_RES` 取句柄。
4. `packet_num` 自增并与回包比对（✅ libimobiledevice 从 1 开始自增；pymobiledevice3 从 0 开始。
   🧩 设备只回带不校验，两种都行——**但同一连接内必须一致**）。

可以回「不支持」：

* `FILE_READ` / `FILE_SEEK` / `FILE_TELL`（安装器只写不读）
* `GET_FILE_HASH*`、`FILE_LOCK`、`MAKE_LINK`、`SET_FS_BS`、`SET_SOCKET_BS`
* `DIR_OPEN`/`DIR_READ`/`DIR_CLOSE`（0x23-0x26，iOS 6+ 的目录枚举器，`READ_DIR` 够用）
* `REMOVE_PATH_AND_CONTENTS`（可用递归 `REMOVE_PATH` 代替）

### 3.8 常见坑

| 现象 | 原因 |
| --- | --- |
| 设备回 `OP_HEADER_INVALID (2)` | `entire_length` 没算上尾随 payload；或 `this_length` 不等于 48 |
| FileWrite 后设备不再回包 | 数据没写完就等响应（先写满 `entire_length`） |
| `OBJECT_NOT_FOUND (8)` | 目标目录不存在（`/PublicStaging` 可能要先 `MAKE_DIR`） |
| 路径写成相对路径 | AFC 只接受绝对路径 |
| 大文件卡住 | 单包太大被 `TOO_MUCH_DATA` 拒绝，降到 1 MiB |

---

## 4. mobile_installation_proxy

### 4.1 传输

**和 lockdown 完全一样**：4 字节大端长度 + XML plist，一问多答。
区别只有一个：发一条 `Command` 之后，设备会**连续推多条状态 plist**，直到 `Status == "Complete"`。

✅ `installation_proxy.c` `instproxy_send_command()` 用的就是
`property_list_service_send_xml_plist()`；接收侧是 `instproxy_receive_status_loop()` 的
`do { ... } while (!complete)`。

### 4.2 命令表

| `Command` | 必需字段 | 说明 |
| --- | --- | --- |
| `Install` | `PackagePath` | 安装（要求包未安装） |
| `Upgrade` | `PackagePath` | 升级（要求已安装同 bundle id） |
| `Uninstall` | `ApplicationIdentifier` | 卸载 |
| `Browse` | — | 列出所有 App（分页，`CurrentList`/`CurrentAmount`/`Total`） |
| `Lookup` | 可选 `ClientOptions.BundleIDs` | 查指定 App 的元数据 |
| `Archive` / `Restore` / `RemoveArchive` | `ApplicationIdentifier` | 归档/恢复 |
| `LookupArchives` | — | 列归档 |
| `CheckCapabilitiesMatch` | 可选 `Capabilities` | 能力检查 |

✅ 全部来自 `installation_proxy.c` 的 `instproxy_*()` 函数。
`Uninstall` 的字段名是 `ApplicationIdentifier`（✅ `instproxy_uninstall()`）。
`Lookup` 的 `BundleIDs` **在 `ClientOptions` 字典里面**，不是顶层键
（✅ `instproxy_lookup()` 把 `BundleIDs` 塞进 client_options 再作为 `ClientOptions` 发送；
pymobiledevice3 的 `lookup(options)` 也一样）。

### 4.3 Install / Upgrade 请求

```xml
<dict>
    <key>ClientOptions</key>
    <dict>
        <key>IsUserInitiated</key><true/>
        <key>PackageType</key><string>Developer</string>
    </dict>
    <key>Command</key><string>Install</string>
    <key>PackagePath</key><string>/PublicStaging/a.ipa</string>
</dict>
```

帧头十六进制（整帧 437 字节）：

```
0000  00 00 01 b1 3c 3f 78 6d 6c 20 76 65 72 73 69 6f
      └────┬────┘
      length = 0x1b1 = 433 = 437 - 4
```

`ClientOptions` 常用键（✅ 键名来自 `installation_proxy.c` / `instproxy.h` 的选项 API，
取值来自 📋 社区日志与 pymobiledevice3）：

| 键 | 取值 | 用途 |
| --- | --- | --- |
| `PackageType` | `Developer` / `Customer` / `CarrierBundle` | 开发者签名包必须 `Developer` |
| `IsUserInitiated` | bool | 让 SpringBoard 显示安装进度 |
| `ApplicationSINF` | data | 包内 `SC_Info/*.sinf`，**device 侧已从 ipa 里读到时不必发** |
| `iTunesMetadata` | data | 同上，`iTunesMetadata.plist` |
| `CFBundleIdentifier` | string | `Upgrade` 时明确目标 bundle id（🧩 待实测是否必需） |
| `ReturnAttributes` | array | `Browse`/`Lookup` 用 |
| `AllowInstallLocalProvisioned` | bool | 📋 日志里出现过，允许本地描述文件 |

🧩 **注意**：iTunes/iTools 走的是「把 ipa 解开、只传必要文件 + 用 SINF 签名」的路线；
libimobiledevice 的 `ideviceinstaller` 走的是「整包传到 `/PublicStaging`，让设备自己解」的路线。
**我们只需要后者**——设备侧 `installd` 会自己解 zip。

### 4.4 状态轮询

设备对每条状态回一个 plist：

```xml
<dict>
    <key>PercentComplete</key><integer>15</integer>
    <key>Status</key><string>ExtractingPackage</string>
</dict>
```

结束标志：

```xml
<dict><key>Status</key><string>Complete</string></dict>
```

`Complete` 这一帧的完整十六进制（238 字节；换成进度消息时只有负载内容不同）：

```
0000  00 00 00 ea 3c 3f 78 6d 6c 20 76 65 72 73 69 6f  ....<?xml versio
      └────┬────┘ └──────────────┬──────────────┘
     length=0xea=234        "<?xml version=..."
     负载 234 = 238 - 4
0010  6e 3d 22 31 2e 30 22 20 65 6e 63 6f 64 69 6e 67  n="1.0" encoding
0020  3d 22 55 54 46 2d 38 22 3f 3e 0a 3c 21 44 4f 43  ="UTF-8"?>.<!DOC
0030  54 59 50 45 20 70 6c 69 73 74 20 50 55 42 4c 49  TYPE plist PUBLI
0040  43 20 22 2d 2f 2f 41 70 70 6c 65 2f 2f 44 54 44  C "-//Apple//DTD
0050  20 50 4c 49 53 54 20 31 2e 30 2f 2f 45 4e 22 20   PLIST 1.0//EN"
0060  22 68 74 74 70 3a 2f 2f 77 77 77 2e 61 70 70 6c  "http://www.appl
0070  65 2e 63 6f 6d 2f 44 54 44 73 2f 50 72 6f 70 65  e.com/DTDs/Prope
0080  72 74 79 4c 69 73 74 2d 31 2e 30 2e 64 74 64 22  rtyList-1.0.dtd"
0090  3e 0a 3c 70 6c 69 73 74 20 76 65 72 73 69 6f 6e  >.<plist version
00a0  3d 22 31 2e 30 22 3e 0a 3c 64 69 63 74 3e 0a 09  ="1.0">.<dict>..
00b0  3c 6b 65 79 3e 53 74 61 74 75 73 3c 2f 6b 65 79  <key>Status</key
00c0  3e 0a 09 3c 73 74 72 69 6e 67 3e 43 6f 6d 70 6c  >..<string>Compl
00d0  65 74 65 3c 2f 73 74 72 69 6e 67 3e 0a 3c 2f 64  ete</string>.</d
00e0  69 63 74 3e 0a 3c 2f 70 6c 69 73 74 3e 0a  ict>.</plist>.
```

进度消息同理（`{"PercentComplete":15,"Status":"ExtractingPackage"}` 是 298 字节，
前缀 `00 00 01 26`，即负载 294 字节）。

失败标志（任何一条状态里出现 `Error` 就要中止）：

```xml
<dict>
    <key>Error</key><string>ApplicationVerificationFailed</string>
    <key>ErrorDescription</key><string>...</string>
    <key>ErrorDetail</key><integer>3892346893</integer>
</dict>
```

✅ 判定逻辑见 `instproxy_receive_status_loop()`：
先 `instproxy_status_get_error()`（读 `Error`/`ErrorDetail`/`ErrorDescription`），
再 `instproxy_status_get_name()` 读 `Status`，`"Complete"` 才置 `complete = 1`；
没有 `Status` 键的消息被忽略。

📋 真机日志里的完整状态序列（ideviceinstaller 输出，见参考链接的 issue）：

```
CreatingStagingDirectory   5%
ExtractingPackage         15%
InspectingPackage         20%
TakingInstallLock         20%
PreflightingApplication   30%
InstallingEmbeddedProfile 30%
VerifyingApplication      40%
CreatingContainer         50%
InstallingApplication     60%
PostflightingApplication  70%
SandboxingApplication     80%
GeneratingApplicationMap  90%
InstallComplete          100%
Complete
```

❓ **不要硬编码这些字符串**：不同 iOS 版本不一样（也有日志出现
`TakingPreflightMeasures`、`GeneratingInstallMap`、`AppInstallation` 等变体）。
正确做法是：**只认 `Complete` 和 `Error`，其余一律当进度显示**（用 `PercentComplete` 画进度条）。

### 4.5 Browse / Lookup 的响应形状

`Browse` 分页推送：

```xml
<dict>
    <key>CurrentList</key><array><dict><key>CFBundleIdentifier</key><string>com.a</string>...</dict></array>
    <key>CurrentAmount</key><integer>50</integer>
    <key>CurrentIndex</key><integer>0</integer>
    <key>Total</key><integer>123</integer>
    <key>Status</key><string>...</string>   <!-- 分页状态名未确认，见下 -->
</dict>
```

✅ 键名来自 `instproxy_status_get_current_list()`（`CurrentList` / `Total` /
`CurrentAmount` / `CurrentIndex`）。
❓ `Status` 的具体取值没有可靠来源，我们只按「有 `CurrentList` 就当一页数据」处理，
`Status == "Complete"` 才收尾——**不依赖中间状态名**。

### 4.6 常见错误标识符

| `Error` | 含义 | 处理 |
| --- | --- | --- |
| `ApplicationVerificationFailed` | 签名/描述文件不被接受 | 包没签名或证书不在设备信任链 |
| `MismatchedApplicationIdentifierEntitlement` | `Upgrade` 的 entitlement 与已装版本不一致 | 用 `Install`（先卸载）或换签名（📋 该字符串来自社区日志，不在源码表里） |
| `APIInternalError` | 设备内部错误（磁盘满、installd 崩） | 看 syslog |
| `UnknownCommand` | `Command` 拼错 | 对照 4.2 |
| `MissingPackagePath` | 没给 `PackagePath` | 补字段 |
| `MissingApplicationIdentifier` | `Uninstall` 少字段 | 补 `ApplicationIdentifier` |
| `ApplicationSINFCaptureFailed` / `iTunesMetadataCaptureFailed` | 设备解包时找不到 SINF/元数据 | 用整包 ipa，别手工拼目录 |
| `DeviceOSVersionTooLow` | 包的最低系统版本高于设备 | 换包 |
| `ApplicationAlreadyInstalled` | 用 `Install` 装已存在的包 | 改用 `Upgrade` |
| `NotEntitled` / `InstallProhibited` | 设备策略/entitlement 不允许 | 换签名或改设备策略 |

✅ 除 `MismatchedApplicationIdentifierEntitlement` 外，表中所有字符串都出现在
`installation_proxy.c` 的 `instproxy_strtoerr()`（该函数共映射 55 个标识符）。

### 4.7 installation_proxy 最小实现清单

必须：

1. 复用 lockdown 的 plist 帧编解码。
2. 发 `Install` / `Upgrade`（`Command` + `PackagePath` + `ClientOptions.PackageType`）。
3. 循环读状态直到 `Status == "Complete"`；见到 `Error` 立即中止并报错。
4. 用 `PercentComplete` 做进度，**不要**依赖具体状态字符串。
5. 超时保护（设备卡死时不会发 `Complete`）。

可以回「不支持」：

* `Archive` / `Restore` / `RemoveArchive` / `LookupArchives`
* `CheckCapabilitiesMatch`
* `Browse` / `Lookup`（如果安装器不需要列 App；但 `Lookup` 对「判断是否已安装」很有用）

---

## 5. 端到端时序（装一个 ipa）

约定：`→` 宿主发，`←` 设备回。三条连接分别记为 **C_lockdown / C_afc / C_inst**。

```
# ---- 准备 ----
1.  connect(/var/run/usbmuxd)                        # 一条新连接 = C_lockdown
2.  C_lockdown → {MessageType: ListDevices}          tag=1
3.  C_lockdown ← {MessageType: Result, DeviceList:[{DeviceID:3, ...}]}
    ⇒ 记下 DeviceID = 3
4.  C_lockdown → {MessageType: Connect, DeviceID:3, PortNumber:htons(62078)}  tag=2
5.  C_lockdown ← {MessageType: Result, Number:0}
    ⇒ 这条连接从此刻起是裸管道，直通设备 lockdownd

# ---- lockdown 握手 ----
6.  C_lockdown → {Request: QueryType}
7.  C_lockdown ← {Type: com.apple.mobile.lockdown}
8.  C_lockdown → {Request: GetValue, Key: ProductVersion}
9.  C_lockdown ← {Result: Success, Value: "16.7.10"}
10. C_lockdown → {Request: StartSession, HostID: "<record 里的 HostID>"}
11. C_lockdown ← {Result: Success, SessionID: "...", EnableSessionSSL: false}
    ⇒ 若 EnableSessionSSL = true：本客户端不支持，报错退出
12. C_lockdown → {Request: StartService, Service: com.apple.afc}
13. C_lockdown ← {Result: Success, Port: <P_afc>, EnableServiceSSL: false}
14. C_lockdown → {Request: StartService, Service: com.apple.mobile.installation_proxy}
15. C_lockdown ← {Result: Success, Port: <P_inst>}
    ⇒ C_lockdown 必须保持打开（session 绑在这条连接上）

# ---- 传包（新连接 C_afc）----
16. connect(/var/run/usbmuxd)                        # C_afc
17. C_afc → {MessageType: Connect, DeviceID:3, PortNumber:htons(P_afc)}
18. C_afc ← {MessageType: Result, Number:0}
19. C_afc → AFC MAKE_DIR  "/PublicStaging"           # 已存在则忽略 OBJECT_EXISTS(16)
20. C_afc → AFC FILE_OPEN "/PublicStaging/<uuid>.ipa" mode=3
21. C_afc ← AFC FILE_OPEN_RES  handle=<h>
22. C_afc → AFC FILE_WRITE handle=<h> chunk=<1 MiB>   （循环，每包等 STATUS）
23. C_afc ← AFC STATUS 0                              （每包一次）
24. C_afc → AFC FILE_CLOSE handle=<h>
25. C_afc ← AFC STATUS 0

# ---- 触发安装（新连接 C_inst）----
26. connect(/var/run/usbmuxd)                        # C_inst
27. C_inst → {MessageType: Connect, DeviceID:3, PortNumber:htons(P_inst)}
28. C_inst ← {MessageType: Result, Number:0}
29. C_inst → {Command: Install,
              ClientOptions: {PackageType: Developer, IsUserInitiated: true},
              PackagePath: "/PublicStaging/<uuid>.ipa"}
30. C_inst ← {Status: CreatingStagingDirectory, PercentComplete: 5}
31. C_inst ← ...（若干条，只取 PercentComplete 画进度）
32. C_inst ← {Status: Complete}
    ⇒ 中途任何一条带 Error 键：立即中止，打印 Error/ErrorDescription/ErrorDetail

# ---- 收尾（可选）----
33. C_afc → AFC REMOVE_PATH "/PublicStaging/<uuid>.ipa"
34. C_lockdown → {Request: StopSession, SessionID: "..."}
35. 关闭三条连接
```

注意事项：

* **不要把三条连接合成一条**：`Connect` 之后连接就专用于那个服务，没法回头再发 usbmux 消息。
* `Install` 是「同步长任务」：从第 29 步到第 32 步可能几分钟（大包 + 慢 UFS）。
  读超时给到 60 s 以上，或者用「无数据超时」而不是「总超时」。
* 第 19 步的 `MAKE_DIR` 在越狱/已激活设备上通常 `/PublicStaging` 已存在，返回 16 要当成功处理。

---

## 6. 三条最不确定、必须上机抓包验证的

> 下面三条都会**直接决定实现能不能跑通**，而且都无法从开源代码里读到答案
> （设备侧 `lockdownd` / `afc` / `installd` 是闭源的）。建议按这个顺序验。

### 6.1 `StartSession` 对 `HostID` 的校验到底有多严

**不确定点**：我们打算不做完整 `Pair`，只带一个 `HostID` 字符串去开 session。
设备是否会接受一个「本机凭空生成、但设备侧没有对应 pairing record」的 HostID？

**为什么不确定**：`lockdown.c` 只是把 record 里的 HostID 发过去，
设备侧怎么比对（是查 `/private/var/root/Library/Lockdown/pair_records/`，
还是只校验 session 里带的证书）完全闭源。🧩 推断：**会查**，因为 `InvalidHostID`
这个错误码就是为这个场景准备的。

**验证方法**：

1. 用随机 UUID 当 `HostID` 发 `StartSession`，期望 `{"Error":"InvalidHostID"}`。
2. 从设备里 dump 一份已存在的 pair record（越狱设备可直接读
   `/private/var/root/Library/Lockdown/pair_records/*.plist`——该路径 🧩 推断，
   以设备实际为准），拿它的 `HostID` 再试。
3. 若第 2 步成功，方案定稿：**出厂镜像里预置一份 record，宿主硬编码同一个 HostID**。
4. 若第 2 步仍失败，说明还需要 `EscrowBag` 或 TLS 客户端证书 → 退回完整 Pair 实现。

**影响**：`framing.start_session()` 的入参、以及是否需要实现 `pair()`（当前是 `NotImplementedError`）。

### 6.2 AFC 响应的三个细节（两套开源实现互相矛盾）

**不确定点**：

1. `STATUS` 包体除了 `u64 status` 之外，**是否还跟一个 NUL 结尾的路径字符串**？
   libimobiledevice 只取前 8 字节、忽略余下（说明它见过更长的包）；
   pymobiledevice3 直接 `if length != 8: logger.error("Status length != 8")`（说明它期望正好 8）。
2. `READ_DIR` 的 `DATA` 里，条目之间用 NUL 分隔，**末尾是否多一个 NUL**？
   libimobiledevice 按「NUL 个数 = 条目数」解析（等于没有多余终止符），
   但很多实现会多写一个。
3. `packet_num` 的**起始值**：libimobiledevice 从 1 开始，pymobiledevice3 从 0 开始。
   设备是「只回带」还是「校验连续性」？

**为什么不确定**：三处都是实现细节，没有任何一方写了规范。

**验证方法**：抓一次 `MAKE_DIR` + 一次 `READ_DIR` 的**原始响应字节**（在 AFC 层打 hexdump），
看 `entire_length` 到底是 48 还是 48+len(path)，以及 `DATA` 的尾部是不是 `00 00`。

**影响**：`afc.AfcPacket.status()`（已做容忍）、`afc.split_cstrings()`（已做容忍）、
以及 `packet_num` 计数器初值。当前实现选择了「两种都能吃」，所以**不改也能跑**，
但要知道真实值才能删掉多余分支。

### 6.3 iOS 16 上 `installation_proxy` 是否还吃 `/PublicStaging` 里的裸 ipa

**不确定点**：`{"Command":"Install","PackagePath":"/PublicStaging/x.ipa"}` 这条路
在 iOS 16 上是否仍然有效，尤其当 `ClientOptions` 里**没有** `ApplicationSINF` /
`iTunesMetadata` 时。设备可能：

* (a) 自己解 zip、自己抽 SINF —— 我们的方案就成立；
* (b) 要求宿主先用 `house_arrest` 把文件塞进 App 容器；
* (c) 要求 `PackageType=Developer` 且 ipa 已用设备信任的证书签名。

**为什么不确定**：libimobiledevice 的 `ideviceinstaller` 至今仍用 (a)，
但那是针对旧版本的路径；iOS 16 的 `installd` 行为没有公开证据。

**验证方法**：拿一个**已用开发者证书签名**的小 ipa（< 5 MB），跑完整流程，
看失败停在哪一步：

* 停在 `ExtractingPackage` → 包格式/路径问题；
* 停在 `VerifyingApplication` 且 `Error=ApplicationVerificationFailed` → 签名问题（走 (c)）；
* 直接 `ApplicationSINFCaptureFailed` → 需要宿主提供 SINF（走 (b) 或手工塞 SINF）。

**影响**：`instproxy.install()` 的 `ClientOptions` 内容；严重时需要新增 `house_arrest` 模块。

### 6.4 次要待实测（不阻塞，但会影响健壮性）

| # | 待确认 | 当前假设 | 影响的代码 |
| --- | --- | --- | --- |
| 1 | `StartSession` 返回 `EnableSessionSSL` 是否为 true | false（否则本客户端直接失败） | `framing.service_descriptor()` 之后的 TLS 分支 |
| 2 | `StartService` 的 `Port` 是否恒为主机序 | 是（源码如此） | `framing.service_descriptor()` |
| 3 | `SystemBUID` 是否必需 | 不必需 | `framing.start_session()` |
| 4 | usbmux plist `Connect` 的 `PortNumber` 是否所有版本都要 `htons` | 是 | `framing.htons()` |
| 5 | `/PublicStaging` 在非越狱设备上是否可写 | 可写（`mobile` 用户） | AFC 第 19 步 |
| 6 | AFC 单包上限（1 MiB 是否安全） | 安全 | `afc.file_write()` 的 chunk 大小 |
| 7 | `ListDevices` 是否必须带 `BundleID` | 不必需 | `framing.DEFAULT_MUX_CLIENT_FIELDS` |

---

## 7. 参考链接

### usbmux

* libusbmuxd `src/usbmuxd-proto.h`（头结构、消息类型、结果码）
  https://github.com/libimobiledevice/libusbmuxd/blob/master/src/usbmuxd-proto.h
* libusbmuxd `src/libusbmuxd.c`（`send_packet` / `receive_packet` / `create_plist_message` / `send_connect_packet`）
  https://github.com/libimobiledevice/libusbmuxd/blob/master/src/libusbmuxd.c
* usbmuxd `src/client.c`（服务端 `ntohs(portnum)`，证明端口是网络序）
  https://github.com/libimobiledevice/usbmuxd/blob/master/src/client.c
* pymobiledevice3 `pymobiledevice3/usbmux.py`（`UsbmuxdHeader`、`PlistMuxConnection`）
  https://github.com/doronz88/pymobiledevice3/blob/master/pymobiledevice3/usbmux.py
* iPhone Dev Team 旧文档（字段名 `reserved`/`type`，little-endian）
  https://wikee.iphone-dev.com/usb:usbmux

### lockdown

* libimobiledevice `src/lockdown.c`（`lockdownd_get_value` / `lockdownd_start_session` /
  `lockdownd_do_start_service` / `lockdownd_do_pair` / `lockdown_check_result` / 错误字符串表）
  https://github.com/libimobiledevice/libimobiledevice/blob/master/src/lockdown.c
* libimobiledevice `src/lockdown.h`（`#define LOCKDOWN_PROTOCOL_VERSION "2"`）
  https://github.com/libimobiledevice/libimobiledevice/blob/master/src/lockdown.h
* libimobiledevice `src/property_list_service.c`（4 字节大端长度前缀）
  https://github.com/libimobiledevice/libimobiledevice/blob/master/src/property_list_service.c
* libimobiledevice `common/userpref.h`（pair record 的键名宏）
  https://github.com/libimobiledevice/libimobiledevice/blob/master/common/userpref.h
* libimobiledevice `common/userpref.c`（`pair_record_generate_keys_and_certs`，RSA 2048）
  https://github.com/libimobiledevice/libimobiledevice/blob/master/common/userpref.c
* pymobiledevice3 `pymobiledevice3/lockdown.py`（`StartSession` 请求形状）
  https://github.com/doronz88/pymobiledevice3/blob/master/pymobiledevice3/lockdown.py
* pymobiledevice3 `pymobiledevice3/service_connection.py`（`build_plist` 的 `">L"`）
  https://github.com/doronz88/pymobiledevice3/blob/master/pymobiledevice3/service_connection.py

### AFC

* libimobiledevice `src/afc.h`（`AFC_MAGIC`、`AFCPacket`、opcode 与错误码全表）
  https://github.com/libimobiledevice/libimobiledevice/blob/master/src/afc.h
* libimobiledevice `src/afc.c`（`afc_dispatch_packet` / `afc_receive_data` / 各操作的数据布局）
  https://github.com/libimobiledevice/libimobiledevice/blob/master/src/afc.c
* pymobiledevice3 `pymobiledevice3/services/afc.py`（`AfcHeader`、`fwrite` 的 `this_length=48`）
  https://github.com/doronz88/pymobiledevice3/blob/master/pymobiledevice3/services/afc.py

### installation_proxy

* libimobiledevice `src/installation_proxy.c`（命令构造、状态循环、错误标识符表）
  https://github.com/libimobiledevice/libimobiledevice/blob/master/src/installation_proxy.c
* libimobiledevice `include/libimobiledevice/installation_proxy.h`
  https://github.com/libimobiledevice/libimobiledevice/blob/master/include/libimobiledevice/installation_proxy.h
* pymobiledevice3 `pymobiledevice3/services/installation_proxy.py`（`install_from_local` / `send_package`）
  https://github.com/doronz88/pymobiledevice3/blob/master/pymobiledevice3/services/installation_proxy.py

### 真机日志（📋 状态字符串的来源）

* ideviceinstaller 完整进度输出（objection issue #661）
  https://github.com/sensepost/objection/issues/661
* Xamarin 的安装进度与 `MismatchedApplicationIdentifierEntitlement`
  https://github.com/xamarin/Xamarin.Forms/issues/13939
* iOS 日志里 `mobile_installation_proxy` 的 `PackageType`/`IsUserInitiated` 选项
  https://stackoverflow.com/questions/50806574/run-detached-app-on-ios-device
* `TakingInstallLock` / `PreflightingApplication` 变体
  https://stackoverflow.com/questions/63445771/failed-to-run-libgdx-app-on-ios-device-at-40

### 本仓库配套代码

* `tools/pcclient/framing.py` —— usbmux 帧 + lockdown 请求/响应
* `tools/pcclient/plistmsg.py` —— plist 帧（4 字节大端长度）
* `tools/pcclient/afc.py` —— AFC 头与各 opcode 请求体
* `tools/pcclient/instproxy.py` —— 安装命令与状态解析
* `tools/pcclient/selftest.py` —— `--selftest` 打印各协议示例帧的十六进制
* `tools/pcclient/test_framing.py` / `test_afc.py` / `test_instproxy.py` —— 单元测试
