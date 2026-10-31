# 壁纸 Plan Z（0249）

## 问题

锁屏能出来，但壁纸是黑的。壁纸由 PaperBoard 自己的 FBScene 画，是另一个
IOSurface；内核里的 CPU 合成器（`AppleCLCD::present`）只把 SwapEnd 记录里
那几个 layer 的 surface 拷进面板，SpringBoard 主窗口之外的 surface 从来没到
过屏幕上。所以"SpringBoard 画了锁屏、ABL 的显存被我们改写"这件事里，缺的是
**一层底下**，不是"GPU 没画"。

## 做法

1. 内核从根卷读一张裸 BGRA（`/private/var/SweetWallpaper.bgra`，默认
   720x1600），首次 `SwapEnd` 时读一次（那时是 backboardd 的进程上下文，
   根卷也挂好了）。读不到就只打一行日志，行为与之前完全相同。
2. `present()` 里按层判断：用 32 像素步长（每行起点按行号错开）扫 alpha，
   全 255 就还走原来的整块 `memcpy`/缩放快路；只要有一个透明像素，这一层就
   逐像素做 src-over。
3. 默认 surface（`s == fSurface`，CA 直接画在面板上）的源像素**就在面板里**，
   不能"先铺壁纸再叠加"——先铺等于把源擦掉。这一层是就地混合：
   `面板 = over(面板里的 UI 像素, 壁纸)`，读同一行的同时写，结果与"先铺壁纸
   再叠加"逐像素等价。缩放路径从右下往左上写（沿用 0209 的 inplace 规则），
   目的永远在源的右下方向，不会踩到还没读的源像素。
4. 本帧没有默认 surface 层时，`present()` 一进来就把整屏铺上壁纸，后面的层
   叠在壁纸上。缩放留下的两条边（`clear_bars`）也从刷黑改成刷壁纸。

## 为什么壁纸不进 patch

- 4.4 MB 的裸 BGRA 变成 C 数组大约 22 MB 文本；CI 的缓存键是
  `hashFiles('xnu/patches/**')`，每次换图都会打穿整条 7 分钟的构建缓存。
- 二进制素材不该进 git（AGENTS.md），改一张图也不该重编内核。
- 失败代价可控：读不到就跳过，锁屏继续是黑底，启动不受影响。

## 上机

根卷里放好图（构建时）：

```sh
SWEET_WALLPAPER=/run/media/.../wall/wall720.bgra tools/mkiosroot.sh ...
```

`wall.bgra.z` 是同一张图 1440x3200 的 zlib，解压后用启动参数指明尺寸：

```sh
zlib-flate -uncompress < wall.bgra.z > wall.bgra     # 18432000 字节
# boot-arg: sweetwp_w=1440 sweetwp_h=3200
```

启动参数：

| 参数 | 意义 |
| --- | --- |
| `sweetwp=/path` | 换一个路径（必须 `/` 开头） |
| `sweetwp_w=720 sweetwp_h=1600` | 图的尺寸，默认就是这两个 |
| `sweetwp_premul=0` | UI 像素按"直乘 alpha"算（默认预乘，CG/IOSurface 的约定） |

## 上机看哪几行

```
SWEET WP: loaded 720x1600 BGRA from /private/var/SweetWallpaper.bgra (4608000 bytes), panel 1440x3200
SWEET WP: layer 0 surface 3 opaque=no, blend over wallpaper
SWEET WP: composite 1440x3200, 1 layer(s) blended, 0 seed(s), center 0xffc6e3e9
SWEET FB: first present N us, ..., wall on
```

- `loaded ...` 说明文件读到、尺寸对；没有这行就看下一行。
- `no /private/var/...` 说明没读到（路径、权限 0644、图是否进了镜像）。
- `is N bytes, need ...` 说明尺寸和 `sweetwp_w/h` 对不上。
- `opaque=no ... blend over wallpaper` 说明锁屏那层被判成半透明，走了混合。
- `composite ... center 0x...`：面板正中间那个像素。全黑是 `0x00000000`，
  `0xffc6e3e9` 这种才是壁纸（示例图是米色）。
- `first present ... wall on` 里能看到这一帧花了多少微秒。

## 已知限制

- alpha 判定是采样（步长 32、行起点错开），规整的大面积全透明/全不透明都能
  测准；只有"透明小洞嵌在不透明大块里"这种极小概率会判成全不透明，此时那
  一层保持原样（壁纸被盖住），不会花屏。
- 多个半透明层的叠放顺序沿用 SwapEnd 记录里的 layer 顺序；`direct` 层不在
  第一个时，它的源像素可能已被前面的层覆盖（与 0209 的行为一致，未修）。
- 混合默认按**预乘 alpha**（CG 交来的 surface 就是预乘的）：
  `面板 = UI + 壁纸 * (1 - a)`。只有半透明边缘那一圈会受这个假设影响；如果上机
  看到文字/圆角边缘偏暗或偏亮，把它当对照实验换成 `sweetwp_premul=0`。
- 混合是纯 CPU 的：1440x3200 一遍约 4.6M 像素，本机 x86 上 8ms 上下，
  A78 上预计 15-25ms；`sweetfb_hz` 默认 10 Hz，仍在预算内。
