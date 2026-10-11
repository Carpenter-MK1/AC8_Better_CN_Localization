#!/bin/sh
# 构建「AC8 简体汉化管理器」单文件 exe，并组装成可直接分发的目录。
# 只需 MinGW GCC（本机 Qt 自带 13.1.0），不需要 VS。
set -e
cd "$(dirname "$0")"

GCC="C:/Qt/Tools/mingw1310_64/bin/gcc.exe"
WINDRES="C:/Qt/Tools/mingw1310_64/bin/windres.exe"
OUT="build"
DIST="../AC8汉化管理器_发布版"

mkdir -p "$OUT"

echo "[1/4] 编译资源（清单 / 视觉样式 / 免提权）"
"$WINDRES" app.rc -O coff -o "$OUT/app.res"

echo "[2/4] 编译 + 静态链接"
"$GCC" -O2 -Wall -Wextra \
       -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE \
       -static -static-libgcc -s -mwindows \
       -o "$OUT/AC8Manager.exe" \
       main.c "$OUT/app.res" \
       -lcomctl32 -lshell32 -lole32 -luser32 -lgdi32 -lwinhttp -ladvapi32

echo "[3/4] 组装发布目录 $DIST"
# 注意：这里**不删目录**、只覆盖文件 —— 环境的 safe-delete 会拦 `rm -rf`；
# 保留目录也顺带避免误清 `_backup\` 和 ini 这类用户数据。
mkdir -p "$DIST/payload"
cp "$OUT/AC8Manager.exe"                       "$DIST/AC8汉化管理器.exe"
cp "../proxy/build/dinput8.dll"                "$DIST/payload/dinput8.dll"
cp "../dist/pakchunk0-Windows_998_P.pak.new"   "$DIST/payload/pakchunk0-Windows_998_P.pak"

echo "[4/4] 校验"
ls -la "$DIST" "$DIST/payload"
md5sum "$DIST/AC8汉化管理器.exe" "$DIST/payload/"*
echo "built $DIST"
