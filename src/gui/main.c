/* =============================================================================
 *  ACE COMBAT 8 · 简体汉化管理器
 *
 *  单文件 Win32 GUI（MinGW 静态编译，零依赖、免管理员、双击即用）。
 *
 *  它做四件事：
 *    ① 自动找到游戏目录（注册表 → libraryfolders.vdf → 手动指定，记到 ini）
 *    ② 一键启用/停用汉化（从同目录 payload\ 拷入/删除；已有非本工具的旧文件先备份）
 *    ③ 按需启动游戏（走 Steam，实测不带任何参数也能加载汉化）
 *    ④ 状态自检：汉化包（含构建时间）、代理 DLL、游戏版本适配、上次运行加载情况
 *    ⑤ 首次「启用汉化」弹「使用前须知」：免责声明 + 多人模式封号警告（红字），须停留 3 秒才可确认
 *
 *  也支持命令行（脚本/自动化友好）：
 *    AC8汉化管理器.exe status | enable | disable | launch | help
 *    status 退出码：0 = 完全就绪，1 = 未装好或有问题
 * ============================================================================= */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdarg.h>
#include <string.h>

/* ------------------------------- 常量 ----------------------------------- */

#define APP_TITLE    L"ACE COMBAT 8 · 简体汉化管理器"
#define APP_VER      L"1.2"

#define PAK_NAME     L"pakchunk0-Windows_998_P.pak"
#define DLL_NAME     L"dinput8.dll"
#define LOG_NAME     L"AC8CNDinput.log"

/* 本工具代理 DLL 的识别标记 —— 必须与 proxy/ac8proxy.c 里的 AC8CN_DLL_TAG 一字不差。 */
static const char kDllMarker[] = "AC8CN-PROXY-DINPUT8-V3";

/* 本 DLL 认的游戏版本指纹（与 ac8proxy.c 里的守卫一致） */
#define EXPECT_TIMESTAMP 0x6AA0E27Du
#define EXPECT_SIZEIMAGE 0x2195A000u

#define STEAM_APPID  L"2288340"

/* ------------------------------- 控件 ID -------------------------------- */

#define IDC_PATH      1001
#define IDC_BROWSE    1002
#define IDC_STATUS    1010
#define IDC_LOG       1011
#define IDC_HINT      1012
#define IDC_ENABLE    1020
#define IDC_DISABLE   1021
#define IDC_LAUNCH    1022
#define IDC_REFRESH   1023
#define IDC_OPENFOLDER 1024

/* ------------------------------- 全局 ----------------------------------- */

static HINSTANCE g_hInst;
static HWND g_hMain, g_hPath, g_hStatus, g_hLog, g_hHint;
static HFONT g_font;
static int   g_dpi = 96;

static wchar_t g_exeDir[MAX_PATH];      /* 管理器自己所在的目录 */
static wchar_t g_payloadDir[MAX_PATH];  /* 同目录下的 payload\ */

#define S(x)  MulDiv((x), g_dpi, 96)

/* ------------------------------ 小工具 ---------------------------------- */

static BOOL fexists(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL dexists(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned long long fsize(const wchar_t* p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d)) return 0;
    return ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

/* 文件时间戳 -> "2026-10-11 14:10"（本地时区） */
static void file_time_str(const wchar_t* path, wchar_t* out)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    FILETIME local;
    SYSTEMTIME st;
    out[0] = 0;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return;
    if (!FileTimeToLocalFileTime(&d.ftLastWriteTime, &local)) return;
    if (!FileTimeToSystemTime(&local, &st)) return;
    wsprintfW(out, L"%04d-%02d-%02d %02d:%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

/* 分组数字：1234567 -> "1,234,567"（英文逗号，纯 ASCII，排版稳定） */
static void commafy(unsigned long long v, wchar_t* out)
{
    wchar_t raw[32];
    int n = 0, j = 0;
    if (!v) { out[0] = L'0'; out[1] = 0; return; }
    while (v && n < 30) { raw[n++] = (wchar_t)(L'0' + (int)(v % 10)); v /= 10; }
    while (n > 0) {
        out[j++] = raw[--n];
        if (n && (n % 3) == 0) out[j++] = L',';
    }
    out[j] = 0;
}

/* 读文件（最多 max 字节），返回实际读到的字节数 */
static int read_head(const wchar_t* path, void* buf, int max)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, buf, (DWORD)max, &got, NULL);
    CloseHandle(h);
    return (int)got;
}

static BOOL bytes_contain(const char* hay, int hayLen, const char* needle)
{
    int nl = (int)strlen(needle), i;
    if (nl <= 0 || hayLen < nl) return 0;
    for (i = 0; i + nl <= hayLen; ++i)
        if (memcmp(hay + i, needle, (size_t)nl) == 0) return 1;
    return 0;
}

static BOOL file_has_marker(const wchar_t* path, const char* marker)
{
    static char buf[64 * 1024];
    int n = read_head(path, buf, (int)sizeof(buf));
    return bytes_contain(buf, n, marker);
}

/* UTF-8 字节 -> 宽字符（失败则原样按 ANSI 转） */
static void u8_to_w(const char* s, int len, wchar_t* out, int outCap)
{
    int n;
    if (len < 0) len = (int)strlen(s);
    n = MultiByteToWideChar(CP_UTF8, 0, s, len, out, outCap - 1);
    if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, s, len, out, outCap - 1);
    if (n < 0) n = 0;
    out[n] = 0;
}

/* --------------------------- 路径探测 ----------------------------------- */

static void get_exe_dir(wchar_t* out)
{
    wchar_t* p;
    GetModuleFileNameW(NULL, out, MAX_PATH);
    p = wcsrchr(out, L'\\');
    if (p) *p = 0;
}

static BOOL game_ok(const wchar_t* root)
{
    wchar_t p[MAX_PATH * 2];
    if (!root || !root[0]) return FALSE;
    wsprintfW(p, L"%s\\Game\\Binaries\\Win64\\AceCombat8.exe", root);
    return fexists(p);
}

static BOOL reg_read_steam(wchar_t* out)
{
    static const struct { HKEY root; const wchar_t* sub; } T[] = {
        { HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam" },
    };
    int i;
    for (i = 0; i < 3; ++i) {
        HKEY k;
        DWORD type = 0, cb = MAX_PATH * sizeof(wchar_t);
        if (RegOpenKeyExW(T[i].root, T[i].sub, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        if (RegQueryValueExW(k, L"InstallPath", NULL, &type, (LPBYTE)out, &cb) == ERROR_SUCCESS
            && type == REG_SZ && out[0]) {
            RegCloseKey(k);
            return TRUE;
        }
        RegCloseKey(k);
    }
    return FALSE;
}

/* 从 libraryfolders.vdf 里捞出所有 "path" 的值 */
static int vdf_collect_paths(const char* txt, int len, wchar_t* out, int outStride, int maxOut)
{
    int i = 0, n = 0;
    while (i < len) {
        if (txt[i] != '"') { ++i; continue; }
        /* 读一个引号串 */
        {
            int s = i + 1, e;
            for (e = s; e < len && txt[e] != '"'; ++e) { }
            if (e >= len) break;
            if (e - s == 4 && memcmp(txt + s, "path", 4) == 0) {
                /* 跳过空白，读下一个引号串 */
                int k = e + 1;
                while (k < len && (txt[k] == ' ' || txt[k] == '\t' || txt[k] == '\r' || txt[k] == '\n')) ++k;
                if (k < len && txt[k] == '"') {
                    int s2 = k + 1, e2, j = 0;
                    wchar_t* dst;
                    if (n >= maxOut) break;
                    dst = out + (size_t)n * outStride;
                    for (e2 = s2; e2 < len && txt[e2] != '"' && j < outStride - 1; ++e2) {
                        if (txt[e2] == '\\' && e2 + 1 < len && txt[e2 + 1] == '\\') { dst[j++] = L'\\'; ++e2; }
                        else dst[j++] = (wchar_t)(unsigned char)txt[e2];
                    }
                    dst[j] = 0;
                    ++n;
                    i = e2 + 1;
                    continue;
                }
            }
            i = e + 1;
        }
    }
    return n;
}

/* 顺序：ini 记录 → 常见默认位置 → 注册表 Steam → libraryfolders.vdf */
static BOOL find_game(const wchar_t* iniPath, wchar_t* out)
{
    wchar_t steam[MAX_PATH], list[MAX_PATH * 2], vdf[MAX_PATH * 2];
    static char raw[256 * 1024];
    static wchar_t cands[32][MAX_PATH * 2];
    int i, n;
    const wchar_t* defs[] = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\ACE COMBAT 8",
        L"C:\\Program Files\\Steam\\steamapps\\common\\ACE COMBAT 8",
        L"D:\\Steam\\steamapps\\common\\ACE COMBAT 8",
        L"D:\\SteamLibrary\\steamapps\\common\\ACE COMBAT 8",
        L"E:\\SteamLibrary\\steamapps\\common\\ACE COMBAT 8",
        NULL
    };

    if (iniPath && iniPath[0]) {
        GetPrivateProfileStringW(L"ac8cn", L"GameDir", L"", out, MAX_PATH, iniPath);
        if (game_ok(out)) return TRUE;
    }

    for (i = 0; defs[i]; ++i) if (game_ok(defs[i])) { lstrcpynW(out, defs[i], MAX_PATH); return TRUE; }

    if (!reg_read_steam(steam)) return FALSE;
    wsprintfW(list, L"%s\\steamapps\\common\\ACE COMBAT 8", steam);
    if (game_ok(list)) { lstrcpynW(out, list, MAX_PATH); return TRUE; }

    wsprintfW(vdf, L"%s\\steamapps\\libraryfolders.vdf", steam);
    n = read_head(vdf, raw, (int)sizeof(raw));
    if (n > 0) {
        int cnt = vdf_collect_paths(raw, n, (wchar_t*)cands, MAX_PATH * 2, 32);
        for (i = 0; i < cnt; ++i) {
            wsprintfW(list, L"%s\\steamapps\\common\\ACE COMBAT 8", cands[i]);
            if (game_ok(list)) { lstrcpynW(out, list, MAX_PATH); return TRUE; }
        }
    }
    return FALSE;
}

/* --------------------------- 状态自检 ----------------------------------- */

typedef struct {
    wchar_t gameRoot[MAX_PATH];
    BOOL haveGame;

    wchar_t pakPath[MAX_PATH * 2];
    BOOL pakOk;   unsigned long long pakSize;
    wchar_t pakTime[64];        /* 汉化包的构建时间（取自 pak 的文件时间戳） */

    wchar_t dllPath[MAX_PATH * 2];
    BOOL dllOk;   unsigned long long dllSize;  BOOL dllOurs;

    BOOL fpRead;  DWORD ts, so;  BOOL fpOk;

    wchar_t logPath[MAX_PATH * 2];
    BOOL logOk;   BOOL logHit;
} Status;

static void probe_status(Status* s, const wchar_t* gameRoot, const wchar_t* iniPath)
{
    wchar_t exe[MAX_PATH * 2];
    unsigned char head[512];
    int n;

    ZeroMemory(s, sizeof(*s));
    if (gameRoot && gameRoot[0]) lstrcpynW(s->gameRoot, gameRoot, MAX_PATH);
    s->haveGame = game_ok(s->gameRoot);

    wsprintfW(s->pakPath, L"%s\\Game\\Content\\Paks\\%s", s->gameRoot, PAK_NAME);
    s->pakOk = fexists(s->pakPath);
    s->pakSize = s->pakOk ? fsize(s->pakPath) : 0;
    if (s->pakOk) file_time_str(s->pakPath, s->pakTime);

    wsprintfW(s->dllPath, L"%s\\Game\\Binaries\\Win64\\%s", s->gameRoot, DLL_NAME);
    s->dllOk = fexists(s->dllPath);
    s->dllSize = s->dllOk ? fsize(s->dllPath) : 0;
    if (s->dllOk) s->dllOurs = file_has_marker(s->dllPath, kDllMarker);

    /* exe 版本指纹 */
    wsprintfW(exe, L"%s\\Game\\Binaries\\Win64\\AceCombat8.exe", s->gameRoot);
    n = read_head(exe, head, (int)sizeof(head));
    if (n > 300) {
        DWORD pe = *(DWORD*)(head + 0x3C);
        if (pe + 24 + 60 < (DWORD)n && *(DWORD*)(head + pe) == 0x00004550u) {
            s->ts = *(DWORD*)(head + pe + 8);
            s->so = *(DWORD*)(head + pe + 24 + 56);
            s->fpRead = TRUE;
            s->fpOk = (s->ts == EXPECT_TIMESTAMP && s->so == EXPECT_SIZEIMAGE);
        }
    }

    /* 上次启动日志 */
    wsprintfW(s->logPath, L"%s\\Game\\Binaries\\Win64\\%s", s->gameRoot, LOG_NAME);
    s->logOk = fexists(s->logPath);
    if (s->logOk) s->logHit = file_has_marker(s->logPath, "PakCtor");

    (void)iniPath;
}

/* 把日志最后若干行转成宽字符，塞进 out */
static void log_tail_w(const wchar_t* logPath, wchar_t* out, int outCap, int maxLines)
{
    static char buf[64 * 1024];
    int n, start, i, cnt = 0;
    out[0] = 0;
    n = read_head(logPath, buf, (int)sizeof(buf));
    if (n <= 0) { lstrcpynW(out, L"(尚无运行记录)", outCap); return; }

    /* 找倒数 maxLines 个换行的位置 */
    start = 0;
    for (i = n - 1; i >= 0; --i) {
        if (buf[i] == '\n') {
            if (++cnt > maxLines) { start = i + 1; break; }
        }
    }
    if (cnt <= maxLines) start = 0;

    /* 逐行转换（\r\n -> \r\n） */
    {
        int w = 0, lineS = start;
        for (i = start; i <= n; ++i) {
            if (i == n || buf[i] == '\n') {
                int lineLen = i - lineS;
                wchar_t tmp[1024];
                if (lineLen > 0 && buf[i > 0 ? i - 1 : 0] == '\r') lineLen--;
                if (lineLen > 1000) lineLen = 1000;
                if (lineLen > 0 || 1) {
                    u8_to_w(buf + lineS, lineLen > 0 ? lineLen : 0, tmp, 1024);
                    if (w + (int)wcslen(tmp) + 3 >= outCap) break;
                    lstrcpynW(out + w, tmp, outCap - w);
                    w += (int)wcslen(out + w);
                    if (w + 2 < outCap) { out[w++] = L'\r'; out[w++] = L'\n'; out[w] = 0; }
                }
                lineS = i + 1;
            }
        }
    }
    if (!out[0]) lstrcpynW(out, L"(日志为空)", outCap);
}

/* --------------------------- 启用 / 停用 -------------------------------- */

typedef struct {
    int done, failed;
    wchar_t msg[4096];
} OpResult;

static void op_append(OpResult* r, const wchar_t* fmt, ...)
{
    va_list ap;
    wchar_t tmp[1024];
    int w, cap = (int)(sizeof(r->msg) / sizeof(r->msg[0]));
    va_start(ap, fmt);
    wvsprintfW(tmp, fmt, ap);
    va_end(ap);
    w = (int)wcslen(r->msg);
    if (w + (int)wcslen(tmp) + 3 >= cap) return;
    lstrcpynW(r->msg + w, tmp, cap - w);
    w = (int)wcslen(r->msg);
    r->msg[w] = L'\r'; r->msg[w + 1] = L'\n'; r->msg[w + 2] = 0;
}

/* 逐块比对两个文件是否完全一致（同时先比大小，快的先失败） */
static BOOL files_identical(const wchar_t* a, const wchar_t* b)
{
    static unsigned char ba[64 * 1024], bb[64 * 1024];
    HANDLE ha, hb;
    BOOL same = TRUE;

    if (fsize(a) != fsize(b)) return FALSE;
    ha = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    hb = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (ha == INVALID_HANDLE_VALUE || hb == INVALID_HANDLE_VALUE) {
        if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
        if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
        return FALSE;
    }
    for (;;) {
        DWORD na = 0, nb = 0, wa = 0, wb = 0;
        ReadFile(ha, ba, sizeof(ba), &na, NULL);
        ReadFile(hb, bb, sizeof(bb), &nb, NULL);
        if (na != nb) { same = FALSE; break; }
        if (na == 0) break;
        if (memcmp(ba, bb, na) != 0) { same = FALSE; break; }
        (void)wa; (void)wb;
    }
    CloseHandle(ha);
    CloseHandle(hb);
    return same;
}

/* 把一个 payload 文件装到目标；目标已有且不是同一个（或非我们的）先备份 */
static BOOL install_one(const wchar_t* src, const wchar_t* dst,
                        const wchar_t* backupDir, OpResult* r, const wchar_t* label)
{
    if (!fexists(src)) {
        op_append(r, L"  %s：payload 中缺少该文件（%s）", label, src);
        ++r->failed;
        return FALSE;
    }
    if (fexists(dst) && files_identical(src, dst)) {
        op_append(r, L"  %s：已是最新", label);
        ++r->done;
        return TRUE;
    }
    if (fexists(dst)) {
        wchar_t bk[MAX_PATH * 2], bp[MAX_PATH * 2];
        const wchar_t* leaf = wcsrchr(dst, L'\\');
        leaf = leaf ? leaf + 1 : dst;
        if (!dexists(backupDir)) CreateDirectoryW(backupDir, NULL);
        wsprintfW(bp, L"%s\\%s", backupDir, leaf);
        wsprintfW(bk, L"%s.old", bp);
        if (fexists(bp)) { DeleteFileW(bk); MoveFileW(bp, bk); }
        if (MoveFileW(dst, bp)) op_append(r, L"  %s：原文件已备份到 %s", label, bp);
        else                    op_append(r, L"  %s：备份失败，将直接覆盖", label);
    }
    if (CopyFileW(src, dst, FALSE)) {
        op_append(r, L"  %s：安装完成（%s）", label, dst);
        ++r->done;
        return TRUE;
    }
    op_append(r, L"  %s：安装失败（错误码 %u）", label, (unsigned)GetLastError());
    ++r->failed;
    return FALSE;
}

static void do_enable(const wchar_t* gameRoot, OpResult* r)
{
    wchar_t srcPak[MAX_PATH * 2], srcDll[MAX_PATH * 2];
    wchar_t dstPak[MAX_PATH * 2], dstDll[MAX_PATH * 2], bk[MAX_PATH * 2];
    ZeroMemory(r, sizeof(*r));
    if (!game_ok(gameRoot)) { op_append(r, L"游戏目录不对：%s", gameRoot); ++r->failed; return; }

    wsprintfW(srcPak, L"%s\\%s", g_payloadDir, PAK_NAME);
    wsprintfW(srcDll, L"%s\\%s", g_payloadDir, DLL_NAME);
    wsprintfW(dstPak, L"%s\\Game\\Content\\Paks\\%s", gameRoot, PAK_NAME);
    wsprintfW(dstDll, L"%s\\Game\\Binaries\\Win64\\%s", gameRoot, DLL_NAME);
    wsprintfW(bk,     L"%s\\_backup", g_exeDir);

    op_append(r, L"启用汉化");
    install_one(srcPak, dstPak, bk, r, L"译文包");
    install_one(srcDll, dstDll, bk, r, L"代理 DLL");
    if (r->failed == 0) op_append(r, L"完成。启动游戏即可。");
}

static void do_disable(const wchar_t* gameRoot, OpResult* r)
{
    wchar_t dstPak[MAX_PATH * 2], dstDll[MAX_PATH * 2], bk[MAX_PATH * 2], bp[MAX_PATH * 2];
    ZeroMemory(r, sizeof(*r));
    if (!game_ok(gameRoot)) { op_append(r, L"游戏目录不对：%s", gameRoot); ++r->failed; return; }

    wsprintfW(dstPak, L"%s\\Game\\Content\\Paks\\%s", gameRoot, PAK_NAME);
    wsprintfW(dstDll, L"%s\\Game\\Binaries\\Win64\\%s", gameRoot, DLL_NAME);
    wsprintfW(bk,     L"%s\\_backup", g_exeDir);
    op_append(r, L"停用汉化");

    if (!fexists(dstDll)) {
        op_append(r, L"  代理 DLL：未安装，跳过");
    } else {
        /* 不是我们的 DLL → 先备份再删，别让人家的东西凭空消失 */
        if (!file_has_marker(dstDll, kDllMarker)) {
            if (!dexists(bk)) CreateDirectoryW(bk, NULL);
            wsprintfW(bp, L"%s\\%s.notours", bk, DLL_NAME);
            if (MoveFileW(dstDll, bp)) op_append(r, L"  代理 DLL：并非本工具的版本，已备份到 %s", bp);
            else                       op_append(r, L"  代理 DLL：备份失败，未删除");
        }
        if (fexists(dstDll)) {
            if (DeleteFileW(dstDll) && !fexists(dstDll)) op_append(r, L"  代理 DLL：已移除");
            else { op_append(r, L"  代理 DLL：移除失败（错误码 %u）", (unsigned)GetLastError()); ++r->failed; }
        }
    }

    if (!fexists(dstPak)) {
        op_append(r, L"  译文包：未安装，跳过");
    } else {
        if (DeleteFileW(dstPak) && !fexists(dstPak)) op_append(r, L"  译文包：已移除");
        else { op_append(r, L"  译文包：移除失败（错误码 %u）", (unsigned)GetLastError()); ++r->failed; }
    }

    if (r->failed == 0) op_append(r, L"完成。游戏已恢复原版（官方文件未被改动）。");
}

/* --------------------------- 启动游戏 ----------------------------------- */

static BOOL do_launch(const wchar_t* gameRoot, OpResult* r)
{
    wchar_t url[128];
    HINSTANCE hi;
    ZeroMemory(r, sizeof(*r));
    (void)gameRoot;
    /* 实测：走 Steam 直启（不带任何参数/环境变量）也能加载汉化 */
    wsprintfW(url, L"steam://rungameid/%s", STEAM_APPID);
    hi = ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hi <= 32) {
        op_append(r, L"无法唤起 Steam（代码 %d）", (int)(INT_PTR)hi);
        op_append(r, L"请确认已安装 Steam，或手动启动游戏。");
        ++r->failed;
        return FALSE;
    }
    op_append(r, L"已通过 Steam 启动 ACE COMBAT 8。");
    return TRUE;
}

/* --------------------------- 命令行模式 --------------------------------- */

static HANDLE g_out = INVALID_HANDLE_VALUE;

static void out_init(void)
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_out == INVALID_HANDLE_VALUE || g_out == NULL ||
        g_out == (HANDLE)(intptr_t)-1) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
    }
}

static void out_line(const wchar_t* s)
{
    char buf[4096];
    int n;
    DWORD w;
    if (g_out == INVALID_HANDLE_VALUE || g_out == NULL) return;
    n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, (int)sizeof(buf), NULL, NULL);
    if (n > 1) WriteFile(g_out, buf, (DWORD)(n - 1), &w, NULL);
    WriteFile(g_out, "\r\n", 2, &w, NULL);
}

static void status_text(const Status* s, wchar_t* out, int cap)
{
    wchar_t num[32];
    out[0] = 0;

#define ADD(...) do { wchar_t _t[1024]; wsprintfW(_t, __VA_ARGS__); \
        if ((int)wcslen(out) + (int)wcslen(_t) + 2 < cap) lstrcatW(out, _t); } while (0)

    if (!s->haveGame) {
        ADD(L"未找到游戏目录\r\n");
        ADD(L"请把「游戏目录」指向 ...\\steamapps\\common\\ACE COMBAT 8\r\n");
        return;
    }
    ADD(L"游戏目录   %s\r\n", s->gameRoot);
    ADD(L"────────────────────────────────────────────────\r\n");

    if (!s->pakOk) {
        ADD(L"汉化包     未安装\r\n");
    } else {
        commafy(s->pakSize, num);
        if (s->pakTime[0])
            ADD(L"汉化包     已安装   %s 字节   构建于 %s\r\n", num, s->pakTime);
        else
            ADD(L"汉化包     已安装   %s 字节\r\n", num);
    }

    commafy(s->dllSize, num);
    if (!s->dllOk)       ADD(L"代理 DLL   未安装\r\n");
    else if (s->dllOurs) ADD(L"代理 DLL   已安装   %s 字节\r\n", num);
    else                 ADD(L"代理 DLL   版本不符   %s 字节（不是本工具的 DLL）\r\n", num);

    if (!s->fpRead)      ADD(L"游戏版本   无法读取\r\n");
    else if (s->fpOk)    ADD(L"游戏版本   已适配\r\n");
    else                 ADD(L"游戏版本   未适配（游戏已更新，需要新版代理 DLL）\r\n");

    if (!s->logOk)       ADD(L"上次运行   尚无记录\r\n");
    else if (s->logHit)  ADD(L"上次运行   加载成功\r\n");
    else                 ADD(L"上次运行   未加载（详见下方日志）\r\n");

    ADD(L"────────────────────────────────────────────────\r\n");
    if (s->pakOk && s->dllOk && s->dllOurs && s->fpOk)
        ADD(L"状态       可用\r\n");
    else
        ADD(L"状态       未就绪 —— 请点「启用汉化」\r\n");
#undef ADD
}

static int status_ready(const Status* s)
{
    return s->haveGame && s->pakOk && s->dllOk && s->dllOurs && s->fpOk;
}

static void cli_usage(void)
{
    out_line(L"ACE COMBAT 8 · 简体汉化管理器 " APP_VER);
    out_line(L"");
    out_line(L"用法：");
    out_line(L"  AC8汉化管理器.exe            打开图形界面");
    out_line(L"  AC8汉化管理器.exe status     打印状态（就绪时退出码 0）");
    out_line(L"  AC8汉化管理器.exe enable     启用汉化");
    out_line(L"  AC8汉化管理器.exe disable    停用汉化（恢复原版）");
    out_line(L"  AC8汉化管理器.exe launch     启动游戏");
    out_line(L"  AC8汉化管理器.exe help       显示本帮助");
}

/* --------------------------- 图形界面 ----------------------------------- */

static wchar_t g_iniPath[MAX_PATH];
static wchar_t g_gameRoot[MAX_PATH];
static Status  g_st;

static void ui_set_text(HWND h, const wchar_t* s) { SetWindowTextW(h, s); }

static void ui_refresh(HWND hwnd)
{
    wchar_t buf[2048], txt[MAX_PATH];
    GetWindowTextW(g_hPath, txt, MAX_PATH);
    lstrcpynW(g_gameRoot, txt, MAX_PATH);

    lstrcpynW(g_st.gameRoot, g_gameRoot, MAX_PATH);
    probe_status(&g_st, g_gameRoot, g_iniPath);

    status_text(&g_st, buf, 2048);
    ui_set_text(g_hStatus, buf);

    if (g_st.haveGame) {
        wchar_t tail[8192];
        log_tail_w(g_st.logPath, tail, 8192, 12);
        ui_set_text(g_hLog, tail);
    } else {
        ui_set_text(g_hLog, L"(未找到游戏目录)");
    }

    if (g_st.haveGame) {
        WritePrivateProfileStringW(L"ac8cn", L"GameDir", g_gameRoot, g_iniPath);
    }
    (void)hwnd;
}

static void ui_op_result(HWND hwnd, const OpResult* r)
{
    int rc = MessageBoxW(hwnd, r->msg, APP_TITLE,
                         r->failed ? MB_ICONWARNING | MB_OK
                                   : MB_ICONINFORMATION | MB_OK);
    (void)rc;
    ui_refresh(hwnd);
}

static HWND mk_btn(HWND p, const wchar_t* text, int id, int x, int y, int w, int h)
{
    HWND c = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        S(x), S(y), S(w), S(h), p, (HMENU)(INT_PTR)id, g_hInst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static HWND mk_static(HWND p, const wchar_t* text, int id, int x, int y, int w, int h)
{
    HWND c = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
        S(x), S(y), S(w), S(h), p, (HMENU)(INT_PTR)id, g_hInst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static HWND mk_edit(HWND p, int id, int x, int y, int w, int h, DWORD extra, BOOL readonly_)
{
    DWORD st = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_LEFT | extra;
    HWND c;
    if (extra & ES_MULTILINE) st |= WS_VSCROLL | ES_AUTOVSCROLL;
    if (readonly_) st |= ES_READONLY;
    c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", st,
                        S(x), S(y), S(w), S(h), p, (HMENU)(INT_PTR)id, g_hInst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void ui_build(HWND hwnd)
{
    HWND h;
    int CW = 780, CH = 560;

    mk_static(hwnd, L"游戏目录：", 0, 16, 20, 80, 22);

    g_hPath = mk_edit(hwnd, IDC_PATH, 100, 16, 780 - 16 - 16 - 84 - 6, 22, ES_AUTOHSCROLL, FALSE);
    mk_btn(hwnd, L"浏览…", IDC_BROWSE, 780 - 16 - 84, 18, 84, 24);

    mk_static(hwnd, L"运行状态", 0, 16, 54, 200, 22);
    g_hStatus = mk_edit(hwnd, IDC_STATUS, 16, 76, 748, 148, ES_MULTILINE, TRUE);

    mk_static(hwnd, L"上次运行日志（最近 12 行）", 0, 16, 232, 500, 22);
    g_hLog = mk_edit(hwnd, IDC_LOG, 16, 254, 748, 174, ES_MULTILINE, TRUE);

    mk_btn(hwnd, L"启用汉化", IDC_ENABLE, 16, 442, 110, 34);
    mk_btn(hwnd, L"停用汉化", IDC_DISABLE, 136, 442, 110, 34);
    mk_btn(hwnd, L"启动游戏", IDC_LAUNCH, 256, 442, 110, 34);
    mk_btn(hwnd, L"刷新状态", IDC_REFRESH, 376, 442, 110, 34);
    mk_btn(hwnd, L"打开游戏目录", IDC_OPENFOLDER, 496, 442, 130, 34);

    g_hHint = mk_static(hwnd,
        L"用法：先「启用汉化」，再「启动游戏」。「停用汉化」会移除汉化文件，官方文件不受影响。"
        L"仅限离线单人游玩，请勿用于多人模式。",
        0, 16, 486, 748, 40);

    (void)h; (void)CW; (void)CH;
}

/* ---------------- 首次使用须知（必须停留 3 秒才能确认） ------------------ */

#define IDC_ACK_OK    2001
#define IDT_ACK_TICK  3001

static HWND g_ackWarn = NULL;    /* 红色警告那一条 */
static HWND g_ackBtn  = NULL;
static int  g_ackLeft = 0;
static int  g_ackOk   = 0;       /* 是否点了「我已阅读并同意」 */

/* 只在第一次「启用汉化」时弹；标记存在管理器自己的 ini 里 */
static BOOL ack_done(void)
{
    return GetPrivateProfileIntW(L"ac8cn", L"AckDisclaimer", 0, g_iniPath) != 0;
}

static void ack_set(void)
{
    WritePrivateProfileStringW(L"ac8cn", L"AckDisclaimer", L"1", g_iniPath);
}

static LRESULT CALLBACK AckProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HWND c;
        static const wchar_t kBody[] =
            L"本汉化补丁为非官方作品，仅供个人学习交流使用，禁止用于商业用途。\r\n"
            L"它会修改游戏运行时的内存与资源加载行为，可能违反游戏的用户协议。\r\n"
            L"使用本补丁的一切后果由使用者自行承担；作者不对账号封禁、存档损坏、\r\n"
            L"设备故障等任何损失负责。建议使用前先备份存档。";
        static const wchar_t kWarn[] =
            L"注意：启用汉化期间，严禁启动多人模式游戏！\r\n"
            L"有封号风险 —— 反作弊系统会检测到被修改的游戏进程。";

        c = CreateWindowExW(0, L"STATIC", kBody, WS_CHILD | WS_VISIBLE | SS_LEFT,
                            S(22), S(18), S(478), S(96), hwnd, NULL, g_hInst, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);

        g_ackWarn = CreateWindowExW(0, L"STATIC", kWarn, WS_CHILD | WS_VISIBLE | SS_LEFT,
                            S(22), S(124), S(478), S(52), hwnd, NULL, g_hInst, NULL);
        SendMessageW(g_ackWarn, WM_SETFONT, (WPARAM)g_font, TRUE);

        g_ackBtn = CreateWindowExW(0, L"BUTTON", L"请阅读（3 秒）",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED,
                            S(352), S(192), S(148), S(32), hwnd,
                            (HMENU)(INT_PTR)IDC_ACK_OK, g_hInst, NULL);
        SendMessageW(g_ackBtn, WM_SETFONT, (WPARAM)g_font, TRUE);

        g_ackLeft = 3;
        g_ackOk = 0;
        SetTimer(hwnd, IDT_ACK_TICK, 1000, NULL);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_ACK_TICK && g_ackBtn) {
            if (--g_ackLeft > 0) {
                wchar_t b[48];
                wsprintfW(b, L"请阅读（%d 秒）", g_ackLeft);
                SetWindowTextW(g_ackBtn, b);
            } else {
                KillTimer(hwnd, IDT_ACK_TICK);
                SetWindowTextW(g_ackBtn, L"我已阅读并同意");
                EnableWindow(g_ackBtn, TRUE);
                SetFocus(g_ackBtn);
            }
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ((HWND)lp == g_ackWarn) ? RGB(198, 32, 32)
                                                 : GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_ACK_OK) { g_ackOk = 1; DestroyWindow(hwnd); return 0; }
        return 0;

    case WM_CLOSE:
        if (g_ackLeft > 0) return 0;     /* 3 秒没到，关窗也关不掉 */
        DestroyWindow(hwnd);             /* 撑满 3 秒可以关，但没点确认 */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 返回 TRUE = 用户点了「我已阅读并同意」；FALSE = 未确认 */
static BOOL show_ack(HWND parent)
{
    WNDCLASSEXW wc;
    HWND h;
    MSG msg;
    RECT r, pr;
    int w, hh, cx, cy;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AckProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AC8CNAckBox";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return FALSE;

    r.left = 0; r.top = 0; r.right = S(522); r.bottom = S(242);
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    w = r.right - r.left;
    hh = r.bottom - r.top;

    if (parent && GetWindowRect(parent, &pr)) { cx = pr.left + 70; cy = pr.top + 70; }
    else { cx = CW_USEDEFAULT; cy = CW_USEDEFAULT; }

    h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AC8CNAckBox", L"使用前须知（首次启用）",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        cx, cy, w, hh, parent, NULL, g_hInst, NULL);
    if (!h) return FALSE;

    if (parent) EnableWindow(parent, FALSE);
    while (IsWindow(h) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (parent) { EnableWindow(parent, TRUE); SetActiveWindow(parent); }
    return g_ackOk;
}

static void ui_browse(HWND hwnd)
{
    BROWSEINFOW bi;
    LPCITEMIDLIST pidl;
    wchar_t buf[MAX_PATH];

    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"请选择游戏根目录（含 Game\\Binaries\\Win64\\AceCombat8.exe 的那一层）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, buf)) {
            SetWindowTextW(g_hPath, buf);
            ui_refresh(hwnd);
        }
        CoTaskMemFree((void*)pidl);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        ui_build(hwnd);
        {
            wchar_t root[MAX_PATH];
            if (find_game(g_iniPath, root)) SetWindowTextW(g_hPath, root);
            else SetWindowTextW(g_hPath, g_gameRoot);
        }
        ui_refresh(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE:  ui_browse(hwnd); return 0;
        case IDC_REFRESH: ui_refresh(hwnd); return 0;
        case IDC_OPENFOLDER: {
            wchar_t t[MAX_PATH];
            GetWindowTextW(g_hPath, t, MAX_PATH);
            if (dexists(t)) ShellExecuteW(NULL, L"open", t, NULL, NULL, SW_SHOWNORMAL);
            else MessageBoxW(hwnd, L"游戏目录不存在。", APP_TITLE, MB_ICONWARNING | MB_OK);
            return 0;
        }
        case IDC_ENABLE: {
            OpResult r; wchar_t t[MAX_PATH];
            GetWindowTextW(g_hPath, t, MAX_PATH);
            /* 首次启用：先看须知（必须停留 3 秒才能确认） */
            if (!ack_done()) {
                if (!show_ack(hwnd)) return 0;      /* 没确认就什么都不做 */
                ack_set();
            }
            do_enable(t, &r);
            ui_op_result(hwnd, &r);
            return 0;
        }
        case IDC_DISABLE: {
            OpResult r; wchar_t t[MAX_PATH];
            GetWindowTextW(g_hPath, t, MAX_PATH);
            if (MessageBoxW(hwnd, L"确定停用汉化？\n\n将从游戏目录移除译文包与代理 DLL。"
                                  L"官方文件不受影响。", APP_TITLE,
                            MB_ICONQUESTION | MB_YESNO) != IDYES) return 0;
            do_disable(t, &r);
            ui_op_result(hwnd, &r);
            return 0;
        }
        case IDC_LAUNCH: {
            wchar_t t[MAX_PATH];
            OpResult r;
            GetWindowTextW(g_hPath, t, MAX_PATH);
            if (!do_launch(t, &r)) ui_op_result(hwnd, &r);
            else {
                SetWindowTextW(g_hHint, L"已通过 Steam 启动。进入标题画面即表示汉化已加载。");
            }
            return 0;
        }
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int run_gui(void)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    RECT rc;
    int w, h;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AC8CNManager";
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    {
        HDC dc = GetDC(NULL);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
        if (g_dpi < 96) g_dpi = 96;
    }
    g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Microsoft YaHei UI");
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    rc.left = 0; rc.top = 0; rc.right = S(780); rc.bottom = S(540);
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
                       FALSE, 0);
    w = rc.right - rc.left; h = rc.bottom - rc.top;

    hwnd = CreateWindowExW(0, L"AC8CNManager", APP_TITLE " " APP_VER,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h, NULL, NULL, g_hInst, NULL);
    if (!hwnd) return 1;
    g_hMain = hwnd;
    UpdateWindow(hwnd);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* ------------------------------- 入口 ----------------------------------- */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    int argc = 0;
    wchar_t** argv;
    const wchar_t* mode = L"";

    (void)hPrev; (void)cmd; (void)show;
    g_hInst = hInst;
    get_exe_dir(g_exeDir);
    wsprintfW(g_payloadDir, L"%s\\payload", g_exeDir);
    wsprintfW(g_iniPath, L"%s\\AC8汉化管理器.ini", g_exeDir);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1 && argv) mode = argv[1];

    if (mode[0]) {
        OpResult r;
        Status st;
        wchar_t root[MAX_PATH] = L"", out[4096];
        InitCommonControls();
        find_game(g_iniPath, root);
        if (!game_ok(root)) {
            GetPrivateProfileStringW(L"ac8cn", L"GameDir", L"", root, MAX_PATH, g_iniPath);
        }

        if (lstrcmpiW(mode, L"status") == 0 || lstrcmpiW(mode, L"-s") == 0) {
            probe_status(&st, root, g_iniPath);
            status_text(&st, out, 4096);
            out_init();
            {
                wchar_t* p = out;
                while (*p) {
                    wchar_t* nl = wcschr(p, L'\r');
                    if (nl) *nl = 0;
                    out_line(p);
                    if (nl) p = nl + 1;
                    else break;
                    while (*p == L'\n') ++p;
                }
            }
            if (st.haveGame) {
                wchar_t tail[8192];
                out_line(L"");
                out_line(L"--- 上次启动日志（尾部）---");
                log_tail_w(st.logPath, tail, 8192, 12);
                {
                    wchar_t* p = tail;
                    while (*p) {
                        wchar_t* nl = wcschr(p, L'\r');
                        if (nl) *nl = 0;
                        out_line(p);
                        if (nl) p = nl + 1; else break;
                        while (*p == L'\n') ++p;
                    }
                }
            }
            return status_ready(&st) ? 0 : 1;
        }
        if (lstrcmpiW(mode, L"enable") == 0) {
            do_enable(root, &r);
            out_init(); out_line(r.msg);
            return r.failed ? 1 : 0;
        }
        if (lstrcmpiW(mode, L"disable") == 0) {
            do_disable(root, &r);
            out_init(); out_line(r.msg);
            return r.failed ? 1 : 0;
        }
        if (lstrcmpiW(mode, L"launch") == 0) {
            do_launch(root, &r);
            out_init(); out_line(r.msg);
            return r.failed ? 1 : 0;
        }
        if (lstrcmpiW(mode, L"help") == 0 || lstrcmpiW(mode, L"-h") == 0
            || lstrcmpiW(mode, L"/?") == 0) {
            out_init(); cli_usage();
            return 0;
        }
        out_init();
        out_line(L"未知参数，看看 help：");
        cli_usage();
        return 2;
    }

    InitCommonControls();
    return run_gui();
}
