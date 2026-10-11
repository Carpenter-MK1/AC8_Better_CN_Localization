# ACE COMBAT 8 · 简体汉化管理器

给《ACE COMBAT 8》安装简体中文补丁的一键工具 —— 单文件 GUI、零依赖、免管理员。

> ⚠️ **仅限离线单人游玩。启用汉化后请勿进入多人模式 —— 有封号风险。**
> 本工具为非官方作品，**需要正版游戏**才能使用。

---

## 功能

| 功能 | 说明 |
|---|---|
| **一键启用 / 停用** | 从同目录 `payload\` 装入或移除汉化文件；被覆盖掉的旧文件自动备份到 `_backup\` |
| **状态自检** | 汉化包（含**构建时间**）、代理 DLL、游戏版本是否适配、上次运行有没有加载成功 |
| **启动游戏** | 走 Steam 启动，无需额外参数 |
| **首次使用须知** | 免责声明 + 多人模式警告，必须停留 3 秒才能确认 |
| **命令行** | `status` / `enable` / `disable` / `launch`，便于脚本化（`status` 就绪时退出码 0） |

界面上的「汉化包 构建于 …」取自译文包**自身的文件时间戳**，所以它反映的是**实际装着的那一版**的构建时间。

## 下载与使用

1. 到 [Releases](../../releases) 下载最新版压缩包，解压到任意位置（**不需要管理员权限**）
2. 双击 `AC8汉化管理器.exe`
3. 确认「游戏目录」正确（不对就点「浏览…」）
4. 点「**启用汉化**」→ 点「**启动游戏**」

想恢复原版：点「**停用汉化**」即可移除汉化文件，**官方文件从头到尾不会被改动**。

### 国内直连下载

`github.com` 的网页端在国内经常打不开，但 **`raw.githubusercontent.com` 可以直连**：

| 文件 | 直链 |
|---|---|
| 译文包 | `https://raw.githubusercontent.com/Carpenter-MK1/ac8-hanhua-manager/main/dist/pakchunk0-Windows_998_P.pak` |
| 代理 DLL | `https://raw.githubusercontent.com/Carpenter-MK1/ac8-hanhua-manager/main/dist/dinput8.dll` |
| 版本信息 | `https://raw.githubusercontent.com/Carpenter-MK1/ac8-hanhua-manager/main/dist/version.txt` |

下载后放进管理器的 `payload\` 目录，再点「启用汉化」即可。

管理器里的【检查更新】就是读上面那个 `version.txt` 来比对版本的。

## 从源码构建

### 前置

| 组件 | 用途 | 备注 |
|---|---|---|
| **MinGW-w64 GCC** | 编译管理器 GUI | 需要 `gcc` 和 `windres` |
| **zig** | 编译代理 DLL | 只用它自带的 LLVM 工具链，**不需要 Windows SDK / VS** |

两者放进 `PATH` 即可，也可以用环境变量指定：

```bash
export GCC=/path/to/gcc.exe
export WINDRES=/path/to/windres.exe
export ZIG=/path/to/zig.exe
```

### 构建

```bash
# 1) 代理 DLL（先编这个，管理器的 payload 会用到）
cd src/proxy && sh build.sh

# 2) 管理器（编译 GUI + 组装发布目录）
cd ../gui && sh build.sh
```

产物在 `AC8汉化管理器_发布版\`。

### 自测（可选，但推荐）

`src/proxy/selftest.py` 会**从 `ac8proxy.c` 里抽出真实的包名匹配函数**，生成一个 C 自测程序并跑 15 个用例 ——
比手抄可靠，能顺带验证「自己的包被放行、官方容器不被误伤」：

```bash
cd src/proxy
python selftest.py
zig cc -o _test_match.exe _test_match.c && ./_test_match.exe
```

## 目录结构

```
├── dist/            译文包与代理 DLL（供下载、以及「检查更新」比对）
│   ├── pakchunk0-Windows_998_P.pak
│   ├── dinput8.dll
│   └── version.txt  版本号 + 本次更新说明
├── src/
│   ├── gui/          管理器（Win32 GUI，单文件 C）
│   │   ├── main.c
│   │   ├── app.rc / app.manifest     （视觉样式 + 免提权清单）
│   │   └── build.sh
│   └── proxy/        代理 DLL（让未签名补丁包能被加载）
│       ├── ac8proxy.c    钩子安装 + 包名匹配
│       ├── ac8hook.s     手写汇编跳板（rsp 精确还原）
│       ├── win32_min.h / k32.def
│       ├── selftest.py   包名匹配规则自测
│       └── build.sh
└── docs/
    └── 使用说明.txt   随发布包一起分发的简版说明
```

## 工作原理（简述）

游戏会校验每个资源容器（`.pak`）的数字签名，**未签名**的容器直接被拒绝加载 —— 这就是普通 mod 包放不进去的原因。

本工具装进去的 `dinput8.dll` 在游戏进程启动的**最早期**挂钩 `FPakFile` 构造函数入口，
对名称形如 `pakchunk0-Windows_<任意>_P.pak` 的**补丁包**强制跳过签名校验；**官方容器完全不受影响**。

> 为什么能用 `dinput8.dll` 这个名字：游戏静态导入 `DINPUT8.dll`，Windows 会在**加载 exe 导入表时**
> 就加载它 —— 这是最早的用户态执行时机，来得及在所有容器挂载之前装好钩子。
> 本 DLL 只依赖 `kernel32`，**不含任何第三方二进制、不依赖 MinGW/CRT 运行时**。

因为包名规则是**通配**而不是写死某一个名字，本工具可以与使用**其它包名**（例如 `pakchunk0-Windows_999_P.pak`）的
mod **共存**。

## ⚠️ 免责声明与使用限制

- 本工具**非官方**，与游戏开发商、发行商没有任何关系。
- **需要正版游戏。** 本仓库与发布包**不含任何游戏本体资源**。
- **只用于离线单人游玩。** 启用汉化后进入多人模式可能被反作弊系统判定为异常，
  **导致账号被封禁** —— 请务必在进多人模式前先「停用汉化」。
- 使用本工具的一切后果由**使用者自行承担**；作者不对账号封禁、存档损坏、设备故障等任何损失负责。
- 建议使用前**备份存档**。

## 已知限制

- **游戏更新后**，可执行文件指纹会变化，代理 DLL 会主动拒绝挂钩（状态里显示「游戏版本 **未适配**」），
  需要等待对应版本的更新。
- 汉化只覆盖**简体中文语言槽的文本**；HUD 等**位图文字**不在覆盖范围内。
- 与**同名** `dinput8.dll` 的其它 mod 无法同时安装（后装的会覆盖先装的）。
- 仅支持 Windows。

## 许可

MIT，见 [LICENSE](LICENSE)。
