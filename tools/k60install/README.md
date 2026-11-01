# k60install —— IPA → 设备容器目录树（宿主侧）

`docs/INSTALLER-PLAN.md` 的 I2/I3：在宿主上把一个 `.ipa` 拆成
"推过去就能装"的目录，外加设备端脚本。**不联网、不碰设备、不要 root。**

## 用法

```sh
python3 k60install.py test.ipa                      # → out/<bundleid>/
python3 k60install.py test.ipa -o /tmp/k60 --uuid 6F9C1B1E-1111-4222-8333-444455556666
python3 k60install.py test.ipa --sign               # 自动找 ldid
python3 k60install.py test.ipa --sign --ldid /var/k60bootstrap/usr/bin/ldid
python3 k60install.py test.ipa --sign --verify      # 签完把 entitlements 读回来
python3 k60install.py test.ipa --json               # stdout 只出 manifest JSON
```

退出码：`0` 成功，`1` 运行错误，`2` 输入有问题，`3` 找不到 ldid。

## 产物

```
out/<bundleid>/
  Bundle/<Name>.app/...        Payload 里解出来的 app（权限位/符号链接尽量保留）
  Bundle/.com.apple.mobile_container_manager.metadata.plist   Bundle 容器 metadata
  Data/{Documents,Library/{Preferences,Caches},tmp}/          数据容器骨架
  metadata.plist               Data 容器 metadata（ContentClass=2）
  install.sh                   设备端安装脚本（支持 --dry-run / --force）
  manifest.json                机器可读摘要（路径、UUID、签名情况）
```

`metadata.plist` 字段：`MCMMetadataIdentifier`（bundle id）、
`MCMMetadataUUID`（容器 UUID）、`MCMMetadataContentClass`
（Bundle=1 / Data=2 / AppGroup=7）、`MCMMetadataSchemaVersion`、
`MCMMetadataPersona`（501）、`MCMMetadataInfo.ContentProtectionClass`（-1）。
这些是从真机/模拟器容器 dump 里抄的字段子集；**上机时要拿一个系统 App 的
容器对照**（I4/I7 的验收点）。

## I3：签名

`--sign` 会给 app 里**每个** Mach-O（主程序、`Frameworks/*.dylib`、
`PlugIns/*.appex/*`）跑一遍：

```
ldid -S<entitlements.xml> <binary>
```

ldid 的查找顺序：`--ldid` → `$K60_LDID` → `$LDID` → `PATH` →
`/var/k60bootstrap/usr/bin/ldid`、`/var/jb/usr/bin/ldid`、`/usr/local/bin`、`/usr/bin`。
找不到时退出码 3，并打印装法（Arch：`yay -S ldid`；或从 Procursus
bootstrap 包里取 `usr/bin/ldid`；或自己编）。

`--verify` 不依赖 macOS 的 `codesign`：本工具自己解析 Mach-O 的
`LC_CODE_SIGNATURE` → `CS_SuperBlob` → `CSSLOT_ENTITLEMENTS`，把签进去的
XML 读出来打印（thin 32/64 位与 fat 都支持）。

`entitlements.xml` 是默认的"要什么给什么"模板，改它就行；也可以
`--entitlements 别的.xml`。

## install.sh 在设备上的前置（上机清单）

1. **root**：要写 `/private/var/containers`、改属主。`sh install.sh` 前先 `su`/以 root 跑。
2. **uicache**：脚本按这个顺序找
   `/var/k60bootstrap/usr/bin/uicache` → `/usr/bin/uicache` →
   `/var/jb/usr/bin/uicache` → `/usr/local/bin/uicache`；
   也可以用 `K60_UICACHE=/path/to/uicache` 直接指定。
   找不到时脚本会警告并跳过刷新（图标不会出现），但容器照写。
3. **两棵容器根目录存在**：
   `/private/var/containers/Bundle/Application` 和
   `/private/var/mobile/Containers/Data/Application`（镜像里是空的，但目录要在）。
4. **mobile 用户/组**：默认 chown 到 `501:501`，要改用
   `K60_UID=... K60_GID=... sh install.sh`。
5. **不叫 installd**：脚本只建目录 + 写 metadata，绕过 installd/amfid。
   若之后改成走 installd，签名才真正重要。
6. 可选：`K60_RESPRING=1 sh install.sh` 会额外 `killall -9 SpringBoard lsd`。

先空跑一遍：`sh install.sh --dry-run`（只打印要执行什么，不写盘）。

## 测试

```sh
cd tools/k60install && python3 -m unittest -v
```

测试自己造最小 IPA（zip 里塞假 `Info.plist` + 假 Mach-O），并用一个假
`ldid`（会真把 entitlements 写进 Mach-O）来验 I3 的回读路径，所以**不需要**
真 ldid、不需要设备、不联网。
