# FontExp（旧版）解耦版编译指南

> 本指南记录 **2026-09-11 在全新 clone 的官方 SDK 上从零编译 FontExp 的完整实测过程**，
> 所有输入命令与输出均为真实记录。FontExp_gen1 的流程完全相同，只是目录名换成
> `FontExp_gen1`（其编译指南见仓库根 `README.md` / `decoupled_release/README.md`）。

## 0. 环境要求

| 项 | 要求 |
|---|---|
| 操作系统 | Windows + Cygwin（make / find / od / dd 等基本工具） |
| 工具链 | gcc-arm-none-eabi **4.9 2015q2**，默认路径 `/home/Administrator/tools/gcc-arm-none-eabi-4_9-2015q2/bin`（补丁后的 `gcc.mk` 写死此路径，装在别处改 `gcc.mk` 的 `CC_DIR` 一行） |
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

## 2. 装配：把解耦发布包"放"进 SDK（文件怎么放）

在 SDK 根目录执行发布包里的 `setup.sh`：

```
$ bash /path/to/decoupled_release/setup.sh
[1/4] 应用 SDK 补丁 (7 文件)...
      补丁 OK
[2/4] 拷贝工程到 project/demo/ ...
[3/4] 写 .config ...
[4/4] touch 预生成文件 ...
```

脚本做四件事：

1. **打补丁**：`sdk_modifications.patch`（仅 **7 个 SDK 文件，+50/−17**，
   无任何工程业务函数）——`gcc.mk`（工具链/警告开关）、`ffconf.h`
   （`_USE_MKFS=1`）、`FreeRTOSConfig.h`（静态分配）、`fs_ctrl.c`
   （+24 行最小扩展点：`fs_ctrl_get_fs/set_fs/lock/unlock`）、
   `sdmmc_diskio.c`（SD 容量换算 bugfix）、`pm.c`（休眠校准 bugfix）、
   `udhcp debug.h`（调试日志开关）。
   **业务代码全部随工程携带**：`fs_ctrl_format` 在工程
   `fs_ctrl_format.c`、newlib 桩在工程 `syscalls.c`、XIP 链接脚本在工程
   `gcc/appos.ld`、384M 板级配置在工程 `board/`；
2. **拷工程**：`our_projects/{FontExp, FontExp_gen1, LVGL_withSDandNetwork}` → `project/demo/`；
3. **写 `.config`**（SDK 根）：
   ```
   __CONFIG_CHIP_TYPE ?= xr872
   __CONFIG_HOSC_TYPE ?= 40
   ```
4. **touch 预生成文件** `project/demo/FontExp*/http_l1glyf_js.c`，
   避免构建时调用 python 重新生成（Cygwin 的 python 损坏也能编，见 §6.1）。

装配后与编译相关的目录结构：

```
xradio-skylark-sdk/                  ← 已打 7 文件最小补丁 (无业务代码)
├── .config                          ← xr872 / 40M
├── gcc.mk                           ← CC_DIR 工具链路径 (换机器改这里)
├── lib/                             ← 闭源预编译库 (clone 自带); 开源组件库由构建现编
├── bin/                             ← boot/wlan 固件 blob (clone 自带)
└── project/
    ├── common/                      ← 仅 fs_ctrl.c 带 +24 行扩展点, 其余原版
    ├── linker_script/gcc/appos.ld   ← 原版 (工程用自带链接脚本)
    └── demo/
        ├── FontExp/                 ← ★ 本指南主角 (自带 lvgl/ third_party/ image/)
        │   ├── fs_ctrl_format.c/.h  ← SD 格式化 (从 SDK 解耦出的业务函数)
        │   ├── syscalls.c           ← newlib 系统调用桩 (从 SDK 解耦出)
        │   ├── board/               ← 工程本地板级配置 (384M 超频, 不动 SDK 公共板级)
        │   ├── gcc/Makefile         ← 编译入口 (PRJ_BOARD/LINKER_SCRIPT/IMAGE_CFG 均指向工程内)
        │   ├── gcc/appos.ld         ← 工程专属链接脚本 (XIP/PSRAM 白名单)
        │   ├── gcc/localconfig.mk   ← XIP/OTA/PSRAM 等工程开关
        │   ├── image/xr872/image*.cfg
        │   └── http_l1glyf_js.c     ← 预生成, 已被 touch
        ├── FontExp_gen1/            ← gen1 (用工程内 appos_gen1.ld, 不依赖共享 appos.ld)
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
要现编全部组件库，耗时数分钟到十几分钟**；库编好后落在 `lib/` 下
（libchip.a、liblwip.a、libmbedtls.a 等约 30 个）。

> 本次实录（2026-09-11）中组件库已在同 SDK 的 FontExp_gen1 验证时编好，
> 该阶段直接跳过。

### 4.2 工程编译

```
/home/Administrator/tools/.../arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb
  -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -c ... -Os ...
  -D__CONFIG_CHIP_XR872 ... -o ../../../../project/demo/FontExp/lvgl/src/draw/lv_img_decoder.o ...
Creating response file with 379 object files...      ← 379 个目标文件走响应文件链接
```

- 本次实录编译 **275 条 gcc 命令**（`lvgl/`、工程根 `.c`、`third_party/expat、miniz`、`coremark/` 及整个 `project/common` 框架层）；
- 过程中会打印若干 **warning**（如 `implicit declaration of xTaskGetTickCount`、
  miniz 指针类型），因 `gcc.mk` 已加 `-Wno-error`，**不影响编译**，属正常现象；
- 无 python 依赖：`http_l1glyf_js.c` 已被 touch 判定为最新，跳过生成规则。

### 4.3 链接与 size（实测输出）

```
Linking FontExp.axf...
Linking FontExp.axf...done
arm-none-eabi-size FontExp.axf
   text	   data	    bss	    dec	    hex	filename
1185480	1090108	  27008	2302596	 232284	FontExp.axf
cp FontExp.bin ../image/xr872/app.bin
cp FontExp_xip.bin ../image/xr872/app_xip.bin
cp FontExp_psram.bin ../image/xr872/app_psram.bin
```

objcopy 按 XIP/PSRAM 配置把 ELF 切成三个 bin：
`app.bin` 112,368 B（RAM 段）、`app_xip.bin` 804,836 B（flash XIP）、
`app_psram.bin` 319,960 B（PSRAM 段）。

### 4.4 mkimage 打包 —— 首次会遇到的段重叠（实测）

`make image` 把 `bin/xradio_v2/boot/xr872/boot_40M.bin`、`wlan_bl/fw/sdd` 拷进
`image/xr872/` 后调用 `tools/mkimage.exe -O -c .image.cfg -o xr_system.img`。
**首次编译时固件体积与 cfg 固定偏移有少量漂移，会报：**

```
err: bin 1 and bin 2 were overlaped!
Overlapped size: 1840 Byte(2kB)
bin 1 name:app.bin    begin: 0x00008000    end: 0x00023730
bin 2 name:app_xip.bin    begin: 0x00023000

We've rearranged bin files and generated new cfg file 'image_auto_cal.cfg', the new one is recommended.
Generate image file failed
make: *** [../../../../project/project.mk:352: image] Error 127
```

这是**预期行为，不是故障**：mkimage 已自动重排各段偏移并生成
`image/xr872/image_auto_cal.cfg`（app_xip 140K→142K、app_psram 845K→929K、
wlan blob 顺延）。**工程 Makefile 已改为读重排版**（2026-09-11 修复，
与 gen1 同款做法）：

```make
# gcc/Makefile 第 85 行附近
IMAGE_CFG := image_auto_cal.cfg     # 原为 image.cfg
```

改完**重跑同一条命令**（增量，只补打包步骤）：

```
$ make build
...
generate image: xr_system.img       ← EXIT:0, 实测 9 秒
```

> 若你拿到的是尚未改 Makefile 的旧工程：看到上面报错后手动把
> `gcc/Makefile` 的 `IMAGE_CFG := image.cfg` 改成 `image_auto_cal.cfg`
> 再 `make image` 即可。

## 5. 产物（2026-09-11 实测）

| 文件 | 大小 | 说明 |
|---|---|---|
| `project/demo/FontExp/image/xr872/xr_system.img` | **1,304,408 B** | ★ 整机镜像，phoenixMC 烧录这一个即可 |
| `project/demo/FontExp/image/xr872/app.bin` | 112,368 B | RAM 段 |
| `project/demo/FontExp/image/xr872/app_xip.bin` | 804,836 B | flash XIP 段 |
| `project/demo/FontExp/image/xr872/app_psram.bin` | 319,960 B | PSRAM 段 |
| `project/demo/FontExp/gcc/FontExp.bin/.axf/.map` | — | 原始 bin / 带 调试符号 / 内存映射 |

全流程实测耗时：首次编译+链接 **114 秒**（库已就绪），修 cfg 后补打包 **9 秒**；
全新机器加上组件库首次编译，预期 10~20 分钟。

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
见 §4.4，读重排版 cfg 重跑即可。

### 6.4 换机器工具链路径
`gcc.mk` 首行 `CC_DIR := /home/Administrator/tools/gcc-arm-none-eabi-4_9-2015q2/bin`
改成你的工具链所在目录。

### 6.5 干净重编
`make build_clean`（清工程+库）；只清工程 `make clean`；只清镜像
`make image_clean`。

## 7. 本次验证记录

- 日期：2026-09-11；SDK：官方 GitHub master（`16624d5`，v1.1.1）全新 clone；
- 装配：`setup.sh`（7 文件最小补丁 + 三工程 + .config）一次通过；
- FontExp `make build`：编译链接通过（首次 114s），mkimage 段重叠 →
  Makefile 改读 `image_auto_cal.cfg` → 重跑 9s 出图，`xr_system.img`
  1,304,408 字节，`EXIT:0`；
- 解耦复验：`fs_ctrl_format`/newlib 桩/XIP 链接脚本/384M 板级配置全部
  搬入工程后重建，FontExp 出图 **1,304,408 字节与解耦前完全一致**
  （gen1 同样 1,328,984 字节一致），行为等价。
