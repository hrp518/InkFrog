# FontExp（旧版）解耦版编译指南

> 本指南记录 **2026-09-12 在全新 clone 的官方 SDK 上从零编译 FontExp 的完整实测过程**，
> 所有输入命令与输出均为真实记录。**SDK 保持 100% 原版（零补丁、git diff 为空）**，
> 全部自研代码/bugfix/配置随工程自包含。FontExp_gen1 流程完全相同，目录名替换即可。

## 0. 环境要求

| 项 | 要求 |
|---|---|
| 操作系统 | Windows + Cygwin（make / find / od / dd 等基本工具） |
| 工具链 | gcc-arm-none-eabi **4.9 2015q2**。原版 `gcc.mk` 默认 `~/tools/gcc-arm-none-eabi-4_9-2015q2/bin`；装在别处时编译命令行加 `CC_DIR=/你的工具链/bin`，无需改任何文件 |
| Python | **不需要**（`http_l1glyf_js.c` 已预生成；重新生成时才需任意 Python 3） |
| 网络 | 仅 clone 时需要 |

## 1. 全新 clone 官方 SDK

```
$ git clone https://github.com/XradioTech/xradio-skylark-sdk.git
$ cd xradio-skylark-sdk
$ ls project/demo/
at_demo  audio_demo  hello_demo  wlan_demo          ← 官方只有这 4 个, 正常
$ git log --oneline -1
16624d5 fix(efuse): fix wlan mac endian mode of special OUI   ← 基线 v1.1.1
```

## 2. 装配：文件怎么放

在 SDK 根目录执行发布包里的 `setup.sh`：

```
$ bash /path/to/decoupled_release/setup.sh
[1/3] 拷贝工程到 project/demo/ ...
[2/3] 写 .config ...
[3/3] touch 预生成文件 ...

装配完成 (SDK 保持原版, git status 应只显示新增的 demo 目录)。
```

脚本做三件事：

1. **拷工程**：`our_projects/{FontExp, FontExp_gen1, LVGL_withSDandNetwork}` → `project/demo/`；
2. **写 `.config`**（SDK 根，configure.sh 的产物，本包直接给出）：
   ```
   __CONFIG_CHIP_TYPE ?= xr872
   __CONFIG_HOSC_TYPE ?= 40
   ```
3. **touch 预生成文件** `project/demo/FontExp*/http_l1glyf_js.c`，
   避免构建时调用 python 重新生成（Cygwin 的 python 损坏也能编，见 §6.1）。

**不修改 SDK 任何文件。** 装配后与编译相关的目录结构：

```
xradio-skylark-sdk/                        ← 100% 原版, git diff 为空
├── .config                                ← xr872 / 40M
├── gcc.mk                                 ← 原版 (~/tools/...; 换机器用 CC_DIR= 覆盖)
├── lib/                                   ← 闭源预编译库 (clone 自带); 开源组件库由构建现编
├── bin/                                   ← boot/wlan 固件 blob (clone 自带)
└── project/
    ├── common/                            ← 原版 (fs_ctrl.c 由工程副本替代, 见下)
    ├── linker_script/gcc/appos.ld         ← 原版 (工程用自己的链接脚本)
    └── demo/
        ├── FontExp/                       ← ★ 本指南主角, 全部自包含:
        │   ├── fs_ctrl.c                  ← fs_ctrl 整文件工程化 (含 fs_ctrl_format),
        │   │                                Makefile 把 SDK 原版 filter-out 掉
        │   ├── fs_ctrl_format.h           ← 格式化函数声明
        │   ├── syscalls.c                 ← newlib 系统调用桩 (原 SDK retarget.c 内)
        │   ├── sdk_fixes.c/.h             ← SD 容量换算 + HIBERNATION 32K 校准 两个
        │   │                                bugfix, 经链接期 --wrap 外移
        │   ├── mkfs_ff/                   ← FatFs 原版源文件 + _USE_MKFS=1 副本,
        │   │                                objcopy 只导出 f_mkfs (SDK ffconf 保持 0)
        │   ├── board/                     ← 工程本地板级配置 (384M 超频)
        │   ├── gcc/Makefile               ← 编译入口 (PRJ_BOARD/LINKER_SCRIPT/
        │   │                                IMAGE_CFG/CC_FLAGS/LD_FLAGS 全部工程内定制)
        │   ├── gcc/appos.ld               ← 工程专属链接脚本 (XIP/PSRAM 白名单)
        │   ├── gcc/localconfig.mk         ← XIP/OTA/PSRAM 等工程开关
        │   ├── image/xr872/image*.cfg
        │   └── http_l1glyf_js.c           ← 预生成, 已被 touch
        ├── FontExp_gen1/                  ← gen1 (自带 gcc/appos_gen1.ld + 同套解耦件)
        └── LVGL_withSDandNetwork/
```

## 3. 编译（输入什么）

```
$ cd project/demo/FontExp/gcc
$ make build                    ← 务必串行, 不要加 -j (见 §6.2)
```

一条命令完成三阶段：**`lib`**（现编 SDK 组件库）→ **`all`**（编译工程/链接/size）
→ **`image`**（拷贝固件 blob + mkimage 打包出图）。

## 4. 输出什么（实测全过程）

### 4.1 库阶段（首次编译较慢）

`make build` 先执行 `make -C ../../../../src install`，把 `src/**` 各组件
现编成 `lib/*.a`（GitHub 仓库不带开源组件的预编译库）。**全新机器首次编译
要现编全部组件库，耗时数分钟到十几分钟**。

> 注：`ffconf.h` 保持原版 `_USE_MKFS=0`，因此 libfs 不含 f_mkfs（也不含
> 原版 sdmmc_diskio.c 里 mkfs 分支的编译错误代码）。f_mkfs 由工程的
> `mkfs_ff/f_mkfs.o` 独立模块在链接时提供。

### 4.2 工程编译

```
Creating response file with 381 object files...      ← 全部目标走响应文件链接
```

- 过程中会打印若干 **warning**（implicit declaration 等），因工程 Makefile
  已追加 `CC_FLAGS += -Wno-error`，**不影响编译**，属正常现象；
- 无 python 依赖：`http_l1glyf_js.c` 已被 touch 判定为最新，跳过生成规则。

### 4.3 链接与 size（实测输出）

```
Linking FontExp.axf...
Linking FontExp.axf...done
arm-none-eabi-size FontExp.axf
   text	   data	    bss	    dec	    hex	filename
1181856	1090100	  27012	2298968	 231458	FontExp.axf
   text	   data	    bss	    dec	    hex	filename
cp FontExp.bin ../image/xr872/app.bin
cp FontExp_xip.bin ../image/xr872/app_xip.bin
cp FontExp_psram.bin ../image/xr872/app_psram.bin
```

objcopy 按 XIP/PSRAM 配置把 ELF 切成 app.bin / app_xip.bin / app_psram.bin 三段。
链接命令自动带 `--wrap,SDMMC_ioctl --wrap,HAL_PRCM_DisableInter32KCalib`
（sdk_fixes.c 的两个 bugfix 生效点）。

### 4.4 mkimage 打包 —— 首次可能遇到的段重叠

`make image` 把 boot/wlan 固件 blob 拷进 `image/xr872/` 后调用
`tools/mkimage.exe -O -c .image.cfg -o xr_system.img`。**若固件体积与 cfg
固定偏移漂移，会报：**

```
err: bin 1 and bin 2 were overlaped!
Overlapped size: 1840 Byte(2kB)
bin 1 name:app.bin    begin: 0x00008000    end: 0x00023730
bin 2 name:app_xip.bin    begin: 0x00023000

We've rearranged bin files and generated new cfg file 'image_auto_cal.cfg', the new one is recommended.
Generate image file failed
make: *** [../../../../project/project.mk:352: image] Error 127
```

这是**预期行为，不是故障**：mkimage 已自动重排并生成 `image_auto_cal.cfg`。
工程 Makefile 已指向重排版（`IMAGE_CFG := image_auto_cal.cfg`），**再跑一次
同一条命令即可出图**（增量，秒级）：

```
$ make build
...
generate image: xr_system.img       ← EXIT:0
```

## 5. 产物（2026-09-12 实测，零补丁 SDK）

| 文件 | 大小 | 说明 |
|---|---|---|
| `project/demo/FontExp/image/xr872/xr_system.img` | **1,301,336 B** | ★ 整机镜像，phoenixMC 烧录这一个即可 |
| `project/demo/FontExp/gcc/FontExp.bin/.axf/.map` | — | 应用 bin / 调试符号 / 内存映射 |

三工程同批实测：FontExp 1,301,336 / FontExp_gen1 1,325,912 / LVGL_withSDandNetwork
920,408 字节，均 EXIT:0。与"SDK 打补丁"的旧方案相比，除刻意移除的调试日志外
体积一致（LVGL 逐字节一致），行为等价。

## 6. 常见问题（全部实测踩过）

### 6.1 `/bin/sh: py: command not found` + `Error 127`
Cygwin 的 python 损坏/缺失，make 试图重新生成 `http_l1glyf_js.c`。
预防：`setup.sh` 已 touch 该文件；若手动重置过时间戳：
`touch project/demo/FontExp/http_l1glyf_js.c`。重新生成字体 JS 才需要
装 Python 3 后手动跑 `tools/gen_l1glyf_web_js.py`。

### 6.2 加了 `-j` 后 `size` / `install` 报 Error 1
SDK 的 `build: lib all image` 是串行设计，并行时打包步骤抢在链接前执行。
**永远用串行 `make build`。**

### 6.3 mkimage `bin were overlaped`
见 §4.4，重跑一次即可。

### 6.4 换机器工具链路径
原版 gcc.mk 默认 `~/tools/...`；装在别处时命令行加
`make build CC_DIR=/你的工具链/bin`。

### 6.5 干净重编
`make build_clean`（清工程+库）；只清工程 `make clean`；只清镜像
`make image_clean`。

## 7. 本次验证记录

- 日期：2026-09-12；SDK：官方 GitHub master（`16624d5`，v1.1.1）全新 clone；
- 装配：`setup.sh`（零补丁，仅拷工程 + .config + touch）；
- FontExp / FontExp_gen1 / LVGL_withSDandNetwork 三工程 `make build` 全部
  EXIT:0；FontExp 1,301,336 B（与打补丁方案相比仅少调试日志，功能等价），
  LVGL 920,408 B 逐字节一致。
