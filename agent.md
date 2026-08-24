# agent.md — Inkfrog FontExp 编译指南

面向开发者的完整编译 / 打包 / 烧录指南。改代码前请先通读「注意事项」，少踩坑。

---

## 1. 环境要求

| 依赖 | 说明 |
|:-----|:-----|
| XR872 SDK | `xradio-skylark-sdk-master`，本工程位于其 `project/demo/FontExp` 下，不能脱离 SDK 单独编译 |
| 交叉工具链 | `gcc-arm-none-eabi 4.9 2015q2`（本机路径 `C:/XR872/home/Administrator/tools/gcc-arm-none-eabi-4_9_2015q2/bin`） |
| Shell | Windows + Git Bash（`C:\XR872\bin\bash.exe`），SDK 的 gcc.mk 依赖该环境 |
| Python 3 | 运行 `tools/` 下的生成脚本 |
| 板级配置 | `project/common/board/xr872_evb_ai`（Makefile 内已指定，无需手改） |

> `gcc/localconfig.mk` 是本机工具链路径配置，**不入库**；新环境第一次编译前按自己的安装路径创建。

## 2. 编译

```bash
cd project/demo/FontExp

# （可选）改过 web/l1glyf_builder.js 才需要：重新生成内嵌 JS
python tools/gen_l1glyf_web_js.py

cd gcc
make all install
```

`make install` 会把 `FontExp.bin` / `FontExp_xip.bin` / `FontExp_psram.bin` 拷到 `image/xr872/`（`app*.bin`）。

## 3. 打包镜像（关键步骤）

`make install` **不会**自动生成 img，必须手动打包，且必须用 `-O`（auto-cal）模式：

```bash
cd ../image/xr872
<SDK_ROOT>/tools/mkimage.exe -O -c image.cfg
```

产物：`image/xr872/xr_system.img`

> ⚠️ 为什么必须 `-O`：普通模式 `mkimage -c image.cfg` 不会把 OTA 地址/大小写进 boot 段头（OTA 字段变成无效值 `0xFFFFFFFF`），固件运行后 file-OTA 擦除地址错乱、无法刷机。

## 4. 烧录

- 图形工具：phoenixMC（选择 `xr_system.img`）
- 脚本：`phoenix_flash.py`（配合 XR872 的 UART 启动模式）

## 5. 版本号

- `version.h`：`INKFROG_VERSION_MAJOR / MINOR / STR`，关于页面显示的编译时间由 `__DATE__ / __TIME__` 自动填充
- `image/xr872/image.cfg`：`"version"` 字段
- **两处必须同步修改**，改完需重新编译打包才会生效

## 6. 注意事项（踩坑记录）

1. **改了 C / JS / 配置 / 镜像布局，必须当轮完成编译 + 打包**，不要只改代码不出镜像；仅文档/注释改动可跳过。
2. bash 里路径写 `c:/XR872/...`，不要写 `/c/XR872/...`（SDK 脚本对后者处理不一致）。
3. 内存布局依赖 XIP + PSRAM 分区（`app.bin` / `app_xip.bin` / `app_psram.bin` 三段），链接脚本在 SDK 的 `project/linker_script/gcc/appos.ld`，改动需重新生成 `.project.ld`。
4. 构建产物（`*.o` / `*.d` / `*.axf` / `app*.bin` / `xr_system.img` 等）已被 `.gitignore` 排除，**不要想办法提交它们**。
5. 根目录开发笔记（`/*.md`）同样被忽略，文档统一写进 `README.md` 或本文件。
