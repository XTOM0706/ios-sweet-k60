# 第三方 App 安装器：可执行方案（2026-11-01）

> 目标：不是纸面评估，是"能把一个 App 装进去、主屏出图标、点开能跑"。
> 设备不在，标清楚哪些阶段现在就能做。

## 结论

**两套东西一起用，别二选一：**

- **包（越狱生态那种 `.deb`）走 Procursus 的 apt/dpkg** —— 现成、可信、
  不用自己写依赖解析。
- **App（IPA）走自建安装器** —— 因为 IPA 不是 deb，苹果那套安装管线
  （`installd` + `amfid` + LaunchServices + 容器）必须有人替它做。
  这一步照抄越狱圈的 **AppSync Unified + uicache** 思路。

两条路共用一个 bootstrap 目录和一套容器管理，装在
`/var/k60bootstrap`（rootless 风格，不污染 16 GiB 根卷的 `/usr`）。

## 0. 先划清边界

| 能做 | 做不了 |
| --- | --- |
| 自己签的、或已解密的 IPA | App Store 下载的加密 IPA（FairPlay，要 SEP） |
| 纯 CPU 渲染的 App | 重度依赖 Metal 的 App（等 GPU） |
| 依赖本地功能的 App | 依赖网络/相机/定位的 App（等对应驱动） |

## 1. Bootstrap 从哪来

三个候选，建议 **A + B 组合**：

| 方案 | 内容 | 优点 | 缺点 |
| --- | --- | --- | --- |
| **A. Procursus bootstrap** | dpkg、apt、bash、coreutils、`ldid`、`uicache`、`firmware` 包 | 全套现成，越狱圈验证过 | 包是为 rootless 越狱准备的，前缀要改 |
| B. 自己攒最小集 | 静态编译的 `dpkg` + `apt`，或干脆只带 `dpkg-deb` | 完全可控 | 依赖自己解决 |
| C. 从 pandora 借 | pandora 有 `tools/pinfile`，但没做包管理 | — | 帮助有限 |

**具体做法**：取 Procursus 的 `bootstrap-iphoneos-arm64.tar.zst`，解到
`/var/k60bootstrap`，把 `dpkg` 的 `--admindir` 指到
`/var/k60bootstrap/var/lib/dpkg`，`PATH` 里加 `/var/k60bootstrap/usr/bin`。

## 2. IPA 安装器怎么设计

一个 200 行左右的小程序 `k60install`（跑在设备上，用我们自己的 shell 也行）：

```
k60install <app.ipa>
  1. unzip 到临时目录，找 Payload/*.app
  2. ldid -S<entitlements.xml>   （或自签；内核不看签名，但 amfid 会）
  3. 生成 bundle id / UUID
  4. 拷到 /private/var/containers/Bundle/Application/<UUID>/<Name>.app
  5. 建 Data 容器：
       /private/var/mobile/Containers/Data/Application/<UUID>/
         Documents/ Library/ tmp/ Library/Preferences/
       写 .com.apple.mobile_container_manager.metadata.plist
         （MCMMetadataIdentifier=<bundle id>，MCMMetadataUUID=<UUID>）
  6. 注册 LaunchServices（见下）
  7. 刷新图标缓存（uicache）
```

关键：**第 5、6 步才是"能起图标、能点开"的分水岭**，签名早就不拦我们了。

### 2.1 容器目录

镜像里 `/private/var/containers/{Bundle,Data,Shared,Temp}` 都在，但**是空的**。
每个 App 需要：

- Bundle 容器：`/private/var/containers/Bundle/Application/<UUID>/`
- Data 容器：`/private/var/mobile/Containers/Data/Application/<UUID>/`

UUID 可以自己生成（v4），但 metadata plist 要写对，否则沙盒找不到家。
模板可以从任意一个系统 App 的容器里抄。

### 2.2 LaunchServices 注册

现代 iOS 的 LS 数据库是 `lsd` 管的二进制库；
越狱圈的做法是 **`uicache`**：直接重建
`/private/var/mobile/Library/Caches/com.apple.mobile.installation.plist`
与图标缓存，再 `killall -9 SpringBoard lsd`。

**建议**：先用 `uicache`（Procursus 里有），跑通之后再考虑自己写。

### 2.3 entitlements

自签时给一份"要什么给什么"的 plist（`platform-application`、
`get-task-allow`、`com.apple.private.*` 里常用的那几项）。
`amfid` 一旦被我们桩成"永远有效"，entitlements 就能原样下发。

## 3. 里程碑（标了依赖）

| # | 做什么 | 依赖 | 判据 |
| --- | --- | --- | --- |
| I1 | 在宿主上做一个 **bootstrap 打包脚本**：拉 Procursus 的 tarball、裁到最小集、生成 `.tar.zst` | 无（离线） | 解包后 `dpkg` 能跑起来（宿主 x86 用 qemu-user 验） |
| I2 | 写 `k60install` 的**宿主版**（不碰设备）：给一个 IPA，产出"目录树 + metadata plist + uicache 输入" | 无（离线） | 输出结构与真机容器布局逐字节对照 |
| I3 | 给 `k60install` 加 **ldid 签名**（用 Procursus 的 ldid） | 无（离线） | `codesign -d` 能读出 entitlements |
| I4 | 把 bootstrap + installer 塞进根卷，`dpkg -l` 能列包 | **上机** | 日志有 `dpkg` 输出 |
| I5 | 装第一个 App 到 Bundle 容器，`uicache` 后主屏出现图标 | **上机** | 图标可见 |
| I6 | 点开，App 能启动（哪怕只显示白屏后退出） | **上机** | `installd`/`launchd` 日志里出现该 bundle id |
| I7 | 建 Data 容器 + metadata，App 能写 `Documents` 不崩 | **上机** | App 内写文件成功 |
| I8 | 纯 CPU 渲染的简单 App 能正常交互 | **上机** | 界面能动 |

**I1–I3 现在就能做**，I4 之后必须等设备。

## 4. 与 amfid 的关系（必须处理）

我们内核不看签名，但 `installd` 安装时会调 `amfid` 验签。
三种处理，按侵入性从小到大：

1. **绕过 installd**：`k60install` 自己写容器和 LS 数据库，完全不叫
   `installd`（越狱圈的 AppSync 就是改 installd，我们不一定要改它）。
2. **桩 amfid**：让它的验签请求永远返回成功 + 请求的 entitlements。
3. **AppSync Unified 思路**：改 `installd` 的校验分支。

建议先走 1（最干净），失败再走 2。

## 5. 验证清单（上机时按这个顺序看）

```
1. dpkg -l                       → 列出 bootstrap 里的包
2. k60install test.ipa           → 打印 UUID / bundle id / 目标路径
3. ls /private/var/containers/Bundle/Application/<UUID>/
4. uicache -a                    → 输出刷新的 App 数
5. SpringBoard 日志里有该 bundle id
6. 点图标 → launchd 日志出现 exec 该 App
7. App 崩溃时看 crash log 的异常类型（签名 vs 缺框架 vs 沙盒）
```

## 6. 风险

- Procursus 的包假设 rootless 前缀 `/var/jb`，改前缀可能出现硬编码路径。
- `uicache` 依赖 `lsd` 的行为，iOS 16.7 与越狱常用的 15/16.0 有差异。
- App 起不来时，**别先怀疑签名**：先看是不是缺框架（GPU/音频）。
