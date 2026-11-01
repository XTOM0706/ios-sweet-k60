# 越狱工具那一套，哪些能搬到自移植的 XNU 上（2026-11-01）

> 纸面调研。设备不在，所有结论来自读原理与对照我们自己的内核状态。

## 结论先行

**它们的"漏洞"我们一个都不需要，它们的"手段"我们几乎全都能自己做。**

越狱工具分两半：

- **拿到内核读写权限**（bootrom 漏洞、内核提权漏洞）——我们没有这个需求，
  内核是我们自己编的，rw 权限天生就有。
- **拿到权限之后做的那套事**（关签名、造 trust cache、放宽沙盒、
  重新挂载根卷、注入 dylib、装包管理器）——这些正是我们需要的，
  而且因为内核在手，**可以直接编译期内建**，比运行时打补丁干净得多。

真正搬不过来的只有两类：**依赖 SEP 的功能**（钥匙串、Apple Pay、
FairPlay/DRM），和**依赖 bootrom 的持久化**（checkm8 那一类），
后者我们也不需要。

## 一张表看完

| 工具 / 组件 | 它解决什么 | 靠什么 | 我们能直接用吗 |
| --- | --- | --- | --- |
| checkra1n | 用 checkm8（bootrom）拿到任意代码执行，再打内核补丁 | A5–A11 的 bootrom 漏洞 | 漏洞**用不上**；但它的**内核补丁清单**是最好的对照表 |
| unc0ver | 内核提权后关签名、放宽沙盒、重挂根卷 | 内核漏洞（各版本不同） | 提权**用不上**；补丁清单可照做 |
| palera1n | checkm8 + ramdisk，rootful/rootless 双形态 | 同上 | 同 checkra1n；它的 **rootful 布局**对我们有用 |
| Dopamine | rootless 越狱，**用 launchd 重启技巧**做引导 | 内核漏洞 + jbctl | 引导机制**可借**；越狱本身不需要 |
| Sileo / Zebra | 包管理器前端，后端是 apt/dpkg | 纯用户态 | **可直接用**，只要有可写根和 shell |
| Procursus bootstrap | 越狱生态的 Unix 工具链（dpkg、apt、bash…） | 纯用户态 | **可直接用** |
| Substitute / libhooker / ElleKit | 把 dylib 注入进程、hook 函数 | 需要 task_for_pid + 可执行内存 + 关签名 | **可以自己做**，前提是放开注入限制 |
| Frida / Cydia Impactor 类 | 调试/签名注入 | 用户态 | 部分可用 |

## 逐个说

### checkra1n / palera1n（bootrom 线）

它们的前提是 bootrom 里的 checkm8：在设备最早期的阶段拿到代码执行，
然后加载一个自制 iBoot（pongoOS）去给内核打补丁、塞 ramdisk。

- **搬不过来**：漏洞绑定 A5–A11 的 bootrom，跟我们的机器无关；
  我们的启动链是 `fastboot boot` 直接进自己编的 xnu。
- **值得抄**：它们的内核补丁清单基本就是"越狱到底要关哪些东西"的目录——
  `amfi`、`cs_*`、`sandbox`、`task_for_pid`、`nvram`、`rootfs remount`、
  `trust cache`。我们已经有 0142/0144/0025/0035/0029 这几块，可以对着补漏。

### unc0ver（内核提权线）

用当时版本的内核漏洞拿到 kernel rw，然后：

- 关 AMFI / codesign、改 `cs_flags`
- 放宽 sandbox（`sandbox_extension_*`、profile 替换）
- 重新挂载根卷为可写
- 往 trust cache 里塞自己的 cdhash

这些**动作**我们全都能做，而且比它干净：它是运行时改内存，我们是编译期
改源码/加补丁。我们缺的只是"照着清单把漏的补上"。

### Dopamine（rootless 越狱）

价值在**引导**：它用一套 `jbctl` + launchd 重启的流程，让越狱环境在
不重启设备的前提下装配起来。我们要做包管理器时，最需要的就是这个：
怎么把 `/usr/bin/dpkg`、`/var/lib/dpkg`、环境变量、`PATH` 在
launchd 管辖的体系里装好，而不需要重启。

另外它的 **rootless 布局**（所有东西塞在 `/var/jb`，不碰系统卷）对我们
也有参考价值——如果我们不想污染那个 16 GiB HFS 根卷。

### Sileo / Zebra + Procursus

纯用户态。Sileo 是前端，后端是 `apt`/`dpkg`；Procursus 提供整套工具链。
只要满足三件事就能跑：

1. 可写的文件系统（我们有，HFS 根卷未封印）
2. 能 exec 任意二进制（内核已放行）
3. 有 launchd 能被重新加载（或者我们自己拉进程）

这是**最容易落地的一块**，也是"能装软件"最实际的形态：不用 IPA，
直接发 `.deb`。

### Substitute / libhooker / ElleKit

三者都是"把一段代码塞进别人进程并改函数入口"。

| 依赖 | 我们的状态 |
| --- | --- |
| `task_for_pid` / `thread_get_state` 等调试权限 | 内核归我们，可以放开 |
| 目标进程能加载未签名 dylib | 签名已关（0142/0144） |
| `DYLD_INSERT_LIBRARIES` 被尊重 | 需要确认 dyld 与 launchd 的限制；越狱是改 launchd 允许 |
| 可执行内存（`mprotect` RX） | 内核可放开 |

所以这条路**可以自己做**，难点不在权限，而在 ABI 稳定性：
我们的 dyld 共享缓存是 iOS 16.7 的，hook 框架要和它对得上。

## 对我们最有用的五个手段

1. **trust cache 自己造**（已有 0015 系）：这是"让任意二进制被认作平台程序"
   的正规做法，比关签名更细粒度。
2. **amfid 桩化**：让用户态的验签请求永远成功。越狱工具也是这么干的
   （替换 `amfid` 或注入它）。
3. **沙盒 profile 放宽**：与其关掉沙盒，不如给 App 一份宽松 profile，
   保住基本的容器隔离，出问题好查。
4. **launchd 重载引导**（Dopamine 的 jbctl 思路）：不重启装配环境。
5. **`.deb` 分发**（Sileo/Procursus）：比 IPA 自签省掉容器与代码签名两道关。

## 搬不过来的

| 东西 | 为什么 |
| --- | --- |
| SEP / Secure Enclave | 钥匙串、Apple Pay、DRM（FairPlay）、iCloud 加密都靠它；我们没有 SEP，只能给降级或桩 |
| bootrom 漏洞与持久化 | 与机型绑定，且我们不需要持久化 |
| PAC / KTRR / PPL 绕过 | 那些是苹果内核的保护；我们的内核是开源的 xnu-8796 自己编，根本没有这些保护 |
| 基带 / 蜂窝相关 | 没有基带驱动，CommCenter 也要桩（pandora 用替身进程接住） |

## 建议的落地顺序

| 阶段 | 做什么 | 判据 |
| --- | --- | --- |
| 1 | 把 Procursus 的 `dpkg`/`apt` 装进根卷，手工装一个 `.deb` | `dpkg -l` 列出该包，二进制能跑 |
| 2 | 用 launchd 重载的方式拉起一个常驻进程 | 不重启就出现在进程表 |
| 3 | 给 App 一份宽松 sandbox profile，实测容器里能读写 | App 的 `Documents` 可写 |
| 4 | 试 ElleKit/Substitute 注入一个测试 dylib 到某个系统进程 | 目标进程日志出现注入痕迹 |
| 5 | 把这套打成一个"bootstrap 包"，开机自动装配 | 冷启动后环境就绪 |

## 待上机（设备不在，攒着）

- 确认 `DYLD_INSERT_LIBRARIES` 在 launchd 起的进程里是否被尊重
- 确认 `task_for_pid` 对系统进程是否被我们自己的 AMFI 桩拦下
- 确认 `.deb` 安装后 launchd 是否需要重新扫描 plist

## 风险

- 越狱那套的补丁清单是按"苹果原厂内核"列的，我们的内核已经改过很多，
  照抄会重复或冲突，必须逐条对照我们已有的 0142/0144/0025/0035/0029。
- `amfid` 与 `installd` 是平台二进制，替换它们可能触发 launchd 的校验路径，
  要小步验证。
- 关掉签名后整个系统对"任意二进制"开放，建议用启动参数门控（现状已是）。
