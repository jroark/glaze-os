/* Boot, memory and app-launch measurements under stock Windows CE, the
 * counterpart of tests/bench.py's for Glaze (tests/bench_ce.py,
 * docs/os/PERFORMANCE.md).
 *
 * Run from a CF card as \CE\R4100\AutoRun.exe, which Windows CE starts once
 * the shell is up. It logs to cebench.log beside it and to the debug
 * output (the emulator's stdout; COM1 would block there):
 *
 *   cebench: boot N ms            GetTickCount() on entry: ms since CE's
 *                                 kernel started, until AutoRun runs (the
 *                                 card is found well after the shell)
 *   cebench: mem LABEL ...        GlobalMemoryStatus and how CE divides RAM
 *                                 between the object store and programs
 *   cebench: launch APP cold|warm N ms   CreateProcess until the app's
 *                                 window is in front and its message loop
 *                                 answers; warm launches it again while
 *                                 the first copy is in the background
 *   cebench: done
 *
 * It waits for the setup wizard to end first. Each app runs cold, the shell comes back to the front, the app is
 * launched again (warm), the shell comes back again (mem home is taken
 * then, with the app in the background as Glaze's sb: home lines are), and
 * the app is closed with WM_CLOSE. */
#include <windows.h>
#include <tlhelp32.h>

#define WIDEN2(s)       L ## s
#define WIDEN(s)        WIDEN2(s)

#define WIZARD_MS       180000  /* longest wait for the setup wizard to end */
#define SETTLE_MS       5000    /* after it, before the first launch */
#define LAUNCH_MS       20000   /* give up on a launch after this */
#define HOME_MS         2000    /* in the background before the next step */

typedef BOOL (*GetSystemMemoryDivisionFn)(LPDWORD, LPDWORD, LPDWORD);

static HANDLE log_file = INVALID_HANDLE_VALUE;
static WCHAR dir[MAX_PATH];
static GetSystemMemoryDivisionFn pGetSystemMemoryDivision;
static HWND shell;

/* The apps measured: the name logged (Glaze's counterpart, as in
 * tests/bench.py) and the executable, which the CCFS driver presents in
 * \Program Disk without its .cpk suffix. */
static const struct {
    const WCHAR *name, *exe;
} apps[] = {
    { L"calculator", L"calc.exe" },
    { L"notes", L"notes.exe" },
    { L"calendar", L"Calendar.exe" },
    { L"clock", L"clock.exe" },
    { L"settings", L"ControlPanel.exe" },
    { L"browser", L"Internet.exe" },
};
static const WCHAR *const folders[] = {
    L"\\Program Disk\\", L"\\Windows\\",
};

static void Say(const WCHAR *fmt, ...)
{
    WCHAR wide[300];
    char line[320];
    DWORD done;
    va_list ap;

    va_start(ap, fmt);
    wvsprintf(wide, fmt, ap);
    va_end(ap);
    OutputDebugString(wide);
    OutputDebugString(L"\r\n");
    int n = WideCharToMultiByte(CP_ACP, 0, wide, -1, line, sizeof(line) - 2, NULL, NULL);
    if (n <= 0)
        return;
    line[n - 1] = '\r';
    line[n] = '\n';
    if (log_file != INVALID_HANDLE_VALUE) {
        WriteFile(log_file, line, n + 1, &done, NULL);
        FlushFileBuffers(log_file);
    }
}

/* Whether a process named exe is running; with log, lists them all. */
static BOOL Running(const WCHAR *exe, BOOL log)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    BOOL found = FALSE;

    if (snap == INVALID_HANDLE_VALUE)
        return FALSE;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (log)
            Say(L"cebench: process %s", pe.szExeFile);
        if (!lstrcmpi(pe.szExeFile, exe))
            found = TRUE;
    }
    CloseToolhelp32Snapshot(snap);
    return found;
}

/* Free program memory, in KiB, and the rest on one line. */
static DWORD Memory(const WCHAR *label)
{
    MEMORYSTATUS ms;
    STORE_INFORMATION si;
    DWORD store = 0, ram = 0, page = 0;

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    if (!pGetSystemMemoryDivision || !pGetSystemMemoryDivision(&store, &ram, &page))
        store = ram = page = 0;
    if (!GetStoreInformation(&si))
        si.dwStoreSize = si.dwFreeSize = 0;
    Say(L"cebench: mem %s phys %u KiB free %u KiB programs %u KiB store %u KiB store_free %u KiB", label,
        ms.dwTotalPhys / 1024, ms.dwAvailPhys / 1024, ram * (page / 1024), store * (page / 1024),
        si.dwFreeSize / 1024);
    return ms.dwAvailPhys / 1024;
}

static DWORD want_pid;
static HWND found;

static BOOL CALLBACK FindWindowOf(HWND hwnd, LPARAM unused)
{
    DWORD pid = 0;

    (void)unused;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == want_pid && IsWindowVisible(hwnd) && !GetParent(hwnd)) {
        found = hwnd;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI Ping(LPVOID hwnd)
{
    SendMessage((HWND)hwnd, WM_NULL, 0, 0);
    return 0;
}

/* Waits until the window answers a message, which it does once its thread
 * is back in its message loop: done starting. On a worker thread, since
 * SendMessage has no timeout in Windows CE 3.0. */
static BOOL Answers(HWND hwnd, DWORD deadline)
{
    HANDLE t = CreateThread(NULL, 0, Ping, hwnd, 0, NULL);
    DWORD now = GetTickCount();
    BOOL ok;

    if (!t)
        return FALSE;
    ok = WaitForSingleObject(t, now < deadline ? deadline - now : 0) == WAIT_OBJECT_0;
    CloseHandle(t);
    return ok;
}

static BOOL Start(const WCHAR *exe, PROCESS_INFORMATION *pi, WCHAR *path)
{
    unsigned i;

    for (i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        wsprintf(path, L"%s%s", folders[i], exe);
        if (CreateProcess(path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, pi))
            return TRUE;
    }
    return FALSE;
}

/* Starts exe and returns its window once it is in front and answering, or
 * NULL. pid is the app's process: for a warm launch, the first copy's,
 * whose window a single-instance app brings back before its new copy
 * exits. Other apps keep a second copy running, returned in *second.
 * *ms is the time taken. */
static HWND Launch(const WCHAR *exe, DWORD *pid, DWORD *second, DWORD *ms)
{
    PROCESS_INFORMATION pi;
    WCHAR path[MAX_PATH];
    DWORD t0 = GetTickCount(), deadline = t0 + LAUNCH_MS;
    HWND front = NULL;

    *ms = 0;
    if (!Start(exe, &pi, path)) {
        Say(L"cebench: can't start %s (error %u)", exe, GetLastError());
        return NULL;
    }
    if (!*pid)
        *pid = pi.dwProcessId;
    while (GetTickCount() < deadline) {
        HWND fg = GetForegroundWindow();
        DWORD owner = 0;

        if (fg)
            GetWindowThreadProcessId(fg, &owner);
        if (fg && fg != shell && (owner == *pid || owner == pi.dwProcessId) && IsWindowVisible(fg)) {
            front = fg;
            break;
        }
        Sleep(5);
    }
    if (front && !Answers(front, deadline))
        front = NULL;
    *ms = GetTickCount() - t0;
    if (pi.dwProcessId != *pid) {
        BOOL exited = WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0;

        Say(L"cebench: %s second copy %s", path, exited ? L"exited" : L"still running");
        if (!exited && second)
            *second = pi.dwProcessId;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return front;
}

static void Home(void)
{
    if (shell)
        SetForegroundWindow(shell);
    Sleep(HOME_MS);
}

static void Close(DWORD pid)
{
    HANDLE p = OpenProcess(0, FALSE, pid);

    want_pid = pid;
    found = NULL;
    EnumWindows(FindWindowOf, 0);
    if (found)
        PostMessage(found, WM_CLOSE, 0, 0);
    if (p && WaitForSingleObject(p, 5000) != WAIT_OBJECT_0) {
        Say(L"cebench: pid %u didn't close; ending it", pid);
        TerminateProcess(p, 0);
        WaitForSingleObject(p, 2000);
    }
    if (p)
        CloseHandle(p);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPWSTR cmdline, int show)
{
    DWORD boot = GetTickCount();
    WCHAR path[MAX_PATH], cls[64];
    unsigned i;

    (void)prev;
    (void)cmdline;
    (void)show;
    DWORD n = GetModuleFileName(instance, dir, MAX_PATH - 1);
    while (n && dir[n - 1] != L'\\')
        n--;
    dir[n] = 0;
    wsprintf(path, L"%scebench.log", dir);
    log_file = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HMODULE core = LoadLibrary(L"coredll.dll");
    pGetSystemMemoryDivision = (GetSystemMemoryDivisionFn)GetProcAddress(core, L"GetSystemMemoryDivision");
    Say(L"cebench: built " WIDEN(__DATE__) L" " WIDEN(__TIME__));
    Say(L"cebench: boot %u ms", boot);
    Memory(L"boot");

    /* A cold boot runs the setup wizard (screen alignment, then the home
     * city and time), which tests/bench_ce.py taps through. It can't be
     * ended from here, and it would sit in front of every app. */
    while (Running(L"Welcome.exe", FALSE)) {
        if (GetTickCount() - boot > WIZARD_MS) {
            Say(L"cebench: the setup wizard is still running; giving up");
            Running(L"", TRUE);
            goto out;
        }
        Sleep(100);
    }
    Say(L"cebench: setup wizard done at %u ms", GetTickCount());
    Sleep(SETTLE_MS);
    Running(L"", TRUE);
    shell = GetForegroundWindow();
    cls[0] = 0;
    if (shell)
        GetClassName(shell, cls, 64);
    Say(L"cebench: shell window class %s", cls);
    Memory(L"idle");

    for (i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
        DWORD pid = 0, second = 0, ms;
        HWND w = Launch(apps[i].exe, &pid, NULL, &ms);

        if (!w) {
            Say(L"cebench: launch %s failed after %u ms", apps[i].name, ms);
            if (pid)
                Close(pid);
            Home();
            continue;
        }
        Say(L"cebench: launch %s cold %u ms", apps[i].name, ms);
        Home();
        if (Launch(apps[i].exe, &pid, &second, &ms))
            Say(L"cebench: launch %s warm %u ms", apps[i].name, ms);
        else
            Say(L"cebench: launch %s warm failed after %u ms", apps[i].name, ms);
        Home();
        Memory(L"home");
        Close(pid);
        if (second)
            Close(second);
        Home();
    }
    Memory(L"end");
    Say(L"cebench: done");
out:
    if (log_file != INVALID_HANDLE_VALUE)
        CloseHandle(log_file);
    return 0;
}
