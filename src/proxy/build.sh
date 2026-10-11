#!/bin/sh
# 构建自研 dinput8 代理（v2：FPakFile 构造函数钩子）
# 只用 zig 自带的 LLVM 工具链，不需要 Windows SDK / VS。
set -e
cd "$(dirname "$0")"

# Git Bash/MSYS 会把 lld-link 的 /dll /nodefaultlib 当成绝对路径去做转换，必须关掉
export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL='*'

# zig 工具链：优先环境变量 ZIG → PATH → 本机默认位置。
ZIG="${ZIG:-$(command -v zig || echo 'C:/zig/zig.exe')}"
[ -x "$ZIG" ] || { echo "找不到 zig：$ZIG"
    echo "请到 https://ziglang.org/download/ 下载，或 export ZIG=/path/to/zig"; exit 1; }
OUT="${OUT:-build}"
mkdir -p "$OUT"

echo "[1/4] 生成 kernel32 导入库 (k32.def -> k32.lib)"
"$ZIG" dlltool -m i386:x86-64 -d k32.def -l "$OUT/k32.lib"

echo "[2/4] 编译 ac8proxy.c (freestanding, 无 CRT)"
"$ZIG" cc -target x86_64-windows-gnu -O2 -ffreestanding -fno-builtin \
      -nostdlibinc -fno-stack-protector -Wall -Wextra \
      -c ac8proxy.c -o "$OUT/ac8proxy.obj"

echo "[3/4] 汇编 ac8hook.s"
"$ZIG" cc -target x86_64-windows-gnu -c ac8hook.s -o "$OUT/ac8hook.obj"

echo "[4/4] 链接 dinput8.dll"
"$ZIG" lld-link /dll /nodefaultlib /entry:DllMain /subsystem:windows \
      "/out:$OUT/dinput8.dll" \
      "$OUT/ac8proxy.obj" "$OUT/ac8hook.obj" "$OUT/k32.lib"

ls -la "$OUT/dinput8.dll"
echo "built $OUT/dinput8.dll"
