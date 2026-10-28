/*
 * GlazeBoot: boots Glaze from RAM on a BE-300 running its stock Windows CE,
 * as CyaCE did for Linux. Nothing is flashed: the device runs Glaze until it
 * is reset, then Windows CE cold-boots from the NAND as before.
 *
 * Built with the be300-dev toolchain (clang and the Casio SDK, `make
 * glazeboot-exe`). Two headers come from the GCC build (`make glazeboot`,
 * out/glazeboot/gen): glazeboot_abi.h (layouts shared with the kernel,
 * wince/glazeboot/mkabi.c) and tramp_blob.h (the trampoline,
 * wince/glazeboot/tramp.c, as bytes, so the loader needs no MIPS assembly).
 *
 * It sits on a CompactFlash card in a folder with glaze.cfg, kernel.bin and
 * romfs.img, reads them, and builds the hand-over plan (plan.h):
 *
 *   PA 0xF000            boot info (glaze/bootinfo.h)
 *   PA 0x10000           the kernel, at its link address
 *   below the romfs      the RAM disk Glaze's / lives on (ramfs_kb)
 *   top of RAM           the romfs archive, 8 KiB aligned
 *
 * Then it asks Windows CE for kernel mode and calls the trampoline, which
 * takes the machine over. With mode=probe in glaze.cfg it only checks what
 * the hand-over relies on and writes the results to glazeboot.log.
 * docs/GLAZEBOOT.md describes the whole path.
 */

#include <windows.h>
#include "glazeboot_abi.h"
#include "tramp_blob.h"

#define SDRAM_BYTES     0x1000000UL
#define KSEG0(pa)       (0x80000000UL | (pa))
#define KSEG1(pa)       (0xA0000000UL | (pa))
#define RAMFS_BLOCK     0x4000UL
#define WIDEN2(s)       L ## s
#define WIDEN(s)        WIDEN2(s)   /* a string macro as a wide literal */

/* pkfuncs.h, not in the SDK. */
#define LOCKFLAG_WRITE          0x001
#define CACHE_SYNC_DISCARD      0x001   /* write back and discard the D-cache */
#define CACHE_SYNC_INSTRUCTIONS 0x002   /* discard the I-cache */

/* The layouts must match the GCC side's. */
typedef char check_bootinfo[sizeof(gz_bootinfo) == GZ_BOOTINFO_SIZE ? 1 : -1];
typedef char check_kernel_bin[sizeof(gz_kernel_bin) == GZ_KERNEL_BIN_SIZE ? 1 : -1];
typedef char check_move[sizeof(gb_move) == GB_MOVE_SIZE ? 1 : -1];
typedef char check_control[sizeof(gb_control) == GB_CONTROL_SIZE ? 1 : -1];

/* Kernel calls the SDK doesn't declare, from COREDLL by name. */
typedef BOOL (*LockPagesFn)(LPVOID, DWORD, PDWORD, int);
typedef BOOL (*SetKModeFn)(BOOL);
typedef DWORD (*SetProcPermissionsFn)(DWORD);
typedef void (*CacheSyncFn)(int);
typedef BOOL (*GetSystemMemoryDivisionFn)(LPDWORD, LPDWORD, LPDWORD);
typedef DWORD (*SetSystemMemoryDivisionFn)(DWORD);

static LockPagesFn pLockPages;
static SetKModeFn pSetKMode;
static SetProcPermissionsFn pSetProcPermissions;
static CacheSyncFn pCacheSync;
static GetSystemMemoryDivisionFn pGetSystemMemoryDivision;
static SetSystemMemoryDivisionFn pSetSystemMemoryDivision;

/* ---- paths, the log, messages --------------------------------------------- */

static WCHAR dir[MAX_PATH];     /* GlazeBoot's folder, with a trailing '\' */
static HANDLE log_file = INVALID_HANDLE_VALUE;
static HANDLE serial = INVALID_HANDLE_VALUE;
static BOOL usb_unattended;

static void PathOf(WCHAR *out, const WCHAR *name)
{
    int n = 0, i;

    for (i = 0; dir[i] && n < MAX_PATH - 1; i++)
        out[n++] = dir[i];
    for (i = 0; name[i] && n < MAX_PATH - 1; i++)
        out[n++] = name[i];
    out[n] = 0;
}

/* A line in glazeboot.log (ASCII), formatted by wvsprintf, and on the
 * debug output (the emulator's stdout), which survives a crash that loses
 * the file's last writes. */
static void Say(const WCHAR *fmt, ...)
{
    WCHAR wide[300];
    char line[320];
    DWORD done;
    va_list ap;

    va_start(ap, fmt);
    wvsprintf(wide, fmt, ap);
    va_end(ap);
    OutputDebugString(L"GlazeBoot: ");
    OutputDebugString(wide);
    OutputDebugString(L"\r\n");
    int n = WideCharToMultiByte(CP_ACP, 0, wide, -1, line, sizeof(line) - 2, NULL, NULL);
    if (n <= 0)
        return;
    line[n - 1] = '\r';
    line[n] = '\n';
    if (log_file != INVALID_HANDLE_VALUE)
        WriteFile(log_file, line, n + 1, &done, NULL);
    if (serial != INVALID_HANDLE_VALUE) {
        WriteFile(serial, line, n + 1, &done, NULL);
        FlushFileBuffers(serial);
    }
}

static void Alert(const WCHAR *fmt, ...)
{
    WCHAR text[300];
    va_list ap;

    va_start(ap, fmt);
    wvsprintf(text, fmt, ap);
    va_end(ap);
    Say(L"%s", text);
    if (!usb_unattended)
        MessageBox(NULL, text, L"GlazeBoot", MB_OK | MB_ICONEXCLAMATION);
}

/* The sync cable's port, opened through Windows CE and left open: its
 * driver powers the RS-232 transceiver (the BE-300 drives none of DSR, CTS
 * or DCD otherwise) and sets up the companion SIU, which Glaze's console
 * then uses as it is. 115200 8N1, as CyaCE's lib/debug.c opened it for
 * Linux. The log goes to it too, each line flushed before the next step. */
static void SerialOpen(void)
{
    DCB dcb;

    serial = CreateFile(L"COM1:", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (serial == INVALID_HANDLE_VALUE) {
        Say(L"serial: can't open COM1: (error %u)", GetLastError());
        return;
    }
    dcb.DCBlength = sizeof(dcb);
    GetCommState(serial, &dcb);
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(serial, &dcb))
        Say(L"serial: SetCommState failed (error %u)", GetLastError());
    EscapeCommFunction(serial, SETDTR);
    EscapeCommFunction(serial, SETRTS);
    Say(L"serial: COM1: open at 115200 8N1");
}

/* ---- memory ------------------------------------------------------------------ */

/* Memory comes in pieces of up to CHUNK_PAGES: Windows CE has room for the
 * romfs only in several (its largest allocation is smaller), and the plan
 * moves each page on its own anyway. */
#define CHUNK_PAGES     64

typedef struct Buf {
    BYTE *p;                    /* the first piece */
    DWORD size, pages;
    DWORD *pfn;                 /* LockPages' frame numbers, one per page */
} Buf;

static DWORD pfn_shift;

static DWORD PaOf(DWORD pfn)
{
    return pfn << pfn_shift;
}

static BYTE *PagesAlloc(DWORD pages)
{
    return (BYTE *)VirtualAlloc(NULL, pages * GB_PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

/* Committed, touched (so backed) and locked pages, with their frames,
 * filled from the file h if it isn't INVALID_HANDLE_VALUE. */
static BOOL BufAlloc(Buf *b, DWORD size, HANDLE h)
{
    DWORD at, i;

    b->size = size;
    b->pages = (size + GB_PAGE - 1) / GB_PAGE;
    if (!b->pages)
        b->pages = 1;
    b->p = NULL;
    b->pfn = (DWORD *)PagesAlloc((b->pages * sizeof(DWORD) + GB_PAGE - 1) / GB_PAGE);
    if (!b->pfn)
        return FALSE;
    for (at = 0; at < b->pages; at += CHUNK_PAGES) {
        DWORD n = b->pages - at < CHUNK_PAGES ? b->pages - at : CHUNK_PAGES;
        BYTE *p = PagesAlloc(n);
        if (!p)
            return FALSE;
        if (!b->p)
            b->p = p;
        for (i = 0; i < n; i++)
            ((volatile BYTE *)p)[i * GB_PAGE] = 0;
        if (!pLockPages(p, n * GB_PAGE, b->pfn + at, LOCKFLAG_WRITE))
            return FALSE;
        if (h != INVALID_HANDLE_VALUE) {
            DWORD want = size - at * GB_PAGE < n * GB_PAGE ? size - at * GB_PAGE : n * GB_PAGE, got;
            for (i = 0; i < want; i += got)
                if (!ReadFile(h, p + i, want - i, &got, NULL) || !got)
                    return FALSE;
        }
    }
    return TRUE;
}

static BOOL ReadWholeFile(const WCHAR *name, Buf *b)
{
    WCHAR path[MAX_PATH];

    PathOf(path, name);
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Alert(L"Can't open %s (error %u).", name, GetLastError());
        return FALSE;
    }
    DWORD size = GetFileSize(h, NULL);
    BOOL ok = size && size != 0xFFFFFFFF && size < SDRAM_BYTES / 2 && BufAlloc(b, size, h);
    if (!ok)
        Alert(L"Can't load %s (%u bytes, error %u).", name, size, GetLastError());
    CloseHandle(h);
    return ok;
}

/* ---- glaze.cfg ----------------------------------------------------------------- */

enum { NAND_OFF, NAND_RO, NAND_RW };

static struct {
    WCHAR kernel[64], romfs[64];
    DWORD ramfs_kb, countdown;
    DWORD nand;                 /* NAND_OFF, NAND_RO or NAND_RW */
    BOOL confirm, probe;
} cfg;

static BOOL Prefix(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return FALSE;
    return TRUE;
}

static void CfgStr(WCHAR *out, const char *v)
{
    int n = 0;

    while (v[n] && v[n] != '\r' && v[n] != '\n' && n < 63) {
        out[n] = (WCHAR)(BYTE)v[n];
        n++;
    }
    out[n] = 0;
}

static DWORD CfgNum(const char *s)
{
    DWORD v = 0;

    while (*s >= '0' && *s <= '9')
        v = v * 10 + (DWORD)(*s++ - '0');
    return v;
}

/* key=value lines; others (such as '#' comments) are ignored, and later
 * lines win. A missing file leaves the defaults. */
static void CfgRead(void)
{
    static char text[2048];
    WCHAR path[MAX_PATH];
    DWORD got = 0;
    char *l;

    lstrcpy(cfg.kernel, L"kernel.bin");
    lstrcpy(cfg.romfs, L"romfs.img");
    cfg.ramfs_kb = 2048;
    cfg.nand = NAND_OFF;
    cfg.confirm = TRUE;
    cfg.countdown = 10;
    cfg.probe = FALSE;

    PathOf(path, L"glaze.cfg");
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    ReadFile(h, text, sizeof(text) - 1, &got, NULL);
    CloseHandle(h);
    text[got] = 0;
    for (l = text; *l; ) {
        if (Prefix(l, "kernel="))
            CfgStr(cfg.kernel, l + 7);
        else if (Prefix(l, "romfs="))
            CfgStr(cfg.romfs, l + 6);
        else if (Prefix(l, "ramfs_kb="))
            cfg.ramfs_kb = CfgNum(l + 9);
        else if (Prefix(l, "nand="))
            cfg.nand = Prefix(l + 5, "rw") ? NAND_RW : Prefix(l + 5, "ro") ? NAND_RO : NAND_OFF;
        else if (Prefix(l, "countdown="))
            cfg.countdown = CfgNum(l + 10);
        else if (Prefix(l, "confirm="))
            cfg.confirm = l[8] != '0';
        else if (Prefix(l, "mode="))
            cfg.probe = Prefix(l + 5, "probe");
        while (*l && *l != '\n')
            l++;
        if (*l)
            l++;
    }
}

/* ---- Windows CE's side ----------------------------------------------------------- */

static BOOL ImportKernelCalls(void)
{
    HMODULE core = LoadLibrary(L"coredll.dll");

    if (!core)
        return FALSE;
    pLockPages = (LockPagesFn)GetProcAddress(core, L"LockPages");
    pSetKMode = (SetKModeFn)GetProcAddress(core, L"SetKMode");
    pSetProcPermissions = (SetProcPermissionsFn)GetProcAddress(core, L"SetProcPermissions");
    pCacheSync = (CacheSyncFn)GetProcAddress(core, L"CacheSync");
    pGetSystemMemoryDivision = (GetSystemMemoryDivisionFn)GetProcAddress(core, L"GetSystemMemoryDivision");
    pSetSystemMemoryDivision = (SetSystemMemoryDivisionFn)GetProcAddress(core, L"SetSystemMemoryDivision");
    Say(L"coredll: LockPages %08X SetKMode %08X SetProcPermissions %08X CacheSync %08X",
        (DWORD)pLockPages, (DWORD)pSetKMode, (DWORD)pSetProcPermissions, (DWORD)pCacheSync);
    return pLockPages && pSetKMode && pSetProcPermissions && pCacheSync;
}

/* How Windows CE divides RAM between the object store and programs. */
static void MemoryReport(void)
{
    MEMORYSTATUS ms;
    DWORD store = 0, ram = 0, page = 0;

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    Say(L"memory: physical %u KiB, %u KiB free", ms.dwTotalPhys / 1024, ms.dwAvailPhys / 1024);
    if (pGetSystemMemoryDivision && pGetSystemMemoryDivision(&store, &ram, &page))
        Say(L"memory: object store %u pages, programs %u pages, of %u bytes", store, ram, page);
}

/* The stock Windows CE leaves programs about 4.5 MiB, short of the romfs,
 * and keeps the rest of the RAM it doesn't run from for the object store.
 * Lend programs all of the store's free pages but STORE_MARGIN; Windows CE
 * is about to stop, so they aren't given back. */
#define STORE_MARGIN    16

static void BorrowStore(void)
{
    STORE_INFORMATION si;
    DWORD store, ram, page;

    if (!pGetSystemMemoryDivision || !pSetSystemMemoryDivision ||
        !pGetSystemMemoryDivision(&store, &ram, &page) || !page || !GetStoreInformation(&si))
        return;
    DWORD spare = si.dwFreeSize / page;
    if (spare <= STORE_MARGIN || spare > store)
        return;
    DWORD rv = pSetSystemMemoryDivision(store - (spare - STORE_MARGIN));
    Say(L"memory: lent %u of the object store's pages to programs (result %u)", spare - STORE_MARGIN, rv);
    MemoryReport();
}

/* Kernel mode, so KSEG0, KSEG1 and CP0 are reachable. */
static void KernelMode(void)
{
    pSetKMode(TRUE);
    pSetProcPermissions(0xFFFFFFFF);
}

/* LockPages' frame numbers are EntryLo PFN fields, which on the VR41xx hold
 * physical address bits 31:10 from bit 6 (VR4131 UM 5.4.3, EntryLo0/1),
 * so the address shifted right by 4; the stock Windows CE's are. Rather
 * than trust that, try the likely shifts on a page written through its
 * virtual address and read back through KSEG1. */
static BOOL FindPfnShift(Buf *b)
{
    static const DWORD shifts[] = { 4, 6, 12, 8, 10, 0 };
    volatile DWORD *v = (volatile DWORD *)b->p;
    DWORD i;

    for (i = 0; i < sizeof(shifts) / sizeof(shifts[0]); i++) {
        DWORD pa = b->pfn[0] << shifts[i];
        if ((pa & (GB_PAGE - 1)) || pa >= SDRAM_BYTES)
            continue;
        v[0] = 0x475a4231 ^ pa;
        v[1] = ~v[0];
        pCacheSync(CACHE_SYNC_DISCARD);
        volatile DWORD *k = (volatile DWORD *)KSEG1(pa);
        if (k[0] == v[0] && k[1] == v[1]) {
            pfn_shift = shifts[i];
            Say(L"pfn: %08X is pa %08X (shift %u)", b->pfn[0], pa, pfn_shift);
            return TRUE;
        }
    }
    Say(L"pfn: no shift maps frame %08X to its page", b->pfn[0]);
    return FALSE;
}

/* ---- the plan ------------------------------------------------------------------ */

static BYTE dst_used[GB_FRAMES / 8];

static BOOL IsDst(DWORD frame)
{
    return (dst_used[frame / 8] >> (frame % 8)) & 1;
}

static void MarkDst(DWORD pa, DWORD len)
{
    DWORD f;

    for (f = pa / GB_PAGE; f < (pa + len + GB_PAGE - 1) / GB_PAGE && f < GB_FRAMES; f++)
        dst_used[f / 8] |= (BYTE)(1 << (f % 8));
}

/* A locked page whose frame is no destination, for the plan and the
 * trampoline; ones that are stay allocated so they don't come back. */
static BOOL AllocSafe(Buf *b)
{
    int tries;

    for (tries = 0; tries < 256; tries++) {
        if (!BufAlloc(b, GB_PAGE, INVALID_HANDLE_VALUE))
            return FALSE;
        DWORD f = PaOf(b->pfn[0]) / GB_PAGE;
        if (f < GB_FRAMES && !IsDst(f))
            return TRUE;
    }
    return FALSE;
}

static gb_control *control;
static Buf ctrl_page, code_page, move_pages[GB_MAX_MOVE_PAGES], who_pages[GB_WHO_PAGES];

static BOOL AddMove(DWORD src_pa, DWORD dst_pa)
{
    DWORD i = control->count;

    if (i >= GB_MAX_MOVES || src_pa >= SDRAM_BYTES || dst_pa >= SDRAM_BYTES)
        return FALSE;
    gb_move *m = (gb_move *)move_pages[i / GB_MOVES_PER_PAGE].p + i % GB_MOVES_PER_PAGE;
    m->src = (WORD)(src_pa / GB_PAGE);
    m->dst = (WORD)(dst_pa / GB_PAGE);
    m->where = m->src;
    m->pad = 0;
    control->count++;
    return TRUE;
}

/* The trampoline's bytes in a page that is no destination, written back to
 * RAM so they can run through KSEG0. Returns the page's address. */
static BOOL LoadTrampoline(DWORD *pa)
{
    if (!AllocSafe(&code_page))
        return FALSE;
    memcpy(code_page.p, tramp_code, TRAMP_BYTES);
    pCacheSync(CACHE_SYNC_DISCARD | CACHE_SYNC_INSTRUCTIONS);
    *pa = PaOf(code_page.pfn[0]);
    return TRUE;
}

/* The romfs file's size, from the card. */
static DWORD RomfsKiB(void)
{
    WCHAR path[MAX_PATH];
    DWORD size = 0;

    PathOf(path, cfg.romfs);
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        size = GetFileSize(h, NULL);
        CloseHandle(h);
    }
    return size / 1024;
}

/* ---- the confirmation --------------------------------------------------------- */

#define ID_BOOT     1
#define ID_CANCEL   2
#define CONFIRM_W   220
#define CONFIRM_H   150

static HINSTANCE app;
static HWND confirm_text;
static DWORD confirm_left;      /* seconds, 0 with no countdown */
static int confirm_result;      /* -1 while asking, then TRUE to boot */

static void ConfirmShowLeft(void)
{
    WCHAR text[160];

    if (confirm_left)
        wsprintf(text, L"Starting Glaze in %u s.\n\nWindows CE stops and nothing is saved to it. "
                       L"Reset the BE-300 to return to it.", confirm_left);
    else
        wsprintf(text, L"Start Glaze now?\n\nWindows CE stops and nothing is saved to it. "
                       L"Reset the BE-300 to return to it.");
    SetWindowText(confirm_text, text);
}

static LRESULT CALLBACK ConfirmProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (confirm_left && !--confirm_left)
            confirm_result = TRUE;
        else
            ConfirmShowLeft();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BOOT || LOWORD(wp) == ID_CANCEL)
            confirm_result = LOWORD(wp) == ID_BOOT;
        return 0;
    case WM_CLOSE:
        confirm_result = FALSE;
        return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

/* A window over the shell counting down cfg.countdown seconds, with Boot
 * and Cancel; TRUE to boot. Its own message loop makes it modal. */
static BOOL Confirm(void)
{
    WNDCLASS wc;
    RECT r;
    MSG msg;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ConfirmProc;
    wc.hInstance = app;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"GlazeBootConfirm";
    RegisterClass(&wc);
    HWND w = CreateWindowEx(WS_EX_TOPMOST, wc.lpszClassName, L"GlazeBoot", WS_POPUP | WS_CAPTION | WS_BORDER,
                            (GetSystemMetrics(SM_CXSCREEN) - CONFIRM_W) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - CONFIRM_H) / 2, CONFIRM_W, CONFIRM_H,
                            NULL, NULL, app, NULL);
    if (!w) {
        Say(L"confirm: no window (error %u); booting", GetLastError());
        return TRUE;
    }
    GetClientRect(w, &r);
    confirm_text = CreateWindow(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 8, 8, r.right - 16,
                                r.bottom - 50, w, NULL, app, NULL);
    CreateWindow(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 8, r.bottom - 34,
                 (r.right - 24) / 2, 26, w, (HMENU)ID_CANCEL, app, NULL);
    CreateWindow(L"BUTTON", L"Boot", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, r.right / 2 + 4,
                 r.bottom - 34, (r.right - 24) / 2, 26, w, (HMENU)ID_BOOT, app, NULL);
    confirm_left = cfg.countdown;
    confirm_result = -1;
    ConfirmShowLeft();
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    UpdateWindow(w);
    if (confirm_left)
        SetTimer(w, 1, 1000, NULL);
    while (confirm_result < 0 && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (confirm_result < 0)     /* WM_QUIT */
        confirm_result = FALSE;
    DestroyWindow(w);
    return confirm_result;
}

/* ---- probe mode ------------------------------------------------------------------ */

typedef DWORD (*ReadC0Fn)(DWORD);

static void Probe(void)
{
    static BYTE *pieces[SDRAM_BYTES / (CHUNK_PAGES * GB_PAGE)];
    DWORD code_pa, mb, n, i;

    if (!LoadTrampoline(&code_pa)) {
        Alert(L"Out of memory for the trampoline.");
        return;
    }
    Say(L"probe: trampoline at pa %08X; reading CP0 through its virtual address", code_pa);
    ReadC0Fn by_va = (ReadC0Fn)(code_page.p + TRAMP_READ_C0);
    Say(L"probe: Status %08X PRId %08X Config %08X", by_va(0), by_va(1), by_va(2));
    Say(L"probe: again through KSEG0 (as the hand-over runs)");
    ReadC0Fn by_kseg0 = (ReadC0Fn)KSEG0(code_pa + TRAMP_READ_C0);
    Say(L"probe: Status %08X PRId %08X Config %08X", by_kseg0(0), by_kseg0(1), by_kseg0(2));
    Say(L"probe: KSEG1 RAM word at 0: %08X", *(volatile DWORD *)KSEG1(0));

    for (mb = 1; mb <= 12; mb++) {
        void *p = VirtualAlloc(NULL, mb << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p)
            break;
        VirtualFree(p, 0, MEM_RELEASE);
    }
    Say(L"probe: allocations up to %u MiB succeed", mb - 1);
    for (n = 0; n < sizeof(pieces) / sizeof(pieces[0]) && (pieces[n] = PagesAlloc(CHUNK_PAGES)); n++)
        for (i = 0; i < CHUNK_PAGES; i++)
            pieces[n][i * GB_PAGE] = 0;
    Say(L"probe: %u KiB in %u KiB pieces (romfs needs %u KiB)", n * CHUNK_PAGES * GB_PAGE / 1024,
        CHUNK_PAGES * GB_PAGE / 1024, RomfsKiB());
    while (n)
        VirtualFree(pieces[--n], 0, MEM_RELEASE);
    Alert(L"Probe done: Status, Config and memory are in glazeboot.log.");
}

/* The boot trace (glaze/bootinfo.h): its three rows black, so that every
 * block shows whatever Windows CE drew there, then block 0, white: the
 * loader is about to call the trampoline. Needs kernel mode for KSEG1. */
static void Trace0(void)
{
    volatile WORD *fb = (volatile WORD *)GZ_TRACE_FB;
    DWORD x, y;

    for (y = 0; y < 42; y++)
        for (x = 0; x < 240; x++)
            fb[y * GZ_TRACE_STRIDE + x] = 0;
    for (y = 0; y < 12; y++)
        for (x = 2; x < 18; x++)
            fb[y * GZ_TRACE_STRIDE + x] = 0xFFFF;
}

/* ---- the boot -------------------------------------------------------------------- */

static int Boot(void)
{
    Buf kernel, romfs, info;
    gz_kernel_bin kh;
    DWORD i, code_pa;

    BorrowStore();
    if (!ReadWholeFile(cfg.kernel, &kernel) || !ReadWholeFile(cfg.romfs, &romfs))
        return 1;
    memcpy(&kh, kernel.p, sizeof(kh));
    DWORD load = kh.load & 0x1FFFFFFF, end = ((kh.end & 0x1FFFFFFF) + GB_PAGE - 1) & ~(GB_PAGE - 1);
    if (kh.magic != GZ_KERNEL_BIN_MAGIC || kh.size + GZ_KERNEL_BIN_HEADER > kernel.size ||
        (load % GB_PAGE) || load < GZ_BOOTINFO_PA + GB_PAGE || end < load + kh.size) {
        Alert(L"%s is not a Glaze kernel (tools/mkkernelbin.py).", cfg.kernel);
        return 1;
    }
    if (romfs.size < 8 || memcmp(romfs.p, "GZRF", 4)) {
        Alert(L"%s is not a Glaze romfs archive.", cfg.romfs);
        return 1;
    }
    Say(L"kernel: %u bytes at %08X, entry %08X, ends %08X", kh.size, kh.load, kh.entry, kh.end);

    /* The layout (glaze/bootinfo.h): romfs at the top, the RAM disk below. */
    DWORD ramfs_kb = cfg.ramfs_kb < 128 ? 128 : cfg.ramfs_kb;
    DWORD romfs_pa = (SDRAM_BYTES - romfs.pages * GB_PAGE) & ~0x1FFFUL;
    DWORD ramfs_len = (ramfs_kb * 1024 + RAMFS_BLOCK - 1) & ~(RAMFS_BLOCK - 1);
    if (romfs_pa < ramfs_len || romfs_pa - ramfs_len < end || ramfs_len / RAMFS_BLOCK > GZ_FLASH_BLOCKS) {
        Alert(L"romfs (%u KiB) and a %u KiB RAM disk don't fit in RAM.", romfs.size / 1024, ramfs_kb);
        return 1;
    }
    DWORD ramfs_pa = romfs_pa - ramfs_len;
    Say(L"layout: romfs %08X-%08X, RAM disk %08X-%08X", romfs_pa, romfs_pa + romfs.pages * GB_PAGE,
        ramfs_pa, romfs_pa);

    if (!BufAlloc(&info, sizeof(gz_bootinfo), INVALID_HANDLE_VALUE)) {
        Alert(L"Out of memory.");
        return 1;
    }
    gz_bootinfo *bi = (gz_bootinfo *)info.p;
    memset(bi, 0, sizeof(*bi));
    bi->magic = GZ_BOOTINFO_MAGIC;
    bi->version = GZ_BOOTINFO_VERSION;
    bi->flags = cfg.nand == NAND_RW ? GZ_BOOT_NAND_RW : cfg.nand == NAND_OFF ? GZ_BOOT_NAND_OFF : 0;
    bi->romfs_pa = romfs_pa;
    bi->romfs_size = romfs.size;
    bi->ramfs_pa = ramfs_pa;
    bi->ramfs_blocks = ramfs_len / RAMFS_BLOCK;

    /* Destinations first, so the plan's own pages can avoid them. */
    DWORD kernel_pages = (kh.size + GB_PAGE - 1) / GB_PAGE;
    MarkDst(GZ_BOOTINFO_PA, GB_PAGE);
    MarkDst(load, kh.size);
    MarkDst(romfs_pa, romfs.pages * GB_PAGE);
    DWORD moves = 1 + kernel_pages + romfs.pages;
    DWORD move_page_count = (moves + GB_MOVES_PER_PAGE - 1) / GB_MOVES_PER_PAGE;
    if (move_page_count > GB_MAX_MOVE_PAGES || !AllocSafe(&ctrl_page)) {
        Alert(L"Out of memory for the hand-over.");
        return 1;
    }
    control = (gb_control *)ctrl_page.p;
    memset(control, 0, sizeof(*control));
    control->magic = GB_CONTROL_MAGIC;
    for (i = 0; i < move_page_count; i++) {
        if (!AllocSafe(&move_pages[i])) {
            Alert(L"Out of memory for the hand-over.");
            return 1;
        }
        control->move_frames[i] = PaOf(move_pages[i].pfn[0]) / GB_PAGE;
    }
    for (i = 0; i < GB_WHO_PAGES; i++) {
        if (!AllocSafe(&who_pages[i])) {
            Alert(L"Out of memory for the hand-over.");
            return 1;
        }
        control->who_frames[i] = PaOf(who_pages[i].pfn[0]) / GB_PAGE;
    }

    /* The kernel image starts on kernel.bin's second page. */
    BOOL ok = AddMove(PaOf(info.pfn[0]), GZ_BOOTINFO_PA);
    for (i = 0; i < kernel_pages; i++)
        ok = AddMove(PaOf(kernel.pfn[1 + i]), load + i * GB_PAGE) && ok;
    for (i = 0; i < romfs.pages; i++)
        ok = AddMove(PaOf(romfs.pfn[i]), romfs_pa + i * GB_PAGE) && ok;
    if (!ok) {
        Alert(L"Memory outside the 16 MiB of SDRAM; can't hand over.");
        return 1;
    }
    control->entry = kh.entry;
    control->arg = KSEG0(GZ_BOOTINFO_PA);
    if (!LoadTrampoline(&code_pa)) {
        Alert(L"Out of memory for the trampoline.");
        return 1;
    }
    DWORD ctrl_pa = PaOf(ctrl_page.pfn[0]);
    Say(L"plan: %u moves; trampoline %u bytes at %08X, control at %08X", control->count, TRAMP_BYTES,
        code_pa, ctrl_pa);

    if (cfg.confirm && !Confirm()) {
        Say(L"cancelled");
        return 0;
    }
    Say(L"handing over");
    CloseHandle(log_file);
    log_file = INVALID_HANDLE_VALUE;
    Sleep(500);                 /* let the card and the serial port drain */

    KernelMode();
    Trace0();
    pCacheSync(CACHE_SYNC_DISCARD | CACHE_SYNC_INSTRUCTIONS);
    ((void (*)(DWORD))KSEG0(code_pa + TRAMP_ENTRY))(KSEG0(ctrl_pa));
    return 1;                   /* not reached */
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WCHAR path[MAX_PATH];
    Buf test;
    int rv = 1;

    app = instance;
    usb_unattended = cmdline && (!lstrcmp(cmdline, L"--usb-boot") || !lstrcmp(cmdline, L"--usb-probe"));
    DWORD n = GetModuleFileName(instance, dir, MAX_PATH - 1);
    while (n && dir[n - 1] != L'\\')
        n--;
    dir[n] = 0;
    PathOf(path, L"glazeboot.log");
    log_file = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!usb_unattended)
        SerialOpen();
    Say(L"GlazeBoot, built " WIDEN(__DATE__));
    Say(L"boot folder: %s", dir);
    CfgRead();
    if (usb_unattended) {
        cfg.confirm = 0;
        cfg.probe = !lstrcmp(cmdline, L"--usb-probe");
        Say(L"USB unattended %s", cfg.probe ? L"probe" : L"boot");
    }
    Say(L"cfg: kernel=%s romfs=%s ramfs_kb=%u nand=%s confirm=%u countdown=%u mode=%s", cfg.kernel,
        cfg.romfs, cfg.ramfs_kb, cfg.nand == NAND_RW ? L"rw" : cfg.nand == NAND_RO ? L"ro" : L"off", cfg.confirm, cfg.countdown, cfg.probe ? L"probe" : L"boot");

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    Say(L"system: page size %u, memory %08X-%08X", si.dwPageSize, (DWORD)si.lpMinimumApplicationAddress,
        (DWORD)si.lpMaximumApplicationAddress);
    if (!ImportKernelCalls())
        Alert(L"This Windows CE lacks LockPages, SetKMode or CacheSync.");
    else {
        MemoryReport();
        KernelMode();
        if (!BufAlloc(&test, GB_PAGE, INVALID_HANDLE_VALUE) || !FindPfnShift(&test))
            Alert(L"Can't find the physical address of memory; can't hand over.");
        else if (cfg.probe) {
            Probe();
            rv = 0;
        } else
            rv = Boot();
    }
    if (log_file != INVALID_HANDLE_VALUE)
        CloseHandle(log_file);
    return rv;
}
