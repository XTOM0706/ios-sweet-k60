# Adreno 730（K60 / SM8475）KMD 现状清单（2026-11-01）

## 一句话

**上电、固件上传、GMU 初步起来过；卡在"放 CM3 复位"那一下整机复位。**
离"出画面"还差 SMMU 给 GMU 的 route、CP 的 ring、以及把复位那一下的
真因钉死。

## 逐项状态

| 环节 | 现状 | 依据 |
| --- | --- | --- |
| 寄存器地图 | ✅ 齐了：kgsl `0x3d00000`、gmu `0x3d68000`、gmu_ao_blk_dec0 `0x3d40000`、gpucc `0x3d90000`、kgsl-smmu `0x3da0000`、rscc `0x3d50000`、cx_dbgc `0x3d61000`、isense `0x3d8b000` | `ADRENO-PLAN.md`，全部来自 `dump/live.dts` |
| 只读探测 | ✅ 代码在（0179/0181），能读到非零 | 0181 上机 |
| GPUCC 上电 | 🟡 只做到 CX（0188）。GDSC、GCC 分支、两个 PLL、CX 域都有代码 | `ADRENO-PLAN.md` M2、0180/0188 |
| GMU 固件 | ✅ 上传通过（0187） | `GPU-PORT-NOTES.md` §22 |
| GMU 初始化 | 🔴 **放 CM3 复位就整机复位**。已排除：上传本身、CX 写、SMMU 全局 | §22/§25/§27 |
| SMMU | 🟡 全局探到了（0189/0191/0192），但**没有给 GMU 的 route** | §24 |
| CP（命令处理器） | ⬜ 未开始 | — |
| 显存池 / ABI | ⬜ 未开始（pandora 那边是 1 GiB + NativeAdreno ABI v1） | `PANDORA-ANATOMY.md` |
| Turnip / Metal | ⬜ 未开始；但 `refs/AdrenoMetal`、`refs/ArmSmmu4K` 可引 | — |

## 固件现成

`~/hackios/gpu-fw/`：`a730_sqe.fw`（73 KB）、`gmu_gen70000.bin`（56 KB）、
`a730_zap.{mbn,mdt,b00..b02,elf}`。**a730 的 GMU 与 ZAP 都在手上**，
不用再进 TWRP 取。

## 放复位那一下：目前证据

- §25：0187/0196/0197 三次都在"放复位"瞬间整机复位，日志环停在
  `console ring flushed before releasing the CM3 reset`。
- §27：找到最可能的真凶——**cache 窗口只映了 56 KB**（固件搬运表要写的
  0x100000/0x200000/0x300000 三段落在了映射之外）。
- §26：RSCC 那一整块窗口本身就是雷区，读它曾经把机器带走。

## 下一步（按顺序，前两步可离线）

1. **②.1 映射修好再试**（离线改代码）：把 cache 窗口按 0207 的方案只映
   0x100000/0x200000/0x300000 三段（约 240 KB），不再整块 16 MB。
2. **②.2 先做 RSCC 只读体检**（离线写代码，上机只读）：按 pandora 的做法
   先 `gen7_rscc_wakeup_sequence()`（`GMU+0x26C1C=2`，轮询 `GMU+0x26C20`
   bit1），再读 RSCC 8 个字，判据是 8 字非 `0xffffffff` 且机器不死。
3. **②.3 VDD_CX/MX/MXC 投票**：pandora 的路径是先投父级电轨再放复位；
   我们的 RPMh 投票器已经在（0240 系），加三个资源名即可。
4. **②.4 再放复位**，目标日志 `gmu boot rc=0 fw_init=0x100`。
5. **②.5 CP 起来**：ring、fence、timestamp 的一次往返。

## 依赖关系

```
RSCC 只读通 ──→ 电轨投票 ──→ 放复位 ──→ GMU fw_init
                                  │
        SMMU route（GMU 那一侧）──┘
                                  ↓
                             CP ring → fence → Turnip
```
