# bootstrap 打包（里程碑 I1）

把 Procursus 的 `bootstrap-iphoneos-arm64.tar.zst` 裁成"最小集"，
重打成 `var/k60bootstrap/...` 前缀，设备上解到 `/` 就是 `/var/k60bootstrap`。

```sh
./build_bootstrap.sh --dry-run           # 只打印计划（不下载、不写盘）
./build_bootstrap.sh                     # 用本地已有的 tarball
./build_bootstrap.sh --src ~/bs.tar.zst
./build_bootstrap.sh --url https://...   # 缓存里没有才下载（tools/bootstrap/cache/）
./build_bootstrap.sh --keep-extra zsh,git
./test_build_bootstrap.sh                # 离线自测：造个假 bootstrap 跑一遍
```

## 裁剪规则

1. **按包留**：基础包 `dpkg apt bash coreutils ldid uikittools`，其余靠
   tarball 里 `var/lib/dpkg/status` 的 `Depends/Pre-Depends` 算闭包
   （备选依赖 `a | b` 取第一个存在的）。
2. **按目录丢**：`usr/share/{man,doc,locale,info,terminfo,gcc,vim}`、
   `usr/{include,lib/debug,lib/gcc,lib/perl5,share/perl5}`。
3. **dpkg 管理数据兜底**：`var/lib/dpkg/{status,available,diversions,info,
   updates,parts,triggers,alternatives}` 与 `var/cache/apt/archives/partial`
   总是留下（不属于任何包，少了 `dpkg -l` 就空）。
4. 没有 status 的 tarball 退回"按目录裁"模式（会警告）。

## 产物

```
build/k60bootstrap-<日期>.tar.zst   解包: tar -xf ... -C /
build/k60bootstrap.env              source 一下: PATH / DPKG_ADMINDIR / APT_CONFIG
build/manifest.txt                  保留了哪些包、文件数、sha256
```

上机：

```sh
tar -xf k60bootstrap-<日期>.tar.zst -C /
. /path/to/k60bootstrap.env
/var/k60bootstrap/usr/bin/dpkg -l | head
```

## 已知限制

- 依赖闭包是按 `Depends` 字段算的**文件级**最小集，不跑 `ldd`；某个包
  声明漏了依赖时，缺的库得手动 `--keep-extra` 或 `--keep-path usr/lib/xxx` 补。
- 前缀改写是纯路径替换（`tar --transform`），Procursus 里若有硬编码
  `/var/jb` 的二进制（某些脚本/plist），上机后可能还要再 patch。
- 没有真机就无法验证 `dpkg --version`；x86 宿主上要跑得靠 `qemu-aarch64-static`
  加整套 arm64 库，不在本脚本范围内。
