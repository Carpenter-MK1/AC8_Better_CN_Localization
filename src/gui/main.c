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
 *    ⑥ 检查更新：先问 GitHub API 要最新 release 的 tag（活接口，不受 CDN 缓存影响），
 *       再按该 tag 取 dist/version.json（tag 内容不可变）→ 数值比对版本 → 下载并校验
 *       （size + sha256）新的「汉化包 + 注入包」；启动时自动查一次（静默，只有发现新版才提示）
 *
 *  也支持命令行（脚本/自动化友好）：
 *    AC8汉化管理器.exe status | enable | disable | launch | check | update | help
 *    status 退出码：0 = 完全就绪，1 = 未装好或有问题
 * ============================================================================= */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdarg.h>
#include <string.h>

/* ------------------------------- 常量 ----------------------------------- */

#define APP_TITLE    L"ACE COMBAT 8 · 简体汉化管理器"
#define APP_VER      L"1.4"

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
#define IDC_UPDATE     1025

#define WM_APP_UPDDONE    (WM_APP + 1)
#define WM_APP_APPLYDONE  (WM_APP + 2)
#define IDT_AUTOCHECK     3002

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

    wchar_t payloadVer[64];     /* payload\version.json 里的资源版本 */

    BOOL fpRead;  DWORD ts, so;  BOOL fpOk;

    wchar_t logPath[MAX_PATH * 2];
    BOOL logOk;   BOOL logHit;
} Status;

static void upd_local_ver(wchar_t* out, int cap);   /* 实现在下方「更新模块」 */

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

    upd_local_ver(s->payloadVer, 64);

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

/* 把带 \r\n 的多行文本逐行输出 */
static void out_multiline(const wchar_t* s)
{
    wchar_t buf[8192];
    const wchar_t* p;
    lstrcpynW(buf, s, 8192);
    p = buf;
    while (*p) {
        wchar_t* nl = wcschr(p, L'\r');
        if (nl) *nl = 0;
        out_line(p);
        if (!nl) break;
        p = nl + 1;
        while (*p == L'\n') ++p;
    }
}

static void out_line2(const wchar_t* fmt, ...)
{
    wchar_t t[2048];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfW(t, fmt, ap);
    va_end(ap);
    out_line(t);
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

    if (s->payloadVer[0]) ADD(L"资源版本   %s\r\n", s->payloadVer);
    else                  ADD(L"资源版本   未知（payload 里缺 version.json）\r\n");

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
    out_line(L"  AC8汉化管理器.exe check      只检查有没有新版（0=已最新，3=有新版）");
    out_line(L"  AC8汉化管理器.exe update     检查并下载更新汉化包与注入包");
    out_line(L"  AC8汉化管理器.exe help       显示本帮助");
}

/* ======================= 更新：汉化包 + 注入包 ========================== */
/*
 * 清单：仓库里的 dist/version.json
 *       含 version / tag / released_at / notes / files[].name+size+sha256
 *       本地就是 payload\version.json，读它的 version 当"当前版本"。
 *
 * 取清单：raw.githubusercontent.com/<o>/<r>/main/dist/version.json
 *         退路 gcore.jsdelivr.net / cdn.jsdelivr.net（jsDelivr 的 gh 通道）
 *         再退 api.github.com/repos/<o>/<r>/releases/latest（只有版本号，不能下载）
 * 下载  ：raw.../<tag>/dist/<文件>，退路同上（用 tag 锁定，避免 main 漂移）
 * 校验  ：size + sha256，全部通过才动 payload\
 * 落地  ：先全下到 payload\_tmp\ 并逐个校验 → 再替换；中途失败不动任何现有文件
 * 回滚  ：**不做**（老大 2026-10-11 明确）
 */

#define UPD_USERW  L"Carpenter-MK1"
#define UPD_REPOW  L"AC8_Better_CN_Localization"
#define UPD_MANIFW L"version.json"
#define UPD_WAIT   9000
#define UPD_MAXF   4
#define UPD_NSRC   3

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS
#define WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS 2
#endif

typedef struct {
    char  name[64];
    unsigned long long size;
    char  sha[80];
} UpdFile;

typedef struct {
    char      localVer[64];      /* 本地 payload\version.json 的 version */
    char      ver[64];           /* 远程 version */
    char      notes[2048];       /* 远程更新说明 */
    wchar_t   tag[80];           /* 下载用的 git tag，如 v2026.10.11 */
    BOOL      haveRemote;
    int       nf;                /* files[] 条数；0 = 走 API 退路，拿不到文件信息 */
    UpdFile   f[UPD_MAXF];
    char      raw[8192];         /* 远程清单原文，落地时写进 payload\version.json */
    int       rawLen;
} UpdInfo;

static void werr(wchar_t* out, int cap, const wchar_t* fmt, ...)
{
    va_list ap;
    if (!out || cap <= 0) return;
    va_start(ap, fmt);
    wvsprintfW(out, fmt, ap);
    va_end(ap);
}

/* ------------------------------ HTTP GET -------------------------------- */

/* 一次 GET。hFile 有效 → 写入文件；否则写进 mem（memCap 含结尾 0）。 */
static BOOL http_get(const wchar_t* url, char* mem, int memCap, int* memLen,
                     HANDLE hFile, wchar_t* errMsg, int errCap)
{
    URL_COMPONENTS uc;
    wchar_t host[256], path[4096];
    HINTERNET hs = NULL, hc = NULL, hr = NULL;
    BOOL ok = FALSE;
    DWORD status = 0, sz = 0;
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;

    if (errMsg && errCap) errMsg[0] = 0;
    if (mem && memCap > 0) { mem[0] = 0; if (memLen) *memLen = 0; }

    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 4096;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { werr(errMsg, errCap, L"地址无法解析"); return FALSE; }

    hs = WinHttpOpen(L"AC8CNManager/" APP_VER, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs)
        hs = WinHttpOpen(L"AC8CNManager/" APP_VER, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { werr(errMsg, errCap, L"网络组件初始化失败"); return FALSE; }

    WinHttpSetTimeouts(hs, UPD_WAIT, UPD_WAIT, UPD_WAIT, UPD_WAIT * 3);

    hc = WinHttpConnect(hs, host, uc.nPort, 0);
    if (!hc) { werr(errMsg, errCap, L"连不上 %s", host); goto done; }

    hr = WinHttpOpenRequest(hc, L"GET", path, NULL, WINHTTP_NO_REFERER,
                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                            (uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                            | WINHTTP_FLAG_REFRESH);
    if (!hr) { werr(errMsg, errCap, L"无法建立请求"); goto done; }

    WinHttpSetOption(hr, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    WinHttpAddRequestHeaders(hr, L"Accept: */*", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0)) {
        werr(errMsg, errCap, L"发送请求失败（%u）", (unsigned)GetLastError()); goto done;
    }
    if (!WinHttpReceiveResponse(hr, NULL)) {
        werr(errMsg, errCap, L"接收响应失败（%u）", (unsigned)GetLastError()); goto done;
    }

    sz = sizeof(status);
    if (!WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)) {
        werr(errMsg, errCap, L"读不到响应状态"); goto done;
    }
    if (status != 200) { werr(errMsg, errCap, L"服务器返回 HTTP %u", (unsigned)status); goto done; }

    {
        static char chunk[32 * 1024];
        for (;;) {
            DWORD avail = 0, got = 0, want;
            if (!WinHttpQueryDataAvailable(hr, &avail)) { werr(errMsg, errCap, L"读取数据失败"); goto done; }
            if (!avail) break;
            want = avail > (DWORD)sizeof(chunk) ? (DWORD)sizeof(chunk) : avail;
            if (!WinHttpReadData(hr, chunk, want, &got)) { werr(errMsg, errCap, L"读取数据失败"); goto done; }
            if (!got) break;
            if (hFile != INVALID_HANDLE_VALUE && hFile != NULL) {
                DWORD w = 0;
                if (!WriteFile(hFile, chunk, got, &w, NULL) || w != got) {
                    werr(errMsg, errCap, L"写文件失败（%u）", (unsigned)GetLastError()); goto done;
                }
            } else if (mem && memLen) {
                int room = memCap - 1 - *memLen;
                int n = (int)got > room ? room : (int)got;
                if (n > 0) { memcpy(mem + *memLen, chunk, (size_t)n); *memLen += n; mem[*memLen] = 0; }
            }
        }
    }
    ok = TRUE;

done:
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    if (hs) WinHttpCloseHandle(hs);
    return ok;
}

/* ------------------------------ 迷你 JSON ------------------------------- */

/* 反转义：\n \r \t \" \\ \/  ；\uXXXX 粗略跳过 */
static void j_unesc(const char* s, int len, char* out, int cap)
{
    int i = 0, o = 0;
    for (i = 0; i < len && o < cap - 1; ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < len) {
            char e = s[++i];
            if (e == 'n') c = '\n';
            else if (e == 'r') c = '\r';
            else if (e == 't') c = '\t';
            else if (e == 'u') { if (i + 4 < len) i += 4; c = '?'; }
            else c = e;
        }
        out[o++] = c;
    }
    out[o] = 0;
}

/* 找 "key" 后面跟 : 的位置，返回值的起点（跳过空白），-1 表示没有 */
static int j_find(const char* s, int len, const char* key, int* isStr)
{
    char pat[64];
    int pl, i;
    wsprintfA(pat, "\"%s\"", key);
    pl = (int)strlen(pat);
    if (len < pl) return -1;
    for (i = 0; i + pl <= len; ++i) {
        int k;
        if (memcmp(s + i, pat, (size_t)pl) != 0) continue;
        k = i + pl;
        while (k < len && (s[k] == ' ' || s[k] == '\t' || s[k] == '\n' || s[k] == '\r')) ++k;
        if (k >= len || s[k] != ':') continue;
        ++k;
        while (k < len && (s[k] == ' ' || s[k] == '\t' || s[k] == '\n' || s[k] == '\r')) ++k;
        if (k >= len) return -1;
        *isStr = (s[k] == '"');
        return k;
    }
    return -1;
}

static BOOL jget_s(const char* s, int len, const char* key, char* out, int cap)
{
    int isStr = 0, k = j_find(s, len, key, &isStr), e;
    if (k < 0 || !isStr) { out[0] = 0; return FALSE; }
    ++k;
    e = k;
    while (e < len && s[e] != '"') {
        if (s[e] == '\\') ++e;
        ++e;
    }
    if (e > len) e = len;
    j_unesc(s + k, e - k, out, cap);
    return TRUE;
}

static BOOL jget_u(const char* s, int len, const char* key, unsigned long long* out)
{
    int isStr = 0, k = j_find(s, len, key, &isStr);
    unsigned long long v = 0;
    if (k < 0 || isStr) return FALSE;
    while (k < len && s[k] >= '0' && s[k] <= '9') { v = v * 10 + (unsigned)(s[k] - '0'); ++k; }
    *out = v;
    return TRUE;
}

/* ------------------------------ SHA-256 --------------------------------- */

static BOOL sha256_file(const wchar_t* path, char* hex /*>=65*/)
{
    static unsigned char buf[64 * 1024];
    HANDLE h;
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    BOOL ok = FALSE;

    hex[0] = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) goto done;
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) goto done;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, (DWORD)sizeof(buf), &got, NULL)) goto done;
        if (!got) break;
        if (!CryptHashData(hash, buf, got, 0)) goto done;
    }
    {
        BYTE d[32];
        DWORD n = (DWORD)sizeof(d), i;
        if (!CryptGetHashParam(hash, HP_HASHVAL, d, &n, 0)) goto done;
        for (i = 0; i < n; ++i) wsprintfA(hex + i * 2, "%02x", d[i]);
        hex[n * 2] = 0;
    }
    ok = TRUE;

done:
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(h);
    return ok;
}

/* ------------------------------ 版本读写 -------------------------------- */

/* 本地版本：payload\version.json 的 version 字段。实现见前向声明。 */
static void upd_local_ver(wchar_t* out, int cap)
{
    static char buf[8192];
    wchar_t p[MAX_PATH * 2];
    int n;
    char v[64];
    out[0] = 0;
    wsprintfW(p, L"%s\\%s", g_payloadDir, UPD_MANIFW);
    n = read_head(p, buf, (int)sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    if (jget_s(buf, n, "version", v, 64)) u8_to_w(v, -1, out, cap);
}

static void upd_local_read(UpdInfo* u)
{
    static char buf[8192];
    wchar_t p[MAX_PATH * 2];
    int n;
    wsprintfW(p, L"%s\\%s", g_payloadDir, UPD_MANIFW);
    n = read_head(p, buf, (int)sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    jget_s(buf, n, "version", u->localVer, 64);
}

/* 解析 files[] 数组 */
static void upd_parse_files(const char* js, int len, UpdInfo* u)
{
    int isStr = 0, k;
    u->nf = 0;
    k = j_find(js, len, "files", &isStr);
    if (k < 0) return;
    while (k < len && u->nf < UPD_MAXF) {
        int a, b;
        while (k < len && js[k] != '{') {
            if (js[k] == ']') return;
            ++k;
        }
        if (k >= len) return;
        a = k;
        b = k;
        while (b < len && js[b] != '}') ++b;
        if (b >= len) return;
        {
            UpdFile* f = &u->f[u->nf];
            if (jget_s(js + a, b - a + 1, "name", f->name, 64) && f->name[0]) {
                if (!jget_u(js + a, b - a + 1, "size", &f->size)) f->size = 0;
                if (!jget_s(js + a, b - a + 1, "sha256", f->sha, 80)) f->sha[0] = 0;
                ++u->nf;
            }
        }
        k = b + 1;
    }
}

/* 从一份清单 JSON 里取出版本等字段 */
static BOOL upd_parse_manifest(const char* js, int len, UpdInfo* u)
{
    char tag[80];
    if (!jget_s(js, len, "version", u->ver, 64) || !u->ver[0]) return FALSE;
    if (!jget_s(js, len, "notes", u->notes, 2048)) u->notes[0] = 0;
    if (!jget_s(js, len, "tag", tag, 80) || !tag[0])
        wsprintfA(tag, "v%s", u->ver);
    u8_to_w(tag, -1, u->tag, 80);
    upd_parse_files(js, len, u);
    if (len > (int)sizeof(u->raw) - 1) len = (int)sizeof(u->raw) - 1;
    memcpy(u->raw, js, (size_t)len);
    u->raw[u->rawLen = len] = 0;
    return TRUE;
}

/* ------------------------------ 取远程清单 ------------------------------ */
/* 逐段数值比较版本号：a>b → 1，a<b → -1，相等 → 0。
 * 例：2026.10.11.4 > 2026.10.11.3 > 2026.10.11。任一段不是纯数字则退回字符串比较。 */
static int ver_cmp(const char* a, const char* b)
{
    const char* pa = a;
    const char* pb = b;
    for (;;) {
        unsigned long va = 0, vb = 0;
        int na = 0, nb = 0, c;
        while (*pa >= '0' && *pa <= '9') { va = va * 10 + (unsigned long)(*pa - '0'); ++pa; ++na; }
        while (*pb >= '0' && *pb <= '9') { vb = vb * 10 + (unsigned long)(*pb - '0'); ++pb; ++nb; }
        if (!na || !nb) {
            c = lstrcmpA(a, b);
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        if (va != vb) return (va > vb) ? 1 : -1;
        if (*pa == '.' && *pb == '.') { ++pa; ++pb; continue; }
        if (*pa == '.') return 1;                 /* a 段数更多 → a 更新 */
        if (*pb == '.') return -1;
        if (!*pa && !*pb) return 0;
        c = lstrcmpA(pa, pb);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
}

/* 从指定 tag 的 dist/version.json 取清单。
 * ★ 一定要按 tag 取：tag 指向的提交内容不可变，不会被 CDN 缓存坑到。 */
static BOOL upd_fetch_at_tag(const wchar_t* tag, UpdInfo* u)
{
    static char js[64 * 1024];
    static const wchar_t* kFmt[UPD_NSRC] = {
        L"https://raw.githubusercontent.com/" UPD_USERW L"/" UPD_REPOW L"/%s/dist/" UPD_MANIFW,
        L"https://gcore.jsdelivr.net/gh/" UPD_USERW L"/" UPD_REPOW L"@%s/dist/" UPD_MANIFW,
        L"https://cdn.jsdelivr.net/gh/" UPD_USERW L"/" UPD_REPOW L"@%s/dist/" UPD_MANIFW,
    };
    int i, n;
    wchar_t url[1024], err[256];
    for (i = 0; i < UPD_NSRC; ++i) {
        wsprintfW(url, kFmt[i], tag);
        n = 0;
        if (!http_get(url, js, (int)sizeof(js), &n, INVALID_HANDLE_VALUE, err, 256) || n <= 0) continue;
        if (upd_parse_manifest(js, n, u)) return TRUE;
    }
    return FALSE;
}

/* 从 main 分支的 dist/version.json 取清单（raw 有 3~5 分钟 CDN 缓存，只作退路） */
static BOOL upd_fetch_at_main(UpdInfo* u, OpResult* log)
{
    static char js[64 * 1024];
    static const wchar_t* kMan[UPD_NSRC] = {
        L"https://raw.githubusercontent.com/" UPD_USERW L"/" UPD_REPOW L"/main/dist/" UPD_MANIFW,
        L"https://gcore.jsdelivr.net/gh/" UPD_USERW L"/" UPD_REPOW L"@main/dist/" UPD_MANIFW,
        L"https://cdn.jsdelivr.net/gh/" UPD_USERW L"/" UPD_REPOW L"@main/dist/" UPD_MANIFW,
    };
    int i, n;
    wchar_t err[256], tmp[300];
    for (i = 0; i < UPD_NSRC; ++i) {
        n = 0;
        if (!http_get(kMan[i], js, (int)sizeof(js), &n, INVALID_HANDLE_VALUE, err, 256) || n <= 0) {
            werr(tmp, 300, L"%s", err);
            op_append(log, L"  main 清单源 %d 失败：%s", i + 1, tmp);
            continue;
        }
        if (upd_parse_manifest(js, n, u)) {
            op_append(log, L"  main 清单源 %d：%s（注意可能有几分钟缓存）", i + 1, u->tag);
            return TRUE;
        }
    }
    return FALSE;
}

/* 取远程最新版本。
 * ① 先问 GitHub API 要「最新 release 的 tag」—— 这是活接口，不受 CDN 缓存影响；
 * ② 再按那个 tag 取清单（tag 内容不可变，所以绝不会读到旧的）；
 * ③ API 不通才退回读 main 分支的清单（可能陈旧几分钟）。 */
static BOOL upd_fetch_remote(UpdInfo* u, OpResult* log)
{
    static char js[64 * 1024];
    int n = 0;
    wchar_t url[512], err[256], tmp[300];
    char tag[80];

    wsprintfW(url, L"https://api.github.com/repos/%s/%s/releases/latest", UPD_USERW, UPD_REPOW);
    if (http_get(url, js, (int)sizeof(js), &n, INVALID_HANDLE_VALUE, err, 256) && n > 0) {
        if (jget_s(js, n, "tag_name", tag, 80) && tag[0]) {
            wchar_t tagW[80];
            u8_to_w(tag, -1, tagW, 80);
            op_append(log, L"  最新 release：%s（GitHub API）", tagW);
            if (upd_fetch_at_tag(tagW, u)) {
                op_append(log, L"  清单取自 %s：%d 个文件", u->tag, u->nf);
                u->haveRemote = TRUE;
                return TRUE;
            }
            {
                const char* q = tag;
                if (*q == 'v' || *q == 'V') ++q;
                lstrcpynA(u->ver, q, 64);
                lstrcpynW(u->tag, tagW, 80);
                u->nf = 0;
                op_append(log, L"  ⚠ 该 tag 上取不到清单，只能提示版本、无法自动下载");
                u->haveRemote = TRUE;
                return TRUE;
            }
        }
        werr(err, 256, L"API 响应里没有 tag_name");
    }
    werr(tmp, 300, L"%s", err);
    op_append(log, L"  GitHub API 不可用（%s），退回读 main 清单", tmp);

    if (upd_fetch_at_main(u, log)) {
        u->haveRemote = TRUE;
        return TRUE;
    }
    op_append(log, L"  所有更新源都不可用");
    return FALSE;
}

/* 返回 0=检查失败  1=已最新  2=线上有新版  3=本地比线上新 */
static int upd_check(UpdInfo* u, OpResult* log)
{
    int c;
    ZeroMemory(u, sizeof(*u));
    ZeroMemory(log, sizeof(*log));
    upd_local_read(u);
    if (!upd_fetch_remote(u, log)) return 0;
    if (!u->localVer[0]) return 2;              /* 本地没有版本信息 → 当作需要更新 */
    c = ver_cmp(u->ver, u->localVer);
    if (c == 0) return 1;
    if (c > 0)  return 2;
    return 3;
}
/* ------------------------------ 下载并校验 ------------------------------ */

static void upd_build_url(const wchar_t* tag, const wchar_t* name, int src,
                          wchar_t* out, int cap)
{
    static const wchar_t* kFmt[UPD_NSRC] = {
        L"https://raw.githubusercontent.com/" UPD_USERW L"/" UPD_REPOW L"/%s/dist/%s",
        L"https://gcore.jsdelivr.net/gh/" UPD_USERW L"/" UPD_REPOW L"@%s/dist/%s",
        L"https://cdn.jsdelivr.net/gh/" UPD_USERW L"/" UPD_REPOW L"@%s/dist/%s",
    };
    if (src < 0 || src >= UPD_NSRC) src = 0;
    (void)cap;
    wsprintfW(out, kFmt[src], tag, name);
}

/* 下载到 dst 并校验 size + sha256 */
static BOOL upd_get_one(const wchar_t* tag, const UpdFile* f, const wchar_t* dst,
                        wchar_t* err, int errCap)
{
    wchar_t nameW[128], url[1024], e2[256];
    int round, s;
    char got[80];

    u8_to_w(f->name, -1, nameW, 128);
    for (round = 0; round < 2; ++round) {
        for (s = 0; s < UPD_NSRC; ++s) {
            /* tag 拿不到时退到 main（内容对不上会被 sha256 拦下） */
            const wchar_t* tg = (round == 1) ? L"main" : tag;
            HANDLE h;
            upd_build_url(tg, nameW, s, url, 1024);
            DeleteFileW(dst);
            h = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                werr(err, errCap, L"无法写入临时文件（%u）", (unsigned)GetLastError());
                return FALSE;
            }
            if (!http_get(url, NULL, 0, NULL, h, e2, 256)) {
                CloseHandle(h);
                DeleteFileW(dst);
                werr(err, errCap, L"源 %d：%s", s + 1, e2);
                continue;
            }
            CloseHandle(h);
            if (f->size && fsize(dst) != f->size) {
                werr(err, errCap, L"源 %d：大小不符（%u != %u）", s + 1,
                     (unsigned)fsize(dst), (unsigned)f->size);
                DeleteFileW(dst);
                continue;
            }
            if (f->sha[0]) {
                if (!sha256_file(dst, got)) {
                    werr(err, errCap, L"源 %d：无法计算哈希", s + 1);
                    DeleteFileW(dst);
                    continue;
                }
                if (lstrcmpiA(got, f->sha) != 0) {
                    werr(err, errCap, L"源 %d：哈希不符", s + 1);
                    DeleteFileW(dst);
                    continue;
                }
            }
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------ 落地 ------------------------------------ */

/* 清掉下载临时目录 —— 成功或失败都要清，别把半个包留在 payload 里 */
static void upd_clean_tmp(const UpdInfo* u, const wchar_t* tmp)
{
    int i;
    wchar_t nameW[128], s[MAX_PATH * 2];
    for (i = 0; i < u->nf; ++i) {
        u8_to_w(u->f[i].name, -1, nameW, 128);
        wsprintfW(s, L"%s\\%s", tmp, nameW);
        DeleteFileW(s);
    }
    RemoveDirectoryW(tmp);
}

/* 更新 payload\（必要时同步游戏目录）。不做回滚。 */
static BOOL upd_apply(const UpdInfo* u, const wchar_t* gameRoot, OpResult* r)
{
    wchar_t tmp[MAX_PATH * 2], err[256], nameW[128];
    wchar_t verW[64];
    int i;

    if (!u->nf) {
        op_append(r, L"  这份清单里没有文件信息（走的是 GitHub API 退路）。");
        op_append(r, L"  请到项目页手动下载：https://github.com/%s/%s/releases", UPD_USERW, UPD_REPOW);
        ++r->failed;
        return FALSE;
    }

    wsprintfW(tmp, L"%s\\_tmp", g_payloadDir);
    if (!dexists(tmp) && !CreateDirectoryW(tmp, NULL)) {
        op_append(r, L"  无法创建临时目录：%s", tmp);
        ++r->failed;
        return FALSE;
    }

    /* ① 全部下载 + 校验（此阶段不动任何现有文件） */
    for (i = 0; i < u->nf; ++i) {
        wchar_t dst[MAX_PATH * 2];
        wchar_t num[32];
        u8_to_w(u->f[i].name, -1, nameW, 128);
        wsprintfW(dst, L"%s\\%s", tmp, nameW);
        if (!upd_get_one(u->tag, &u->f[i], dst, err, 256)) {
            op_append(r, L"  %s 下载失败：%s", nameW, err);
            ++r->failed;
            upd_clean_tmp(u, tmp);
            return FALSE;
        }
        commafy(fsize(dst), num);
        op_append(r, L"  %s 已下载并校验通过（%s 字节）", nameW, num);
    }

    /* ② 替换 payload\ */
    for (i = 0; i < u->nf; ++i) {
        wchar_t s[MAX_PATH * 2], d[MAX_PATH * 2];
        u8_to_w(u->f[i].name, -1, nameW, 128);
        wsprintfW(s, L"%s\\%s", tmp, nameW);
        wsprintfW(d, L"%s\\%s", g_payloadDir, nameW);
        if (!MoveFileExW(s, d, MOVEFILE_REPLACE_EXISTING)) {
            op_append(r, L"  写入 payload 失败：%s（错误码 %u）", nameW, (unsigned)GetLastError());
            ++r->failed;
        }
    }
    if (r->failed) { upd_clean_tmp(u, tmp); return FALSE; }

    /* ③ 把清单原文写成新的本地版本标记 */
    if (u->rawLen > 0) {
        wchar_t p[MAX_PATH * 2];
        HANDLE h;
        wsprintfW(p, L"%s\\%s", g_payloadDir, UPD_MANIFW);
        h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, u->raw, (DWORD)u->rawLen, &w, NULL);
            CloseHandle(h);
        }
    }

    /* ④ 旧的 version.txt 退役 */
    {
        wchar_t p[MAX_PATH * 2];
        wsprintfW(p, L"%s\\version.txt", g_payloadDir);
        if (fexists(p)) DeleteFileW(p);
    }

    /* ⑤ 汉化已启用 → 同步游戏目录 */
    if (game_ok(gameRoot)) {
        wchar_t dstPak[MAX_PATH * 2], dstDll[MAX_PATH * 2], bk[MAX_PATH * 2];
        wsprintfW(dstPak, L"%s\\Game\\Content\\Paks\\%s", gameRoot, PAK_NAME);
        wsprintfW(dstDll, L"%s\\Game\\Binaries\\Win64\\%s", gameRoot, DLL_NAME);
        wsprintfW(bk, L"%s\\_backup", g_exeDir);
        if (fexists(dstPak) || fexists(dstDll)) {
            wchar_t srcPak[MAX_PATH * 2], srcDll[MAX_PATH * 2];
            wsprintfW(srcPak, L"%s\\%s", g_payloadDir, PAK_NAME);
            wsprintfW(srcDll, L"%s\\%s", g_payloadDir, DLL_NAME);
            op_append(r, L"  汉化已启用 → 同步更新游戏目录");
            install_one(srcPak, dstPak, bk, r, L"译文包");
            install_one(srcDll, dstDll, bk, r, L"代理 DLL");
        } else {
            op_append(r, L"  汉化尚未启用，下次点「启用汉化」即可用上新版");
        }
    }

    /* ⑥ 清理临时目录 */
    upd_clean_tmp(u, tmp);

    u8_to_w(u->ver, -1, verW, 64);
    op_append(r, L"完成，现在是 %s。", verW);
    return r->failed == 0;
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

/* 底部提示：第一行固定用法，第二行显示最近一次更新检查的结果 ——
 * 这样"有没有真的查过"一眼可见，不用猜。 */
static const wchar_t kHintL1[] =
    L"用法：先「启用汉化」，再「启动游戏」。仅限离线单人游玩，请勿用于多人模式。";
static wchar_t g_updLast[320] = L"尚未检查";

static void hint_show(const wchar_t* line2)
{
    wchar_t t[800];
    if (line2) lstrcpynW(g_updLast, line2, 320);
    wsprintfW(t, L"%s\r\n更新检查：%s", kHintL1, g_updLast);
    SetWindowTextW(g_hHint, t);
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
    mk_btn(hwnd, L"检查更新", IDC_UPDATE, 640, 442, 124, 34);

    g_hHint = mk_static(hwnd, L"", 0, 16, 486, 748, 40);
    hint_show(NULL);

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

/* ------------------- 检查更新（后台线程，不卡界面） --------------------- */

static volatile LONG g_updBusy = 0;
static BOOL     g_updAuto = FALSE;      /* 本次是不是启动时的静默检查 */
static int      g_updRc = 0;
static wchar_t  g_updGame[MAX_PATH];
static UpdInfo  g_updInfo;
static OpResult g_updLog;
static OpResult g_updApplyR;

static DWORD WINAPI upd_check_thread(LPVOID p)
{
    (void)p;
    g_updRc = upd_check(&g_updInfo, &g_updLog);
    PostMessageW(g_hMain, WM_APP_UPDDONE, 0, 0);
    return 0;
}

static DWORD WINAPI upd_apply_thread(LPVOID p)
{
    wchar_t vw[64];
    (void)p;
    ZeroMemory(&g_updApplyR, sizeof(g_updApplyR));
    u8_to_w(g_updInfo.ver, -1, vw, 64);
    op_append(&g_updApplyR, L"更新到 %s", vw);
    upd_apply(&g_updInfo, g_updGame, &g_updApplyR);
    PostMessageW(g_hMain, WM_APP_APPLYDONE, 0, 0);
    return 0;
}

/* autoCheck=TRUE 时静默：只有发现新版才弹窗，已最新或检查失败都不打扰 */
static void upd_start_check(BOOL autoCheck)
{
    if (InterlockedCompareExchange(&g_updBusy, 1, 0) != 0) return;
    g_updAuto = autoCheck;
    GetWindowTextW(g_hPath, g_updGame, MAX_PATH);
    if (!autoCheck) hint_show(L"正在检查…");
    CloseHandle(CreateThread(NULL, 0, upd_check_thread, NULL, 0, NULL));
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
        SetTimer(hwnd, IDT_AUTOCHECK, 1200, NULL);   /* 启动 1.2 秒后静默查一次 */
        return 0;

    case WM_TIMER:
        if (wp == IDT_AUTOCHECK) {
            KillTimer(hwnd, IDT_AUTOCHECK);
            upd_start_check(TRUE);
        }
        return 0;

    case WM_APP_UPDDONE: {
        int rc = g_updRc;
        static wchar_t vw[64], lw[64], nw[2560], msg[6144], last[320];
        InterlockedExchange(&g_updBusy, 0);
        u8_to_w(g_updInfo.ver, -1, vw, 64);
        u8_to_w(g_updInfo.localVer, -1, lw, 64);
        u8_to_w(g_updInfo.notes, -1, nw, 2560);

        if (rc == 2) {
            wsprintfW(last, L"发现新版 %s（当前 %s）", vw, lw[0] ? lw : L"未知");
            hint_show(last);
            wsprintfW(msg,
                L"发现新版本：%s      当前版本：%s\r\n\r\n%s\r\n\r\n"
                L"是否现在下载并更新？\r\n"
                L"（更新译文包与代理 DLL；汉化已启用时，游戏目录里的文件会一并更新）",
                vw, lw[0] ? lw : L"未知", nw[0] ? nw : L"本次发布没有写更新说明");
            if (MessageBoxW(hwnd, msg, APP_TITLE, MB_ICONQUESTION | MB_YESNO) == IDYES) {
                if (InterlockedCompareExchange(&g_updBusy, 1, 0) == 0) {
                    hint_show(L"正在下载更新…请稍候");
                    CloseHandle(CreateThread(NULL, 0, upd_apply_thread, NULL, 0, NULL));
                }
            }
        } else if (rc == 1) {
            wsprintfW(last, L"已是最新（本机 %s = 线上 %s）", lw[0] ? lw : L"未知", vw);
            hint_show(last);
            if (!g_updAuto) MessageBoxW(hwnd, last, APP_TITLE, MB_ICONINFORMATION | MB_OK);
        } else if (rc == 3) {
            wsprintfW(last, L"本机比线上新（本机 %s > 线上 %s），无需更新", lw, vw);
            hint_show(last);
            if (!g_updAuto) MessageBoxW(hwnd, last, APP_TITLE, MB_ICONINFORMATION | MB_OK);
        } else {
            hint_show(L"检查失败（连不上更新源）");
            if (!g_updAuto) {
                static wchar_t m2[8192];
                lstrcpynW(m2, L"检查更新失败 —— 连不上更新源。\r\n\r\n", 8192);
                if ((int)wcslen(m2) + (int)wcslen(g_updLog.msg) < 8000) lstrcatW(m2, g_updLog.msg);
                lstrcatW(m2, L"\r\n稍后重试，或到项目页手动下载。");
                MessageBoxW(hwnd, m2, APP_TITLE, MB_ICONWARNING | MB_OK);
            }
        }
        return 0;
    }

    case WM_APP_APPLYDONE:
        InterlockedExchange(&g_updBusy, 0);
        hint_show(NULL);
        ui_op_result(hwnd, &g_updApplyR);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE:  ui_browse(hwnd); return 0;
        case IDC_UPDATE:  upd_start_check(FALSE); return 0;
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
        if (lstrcmpiW(mode, L"check") == 0) {
            UpdInfo u; OpResult lg;
            int rc;
            wchar_t lw[64], rw[64];
            out_init();
            rc = upd_check(&u, &lg);
            out_line(L"--- 更新检查 ---");
            out_multiline(lg.msg);
            if (rc == 0) { out_line(L"结果：检查失败"); return 1; }
            u8_to_w(u.localVer, -1, lw, 64);
            u8_to_w(u.ver, -1, rw, 64);
            out_line2(L"本地版本：%s", lw[0] ? lw : L"（未知）");
            out_line2(L"远程版本：%s", rw);
            if (rc == 1) { out_line(L"结果：已是最新"); return 0; }
            if (rc == 3) { out_line(L"结果：本机比线上新，无需更新"); return 0; }
            out_line(L"结果：有新版本");
            if (u.notes[0]) { wchar_t nw[2048]; out_line(L""); u8_to_w(u.notes, -1, nw, 2048); out_multiline(nw); }
            return 3;
        }
        if (lstrcmpiW(mode, L"update") == 0) {
            UpdInfo u; OpResult lg, r2;
            int rc;
            wchar_t lw[64], rw[64];
            out_init();
            rc = upd_check(&u, &lg);
            out_multiline(lg.msg);
            u8_to_w(u.localVer, -1, lw, 64);
            u8_to_w(u.ver, -1, rw, 64);
            if (rc == 1) { out_line(L"已是最新，无需更新。"); return 0; }
            if (rc == 3) { out_line(L"本机比线上新，无需更新。"); return 0; }
            if (rc == 0) { out_line(L"检查更新失败。"); return 1; }
            out_line2(L"更新：%s → %s", lw[0] ? lw : L"（未知）", rw);
            ZeroMemory(&r2, sizeof(r2));
            {
                wchar_t vw[64];
                u8_to_w(u.ver, -1, vw, 64);
                op_append(&r2, L"更新到 %s", vw);
            }
            upd_apply(&u, root, &r2);
            out_line(L"");
            out_multiline(r2.msg);
            return r2.failed ? 1 : 0;
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
