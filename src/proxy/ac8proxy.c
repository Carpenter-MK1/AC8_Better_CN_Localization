/* =============================================================================
 *  AC8 简中汉化 · 自研代理 DLL v2（dinput8.dll）
 *
 *  v2 与 v1 的区别（v1 未生效的原因）：
 *    v1 只把「容器是否需要签名」那个 35 字节特征码的 je(74) 改成 jmp(EB)。
 *    实测证明那条分支**只管 IoStore(.utoc)**，legacy `.pak` 根本不走它。
 *
 *    v2 改为挂钩 **FPakFile 构造函数入口**（RVA 0x27F6C80）：
 *      原逻辑：ctor(this, lower, path, bSigned, bLoadIndex)
 *      v3：basename 命中 pakchunk0-Windows_<任意>_P.pak（通配）时，把 bSigned 强制成 false，
 *          游戏因而跳过该容器的签名要求 —— 未签名的 legacy pak 被正常挂载。
 *      官方容器（pakchunk0-Windows.pak / pakchunk0optional-Windows.pak …）没有 "_P.pak" 尾巴，
 *      不会命中，完全不受影响。
 *      ★ 用通配而不是写死包名，是为了让「我们的汉化包」与「第三方 mod 包」
 *        （例如泰语 mod 的 pakchunk0-Windows_999_P.pak）**能同时加载**。
 *
 *    这一段机制来自泰语 mod 作者公开的 dinput8_proxy.cpp（同类钩子），
 *    但本文件是自研实现：**不依赖 MinHook、不依赖任何第三方二进制、不依赖 C 运行时**，
 *    仅用 kernel32 + 一段自写的汇编 thunk（见 ac8hook.s）。
 *
 *  为什么用 dinput8 这个名字：exe 静态导入 DINPUT8.dll，
 *  所以这个文件放在 Game\Binaries\Win64\ 会被 Windows 在**进程初始化阶段**加载，
 *  早于任何引擎代码，来得及在所有容器挂载之前装好钩子。
 * ============================================================================= */

#include "win32_min.h"

/* ★ 识别标记：管理器靠它确认「这是本工具的代理 DLL」。
 * 这是一处**接口约定** —— 改它必须同步改 gui/main.c 里的 kDllMarker，
 * 否则管理器会把我们的 DLL 报成「版本不符」。
 * （2026-10-11 就踩过一次：改日志文案把旧标记 "AC8CN dinput8" 一起改没了。） */
#define AC8CN_DLL_TAG "AC8CN-PROXY-DINPUT8-V3"

/* ------------------------- win32_min.h 未声明的 API ---------------------- */

IMPORT HANDLE WINAPI CreateThread(void* sec, size_t stack,
                                 unsigned long (WINAPI* fn)(void*), void* arg,
                                 DWORD flags, DWORD* tid);
IMPORT void*  WINAPI LoadLibraryW(const wchar_t* name);
IMPORT void*  WINAPI GetProcAddress(void* mod, const char* name);
IMPORT unsigned int WINAPI GetSystemDirectoryW(wchar_t* buf, unsigned int cap);

#define EXPORT __declspec(dllexport)

/* ------------------------------ 锚点常量 -------------------------------- */

#define RVA_CTOR   0x27F6C80u        /* FPakFile 构造函数入口 */
#define RVA_STORE  0x27F6E8Eu        /* ctor 内 mov [r14+0x210], sil */
#define CTOR_PATCH_LEN 15            /* 前 3 条指令 = 3 × 5 字节 */

#define EXPECT_TIMESTAMP 0x6AA0E27Du
#define EXPECT_SIZEIMAGE 0x2195A000u

/* 构造函数前 64 字节（唯一命中；泰语源码同一份，本机实测一致） */
static const unsigned char kCtorGuard[64] = {
    0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x6c,0x24,0x18,
    0x48,0x89,0x74,0x24,0x20, 0x57,
    0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
    0x48,0x83,0xec,0x30,
    0x48,0x8d,0x05,0xe5,0x7a,0xf1,0x08, 0x4c,0x8b,0xea,
    0x48,0x89,0x01, 0x4c,0x8b,0xf1,
    0x48,0x8d,0x05,0x65,0x7a,0xf1,0x08,
    0x33,0xff, 0x48,0x89,0x41,0x10,
    0x49,0x8b,0xd0, 0x89,0x79,0x08, 0x41
};
static const unsigned char kStoreGuard[7] = {
    0x41,0x88,0xb6,0x10,0x02,0x00,0x00
};

/* 需要放行的容器名「规则」（basename、大小写不敏感）：
 *     pakchunk0-Windows_<任意>_P.pak
 * 用通配而不是写死单个包名 —— 我们的汉化包与第三方 mod 包（泰语 mod 用 _999_P）
 * 才能同时被放行、同时加载。 */
static const wchar_t kPakPrefix[] = L"pakchunk0-windows_";
static const wchar_t kPakSuffix[] = L"_p.pak";
#define PAK_SUFFIX_LEN 6

/* 由 ac8hook.s 提供 */
extern void   ac8_thunk(void);
extern void*  ac8_resume;        /* = ctor + 15，安装时填入 */

/* ------------------------------ 日志 ------------------------------------ */

static HANDLE g_log = (HANDLE)(intptr_t)-1;

static void log_open(void)
{
    wchar_t path[1024];
    DWORD n = GetModuleFileNameW((HANDLE)0, path, 1024);
    if (n == 0 || n >= 1010) return;
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
    const wchar_t* tail = L"AC8CNDinput.log";
    int i = 0;
    while (tail[i] && n + i < 1020) { path[n + i] = tail[i]; ++i; }
    path[n + i] = 0;
    g_log = CreateFileW(path, 0x40000000u, 1u, (void*)0, 2u, 0x80u, (HANDLE)0);
}

static void log_a(const char* s)
{
    if (g_log == (HANDLE)(intptr_t)-1) return;
    DWORD put = 0, len = 0;
    while (s[len]) ++len;
    if (len) WriteFile(g_log, s, len, &put, (void*)0);
}

static const char* hex64(unsigned long long v, char* out)
{
    static const char* d = "0123456789ABCDEF";
    out[16] = 0;
    for (int i = 15; i >= 0; --i) { out[i] = d[v & 0xF]; v >>= 4; }
    return out;
}

static void log_line(const char* a, const char* b, const char* c)
{
    log_a("[AC8CN] ");
    if (a) log_a(a);
    if (b) log_a(b);
    if (c) log_a(c);
    log_a("\r\n");
}

static void log_flush(void)
{
    if (g_log != (HANDLE)(intptr_t)-1) FlushFileBuffers(g_log);
}

/* ------------------------------ 小工具 ---------------------------------- */

static int mem_eq(const unsigned char* a, const unsigned char* b, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

static wchar_t lower_w(wchar_t c)
{
    if (c >= L'A' && c <= L'Z') return (wchar_t)(c + 32);
    return c;
}

/* ------------------------- 判定：是不是 mod 补丁包 ----------------------- */

/* basename 是否形如 pakchunk0-Windows_<任意>_P.pak（大小写不敏感，中间至少 1 个字符）。
   官方容器 pakchunk0-Windows.pak 没有 "_P.pak" 尾巴，不会误命中。 */
static int is_mod_pak(const wchar_t* path)
{
    const wchar_t* base = path;
    const wchar_t* p;
    unsigned n, i;

    for (p = path; *p; ++p)
        if (*p == L'/' || *p == L'\\') base = p + 1;

    for (i = 0; kPakPrefix[i]; ++i)
        if (lower_w(base[i]) != kPakPrefix[i]) return 0;

    n = 0;
    for (p = base; *p; ++p) ++n;
    if (n < i + PAK_SUFFIX_LEN + 1) return 0;      /* 前缀 + 中间≥1字 + 后缀 */

    p = base + n - PAK_SUFFIX_LEN;
    for (i = 0; i < PAK_SUFFIX_LEN; ++i)
        if (lower_w(p[i]) != kPakSuffix[i]) return 0;
    return 1;
}

/* 被 ac8hook.s 的 thunk 调用（MS x64 ABI）。
 * 返回要用的 bSigned：命中 mod 补丁包 → 0；否则原样透传。 */
unsigned char ac8_decide(const wchar_t* path, unsigned char bSigned)
{
    if (!path) return bSigned;

    if (is_mod_pak(path)) {
        /* 末尾这个 ASCII 标记是给管理器认的判据（接口约定），改文案时别删 */
        log_line("已放行补丁包（跳过签名校验） [PAK-RELEASED-OK]", 0, 0);
        log_flush();
        return 0;
    }
    return bSigned;
}

/* --------------------------- 安装构造函数钩子 ---------------------------- */

static int g_installed = 0;

static int install_ctor_hook(void)
{
    char num[20];
    if (g_installed) return 1;

    unsigned char* base = (unsigned char*)GetModuleHandleW((const wchar_t*)0);
    if (!base) { log_line("取模块基址失败", 0, 0); return 0; }

    /* --- PE 校验 --- */
    unsigned int pe = *(unsigned int*)(base + 0x3C);
    if (*(unsigned int*)(base + pe) != 0x00004550u) {
        log_line("镜像格式异常", 0, 0); return 0;
    }
    unsigned int ts = *(unsigned int*)(base + pe + 8);
    unsigned int so = *(unsigned int*)(base + pe + 24 + 56);
    if (ts != EXPECT_TIMESTAMP || so != EXPECT_SIZEIMAGE) {
        log_line("游戏版本不受支持（文件指纹不符）", 0, 0);
        log_line("    ts=0x", hex64((unsigned long long)ts, num), 0);
        log_line("    so=0x", hex64((unsigned long long)so, num), 0);
        return 0;
    }

    /* --- 代码指纹 --- */
    unsigned char* ctor = base + RVA_CTOR;
    if (!mem_eq(ctor, kCtorGuard, sizeof(kCtorGuard))) {
        log_line("挂载校验点未找到（代码可能尚未解密）", 0, 0);
        return 0;
    }
    if (!mem_eq(base + RVA_STORE, kStoreGuard, sizeof(kStoreGuard))) {
        log_line("校验点指纹不符", 0, 0);
        return 0;
    }
    log_line("挂载校验点已就位 @0x",
             hex64((unsigned long long)(uintptr_t)ctor, num), 0);

    /* --- 先填 trampoline 的续跑地址，再打补丁 --- */
    ac8_resume = (void*)(ctor + CTOR_PATCH_LEN);

    unsigned char patch[CTOR_PATCH_LEN];
    patch[0] = 0xFF;                       /* jmp qword ptr [rip+0] */
    patch[1] = 0x25;
    patch[2] = 0x00; patch[3] = 0x00; patch[4] = 0x00; patch[5] = 0x00;
    unsigned long long tp = (unsigned long long)(uintptr_t)&ac8_thunk;
    for (int i = 0; i < 8; ++i)
        patch[6 + i] = (unsigned char)((tp >> (8 * i)) & 0xFF);
    patch[14] = 0x90;                      /* 补齐到 15 字节 */

    DWORD old = 0;
    if (!VirtualProtect(ctor, CTOR_PATCH_LEN, 0x40u, &old)) {
        log_line("内存保护设置失败", 0, 0); return 0;
    }
    for (unsigned i = 0; i < CTOR_PATCH_LEN; ++i) ctor[i] = patch[i];
    DWORD tmp = 0;
    VirtualProtect(ctor, CTOR_PATCH_LEN, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), ctor, CTOR_PATCH_LEN);

    g_installed = 1;
    log_line("挂载校验已建立 @0x", hex64((unsigned long long)(uintptr_t)&ac8_thunk, num), 0);
    return 1;
}

/* --------------------------- 轮询线程（兜底） --------------------------- */

static unsigned long WINAPI poll_worker(void* arg)
{
    char num[20];
    (void)arg;
    for (int i = 0; i < 3000; ++i) {           /* 3000 × 20ms = 60 秒 */
        if (install_ctor_hook()) {
            log_line("挂载校验已建立", 0, 0);
            log_flush();
            return 0;
        }
        Sleep(20);
    }
    log_line("挂载校验建立失败 —— 游戏可能已更新，需要新版代理", 0, 0);
    log_flush();
    return 1;
}

/* --------------------- DirectInput8Create 转发 --------------------------- */

typedef long (WINAPI* PFN_DI8CREATE)(HANDLE hinst, DWORD ver, const void* riid,
                                     void** out, void* outer);
static void*  g_real_mod = 0;
static PFN_DI8CREATE g_real_create = 0;

static void resolve_real(void)
{
    wchar_t path[512];
    unsigned int n = GetSystemDirectoryW(path, 500);
    if (!n || n > 480) return;
    const wchar_t* tail = L"\\dinput8.dll";
    int i = 0;
    while (tail[i]) { path[n + i] = tail[i]; ++i; }
    path[n + i] = 0;
    g_real_mod = LoadLibraryW(path);
    if (g_real_mod)
        g_real_create = (PFN_DI8CREATE)GetProcAddress(g_real_mod, "DirectInput8Create");
}

EXPORT long WINAPI DirectInput8Create(HANDLE hinst, DWORD ver, const void* riid,
                                      void** out, void* outer)
{
    if (!g_real_create) {
        resolve_real();
        log_line(g_real_create ? "输入转发已就绪"
                               : "输入转发失败（找不到系统 dinput8.dll）", 0, 0);
    }
    if (!g_real_create) return (long)0x80004001L;   /* E_NOTIMPL */
    return g_real_create(hinst, ver, riid, out, outer);
}

EXPORT long WINAPI DllGetClassObject(const void* a, const void* b, void** c)
{ (void)a; (void)b; (void)c; return (long)0x80004005L; }   /* E_FAIL */
EXPORT long WINAPI DllCanUnloadNow(void) { return 1; }     /* S_FALSE */
EXPORT long WINAPI DllRegisterServer(void) { return (long)0x80040154L; }
EXPORT long WINAPI DllUnregisterServer(void) { return (long)0x80040154L; }

/* ------------------------------ 入口 ------------------------------------ */

BOOL WINAPI DllMain(HANDLE module, DWORD reason, void* reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        log_open();
        log_line("ACE COMBAT 8 简中汉化 · 代理模块已加载 [" AC8CN_DLL_TAG "]", 0, 0);

        if (install_ctor_hook()) {
            log_flush();
        } else {
            log_line("游戏仍在加载，后台将自动重试", 0, 0);
            log_flush();
            DWORD tid = 0;
            CreateThread(0, 0, poll_worker, 0, 0, &tid);
        }
    }
    return 1;
}
