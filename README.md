# ACE COMBAT 8 简体汉化管理器

为《皇牌空战 8》补充简体中文文本的 Windows 工具。

<a href="https://github.com/Carpenter-MK1/AC8_Better_CN_Localization/releases/latest"><img src="https://img.shields.io/github/v/release/Carpenter-MK1/AC8_Better_CN_Localization?label=version" alt="version"></a>
<a href="https://github.com/Carpenter-MK1/AC8_Better_CN_Localization/releases"><img src="https://img.shields.io/github/downloads/Carpenter-MK1/AC8_Better_CN_Localization/total" alt="downloads"></a>
<img src="https://img.shields.io/badge/platform-Windows%2010%2F11-0078D4" alt="platform">
<img src="https://img.shields.io/badge/license-MIT-blue" alt="license">

> **仅限离线单人游玩。** 启用汉化后进入多人模式可能被反作弊系统判定为异常并封禁账号。
> 进入多人模式前请先停用汉化。
>
> 本工具为非官方作品，需要正版游戏，不包含任何游戏本体资源。

[概述](#概述) · [安装](#安装) · [写入的文件](#写入的文件) · [功能](#功能) · [命令行](#命令行) · [常见问题](#常见问题) · [从源码构建](#从源码构建) · [更新日志](CHANGELOG.md)

---

## 概述

《皇牌空战 8》的简体中文文本基于日语文本直译，在使用英语语音时体验较差。本工具以独立补丁包的
形式加载基于英语文本重新翻译的译文，补齐界面文本、字幕、无线电通话、机体图鉴与涂装说明，
共 4.6 万余条。

游戏自身的文件不被修改。启用时写入两个文件，停用时删除这两个文件，游戏即恢复原版。

## 安装

1. 从 [Releases](https://github.com/Carpenter-MK1/AC8_Better_CN_Localization/releases/latest)
   下载 `AC8_CN_Manager_<版本>.zip`（约 590 KB）
2. 解压到任意目录。不需要管理员权限，不要解压到游戏目录
3. 运行 `AC8汉化管理器.exe`
4. 确认「游戏目录」指向正确位置，如有误点【浏览…】重新选择
5. 点【启用汉化】，再点【启动游戏】

首次启用时会显示使用须知，需停留 3 秒后方可确认。该须知在每台设备上只出现一次。

游戏运行期间启用不会生效。若游戏已启动，请先退出再执行第 5 步。

也可以不使用管理器，直接把 `payload\` 下的两个文件复制到游戏对应位置，效果相同。

## 写入的文件

| 文件 | 目标位置 | 作用 |
|---|---|---|
| `pakchunk0-Windows_998_P.pak` | `Game\Content\Paks\` | 译文数据 |
| `dinput8.dll` | `Game\Binaries\Win64\` | 使未签名补丁包能够被游戏加载 |

停用汉化时，上述两个文件被删除。目录中若已有同名文件，覆盖前会备份到管理器目录下的 `_backup\`。

管理器只读写这两个文件名，不扫描、移动或删除其它文件，也不读取存档。

## 功能

- **启用 / 停用汉化**：写入或删除上述两个文件
- **状态检查**：译文包版本与构建时间、代理 DLL 是否就位、游戏版本是否适配、上次启动的加载结果
- **启动游戏**：通过 Steam 启动，无需修改启动参数
- **检查更新**：下载译文包与代理 DLL 后校验文件大小与 SHA-256，全部通过才替换；
  任一步骤失败则不改动现有文件。启动时自动检查一次，仅在新版本存在时提示
- **命令行接口**：`status`、`enable`、`disable`、`launch`、`check`、`update`

界面显示的「汉化包 构建于 …」取自译文包自身的文件时间戳，表示当前安装的译文版本，
与管理器版本无关。

## 命令行

```
AC8汉化管理器.exe            启动图形界面
AC8汉化管理器.exe status     输出状态，就绪时退出码为 0
AC8汉化管理器.exe enable     启用汉化
AC8汉化管理器.exe disable    停用汉化
AC8汉化管理器.exe launch     启动游戏
AC8汉化管理器.exe check      仅检查更新，退出码 0 表示已是最新，3 表示有新版本
AC8汉化管理器.exe update     检查并安装更新
```

## 常见问题

**启用后游戏内仍是英文**

依次检查：管理器状态是否显示「已启用」且「就绪」；`Game\Content\Paks\` 下是否存在译文包；
启用时游戏是否已完全退出。游戏运行期间写入的文件不会在本次运行中生效。

**部分 HUD 文本仍是英文**

HUD 上的目标标识（`GUN`、`MISSILE`、`FLARE` 等）为预渲染位图字体，不属于文本资源，
译文包无法覆盖。官方英文版同样如此。

**游戏更新后汉化失效**

代理 DLL 依赖游戏可执行文件的指纹。游戏更新后指纹发生变化，DLL 会停止挂钩，
状态显示「游戏版本未适配」。此情况需要等待对应版本的更新，无法通过重装解决。

**杀毒软件报告 `dinput8.dll`**

该 DLL 在游戏启动早期挂钩资源加载，行为特征与注入类程序相似，可能被误报。
源码位于 [`src/proxy/`](src/proxy/)，仅依赖 `kernel32`，不包含第三方二进制。
确认后可加入信任列表。

**如何完全卸载**

点【停用汉化】，然后删除管理器所在目录。`_backup\` 中是覆盖前的备份，可一并删除。
游戏目录不会残留文件。

**能否与其它 mod 共存**

补丁包按文件名通配放行（`pakchunk0-Windows_<任意>_P.pak`），使用其它编号的补丁包可以与本工具共存。
与其它 mod 同时挂载的情况未做完整测试。若其它 mod 也使用 `dinput8.dll`，两者不能同时安装。

**多人模式是否安全**

存在封禁风险，具体判定条件未知。进入多人模式前请先停用汉化。

<details>
<summary>国内网络下载</summary>

Release 附件由 GitHub 提供，国内网络可能无法直接访问。以下转发服务经实测可用，
用法是在原地址前加上前缀：

| 服务 | 说明 |
|---|---|
| `https://ghfast.top/` + 原地址 | 可代理 Release 附件下载 |
| `https://cdn.jsdelivr.net/gh/Carpenter-MK1/AC8_Better_CN_Localization@main/` + 仓库内路径 | 仅提供仓库内文件，不含 Release 附件 |

若只需更新译文，不必重新下载整个压缩包，可直接获取 `dist/` 下的文件：

- 译文包：`dist/pakchunk0-Windows_998_P.pak`
- 代理 DLL：`dist/dinput8.dll`
- 更新清单：`dist/version.json`

将文件放入管理器的 `payload\` 目录，文件名保持不变，然后点【启用汉化】。

以上为第三方服务，可用性与安全性由服务方负责。条件允许时请使用 GitHub 官方地址。

</details>

## 从源码构建

需要 [MinGW-w64 GCC](https://www.mingw-w64.org/)（含 `windres`）和 [zig](https://ziglang.org/)。

```bash
cd src/proxy && sh build.sh     # 代理 DLL
cd ../gui    && sh build.sh     # 管理器，产物在 AC8汉化管理器_发布版\
```

`src/proxy/selftest.py` 从 `ac8proxy.c` 中提取实际的包名匹配函数，生成 C 测试程序并运行 15 个用例，
用于验证补丁包被放行、官方容器不被误伤。

## 工作原理

游戏会校验每个资源容器的数字签名，未签名的 `.pak` 无法被加载。本工具通过 `dinput8.dll` 在游戏进程
启动早期挂钩容器构造函数，仅对补丁包跳过签名校验，官方容器仍走原有校验路径。

挂钩位置、包名规则与更新清单格式见 [docs/工作原理.md](docs/工作原理.md)。

## 已知限制

- 游戏更新后需要等待对应版本的适配
- HUD 位图文字不在汉化范围内
- 与其它使用 `dinput8.dll` 的 mod 不能同时安装
- 仅支持 Windows 10 / 11
- 汉化只替换简体中文语言槽的文本，不改变游戏的其它行为

## 免责声明

- 本工具为非官方作品，与游戏开发商、发行商无关联
- 需要正版游戏。本仓库与发布包不包含任何游戏本体资源
- 仅限离线单人游玩。启用汉化后进入多人模式可能导致账号被封禁
- 使用本工具的后果由使用者自行承担，作者不对账号封禁、存档损坏、设备故障等损失负责
- 建议使用前备份存档

## 许可

MIT，见 [LICENSE](LICENSE)。译文由本项目译者贡献，欢迎提交修正。
