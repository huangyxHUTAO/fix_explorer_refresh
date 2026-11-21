/*
 * fixwatch.exe -- 让资源管理器在目录内容变化后重新排序
 *
 * 原理：shell 的增量插入（CDefCollection::_InsertItemInView）恒追加、不排序，
 * 只有整目录重新枚举才会重排；而那次枚举靠 SHCNE_UPDATEDIR 触发，系统不保证发。
 * 本程序自己补这条通知（消抖 400ms）。
 *
 * 用法：
 *   fixwatch.exe                 自动跟踪所有打开的 Explorer 文件夹（含标签页）
 *   fixwatch.exe "D:\x" "D:\y"   额外加上这些目录
 * 日志：E:/kimi-code-temp/_re/fixwatch.log（带时间戳）
 * 名单：E:/kimi-code-temp/_re/watchlist.txt（可选，一行一个目录，运行时热加载）
 */
#define _WIN32_WINNT 0x0601
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <io.h>
#include <wchar.h>
#include <tlhelp32.h>

#define POLL_MS        100
#define DEBOUNCE_MS    400
#define RESCAN_MS      2000
#define MAXLOG_ITEMS   20
#define SRC_CMD        1
#define SRC_LIST       2
#define SRC_AUTO       4

static const GUID g_clsShellWindows = {0x9BA05972,0xF6A8,0x11CF,{0xA4,0x42,0x00,0xA0,0xC9,0x0A,0x8F,0x39}};
static const GUID g_iidShellWindows = {0x85CB6900,0x4D95,0x11CF,{0x96,0x0C,0x00,0x80,0xC7,0xF4,0xEE,0x85}};
static const GUID g_iidWebBrowser2  = {0xD30C1661,0xCDAF,0x11D0,{0x8A,0x3E,0x00,0xC0,0x4F,0xC9,0xE2,0x6E}};

typedef struct { DWORD hash; WCHAR *name; } ENTRY;

typedef struct {
    WCHAR            path[MAX_PATH];
    PIDLIST_ABSOLUTE pidl;
    ENTRY           *e;
    DWORD            ne, cap;
    DWORD            pendingDiff, pendingFrom, lastChange;
    int              fromCmd;
    int              fromList;
    int              fromAuto;
    int              scanned;
    DWORD            lastErr;
    int              seen;
} DIRW;

static WCHAR  g_dir[MAX_PATH] = L".";
static WCHAR  g_base[MAX_PATH] = L"app";
static WCHAR  g_logPath[MAX_PATH * 2];
static WCHAR  g_listPath[MAX_PATH * 2];
static DIRW  *g_d = NULL;
static DWORD  g_nd = 0, g_cap = 0;
static FILE  *g_log = NULL;
static int    g_useLog = 0;
static int    g_noAuto = 0;

/* ---------------- 日志 ---------------- */

static void stamp(WCHAR *buf, int cch)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(buf, cch, L"%02u:%02u:%02u.%03u",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static void init_paths(void)
{
    WCHAR buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        WCHAR *p = wcsrchr(buf, 92);
        if (p) {
            wcsncpy(g_base, p + 1, MAX_PATH - 1);
            *p = 0;
        }
        wcsncpy(g_dir, buf, MAX_PATH - 1);
        {
            WCHAR *dot = wcsrchr(g_base, 46);
            if (dot) *dot = 0;
        }
    }
    swprintf(g_logPath, MAX_PATH * 2, L"%ls/%ls.log", g_dir, g_base);
    swprintf(g_listPath, MAX_PATH * 2, L"%ls/watchlist.txt", g_dir);
}

static void L(const WCHAR *fmt, ...)
{
    WCHAR ts[32], msg[2048], line[2200];
    char  u8[4400];
    va_list ap;
    int cb;
    stamp(ts, 32);
    va_start(ap, fmt);
    vswprintf(msg, 2048, fmt, ap);
    va_end(ap);
    swprintf(line, 2200, L"[%ls] %ls", ts, msg);
    cb = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, (int)sizeof(u8), NULL, NULL);
    if (cb > 0) { fwrite(u8, 1, (size_t)(cb - 1), stdout); fputc(10, stdout); fflush(stdout); }
    if (g_log) { fwprintf(g_log, L"%ls", line); fputwc(10, g_log); fflush(g_log); }
}

/* ---------------- 基础 ---------------- */

static DWORD hash_name(const WCHAR *s)
{
    DWORD h = 2166136261u;
    while (*s) {
        WCHAR c = *s++;
        if (c >= L'A' && c <= L'Z') c += 32;
        h ^= (DWORD)c;
        h *= 16777619u;
    }
    return h;
}

static void normalize(WCHAR *s)
{
    WCHAR *q;
    for (q = s; *q; q++) if (*q == L'/') *q = L'\\';
    if (q > s && q[-1] == L'\\') q[-1] = 0;
}

static int cmp_entry(const void *a, const void *b)
{
    return _wcsicmp(((const ENTRY *)a)->name, ((const ENTRY *)b)->name);
}

static void free_entries(DIRW *d)
{
    DWORD i;
    for (i = 0; i < d->ne; i++) free(d->e[i].name);
    free(d->e);
    d->e = NULL;
    d->ne = d->cap = 0;
}

/* ---------------- 目录表 ---------------- */

static DIRW *dir_find(const WCHAR *path)
{
    DWORD i;
    for (i = 0; i < g_nd; i++)
        if (_wcsicmp(g_d[i].path, path) == 0) return &g_d[i];
    return NULL;
}

static DIRW *dir_add(const WCHAR *rawpath, int src)
{
    WCHAR path[MAX_PATH];
    if (wcslen(rawpath) >= MAX_PATH - 1) {
        L(L"[skip ] 路径过长（>=%d 字符），本版本不支持: %ls", MAX_PATH - 1, rawpath);
        return NULL;
    }
    wcsncpy(path, rawpath, MAX_PATH - 1);
    path[MAX_PATH - 1] = 0;
    normalize(path);
    if (wcslen(path) < 3) return NULL;

    DIRW *old = dir_find(path);
    if (old) {
        if (src == SRC_CMD) old->fromCmd = 1;
        else if (src == SRC_LIST) old->fromList = 1;
        else old->fromAuto = 1;
        return old;
    }

    if (g_nd == g_cap) {
        DWORD ncap = g_cap ? g_cap * 2 : 16;
        DIRW *nw = (DIRW *)realloc(g_d, ncap * sizeof(DIRW));
        if (!nw) return NULL;
        g_d = nw;
        g_cap = ncap;
    }

    DIRW *d = &g_d[g_nd];
    memset(d, 0, sizeof(*d));
    wcsncpy(d->path, path, MAX_PATH - 1);
    d->fromCmd  = (src == SRC_CMD);
    d->fromList = (src == SRC_LIST);
    d->fromAuto = (src == SRC_AUTO);
    d->seen = 1;

    HRESULT hr = SHParseDisplayName(d->path, NULL, &d->pidl, 0, NULL);
    if (FAILED(hr) || !d->pidl) {
        L(L"[skip ] %ls  (pidl hr=0x%08X)", d->path, (unsigned)hr);
        return NULL;
    }
    g_nd++;
    L(L"[watch] %ls%ls", d->path,
      (src == SRC_AUTO) ? L"   (自动发现)" : (src == SRC_LIST) ? L"   (名单)" : L"   (命令行)");
    return d;
}

static void dir_remove(DWORD idx)
{
    DIRW *d = &g_d[idx];
    L(L"[watch] 停止监视 %ls", d->path);
    if (d->pidl) CoTaskMemFree(d->pidl);
    free_entries(d);
    memmove(&g_d[idx], &g_d[idx + 1], (g_nd - idx - 1) * sizeof(DIRW));
    g_nd--;
}

/* ---------------- 自动发现 ---------------- */

static void scan_explorer(int verbose)
{
    IShellWindows *psw = NULL;
    HRESULT hr = CoCreateInstance(&g_clsShellWindows, NULL, CLSCTX_ALL,
                                  &g_iidShellWindows, (void **)&psw);
    if (FAILED(hr) || !psw) {
        if (verbose) L(L"[auto ] ShellWindows hr=0x%08X", (unsigned)hr);
        return;
    }

    DWORD i;
    for (i = 0; i < g_nd; i++) if (g_d[i].fromAuto) g_d[i].seen = 0;

    long n = 0;
    if (SUCCEEDED(IShellWindows_get_Count(psw, &n))) {
        long k;
        for (k = 0; k < n; k++) {
            VARIANT v;
            IDispatch *pd = NULL;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = k;
            if (FAILED(IShellWindows_Item(psw, v, &pd)) || !pd) continue;

            IWebBrowser2 *pwb = NULL;
            if (SUCCEEDED(IDispatch_QueryInterface(pd, &g_iidWebBrowser2, (void **)&pwb)) && pwb) {
                BSTR url = NULL;
                if (SUCCEEDED(IWebBrowser2_get_LocationURL(pwb, &url)) && url) {
                    WCHAR path[MAX_PATH * 2];
                    DWORD cch = MAX_PATH * 2;
                    if (SUCCEEDED(PathCreateFromUrlW(url, path, &cch, 0))) {
                        DWORD at = GetFileAttributesW(path);
                        if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) {
                            DIRW *d = dir_add(path, SRC_AUTO);
                            if (d) d->seen = 1;
                        }
                    } else if (verbose) {
                        L(L"[auto ] 跳过虚拟位置：%ls", url[0] ? url : L"(空 URL，通常是此电脑/网络)");
                    }
                    SysFreeString(url);
                }
                IWebBrowser2_Release(pwb);
            }
            IDispatch_Release(pd);
        }
    }
    IShellWindows_Release(psw);

    /* 窗口已关 → 摘掉自动标记；若也没被名单/命令行引用，则彻底移除 */
    DWORD idx = 0;
    while (idx < g_nd) {
        if (g_d[idx].fromAuto && !g_d[idx].seen) {
            g_d[idx].fromAuto = 0;
            if (!g_d[idx].fromList && !g_d[idx].fromCmd) { dir_remove(idx); continue; }
        }
        idx++;
    }
}

/* ---------------- 轮询 / 比较 ---------------- */

static void poll_dir(DIRW *d)
{
    WCHAR pat[MAX_PATH + 4];
    swprintf(pat, MAX_PATH + 4, L"%ls\\*", d->path);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err != d->lastErr) {
            d->lastErr = err;
            L(L"[err  ] 无法枚举 %ls (err %lu)，该目录暂停检查", d->path, err);
        }
        return;
    }
    if (d->lastErr) { L(L"[err  ] 恢复枚举 %ls", d->path); d->lastErr = 0; }

    ENTRY *cur = NULL;
    DWORD n = 0, cap = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            ENTRY *nw = (ENTRY *)realloc(cur, cap * sizeof(ENTRY));
            if (!nw) { while (n) free(cur[--n].name); free(cur); FindClose(h); return; }
            cur = nw;
        }
        WCHAR *nm = _wcsdup(fd.cFileName);
        if (!nm) continue;
        DWORD hh = hash_name(fd.cFileName);
        hh = hh * 16777619u ^ (DWORD)fd.nFileSizeLow;
        hh = hh * 16777619u ^ (DWORD)fd.nFileSizeHigh;
        hh = hh * 16777619u ^ (DWORD)fd.ftLastWriteTime.dwLowDateTime;
        cur[n].hash = hh;
        cur[n].name = nm;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    qsort(cur, n, sizeof(ENTRY), cmp_entry);
    if (d->e) qsort(d->e, d->ne, sizeof(ENTRY), cmp_entry);

    int first = !d->scanned;

    /* 合并比较：+ 新增 / - 删除 / ~ 修改 */
    DWORD added = 0, removed = 0, changed = 0, logged = 0, i = 0, j = 0;
    while (i < n || j < d->ne) {
        int c;
        if (i >= n) c = 1;
        else if (j >= d->ne) c = -1;
        else c = _wcsicmp(cur[i].name, d->e[j].name);

        if (c == 0) {
            if (cur[i].hash != d->e[j].hash) {
                changed++;
                if (!first && logged < MAXLOG_ITEMS) { L(L"[diff] ~ %ls   (%ls)", cur[i].name, d->path); logged++; }
            }
            i++; j++;
        } else if (c < 0) {
            added++;
            if (!first && logged < MAXLOG_ITEMS) { L(L"[diff] + %ls   (%ls)", cur[i].name, d->path); logged++; }
            i++;
        } else {
            removed++;
            if (!first && logged < MAXLOG_ITEMS) { L(L"[diff] - %ls   (%ls)", d->e[j].name, d->path); logged++; }
            j++;
        }
    }
    if (!first && (added + removed + changed) > logged)
        L(L"[diff] ... 另有 %lu 项未逐条列出   (%ls)", added + removed + changed - logged, d->path);

    free_entries(d);
    d->e = cur;
    d->ne = n;
    d->cap = cap;

    d->scanned = 1;
    if (first) {
        L(L"[snap] %lu 项   (%ls)", n, d->path);
        return;
    }
    if (added || removed || changed) {
        DWORD t = GetTickCount();
        if (!d->pendingFrom) d->pendingFrom = t;
        d->lastChange = t;
        d->pendingDiff += added + removed + changed;
    }
}

static void maybe_fire(DIRW *d)
{
    if (!d->pendingFrom) return;
    DWORD now = GetTickCount();
    if ((now - d->lastChange) < DEBOUNCE_MS) return;

    DWORD diff = d->pendingDiff;
    DWORD waited = now - d->pendingFrom;
    d->pendingFrom = 0;
    d->pendingDiff = 0;

    if (d->pidl)
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSH, d->pidl, NULL);
    L(L"[notify] 补发重排 diff=%lu 攒了%lums   (%ls)", diff, waited, d->path);
}

static void load_list(void)
{
    DWORD ii;
    for (ii = 0; ii < g_nd; ii++) g_d[ii].fromList = 0;

    FILE *f = _wfopen(g_listPath, L"r, ccs=UTF-8");
    if (f) {
    WCHAR line[MAX_PATH];
    while (fgetws(line, MAX_PATH, f)) {
        size_t k = wcslen(line);
        while (k && (line[k - 1] == 10 || line[k - 1] == 13 || line[k - 1] == 32 || line[k - 1] == 9))
            line[--k] = 0;
        if (k > 2) dir_add(line, SRC_LIST);
    }
    fclose(f);
    }

    DWORD idx = 0;
    while (idx < g_nd) {
        if (!g_d[idx].fromCmd && !g_d[idx].fromAuto && !g_d[idx].fromList) dir_remove(idx);
        else idx++;
    }
}

static void app(WCHAR *d, int *k, const WCHAR *src)
{
    while (*src && *k < 30000) d[(*k)++] = *src++;
    d[*k] = 0;
}

static void appch(WCHAR *d, int *k, WCHAR c)
{
    if (*k < 30000) { d[(*k)++] = c; d[*k] = 0; }
}

static void do_detach(int argc, WCHAR **argv)
{
    WCHAR exe[MAX_PATH], cmd[32000];
    int k = 0, i, hasLog = 0;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    cmd[0] = 0;
    appch(cmd, &k, L'"');
    app(cmd, &k, exe);
    appch(cmd, &k, L'"');
    for (i = 1; i < argc; i++) if (!_wcsicmp(argv[i], L"--log")) hasLog = 1;
    for (i = 1; i < argc; i++) {
        int needq;
        if (!_wcsicmp(argv[i], L"--detach")) continue;
        appch(cmd, &k, L' ');
        needq = (wcschr(argv[i], L' ') != NULL);
        if (needq) appch(cmd, &k, L'"');
        app(cmd, &k, argv[i]);
        if (needq) appch(cmd, &k, L'"');
    }
    if (!hasLog) { appch(cmd, &k, L' '); app(cmd, &k, L"--log"); }

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DWORD wt = WaitForSingleObject(pi.hProcess, 800);
        if (wt == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            printf("not started: another instance is already running (exit=%lu)", code);
            fputc(10, stdout);
        } else {
            printf("started in background, pid=%lu", pi.dwProcessId); fputc(10, stdout);
            printf("log: %ls", g_logPath); fputc(10, stdout);
        }
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    } else {
        printf("start failed, error=%lu", GetLastError()); fputc(10, stdout);
    }
}

static void do_stop(void)
{
    WCHAR want[MAX_PATH];
    HANDLE snap; PROCESSENTRY32W pe;
    int n = 0;
    DWORD me = GetCurrentProcessId();
    swprintf(want, MAX_PATH, L"%ls.exe", g_base);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { printf("snapshot failed"); fputc(10, stdout); return; }
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            HANDLE h;
            if (pe.th32ProcessID == me) continue;
            if (_wcsicmp(pe.szExeFile, want) != 0) continue;
            h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (!h) continue;
            if (TerminateProcess(h, 0)) { n++; printf("stopped pid=%lu", pe.th32ProcessID); fputc(10, stdout); }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!n) { printf("no running instance"); fputc(10, stdout); }
}

int wmain(int argc, WCHAR **argv)
{
    init_paths();

    if (argc >= 2 && !wcscmp(argv[1], L"--stop")) { do_stop(); return 0; }
    if (argc >= 2 && !wcscmp(argv[1], L"--detach")) { do_detach(argc, argv); return 0; }
    if (argc >= 2 && !wcscmp(argv[1], L"--version")) {
        printf("%ls  built " __DATE__ " " __TIME__ "%ls", argv[0], "");
        fputc(10, stdout);
        return 0;
    }

    if (argc >= 3 && !wcscmp(argv[1], L"--now")) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        WCHAR path[MAX_PATH];
        wcsncpy(path, argv[2], MAX_PATH - 1);
        path[MAX_PATH - 1] = 0;
        normalize(path);
        PIDLIST_ABSOLUTE pidl = NULL;
        if (FAILED(SHParseDisplayName(path, NULL, &pidl, 0, NULL)) || !pidl) return 1;
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSH, pidl, NULL);
        CoTaskMemFree(pidl);
        return 0;
    }

    SetConsoleOutputCP(65001);   /* 让控制台按 UTF-8 显示中文路径 */
    {
        int a;
        for (a = 1; a < argc; a++) if (!wcscmp(argv[a], L"--log")) g_useLog = 1;
    }
    {
        HANDLE hm = CreateMutexW(NULL, FALSE, L"fix_explorer_refresh_singleton_v1");
        if (hm && GetLastError() == ERROR_ALREADY_EXISTS) {
            fputs("already running. use: --stop  (or taskkill /f /im fix_explorer_refresh.exe)", stdout);
            fputc(10, stdout);
            return 0;
        }
    }
    if (g_useLog) g_log = _wfopen(g_logPath, L"w, ccs=UTF-8");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    L(L"==== %ls pid=%lu  poll=%dms debounce=%dms rescan=%dms  日志=%ls ====",
      g_base, GetCurrentProcessId(), POLL_MS, DEBOUNCE_MS, RESCAN_MS,
      g_useLog ? g_logPath : L"仅控制台（加 --log 才写文件）");

    int i;
    for (i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--log")) { g_useLog = 1; continue; }
        if (!wcscmp(argv[i], L"--no-auto")) { g_noAuto = 1; continue; }
        if (argv[i][0] == L'-' && argv[i][1] == L'-') continue;
        dir_add(argv[i], SRC_CMD);
    }
    load_list();
    if (g_noAuto) L(L"[auto ] 已用 --no-auto 关闭自动发现，只监视显式指定的目录");
    else scan_explorer(1);
    {
        DWORD a = 0, k2;
        for (k2 = 0; k2 < g_nd; k2++) if (g_d[k2].fromAuto) a++;
        L(L"[auto ] 自动跟踪 %lu 个目录，共 %lu 个监视目标", a, g_nd);
    }

    DWORD tick = 0;
    for (;;) {
        Sleep(POLL_MS);
        DWORD k;
        for (k = 0; k < g_nd; k++) { poll_dir(&g_d[k]); maybe_fire(&g_d[k]); }
        if (++tick * POLL_MS >= RESCAN_MS) {
            tick = 0;
            if (!g_noAuto) scan_explorer(0);
            load_list();
        }
    }
    return 0;
}
