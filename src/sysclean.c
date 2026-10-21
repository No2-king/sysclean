/* ============================================================
 * SysClean - 进程清理 + 强制卸载工具
 * 目标: Windows 7 SP1 及以上 (32/64 位均可运行)
 * 编译: 见 build_exe.bat (MSVC, 静态链接, 无运行时依赖)
 *
 * 设计原则:
 *   1. 扫描只读, 不改任何东西
 *   2. 卸载前必出计划 + MessageBox 确认
 *   3. 注册表删除前先备份到 backup\ 目录
 *   4. 危险路径(系统目录/盘符根)硬拦截, 不允许删
 *   5. critical process 硬白名单, 绝不终止
 * ============================================================ */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shlobj.h>
#include <restartmanager.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "rstrtmgr.lib")

#define MAX_PROGS       2048
#define MAX_PROCS       4096
#define MAX_THREADS     16384
#define MAX_MODULES     1024
#define MAX_PL          1024
#define LOG_HEIGHT      170
#define TOP_HEIGHT      46

/* 控件 ID */
#define ID_LIST         1001
#define ID_LOG          1002
#define ID_STATUS       1003
#define ID_BTN_SCAN     1010
#define ID_BTN_KILL     1011
#define ID_BTN_UNINST   1012
#define ID_BTN_BACKUP   1013
#define ID_BTN_THREADS  1014
#define ID_BTN_MENU     1015

/* 右键菜单管理窗口 */
#define ID_M_LIST       1301
#define ID_M_INFO       1302
#define ID_M_REFRESH    1310
#define ID_M_TOGGLE     1311
#define ID_M_DELETE     1312
#define ID_M_BACKUP     1313

/* 线程窗口控件 */
#define ID_T_PROC       1101
#define ID_T_THREAD     1102
#define ID_T_INFO       1103
#define ID_T_REFRESH    1110
#define ID_T_REVEAL     1111
#define ID_T_REVEALP    1112
#define ID_T_KILLTHR    1113
#define ID_T_COPY       1114
#define ID_T_CLOSE      1115
#define ID_T_MENU       1200

/* ---------------- 数据结构 ---------------- */

/* 条目来源 */
typedef enum {
    SRC_HKLM = 0,
    SRC_HKCU,
    SRC_APPDATA_LOCAL,
    SRC_APPDATA_ROAMING
} SrcType;

#define MAX_DATADIRS 4

typedef struct {
    WCHAR name[256];
    WCHAR publisher[128];
    WCHAR version[64];
    WCHAR installDir[MAX_PL];
    WCHAR uninstall[MAX_PL];
    WCHAR iconPath[MAX_PL];
    WCHAR regPath[MAX_PL];     /* SOFTWARE\...\Uninstall\<子键>, AppData 来源时为空 */
    WCHAR dataDirs[MAX_DATADIRS][MAX_PL];   /* 关联的 AppData 数据目录 */
    int   dataDirCount;
    DWORD sizeKB;
    SrcType src;
    BOOL  is64;
    BOOL  running;
    HKEY  hive;
    REGSAM view;
} ProgramInfo;

/* 关联的注册表位置 */
#define MAX_REGHITS 24
typedef struct {
    HKEY  root;
    WCHAR path[MAX_PL];
    WCHAR label[48];           /* 卸载项 / 软件项 / 启动项 / 服务 */
    BOOL  isValue;             /* TRUE = 删这个值, FALSE = 删整个键 */
    WCHAR valueName[256];
} RegHit;

typedef struct {
    DWORD pid;
    DWORD ppid;
    DWORD session;
    WCHAR name[MAX_PATH];
    WCHAR path[MAX_PL];
} ProcEntry;

/* ---- 右键菜单项 ---- */

#define MAX_MENU 1024

typedef struct {
    WCHAR scope[32];       /* 文件 / 文件夹 / 文件夹背景 / 驱动器 / 桌面 */
    WCHAR name[256];       /* 菜单显示名 */
    WCHAR verb[128];       /* 注册表键名 */
    WCHAR path[MAX_PL];    /* HKEY_CLASSES_ROOT 下的完整路径 */
    WCHAR clsid[128];      /* shellex 处理器的 CLSID */
    WCHAR cmd[MAX_PL];     /* 命令行 / dll 路径, 用来判断是不是系统项 */
    BOOL  isShellex;       /* TRUE = shellex\ContextMenuHandlers, FALSE = shell\<verb> */
    BOOL  disabled;
    BOOL  system;
} MenuItem;

static MenuItem g_menu[MAX_MENU];
static int      g_menuCount;

/* ---- 进程与线程视图 ---- */

typedef struct {
    DWORD     pid, ppid, session;
    DWORD     threadCount, handleCount;
    SIZE_T    memBytes;
    ULONGLONG cpu100ns;          /* 内核 + 用户, 单位 100ns */
    WCHAR     name[MAX_PATH];
    WCHAR     path[MAX_PL];
} ProcRow;

typedef struct {
    DWORD     tid;
    ULONG_PTR start;             /* 线程起始地址 (Win32StartAddress) */
    int       prio;
    ULONGLONG user100ns, kernel100ns;
    ULONG_PTR modBase, modOffset;
    BOOL      hasMod;
    BOOL      isWow64Owner;      /* 目标进程是 32 位跑在 64 位系统上 */
    WCHAR     module[MAX_PATH];  /* 模块名, 如 kernel32.dll */
    WCHAR     modPath[MAX_PL];   /* 模块完整磁盘路径 */
} ThreadRow;

typedef struct {
    ULONG_PTR base;
    ULONG_PTR size;
    WCHAR     name[MAX_PATH];
    WCHAR     path[MAX_PL];
} ModInfo;

static ProcRow   g_prows[MAX_PROCS];
static int       g_prowCount;
static ThreadRow g_trows[MAX_THREADS];
static int       g_trowCount;
static ModInfo   g_mods[MAX_MODULES];
static int       g_modCount;
static DWORD     g_selPid;
static BOOL      g_isWow64;      /* 本程序是 32 位, 但跑在 64 位系统上 */

static ProgramInfo g_progs[MAX_PROGS];
static int         g_progCount;
static ProcEntry   g_procs[MAX_PROCS];
static int         g_procCount;

static HWND g_hList, g_hLog, g_hStatus, g_hMain;
static HFONT g_hFont;
static BOOL g_busy;
static int  g_delFiles, g_delFail;
static FILE *g_testLog;          /* 命令行测试模式下把日志同时写文件 */
static WCHAR g_selfDir[MAX_PATH];

/* 线程窗口 */
static HWND g_hTwnd, g_hTProc, g_hTThread, g_hTInfo;
static BOOL g_twndOpen;

/* NtQueryInformationThread —— 拿线程起始地址, 官方 API 没有对应函数 */
typedef LONG (NTAPI *PFN_NtQIT)(HANDLE, ULONG, PVOID, ULONG, PULONG);
static PFN_NtQIT g_NtQIT;
#define ThreadQuerySetWin32StartAddress 9

/* ---------------- 前向声明 ---------------- */
static void Log(LPCWSTR fmt, ...);
static void Status(LPCWSTR text);
static BOOL IsProtectedName(LPCWSTR name);
static void SnapshotProcs(void);
static BOOL StartsWithI(LPCWSTR s, LPCWSTR prefix);
static void NormalizePath(LPCWSTR in, LPWSTR out, size_t cch);

/* ============================================================
 * 基础工具
 * ============================================================ */

static void Log(LPCWSTR fmt, ...)
{
    WCHAR buf[2048];
    va_list ap;
    int len;

    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    /* 命令行测试模式下同时落盘 */
    if (g_testLog) {
        fwprintf(g_testLog, L"%s\n", buf);
        fflush(g_testLog);
    }

    if (!g_hLog) return;

    len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

static void Status(LPCWSTR text)
{
    if (g_hStatus) SetWindowTextW(g_hStatus, text);
    UpdateWindow(g_hMain);
}

/* 把相对/普通路径转成 \\?\ 长路径 */
static void ToLongPath(LPCWSTR in, LPWSTR out, size_t cch)
{
    WCHAR norm[MAX_PL];

    NormalizePath(in, norm, _countof(norm));
    if (!norm[0]) { out[0] = 0; return; }

    if (wcsncmp(norm, L"\\\\?\\", 4) == 0) {
        wcsncpy_s(out, cch, norm, _TRUNCATE);
        return;
    }
    if (norm[1] == L':' && norm[2] == L'\\') {
        swprintf_s(out, cch, L"\\\\?\\%s", norm);
    } else {
        wcsncpy_s(out, cch, norm, _TRUNCATE);
    }
}

static void TrimRightSlash(LPWSTR s)
{
    size_t n = wcslen(s);
    while (n > 3 && (s[n-1] == L'\\' || s[n-1] == L'/')) {
        s[--n] = 0;
    }
}

/* 统一路径: 去首尾引号 -> 正斜杠转反斜杠 -> 去尾斜杠
   必须做这一步: \\?\ 前缀不接受正斜杠, 且正斜杠能绕过路径黑名单 */
static void NormalizePath(LPCWSTR in, LPWSTR out, size_t cch)
{
    size_t i, n, w, r, start;

    if (!in) { out[0] = 0; return; }
    wcsncpy_s(out, cch, in, _TRUNCATE);

    n = wcslen(out);
    if (n >= 2 && out[0] == L'"' && out[n - 1] == L'"') {
        memmove(out, out + 1, (n - 1) * sizeof(WCHAR));
        out[n - 2] = 0;
    }
    for (i = 0; out[i]; i++) {
        if (out[i] == L'/') out[i] = L'\\';
    }
    /* 合并连续分隔符, 保留开头的 \\ (UNC / \\?\ 前缀) */
    start = (out[0] == L'\\' && out[1] == L'\\') ? 2 : 0;
    w = start;
    for (r = start; out[r]; r++) {
        if (out[r] == L'\\' && w > 0 && out[w - 1] == L'\\') continue;
        out[w++] = out[r];
    }
    out[w] = 0;
    TrimRightSlash(out);
}

static void SanitizeFileName(LPCWSTR in, LPWSTR out, size_t cch)
{
    size_t i = 0, j = 0;
    for (; in[i] && j + 1 < cch; i++) {
        WCHAR c = in[i];
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' ||
            c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
            c = L'_';
        out[j++] = c;
    }
    out[j] = 0;
    if (j == 0) wcsncpy_s(out, cch, L"unnamed", _TRUNCATE);
}

/* ============================================================
 * 权限
 * ============================================================ */

static BOOL EnablePrivilege(LPCWSTR name)
{
    HANDLE token = NULL;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    BOOL ok = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return FALSE;

    if (LookupPrivilegeValueW(NULL, name, &luid)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    }
    CloseHandle(token);
    return ok;
}

static BOOL IsAdmin(void)
{
    BOOL admin = FALSE;
    PSID sid = NULL;
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };

    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &sid)) {
        CheckTokenMembership(NULL, sid, &admin);
        FreeSid(sid);
    }
    return admin;
}

/* ============================================================
 * 进程快照
 * ============================================================ */

static BOOL IsProtectedName(LPCWSTR name)
{
    static const WCHAR *prot[] = {
        L"system", L"smss", L"csrss", L"wininit", L"winlogon", L"services",
        L"lsass", L"lsaiso", L"svchost", L"registry", L"memory compression",
        L"secure system", L"ngciso",
        L"explorer", L"dwm", L"sihost", L"logonui", L"consent", L"userinit",
        L"conhost", L"ctfmon", L"audiodg", L"fontdrvhost", L"tabtip",
        L"wmiprvse", L"dllhost", L"sppsvc", L"msdtc", L"spoolsv", L"spoolss",
        L"wudfhost", L"unsecapp", L"trustedinstaller", L"tiworker", L"wuauclt",
        L"taskeng", L"taskhost", L"taskhostw",
        L"searchindexer", L"securityhealthservice", L"msmpeng", L"nissrv",
        L"smartscreen",
        L"runtimebroker", L"startmenuexperiencehost", L"shellexperiencehost",
        L"textinputhost", L"applicationframehost", L"searchui",
        L"searchhost", L"shellhost", L"sysclean",
        NULL
    };
    WCHAR base[MAX_PATH];
    int i;

    if (!name || !name[0]) return TRUE;
    wcsncpy_s(base, _countof(base), name, _TRUNCATE);
    {
        WCHAR *dot = wcsrchr(base, L'.');
        if (dot && _wcsicmp(dot, L".exe") == 0) *dot = 0;
    }
    for (i = 0; prot[i]; i++) {
        if (_wcsicmp(base, prot[i]) == 0) return TRUE;
    }
    return FALSE;
}

static void SnapshotProcs(void)
{
    HANDLE snap;
    PROCESSENTRY32W pe;

    g_procCount = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (g_procCount >= MAX_PROCS) break;
            g_procs[g_procCount].pid = pe.th32ProcessID;
            g_procs[g_procCount].ppid = pe.th32ParentProcessID;
            wcsncpy_s(g_procs[g_procCount].name, MAX_PATH, pe.szExeFile, _TRUNCATE);
            g_procs[g_procCount].path[0] = 0;
            g_procs[g_procCount].session = 0xFFFFFFFF;

            {
                DWORD sid = 0;
                if (ProcessIdToSessionId(pe.th32ProcessID, &sid))
                    g_procs[g_procCount].session = sid;
            }
            {
                HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (hp) {
                    DWORD cch = MAX_PL;
                    QueryFullProcessImageNameW(hp, 0, g_procs[g_procCount].path, &cch);
                    CloseHandle(hp);
                }
            }
            g_procCount++;
            pe.dwSize = sizeof(pe);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

/* ============================================================
 * 进程 / 线程枚举 (全部只读)
 * ============================================================ */

static void InitNtQuery(void)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (nt) g_NtQIT = (PFN_NtQIT)(void *)GetProcAddress(nt, "NtQueryInformationThread");
}

static void FormatAddr(ULONG_PTR v, LPWSTR out, size_t cch)
{
#if defined(_WIN64)
    swprintf_s(out, cch, L"0x%016I64X", (unsigned __int64)v);
#else
    swprintf_s(out, cch, L"0x%08lX", (unsigned long)v);
#endif
}

/* 偏移用不补零的写法, 短得多, 一列能放下 */
static void FormatOffset(ULONG_PTR v, LPWSTR out, size_t cch)
{
#if defined(_WIN64)
    swprintf_s(out, cch, L"0x%I64X", (unsigned __int64)v);
#else
    swprintf_s(out, cch, L"0x%lX", (unsigned long)v);
#endif
}

/* 加载指定进程的模块表, 用于把线程起始地址反查成 "哪个 dll + 偏移多少" */
static void LoadModules(DWORD pid)
{
    HANDLE snap;
    MODULEENTRY32W me;

    g_modCount = 0;
    if (pid == 0 || pid == 4) return;

    /* 同时带 SNAPMODULE32: 64 位进程也要能列 WOW64 子进程的模块 */
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return;

    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (g_modCount >= MAX_MODULES) break;
            g_mods[g_modCount].base = (ULONG_PTR)me.modBaseAddr;
            g_mods[g_modCount].size = (ULONG_PTR)me.modBaseSize;
            wcsncpy_s(g_mods[g_modCount].name, MAX_PATH, me.szModule, _TRUNCATE);
            wcsncpy_s(g_mods[g_modCount].path, MAX_PL, me.szExePath, _TRUNCATE);
            g_modCount++;
            me.dwSize = sizeof(me);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

static HANDLE OpenThreadQuery(DWORD tid)
{
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    return h;
}

/* 枚举某进程的全部线程 */
static void LoadThreadsFor(DWORD pid)
{
    HANDLE snap;
    THREADENTRY32 te;

    g_trowCount = 0;
    LoadModules(pid);
    if (pid == 0) return;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            ThreadRow *r;
            HANDLE ht;

            if (te.th32OwnerProcessID != pid) continue;
            if (g_trowCount >= MAX_THREADS) break;

            r = &g_trows[g_trowCount];
            ZeroMemory(r, sizeof(*r));
            r->tid = te.th32ThreadID;
            r->prio = te.tpBasePri;

            ht = OpenThreadQuery(te.th32ThreadID);
            if (ht) {
                FILETIME ct, et, kt, ut;
                ULONG_PTR start = 0;
                ULONG retLen = 0;

                /* 起始地址: 官方 API 没有, 走 ntdll */
                if (g_NtQIT && g_NtQIT(ht, ThreadQuerySetWin32StartAddress,
                                       &start, (ULONG)sizeof(start), &retLen) == 0)
                    r->start = start;

                if (GetThreadTimes(ht, &ct, &et, &kt, &ut)) {
                    r->kernel100ns = ((ULONGLONG)kt.dwHighDateTime << 32) | kt.dwLowDateTime;
                    r->user100ns   = ((ULONGLONG)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
                }
                CloseHandle(ht);
            }

            /* 地址 -> 模块 */
            if (r->start) {
                int m;
                for (m = 0; m < g_modCount; m++) {
                    ULONG_PTR b = g_mods[m].base;
                    ULONG_PTR s = g_mods[m].size ? g_mods[m].size : 0x1000;
                    if (r->start >= b && r->start < b + s) {
                        r->hasMod = TRUE;
                        r->modBase = b;
                        r->modOffset = r->start - b;
                        wcsncpy_s(r->module, MAX_PATH, g_mods[m].name, _TRUNCATE);
                        wcsncpy_s(r->modPath, MAX_PL, g_mods[m].path, _TRUNCATE);
                        break;
                    }
                }
            }
            g_trowCount++;
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static int CompareProcRow(const void *a, const void *b)
{
    const ProcRow *pa = (const ProcRow *)a;
    const ProcRow *pb = (const ProcRow *)b;
    if (pa->threadCount != pb->threadCount)
        return (pa->threadCount < pb->threadCount) ? 1 : -1;   /* 线程多的排前面 */
    return _wcsicmp(pa->name, pb->name);
}

static void LoadProcRows(void)
{
    HANDLE snap;
    PROCESSENTRY32W pe;

    g_prowCount = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcRow *r;
            HANDLE hp;

            if (g_prowCount >= MAX_PROCS) break;
            r = &g_prows[g_prowCount];
            ZeroMemory(r, sizeof(*r));
            r->pid = pe.th32ProcessID;
            r->ppid = pe.th32ParentProcessID;
            r->threadCount = pe.cntThreads;
            wcsncpy_s(r->name, MAX_PATH, pe.szExeFile, _TRUNCATE);
            if (!ProcessIdToSessionId(r->pid, &r->session)) r->session = 0xFFFFFFFF;

            hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, r->pid);
            if (!hp)
                hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                 FALSE, r->pid);
            if (hp) {
                DWORD cch = MAX_PL;
                DWORD hc = 0;
                FILETIME ct, et, kt, ut;
                PROCESS_MEMORY_COUNTERS_EX pmc;

                QueryFullProcessImageNameW(hp, 0, r->path, &cch);
                if (GetProcessTimes(hp, &ct, &et, &kt, &ut)) {
                    r->cpu100ns = (((ULONGLONG)kt.dwHighDateTime << 32) | kt.dwLowDateTime)
                                + (((ULONGLONG)ut.dwHighDateTime << 32) | ut.dwLowDateTime);
                }
                ZeroMemory(&pmc, sizeof(pmc));
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(hp, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc)))
                    r->memBytes = pmc.WorkingSetSize;
                if (GetProcessHandleCount(hp, &hc))
                    r->handleCount = hc;
                CloseHandle(hp);
            }
            g_prowCount++;
            pe.dwSize = sizeof(pe);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    qsort(g_prows, g_prowCount, sizeof(ProcRow), CompareProcRow);
}

/* 在资源管理器里定位到文件 (选中高亮) */
static void RevealInExplorer(LPCWSTR path)
{
    WCHAR args[MAX_PL + 32];
    WCHAR dir[MAX_PL];

    if (!path || !path[0]) {
        MessageBoxW(g_hTwnd, L"该线程没有可定位的模块路径。\n\n"
                             L"常见原因: 目标进程是 64 位而当前是 32 位版本的程序, "
                             L"或该地址属于 JIT 动态生成的代码。",
                    L"无法定位", MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        /* 文件已不在, 退化为打开所在目录 */
        wcsncpy_s(dir, _countof(dir), path, _TRUNCATE);
        PathRemoveFileSpecW(dir);
        ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
        return;
    }

    swprintf_s(args, _countof(args), L"/select,\"%s\"", path);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

static BOOL KillThreadById(DWORD tid)
{
    HANDLE ht = OpenThread(THREAD_TERMINATE, FALSE, tid);
    BOOL ok = FALSE;

    if (!ht) return FALSE;
    ok = TerminateThread(ht, 0);
    CloseHandle(ht);
    return ok;
}

/* ============================================================
 * 注册表扫描 (卸载列表)
 * ============================================================ */

static BOOL ReadStr(HKEY hk, LPCWSTR value, LPWSTR out, DWORD cch)
{
    DWORD type = 0, size = cch * sizeof(WCHAR);
    out[0] = 0;
    if (RegQueryValueExW(hk, value, NULL, &type, (LPBYTE)out, &size) != ERROR_SUCCESS)
        return FALSE;
    if (type != REG_SZ && type != REG_EXPAND_SZ) { out[0] = 0; return FALSE; }
    out[cch - 1] = 0;
    return out[0] != 0;
}

static BOOL IsNoiseEntry(LPCWSTR name)
{
    /* 补丁 / 更新 / 系统组件 / 驱动包 一律跳过 */
    static const WCHAR *noise[] = {
        L"Update for", L"Security Update", L"Hotfix for", L"Service Pack for",
        L"Security Update for", L"Microsoft .NET Framework", L"Visual C++ 20",
        NULL
    };
    int i;
    if (name[0] == L'K' && name[1] == L'B') return TRUE;
    if (wcsstr(name, L"驱动程序包")) return TRUE;
    if (wcsstr(name, L"Driver Package")) return TRUE;
    for (i = 0; noise[i]; i++) {
        if (_wcsnicmp(name, noise[i], wcslen(noise[i])) == 0) return TRUE;
    }
    return FALSE;
}

static void Unquote(LPWSTR s)
{
    size_t n = wcslen(s);
    if (n >= 2 && s[0] == L'"' && s[n - 1] == L'"') {
        memmove(s, s + 1, (n - 1) * sizeof(WCHAR));
        s[n - 2] = 0;
    }
    TrimRightSlash(s);
}

/* 没有 InstallLocation 时, 依次尝试从 UninstallString / DisplayIcon 反推安装目录 */
static void DeriveInstallDir(ProgramInfo *p)
{
    WCHAR tmp[MAX_PL];
    int n = 0, i;

    if (p->installDir[0]) return;

    /* 1. UninstallString 的第一个 token (支持带引号的路径 + 后面跟参数) */
    if (p->uninstall[0]) {
        if (p->uninstall[0] == L'"') {
            for (i = 1; p->uninstall[i] && p->uninstall[i] != L'"' && n < MAX_PL - 1; i++)
                tmp[n++] = p->uninstall[i];
        } else {
            for (i = 0; p->uninstall[i] && p->uninstall[i] != L' ' && n < MAX_PL - 1; i++)
                tmp[n++] = p->uninstall[i];
        }
        tmp[n] = 0;

        if (tmp[1] == L':' && tmp[2] == L'\\' &&
            _wcsicmp(PathFindExtensionW(tmp), L".exe") == 0) {
            PathRemoveFileSpecW(tmp);
            if (wcslen(tmp) >= 10 &&
                !StartsWithI(tmp, L"C:\\Windows") &&
                !StartsWithI(tmp, L"C:\\Program Files\\Common Files")) {
                wcsncpy_s(p->installDir, MAX_PL, tmp, _TRUNCATE);
                return;
            }
        }
    }

    /* 2. DisplayIcon 所在目录 */
    if (p->iconPath[0] && p->iconPath[1] == L':' && p->iconPath[2] == L'\\' &&
        _wcsicmp(PathFindExtensionW(p->iconPath), L".exe") == 0 &&
        !StartsWithI(p->iconPath, L"C:\\Windows") &&
        !StartsWithI(p->iconPath, L"C:\\Program Files\\Common Files")) {
        wcsncpy_s(tmp, _countof(tmp), p->iconPath, _TRUNCATE);
        PathRemoveFileSpecW(tmp);
        if (wcslen(tmp) >= 10)
            wcsncpy_s(p->installDir, MAX_PL, tmp, _TRUNCATE);
    }
}

static BOOL IsDuplicate(LPCWSTR name, LPCWSTR ver)
{
    int i;
    for (i = 0; i < g_progCount; i++) {
        if (_wcsicmp(g_progs[i].name, name) == 0 &&
            _wcsicmp(g_progs[i].version, ver) == 0)
            return TRUE;
    }
    return FALSE;
}

/* 从 DisplayIcon 里抽出 exe 路径: "C:\a\b.exe,0" -> "C:\a\b.exe" */
static void ExtractIconPath(LPCWSTR src, LPWSTR out, DWORD cch)
{
    WCHAR tmp[MAX_PL];
    int n = 0, i;
    out[0] = 0;
    if (!src || !src[0]) return;

    wcsncpy_s(tmp, _countof(tmp), src, _TRUNCATE);
    if (tmp[0] == L'"') {
        for (i = 1; tmp[i] && tmp[i] != L'"' && n < (int)cch - 1; i++)
            out[n++] = tmp[i];
        out[n] = 0;
        return;
    }
    for (i = 0; tmp[i] && n < (int)cch - 1; i++) {
        if (tmp[i] == L',' && (tmp[i+1] == L'0' || tmp[i+1] == L'-' || tmp[i+1] == L' '))
            break;
        out[n++] = tmp[i];
    }
    out[n] = 0;
    TrimRightSlash(out);
}

static void AddProgram(HKEY root, REGSAM view, BOOL is64, LPCWSTR subKey, LPCWSTR leaf)
{
    HKEY hk;
    WCHAR full[MAX_PL];
    ProgramInfo *p;
    DWORD sysComp = 0, sizeVal = 0, sz;
    WCHAR tmp[512];

    if (g_progCount >= MAX_PROGS) return;
    if (swprintf_s(full, MAX_PL, L"%s\\%s", subKey, leaf) < 0) return;

    if (RegOpenKeyExW(root, full, 0, KEY_READ | view, &hk) != ERROR_SUCCESS)
        return;

    p = &g_progs[g_progCount];
    ZeroMemory(p, sizeof(*p));

    if (!ReadStr(hk, L"DisplayName", p->name, _countof(p->name))) {
        RegCloseKey(hk);
        return;
    }
    if (IsNoiseEntry(p->name)) { RegCloseKey(hk); return; }

    /* 系统组件 / 子组件 跳过 */
    sz = sizeof(sysComp);
    if (RegQueryValueExW(hk, L"SystemComponent", NULL, NULL, (LPBYTE)&sysComp, &sz) == ERROR_SUCCESS) {
        if (sysComp == 1) { RegCloseKey(hk); return; }
    }
    if (ReadStr(hk, L"ParentKeyName", tmp, _countof(tmp))) { RegCloseKey(hk); return; }

    ReadStr(hk, L"Publisher", p->publisher, _countof(p->publisher));
    ReadStr(hk, L"DisplayVersion", p->version, _countof(p->version));
    ReadStr(hk, L"InstallLocation", p->installDir, _countof(p->installDir));
    ReadStr(hk, L"UninstallString", p->uninstall, _countof(p->uninstall));
    ReadStr(hk, L"DisplayIcon", tmp, _countof(tmp));
    ExtractIconPath(tmp, p->iconPath, _countof(p->iconPath));

    sz = sizeof(sizeVal);
    if (RegQueryValueExW(hk, L"EstimatedSize", NULL, NULL, (LPBYTE)&sizeVal, &sz) == ERROR_SUCCESS)
        p->sizeKB = sizeVal;

    RegCloseKey(hk);

    Unquote(p->installDir);
    DeriveInstallDir(p);

    if (IsDuplicate(p->name, p->version)) return;

    p->is64 = is64;
    p->hive = root;
    p->view = view;
    p->src = (root == HKEY_LOCAL_MACHINE) ? SRC_HKLM : SRC_HKCU;
    wcsncpy_s(p->regPath, MAX_PL, full, _TRUNCATE);
    g_progCount++;
}

static void ScanUninstallKey(HKEY root, REGSAM view, BOOL is64)
{
    /* 32 位进程读此路径时由 WOW64 自动重定向到 Wow6432Node,
       所以 64KEY / 32KEY 各调一次即可覆盖两个视图, 不要显式再拼 Wow6432Node (会重复) */
    static const WCHAR *path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    HKEY hk;
    DWORD idx = 0;

    if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &hk) != ERROR_SUCCESS)
        return;

    for (;;) {
        WCHAR leaf[512];
        DWORD cch = _countof(leaf);
        if (RegEnumKeyExW(hk, idx++, leaf, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        AddProgram(root, view, is64, path, leaf);
    }
    RegCloseKey(hk);
}

/* ============================================================
 * AppData 扫描
 *   很多程序（Electron / 便携版 / 用户级安装）根本不写卸载注册表项,
 *   只在 %LOCALAPPDATA% 或 %APPDATA% 下留一个目录。这里把它们捞出来。
 * ============================================================ */

/* AppData 下这些是系统目录 / 缓存 / 厂商容器, 不是独立程序, 一律不列 */
static BOOL IsAppDataNoise(LPCWSTR name)
{
    static const WCHAR *noise[] = {
        L"Microsoft", L"Packages", L"Temp", L"Tmp", L"CrashDumps",
        L"ConnectedDevicesPlatform", L"Comms", L"D3DSCache", L"IconCache",
        L"ElevatedDiagnostics", L"VirtualStore", L"Package Cache",
        L"Downloaded Installations", L"Programs", L"Publishers",
        L"NVIDIA", L"Intel", L"AMD", L"Realtek", L"Apple", L"Adobe",
        L"Google", L"Mozilla", L"Tencent", L"Microsoft Corporation",
        L"CEF", L"Steam", L"Battle.net", L"EasyAntiCheat",
        NULL
    };
    int i;

    if (!name || !name[0]) return TRUE;
    if (name[0] == L'.') return TRUE;
    for (i = 0; noise[i]; i++) {
        if (_wcsicmp(name, noise[i]) == 0) return TRUE;
    }
    return FALSE;
}

static BOOL DirHasExe(LPCWSTR dir)
{
    WCHAR pat[MAX_PL];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (swprintf_s(pat, _countof(pat), L"%s\\*.exe", dir) < 0) return FALSE;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    FindClose(h);
    return !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL ExistsSameDir(LPCWSTR dir)
{
    int i;
    for (i = 0; i < g_progCount; i++) {
        if (g_progs[i].installDir[0] && _wcsicmp(g_progs[i].installDir, dir) == 0)
            return TRUE;
    }
    return FALSE;
}

static BOOL ExistsSameName(LPCWSTR name)
{
    int i;
    for (i = 0; i < g_progCount; i++) {
        if (_wcsicmp(g_progs[i].name, name) == 0) return TRUE;
    }
    return FALSE;
}

/* Squirrel / Electron 的更新器临时目录、带版本号的暂存目录, 不是软件本体 */
static BOOL IsAppDataJunkName(LPCWSTR name)
{
    size_t n = wcslen(name);

    if (n > 8 && _wcsicmp(name + n - 8, L"-updater") == 0) return TRUE;
    if (n > 8 && _wcsicmp(name + n - 8, L"_updater") == 0) return TRUE;

    /* 结尾是 -x.y.z / _x.y.z 的版本化目录, 如 copilot-desktop-gh-2.96.0 */
    {
        LPCWSTR p = name + n;
        int dots = 0, digits = 0;
        while (p > name) {
            WCHAR c = *(p - 1);
            if (c >= L'0' && c <= L'9') { digits++; p--; }
            else if (c == L'.') { dots++; p--; }
            else break;
        }
        if (dots == 2 && digits >= 2 && p > name && (*(p - 1) == L'-' || *(p - 1) == L'_'))
            return TRUE;
    }

    /* 结尾是 _1234 这种解压出来的临时目录, 如 TCPView_5279 */
    {
        LPCWSTR p = name + n;
        int digits = 0;
        while (p > name && *(p - 1) >= L'0' && *(p - 1) <= L'9') { digits++; p--; }
        if (digits >= 4 && p > name && *(p - 1) == L'_') return TRUE;
    }
    return FALSE;
}

/* 这个目录已经是某个已扫描程序的数据目录 -> 不再单独当成一个"软件" */
static BOOL IsKnownDataDir(LPCWSTR dir)
{
    int i, d;
    for (i = 0; i < g_progCount; i++) {
        for (d = 0; d < g_progs[i].dataDirCount; d++) {
            if (_wcsicmp(g_progs[i].dataDirs[d], dir) == 0) return TRUE;
        }
    }
    return FALSE;
}

static void AddAppDataProgram(LPCWSTR dir, LPCWSTR name, SrcType src)
{
    ProgramInfo *p;

    if (g_progCount >= MAX_PROGS) return;
    if (!name || !name[0]) return;
    if (IsAppDataNoise(name)) return;
    if (IsAppDataJunkName(name)) return;
    if (ExistsSameDir(dir) || ExistsSameName(name)) return;
    if (IsKnownDataDir(dir)) return;

    p = &g_progs[g_progCount];
    ZeroMemory(p, sizeof(*p));
    wcsncpy_s(p->name, _countof(p->name), name, _TRUNCATE);
    wcsncpy_s(p->installDir, MAX_PL, dir, _TRUNCATE);
    wcsncpy_s(p->publisher, _countof(p->publisher),
              src == SRC_APPDATA_LOCAL ? L"(AppData\\Local)" : L"(AppData\\Roaming)",
              _TRUNCATE);
    p->src = src;
    p->hive = NULL;
    g_progCount++;
}

static void ScanAppDataDir(LPCWSTR rootDir, SrcType src)
{
    WCHAR pat[MAX_PL], sub[MAX_PL];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (!rootDir || !rootDir[0]) return;

    /* %LOCALAPPDATA%\Programs\<X> 是用户级安装的标准位置, 整个子目录都算程序 */
    if (src == SRC_APPDATA_LOCAL) {
        swprintf_s(sub, _countof(sub), L"%s\\Programs", rootDir);
        swprintf_s(pat, _countof(pat), L"%s\\*", sub);
        h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                WCHAR full[MAX_PL];
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == L'.') continue;
                swprintf_s(full, _countof(full), L"%s\\%s", sub, fd.cFileName);
                if (DirHasExe(full))
                    AddAppDataProgram(full, fd.cFileName, src);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    /* 根目录下一层: 只有直接含 .exe 的才算程序, 避免把厂商容器目录当成程序 */
    swprintf_s(pat, _countof(pat), L"%s\\*", rootDir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR full[MAX_PL];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (IsAppDataNoise(fd.cFileName)) continue;
        swprintf_s(full, _countof(full), L"%s\\%s", rootDir, fd.cFileName);
        if (!DirHasExe(full)) continue;
        AddAppDataProgram(full, fd.cFileName, src);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void ScanAppData(void)
{
    WCHAR local[MAX_PATH] = L"", roaming[MAX_PATH] = L"";

    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local) != S_OK)
        local[0] = 0;
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, roaming) != S_OK)
        roaming[0] = 0;

    ScanAppDataDir(local, SRC_APPDATA_LOCAL);
    ScanAppDataDir(roaming, SRC_APPDATA_ROAMING);
}

/* ---------------- 通用辅助 ---------------- */

static BOOL IsUnderDir(LPCWSTR path, LPCWSTR dir)
{
    size_t dl;
    if (!path || !dir || !dir[0]) return FALSE;
    dl = wcslen(dir);
    if (_wcsnicmp(path, dir, dl) != 0) return FALSE;
    return path[dl] == L'\\';
}

static BOOL ContainsI(LPCWSTR hay, LPCWSTR needle)
{
    size_t nl;
    if (!hay || !needle) return FALSE;
    nl = wcslen(needle);
    if (nl == 0) return FALSE;
    for (; *hay; hay++) {
        if (_wcsnicmp(hay, needle, nl) == 0) return TRUE;
    }
    return FALSE;
}

/* 通用容器名: 拿它去 AppData 里找数据目录会误匹配一大片, 必须跳过 */
static BOOL IsGenericDirName(LPCWSTR name)
{
    static const WCHAR *gen[] = {
        L"bin", L"app", L"application", L"client", L"data", L"cache", L"temp",
        L"lib", L"libs", L"resources", L"dist", L"build", L"src", L"tools",
        L"common", L"shared", L"plugins", L"x86", L"x64", L"current", L"latest",
        L"version", L"versions", L"program", L"programs", L"install", L"setup",
        L"core", L"runtime", L"framework", L"assets", L"modules",
        NULL
    };
    int i;
    if (!name || !name[0]) return TRUE;
    for (i = 0; gen[i]; i++) {
        if (_wcsicmp(name, gen[i]) == 0) return TRUE;
    }
    return FALSE;
}

static BOOL RegKeyExists(HKEY root, LPCWSTR path, REGSAM view)
{
    HKEY hk;
    if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &hk) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(hk);
    return TRUE;
}

/* 这些键是共享容器, 删了会连累别的软件 —— 硬拦截 */
static BOOL IsDangerousRegPath(LPCWSTR path)
{
    static const WCHAR *bad[] = {
        L"SOFTWARE", L"SOFTWARE\\Microsoft", L"SOFTWARE\\Microsoft\\Windows",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"SOFTWARE\\Classes", L"SOFTWARE\\Policies", L"SOFTWARE\\Wow6432Node",
        L"SOFTWARE\\Wow6432Node\\Microsoft", L"SYSTEM", L"SYSTEM\\CurrentControlSet",
        L"SYSTEM\\CurrentControlSet\\Services",
        NULL
    };
    int i;

    if (!path || !path[0]) return TRUE;
    for (i = 0; bad[i]; i++) {
        if (_wcsicmp(path, bad[i]) == 0) return TRUE;
    }
    /* 至少要有 SOFTWARE\X 两级, 单段一律拒绝 */
    if (!wcsrchr(path, L'\\')) return TRUE;
    return FALSE;
}

/* 服务列表缓存: 扫描时读一次, 避免每个程序都枚举几百个服务 */
typedef struct {
    WCHAR name[256];
    WCHAR image[MAX_PL];
} SvcEntry;

static SvcEntry g_svcs[1024];
static int      g_svcCount;

static void LoadServices(void)
{
    HKEY hk;
    DWORD idx = 0;

    g_svcCount = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services",
                      0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;

    for (;;) {
        WCHAR name[256];
        DWORD cch = _countof(name);
        HKEY hs;

        if (RegEnumKeyExW(hk, idx++, name, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (g_svcCount >= 1024) break;
        if (RegOpenKeyExW(hk, name, 0, KEY_READ, &hs) != ERROR_SUCCESS) continue;
        if (ReadStr(hs, L"ImagePath", g_svcs[g_svcCount].image, MAX_PL)) {
            wcsncpy_s(g_svcs[g_svcCount].name, 256, name, _TRUNCATE);
            g_svcCount++;
        }
        RegCloseKey(hs);
    }
    RegCloseKey(hk);
}

/* ---------------- 关联数据目录 ---------------- */

static BOOL IsReparsePoint(LPCWSTR dir)
{
    DWORD attr = GetFileAttributesW(dir);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* AppData 下这些是临时区 / 系统兼容性 junction, 当数据目录会出事 */
static BOOL IsDataDirNoise(LPCWSTR name)
{
    static const WCHAR *noise[] = {
        L"Temp", L"Tmp", L"CrashDumps", L"Application Data", L"Local Settings",
        L"Temporary Internet Files", L"History", L"My Documents", L"Start Menu",
        L"Templates", L"Recent", L"SendTo", L"Cookies", L"NetHood", L"PrintHood",
        L"Microsoft", L"Packages", L"ConnectedDevicesPlatform", L"Programs",
        L"ProgramData", L"Desktop", L"Documents", L"Downloads", L"Package Cache",
        NULL
    };
    int i;
    if (!name || !name[0]) return TRUE;
    for (i = 0; noise[i]; i++) {
        if (_wcsicmp(name, noise[i]) == 0) return TRUE;
    }
    return FALSE;
}

static void AddDataDir(ProgramInfo *p, LPCWSTR dir)
{
    int i;
    if (!dir || !dir[0]) return;
    if (p->dataDirCount >= MAX_DATADIRS) return;
    if (p->installDir[0] && _wcsicmp(p->installDir, dir) == 0) return;
    if (IsReparsePoint(dir)) return;          /* junction / 符号链接: 删了会穿透 */
    for (i = 0; i < p->dataDirCount; i++) {
        if (_wcsicmp(p->dataDirs[i], dir) == 0) return;
    }
    wcsncpy_s(p->dataDirs[p->dataDirCount], MAX_PL, dir, _TRUNCATE);
    p->dataDirCount++;
}

/*
 * 在 root 下找该程序的数据目录:
 *   root\<name>
 *   root\<vendor>\<name>   —— vendor 必须和安装目录的父目录同名, 否则不猜
 * 只按名字精确匹配, 不做模糊搜索, 漏掉总比误删强。
 */
static void ProbeDataDirUnder(LPCWSTR root, LPCWSTR vendor, LPCWSTR name, ProgramInfo *p)
{
    WCHAR path[MAX_PL];

    if (!root || !root[0]) return;
    if (IsDataDirNoise(name)) return;

    swprintf_s(path, _countof(path), L"%s\\%s", root, name);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        AddDataDir(p, path);

    if (vendor && vendor[0] && !IsDataDirNoise(vendor)) {
        swprintf_s(path, _countof(path), L"%s\\%s\\%s", root, vendor, name);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            AddDataDir(p, path);
    }
}

static void CollectDataDirs(ProgramInfo *p)
{
    WCHAR base[MAX_PL], name[128], vendor[128], tmp[MAX_PL];
    WCHAR local[MAX_PATH] = L"", roaming[MAX_PATH] = L"";
    int depth;

    p->dataDirCount = 0;
    if (!p->installDir[0]) return;
    /* AppData 来源的条目, installDir 本身就是数据目录, 不用再找 */
    if (p->src == SRC_APPDATA_LOCAL || p->src == SRC_APPDATA_ROAMING) return;

    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local);
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, roaming);

    /* vendor = 安装目录的父目录名, 如 D:\Program Files\Tencent\Weixin -> Tencent */
    vendor[0] = 0;
    wcsncpy_s(tmp, _countof(tmp), p->installDir, _TRUNCATE);
    {
        LPWSTR s;
        PathRemoveFileSpecW(tmp);
        s = wcsrchr(tmp, L'\\');
        if (s && s[1]) wcsncpy_s(vendor, _countof(vendor), s + 1, _TRUNCATE);
    }

    wcsncpy_s(base, _countof(base), p->installDir, _TRUNCATE);
    for (depth = 0; depth < 2; depth++) {
        LPWSTR leaf = wcsrchr(base, L'\\');
        if (!leaf) break;
        wcsncpy_s(name, _countof(name), leaf + 1, _TRUNCATE);

        /* MSI 的 {GUID} 目录名不是程序名, 拿它猜数据目录只会猜错 */
        if (name[0] == L'{') break;

        if (name[0] && !IsGenericDirName(name)) {
            ProbeDataDirUnder(local, vendor, name, p);
            ProbeDataDirUnder(roaming, vendor, name, p);
            /* 拿到具体名字就停, 绝不退到厂商目录 —— 否则 "Tencent" 会被当成
               微信的数据目录, 一删就是全家桶 */
            break;
        }
        *leaf = 0;
    }
}

/* 数据目录只允许在 AppData 之下, 且不能就是 AppData 本身 */
static BOOL IsSafeToDeleteData(LPCWSTR dir)
{
    WCHAR norm[MAX_PL];
    WCHAR local[MAX_PATH] = L"", roaming[MAX_PATH] = L"";

    if (!dir || !dir[0]) return FALSE;
    if (IsReparsePoint(dir)) return FALSE;
    NormalizePath(dir, norm, _countof(norm));
    if (wcslen(norm) < 20) return FALSE;
    if (!(norm[1] == L':' && norm[2] == L'\\')) return FALSE;
    if (wcsstr(norm, L"..")) return FALSE;

    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local);
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, roaming);

    if (local[0] && IsUnderDir(norm, local)) return TRUE;
    if (roaming[0] && IsUnderDir(norm, roaming)) return TRUE;
    return FALSE;
}

/* ---------------- 关联注册表项 ---------------- */

static void AddRegHit(RegHit *hits, int *n, HKEY root, LPCWSTR path, LPCWSTR label,
                      BOOL isValue, LPCWSTR valueName)
{
    int i;

    if (*n >= MAX_REGHITS) return;
    if (!path || !path[0]) return;

    for (i = 0; i < *n; i++) {
        if (hits[i].root == root && _wcsicmp(hits[i].path, path) == 0 &&
            hits[i].isValue == isValue) {
            if (!isValue) return;
            if (valueName && _wcsicmp(hits[i].valueName, valueName) == 0) return;
        }
    }

    hits[*n].root = root;
    wcsncpy_s(hits[*n].path, MAX_PL, path, _TRUNCATE);
    wcsncpy_s(hits[*n].label, 48, label ? label : L"", _TRUNCATE);
    hits[*n].isValue = isValue;
    hits[*n].valueName[0] = 0;
    if (isValue && valueName)
        wcsncpy_s(hits[*n].valueName, 256, valueName, _TRUNCATE);
    (*n)++;
}

/* 启动项里指向该程序的值 */
static void CollectRunValues(ProgramInfo *p, RegHit *hits, int *n)
{
    static const struct { HKEY root; const WCHAR *path; } runs[] = {
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run" },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run" },
    };
    int k;

    for (k = 0; k < 4; k++) {
        HKEY hk;
        DWORD idx = 0;

        if (RegOpenKeyExW(runs[k].root, runs[k].path, 0, KEY_READ, &hk) != ERROR_SUCCESS)
            continue;

        for (;;) {
            WCHAR vname[256], vdata[MAX_PL];
            DWORD cch = _countof(vname), size, type;
            DWORD i;

            if (RegEnumValueW(hk, idx++, vname, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            cch = _countof(vname);
            size = sizeof(vdata);
            if (RegQueryValueExW(hk, vname, NULL, &type, (LPBYTE)vdata, &size) != ERROR_SUCCESS)
                continue;
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
            vdata[MAX_PL - 1] = 0;

            {
                BOOL hit = FALSE;
                if (p->installDir[0] && ContainsI(vdata, p->installDir)) hit = TRUE;
                for (i = 0; !hit && i < (DWORD)p->dataDirCount; i++) {
                    if (ContainsI(vdata, p->dataDirs[i])) hit = TRUE;
                }
                if (hit)
                    AddRegHit(hits, n, runs[k].root, runs[k].path, L"启动项", TRUE, vname);
            }
        }
        RegCloseKey(hk);
    }
}

/* ImagePath 指向该程序的服务 */
static void CollectServices(ProgramInfo *p, RegHit *hits, int *n)
{
    int i;

    if (!p->installDir[0]) return;
    for (i = 0; i < g_svcCount; i++) {
        if (ContainsI(g_svcs[i].image, p->installDir)) {
            WCHAR path[MAX_PL];
            swprintf_s(path, _countof(path),
                       L"SYSTEM\\CurrentControlSet\\Services\\%s", g_svcs[i].name);
            AddRegHit(hits, n, HKEY_LOCAL_MACHINE, path, L"服务", FALSE, NULL);
        }
    }
}

static int CollectRegHits(ProgramInfo *p, RegHit *hits)
{
    int n = 0, depth;
    WCHAR cand[MAX_PL];

    /* 1. 卸载项本身 */
    if (p->regPath[0] && p->hive) {
        AddRegHit(hits, &n, p->hive, p->regPath, L"卸载项", FALSE, NULL);
    }

    /* 2. SOFTWARE\<目录名> —— 只认完全同名的键, 不做模糊匹配。
       绝不退到厂商级: SOFTWARE\Tencent 是全家桶容器, 删了 QQ/企业微信一起完蛋 */
    wcsncpy_s(cand, _countof(cand), p->installDir, _TRUNCATE);
    for (depth = 0; depth < 2; depth++) {
        LPWSTR s = wcsrchr(cand, L'\\');
        WCHAR leaf[128], path[MAX_PL];

        if (!s) break;
        wcsncpy_s(leaf, _countof(leaf), s + 1, _TRUNCATE);
        if (leaf[0] == L'{') break;                 /* MSI GUID 目录 */
        if (leaf[0] && !IsGenericDirName(leaf)) {
            swprintf_s(path, _countof(path), L"SOFTWARE\\%s", leaf);
            if (!IsDangerousRegPath(path)) {
                if (RegKeyExists(HKEY_CURRENT_USER, path, 0))
                    AddRegHit(hits, &n, HKEY_CURRENT_USER, path, L"软件项", FALSE, NULL);
                if (RegKeyExists(HKEY_LOCAL_MACHINE, path, KEY_WOW64_64KEY))
                    AddRegHit(hits, &n, HKEY_LOCAL_MACHINE, path, L"软件项", FALSE, NULL);
                if (RegKeyExists(HKEY_LOCAL_MACHINE, path, KEY_WOW64_32KEY))
                    AddRegHit(hits, &n, HKEY_LOCAL_MACHINE, path, L"软件项(32)", FALSE, NULL);
            }
            break;
        }
        *s = 0;
    }

    /* 3. 启动项 */
    CollectRunValues(p, hits, &n);

    /* 4. 服务 */
    CollectServices(p, hits, &n);

    return n;
}

static BOOL DeleteRegHit(const RegHit *h)
{
    if (h->isValue) {
        HKEY hk;
        LONG r;
        if (RegOpenKeyExW(h->root, h->path, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
            return FALSE;
        r = RegDeleteValueW(hk, h->valueName);
        RegCloseKey(hk);
        return r == ERROR_SUCCESS;
    }
    return RegDeleteTreeW(h->root, h->path) == ERROR_SUCCESS;
}

/* 判断某程序是否正在运行: 进程路径落在 installDir 下 */
static BOOL IsProgramRunning(const ProgramInfo *p)
{
    int i;
    size_t dl;

    if (p->installDir[0]) {
        dl = wcslen(p->installDir);
        for (i = 0; i < g_procCount; i++) {
            if (!g_procs[i].path[0]) continue;
            if (_wcsnicmp(g_procs[i].path, p->installDir, dl) == 0) {
                WCHAR c = g_procs[i].path[dl];
                if (c == 0 || c == L'\\') return TRUE;
            }
        }
    }
    if (p->iconPath[0]) {
        LPCWSTR base = wcsrchr(p->iconPath, L'\\');
        base = base ? base + 1 : p->iconPath;
        for (i = 0; i < g_procCount; i++) {
            if (_wcsicmp(g_procs[i].name, base) == 0) return TRUE;
        }
    }
    return FALSE;
}

/* ============================================================
 * 路径安全校验
 * ============================================================ */

static BOOL StartsWithI(LPCWSTR s, LPCWSTR prefix)
{
    size_t n = wcslen(prefix);
    return _wcsnicmp(s, prefix, n) == 0;
}

/* 返回 TRUE 表示该目录可以删 */
static BOOL IsSafeToDelete(LPCWSTR dir)
{
    static const WCHAR *exactBad[] = {
        L"C:\\", L"C:\\Program Files", L"C:\\Program Files (x86)",
        L"C:\\ProgramData", L"C:\\Users", L"C:\\Windows",
        NULL
    };
    static const WCHAR *treeBad[] = {
        L"C:\\Windows\\",
        L"C:\\Program Files\\Common Files",
        L"C:\\Program Files (x86)\\Common Files",
        L"C:\\ProgramData\\Microsoft",
        L"C:\\ProgramData\\Package Cache",
        L"C:\\$Recycle.Bin",
        L"C:\\Recovery",
        L"C:\\Boot",
        NULL
    };
    WCHAR norm[MAX_PL];
    int i;

    if (!dir || !dir[0]) return FALSE;

    NormalizePath(dir, norm, _countof(norm));
    if (!norm[0]) return FALSE;

    /* 去掉 \\?\ 前缀再判断, 否则盘符检查会误判 */
    if (wcsncmp(norm, L"\\\\?\\", 4) == 0) {
        memmove(norm, norm + 4, (wcslen(norm + 4) + 1) * sizeof(WCHAR));
    }

    if (wcslen(norm) < 10) return FALSE;          /* 太浅, 如 C:\abc */
    if (wcsstr(norm, L"..")) return FALSE;
    if (norm[0] == L'\\' && norm[1] == L'\\') return FALSE;   /* UNC */

    /* 必须是 X:\ 开头的绝对路径 */
    if (!(norm[1] == L':' && norm[2] == L'\\')) return FALSE;

    for (i = 0; exactBad[i]; i++) {
        if (_wcsicmp(norm, exactBad[i]) == 0) return FALSE;
    }
    for (i = 0; treeBad[i]; i++) {
        if (StartsWithI(norm, treeBad[i])) return FALSE;
        /* 目录本身正好等于 treeBad 去掉尾部反斜杠 */
        {
            WCHAR t[64];
            wcsncpy_s(t, _countof(t), treeBad[i], _TRUNCATE);
            TrimRightSlash(t);
            if (_wcsicmp(norm, t) == 0) return FALSE;
        }
    }

    /* 目录名本身不能是通用容器名 (如 C:\Users\me\AppData\Local\Programs) */
    {
        static const WCHAR *generic[] = {
            L"programs", L"program files", L"program files (x86)", L"appdata",
            L"local", L"locallow", L"roaming", L"temp", L"tmp",
            L"windows", L"system32", L"syswow64", L"users", L"programdata",
            L"common files", L"documents", L"desktop", L"downloads",
            L"microsoft", L"packages", NULL
        };
        LPCWSTR base = wcsrchr(norm, L'\\');
        int g;
        base = base ? base + 1 : norm;
        for (g = 0; generic[g]; g++) {
            if (_wcsicmp(base, generic[g]) == 0) return FALSE;
        }
    }
    return TRUE;
}

/* ============================================================
 * 文件删除
 *
 * 被占用的文件不再直接判死刑:
 *   1. 先正常删, 删不掉的记进 g_locked
 *   2. Restart Manager 查是哪个进程占着
 *   3. 强杀占用进程 (桌面/杀软这类不杀)
 *   4. 重试删除, 最多 3 轮
 *   5. 还不行就 MoveFileEx 标记重启后删除
 * ============================================================ */

#define MAX_LOCKED 512

typedef struct {
    WCHAR path[MAX_PL];
} LockedFile;

static LockedFile g_locked[MAX_LOCKED];
static int        g_lockedCount;
static int        g_pendingReboot;
static int        g_killedLockers;
static BOOL       g_noKillTest;      /* /testdelete /nokill: 跳过强杀, 验证重启删除兜底 */

static BOOL KillPid(DWORD pid);

static void AddLocked(LPCWSTR path)
{
    LPCWSTR p = path;
    int i;

    if (g_lockedCount >= MAX_LOCKED) return;
    /* MoveFileEx 的延迟删除不认 \\?\ 前缀, 存普通路径 */
    if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;

    for (i = 0; i < g_lockedCount; i++) {
        if (_wcsicmp(g_locked[i].path, p) == 0) return;
    }
    wcsncpy_s(g_locked[g_lockedCount].path, MAX_PL, p, _TRUNCATE);
    g_lockedCount++;
}

static void DeleteTreeRec(LPCWSTR dir, int depth)
{
    WCHAR pattern[MAX_PL];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (depth > 40) return;
    if (swprintf_s(pattern, MAX_PL, L"%s\\*", dir) < 0) return;

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        WCHAR child[MAX_PL];
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (swprintf_s(child, MAX_PL, L"%s\\%s", dir, fd.cFileName) < 0)
            continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (!RemoveDirectoryW(child)) AddLocked(child);   /* 不跟链接进去 */
            } else {
                DeleteTreeRec(child, depth + 1);
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                if (!RemoveDirectoryW(child)) AddLocked(child);
            }
        } else {
            SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(child)) {
                g_delFiles++;
            } else {
                if (g_lockedCount < MAX_LOCKED) AddLocked(child);
                else g_delFail++;
            }
        }
        if ((g_delFiles & 0x3F) == 0) UpdateWindow(g_hMain);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

/* 用 Restart Manager 查这些文件被哪些进程占着 */
static int RmFindLockers(LPCWSTR *files, int count, DWORD *pids, int maxPids)
{
    DWORD sess = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1];
    UINT needed = 0, got = 0, reasons = 0;
    RM_PROCESS_INFO *info;
    int found = 0;

    if (count <= 0) return 0;
    ZeroMemory(key, sizeof(key));

    if (RmStartSession(&sess, 0, key) != ERROR_SUCCESS) return 0;
    if (RmRegisterResources(sess, (UINT)count, files, 0, NULL, 0, NULL) != ERROR_SUCCESS) {
        RmEndSession(sess);
        return 0;
    }

    RmGetList(sess, &needed, &got, NULL, &reasons);
    if (needed == 0) {
        RmEndSession(sess);
        return 0;
    }

    info = (RM_PROCESS_INFO *)calloc(needed, sizeof(RM_PROCESS_INFO));
    if (!info) {
        RmEndSession(sess);
        return 0;
    }

    got = needed;
    if (RmGetList(sess, &needed, &got, info, &reasons) == ERROR_SUCCESS) {
        UINT i;
        for (i = 0; i < got && found < maxPids; i++) {
            DWORD pid = info[i].Process.dwProcessId;
            int k, dup = 0;
            for (k = 0; k < found; k++) {
                if (pids[k] == pid) { dup = 1; break; }
            }
            if (!dup && pid != 0) pids[found++] = pid;
        }
    }
    free(info);
    RmEndSession(sess);
    return found;
}

static BOOL FindProcName(DWORD pid, LPWSTR name, size_t cch)
{
    int i;
    HANDLE hp;
    WCHAR path[MAX_PL];
    DWORD c = MAX_PL;

    for (i = 0; i < g_procCount; i++) {
        if (g_procs[i].pid == pid) {
            wcsncpy_s(name, cch, g_procs[i].name, _TRUNCATE);
            return TRUE;
        }
    }

    hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return FALSE;
    if (QueryFullProcessImageNameW(hp, 0, path, &c)) {
        wcsncpy_s(name, cch, PathFindFileNameW(path), _TRUNCATE);
        CloseHandle(hp);
        return TRUE;
    }
    CloseHandle(hp);
    return FALSE;
}

/* 就算持有文件也不该杀的: 杀了闪桌面, 或者根本杀不掉 */
static BOOL IsNoKillName(LPCWSTR name)
{
    static const WCHAR *no[] = {
        L"explorer.exe", L"dwm.exe", L"MsMpEng.exe", L"NisSrv.exe",
        L"SecurityHealthService.exe", L"winlogon.exe", L"csrss.exe",
        L"smss.exe", L"wininit.exe", L"services.exe", L"lsass.exe",
        L"System", L"Registry", L"Memory Compression", L"csrss",
        NULL
    };
    int i;
    for (i = 0; no[i]; i++) {
        if (_wcsicmp(name, no[i]) == 0) return TRUE;
    }
    return FALSE;
}

/* 查占用进程并强杀, 返回杀掉的个数 (0 表示没有可杀的, 再试也没用) */
static int FreeLockedFiles(void)
{
    LPCWSTR files[128];
    DWORD pids[64];
    int n = 0, i, found, killed = 0;

    if (g_lockedCount == 0) return 0;

    if (g_noKillTest) {
        Log(L"    [测试模式] 跳过强杀, 直接走重启删除");
        return 0;
    }

    for (i = 0; i < g_lockedCount && n < 128; i++) {
        files[n++] = g_locked[i].path;
    }

    found = RmFindLockers(files, n, pids, 64);
    if (found == 0) {
        Log(L"    没找到占用进程 (可能是目录被当工作目录, 或权限不足)");
        return 0;
    }

    for (i = 0; i < found; i++) {
        WCHAR name[MAX_PATH];
        if (pids[i] == GetCurrentProcessId()) continue;
        if (!FindProcName(pids[i], name, _countof(name))) continue;

        if (IsNoKillName(name)) {
            Log(L"    [不杀] %s (PID %lu) 持有文件, 稍后标记重启删除", name, pids[i]);
            continue;
        }
        if (KillPid(pids[i])) {
            Log(L"    [强制结束] %s (PID %lu)", name, pids[i]);
            killed++;
        } else {
            Log(L"    [杀不掉] %s (PID %lu)", name, pids[i]);
        }
    }
    g_killedLockers += killed;
    return killed;
}

static void DeleteDirectory(LPCWSTR dir)
{
    WCHAR root[MAX_PL];
    int round;

    ToLongPath(dir, root, _countof(root));
    g_lockedCount = 0;

    for (round = 0; round < 3; round++) {
        DeleteTreeRec(root, 0);
        SetFileAttributesW(root, FILE_ATTRIBUTE_NORMAL);

        if (GetFileAttributesW(root) == INVALID_FILE_ATTRIBUTES)
            return;                              /* 目录已经没了 */

        if (g_lockedCount == 0) {
            if (RemoveDirectoryW(root)) return;  /* 根目录也删掉了 */
            /* 文件都清完了, 只剩根目录删不掉: 多半是被当成工作目录 */
            AddLocked(root);
        }

        if (round == 0)
            Log(L"    %d 项删不掉, 开始查占用进程", g_lockedCount);

        if (FreeLockedFiles() == 0) break;       /* 没杀到人, 重试也没用 */
        Sleep(250);
        g_lockedCount = 0;
    }

    /* 剩下的标记重启后删除 */
    if (g_lockedCount > 0) {
        int i, marked = 0;
        for (i = 0; i < g_lockedCount; i++) {
            if (MoveFileExW(g_locked[i].path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
                marked++;
                g_pendingReboot++;
            } else {
                g_delFail++;
                if (g_delFail <= 15)
                    Log(L"    [删不掉] %s", g_locked[i].path);
            }
        }
        if (marked > 0)
            Log(L"    %d 项已标记为重启后自动删除", marked);
        g_lockedCount = 0;
    }
}

/* ============================================================
 * 进程终止
 * ============================================================ */

static BOOL KillPid(DWORD pid)
{
    HANDLE hp = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    BOOL ok = FALSE;

    if (!hp) return FALSE;
    ok = TerminateProcess(hp, 0);
    if (ok) WaitForSingleObject(hp, 2000);
    CloseHandle(hp);
    return ok;
}

/* 杀掉 exe 路径位于 dir 下的进程 */
static int KillProcsUnder(LPCWSTR dir)
{
    int i, killed = 0;
    size_t dl = wcslen(dir);

    for (i = 0; i < g_procCount; i++) {
        if (!g_procs[i].path[0]) continue;
        if (g_procs[i].pid == GetCurrentProcessId()) continue;
        if (_wcsnicmp(g_procs[i].path, dir, dl) != 0) continue;
        {
            WCHAR c = g_procs[i].path[dl];
            if (c != 0 && c != L'\\') continue;
        }
        if (KillPid(g_procs[i].pid)) {
            killed++;
            Log(L"    已终止 %s (PID %lu)", g_procs[i].name, g_procs[i].pid);
        }
    }
    return killed;
}

/* 一键关闭: 杀所有非白名单的用户会话进程 */
static int KillAllNonEssential(void)
{
    int i, killed = 0;
    DWORD self = GetCurrentProcessId();
    DWORD parent = 0;

    for (i = 0; i < g_procCount; i++) {
        if (g_procs[i].pid == self) { parent = g_procs[i].ppid; break; }
    }

    for (i = 0; i < g_procCount; i++) {
        ProcEntry *p = &g_procs[i];
        if (p->pid == 0 || p->pid == 4) continue;
        if (p->pid == self || p->pid == parent) continue;
        if (p->session == 0) continue;              /* 不碰 Session 0 服务 */
        if (IsProtectedName(p->name)) continue;
        if (KillPid(p->pid)) killed++;
    }
    return killed;
}

/* ============================================================
 * 注册表备份与删除
 * ============================================================ */

static BOOL BackupRegKey(HKEY root, LPCWSTR path, LPCWSTR file)
{
    HKEY hk;
    BOOL ok;

    if (RegOpenKeyExW(root, path, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return FALSE;
    ok = (RegSaveKeyExW(hk, file, NULL, REG_LATEST_FORMAT) == ERROR_SUCCESS);
    RegCloseKey(hk);
    return ok;
}

static BOOL DeleteRegKey(HKEY root, LPCWSTR path)
{
    return RegDeleteTreeW(root, path) == ERROR_SUCCESS;
}

/* ============================================================
 * 列表刷新
 * ============================================================ */

static void FormatSize(DWORD kb, LPWSTR out, size_t cch)
{
    if (kb == 0) { wcsncpy_s(out, cch, L"-", _TRUNCATE); return; }
    if (kb >= 1024 * 1024)
        swprintf_s(out, cch, L"%.1f GB", kb / 1024.0 / 1024.0);
    else if (kb >= 1024)
        swprintf_s(out, cch, L"%.1f MB", kb / 1024.0);
    else
        swprintf_s(out, cch, L"%lu KB", kb);
}

static int CompareProg(const void *a, const void *b)
{
    const ProgramInfo *pa = (const ProgramInfo *)a;
    const ProgramInfo *pb = (const ProgramInfo *)b;
    if (pa->running != pb->running)
        return pa->running ? -1 : 1;      /* 运行中的排前面 */
    return _wcsicmp(pa->name, pb->name);
}

static void PlanAppend(LPWSTR buf, size_t cch, size_t *pos, LPCWSTR fmt, ...)
{
    int w;
    va_list ap;

    if (*pos + 1 >= cch) return;
    va_start(ap, fmt);
    w = _vsnwprintf_s(buf + *pos, cch - *pos, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (w > 0) *pos += (size_t)w;
}

static const WCHAR *SrcLabel(SrcType s)
{
    switch (s) {
    case SRC_HKCU:            return L"HKCU";
    case SRC_APPDATA_LOCAL:   return L"AppData";
    case SRC_APPDATA_ROAMING: return L"Roaming";
    default:                  return L"HKLM";
    }
}

static void RefreshList(void)
{
    int i;
    LVITEMW item;

    if (!g_hList) return;
    ListView_DeleteAllItems(g_hList);

    for (i = 0; i < g_progCount; i++) {
        ProgramInfo *p = &g_progs[i];
        WCHAR szText[64];
        WCHAR *cols[7];

        FormatSize(p->sizeKB, szText, _countof(szText));
        cols[0] = p->name;
        cols[1] = (LPWSTR)SrcLabel(p->src);
        cols[2] = p->publisher[0] ? p->publisher : (LPWSTR)L"-";
        cols[3] = p->version[0] ? p->version : (LPWSTR)L"-";
        cols[4] = szText;
        cols[5] = p->running ? (LPWSTR)L"运行中" : (LPWSTR)L"已停止";
        cols[6] = p->installDir[0] ? p->installDir : (LPWSTR)L"(未知)";

        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = cols[0];
        item.lParam = i;
        SendMessageW(g_hList, LVM_INSERTITEMW, 0, (LPARAM)&item);

        {
            int c;
            for (c = 1; c < 7; c++) {
                LVITEMW sub;
                ZeroMemory(&sub, sizeof(sub));
                sub.iSubItem = c;
                sub.pszText = cols[c];
                SendMessageW(g_hList, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&sub);
            }
        }
        ListView_SetItemState(g_hList, i, INDEXTOSTATEIMAGEMASK(1), LVIS_STATEIMAGEMASK);
    }
}

static void DoScan(void)
{
    int i;

    Status(L"正在扫描已安装程序...");
    Log(L"");
    Log(L"======== 开始扫描 ========");

    g_progCount = 0;
    SnapshotProcs();
    Log(L"进程快照: %d 个", g_procCount);

    ScanUninstallKey(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, TRUE);
    ScanUninstallKey(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, FALSE);
    ScanUninstallKey(HKEY_CURRENT_USER,  0,              FALSE);
    /* 先算出注册表程序的 AppData 目录, 再扫 AppData, 才能把数据目录从软件列表里剔除 */
    for (i = 0; i < g_progCount; i++) CollectDataDirs(&g_progs[i]);
    ScanAppData();
    LoadServices();

    for (i = 0; i < g_progCount; i++) {
        g_progs[i].running = IsProgramRunning(&g_progs[i]);
    }
    qsort(g_progs, g_progCount, sizeof(ProgramInfo), CompareProg);
    RefreshList();

    {
        int running = 0, appdata = 0;
        for (i = 0; i < g_progCount; i++) {
            if (g_progs[i].running) running++;
            if (g_progs[i].src == SRC_APPDATA_LOCAL || g_progs[i].src == SRC_APPDATA_ROAMING)
                appdata++;
        }
        Log(L"扫描完成: %d 个程序 (其中 %d 个仅存在于 AppData), %d 个正在运行",
            g_progCount, appdata, running);
        Status(L"扫描完成");
    }
}

/* ============================================================
 * 一键关闭
 * ============================================================ */

static void OnKillAll(void)
{
    WCHAR msg[512];
    int killed;

    if (g_busy) return;
    if (MessageBoxW(g_hMain,
            L"将终止当前用户会话内所有非系统进程。\n\n"
            L"未保存的文档、浏览器标签、正在运行的任务都会丢失。\n"
            L"系统服务(Session 0)和内核关键进程不会被动。\n\n"
            L"确定继续？",
            L"一键关闭进程", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    g_busy = TRUE;
    SnapshotProcs();
    Status(L"正在终止进程...");
    Log(L"");
    Log(L"======== 一键关闭 ========");

    killed = KillAllNonEssential();

    swprintf_s(msg, _countof(msg), L"已终止 %d 个进程", killed);
    Log(L"完成: %s", msg);
    Status(msg);
    g_busy = FALSE;

    SnapshotProcs();
    DoScan();
}

/* ============================================================
 * 卸载选中项
 * ============================================================ */

static void OnUninstall(void)
{
    int idx[MAX_PROGS];
    int count = 0, i, n, d, h;
    WCHAR plan[8192];
    size_t pos = 0;
    SYSTEMTIME st;
    WCHAR backupDir[MAX_PATH], stamp[64];
    int totalDataDirs = 0, totalRegHits = 0;

    if (g_busy) return;
    if (g_progCount == 0) {
        MessageBoxW(g_hMain, L"请先扫描。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    n = ListView_GetItemCount(g_hList);
    for (i = 0; i < n; i++) {
        if (ListView_GetCheckState(g_hList, i))
            idx[count++] = i;
    }
    if (count == 0) {
        MessageBoxW(g_hMain,
            L"没有勾选任何程序。\n\n先勾选左侧复选框，再点卸载。",
            L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    /* 先算清楚要删什么, 再让用户确认 */
    for (i = 0; i < count; i++) {
        RegHit hits[MAX_REGHITS];
        totalDataDirs += g_progs[idx[i]].dataDirCount;
        totalRegHits += CollectRegHits(&g_progs[idx[i]], hits);
    }

    PlanAppend(plan, _countof(plan), &pos,
        L"即将强制卸载 %d 个程序\r\n"
        L"共删除 %d 个数据目录、%d 项注册表\r\n\r\n", count, totalDataDirs, totalRegHits);

    for (i = 0; i < count && i < 4; i++) {
        ProgramInfo *p = &g_progs[idx[i]];
        BOOL hasDir = p->installDir[0] != 0;
        BOOL safe = hasDir && IsSafeToDelete(p->installDir);
        const WCHAR *note;

        if (!hasDir)    note = L"无安装目录记录 -> 只清注册表";
        else if (safe)  note = L"将删除";
        else            note = L"[警告] 在保护范围内 -> 跳过";

        PlanAppend(plan, _countof(plan), &pos, L"%d. %s  [%s]\r\n   目录: %s\r\n   %s\r\n",
            i + 1, p->name, SrcLabel(p->src),
            hasDir ? p->installDir : L"(无)", note);

        for (d = 0; d < p->dataDirCount; d++) {
            PlanAppend(plan, _countof(plan), &pos, L"   数据: %s\r\n", p->dataDirs[d]);
        }
        /* 服务单独列出来 —— 删服务影响面大, 必须让人看见 */
        {
            RegHit hh[MAX_REGHITS];
            int hc = CollectRegHits(p, hh), k;
            for (k = 0; k < hc; k++) {
                if (wcsstr(hh[k].path, L"\\Services\\"))
                    PlanAppend(plan, _countof(plan), &pos, L"   服务: %s\r\n",
                               PathFindFileNameW(hh[k].path));
            }
        }
    }
    if (count > 4)
        PlanAppend(plan, _countof(plan), &pos, L"... 另有 %d 个程序\r\n", count - 4);

    PlanAppend(plan, _countof(plan), &pos,
        L"\r\n文件和注册表都将永久删除, 不进回收站。\r\n"
        L"注册表会先备份到 backup 目录。\r\n"
        L"文件被占用时会强制结束占用它的进程。\r\n"
        L"详细清单见主窗口日志。\r\n\r\n确定执行？");

    /* 详细清单先写日志 */
    Log(L"");
    Log(L"======== 卸载计划 ========");
    for (i = 0; i < count; i++) {
        ProgramInfo *p = &g_progs[idx[i]];
        RegHit hits[MAX_REGHITS];
        int hc = CollectRegHits(p, hits);

        Log(L"%d. %s  [%s]", i + 1, p->name, SrcLabel(p->src));
        if (p->installDir[0])
            Log(L"   安装目录: %s", p->installDir);
        for (d = 0; d < p->dataDirCount; d++)
            Log(L"   数据目录: %s", p->dataDirs[d]);
        for (h = 0; h < hc; h++) {
            Log(L"   注册表[%s]: %s%s%s%s", hits[h].label,
                hits[h].root == HKEY_LOCAL_MACHINE ? L"HKLM\\" : L"HKCU\\",
                hits[h].path,
                hits[h].isValue ? L" 值=" : L"",
                hits[h].isValue ? hits[h].valueName : L"");
        }
    }

    if (MessageBoxW(g_hMain, plan, L"确认强制卸载", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    /* 备份目录 */
    GetLocalTime(&st);
    swprintf_s(stamp, _countof(stamp), L"%04d%02d%02d_%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    swprintf_s(backupDir, _countof(backupDir), L"%s\\backup", g_selfDir);
    CreateDirectoryW(backupDir, NULL);
    swprintf_s(backupDir, _countof(backupDir), L"%s\\backup\\%s", g_selfDir, stamp);
    CreateDirectoryW(backupDir, NULL);

    /* 每次卸载单独统计 */
    g_delFiles = 0;
    g_delFail = 0;
    g_killedLockers = 0;
    g_pendingReboot = 0;

    EnablePrivilege(L"SeDebugPrivilege");
    EnablePrivilege(L"SeBackupPrivilege");
    EnablePrivilege(L"SeRestorePrivilege");

    g_busy = TRUE;
    Log(L"");
    Log(L"======== 开始卸载 (%d 个) ========", count);
    Log(L"备份目录: %s", backupDir);

    for (i = 0; i < count; i++) {
        ProgramInfo *p = &g_progs[idx[i]];
        WCHAR safeName[128];
        RegHit hits[MAX_REGHITS];
        int hitCount, okReg = 0, failReg = 0, svcTouched = 0;

        Log(L"");
        Log(L"[%d/%d] %s", i + 1, count, p->name);
        Status(p->name);
        UpdateWindow(g_hMain);

        /* 1. 关进程 */
        if (p->installDir[0]) {
            SnapshotProcs();
            {
                int k = KillProcsUnder(p->installDir);
                Log(L"    终止进程: %d 个", k);
            }
            Sleep(300);
        }

        /* 2. 收集 + 备份注册表 */
        hitCount = CollectRegHits(p, hits);
        SanitizeFileName(p->name, safeName, _countof(safeName));
        for (h = 0; h < hitCount; h++) {
            WCHAR regFile[MAX_PL];
            swprintf_s(regFile, _countof(regFile), L"%s\\%s_%d.hiv", backupDir, safeName, h + 1);
            if (BackupRegKey(hits[h].root, hits[h].path, regFile))
                Log(L"    备份[%s]: %s", hits[h].label, hits[h].path);
            else
                Log(L"    [警告] 备份失败: %s", hits[h].path);
        }

        /* 3. 删安装目录 */
        if (p->installDir[0]) {
            if (IsSafeToDelete(p->installDir)) {
                int before = g_delFiles;
                Log(L"    删除目录: %s", p->installDir);
                DeleteDirectory(p->installDir);
                Log(L"    已删除文件: %d", g_delFiles - before);
            } else {
                Log(L"    [跳过] 目录在保护范围内, 不删: %s", p->installDir);
            }
        }

        /* 4. 删数据目录 */
        for (d = 0; d < p->dataDirCount; d++) {
            if (IsSafeToDeleteData(p->dataDirs[d])) {
                int before = g_delFiles;
                Log(L"    删除数据: %s", p->dataDirs[d]);
                DeleteDirectory(p->dataDirs[d]);
                Log(L"    已删除文件: %d", g_delFiles - before);
            } else {
                Log(L"    [跳过] 数据目录不在 AppData 内, 不删: %s", p->dataDirs[d]);
            }
        }

        /* 5. 删注册表 */
        for (h = 0; h < hitCount; h++) {
            if (DeleteRegHit(&hits[h])) {
                okReg++;
                if (wcsstr(hits[h].path, L"\\Services\\")) svcTouched++;
            } else {
                failReg++;
                Log(L"    [失败] 删除注册表: %s", hits[h].path);
            }
        }
        Log(L"    注册表: 删除 %d 项, 失败 %d 项", okReg, failReg);
        if (svcTouched)
            Log(L"    [注意] 删了 %d 个服务项, 需重启后彻底消失", svcTouched);
    }

    Log(L"");
    Log(L"======== 卸载结束 ========");
    Log(L"共删除文件 %d 个, 失败 %d 个", g_delFiles, g_delFail);
    if (g_killedLockers)
        Log(L"强制结束占用进程 %d 个", g_killedLockers);
    if (g_pendingReboot)
        Log(L"标记重启后删除 %d 项", g_pendingReboot);
    Status(L"卸载完成");

    g_busy = FALSE;
    {
        WCHAR done[1024];
        WCHAR extra[512] = L"";
        size_t ep = 0;

        if (g_killedLockers)
            PlanAppend(extra, _countof(extra), &ep,
                L"\n有文件被占用，已强制结束 %d 个占用进程。", g_killedLockers);
        if (g_pendingReboot)
            PlanAppend(extra, _countof(extra), &ep,
                L"\n有 %d 项被系统占用无法立即删除，已标记为重启后自动删除。",
                g_pendingReboot);

        swprintf_s(done, _countof(done),
            L"卸载完成。\n\n"
            L"删除文件 %d 个，失败 %d 个。%s\n\n"
            L"注册表备份在 backup 文件夹下 (.hiv 格式)。\n"
            L"如果删除了服务项，需要重启才能彻底消失。\n\n"
            L"建议重新扫描确认结果。",
            g_delFiles, g_delFail, extra);

        MessageBoxW(g_hMain, done, L"完成", MB_OK | MB_ICONINFORMATION);
    }

    DoScan();
}

/* ============================================================
 * 右键菜单管理
 *
 * 扫描 HKEY_CLASSES_ROOT 下所有 shell verb 和 shellex 处理器。
 * 禁用方式:
 *   shell\<verb>                 -> 写 LegacyDisable 值 (微软官方隐藏标志)
 *   shellex\ContextMenuHandlers  -> 把 CLSID 挪到 SysCleanBackup, 清空默认值
 * 两种都是可逆的, 随时能切回来。
 * ============================================================ */

static void LVSet(HWND lv, int row, int col, LPCWSTR text);

/* 取 MUIVerb 里的资源路径部分, 用来判断是不是系统自带的菜单 */
static BOOL IsSystemPath(LPCWSTR p);

static BOOL IndirectIsSystem(LPCWSTR s)
{
    WCHAR p[MAX_PATH];
    const WCHAR *q;
    int i = 0;

    if (!s || s[0] != L'@') return FALSE;
    q = s + 1;
    while (*q && *q != L',' && i < MAX_PATH - 1) p[i++] = *q++;
    p[i] = 0;
    /* 带盘绝对路径 / %VAR% 展开路径: 看它落在哪 */
    if (p[1] == L':' || p[0] == L'%') return IsSystemPath(p);
    /* 不带路径的裸文件名 (shell32.dll / display.dll) 几乎都是系统的,
       第三方很少这么写, 误判只影响排序和标签, 不影响安全性 */
    return TRUE;
}

static HWND g_hMwnd, g_hMList, g_hMInfo;
static BOOL g_mFilling;

static void ReadClsidInfo(LPCWSTR clsid, LPWSTR name, size_t ncch, LPWSTR dll, size_t dcch)
{
    WCHAR path[MAX_PL];
    HKEY hk;

    name[0] = 0;
    dll[0] = 0;
    if (!clsid || !clsid[0]) return;

    swprintf_s(path, _countof(path), L"CLSID\\%s", clsid);
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        ReadStr(hk, NULL, name, (DWORD)ncch);
        RegCloseKey(hk);
    }

    swprintf_s(path, _countof(path), L"CLSID\\%s\\InprocServer32", clsid);
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        ReadStr(hk, NULL, dll, (DWORD)dcch);
        RegCloseKey(hk);
    }
}

static BOOL IsSystemPath(LPCWSTR p)
{
    if (!p || !p[0]) return FALSE;
    if (ContainsI(p, L"\\Windows\\")) return TRUE;
    if (ContainsI(p, L"%SystemRoot%")) return TRUE;
    if (ContainsI(p, L"%systemroot%")) return TRUE;
    if (ContainsI(p, L"\\Program Files\\Windows Defender")) return TRUE;
    if (ContainsI(p, L"\\Program Files (x86)\\Windows Defender")) return TRUE;
    if (ContainsI(p, L"Common Files\\Microsoft Shared")) return TRUE;
    return FALSE;
}

static BOOL IsSystemCmd(LPCWSTR cmd)
{
    return IsSystemPath(cmd);
}

static void ScanMenuKey(LPCWSTR basePath, LPCWSTR scope, BOOL shellex)
{
    HKEY hk;
    DWORD idx = 0;

    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, basePath, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;

    for (;;) {
        WCHAR leaf[256];
        DWORD cch = _countof(leaf);
        HKEY hsub;
        MenuItem *m;
        BOOL indirSys = FALSE;                    /* MUIVerb 指向系统资源 */

        if (RegEnumKeyExW(hk, idx++, leaf, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (g_menuCount >= MAX_MENU) break;
        if (leaf[0] == L'-') continue;          /* 旧式禁用写法, 不列 */

        m = &g_menu[g_menuCount];
        ZeroMemory(m, sizeof(*m));
        wcsncpy_s(m->verb, _countof(m->verb), leaf, _TRUNCATE);
        wcsncpy_s(m->scope, _countof(m->scope), scope, _TRUNCATE);
        swprintf_s(m->path, MAX_PL, L"%s\\%s", basePath, leaf);
        m->isShellex = shellex;

        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, m->path, 0, KEY_READ, &hsub) != ERROR_SUCCESS)
            continue;

        if (shellex) {
            WCHAR backup[256] = L"", dll[MAX_PL] = L"", cname[256] = L"";

            ReadStr(hsub, NULL, m->clsid, _countof(m->clsid));
            ReadStr(hsub, L"SysCleanBackup", backup, _countof(backup));

            if (m->clsid[0]) {
                m->disabled = FALSE;
            } else if (backup[0]) {
                m->disabled = TRUE;
                wcsncpy_s(m->clsid, _countof(m->clsid), backup, _TRUNCATE);
            } else {
                RegCloseKey(hsub);
                continue;                        /* 空壳键 */
            }

            ReadClsidInfo(m->clsid, cname, _countof(cname), dll, _countof(dll));
            wcsncpy_s(m->cmd, MAX_PL, dll, _TRUNCATE);
            wcsncpy_s(m->name, _countof(m->name), cname[0] ? cname : leaf, _TRUNCATE);
        } else {
            WCHAR tmp[256] = L"", cmdKey[MAX_PL];
            DWORD sz = 0;

            ReadStr(hsub, L"MUIVerb", tmp, _countof(tmp));
            if (!tmp[0]) ReadStr(hsub, NULL, tmp, _countof(tmp));

            m->disabled = (RegQueryValueExW(hsub, L"LegacyDisable", NULL, NULL,
                                            NULL, &sz) == ERROR_SUCCESS);

            swprintf_s(cmdKey, _countof(cmdKey), L"%s\\command", m->path);
            {
                HKEY hc;
                if (RegOpenKeyExW(HKEY_CLASSES_ROOT, cmdKey, 0, KEY_READ, &hc) == ERROR_SUCCESS) {
                    ReadStr(hc, NULL, m->cmd, _countof(m->cmd));
                    RegCloseKey(hc);
                }
            }

            /* "@xxx.dll,-8506" 是间接字符串, 只有解析之后才知道菜单到底叫啥 */
            if (tmp[0] == L'@') {
                WCHAR real[256];
                indirSys = IndirectIsSystem(tmp);
                if (SUCCEEDED(SHLoadIndirectString(tmp, real, _countof(real), NULL))) {
                    wcsncpy_s(tmp, _countof(tmp), real, _TRUNCATE);
                } else {
                    /* 资源可能被卸载了, 退回来用文件名, 至少能认出来 */
                    const WCHAR *bs = wcsrchr(tmp, L'\\');
                    wcsncpy_s(tmp, _countof(tmp), bs ? bs + 1 : tmp, _TRUNCATE);
                }
            }

            /* 没名字也没命令的是内部 verb, 列出来只会干扰 */
            if (!tmp[0] && !m->cmd[0]) {
                RegCloseKey(hsub);
                continue;
            }
            wcsncpy_s(m->name, _countof(m->name), tmp[0] ? tmp : leaf, _TRUNCATE);
        }

        m->system = IsSystemCmd(m->cmd) || indirSys;
        RegCloseKey(hsub);
        g_menuCount++;
    }
    RegCloseKey(hk);
}

static int CompareMenu(const void *a, const void *b)
{
    const MenuItem *ma = (const MenuItem *)a;
    const MenuItem *mb = (const MenuItem *)b;
    if (ma->system != mb->system) return ma->system ? 1 : -1;   /* 第三方的排前面 */
    return _wcsicmp(ma->name, mb->name);
}

static void ScanAllMenus(void)
{
    g_menuCount = 0;
    ScanMenuKey(L"*\\shell", L"文件", FALSE);
    ScanMenuKey(L"*\\shellex\\ContextMenuHandlers", L"文件", TRUE);
    ScanMenuKey(L"Directory\\shell", L"文件夹", FALSE);
    ScanMenuKey(L"Directory\\shellex\\ContextMenuHandlers", L"文件夹", TRUE);
    ScanMenuKey(L"Directory\\Background\\shell", L"文件夹背景", FALSE);
    ScanMenuKey(L"Directory\\Background\\shellex\\ContextMenuHandlers", L"文件夹背景", TRUE);
    ScanMenuKey(L"Drive\\shell", L"驱动器", FALSE);
    ScanMenuKey(L"DesktopBackground\\Shell", L"桌面", FALSE);
    qsort(g_menu, g_menuCount, sizeof(MenuItem), CompareMenu);
}

static BOOL ToggleMenuItem(MenuItem *m)
{
    HKEY hk;

    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, m->path, 0, KEY_READ | KEY_WRITE, &hk) != ERROR_SUCCESS)
        return FALSE;

    if (!m->isShellex) {
        if (m->disabled) {
            RegDeleteValueW(hk, L"LegacyDisable");
            m->disabled = FALSE;
        } else {
            RegSetValueExW(hk, L"LegacyDisable", 0, REG_SZ, (const BYTE *)L"", sizeof(WCHAR));
            m->disabled = TRUE;
        }
    } else if (m->disabled) {
        WCHAR backup[256] = L"";
        if (ReadStr(hk, L"SysCleanBackup", backup, _countof(backup)) && backup[0]) {
            RegSetValueExW(hk, NULL, 0, REG_SZ, (const BYTE *)backup,
                           (DWORD)((wcslen(backup) + 1) * sizeof(WCHAR)));
            RegDeleteValueW(hk, L"SysCleanBackup");
            m->disabled = FALSE;
        }
    } else {
        WCHAR cur[256] = L"";
        if (ReadStr(hk, NULL, cur, _countof(cur)) && cur[0]) {
            RegSetValueExW(hk, L"SysCleanBackup", 0, REG_SZ, (const BYTE *)cur,
                           (DWORD)((wcslen(cur) + 1) * sizeof(WCHAR)));
            RegSetValueExW(hk, NULL, 0, REG_SZ, (const BYTE *)L"", sizeof(WCHAR));
            m->disabled = TRUE;
        }
    }
    RegCloseKey(hk);
    return TRUE;
}

static void RefreshMenuList(void)
{
    int i, off = 0, third = 0;
    WCHAR info[256];

    if (!g_hMList) return;
    g_mFilling = TRUE;
    SendMessageW(g_hMList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hMList);

    for (i = 0; i < g_menuCount; i++) {
        MenuItem *m = &g_menu[i];
        LVITEMW it;
        int row;

        if (m->disabled) off++;
        if (!m->system) third++;

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = m->disabled ? (LPWSTR)L"已禁用" : (LPWSTR)L"启用";
        it.lParam = i;
        row = (int)SendMessageW(g_hMList, LVM_INSERTITEMW, 0, (LPARAM)&it);

        LVSet(g_hMList, row, 1, m->name);
        LVSet(g_hMList, row, 2, m->scope);
        LVSet(g_hMList, row, 3, m->isShellex ? L"扩展" : L"命令");
        LVSet(g_hMList, row, 4, m->system ? L"系统" : L"第三方");
        LVSet(g_hMList, row, 5, m->path);

        ListView_SetItemState(g_hMList, row,
            INDEXTOSTATEIMAGEMASK(m->disabled ? 1 : 2), LVIS_STATEIMAGEMASK);
    }

    SendMessageW(g_hMList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hMList, NULL, TRUE);
    g_mFilling = FALSE;

    swprintf_s(info, _countof(info),
        L"共 %d 项 · 已禁用 %d 项 · 第三方 %d 项 (勾选=启用, 取消勾选=禁用)",
        g_menuCount, off, third);
    SetWindowTextW(g_hMInfo, info);
}

/* 命令行自检: SysClean.exe /menutest —— 不开 GUI 验证扫描与开关逻辑 */
static void MenuSelfTest(LPCWSTR out)
{
    FILE *f = _wfsopen(out, L"w, ccs=UTF-8", _SH_DENYNO);
    int i, off = 0, third = 0;

    if (!f) return;
    ScanAllMenus();
    if (g_menuCount <= 0) {
        fwprintf(f, L"FAIL 扫描结果为空, 检查 HKCR 读取权限\n");
        fclose(f);
        return;
    }
    for (i = 0; i < g_menuCount; i++) {
        if (g_menu[i].disabled) off++;
        if (!g_menu[i].system) third++;
    }
    fwprintf(f, L"total=%d disabled=%d third=%d\n\n", g_menuCount, off, third);
    for (i = 0; i < g_menuCount; i++) {
        MenuItem *m = &g_menu[i];
        fwprintf(f, L"[%-6s] %-24s %-8s %s\n  %s\n",
            m->disabled ? L"OFF" : L"ON", m->name, m->scope, m->path,
            m->cmd[0] ? m->cmd : L"(无命令)");
    }
    fclose(f);
}

/* 命令行自检: SysClean.exe /menutoggle "HKCR 子路径" —— 验证禁用/启用是否真的写进注册表 */
static void MenuToggleTest(LPCWSTR out, LPCWSTR path)
{
    FILE *f = _wfsopen(out, L"w, ccs=UTF-8", _SH_DENYNO);
    MenuItem m;
    HKEY hk;
    DWORD sz;
    int i;

    if (!f) return;
    ZeroMemory(&m, sizeof(m));
    wcsncpy_s(m.path, MAX_PL, path, _TRUNCATE);
    m.isShellex = (wcsstr(path, L"shellex") != NULL);
    wcsncpy_s(m.name, _countof(m.name), L"test", _TRUNCATE);
    wcsncpy_s(m.scope, _countof(m.scope), L"测试", _TRUNCATE);

    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, m.path, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        fwprintf(f, L"OK   test key opened: %s\n", path);
        RegCloseKey(hk);
    } else {
        fwprintf(f, L"FAIL 打不开测试键: %s\n", path);
        fclose(f);
        return;
    }

    /* 关 */
    if (!ToggleMenuItem(&m)) { fwprintf(f, L"FAIL 禁用失败\n"); fclose(f); return; }
    {
        int written = 0;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, m.path, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            if (!m.isShellex) {
                sz = 0;
                if (RegQueryValueExW(hk, L"LegacyDisable", NULL, NULL, NULL, &sz) == ERROR_SUCCESS)
                    written = 1;
            } else {
                WCHAR cur[256] = L"", bak[256] = L"";
                ReadStr(hk, NULL, cur, 256);
                ReadStr(hk, L"SysCleanBackup", bak, 256);
                if (!cur[0] && bak[0]) written = 1;
            }
            RegCloseKey(hk);
        }
        fwprintf(f, written ? L"OK   禁用已写入注册表\n" : L"FAIL 禁用没生效\n");
    }

    /* 开 */
    if (!ToggleMenuItem(&m)) { fwprintf(f, L"FAIL 启用失败\n"); fclose(f); return; }
    {
        int clean = 1;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, m.path, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            WCHAR cur[256] = L"", bak[256] = L"";
            DWORD s2 = 0;
            ReadStr(hk, NULL, cur, 256);
            ReadStr(hk, L"SysCleanBackup", bak, 256);
            if (!m.isShellex && RegQueryValueExW(hk, L"LegacyDisable", NULL, NULL, NULL, &s2) == ERROR_SUCCESS)
                clean = 0;
            if (m.isShellex && (bak[0] || !cur[0])) clean = 0;
            RegCloseKey(hk);
        }
        fwprintf(f, clean ? L"OK   还原已写入注册表\n" : L"FAIL 还原没生效\n");
    }

    /* 顺带确认扫描能认出刚动过的那一项 */
    ScanAllMenus();
    for (i = 0; i < g_menuCount; i++)
        if (!_wcsicmp(g_menu[i].path, m.path))
            fwprintf(f, L"OK   扫描可见: %s [%s]\n", g_menu[i].name, g_menu[i].scope);
    fclose(f);
}

static void OnMenuToggle(int row)
{
    MenuItem *m;

    if (row < 0 || row >= g_menuCount) return;
    m = &g_menu[row];

    if (!ToggleMenuItem(m)) {
        MessageBoxW(g_hMwnd, L"操作失败: 权限不足或注册表项已被删除。", L"失败",
                    MB_OK | MB_ICONERROR);
        RefreshMenuList();
        return;
    }

    Log(L"[右键菜单] %s [%s] -> %s", m->name, m->scope,
        m->disabled ? L"已禁用" : L"已启用");

    g_mFilling = TRUE;
    ListView_SetItemState(g_hMList, row,
        INDEXTOSTATEIMAGEMASK(m->disabled ? 1 : 2), LVIS_STATEIMAGEMASK);
    LVSet(g_hMList, row, 0, m->disabled ? L"已禁用" : L"启用");
    g_mFilling = FALSE;

    SetWindowTextW(g_hMInfo, m->disabled
        ? L"已禁用 · 重新打开右键菜单即可看到效果"
        : L"已启用 · 重新打开右键菜单即可看到效果");
}

static void OnMenuDelete(void)
{
    int row = ListView_GetNextItem(g_hMList, -1, LVNI_SELECTED);
    MenuItem *m;
    WCHAR msg[512], dir[MAX_PL], file[MAX_PL], safe[128], stamp[64];
    SYSTEMTIME st;

    if (row < 0 || row >= g_menuCount) {
        MessageBoxW(g_hMwnd, L"先选中一行。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    m = &g_menu[row];

    swprintf_s(msg, _countof(msg),
        L"彻底删除这一项右键菜单？\n\n%s\n范围: %s\n位置: %s\n\n"
        L"删除后注册表项就没了, 会先备份成 .hiv。\n"
        L"如果只是想临时关掉, 用勾选框禁用就够了。",
        m->name, m->scope, m->path);

    if (MessageBoxW(g_hMwnd, msg, L"删除右键菜单项",
                    MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    GetLocalTime(&st);
    swprintf_s(stamp, _countof(stamp), L"%04d%02d%02d_%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    swprintf_s(dir, _countof(dir), L"%s\\backup\\menu", g_selfDir);
    CreateDirectoryW(dir, NULL);
    swprintf_s(dir, _countof(dir), L"%s\\backup\\menu\\%s", g_selfDir, stamp);
    CreateDirectoryW(dir, NULL);

    SanitizeFileName(m->name, safe, _countof(safe));
    swprintf_s(file, _countof(file), L"%s\\%s.hiv", dir, safe);
    if (BackupRegKey(HKEY_CLASSES_ROOT, m->path, file))
        Log(L"[右键菜单] 已备份: %s", file);
    else
        Log(L"[右键菜单] [警告] 备份失败, 仍继续删除");

    if (RegDeleteTreeW(HKEY_CLASSES_ROOT, m->path) == ERROR_SUCCESS) {
        Log(L"[右键菜单] 已删除: %s", m->name);
        ScanAllMenus();
        RefreshMenuList();
    } else {
        MessageBoxW(g_hMwnd, L"删除失败: 权限不足。\n\n请用管理员身份运行。",
                    L"失败", MB_OK | MB_ICONERROR);
    }
}

static void CreateMenuControls(HWND hwnd)
{
    static const WCHAR *btn[] = {
        L"刷新", L"切换启用", L"删除选中", L"打开备份目录"
    };
    static const int btnId[] = {
        ID_M_REFRESH, ID_M_TOGGLE, ID_M_DELETE, ID_M_BACKUP
    };
    int i;

    g_hMList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, hwnd, (HMENU)ID_M_LIST, NULL, NULL);

    ListView_SetExtendedListViewStyle(g_hMList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);

    {
        static const WCHAR *heads[] = {
            L"状态", L"菜单名称", L"出现位置", L"类型", L"来源", L"注册表路径"
        };
        static const int widths[] = { 80, 260, 100, 60, 70, 520 };
        int c;
        for (c = 0; c < 6; c++) {
            LVCOLUMNW col;
            ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = (LPWSTR)heads[c];
            col.cx = widths[c];
            col.iSubItem = c;
            SendMessageW(g_hMList, LVM_INSERTCOLUMNW, c, (LPARAM)&col);
        }
    }

    for (i = 0; i < 4; i++) {
        CreateWindowExW(0, L"BUTTON", btn[i],
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            8 + i * 120, 0, 112, 28, hwnd, (HMENU)(INT_PTR)btnId[i], NULL, NULL);
    }

    g_hMInfo = CreateWindowExW(0, L"STATIC", L"就绪",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        0, 0, 100, 20, hwnd, (HMENU)ID_M_INFO, NULL, NULL);

    {
        HWND kids[] = { g_hMList, g_hMInfo };
        int k;
        for (k = 0; k < 2; k++)
            SendMessageW(kids[k], WM_SETFONT, (WPARAM)g_hFont, TRUE);
        for (i = 0; i < 4; i++) {
            HWND b = GetDlgItem(hwnd, btnId[i]);
            if (b) SendMessageW(b, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        }
    }
}

static void LayoutMenuControls(HWND hwnd)
{
    RECT rc;
    int w, h, barY;

    GetClientRect(hwnd, &rc);
    w = rc.right;
    h = rc.bottom;
    barY = h - 62;
    if (barY < 80) barY = 80;

    MoveWindow(g_hMList, 8, 8, w - 16, barY - 16, TRUE);

    {
        int i;
        for (i = 0; i < 4; i++) {
            HWND b = GetDlgItem(hwnd, ID_M_REFRESH + i);
            if (b) MoveWindow(b, 8 + i * 120, barY, 112, 28, TRUE);
        }
    }
    MoveWindow(g_hMInfo, 8, barY + 32, w - 16, 20, TRUE);
}

static LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hMwnd = hwnd;
        CreateMenuControls(hwnd);
        LayoutMenuControls(hwnd);
        ScanAllMenus();
        RefreshMenuList();
        return 0;

    case WM_SIZE:
        LayoutMenuControls(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = 780;
        mm->ptMinTrackSize.y = 420;
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == ID_M_LIST && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nl = (NMLISTVIEW *)lp;
            if (!g_mFilling && (nl->uChanged & LVIF_STATE)) {
                UINT was = (nl->uOldState & LVIS_STATEIMAGEMASK) >> 12;
                UINT now = (nl->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                if (was && now && was != now)
                    OnMenuToggle(nl->iItem);
            }
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_M_REFRESH:
            ScanAllMenus();
            RefreshMenuList();
            return 0;
        case ID_M_TOGGLE:
            OnMenuToggle(ListView_GetNextItem(g_hMList, -1, LVNI_SELECTED));
            return 0;
        case ID_M_DELETE:
            OnMenuDelete();
            return 0;
        case ID_M_BACKUP: {
            WCHAR dir[MAX_PL];
            swprintf_s(dir, _countof(dir), L"%s\\backup\\menu", g_selfDir);
            CreateDirectoryW(dir, NULL);
            ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_hMwnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenMenuWindow(HWND owner)
{
    if (g_hMwnd && IsWindow(g_hMwnd)) {
        ShowWindow(g_hMwnd, SW_RESTORE);
        SetForegroundWindow(g_hMwnd);
        return;
    }

    g_hMwnd = CreateWindowExW(0, L"SysCleanMenuWnd", L"SysClean - 右键菜单管理",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 700,
        owner, NULL, (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE), NULL);
    if (!g_hMwnd) return;
    ShowWindow(g_hMwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hMwnd);
}

/* ============================================================
 * 进程与线程窗口
 * ============================================================ */

static BOOL g_tFilling;

static void LVSet(HWND lv, int row, int col, LPCWSTR text)
{
    LVITEMW sub;
    ZeroMemory(&sub, sizeof(sub));
    sub.iSubItem = col;
    sub.pszText = (LPWSTR)text;
    SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&sub);
}

static void FmtBytes(SIZE_T b, LPWSTR out, size_t cch)
{
    if (b >= (SIZE_T)1073741824)
        swprintf_s(out, cch, L"%.1f GB", (double)b / 1073741824.0);
    else if (b >= (SIZE_T)1048576)
        swprintf_s(out, cch, L"%.1f MB", (double)b / 1048576.0);
    else if (b >= (SIZE_T)1024)
        swprintf_s(out, cch, L"%.0f KB", (double)b / 1024.0);
    else
        swprintf_s(out, cch, L"%I64u B", (unsigned __int64)b);
}

static void FmtCpu(ULONGLONG t100ns, LPWSTR out, size_t cch)
{
    double s = (double)t100ns / 10000000.0;
    if (s >= 3600.0)
        swprintf_s(out, cch, L"%d:%02d:%02d", (int)(s / 3600), ((int)(s / 60)) % 60, ((int)s) % 60);
    else if (s >= 60.0)
        swprintf_s(out, cch, L"%d:%04.1f", (int)(s / 60), s - (double)((int)(s / 60)) * 60.0);
    else
        swprintf_s(out, cch, L"%.2f s", s);
}

static void RefreshProcList(void)
{
    int i;
    WCHAR buf[64], buf2[64];
    DWORD keep = g_selPid;
    int selRow = -1;

    if (!g_hTProc) return;
    g_tFilling = TRUE;
    SendMessageW(g_hTProc, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hTProc);

    for (i = 0; i < g_prowCount; i++) {
        ProcRow *p = &g_prows[i];
        LVITEMW it;
        int row;

        swprintf_s(buf, _countof(buf), L"%lu", p->pid);
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = buf;
        it.lParam = (LPARAM)p->pid;
        row = (int)SendMessageW(g_hTProc, LVM_INSERTITEMW, 0, (LPARAM)&it);

        LVSet(g_hTProc, row, 1, p->name);
        swprintf_s(buf, _countof(buf), L"%lu",
                   p->session == 0xFFFFFFFF ? 0 : p->session);
        LVSet(g_hTProc, row, 2, buf);
        swprintf_s(buf, _countof(buf), L"%lu", p->threadCount);
        LVSet(g_hTProc, row, 3, buf);
        swprintf_s(buf, _countof(buf), L"%lu", p->handleCount);
        LVSet(g_hTProc, row, 4, buf);
        FmtBytes(p->memBytes, buf, _countof(buf));
        LVSet(g_hTProc, row, 5, buf);
        FmtCpu(p->cpu100ns, buf2, _countof(buf2));
        LVSet(g_hTProc, row, 6, buf2);
        LVSet(g_hTProc, row, 7, p->path[0] ? p->path : L"(无法读取)");

        if (keep && p->pid == keep) selRow = row;
    }

    SendMessageW(g_hTProc, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hTProc, NULL, TRUE);

    if (selRow >= 0) {
        ListView_SetItemState(g_hTProc, selRow, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_hTProc, selRow, FALSE);
    } else if (g_prowCount > 0) {
        /* 默认跳过 System / Idle —— 它们不可访问, 列出来也定位不了 */
        int pick = 0, k;
        for (k = 0; k < g_prowCount; k++) {
            if (g_prows[k].pid != 0 && g_prows[k].pid != 4) { pick = k; break; }
        }
        ListView_SetItemState(g_hTProc, pick, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_hTProc, pick, FALSE);
        g_selPid = g_prows[pick].pid;
    }
    g_tFilling = FALSE;
}

static void RefreshThreadList(void)
{
    int i;
    WCHAR addr[32], off[32], cpu[32], prio[16];
    ULONGLONG total = 0;
    int matched = 0;

    if (!g_hTThread) return;
    SendMessageW(g_hTThread, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hTThread);

    for (i = 0; i < g_trowCount; i++) {
        ThreadRow *t = &g_trows[i];
        LVITEMW it;
        int row;
        WCHAR tid[24];

        FormatAddr(t->start, addr, _countof(addr));
        if (t->hasMod) {
            FormatOffset(t->modOffset, off, _countof(off));
            matched++;
        } else {
            wcsncpy_s(off, _countof(off), L"-", _TRUNCATE);
        }
        swprintf_s(cpu, _countof(cpu), L"%.2f s", (t->user100ns + t->kernel100ns) / 10000000.0);
        swprintf_s(prio, _countof(prio), L"%d", t->prio);
        total += t->user100ns + t->kernel100ns;

        swprintf_s(tid, _countof(tid), L"%lu", t->tid);
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = tid;
        it.lParam = (LPARAM)t->tid;
        row = (int)SendMessageW(g_hTThread, LVM_INSERTITEMW, 0, (LPARAM)&it);

        LVSet(g_hTThread, row, 1, addr);
        LVSet(g_hTThread, row, 2, t->hasMod ? t->module : L"(未知模块)");
        LVSet(g_hTThread, row, 3, off);
        LVSet(g_hTThread, row, 4, prio);
        LVSet(g_hTThread, row, 5, cpu);
        LVSet(g_hTThread, row, 6, t->modPath[0] ? t->modPath : L"-");
    }

    SendMessageW(g_hTThread, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hTThread, NULL, TRUE);

    if (g_hTInfo) {
        WCHAR info[512];
        WCHAR c[64];
        FmtCpu(total, c, _countof(c));
        if (g_isWow64 && g_modCount == 0 && g_trowCount > 0) {
            swprintf_s(info, _countof(info),
                L"[受限] 当前是 32 位版本, 在 64 位系统上读不到进程的模块表, "
                L"线程无法定位 —— 请改用同目录的 SysClean.exe (64 位版)");
        } else {
            swprintf_s(info, _countof(info),
                L"PID %lu · 线程 %d 个 · 已定位模块 %d 个 · 累计 CPU %s",
                g_selPid, g_trowCount, matched, c);
        }
        SetWindowTextW(g_hTInfo, info);
    }
}

static void OnProcSelChange(int row)
{
    LVITEMW it;
    DWORD pid = 0;

    if (g_tFilling || row < 0) return;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = row;
    if (!SendMessageW(g_hTProc, LVM_GETITEMW, 0, (LPARAM)&it)) return;

    pid = (DWORD)it.lParam;
    if (pid == g_selPid && g_trowCount > 0) return;

    g_selPid = pid;
    LoadThreadsFor(pid);
    RefreshThreadList();
}

static DWORD GetSelPid(void)
{
    int row = ListView_GetNextItem(g_hTProc, -1, LVNI_SELECTED);
    LVITEMW it;
    if (row < 0) return 0;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = row;
    if (!SendMessageW(g_hTProc, LVM_GETITEMW, 0, (LPARAM)&it)) return 0;
    return (DWORD)it.lParam;
}

static int GetSelThreadRow(void)
{
    return ListView_GetNextItem(g_hTThread, -1, LVNI_SELECTED);
}

static void OnRevealThread(void)
{
    int row = GetSelThreadRow();
    ThreadRow *t;

    if (row < 0 || row >= g_trowCount) {
        MessageBoxW(g_hTwnd, L"先在下面的线程列表里选中一个线程。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    t = &g_trows[row];
    RevealInExplorer(t->modPath);
}

static void OnRevealProc(void)
{
    DWORD pid = GetSelPid();
    int i;

    for (i = 0; i < g_prowCount; i++) {
        if (g_prows[i].pid == pid) {
            RevealInExplorer(g_prows[i].path);
            return;
        }
    }
}

static void OnCopyThread(void)
{
    int row = GetSelThreadRow();
    ThreadRow *t;
    WCHAR buf[MAX_PL + 128];
    WCHAR off[32];
    HGLOBAL hg;
    LPVOID dst;
    size_t len;

    if (row < 0 || row >= g_trowCount) return;
    t = &g_trows[row];

    if (t->hasMod) {
        FormatOffset(t->modOffset, off, _countof(off));
        swprintf_s(buf, _countof(buf), L"TID %lu  %s+%s  %s",
                   t->tid, t->module, off, t->modPath);
    } else {
        WCHAR addr[32];
        FormatAddr(t->start, addr, _countof(addr));
        swprintf_s(buf, _countof(buf), L"TID %lu  %s  (模块未知)", t->tid, addr);
    }

    len = (wcslen(buf) + 1) * sizeof(WCHAR);
    hg = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hg) return;
    dst = GlobalLock(hg);
    if (dst) {
        memcpy(dst, buf, len);
        GlobalUnlock(hg);
        if (OpenClipboard(g_hTwnd)) {
            EmptyClipboard();
            SetClipboardData(CF_UNICODETEXT, hg);
            CloseClipboard();
            SetWindowTextW(g_hTInfo, L"已复制到剪贴板");
            return;
        }
    }
    GlobalFree(hg);
}

static void OnKillThread(void)
{
    int row = GetSelThreadRow();
    ThreadRow *t;
    WCHAR msg[512];

    if (row < 0 || row >= g_trowCount) {
        MessageBoxW(g_hTwnd, L"先在下面的线程列表里选中一个线程。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    t = &g_trows[row];
    if (t->tid == GetCurrentThreadId()) {
        MessageBoxW(g_hTwnd, L"不能结束本程序自己的主线程。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    swprintf_s(msg, _countof(msg),
        L"结束线程 TID %lu (%s) ?\n\n"
        L"TerminateThread 不会让目标线程释放它持有的锁和内存, "
        L"被结束的进程可能卡死或崩溃。\n\n"
        L"只建议用于已经无响应的进程。确定继续？",
        t->tid, t->hasMod ? t->module : L"未知模块");

    if (MessageBoxW(g_hTwnd, msg, L"结束线程", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    if (KillThreadById(t->tid)) {
        SetWindowTextW(g_hTInfo, L"线程已结束");
        Sleep(200);
        LoadThreadsFor(g_selPid);
        RefreshThreadList();
    } else {
        MessageBoxW(g_hTwnd, L"结束失败: 权限不足或线程已退出。", L"失败",
                    MB_OK | MB_ICONERROR);
    }
}

static void TRefreshAll(void)
{
    DWORD keep = g_selPid ? g_selPid : GetSelPid();
    g_selPid = keep;
    LoadProcRows();
    RefreshProcList();
    if (g_selPid) {
        LoadThreadsFor(g_selPid);
        RefreshThreadList();
    }
}

static void CreateThreadControls(HWND hwnd)
{
    static const WCHAR *btn[] = {
        L"刷新", L"定位线程模块", L"定位进程文件", L"结束线程", L"复制信息"
    };
    static const int btnId[] = {
        ID_T_REFRESH, ID_T_REVEAL, ID_T_REVEALP, ID_T_KILLTHR, ID_T_COPY
    };
    int i;

    g_hTProc = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, hwnd, (HMENU)ID_T_PROC, NULL, NULL);

    g_hTThread = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, hwnd, (HMENU)ID_T_THREAD, NULL, NULL);

    ListView_SetExtendedListViewStyle(g_hTProc,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    ListView_SetExtendedListViewStyle(g_hTThread,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    {
        static const WCHAR *heads[] = {
            L"PID", L"进程名", L"会话", L"线程", L"句柄", L"内存", L"CPU 时间", L"可执行文件路径"
        };
        static const int widths[] = { 70, 180, 50, 55, 55, 85, 85, 560 };
        int c;
        for (c = 0; c < 8; c++) {
            LVCOLUMNW col;
            ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = (LPWSTR)heads[c];
            col.cx = widths[c];
            col.iSubItem = c;
            SendMessageW(g_hTProc, LVM_INSERTCOLUMNW, c, (LPARAM)&col);
        }
    }
    {
        static const WCHAR *heads[] = {
            L"TID", L"起始地址", L"所属模块", L"模块内偏移", L"优先级", L"CPU 时间", L"模块磁盘路径"
        };
        static const int widths[] = { 80, 150, 150, 110, 55, 80, 520 };
        int c;
        for (c = 0; c < 7; c++) {
            LVCOLUMNW col;
            ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = (LPWSTR)heads[c];
            col.cx = widths[c];
            col.iSubItem = c;
            SendMessageW(g_hTThread, LVM_INSERTCOLUMNW, c, (LPARAM)&col);
        }
    }

    for (i = 0; i < 5; i++) {
        CreateWindowExW(0, L"BUTTON", btn[i],
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            8 + i * 120, 0, 112, 28, hwnd, (HMENU)(INT_PTR)btnId[i], NULL, NULL);
    }

    g_hTInfo = CreateWindowExW(0, L"STATIC", L"就绪",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        0, 0, 100, 20, hwnd, (HMENU)ID_T_INFO, NULL, NULL);

    {
        HWND kids[] = { g_hTProc, g_hTThread, g_hTInfo };
        int k;
        for (k = 0; k < 3; k++)
            SendMessageW(kids[k], WM_SETFONT, (WPARAM)g_hFont, TRUE);
        for (i = 0; i < 5; i++) {
            HWND b = GetDlgItem(hwnd, btnId[i]);
            if (b) SendMessageW(b, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        }
    }
}

static void LayoutThreadControls(HWND hwnd)
{
    RECT rc;
    int w, h, barY, procH, thrH;

    GetClientRect(hwnd, &rc);
    w = rc.right;
    h = rc.bottom;

    barY = h - 62;
    if (barY < 100) barY = 100;

    procH = (barY - 16) * 42 / 100;
    if (procH < 80) procH = 80;
    thrH = barY - 8 - procH - 4;
    if (thrH < 80) thrH = 80;

    MoveWindow(g_hTProc, 8, 8, w - 16, procH, TRUE);
    MoveWindow(g_hTThread, 8, 8 + procH + 4, w - 16, thrH, TRUE);

    {
        int i;
        for (i = 0; i < 5; i++) {
            HWND b = GetDlgItem(hwnd, ID_T_REFRESH + i);
            if (b) MoveWindow(b, 8 + i * 120, barY, 112, 28, TRUE);
        }
    }
    MoveWindow(g_hTInfo, 8, barY + 32, w - 16, 20, TRUE);
}

static LRESULT CALLBACK ThreadWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hTwnd = hwnd;
        CreateThreadControls(hwnd);
        LayoutThreadControls(hwnd);
        TRefreshAll();
        return 0;

    case WM_SIZE:
        LayoutThreadControls(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = 760;
        mm->ptMinTrackSize.y = 460;
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == ID_T_PROC && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nl = (NMLISTVIEW *)lp;
            if ((nl->uNewState & LVIS_SELECTED) && !(nl->uOldState & LVIS_SELECTED))
                OnProcSelChange(nl->iItem);
        } else if (nh->idFrom == ID_T_PROC && nh->code == NM_DBLCLK) {
            OnRevealProc();
        } else if (nh->idFrom == ID_T_THREAD && nh->code == NM_DBLCLK) {
            OnRevealThread();
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_T_REFRESH: TRefreshAll();    return 0;
        case ID_T_REVEAL:  OnRevealThread(); return 0;
        case ID_T_REVEALP: OnRevealProc();   return 0;
        case ID_T_KILLTHR: OnKillThread();   return 0;
        case ID_T_COPY:    OnCopyThread();   return 0;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_hTwnd = NULL;
        g_twndOpen = FALSE;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenThreadWindow(HWND owner)
{
    if (g_hTwnd && IsWindow(g_hTwnd)) {
        ShowWindow(g_hTwnd, SW_RESTORE);
        SetForegroundWindow(g_hTwnd);
        return;
    }

    g_twndOpen = TRUE;
    g_hTwnd = CreateWindowExW(0, L"SysCleanThreadWnd", L"SysClean - 进程与线程",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 760,
        owner, NULL, (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE), NULL);
    if (!g_hTwnd) { g_twndOpen = FALSE; return; }
    ShowWindow(g_hTwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hTwnd);
}

/* ============================================================
 * 界面
 * ============================================================ */

static void CreateControls(HWND hwnd)
{
    static const WCHAR *btnText[] = {
        L"重新扫描", L"一键关闭进程", L"卸载选中项",
        L"打开备份目录", L"进程与线程", L"右键菜单"
    };
    int i;
    WNDCLASSEXW wc;

    g_hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

    for (i = 0; i < 6; i++) {
        CreateWindowExW(0, L"BUTTON", btnText[i],
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10 + i * 116, 8, 108, 30, hwnd, (HMENU)(INT_PTR)(ID_BTN_SCAN + i),
            NULL, NULL);
    }

    g_hStatus = CreateWindowExW(0, L"STATIC", L"就绪",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        706, 15, 280, 20, hwnd, (HMENU)ID_STATUS, NULL, NULL);

    g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, hwnd, (HMENU)ID_LIST, NULL, NULL);

    ListView_SetExtendedListViewStyle(g_hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES);

    {
        static const WCHAR *heads[] = {
            L"程序名", L"来源", L"发布者", L"版本", L"大小", L"状态", L"安装路径"
        };
        static const int widths[] = { 240, 70, 140, 80, 80, 70, 380 };
        int c;
        for (c = 0; c < 7; c++) {
            LVCOLUMNW col;
            ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = (LPWSTR)heads[c];
            col.cx = widths[c];
            col.iSubItem = c;
            SendMessageW(g_hList, LVM_INSERTCOLUMNW, c, (LPARAM)&col);
        }
    }

    g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 100, 100, hwnd, (HMENU)ID_LOG, NULL, NULL);

    /* 统一字体 */
    {
        HWND kids[] = { g_hList, g_hLog, g_hStatus };
        int k;
        for (k = 0; k < 3; k++)
            SendMessageW(kids[k], WM_SETFONT, (WPARAM)g_hFont, TRUE);
        for (i = 0; i < 6; i++) {
            HWND b = GetDlgItem(hwnd, ID_BTN_SCAN + i);
            if (b) SendMessageW(b, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        }
    }

    (void)wc;
}

static void LayoutControls(HWND hwnd)
{
    RECT rc;
    int w, h, listH;

    GetClientRect(hwnd, &rc);
    w = rc.right;
    h = rc.bottom;
    listH = h - TOP_HEIGHT - LOG_HEIGHT - 8;
    if (listH < 60) listH = 60;

    MoveWindow(g_hList, 8, TOP_HEIGHT, w - 16, listH, TRUE);
    MoveWindow(g_hLog, 8, TOP_HEIGHT + listH + 4, w - 16, LOG_HEIGHT - 8, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hMain = hwnd;
        CreateControls(hwnd);
        LayoutControls(hwnd);
        SetTimer(hwnd, 1, 300, NULL);
        return 0;

    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            if (IsAdmin()) {
                Log(L"以管理员权限运行, 可以清理系统级注册表项。");
            } else {
                Log(L"[警告] 未以管理员身份运行, HKLM 下的注册表项将无法删除。");
                Log(L"       请关闭本程序, 右键选择「以管理员身份运行」。");
            }
            EnablePrivilege(L"SeDebugPrivilege");
            DoScan();
        }
        return 0;

    case WM_SIZE:
        LayoutControls(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_SCAN:   DoScan();      return 0;
        case ID_BTN_KILL:   OnKillAll();   return 0;
        case ID_BTN_UNINST: OnUninstall(); return 0;
        case ID_BTN_THREADS: OpenThreadWindow(hwnd); return 0;
        case ID_BTN_MENU:   OpenMenuWindow(hwnd);   return 0;
        case ID_BTN_BACKUP: {
            WCHAR path[MAX_PATH];
            swprintf_s(path, _countof(path), L"%s\\backup", g_selfDir);
            CreateDirectoryW(path, NULL);
            ShellExecuteW(hwnd, L"open", path, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ============================================================
 * 命令行自检 (只读, 不修改任何东西)
 * ============================================================ */

static void SelfTest(LPCWSTR outFile)
{
    FILE *f;
    int i, running = 0, unsafeDir = 0;

    _wfopen_s(&f, outFile, L"w, ccs=UTF-8");
    if (!f) return;

    SnapshotProcs();
    ScanUninstallKey(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, TRUE);
    ScanUninstallKey(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, FALSE);
    ScanUninstallKey(HKEY_CURRENT_USER,  0,              FALSE);
    /* 先算出注册表程序的 AppData 目录, 再扫 AppData, 才能把数据目录从软件列表里剔除 */
    for (i = 0; i < g_progCount; i++) CollectDataDirs(&g_progs[i]);
    ScanAppData();
    LoadServices();

    for (i = 0; i < g_progCount; i++) {
        g_progs[i].running = IsProgramRunning(&g_progs[i]);
    }

    fwprintf(f, L"processes=%d programs=%d services=%d\n",
             g_procCount, g_progCount, g_svcCount);
    for (i = 0; i < g_progCount; i++) {
        ProgramInfo *p = &g_progs[i];
        BOOL hasDir = p->installDir[0] != 0;
        BOOL safe = hasDir && IsSafeToDelete(p->installDir);
        BOOL run = p->running;

        if (run) running++;
        if (hasDir && !safe) unsafeDir++;
        fwprintf(f, L"[%d] %s | src=%s | pub=%s | ver=%s | size=%luKB | %s | hasDir=%d safe=%d data=%d\n    path=%s\n    reg=%s\n",
            i, p->name, SrcLabel(p->src), p->publisher, p->version, p->sizeKB,
            run ? L"RUNNING" : L"stopped",
            hasDir ? 1 : 0, safe ? 1 : 0, p->dataDirCount,
            p->installDir, p->regPath);

        {
            int d;
            for (d = 0; d < p->dataDirCount; d++) {
                fwprintf(f, L"    data[%d]=%s safe=%d\n", d, p->dataDirs[d],
                         IsSafeToDeleteData(p->dataDirs[d]) ? 1 : 0);
            }
        }

        /* 注册表关联扫描比较慢, 只对前 40 个程序跑 */
        if (i < 40) {
            RegHit hits[MAX_REGHITS];
            int hc = CollectRegHits(p, hits), h;
            fwprintf(f, L"    reghits=%d\n", hc);
            for (h = 0; h < hc; h++) {
                fwprintf(f, L"      <%s> %s%s%s%s\n", hits[h].label,
                         hits[h].root == HKEY_LOCAL_MACHINE ? L"HKLM\\" : L"HKCU\\",
                         hits[h].path,
                         hits[h].isValue ? L" [value] " : L"",
                         hits[h].isValue ? hits[h].valueName : L"");
            }
        }
    }
    fwprintf(f, L"\nsummary: running=%d unsafeDir=%d\n", running, unsafeDir);

    /* 路径守卫自检: 确认危险目录一律被拦 */
    fwprintf(f, L"\n=== path guard self-check ===\n");
    {
        static const struct { LPCWSTR p; BOOL expect; } cases[] = {
            { L"C:\\",                                          FALSE },
            { L"C:\\Windows",                                   FALSE },
            { L"C:\\Windows\\System32",                         FALSE },
            { L"C:\\Program Files",                             FALSE },
            { L"C:\\Program Files (x86)",                       FALSE },
            { L"C:\\Program Files\\Common Files",               FALSE },
            { L"C:\\ProgramData",                               FALSE },
            { L"C:\\Users",                                     FALSE },
            { L"C:\\abc",                                       FALSE },
            { L"",                                              FALSE },
            { L"C:\\Program Files\\SomeApp",                    TRUE  },
            { L"D:\\Program Files\\Clash Verge",                TRUE  },
            { L"C:\\Users\\me\\AppData\\Local\\Programs\\Yuque", TRUE  },
            { L"C:\\Users\\me\\AppData\\Local\\Programs",       FALSE },
            { L"C:\\Users\\me\\AppData\\Local",                 FALSE },
            { L"C:\\Users\\me\\AppData",                        FALSE },
        };
        int k;
        int pass = 0;
        for (k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
            BOOL got = IsSafeToDelete(cases[k].p);
            BOOL ok = (got == cases[k].expect);
            if (ok) pass++;
            fwprintf(f, L"%s expect=%d got=%d  %s\n",
                ok ? L"OK  " : L"FAIL", cases[k].expect, got,
                cases[k].p[0] ? cases[k].p : L"(empty)");
        }
        fwprintf(f, L"path guard: %d/%d passed\n",
            pass, (int)(sizeof(cases) / sizeof(cases[0])));
    }

    /* 数据目录守卫: 只允许 AppData 之下, 且不能是 AppData 本身, 也不能是名字相近的兄弟目录 */
    fwprintf(f, L"\n=== data dir guard self-check ===\n");
    {
        WCHAR local[MAX_PATH] = L"", roaming[MAX_PATH] = L"";
        WCHAR t1[MAX_PL], t2[MAX_PL], t3[MAX_PL];
        int pass2 = 0, total2 = 0;

        SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local);
        SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, roaming);

        swprintf_s(t1, _countof(t1), L"%s\\SomeApp", local);
        swprintf_s(t2, _countof(t2), L"%sX\\SomeApp", local);      /* 名字相近的兄弟目录 */
        swprintf_s(t3, _countof(t3), L"%s\\SomeApp", roaming);

#define DTEST(path, expect) do {                                        \
            BOOL got = IsSafeToDeleteData(path);                        \
            BOOL ok = (got == (expect));                                \
            total2++; if (ok) pass2++;                                  \
            fwprintf(f, L"%s expect=%d got=%d  %s\n",                   \
                     ok ? L"OK  " : L"FAIL", (int)(expect), got, path); \
        } while (0)

        DTEST(t1, TRUE);
        DTEST(t2, FALSE);
        DTEST(t3, TRUE);
        DTEST(local, FALSE);
        DTEST(roaming, FALSE);
        DTEST(L"C:\\Program Files\\SomeApp", FALSE);
        DTEST(L"C:\\Windows\\Temp", FALSE);
        DTEST(L"C:\\Users\\Public\\Documents", FALSE);
#undef DTEST

        fwprintf(f, L"data guard: %d/%d passed\n", pass2, total2);
    }

    /* 注册表路径守卫: expect=1 表示"危险, 必须拦" */
    fwprintf(f, L"\n=== reg path guard self-check ===\n");
    {
        static const struct { LPCWSTR p; BOOL danger; } rc[] = {
            { L"SOFTWARE",                                            TRUE  },
            { L"SOFTWARE\\Microsoft",                                 TRUE  },
            { L"SOFTWARE\\Microsoft\\Windows",                        TRUE  },
            { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",        TRUE  },
            { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",   TRUE  },
            { L"SOFTWARE\\Classes",                                   TRUE  },
            { L"SOFTWARE\\Policies",                                  TRUE  },
            { L"SOFTWARE\\Wow6432Node",                               TRUE  },
            { L"SYSTEM",                                              TRUE  },
            { L"SYSTEM\\CurrentControlSet\\Services",                 TRUE  },
            { L"SOFTWARE\\Tencent",                                   FALSE },
            { L"SOFTWARE\\Everything",                                FALSE },
            { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Weixin", FALSE },
            { L"SYSTEM\\CurrentControlSet\\Services\\Everything",     FALSE },
        };
        int k, pass3 = 0;
        for (k = 0; k < (int)(sizeof(rc) / sizeof(rc[0])); k++) {
            BOOL got = IsDangerousRegPath(rc[k].p);
            BOOL ok = (got == rc[k].danger);
            if (ok) pass3++;
            fwprintf(f, L"%s danger=%d got=%d  %s\n",
                     ok ? L"OK  " : L"FAIL", rc[k].danger, got, rc[k].p);
        }
        fwprintf(f, L"reg guard: %d/%d passed\n",
                 pass3, (int)(sizeof(rc) / sizeof(rc[0])));
    }

    fclose(f);
}

/* 线程枚举自检: 只读, 把进程/线程/模块反查结果写成文本 */
static void ThreadSelfTest(LPCWSTR outFile)
{
    FILE *f;
    int i, totalThreads = 0, located = 0, totalMods = 0;

    _wfopen_s(&f, outFile, L"w, ccs=UTF-8");
    if (!f) return;

    LoadProcRows();
    fwprintf(f, L"processes=%d\n\n", g_prowCount);

    for (i = 0; i < g_prowCount && i < 40; i++) {
        ProcRow *p = &g_prows[i];
        fwprintf(f, L"[%d] pid=%lu name=%s sess=%lu thr=%lu hnd=%lu mem=%I64uKB cpu=%.2fs\n    path=%s\n",
            i, p->pid, p->name, p->session, p->threadCount, p->handleCount,
            (unsigned __int64)(p->memBytes / 1024), p->cpu100ns / 10000000.0,
            p->path[0] ? p->path : L"(unreadable)");
    }

    for (i = 0; i < g_prowCount && i < 25; i++) {
        int j;
        LoadThreadsFor(g_prows[i].pid);
        totalThreads += g_trowCount;
        totalMods += g_modCount;
        fwprintf(f, L"\n== %s (pid %lu) threads=%d modules=%d ==\n",
            g_prows[i].name, g_prows[i].pid, g_trowCount, g_modCount);
        for (j = 0; j < g_trowCount && j < 8; j++) {
            ThreadRow *t = &g_trows[j];
            WCHAR a[32], o[32];
            FormatAddr(t->start, a, _countof(a));
            FormatOffset(t->modOffset, o, _countof(o));
            if (t->hasMod) located++;
            fwprintf(f, L"  tid=%lu start=%s mod=%s off=%s prio=%d cpu=%.2fs\n    path=%s\n",
                t->tid, a, t->hasMod ? t->module : L"?", o, t->prio,
                (t->user100ns + t->kernel100ns) / 10000000.0,
                t->modPath[0] ? t->modPath : L"-");
        }
    }
    fwprintf(f, L"\nsummary: threads=%d located=%d modules=%d ptr=%d\n",
        totalThreads, located, totalMods, (int)sizeof(ULONG_PTR));
    fclose(f);
}

/* ============================================================
 * 入口
 * ============================================================ */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int nShow)
{
    WNDCLASSEXW wc;
    MSG msg;
    HWND hwnd;
    INITCOMMONCONTROLSEX icc;

    (void)hPrev;
    (void)cmdLine;

    GetModuleFileNameW(NULL, g_selfDir, MAX_PATH);
    PathRemoveFileSpecW(g_selfDir);
    InitNtQuery();          /* 自检分支也要用, 必须放在命令行处理之前 */
    {
        BOOL wow = FALSE;
        if (IsWow64Process(GetCurrentProcess(), &wow)) g_isWow64 = wow;
    }

    /* 自检模式: SysClean.exe /selftest */
    {
        LPWSTR cl = GetCommandLineW();
        if (wcsstr(cl, L"/selftest")) {
            WCHAR out[MAX_PATH];
            swprintf_s(out, _countof(out), L"%s\\selftest.txt", g_selfDir);
            SelfTest(out);
            return 0;
        }
        /* 线程枚举自检: SysClean.exe /threadtest */
        if (wcsstr(cl, L"/threadtest")) {
            WCHAR out[MAX_PATH];
            swprintf_s(out, _countof(out), L"%s\\threadtest.txt", g_selfDir);
            ThreadSelfTest(out);
            return 0;
        }
        /* 删除逻辑隔离测试: SysClean.exe /testdelete "D:\some\temp\dir" */        if (wcsstr(cl, L"/testdelete")) {
            int argc = 0;
            LPWSTR *argv = CommandLineToArgvW(cl, &argc);
            if (argv && argc >= 3) {
                WCHAR out[MAX_PATH];
                WCHAR root[MAX_PL], pat[MAX_PL];
                WIN32_FIND_DATAW fd;
                HANDLE h;
                DWORD e;

                swprintf_s(out, _countof(out), L"%s\\testdelete.txt", g_selfDir);
                g_testLog = _wfsopen(out, L"w, ccs=UTF-8", _SH_DENYNO);
                g_noKillTest = (wcsstr(cl, L"/nokill") != NULL);

                ToLongPath(argv[argc - 1], root, _countof(root));
                swprintf_s(pat, MAX_PL, L"%s\\*", root);
                h = FindFirstFileW(pat, &fd);
                e = GetLastError();
                Log(L"arg  = %s", argv[argc - 1]);
                Log(L"root = %s", root);
                Log(L"handle=%p err=%lu", h, e);
                if (h != INVALID_HANDLE_VALUE) FindClose(h);

                DeleteDirectory(argv[argc - 1]);

                Log(L"deleted=%d failed=%d killed=%d pendingReboot=%d",
                    g_delFiles, g_delFail, g_killedLockers, g_pendingReboot);

                if (g_testLog) { fclose(g_testLog); g_testLog = NULL; }
            }
            if (argv) LocalFree(argv);
            return 0;
        }
        /* 右键菜单扫描自检: SysClean.exe /menutest */
        if (wcsstr(cl, L"/menutest")) {
            WCHAR out[MAX_PATH];
            swprintf_s(out, _countof(out), L"%s\\menutest.txt", g_selfDir);
            MenuSelfTest(out);
            return 0;
        }
        /* 右键菜单开关自检: SysClean.exe /menutoggle "Directory\\shell\\cmd" */
        if (wcsstr(cl, L"/menutoggle")) {
            int argc = 0;
            LPWSTR *argv = CommandLineToArgvW(cl, &argc);
            if (argv && argc >= 3) {
                WCHAR out[MAX_PATH];
                swprintf_s(out, _countof(out), L"%s\\menutoggle.txt", g_selfDir);
                MenuToggleTest(out, argv[argc - 1]);
            }
            if (argv) LocalFree(argv);
            return 0;
        }
    }

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.lpszClassName = L"SysCleanWnd";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&wc)) return 0;

    /* 进程与线程窗口的类 */
    wc.lpfnWndProc = ThreadWndProc;
    wc.lpszClassName = L"SysCleanThreadWnd";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    /* 右键菜单管理窗口的类 */
    wc.lpfnWndProc = MenuWndProc;
    wc.lpszClassName = L"SysCleanMenuWnd";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    InitNtQuery();

    hwnd = CreateWindowExW(0, L"SysCleanWnd",
        L"SysClean - 进程清理 / 强制卸载 / 线程定位 / 右键菜单",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 680,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 0;

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
