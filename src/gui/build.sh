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
DIST="../../AC8汉化管理器_发布版"

mkdir -p "$OUT"

echo "[1/4] 编译资源（清单 / 视觉样式 / 免提权）"
"$WINDRES" app.rc -O coff -o "$OUT/app.res"

echo "[2/4] 编译 + 静态链接"
# winhttp → 检查更新；advapi32 → sha256（CryptoAPI）。都是系统自带 DLL，不引入第三方依赖。
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
cp "$OUT/AC8Manager.exe" "$DIST/AC8汉化管理器.exe"

# 代理 DLL：优先用 src/proxy 刚编出来的，其次用 dist/ 里现成的
if [ -f "../proxy/build/dinput8.dll" ]; then
  cp "../proxy/build/dinput8.dll" "$DIST/payload/dinput8.dll"
elif [ -f "../../dist/dinput8.dll" ]; then
  cp "../../dist/dinput8.dll" "$DIST/payload/dinput8.dll"
else
  echo "（跳过代理 DLL —— 先跑 src/proxy/build.sh）"
fi

# 译文包与更新清单：本仓库不含译文包，dist/ 里只有代理 DLL 与清单
for f in pakchunk0-Windows_998_P.pak version.json; do
  if [ -f "../../dist/$f" ]; then
    cp "../../dist/$f" "$DIST/payload/$f"
  elif [ -f "../../dist/pakchunk0-Windows_998_P.pak.new" ]; then
    cp "../../dist/pakchunk0-Windows_998_P.pak.new" "$DIST/payload/pakchunk0-Windows_998_P.pak"
  else
    echo "（跳过 $f —— 没找到）"
  fi
done

echo "[4/4] 校验"
ls -la "$DIST" "$DIST/payload"
md5sum "$DIST/AC8汉化管理器.exe" "$DIST/payload/"*
echo "built $DIST"
