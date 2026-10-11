#!/bin/sh
# 构建「AC8 简体汉化管理器」单文件 exe，并组装成可直接分发的目录。
# 只需 MinGW GCC（本机 Qt 自带 13.1.0），不需要 VS。
set -e
cd "$(dirname "$0")"

# 编译器：优先环境变量 GCC / WINDRES → PATH → 本机常见的 Qt 自带 MinGW。
GCC="${GCC:-$(command -v gcc || echo 'C:/Qt/Tools/mingw1310_64/bin/gcc.exe')}"
WINDRES="${WINDRES:-$(command -v windres || echo 'C:/Qt/Tools/mingw1310_64/bin/windres.exe')}"
[ -x "$GCC" ] || { echo "找不到 gcc：$GCC"
    echo "请安装 MinGW-w64，或 export GCC=/path/to/gcc.exe"; exit 1; }
[ -x "$WINDRES" ] || { echo "找不到 windres：$WINDRES"; exit 1; }
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
       -lcomctl32 -lshell32 -lole32 -luser32 -lgdi32

echo "[3/4] 组装发布目录 $DIST"
# 注意：这里**不删目录**、只覆盖文件 —— 环境的 safe-delete 会拦 `rm -rf`；
# 保留目录也顺带避免误清 `_backup\` 和 ini 这类用户数据。
mkdir -p "$DIST/payload"
cp "$OUT/AC8Manager.exe"                       "$DIST/AC8汉化管理器.exe"
cp "../proxy/build/dinput8.dll"                "$DIST/payload/dinput8.dll"
# 译文包（可选）：自己生成过就一起放进 payload。仓库里不含译文包，见 README。
if [ -f "../dist/pakchunk0-Windows_998_P.pak.new" ]; then
  cp "../dist/pakchunk0-Windows_998_P.pak.new" "$DIST/payload/pakchunk0-Windows_998_P.pak"
else
  echo "（跳过译文包 —— 没找到 ../dist/pakchunk0-Windows_998_P.pak.new）"
fi

echo "[4/4] 校验"
ls -la "$DIST" "$DIST/payload"
md5sum "$DIST/AC8汉化管理器.exe" "$DIST/payload/"*
echo "built $DIST"
