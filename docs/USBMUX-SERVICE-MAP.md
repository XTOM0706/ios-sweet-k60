# USBMUX-SERVICE-MAP：根镜像里"PC 装 App"那几条设备端服务，逐个摸清

日期：2026-11-01　　方式：纯离线（`root-hfsx.img` 只读 loop 挂载 + dyld 共享缓存切分 + 代码签名 entitlements 解析）
临时文件：`/tmp/usbmux/`（脚本与切出来的段都在这里，可随时删）　　没有写镜像，没有碰设备，没有 git 操作。

镜像：`/home/xtom0706/hackios/iosroot/root-hfsx.img`，挂载为 `hfsplus (ro)`，
`LC_BUILD_VERSION` 全部是 `platform ios, minos 16.7, sdk 16.7.3`。

---

## 0. 结论（先看这五条）

1. **usbmuxd 不在这份镜像里，物理上没有。** 三层证据都指向 0：
   文件名搜遍整盘 0 命中；dyld 共享缓存里索引出 **2627 个 image**，路径里带 `mux` 的 **0 个**，
   全 43 个缓存分片里字节串 `usbmuxd` / `com.apple.usbmuxd` / `USBMuxListener` 的出现次数 **各为 0**；
   492 个 LaunchDaemon plist 里没有 `com.apple.usbmuxd.plist`。
   但它的**残迹全在**：`/etc/passwd` 还有 `_usbmuxd:*:213:213:iPhone OS Device Helper:/var/db/lockdown`，
   `/etc/group` 还有 `_usbmuxd:*:213:`，家目录 `/var/db/lockdown` 还在（owner 213:213, 0700, 空），
   `lockdownd` 里还留着 `mux_notification_handler` 和日志串 `Misc usbmuxd event: (messageType=0x%08x, argument=%p)`。
   **结论：它是"该在但被摘掉/没打包"的组件，不是"改名藏到别处"。**

2. **五个服务的接入面是两类，不是一类。** `lockdownd` 是唯一由 launchd 直接给 socket 的
   （TCP **62078** v4+v6 且**不绑 loopback**，加 unix `/var/run/lockdown.sock` mode **0777**）；
   另外四个（afcd / misagent / mobile_installation_proxy / installd）都只有 `MachServices`，
   靠 **check-in** 把自己的 socket 交给 lockdownd——四个里面三个链 `liblockdown.dylib`，
   **`installd` 根本没链 liblockdown**（它压根不跟主机说话，只被 `com.apple.mobile.installd` 这个 mach 服务调用）。

3. **启动顺序不是 launchd 的 `Requires` 写的。** 这五个 plist 里 `Requires` / `RequireSuccessfulExit`
   全是 `-`（492 个 plist 里有 48 个用了 `Requires`，这五个一个都没用）。
   顺序是三条隐式规则叠出来的：`RunAtLoad`（只有 lockdownd 有）→ 主机 `StartSession` 之后才能 `StartService`
   → 被拉起的进程回连 `/var/run/lockdown.sock` 做 check-in（`spawn_and_handle_checkin`）。

4. **Pair 的硬依赖是 keychain（securityd）+ keybag（keybagd→AppleKeyStore）+ activation identity（MobileActivation）。**
   镜像里这三样都没就绪：`/private/var/Keychains` 空、`/private/var/root/Library/Lockdown` 不存在（没有 pair record）、
   `/System/Library/Caches/apticket.der` 是**断链**，而 `/var/keybags/systembag.kb` 只有 227 字节、
   内容是一个手搓的 XML：`<key>KeyBagKeys</key><data>U1dFRVQ=</data>`（base64 = `SWEET`）。
   也就是说**有人已经桩过一次 keybag 了**，但桩得不对（真 keybag 要的是完整的 class key 二进制结构）。

5. **"PC 装 IPA"的最小集是 6 个进程 + 1 条传输链路**，按序：
   `launchd`（前提）→ `keybagd` → `usermanagerd` → `securityd` → `containermanagerd` → `lockdownd`
   → 主机 `StartSession` → `mobile_installation_proxy` →（mach）→ `installd`。
   `afcd` 只在你要**先传 IPA 到设备再装**时才需要；`misagent` 只在装描述文件时需要；
   `lsd`/SpringBoard 只影响图标与可启动性，不影响"装成功"。传输层要么让 USB-NCM 网卡起来走 `IP:62078`，
   要么自己写一个 usbmuxd 桩转发到 `127.0.0.1:62078`。

---

## 1. 方法与证据来源（可复现）

| 目的 | 手段 | 具体命令 |
|---|---|---|
| 拿 Mach-O 类型与全部 load command | llvm 23.1.4 | `llvm-objdump --macho -p <bin>` |
| 拿导入符号（谁调了谁的 check-in） | `llvm-nm` | `llvm-nm -u <bin>` |
| 拿字符串 | `llvm-strings` | `llvm-strings -a <bin>` |
| 拿 entitlements（二进制自带，不是猜的） | 自写 SuperBlob 解析 | `python3 tools/ents2.py <bin>`（见附录 D） |
| launchd 作业定义 | 直接 plistlib 解析 | `plistlib.load(...)`（原文件是 bplist00 二进制 plist） |
| 共享缓存里有哪些 image | 自写 Mach-O header 扫描器 | `python3 tools/cache_index.py` → `cache_index.json`（2627 条） |
| 缓存 image 的依赖 | 直接读 `LC_LOAD_DYLIB` | `python3 tools/macho_lc.py <install_name>` |

本机 `otool` 没有，`llvm-objdump`/`llvm-readobj`/`llvm-nm`/`llvm-strings` 都在
（`pacman -Qo $(which llvm-objdump)` → `/usr/bin/llvm-objdump 由 llvm 23.1.4-1 所拥有`）。
`file` 也能直接认出这几个 Mach-O，结果与 objdump 一致。

> 注意一个坑：**dyld 共享缓存里的 image 不能直接喂 objdump**。
> 我把 `MobileDeviceLink` 的 `__TEXT` 单独切出来存盘后，objdump 报
> `truncated or malformed object (offset field of section 0 in LC_SEGMENT_64 command 0 extends past the end of the file)`——
> 因为缓存 image 的 `__DATA_CONST` / `__LINKEDIT` 的 `fileoff` 指向**别的分片**
> （`__LINKEDIT` 统一在 `.28.dyldlinkedit` / `.42.dyldlinkedit` 的 offset 376832 处，长度 333070336）。
> 所以缓存 image 的依赖一律用自写的 load-command 解析器读，不用 objdump。

### 1.1 顺手确认的一件事：这份 cache 是 arm64，不是 arm64e

2627 个 image 的 `cpusubtype` 统计：`{0x0: 2627}`，全部 `ARM64 / ALL`；
`llvm-objdump --macho -p` 对五个可执行文件也都是 `ARM64 ALL`，`file` 报 `Mach-O 64-bit arm64 executable`。
缓存文件只有 `dyld_shared_cache_arm64*`（没有 `..._arm64e`）。
→ 这套 rootfs 面向 **A11 一类 arm64-only 设备**（iOS 16.7 支持的最低档就是 A11）。

这条对第 5 节很重要：**A11 是带 SEP 的**。所以"缺 SEP"在这台机器上不是"iOS 不支持"，
而是"我们把 iOS 跑在没有 SEP 的 SoC 上"——见 5.3。

---

## 2. 逐个服务

### 2.0 速查表

| 服务 | 路径 | 大小 | Mach-O | dylib 数 | launchd 给的接入面 | lockdown 协议名 |
|---|---|---|---|---|---|---|
| `afcd` | `/usr/libexec/afcd` | 75104 | arm64 EXECUTE PIE | 6 | `MachServices: com.apple.afcd` | `com.apple.afc` / `com.apple.afc.root` |
| `lockdownd` | `/usr/libexec/lockdownd` | 1205152 | arm64 EXECUTE PIE | 39 | **Sockets**: TCP 62078 ×2 + unix `/var/run/lockdown.sock`；**RemoteServices** ×2；**没有 MachServices** | 它就是 `com.apple.mobile.lockdown` 本体 |
| `misagent` | `/usr/libexec/misagent` | 135808 | arm64 EXECUTE PIE | 10 | `MachServices: com.apple.misagent` | `com.apple.misagent` |
| `mobile_installation_proxy` | `/usr/libexec/mobile_installation_proxy` | 112112 | arm64 EXECUTE PIE | 15 | `MachServices: com.apple.mobile.installation_proxy` | `com.apple.mobile.installation_proxy` |
| `installd` | `/usr/libexec/installd` | 608880 | arm64 EXECUTE PIE | 23 | `MachServices: com.apple.mobile.installd` | 无（不走 lockdown） |

五个都是 `MH_MAGIC_64 / ARM64 / cpusubtype ALL / MH_EXECUTE / PIE`，
`LC_BUILD_VERSION: platform ios, minos 16.7, sdk 16.7.3`，
`LC_CODE_SIGNATURE` 都在，`LC_RPATH` **一个都没有**（`grep -l LC_RPATH *.pro.txt` 无输出）——
它们只靠 `/usr/lib`、`/System/Library/Frameworks` 这些默认路径找库，这对替换 dylib 是个好消息。

五个的 `llvm-nm -g --defined-only` 输出**只有 `__mh_execute_header`**（misagent 多一个 `_CopyMockUDID`），
即**它们不导出任何符号**。所以下面的"关键符号"一律取**导入**符号（它们在调什么），这比导出的更能说明职责。

### 2.1 `afcd` —— AFC 文件服务

```
/usr/libexec/afcd          75104 bytes
Mach-O 64-bit arm64 executable, flags:<NOUNDEFS|DYLDLINK|TWOLEVEL|PIE>
ncmds 24  sizeofcmds 2064  platform ios  minos 16.7  sdk 16.7.3
```

**依赖（6 条 `LC_LOAD_DYLIB`）**：`MobileKeyBag.framework`、`liblockdown.dylib`、**`libafc.dylib`**、
`CoreFoundation`、`IOKit`、`libSystem.B`。

**导入的关键符号**（`llvm-nm -u`）：

```
_secure_lockdown_checkin              _lockdown_copy_checkin_info
_lockdown_get_socket                  _lockdown_get_securecontext
_lockdown_disconnect
_AFCCreateServerContext               _AFCServeWithRoot
_AFCServerContextSetRootPath          _AFCServerContextGetRootPath
_AFCServerContextSetLockdownConnection
_AFCServerConnectionSetFileCoordinationEnabled
_xpc_connection_create_mach_service
```

**关键字符串**：

```
@(#)PROGRAM:afcd  PROJECT:afc-276        ← 构建标签，afc-276
usage: afcd [-r]
afcd starting / afcd xpc listener / Creating XPC service %s
Serving directory %s / run_connection for root %s
/private/var/mobile/Media                 ← 默认 AFC 根
/private/var/mobile/Library/Logs/CrashReporter
com.apple.afcd / com.apple.afc / com.apple.afc.root / com.apple.crashreportcopymobile
Checked in with lockdown on path '%s' socket %d with%s SSL
Processing checkin / Checkin handled / Handle lockdown message
```

**entitlements**（从 `LC_CODE_SIGNATURE` 里解出来的原文）：

```xml
<key>com.apple.security.network.client</key><true/>
<key>com.apple.security.network.server</key><true/>
<key>com.apple.security.system-groups</key>
<array><string>systemgroup.com.apple.osanalytics</string></array>
```

**它监听什么**：plist 没给它 `Sockets`，只给了 `MachServices: com.apple.afcd`。实际数据面是
**它自己建一个 XPC listener / socket，然后 `_secure_lockdown_checkin` 交给 lockdownd**，
由 lockdownd 把主机那条连接接过来。`libafc.dylib` 负责协议本身（`_AFCCreateServerContext` 等）。
默认根是 `/private/var/mobile/Media`（不带 `-r` 时），`-r` 才是整盘根；`com.apple.afc.root` 是那个 unrestricted 变体。

`com.apple.afc.unrestricted`（lockdownd 里的名字）与 afcd 里的 `com.apple.afc.root` 是同一档东西的两端写法，
**这条我没法从静态数据里证死，上机要验**（见 §8 第 5 项）。

### 2.2 `lockdownd` —— 唯一的 socket 型服务，也是整条链的枢纽

```
/usr/libexec/lockdownd     1205152 bytes   ARM64 ALL  MH_EXECUTE|PIE
minos 16.7  sdk 16.7.3  39 条 LC_LOAD_DYLIB
```

**依赖按性质分组**（完整清单见附录 A）：

| 组 | 具体 |
|---|---|
| 自己这套 | `liblockdown.dylib`、`MobileKeyBag.framework`、`MobileInstallation.framework`、`MobileStorage.framework`、`Bom.framework` |
| 身份/配对 | **`MobileActivation.framework`**、**`DeviceIdentity.framework`**、`Security.framework`、`LocalAuthentication.framework` |
| 远程 | **`RemoteServiceDiscovery.framework`**、**`RemotePairingDevice.framework`**、`CoreUtils`、`CFNetwork` |
| 杂 | `UserManagement`、`CoreTelephony`、`CrashReporterSupport`、`CoreDuet`、`FindMyDevice`、`CoreServices`、`SpringBoardServices`、`ManagedConfiguration`、`Foundation`、`ServiceManagement`、`AppSupport`、`CoreFoundation`、`IOKit`、`SystemConfiguration`、`MobileWiFi`、`CoreTime`、`IntlPreferences` |
| libSystem 系 | `libAccessibility`、`libz.1`、`libMobileCheckpoint`、`libMobileGestalt`、`libMobileGestaltExtensions`、`libamsupport`、`libobjc.A`、`libSystem.B`、`libc++.1` |

**plist 是整篇文档里最关键的一块**（原文件是 bplist00，下面是解析后的等价 XML）：

```xml
<key>Label</key>              <string>com.apple.mobile.lockdown</string>
<key>ProgramArguments</key>   <array><string>/usr/libexec/lockdownd</string></array>
<key>RunAtLoad</key>          <true/>
<key>MinimalBootProfile</key> <true/>
<key>POSIXSpawnType</key>     <string>Interactive</string>
<key>ServiceIPC</key>         <false/>
<key>Sockets</key>
<dict>
  <key>MyIPv4Socket</key><dict><key>SockFamily</key><string>IPv4</string>
                             <key>SockServiceName</key><string>62078</string></dict>
  <key>MyIPv6Socket</key><dict><key>SockFamily</key><string>IPv6</string>
                             <key>SockServiceName</key><string>62078</string></dict>
  <key>MyUnixSocket</key><dict><key>SockFamily</key><string>Unix</string>
                             <key>SockPathName</key><string>/var/run/lockdown.sock</string>
                             <key>SockPathMode</key><integer>511</integer></dict>
</dict>
<key>RemoteServices</key>
<dict>
  <key>com.apple.mobile.lockdown.remote.trusted</key>
  <dict><key>ExposedToUntrustedDevices</key><false/>
        <key>RequireEntitlement</key><string>com.apple.mobile.lockdown.remote.trusted</string>
        <key>ServiceProperties</key><dict><key>ServiceVersion</key><integer>1</integer>
                                         <key>UsesRemoteXPC</key><true/></dict></dict>
  <key>com.apple.mobile.lockdown.remote.untrusted</key>
  <dict><key>ExposedToUntrustedDevices</key><true/>
        <key>RequireEntitlement</key><string>com.apple.mobile.lockdown.remote.untrusted</string>
        <key>ServiceProperties</key><dict><key>ServiceVersion</key><integer>1</integer>
                                         <key>UsesRemoteXPC</key><true/></dict></dict>
</dict>
<key>LaunchEvents</key>
<dict>
  <key>com.apple.iokit.matching</key>
  <dict>
    <key>com.apple.lockdown.USB2.Device</key>
    <dict><key>IOProviderClass</key><string>IOPortTransportState</string>
          <key>IOPropertyMatch</key><dict><key>Active</key><true/><key>DataRole</key><integer>1</integer>
                                        <key>TransportType</key><integer>2</integer></dict>
          <key>IOMatchAll</key><true/><key>IOMatchLaunchStream</key><true/></dict>
    <key>com.apple.lockdown.USB3.Device</key>  <!-- 同上，TransportType 3 -->
    <key>com.apple.lockdown.KIS</key>          <!-- 同上，TransportType 11 -->
    <key>com.apple.lockdown.USBHost</key>
    <dict><key>IOProviderClass</key><string>IOResources</string>
          <key>IOPropertyMatch</key><dict><key>AppleUSBCableType</key><string>USBHost</string></dict>
          <key>IOMatchAll</key><true/><key>IOMatchLaunchStream</key><true/></dict>
  </dict>
  <key>com.apple.notifyd.matching</key>
  <dict>  <!-- 5 个 Darwin 通知：applicationRegistered / applicationUnregistered /
             datamigrator.migrationDidFinish / mobile.disk_image_mounted / mobile.storage_unmounted -->
  </dict>
</dict>
<key>_PanicOnCrash</key><dict><key>AutomationOnly</key><true/><key>PanicOnConsecutiveCrash</key><true/></dict>
```

**三个可以直接落地的细节**：

1. `SockFamily: IPv4` + `SockServiceName: 62078`，**没有 `SockNodeName`** → launchd 绑的是
   `INADDR_ANY:62078`，不是 `127.0.0.1`。所以只要设备侧有一张能通到 PC 的网卡（USB-NCM 就算），
   PC 直连 `<设备IP>:62078` 就能打，**不需要 usbmux 协议**。这是 §6 里最省事的一条路。
2. `SockPathMode: 511` = `0777` → `/var/run/lockdown.sock` 任何用户都能连。这是 check-in 通道。
3. **`lockdownd` 的 plist 里没有 `MachServices`**。这点很反直觉：afcd 那些都有，唯独枢纽没有。
   因为 lockdownd 不走 mach 服务上架自己，它靠 (a) 上面那两个 socket，(b) `RemoteServices`（RemoteXPC 上架）。

**服务分发表（`StartService` 认哪些名字）**——下面这些名字在 `lockdownd` 里是连续存放的
（文件偏移 665625–666000，`__const` 段内）：

```
com.apple.mobile.insecure_notification_proxy
com.apple.preboardservice_v2
com.apple.afc
com.apple.afc.unrestricted
com.apple.mobile.installation_proxy
com.apple.mobile.file_relay
com.apple.mobile.FactoryServices
```

`/Library/Lockdown/ServiceAgents`（按服务名拆 plist 的目录）在**这份镜像里不存在**
（`ls Library/Lockdown` → 没有那个文件或目录；`ls System/Library/Lockdown/ServiceAgents` 同样），
所以 iOS 走**编译进二进制的内建表**，不是磁盘上的 ServiceAgents。

**`StartService` 的实现路径（符号被 strip 了，但函数名以日志串形式留下来了）**：

```
handle_start_session / handle_stop_session / handle_start_service
handle_start_service_with_socket / spawn_and_handle_checkin
register_launchd_job / checkin_with_launchd / checkin_with_launchd_block_invoke
_launch_socket_service_check_in / launch_socket_service_check_in failed: %d (%s)
Couldn't create launchd plist for %@        ← 运行时现造 plist
Failed to register service '%@' with launchd.
Failed to spawn '%@' service agent: %@
%@ attempting to spawn %@ service.
The connection is not within a session. We cannot start services.
Service %@ did not acknowledge checkin before connection was closed.
Service %@ has taken longer than one minute to acknowledge checkin.
Received checkin acknowledgement from '%@'
Cannot start lockdown services via lockdown remote service. Use remote service discovery.
```

**导入的关键符号**：

```
_bootstrap_look_up / _bootstrap_port          ← 需要真正的 launchd bootstrap
_launch_socket_service_check_in               ← liblaunch，向 launchd 做 socket check-in
_xpc_connection_create_mach_service
_MKBDeviceUnlockedSinceBoot / _MKBGetDeviceLockState / _MKBKeyBagCreateWithData
_MKBKeyBagUnlock / _MKBKeyBagCopyData / _MKBKeyBagCreateEscrow / _MKBUserSessionIsLoginWindow
_SecItemAdd / _SecItemCopyMatching / _SecItemDelete / _SecKeyCreateRandomKey
_SecKeyCreateRSAPrivateKey / _SecIdentityCreate / _SecCertificateCopyData / _SecTrustEvaluateWithError
_MAECopyLegacyDeviceIdentityWithError         ← MobileActivation，设备身份证书从这里来
_RemotePairingCopyRemoteUnlockDeviceKeyForTunnel / _RemotePairingGetUSBConnectedHostTrustState
_remotepairingAllowPromptlessPairing / _remotepairing_delete_pair_records
```

check-in 用的两个字典 key 是**字符串常量**而不是导入符号（`llvm-nm -u` 里没有，`llvm-strings` 里有）：
`_LDCHECKININFO`、`_LDCHECKINDICT`——和 `_launch_socket_service_check_in` 配套用。

注意**没有**任何 `_AKS*`（AppleKeyStore）直连导入——SEP 相关的东西全部经
`MobileKeyBag.framework` → `keybagd` → `AppleKeyStore` 这一层间接走。这对桩化是好事（见 §7）。

**entitlements（全量见附录 B）**——这几条直接告诉我们它需要谁：

```
com.apple.private.security.storage.Lockdown        ← 需要 securityd 给它开 Lockdown 存储
com.apple.private.securityd.ae.lockdown
com.apple.private.system-keychain
keychain-access-groups: [com.apple.mobile.lockdownd, lockdown-identities]
com.apple.private.applesepmanager.allow            ← 明确点名 SEP manager
com.apple.private.applecredentialmanager.allow
com.apple.mkb.usersession.info
com.apple.mobileactivationd.spi                    ← 激活身份
com.apple.dt.remotepairing.pairingmanagement / .tunnelmanagement
com.apple.private.RemoteServiceDiscovery.device-admin
com.apple.security.exception.mach-lookup.global-name:
    [com.apple.remoted, com.apple.pcapd, com.apple.nearbyd.xpc.diagnostics,
     com.apple.nfcd.hwmanager, com.apple.PairingManager,
     com.apple.commcenter.mobile-helper-cbupdateservice, com.apple.carkit.service.lockdown,
     com.apple.amfi.lockdown, com.apple.backgroundassets.lockdownservice]
seatbelt-profiles: [lockdownd]
```

### 2.3 `misagent` —— 描述文件（provisioning profile）代理

```
/usr/libexec/misagent      135808 bytes
依赖 10 条：IOKit, CoreFoundation, libsqlite3, Security, libMobileGestalt,
            liblockdown, libmis, Foundation, libobjc.A, libSystem.B
```

**导入的关键符号**：`_secure_lockdown_checkin`、`_lockdown_copy_checkin_info`、
`_lockdown_recv`、`_lockdown_send`、`_lockdown_disconnect`、`_xpc_connection_create_mach_service`、
以及 `libmis.dylib` 的 `_MISProvisioningProfile*` 全家桶
（`GetUUID`/`GetName`/`GetTeamIdentifier`/`GetEntitlements`/`GetExpirationDate`/
`GetDeveloperCertificatesHashes`/`GetEmbeddedDER`/`ValidateSignature`/`Installed`/`Removed`）。

**协议名与消息**：`com.apple.misagent`（也是 mach 服务名）、`com.apple.misagent.worker`；
lockdown 侧的消息名是 **`Install` / `Remove` / `CopyAll`**（还有 `Provisioning`、`Profile`、`ProfileID` 等 key）。

**存储**：`/Library/MobileDevice/ProvisioningProfiles` + 同目录下 `/mis.db`（sqlite3）。
建表语句在二进制里是明文，`profiles` 表长这样：

```sql
CREATE TABLE profiles ( uuid TEXT NOT NULL PRIMARY KEY, team_id TEXT NOT NULL,
  install_time TEXT DEFAULT CURRENT_TIMESTAMP, name TEXT NOT NULL, expires INTEGER,
  is_for_all_devices INTEGER, is_apple_internal INTEGER, is_local INTEGER, is_beta INTEGER,
  cms_blob BLOB NOT NULL)
```

镜像里这个目录**存在且是空的**：`/private/var/MobileDevice/ProvisioningProfiles`（owner 501:501, 空）。
`/Library/MobileDevice` 是指向 `../private/var/MobileDevice` 的符号链接，两边对得上。

**entitlements**：`com.apple.private.mis.profiles.write`、
`com.apple.private.security.storage.MobileIdentityService`、
`com.apple.security.iokit-user-client-class: [AppleMobileFileIntegrityUserClient]`
→ 它要碰 AMFI 的 user client。另有 `com.apple.private.mis.beta-install` 的检查
（串：`Attempted to install a Beta profile without the proper entitlement.`、
`Attempt to install a Beta profile over lockdown connection.`）。

### 2.4 `mobile_installation_proxy` —— 主机 `installation_proxy` 的落点

```
/usr/libexec/mobile_installation_proxy    112112 bytes
依赖 15 条：UserManagement, InstallCoordination, CoreServices, CoreTelephony, CoreData,
            libMobileGestalt, ManagedConfiguration, CoreFoundation, MobileInstallation,
            MobileSystemServices, liblockdown, Security, Foundation, libobjc.A, libSystem.B
```

**导入的关键符号**（跟 afcd/misagent 的写法不一样，它是 XPC 版 check-in）：

```
_lockdown_checkin_xpc            ← 不是 _secure_lockdown_checkin
_lockdown_receive_message / _lockdown_send_message / _lockdown_disconnect
```

**协议名**：`com.apple.mobile.installation_proxy`（协议 + mach 服务名）、
`com.apple.mobile_installation_proxy`（mach 服务名，和上面**不是同一个串**）、
内部还带 `com.apple.mobile_installation.metadata`。

**handler 全表**（字符串里就是函数名）：

```
handle_browse             → Browse
handle_install            → Install / InstallLocalProvisioned
handle_lookup             → Lookup
handle_uninstall          → Uninstall
handle_restore            → Restore
handle_check_capabilities → CheckCapabilitiesMatch
```

**它的下游**：

- `InstallCoordination.framework`（`installApplication:consumeSource:options:legacyProgressBlock:completion:`）
  → 对应守护进程 `com.apple.installcoordinationd`（plist 里 `MachServices: com.apple.installcoordinationd`）；
- `MobileInstallation.framework` → `MIInstallerClient` → mach 服务 **`com.apple.mobile.installd`**
  （`MobileInstallation` 二进制里的串：`com.apple.mobile.installd`、`com.apple.mobileinstallation_client`）；
- `LSApplicationWorkspace`（`_OBJC_CLASS_$_LSApplicationWorkspace`）→ LaunchServices 查询/注册。

**一条很硬的限制**：串 `Installing system apps from the host is prohibited.` —— 主机不能装系统 App。

**entitlements**：

```xml
<key>com.apple.private.InstallCoordination.allowed</key><true/>
<key>com.apple.private.mobileinstall.allowedSPI</key>
<array><string>Lookup</string><string>InstallForLaunchServices</string>
       <string>UninstallForLaunchServices</string><string>Browse</string>
       <string>CheckCapabilitiesMatch</string><string>CopyDiskUsageForLaunchServices</string>
       <string>InstallLocalProvisioned</string></array>
<key>com.apple.mobile.keybagd.UserManager.bubblepop</key><true/>
<key>com.apple.usermanagerd.persona.fetch</key><true/>
<key>com.apple.private.MobileContainerManager.otherIdLookup</key><true/>
<key>com.apple.atvcached.client</key><true/>
```

### 2.5 `installd` —— 真正落盘的那个，而且它不认识主机

```
/usr/libexec/installd      608880 bytes
依赖 23 条：libmis, libParallelCompression, DataMigration, MobileStorage, StreamingZip,
            ImageIO, UniformTypeIdentifiers, IconServices, RunningBoardServices, SoftLinking,
            InstalledContentLibrary, UserManagement, Security, MobileInstallation, libbz2.1.0,
            IOKit, Foundation, libMobileGestalt, MobileSystemServices, CoreFoundation,
            CoreServices, libobjc.A, libSystem.B
```

**最关键的否定事实：`installd` 没有 `liblockdown.dylib`。**
`llvm-nm -u installd | grep -E '^_(secure_)?lockdown|^_bootstrap|^_launch_socket'` → **空**。
它是纯 mach 服务：只对外开 `com.apple.mobile.installd`，只被 `MobileInstallation`/`InstallCoordination` 调。

**它要的下游 / 要的特权**（字符串 + entitlements）：

| 依赖面 | 证据 |
|---|---|
| 容器管理 | `entitlement com.apple.private.MobileContainerManager.allowed` → containermanagerd |
| AMFI 信任缓存 | `codeSigningInfoByValidatingResources:performingOnlineAuthorization:ignoringCachedSigningInfo:checkingTrustCacheIfApplicable:skippingProfileIDValidation:error:` 加 `entitlement com.apple.private.amfi.can-check-trust-cache` |
| 描述文件校验 | `libmis.dylib` + `com.apple.installd.MIFreeProfileValidatedAppTracker` |
| FairPlay | `entitlement fairplay-client: 2033844765`、`ApplicationSINF`、`needsSinf`、`Failed to read SINF from %@` |
| LaunchServices 注册 | `com.apple.installd.LaunchServicesRegistrationQueue`、`_beginLaunchServicesRegistrationWithError:`、`com.apple.private.coreservices.can-register-install-results`、`...can-perform-rebuild-registration` |
| 进程管理 | `RunningBoardServices`、`entitlement com.apple.multitasking.termination` |
| 解压 | `StreamingZip`、`libParallelCompression`、`libbz2.1.0`、`InstalledContentLibrary` |

**plist 里有个和别的四个都不一样的地方**：`UserName` 和 `GroupName` 都是 `_installd`
（`/etc/passwd`: `_installd:*:33:33:Install Daemon:/var/installd:/usr/bin/false`），
另外三个是 `mobile`（501）。只有 lockdownd 是 root。

---

## 3. 启动依赖：谁先起、谁把谁挂出来

### 3.1 先把"没有的东西"说清楚

- 五个 plist 的 `Requires` / `RequireSuccessfulExit`：**全部没有**（逐条脚本验证过）。
- 492 个 LaunchDaemon plist 里有 48 个用 `Requires`；反向查"谁 requires 我们"——
  `grep -rl -e 'com.apple.mobile.lockdown' -e 'com.apple.afcd' -e 'com.apple.misagent' \
   -e 'com.apple.mobile.installd' -e 'com.apple.mobile_installation_proxy' LaunchDaemons/`
  只命中**它们自己的 plist**（因为 plist 里写着 `ProgramArguments` 的路径名）
  → 也就是说：**没有任何别的作业声明"要等我先起来"。**
- `/System/Library/LaunchAgents` 目录在这份镜像里不存在（`ls` 报 0 项），`/Library/LaunchAgents` 也没有相关内容。

所以顺序完全由 **launchd 的 socket/RunAtLoad 语义 + 主机的会话时序 + check-in 握手** 三者决定。

### 3.2 三层激活面

| 层 | 谁 | 触发方式 | 证据 |
|---|---|---|---|
| L0 | `launchd` | 内核起，pid 1，提供 bootstrap + socket 代持 | `/sbin/launchd`（541600 bytes）存在；lockdownd 导入 `_bootstrap_look_up` / `_launch_socket_service_check_in` |
| L1 | `lockdownd` | `RunAtLoad` + `MinimalBootProfile`，launchd 已代持 62078 与 `/var/run/lockdown.sock` | plist 见 §2.2 |
| L2 | `afcd` / `misagent` / `mobile_installation_proxy` | 只有 `MachServices` + `EnablePressuredExit`（按需），由 lockdownd 的 `StartService` 经 launchd 拉起 | 三个 plist 都只有 `MachServices`，没有 `RunAtLoad` |
| L2' | `installd` | 只有 `MachServices`，**由 `MobileInstallation` 的 mach 调用拉起**，不是 lockdownd 拉 | installd 无 liblockdown 依赖 |
| L1' | `keybagd` / `usermanagerd` / `securityd` | `RunAtLoad` / `MinimalBootProfile` | 见 §3.3 与附录 C |

### 3.3 上游三个的 plist（§6 也会用到）

```xml
<!-- com.apple.mobile.keybagd -->
<key>MachServices</key><dict><key>com.apple.mobile.keybagd.mach</key><true/>
                             <key>com.apple.mobile.keybagd.xpc</key><true/></dict>
<key>ProgramArguments</key><array><string>/usr/libexec/keybagd</string>
                                 <string>-t</string><string>15</string></array>
<key>RunAtLoad</key><true/>   <key>POSIXSpawnType</key><string>Interactive</string>

<!-- com.apple.mobile.usermanagerd -->
<key>MachServices</key><dict><key>com.apple.mobile.usermanagerd.xpc</key><true/>
                             <key>com.apple.mobile.keybagd.UserManager.xpc</key><true/>
                             <key>com.apple.system.libinfo.muser</key><true/></dict>
<key>ProgramArguments</key><array><string>/usr/libexec/usermanagerd</string>
                                 <string>-t</string><string>15</string></array>
<key>RunAtLoad</key><true/>

<!-- com.apple.securityd -->
<key>UserName</key><string>_securityd</string>
<key>GroupName</key><string>_securityd</string>
<key>MinimalBootProfile</key><true/>
<key>ProgramArguments</key><array><string>/usr/libexec/securityd</string></array>
<key>MachServices</key><dict>
  <key>com.apple.security.escrow-update</key><true/>  <key>com.apple.security.octagon</key><true/>
  <key>com.apple.securityd</key><true/>               <key>com.apple.securityd.aps</key><true/>
  <key>com.apple.securityd.ckks</key><true/>          <key>com.apple.securityd.general</key><true/>
  <key>com.apple.securityd.sos</key><true/>
</dict>
```

### 3.4 时序图

```
kernel
  │
  ├─ launchd(1) ── 读 492 个 LaunchDaemon plist
  │      ├─ 代持 socket: 0.0.0.0:62078(v4/v6) + /var/run/lockdown.sock(0777)
  │      ├─ spawn com.apple.mobile.lockdown    (RunAtLoad)
  │      ├─ spawn com.apple.mobile.keybagd     (RunAtLoad)
  │      ├─ spawn com.apple.mobile.usermanagerd(RunAtLoad)
  │      └─ spawn securityd / containermanagerd / runningboardd (按需或 RunAtLoad)
  │
  ├─ lockdownd 起来 → accept(62078)
  │
  │   ← 主机（PC）连上来（USB-NCM 的 IP:62078，或自己写的 usbmuxd 桩转发到 127.0.0.1:62078）
  │
  ├─ 【主机】QueryType / GetValue(Label)          ── lockdownd 直接答
  ├─ 【主机】StartSession + EnableSessionSSL      ── 要 pair record（§5）
  │        │  handle_start_session / EnableSessionSSL
  │        └─ 失败串：Session already active. / No pairing record for %@.
  │
  ├─ 【主机】StartService(com.apple.mobile.installation_proxy)
  │        │  handle_start_service → 查内建表 → register_launchd_job
  │        │  launchd 按 MachServices 拉起 mobile_installation_proxy
  │        │  mobile_installation_proxy → _lockdown_checkin_xpc → /var/run/lockdown.sock
  │        │  lockdownd: spawn_and_handle_checkin → 收 fd → 回 ACK
  │        └─ lockdownd 把这条 fd 接到主机 socket 上（可选 EnableServiceSSL）
  │
  ├─ 【主机】如果要传文件：StartService(com.apple.afc) → afcd _secure_lockdown_checkin
  │        → 传到 /private/var/mobile/Media/...
  │
  └─ 【主机】Install(package path)
           → mobile_installation_proxy → MobileInstallation.framework(MIInstallerClient)
           → mach 服务 com.apple.mobile.installd → launchd spawn installd (_installd, uid 33)
           → containermanagerd 建容器 → libmis/AMFI 验签 + (FairPlay 时) sinf
           → LaunchServices 注册队列（图标/可启动性）
```

### 3.5 顺序上的硬约束（每条都有串支撑）

1. **`StartSession` 必须先于 `StartService`**：
   `The connection is not within a session. We cannot start services.`
2. **`StartService` 认的名字是白名单**：§2.2 那 7 个 + `com.apple.misagent` 等。
   不在表里就走 `handle_start_service_with_socket` 的通用分支（要求 `Service` + `Options` 里给 socket）。
3. **被拉起的进程必须在一分钟内 check-in**：
   `Service %@ has taken longer than one minute to acknowledge checkin.`
   → 桩化 afcd/mip 时如果只建 socket 不 check-in，lockdownd 会 60s 后放弃。
4. **`installd` 不在 lockdownd 的拉取路径上**：它由 `MobileInstallation` 的 mach 调用激活，
   所以"lockdownd 起来了"不代表 installd 起来了。
5. **有些服务要求设备已解锁**：`Device not unlocked since boot.` /
   `Device not unlocked within required time.` / `/keybag/isLocked`。
   这条直接卡在 keybag 上（§5）。

---

## 4. usbmuxd 到底在哪

### 4.1 负面证据（三层，全部 0 命中）

| 层 | 做法 | 结果 |
|---|---|---|
| 文件系统 | `find /mnt/k60root -iname '*usbmux*'` | **无输出**（rc=1） |
| launchd | `ls System/Library/LaunchDaemons/*.plist \| grep -i usb` | 只有 `com.apple.usb.networking.addNetworkInterface.plist` 与 `com.apple.usbsmartcardreaderd.plist`，**没有 `com.apple.usbmuxd.plist`** |
| 全盘字节串 | `grep -rl --binary-files=text 'usbmuxd' usr bin sbin etc System/Library/{LaunchDaemons,PrivateFrameworks,Frameworks,CoreServices} Library var/db/...` | **只有 3 个文件命中**：`usr/libexec/lockdownd`、`etc/group`、`etc/passwd` |
| 共享缓存·路径 | 自写扫描器索引 43 个分片 → 2627 个 image；筛 `'mux' in ident.lower()` | **0 个** |
| 共享缓存·字节串 | python 全读 3.2 GB，逐 pattern `count()` | `usbmuxd`=**0**，`com.apple.usbmuxd`=**0**，`USBMuxListener`=**0** |
| 共享缓存·符号表 | `grep -c -a usbmuxd dyld_shared_cache_arm64.symbols` | **0** |

对照（证明这套搜法本身有效）：同一套 `count()` 对 `liblockdown.dylib` 得 **57**、
`MobileDeviceLink` 得 **56**、`com.apple.mobile.lockdown` 得 **22**。
所以 0 就是 0，不是"搜法失灵"。

### 4.2 `usbmux` 一共只出现 5 次，全是旁证级

全缓存 3.2 GB 里，小写 `usbmux` **只有 1 次**：

```
dyld_shared_cache_arm64.37 @ 42645443
  ... Connection via direct usbmux is deprecated. SocketStreamHandlerConnect: Hostname in
  info dictionary is invalid: %@ ...
```

这条落在 `MobileDeviceLink` 的 `__TEXT` 里（该 image 的 `__TEXT` 在 `.37` 的 offset 42569728、长 77816，
42645443 正好在区间内），完整串是：

> `SocketStreamHandlerConnect: Connection via direct usbmux is deprecated.`

大写 `USBMux` **4 次**，全是误报：

```
dyld_shared_cache_arm64.42.dyldlinkedit / dyld_shared_cache_arm64.symbols:
  __ZN12capabilities2bb3abm16supportsBBUSBMuxEv         ← 基带(baseband)能力查询
```

### 4.3 `MobileDeviceLink.framework` 是什么（它是唯一候选，但不是 usbmuxd）

```
磁盘上只有壳：/System/Library/PrivateFrameworks/MobileDeviceLink.framework/
   Info.plist (826 B) + _CodeSignature/CodeResources     ← 没有可执行文件
二进制在缓存里：/System/Library/PrivateFrameworks/MobileDeviceLink.framework/MobileDeviceLink
   → dyld_shared_cache_arm64.37 @ 42569728
```

`Info.plist`：

```
CFBundleName            = DeviceLink
CFBundleIdentifier      = com.apple.DeviceLink
CFBundleExecutable      = MobileDeviceLink
CFBundleShortVersionString = 5.0
MinimumOSVersion        = 16.7
UIRequiredDeviceCapabilities = [arm64]
```

它自己的依赖（读 `LC_LOAD_DYLIB`，共 6 条，第一条是 `LC_ID_DYLIB` 自己）：
`liblockdown.dylib`、`Security`、`IOKit`、`CoreFoundation`、`libSystem.B`。
`LC_BUILD_VERSION: platform ios, minos 16.7, sdk 16.7.3`。

它的字符串是 **`DL*` 客户端 API**，不是 daemon：

```
DLConnect / DLDisconnectMessage / DLAcceptMessage / DLConnectionLostMessage
DLCreateDeviceLinkConnectionForComputer   ← 明确是"连到另一台机器"的那一侧
DLCreateDeviceLinkConnectionForDevice
DLContentsOfDirectory / DLCopyItem / DLCreateDirectory / DLDownloadFiles
DLLockdownXPCCheckin / lockdown_checkin_xpc, serviceName:%{public}s
lockdown_checkin_xpc failed: %d / lockdown_checkin_xpc timed out after %llds
/Library/Caches/com.apple.xbs/Sources/DeviceLink/WireProtocol/SocketStreamHandler.c
com.apple.DeviceLink / com.apple.DeviceLink.%@.%@
```

**判定：`MobileDeviceLink` 是"设备端去连另一台设备的客户端库"**（host 侧 DeviceLink/MobileDevice 的对偶），
它**也依赖 liblockdown、也走 check-in**，说明它是 lockdown 的**顾客**，不是 mux 的**提供者**。
把它当 usbmuxd 会走偏。

### 4.4 那为什么这个镜像没有 usbmuxd？留下的三条替代通道

**"不在"是事实；"为什么不在"是推断，我把证据摆出来，标清楚哪句是推断。**

硬事实：

1. `_usbmuxd` 账号与家目录**都还在**：
   ```
   /etc/passwd: _usbmuxd:*:213:213:iPhone OS Device Helper:/var/db/lockdown:/usr/bin/false
   /etc/group:  _usbmuxd:*:213:
   /var/db/lockdown    drwx------ 213 213       ← 存在，空
   ```
   家目录被建出来并 chown 到 213，说明**安装/打包阶段它本来是要起的**。
2. `lockdownd` 里还留着 usbmux 的通知回调：
   ```
   mux_notification_handler
   Misc usbmuxd event: (messageType=0x%08x, argument=%p)
   ```
   （文件 offset 672793 附近，紧邻 `peer_is_safe` / `Could not create lockconn`）
3. lockdownd 的监听面**不是 loopback**（`SockFamily IPv4` 且无 `SockNodeName`），
   并且 plist 里有 `LaunchEvents: com.apple.iokit.matching → IOPortTransportState`
   （USB2 / USB3 / KIS / USBHost 四种，`TransportType` 2 / 3 / 11）。
4. 镜像里有一套 USB 以太网卡的拉起作业：
   ```
   com.apple.usb.networking.addNetworkInterface
     /usr/sbin/addNetworkInterface -x -s
       -c AppleUSBNCMData -c AppleUSBEthernetController -c AppleUSBEthernetHost
       -c AppleUSBDeviceNCMData -c AppleUSBECMData -c AppleEthernetAquantiaAqtion -c ...
     LaunchEvents: com.apple.iokit.matching / IONetworkInterface（IOMatchLaunchStream）
   ```
5. lockdownd 自己上架了 `com.apple.mobile.lockdown.remote.{trusted,untrusted}`，
   `ServiceProperties = {ServiceVersion 1, UsesRemoteXPC: true}`——这是 RSD（RemoteServiceDiscovery）路线。
   而 `com.apple.dt.remotepairingdeviced` 另开了 `TCPControlChannelSocket`（`SockFamily IPv4v6, SockProtocol TCP`）
   与 `com.apple.internal.dt.coredevice.untrusted.tunnelservice`（`UsesRemoteXPC: true, ServiceVersion 2`）。

**推断（标注清楚）**：iOS 16 起这把 USB 承载从"usbmuxd 做 USB↔TCP 桥"改成了
"USB-NCM 网卡 + wildcard 62078 + RemoteXPC"，所以我们拿到的这份 rootfs 里 usbmuxd 被摘掉了。
如果这个推断成立，**我们不需要补 usbmuxd**：让 `addNetworkInterface` 把 NCM 拉起来，
PC 直接 `IP:62078` 就能跟 lockdownd 说话（§6 路径 A）。
如果 NCM 在我们这套 USB gadget 上起不来，就自己写一个最小 usbmuxd 桩（§6 路径 B）——
协议本身很简单（16 字节头 + 长度前缀的 plist），我们只需要它做"USB 包 ↔ 127.0.0.1:62078"。

---

## 5. 配对（Pair）：缺 SEP 时断在哪一步、能不能桩

### 5.1 Pair 用到的全部状态（都在 lockdownd 里）

| 东西 | 路径 / 来源 | 镜像里的状态 |
|---|---|---|
| pair record 目录 | `<userhome>/Library/Lockdown/pair_records/`（串：`/Library/Lockdown/pair_records`、`/Library/Lockdown/pair_records/`） | `/private/var/root/Library/Lockdown` **不存在**（root 的 Library 下只有 `Preferences`） |
| 未信任主机表 | `/private/var/root/Library/Lockdown/untrusted_hosts.plist` | 不存在 |
| 设备私钥/公钥 PEM | `/Library/Lockdown/device_private_key.pem`、`device_public_key.pem` | 不存在 |
| escrow | `/Library/Lockdown/escrow_records`、`/Library/Lockdown/data_ark.plist` | 不存在 |
| 设备身份证书链 | `_MAECopyLegacyDeviceIdentityWithError`（MobileActivation）+ `DeviceIdentity.framework`；锚点 `/System/Library/Lockdown/iPhoneDeviceCA.pem`、`iPhoneCA.pem`、`iPhoneDebug.pem` | CA pem **在**（4 个 + `SBOOT_S5L8900.pem`）；但 `/System/Library/Caches/apticket.der` → `../../../usr/standalone/firmware/apticket.der` 是**断链**（目标不存在） |
| keybag | `MKBKeyBag*`（MobileKeyBag）→ `keybagd` → `AppleKeyStore` | `/var/keybags/systembag.kb` **227 B 的桩**（见 5.2） |
| keychain | `_SecItemAdd` 等 → securityd；`kSecUseSystemKeychain` + `kSecAttrAccessibleAlwaysThisDeviceOnlyPrivate` + access group `lockdown-identities` | `/private/var/Keychains` **空目录**；`/var/mobile/Library/Keychains`、`/var/root/Library/Keychains` 都不存在 |

### 5.2 那个 227 字节的 systembag 是个手搓桩（重要发现）

```
$ sudo xxd /var/keybags/systembag.kb
00000000: 3c3f 786d 6c20 7665 7273 696f 6e3d 2231  <?xml version="1
...
000000a0: 3022 3e3c 6469 6374 3e3c 6b65 793e 4b65  0"><dict><key>Ke
000000b0: 7942 6167 4b65 7973 3c2f 6b65 793e 3c64  yBagKeys</key><d
000000c0: 6174 613e 5531 6446 5256 513d 3c2f 6461  ata>U1dFRVQ=</da
000000d0: 7461 3e3c 2f64 6963 743e 3c2f 706c 6973  ta></dict></plis
```

base64 `U1dFRVQ=` = ASCII **`SWEET`**（5 字节）。

真系统的 `systembag.kb` 是 XML plist 没错，但 `KeyBagKeys` 里应该是完整的二进制 keybag
（若干 class key + UUID + 卷 KEK），长度以 KB 计。所以：

- **格式外壳对了**（说明桩的人知道 keybag 是"XML plist 包一层 data"），
- **内容是个占位符**（`SWEET`）——`MKBKeyBagUnlock` / `MKBKeyBagCreateWithData` 解析这 5 字节必然失败。

顺带一条时间戳旁证：`/var/keybags/systembag.kb`、`/var/db/lockdown`、`/var/run`、`/private/var/run`
的 mtime 是 **10 月 24 日 15:07**，而 rootfs 自己的文件是 **4 月 30 日**。
→ 这些是镜像做出来之后**被跑过一次**留下的，不是打包时带的。

### 5.3 缺 SEP 时，到底哪几个调用会失败

把 lockdownd 的 Pair 路径摊开，按"第一个断点"排序：

| 序 | 调用 | 依赖 | 缺 SEP / 空 keychain 时的表现（串） |
|---|---|---|---|
| 1 | `lockcrypto_genenerate_keypair` → `_SecKeyCreateRandomKey`（system keychain，`...ThisDeviceOnlyPrivate`） | securityd + keybag 的 class key | `Keychain not available, no key generation attempted.` → `Failed to create private key.` |
| 2 | `copy_pairing_private_key` → `_SecItemCopyMatching`（group `lockdown-identities`） | 同上 | `Failed to retrieve private key from keychain: %d` |
| 3 | `copy_keys_and_certs` → `_MAECopyLegacyDeviceIdentityWithError` | MobileActivation + 芯片个性化证书 | `Failed to load legacy device identity: %@` |
| 4 | keybag 解锁 | `MKBKeyBagUnlock` → keybagd → `AppleKeyStore`（IOKit，SEP） | `Can't unlock escrow bag: %d` / `Escrow records are not available before first unlock.` |
| 5 | 解锁状态门禁 | `_MKBDeviceUnlockedSinceBoot` | `Device has not been unlocked since boot.` / `Device not unlocked within required time.` / `/keybag/isLocked` |
| 6 | 信任弹窗 | `ask_user_to_trust` → `CFUserNotificationReceiveResponse`（`TRUST_DIALOG_HEADER/MESSAGE/ALLOW_TRUST/DENY_TRUST`、`PTP_DIALOG_ALLOW_TRUST`） | `Failed to create user notification.` / `Asking the user if they want to pair.` |
| 7 | SINF / FairPlay（装 App 时才走） | installd 的 `fairplay-client: 2033844765`、`ApplicationSINF`、`needsSinf` | `Failed to read SINF from %@` / `Client requesting patch installation did not supply a new SINF for an app that requires one to function` |

**七条里：1/2/3 是"配对直接过不去"；4/5 是"服务起得来但一堆操作被 gate 住"；
6 是"没 UI 就没人点同意"；7 只影响 FairPlay 加密的包。**

另外两条旁证，说明这条路本来就要靠平台：
`applesepmanager.allow` 这个 entitlement 只在 lockdownd 上出现；
`keybagd` 自己在字符串里直接点名 `AppleKeyStore`（`Can't register for notifications with AppleKeyStore` /
`aks_unlock_device() -> %d` / `aks_se_set_secret() -> %d` / `aks_stash_commit failed %x (handle:%d)`）。

### 5.4 能不能桩？逐条结论

| 断点 | 能不能桩 | 怎么桩（具体到文件/函数） |
|---|---|---|
| systembag.kb | **能**，而且有人已经动了手 | 生成一个**真格式**的 keybag（class key 全固定值也行）写回 `/var/keybags/systembag.kb`，至少让 `MKBKeyBagCreateWithData` + `MKBKeyBagUnlock` 接受 |
| keychain（最大的坑） | **能绕过，不好桩** | (a) 桩 securityd 的 `SecItemAdd`/`CopyMatching` 落到文件；(b) 更省事——**直接预置 pair record**（下一行）跳过 PairSetup，lockdownd 就不会去要 device keypair |
| pair record | **能，性价比最高** | 手写 `/private/var/root/Library/Lockdown/pair_records/<BUID>.plist`。lockdownd 认的 key 名（都是字符串常量）：`HostID`、`SystemBUID`、`HostCertificate`、`HostPrivateKey`、`RootCertificate`、`RootPrivateKey`、`DeviceCertificate`、`DevicePrivateKey`、`EscrowBag`、`EscrowSecret`、`HostConnectionType`、`HostIP`、`HostSideClientName`、`HostSideHostName`、`UseHostTracker`、`SessionID`、`EnableSessionSSL`；通知名 `com.apple.mobile.lockdown.paired_host_info` |
| 设备身份证书 | **能** | 自签一套：根用我们自己的 `RootCertificate`，`DeviceCertificate` 自签。lockdownd 只做 `SecTrustCreateWithCertificates` + `SecTrustSetAnchorCertificates` + `SecTrustEvaluateWithError`，锚点由我们给，能过 |
| keybag 解锁门禁 | **能** | `MKBDeviceUnlockedSinceBoot` 是可桩返回值；桩成 1 就绕过 `/keybag/isLocked` 系列 |
| 信任弹窗 | **能** | 有 `com.apple.private.iamlockdown` / `com.apple.private.lockdown.is-host-trusted` 两条 entitlement 通道；也可以直接把 BUID 写进"已信任"（`set_trusted_host`），绕过 `ask_user_to_trust` |
| SINF / FairPlay | **只能绕过，不能造** | 装**本地签名/未加密**的 IPA（`needsSinf` 为假），或走 `InstallLocalProvisioned` + 描述文件。真 FairPlay 包必须要 Apple 签发的 SINF，无 SEP 就无解 |
| AMFI trust cache | **能** | installd 的 `com.apple.private.amfi.can-check-trust-cache` 是它主动去问；预置 trust cache，或让 `amfid` 起不来时走 `performingOnlineAuthorization:NO` / `checkingTrustCacheIfApplicable:NO` 分支 |

**一句话：Pair 本身完全可以桩掉（预置 pair record 最省事）；真正桩不掉的是 FairPlay 加密的包。**
所以目标如果只是"装自己的 IPA"，SEP 不是死结；如果是"装 App Store 的包"，那是死结。

---

## 6. 最小启动集：只为"PC 能装 IPA"

### 6.1 必需进程（含前置）

| # | 进程 | 为什么必需 | 前置 |
|---|---|---|---|
| 0 | `kernel` + `/sbin/launchd` | bootstrap 命名空间、socket 代持 | 无（已是现状） |
| 1 | `com.apple.mobile.keybagd` | `lockdownd`/`mip` 的 `MKB*` 全走它；`/usr/libexec/keybagd` 存在（275504 B，arm64，依赖含 `APFS.framework`、`Security`、`SoftLinking`） | 一个**能用**的 `/var/keybags/systembag.kb`；`AppleKeyStore`（可桩） |
| 2 | `com.apple.mobile.usermanagerd` | 提供 `com.apple.mobile.keybagd.UserManager.xpc`；`mobile_installation_proxy` 的 entitlement 直接点名 `keybagd.UserManager.bubblepop` | 1 |
| 3 | `com.apple.securityd` | `_SecItemAdd`/`_SecItemCopyMatching` 的落点；lockdownd 有 `security.storage.Lockdown` + `system-keychain` 特权 | 1（class key） |
| 4 | `com.apple.containermanagerd` | `installd` 建容器必过它（`MobileContainerManager.allowed`） | 3 |
| 5 | `com.apple.mobile.lockdown` | 主机入口：62078 + `/var/run/lockdown.sock` | 0；pair record（可用桩） |
| 6 | 主机 `StartSession` | 之后才允许 `StartService` | 5 + pair record |
| 7 | `com.apple.mobile.installation_proxy` | 主机的 `Install`/`Browse`/`Lookup` 落点 | 5、6（由 lockdownd 经 launchd 拉起） |
| 8 | `com.apple.mobile.installd` | 真正落盘 | 4、3（由 mach 调用拉起，不是 lockdownd） |
| 9 | `lsd`（或 SpringBoard） | 把 App 注册进 LaunchServices。**只影响图标/能不能点开**，不影响"装成功" | 8 |

**可选**：

- `com.apple.afcd`（+ `libafc.dylib`）：只有在你要"先 AFC 传 IPA 到设备再 Install"时才需要。
  不装它也能装——`Install` 的 `PackagePath` 也可以由你自己用别的方式放进 `/private/var/mobile/Media/`。
- `com.apple.misagent`：只有装 `.mobileprovision` 时才需要。**但**如果你要装"开发签名的 IPA"，
  描述文件必须先落库（`mis.db` + `ProvisioningProfiles/`），否则 `libmis` 校验会拒。
- `com.apple.installcoordinationd`：`InstallCoordination.framework` 那条路（`InstallForLaunchServices` 等）会用到；
  走 `MobileInstallation` 直连 installd 的话可以不要。

### 6.2 传输链路：两条路，选一条

**路径 A（推荐，零代码）：USB-NCM + wildcard 62078。**

```
前提：/usr/sbin/addNetworkInterface 能把 AppleUSBNCMData 接口拉起来
      （作业 com.apple.usb.networking.addNetworkInterface 已经在镜像里）
PC 拿到设备 IP → TCP connect <dev-ip>:62078 → 直接说 lockdown 协议
```

依据：lockdownd 的 socket 定义里**没有 SockNodeName**，绑的是 `INADDR_ANY`。

**路径 B（兜底，要写代码）：最小 usbmuxd 桩。**

```
USB 侧收 usbmuxd 帧（16 字节头 + 长度前缀的 plist；version/proto/message/tag 四个字段）
  → 只实现 Listen(62078) 与 Connect(devport 62078)
  → 转发到 127.0.0.1:62078
```

依据：`lockdownd` 已经在 62078 上 accept，桩只需要做搬运；`lockdownd` 里的
`mux_notification_handler` / `Misc usbmuxd event:` 说明它还认 usbmux 的通知，接上不会打架。

### 6.3 明确"不需要"的东西

- **不需要在设备上跑 `libimobiledevice` 那一套**（它本来就是 PC 侧的）。
- **不需要 `com.apple.afcd` 就能装**（见 6.1）。
- **不需要 SpringBoard 起来**才能装成功（只影响图标）。
- **不需要 `com.apple.misagent`**，如果你装的是完全自签名、不需要描述文件的包。
- **不需要 SEP**，如果只装本地签名的包（§5.4）。

---

## 7. 桩化优先级（按"性价比 × 阻塞度"排）

1. **预置 pair record**——跳过整个 PairSetup/PairVerify + keychain + 弹窗。收益最大。
2. **修 `/var/keybags/systembag.kb`**——现在是个 `SWEET` 占位符，先让它变成格式正确的 keybag。
3. **桩 `MKBDeviceUnlockedSinceBoot` → 1**——一次性解掉 `/keybag/isLocked` 全家门禁。
4. **桩 `_MAECopyLegacyDeviceIdentityWithError`** 返回自签 identity（配合 §5.4 的自签根）。
5. **预置一对描述文件 + `mis.db`**（如果走开发签名）。
6. **AMFI trust cache**——装包时 `checkingTrustCacheIfApplicable:` 才会被调。
7. **FairPlay / SINF**——桩不掉，只能选不加密的包。

---

## 8. 下一步上机要验什么（按这个顺序，每步失败点都不一样）

| # | 验什么 | 怎么验 | 期望 / 失败说明什么 |
|---|---|---|---|
| 1 | launchd 有没有把两个 socket 建出来 | `launchctl print system/com.apple.mobile.lockdown`；`ls -l /var/run/lockdown.sock`；`netstat -an \| grep 62078` | 有 socket 且 mode `srwxrwxrwx` → L0/L1 没问题；没有 → plist 没被读或 launchd 拒绝 |
| 2 | lockdownd 能不能**不经过任何 USB** 答话 | 设备侧自己连：往 `127.0.0.1:62078` 发 `{"Label":"com.apple.lockdown"}` 的 plist 请求 | 回一个 plist（含 `ProductVersion`/`BuildVersion`）→ 传输层问题和 lockdownd 问题就彻底分开了。**这一步最值钱** |
| 3 | `StartSession` 卡在哪 | 发 `StartSession` + `HostID`/`SystemBUID`，看回什么 error | `No pairing record for %@` → pair record 没预置；`Invalid host id` → BUID 对不上 |
| 4 | keybag 到底能不能解 | 跑 `MKBDeviceUnlockedSinceBoot` / `MKBGetDeviceLockState`，并看 `/private/var/logs/lockdownd.log` | `Can't unlock escrow bag: %d` → §5 第 4 步断；返回 0 → 第 5 步门禁还在 |
| 5 | `com.apple.afc` 与 `com.apple.afc.root` 是不是同一个东西 | `StartService` 这两个名字各起一次，看 afcd 走 `run_connection for root /` 还是 `/private/var/mobile/Media` | 这条我在静态数据里只见两个名字并存，**没有证死**，必须上机确认 |
| 6 | 一个**不需要签名**的调用先打通 | 主机 `Browse`（走 `mobile_installation_proxy`，不落盘、不验签） | 能列出 App 列表 → 说明 lockdownd→checkin→mip→MobileInstallation 这条链全通，只剩 installd 的验签 |
| 7 | 装包前半程 | `Install` 一个**本地签名**的 IPA（不要 FairPlay 包），看 `/private/var/logs/installd.log` | 卡在 `SINF` → 选了加密包；卡在 container → containermanagerd；卡在 trust cache/AMFI → §7 第 6 项 |
| 8 | AMFI / trust cache 现状 | `amfid` 是否在跑；kernel 有没有 CDHASH 校验拒绝；`sysctl security.mac.amfi.developer_mode_status`（lockdownd 的 entitlement 里就有这条只读 sysctl） | 被拒即 AMFI 在管；此时要么预置 trust cache，要么桩 `can-check-trust-cache` |
| 9 | USB 承载 | `addNetworkInterface` 是否 spawn 成功、有没有新 `en*` 接口、PC 侧能不能 ping 通设备 | 接口起得来 → 走 §6 路径 A；起不来 → 走路径 B 写 usbmuxd 桩 |
| 10 | `_usbmuxd` 账号有没有被用到 | 起完所有服务后 `ps -axo user,pid,comm \| grep 213` | 全空 → 这条残迹确实是死的，桩方案不受影响 |

---

## 附录 A：五个可执行文件的完整 dylib 清单

（`llvm-objdump --macho -p <bin> | grep 'name /'`，去掉 `/usr/lib/dyld`）

**afcd（6）**

```
/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag
/usr/lib/liblockdown.dylib
/usr/lib/libafc.dylib
/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation
/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit
/usr/lib/libSystem.B.dylib
```

**lockdownd（39）**

```
/System/Library/Frameworks/CFNetwork.framework/CFNetwork
/System/Library/PrivateFrameworks/UserManagement.framework/UserManagement
/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony
/System/Library/PrivateFrameworks/CrashReporterSupport.framework/CrashReporterSupport
/System/Library/Frameworks/LocalAuthentication.framework/LocalAuthentication
/System/Library/PrivateFrameworks/CoreDuet.framework/CoreDuet
/System/Library/PrivateFrameworks/CoreUtils.framework/CoreUtils
/System/Library/PrivateFrameworks/FindMyDevice.framework/FindMyDevice
/System/Library/Frameworks/CoreServices.framework/CoreServices
/System/Library/PrivateFrameworks/MobileActivation.framework/MobileActivation
/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices
/System/Library/PrivateFrameworks/ManagedConfiguration.framework/ManagedConfiguration
/System/Library/Frameworks/Foundation.framework/Foundation
/System/Library/PrivateFrameworks/ServiceManagement.framework/ServiceManagement
/System/Library/PrivateFrameworks/AppSupport.framework/AppSupport
/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation
/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit
/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration
/System/Library/PrivateFrameworks/MobileStorage.framework/MobileStorage
/System/Library/PrivateFrameworks/Bom.framework/Bom
/System/Library/Frameworks/Security.framework/Security
/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi
/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation
/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag
/System/Library/PrivateFrameworks/CoreTime.framework/CoreTime
/System/Library/PrivateFrameworks/RemoteServiceDiscovery.framework/RemoteServiceDiscovery
/System/Library/PrivateFrameworks/IntlPreferences.framework/IntlPreferences
/System/Library/PrivateFrameworks/RemotePairingDevice.framework/RemotePairingDevice
/usr/lib/libAccessibility.dylib
/usr/lib/libz.1.dylib
/usr/lib/libMobileCheckpoint.dylib
/usr/lib/liblockdown.dylib
/usr/lib/libMobileGestalt.dylib
/usr/lib/libMobileGestaltExtensions.dylib
/usr/lib/libamsupport.dylib
/usr/lib/libobjc.A.dylib
/usr/lib/libSystem.B.dylib
/System/Library/PrivateFrameworks/DeviceIdentity.framework/DeviceIdentity
/usr/lib/libc++.1.dylib
```

**misagent（10）**

```
/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit
/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation
/usr/lib/libsqlite3.dylib
/System/Library/Frameworks/Security.framework/Security
/usr/lib/libMobileGestalt.dylib
/usr/lib/liblockdown.dylib
/usr/lib/libmis.dylib
/System/Library/Frameworks/Foundation.framework/Foundation
/usr/lib/libobjc.A.dylib
/usr/lib/libSystem.B.dylib
```

**mobile_installation_proxy（15）**

```
/System/Library/PrivateFrameworks/UserManagement.framework/UserManagement
/System/Library/PrivateFrameworks/InstallCoordination.framework/InstallCoordination
/System/Library/Frameworks/CoreServices.framework/CoreServices
/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony
/System/Library/Frameworks/CoreData.framework/CoreData
/usr/lib/libMobileGestalt.dylib
/System/Library/PrivateFrameworks/ManagedConfiguration.framework/ManagedConfiguration
/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation
/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation
/System/Library/PrivateFrameworks/MobileSystemServices.framework/MobileSystemServices
/usr/lib/liblockdown.dylib
/System/Library/Frameworks/Security.framework/Security
/System/Library/Frameworks/Foundation.framework/Foundation
/usr/lib/libobjc.A.dylib
/usr/lib/libSystem.B.dylib
```

**installd（23）** —— 注意**没有 `liblockdown.dylib`**

```
/usr/lib/libmis.dylib
/usr/lib/libParallelCompression.dylib
/System/Library/PrivateFrameworks/DataMigration.framework/DataMigration
/System/Library/PrivateFrameworks/MobileStorage.framework/MobileStorage
/System/Library/PrivateFrameworks/StreamingZip.framework/StreamingZip
/System/Library/Frameworks/ImageIO.framework/ImageIO
/System/Library/Frameworks/UniformTypeIdentifiers.framework/UniformTypeIdentifiers
/System/Library/PrivateFrameworks/IconServices.framework/IconServices
/System/Library/PrivateFrameworks/RunningBoardServices.framework/RunningBoardServices
/System/Library/PrivateFrameworks/SoftLinking.framework/SoftLinking
/System/Library/PrivateFrameworks/InstalledContentLibrary.framework/InstalledContentLibrary
/System/Library/PrivateFrameworks/UserManagement.framework/UserManagement
/System/Library/Frameworks/Security.framework/Security
/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation
/usr/lib/libbz2.1.0.dylib
/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit
/System/Library/Frameworks/Foundation.framework/Foundation
/usr/lib/libMobileGestalt.dylib
/System/Library/PrivateFrameworks/MobileSystemServices.framework/MobileSystemServices
/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation
/System/Library/Frameworks/CoreServices.framework/CoreServices
/usr/lib/libobjc.A.dylib
/usr/lib/libSystem.B.dylib
```

## 附录 B：lockdownd 的 entitlements（完整原文）

从 `LC_CODE_SIGNATURE` 的 SuperBlob 里类型 5（entitlements）取出的 XML：

```xml
<key>application-identifier</key><string>com.apple.mobile.lockdownd</string>
<key>com.apple.CommCenter.fine-grained</key><array><string>spi</string><string>identity</string></array>
<key>com.apple.PairingManager.Read</key><true/>
<key>com.apple.PairingManager.RemovePeer</key><true/>
<key>com.apple.PairingManager.Write</key><true/>
<key>com.apple.SystemConfiguration.SCDynamicStore-write-access</key><true/>
<key>com.apple.ULLAConcierge.xpc</key><true/>
<key>com.apple.USBCEntitlement</key><true/>
<key>com.apple.aosnotification.aosnotifyd-access</key><true/>
<key>com.apple.appletv.pbs.mediaremote</key><true/>
<key>com.apple.camera.iokit-user-access</key><true/>
<key>com.apple.coreduetd.allow</key><true/>
<key>com.apple.developer.device-information.user-assigned-device-name</key><true/>
<key>com.apple.driver.I2C.device.user-access</key><true/>
<key>com.apple.dt.remotepairing.pairingmanagement</key><true/>
<key>com.apple.dt.remotepairing.tunnelmanagement</key><true/>
<key>com.apple.itunesstored.private</key><true/>
<key>com.apple.keystore.escrow.create</key><true/>
<key>com.apple.managedconfiguration.profiled-access</key><true/>
<key>com.apple.mkb.usersession.info</key><true/>
<key>com.apple.mobileactivationd.spi</key><true/>
<key>com.apple.nearbyd.diagnostics</key><true/>
<key>com.apple.nfcd.hwmanager</key><true/>
<key>com.apple.nfcd.info</key><true/>
<key>com.apple.osanalytics.otatasking-service-access</key><true/>
<key>com.apple.private.CoreAuthentication.SPI</key><true/>
<key>com.apple.private.MobileGestalt.AllowedProtectedKeys</key>
<array><string>IceFallID</string><string>UniqueDeviceID</string><string>InverseDeviceID</string>
<string>ProximitySensorCalibration</string><string>ProximitySensorCalibrationDictionary</string>
<string>CompassCalibration</string><string>CompassCalibrationDictionary</string>
<string>SecureElementID</string><string>YonkersUID</string><string>SavageUID</string>
<string>SysCfgDict</string></array>
<key>com.apple.private.RemoteServiceDiscovery.device-admin</key><true/>
<key>com.apple.private.applecredentialmanager.allow</key><true/>
<key>com.apple.private.applecredentialmanager.devicerestrictedmode.allow</key><true/>
<key>com.apple.private.applesepmanager.allow</key><true/>
<key>com.apple.private.applesmc.user-access</key><true/>
<key>com.apple.private.gasgauge-update</key><true/>
<key>com.apple.private.iamlockdown</key><true/>
<key>com.apple.private.ioavfamily.hdmi-port</key><true/>
<key>com.apple.private.lockbot</key><true/>
<key>com.apple.private.mobile_storage.allowedSPI</key><array><string>CopyDevices</string></array>
<key>com.apple.private.mobileinstall.allowedSPI</key><array><string>CopyDiskUsageForLaunchServices</string></array>
<key>com.apple.private.photos.service.internal.library</key><true/>
<key>com.apple.private.security.storage.Calendar</key><true/>
<key>com.apple.private.security.storage.Lockdown</key><true/>
<key>com.apple.private.securityd.ae.lockdown</key><true/>
<key>com.apple.private.system-keychain</key><true/>
<key>com.apple.private.tcc.allow</key><array><string>kTCCServicePhotos</string></array>
<key>com.apple.private.webinspector.allow-remote-inspection</key><true/>
<key>com.apple.private.xpc.launchd.app-server</key><true/>
<key>com.apple.rootless.storage.coreduet_knowledge_store</key><true/>
<key>com.apple.security.exception.iokit-user-client-class</key>
<array><string>AppleSMCClient</string><string>AppleGasGaugeUpdateUserClient</string>
<string>AppleHPMUserClient</string><string>AppleARMIICUserClient</string>
<string>IOThunderboltFamilyUserClient</string></array>
<key>com.apple.security.exception.mach-lookup.global-name</key>
<array><string>com.apple.remoted</string><string>com.apple.pcapd</string>
<string>com.apple.nearbyd.xpc.diagnostics</string><string>com.apple.nfcd.hwmanager</string>
<string>com.apple.PairingManager</string>
<string>com.apple.commcenter.mobile-helper-cbupdateservice</string>
<string>com.apple.carkit.service.lockdown</string><string>com.apple.amfi.lockdown</string>
<string>com.apple.backgroundassets.lockdownservice</string></array>
<key>com.apple.security.exception.sysctl.read-only</key>
<array><string>security.mac.amfi.developer_mode_status</string></array>
<key>com.apple.security.network.client</key><true/>
<key>com.apple.security.network.server</key><true/>
<key>com.apple.security.system-group-containers</key>
<array><string>systemgroup.com.apple.icloud.findmydevice.managed</string></array>
<key>com.apple.timed</key><true/>
<key>com.apple.timed.sources</key><array><string>MobileLockdown</string></array>
<key>com.apple.wifi.manager-access</key><true/>
<key>fairplay-client</key><integer>453076874</integer>
<key>keychain-access-groups</key>
<array><string>com.apple.mobile.lockdownd</string><string>lockdown-identities</string></array>
<key>seatbelt-profiles</key><array><string>lockdownd</string></array>
```

## 附录 C：相关守护进程的 MachServices（精确值）

| 作业 | MachServices | 其它 |
|---|---|---|
| `com.apple.mobile.keybagd` | `com.apple.mobile.keybagd.mach`, `com.apple.mobile.keybagd.xpc` | `RunAtLoad`，`-t 15`，`Interactive` |
| `com.apple.mobile.usermanagerd` | `com.apple.mobile.usermanagerd.xpc`, `com.apple.mobile.keybagd.UserManager.xpc`, `com.apple.system.libinfo.muser` | `RunAtLoad`，`-t 15`，`Interactive` |
| `com.apple.securityd` | `com.apple.security.escrow-update`, `com.apple.security.octagon`, `com.apple.securityd`, `com.apple.securityd.aps`, `com.apple.securityd.ckks`, `com.apple.securityd.general`, `com.apple.securityd.sos` | `_securityd`/`_securityd`，`MinimalBootProfile`，`Interactive` |
| `com.apple.trustd` | `com.apple.trustd` | `_trustd`/`_trustd`，`MinimalBootProfile`，`Adaptive` |
| `com.apple.containermanagerd` | `com.apple.containermanagerd`；`com.apple.containermanagerd.upcall` = `{HostSpecialPort: 25}` | `RunAtLoad`；参数含条件分支（`#Then` / `#IfFeatureFlagEnabled: containermanagerd/user_system_split`），展开后含 `--default-user=mobile --bundle-container-owner=_installd --system-container-owner=root --kernel-upcall=yes` |
| `com.apple.installcoordinationd` | `com.apple.installcoordinationd` | `KeepAlive.PathState` 盯 `PendingWork.plist`；`mobile` |
| `com.apple.installcoordination_proxy` | 无 MachServices | `RemoteServices: com.apple.remote.installcoordination_proxy`（`UsesRemoteXPC: true`，要求 `com.apple.private.InstallCoordinationRemote`） |
| `com.apple.runningboardd` | `com.apple.runningboard`, `com.apple.runningboard.watchdog`, `com.apple.runningboard.resource_notify` | `KeepAlive`，`RunAtLoad` |
| `com.apple.dt.remotepairingdeviced` | `com.apple.remotepairingdevice.pairingmanagement`, `com.apple.remotepairingdevice.tunnelmanagement`, `com.apple.dt.remotepairingdeviced.lockdown` | `Sockets: TCPControlChannelSocket`（`IPv4v6/TCP/0`）；`RemoteServices: com.apple.internal.dt.coredevice.untrusted.tunnelservice`（`UsesRemoteXPC: true`, `ServiceVersion 2`, untrusted） |
| `com.apple.mobile.house_arrest` | `com.apple.mobile.house_arrest` | `mobile`，`Adaptive` |
| `com.apple.SpringBoard` | 33 条（`com.apple.springboard.services`、`PurpleSystemAppPort`、`com.apple.frontboard.systemappservices`、`com.apple.springboard.backgroundappservices` …） | `Program = /System/Library/CoreServices/SpringBoard.app/SpringBoard`，`POSIXSpawnType: App` |

## 附录 D：复现命令

```bash
# 0) 挂载（只读）
echo 'asdfghjkl;'"'" | sudo -S mount -o loop,ro \
  ~/hackios/iosroot/root-hfsx.img /mnt/k60root

# 1) 五个二进制的 Mach-O 概要 + 依赖 + 导入符号 + 字符串
mkdir -p /tmp/usbmux
cd /mnt/k60root/usr/libexec
for f in afcd lockdownd misagent mobile_installation_proxy installd; do
  llvm-objdump --macho -p $f  > /tmp/usbmux/$f.pro.txt
  llvm-nm -u $f | awk '{print $NF}' | sort -u > /tmp/usbmux/$f.undef.txt
  llvm-strings -a $f | sort -u > /tmp/usbmux/$f.strings.txt
done

# 2) entitlements（SuperBlob -> type 5）
python3 /tmp/usbmux/tools/ents2.py \
  /mnt/k60root/usr/libexec/afcd \
  /mnt/k60root/usr/libexec/misagent \
  /mnt/k60root/usr/libexec/lockdownd \
  /mnt/k60root/usr/libexec/mobile_installation_proxy \
  /mnt/k60root/usr/libexec/installd

# 3) launchd plist（原文件是 bplist00）
python3 -c '
import plistlib, json
for p in ["com.apple.afcd","com.apple.misagent","com.apple.mobile.installd",
          "com.apple.mobile.lockdown","com.apple.mobile_installation_proxy"]:
    d = plistlib.load(open("/mnt/k60root/System/Library/LaunchDaemons/%s.plist" % p, "rb"))
    print(p, json.dumps(d, indent=1, default=str))'

# 4) 共享缓存：索引 2627 个 image + 查 usbmux
python3 /tmp/usbmux/tools/cache_index.py          # -> /tmp/usbmux/raw/cache_index.json
python3 -c '
import json
d = json.load(open("/tmp/usbmux/raw/cache_index.json"))
print(len(d), "images")
print("路径含 mux:", [x["ident"] for x in d if "mux" in x["ident"].lower()])
print([x["file"], x["off"] for x in d if x["ident"].endswith("/MobileDeviceLink")])'

# 全缓存 3.2 GB 字节串计数（这才是"0 就是 0"的依据）
python3 -c '
import glob
pats = [b"usbmuxd", b"com.apple.usbmuxd", b"USBMuxListener", b"usbmux",
        b"MobileDeviceLink", b"liblockdown.dylib"]
tot = dict.fromkeys(pats, 0)
for f in sorted(glob.glob("/mnt/k60root/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64*")):
    d = open(f, "rb").read()
    for p in pats: tot[p] += d.count(p)
for k,v in tot.items(): print(k, v)'

# 5) 缓存 image 的依赖（不能用 objdump，会报 truncated）
python3 /tmp/usbmux/tools/macho_lc.py \
  /System/Library/PrivateFrameworks/MobileDeviceLink.framework/MobileDeviceLink \
  /usr/lib/liblockdown.dylib /usr/lib/libafc.dylib

# 6) 配对状态（要 sudo，几个目录是 0700）
echo 'asdfghjkl;'"'" | sudo -S sh -c '
  xxd /mnt/k60root/var/keybags/systembag.kb | head -16
  ls -la  /mnt/k60root/private/var/Keychains /mnt/k60root/var/db/lockdown
  ls -la  /mnt/k60root/private/var/root/Library
  ls -la  /mnt/k60root/System/Library/Caches/apticket.der'

# 7) 卸载
echo 'asdfghjkl;'"'" | sudo -S umount /mnt/k60root
```

> `/tmp/usbmux/tools/cache_index.py` 的核心就是"在 43 个分片里按 8 字节对齐扫 `0xfeedfacf`，
> 校验 `filetype ∈ {1,2,6,8,11}` + `0 < ncmds < 80` + `sizeofcmds < 20000`，然后走 load command 拿 `LC_ID_DYLIB`"。
> 用这套扫出 2627 个 image、`uniq 2627`（无重复），且 `UIKit` / `libswiftCore` / `liblockdown.dylib` /
> `MobileDeviceLink` / `Security.framework` 全部命中 —— 索引是完整的，所以"没有 usbmuxd"这个结论能站住。
>
> 另一个容易踩的点：`LC_LOAD_DYLIB` 的 `name` 字段偏移是相对**该 load command 起点**的（`lc_cmd + lc_str.offset`），
> 解析器里要按 `off+no` 取，不是按文件起点取——这一点搞错会读出乱码 name 却不报错。
