/* Veo Photo Traveler check under stock Windows CE (docs/hardware/VEO.md).
 *
 * VeoTest install  registers Veo's own veocam.dll (staged beside this
 *                  program) for the card, as Veo's CAB does; reinsert
 *                  the card afterwards so pcmcia.dll loads it.
 * VeoTest capture  opens CAM1: and asks the driver for one still per mode
 *                  through its IOCTL 9; the driver writes each JPEG itself.
 *
 * VeoTest hw       skips Veo's driver: in kernel mode (as GlazeBoot does)
 *                  it logs CE's PCMCIA socket registers, then runs
 *                  Glaze's register test (kernel/drivers/veo.c) on the card
 *                  as CE's pcmcia.dll left the socket.
 * VeoTest uninstall removes the install keys.
 * VeoTest app-install installs Veo's app and driver (staged beside this
 *                  program) as Casio's installer would: files under
 *                  \Nand Disk\Program Files\VeoTraveler, Veo's CAB registry
 *                  values, and a Top Menu entry (HKLM\SOFTWARE\MENU), then
 *                  saves the registry; it appears after a soft reset.
 * VeoTest app-uninstall removes all of that.
 * VeoTest cis      logs the inserted card's CIS tuples (kernel mode) and the
 *                  active drivers.
 * VeoTest pcmcia   logs CE's PCMCIA registry keys, the active drivers and the
 *                  socket register words (read only).
 * VeoTest snap     logs every register block pcmcia.dll maps (read only).
 * VeoTest ne       reads the NE2000's command register at 0x0A00C300 (read only).
 * VeoTest snap1    logs the socket 1 registers pcmcia.dll reads (read only).
 * VeoTest s1probe  logs socket 1's registers and reads its candidate card
 *                  windows (CIS, NE2000 command register), read only.
 * VeoTest menu     logs the menu, installer and CompDisk registry keys and the
 *                  program folders.
 *
 * Everything is logged to veo-test.txt beside the program. Veo's capture
 * loop never returns if the camera sends no data, so each request runs on
 * a worker thread with a timeout. Only hw touches hardware directly. */
#include <windows.h>

static HANDLE log_file;
static WCHAR folder[MAX_PATH];

static void say(const WCHAR *format, ...)
{
    WCHAR w[300];
    char a[300];
    va_list ap;
    va_start(ap, format);
    int n = wvsprintf(w, format, ap);
    va_end(ap);
    for (int i = 0; i < n && i < 296; i++) a[i] = w[i] < 128 ? (char)w[i] : '?';
    if (n > 296) n = 296;
    a[n++] = '\r'; a[n++] = '\n';
    DWORD done;
    WriteFile(log_file, a, (DWORD)n, &done, NULL);
    FlushFileBuffers(log_file);
}

/* The CAB's PnP ID, and the one veocam.dll's DetectVEOCam returns. */
static const WCHAR *const install_keys[] = {
    L"Drivers\\PCMCIA\\VEO-CFTraveler622-FE86",
    L"Drivers\\PCMCIA\\VEO-CFTraveler622-8046",
};

static void set_string(HKEY key, const WCHAR *name, const WCHAR *value)
{
    LONG r = RegSetValueEx(key, name, 0, REG_SZ, (const BYTE *)value,
                           (DWORD)(lstrlen(value) + 1) * sizeof(WCHAR));
    say(L"  %s = %s (%ld)", name, value, r);
}

static void install(void)
{
    const WCHAR *const *ids = install_keys;
    WCHAR dll[MAX_PATH];
    wsprintf(dll, L"%sveocam.dll", folder);
    if (GetFileAttributes(dll) == 0xFFFFFFFF) { say(L"missing %s", dll); return; }
    for (unsigned i = 0; i < 2; i++) {
        HKEY key; DWORD disp;
        LONG r = RegCreateKeyEx(HKEY_LOCAL_MACHINE, ids[i], 0, NULL, 0, 0, NULL, &key, &disp);
        say(L"HKLM\\%s: %ld", ids[i], r);
        if (r) continue;
        set_string(key, L"Dll", dll);
        set_string(key, L"Prefix", L"CAM");
        RegCloseKey(key);
    }
    say(L"installed; remove and reinsert the camera, then run capture");
}

static void list_active(void)
{
    HKEY active;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Drivers\\Active", 0, 0, &active)) return;
    for (DWORD i = 0; i < 64; i++) {
        WCHAR sub[64], name[64] = L"", key_path[128] = L"";
        DWORD n = 64;
        if (RegEnumKeyEx(active, i, sub, &n, NULL, NULL, NULL, NULL)) break;
        HKEY k;
        if (RegOpenKeyEx(active, sub, 0, 0, &k)) continue;
        DWORD size = sizeof(name), type;
        RegQueryValueEx(k, L"Name", NULL, &type, (BYTE *)name, &size);
        size = sizeof(key_path);
        RegQueryValueEx(k, L"Key", NULL, &type, (BYTE *)key_path, &size);
        BYTE sckt[4] = {0}; DWORD ssize = sizeof(sckt);
        LONG has = RegQueryValueEx(k, L"Sckt", NULL, &type, sckt, &ssize);
        say(L"  active %s: %s %s%s", sub, name, key_path, has ? L"" : L" (has Sckt)");
        RegCloseKey(k);
    }
    RegCloseKey(active);
}

struct request {
    HANDLE dev;
    DWORD code, in_size, out_size, returned;
    BYTE in[216];
    DWORD out[16];
    BOOL ok;
    DWORD error;
};

static DWORD WINAPI run_request(LPVOID arg)
{
    struct request *r = arg;
    r->ok = DeviceIoControl(r->dev, r->code, r->in_size ? r->in : NULL, r->in_size,
                            r->out_size ? r->out : NULL, r->out_size, &r->returned, NULL);
    r->error = GetLastError();
    return 0;
}

/* FALSE when it timed out: the driver thread may still be looping. */
static BOOL call(struct request *r, DWORD timeout_ms)
{
    HANDLE t = CreateThread(NULL, 0, run_request, r, 0, NULL);
    if (!t) { say(L"  CreateThread failed %lu", GetLastError()); return FALSE; }
    DWORD w = WaitForSingleObject(t, timeout_ms);
    CloseHandle(t);
    return w == WAIT_OBJECT_0;
}

static void capture(void)
{
    list_active();
    HANDLE dev = CreateFile(L"CAM1:", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE) {
        say(L"CAM1: did not open (%lu); the driver isn't loaded", GetLastError());
        return;
    }
    say(L"CAM1: open");
    static struct request r;
    memset(&r, 0, sizeof(r));
    r.dev = dev; r.code = 1; r.out_size = 4;
    if (call(&r, 5000))
        say(L"IOCTL 1: ok %d, returned %lu, value %lx", r.ok, r.returned, r.out[0]);
    /* Smallest first: a dead sensor hangs the driver's capture loop. */
    static const int modes[] = {3, 2, 1};
    for (unsigned i = 0; i < 3; i++) {
        memset(&r, 0, sizeof(r));
        r.dev = dev; r.code = 9; r.in_size = 216; r.out_size = 4;
        r.in[0] = (BYTE)modes[i];
        r.in[1] = 1;    /* normal quality (bridge 0xB2 = 0x38) */
        WCHAR path[MAX_PATH];
        wsprintf(path, L"%sveo-mode%d.jpg", folder, modes[i]);
        DeleteFile(path);
        memcpy(r.in + 2, path, (lstrlen(path) + 1) * sizeof(WCHAR));
        /* in[0xCC..0xCF] = 0: save to the path (10 would skip saving). */
        DWORD t0 = GetTickCount();
        if (!call(&r, 20000)) {
            say(L"IOCTL 9 mode %d: no answer in 20 s; the camera sent no frame", modes[i]);
            say(L"the driver may still be looping: reset the BE-300 before retrying");
            break;
        }
        say(L"IOCTL 9 mode %d: ok %d, error %lu, %lu ms, file %s", modes[i], r.ok, r.error,
            GetTickCount() - t0, GetFileAttributes(path) == 0xFFFFFFFF ? L"missing" : L"written");
    }
    CloseHandle(dev);
}

static void uninstall(void)
{
    for (unsigned i = 0; i < 2; i++)
        say(L"delete HKLM\\%s: %ld", install_keys[i], RegDeleteKey(HKEY_LOCAL_MACHINE, install_keys[i]));
}

/* ---- direct register test, mirroring kernel/drivers/veo.c ---- */
#define KSEG1(pa)   (0xA0000000UL | (pa))
#define R32(pa)     (*(volatile DWORD *)KSEG1(pa))
#define R16(pa)     (*(volatile WORD *)KSEG1(pa))
#define CAM         0x0A00C300UL

static void wr(BYTE reg, BYTE v) { R16(CAM + 6) = (WORD)(reg << 8 | v); }
static BYTE rd(BYTE reg) { R16(CAM + 4) = (WORD)(reg << 8); return (BYTE)R16(CAM + 4); }

static const BYTE slot_reg[5] = {0x27, 0x2D, 0x2E, 0x2F, 0x30};
struct bus { WORD ops; unsigned n; BYTE *result; };
static void bus_run(struct bus *b)
{
    WORD ops = (WORD)(b->ops | (0xFFFFu << (3 * b->n)));
    wr(0x1F, 0); wr(0x20, (BYTE)ops); wr(0x1F, (BYTE)(ops >> 8));
    DWORD t0 = GetTickCount();
    while ((rd(0x0D) & 0x80) && GetTickCount() - t0 < 20)
        ;
    if (b->result) { *b->result = rd(0x26); b->result = NULL; }
    b->ops = 0; b->n = 0;
}
static void step(struct bus *b, unsigned op, BYTE byte, BYTE *result)
{
    unsigned code = op & 7;
    if (b->n > 4 || (result && b->result)) bus_run(b);
    if (code == 0 || code == 3 || code == 4) wr(slot_reg[b->n], byte);
    b->ops |= (WORD)(code << (3 * b->n));
    b->n++;
    if (result) b->result = result;
}
static void address(struct bus *b, BYTE reg)
{
    step(b, 1, 0, NULL); step(b, 3, 0xAA, NULL); step(b, 4, 0xFF, NULL);
    step(b, 3, (BYTE)(reg << 1), NULL); step(b, 4, 0xFF, NULL);
}
static void sensor_write(BYTE reg, BYTE v)
{
    struct bus b = {0};
    address(&b, reg);
    step(&b, 3, v, NULL); step(&b, 4, 0xFF, NULL); step(&b, 2, 0, NULL);
    bus_run(&b);
}
static BYTE sensor_read(BYTE reg)
{
    struct bus b = {0};
    BYTE v = 0;
    address(&b, reg);
    step(&b, 1, 0, NULL); step(&b, 3, 0xAB, NULL); step(&b, 4, 0xFF, NULL);
    step(&b, 6 | 8, 0, &v); step(&b, 4, 0xFF, NULL); step(&b, 2, 0, NULL);
    bus_run(&b);
    return v;
}

static void hw(void)
{
    HMODULE core = LoadLibrary(L"coredll.dll");
    BOOL (*set_kmode)(BOOL) = core ? (BOOL (*)(BOOL))GetProcAddress(core, L"SetKMode") : NULL;
    if (!set_kmode) { say(L"no SetKMode"); return; }
    BOOL was = set_kmode(TRUE);
    /* The socket registers Glaze's cf.c logs at boot (note_boot_state). */
    say(L"socket CTRL %08lx RESET %08lx STATUS %08lx BRIDGE %08lx",
        R32(0x0A00A000), R32(0x0A00A004), R32(0x0A00A008), R32(0x0A00A01C));
    say(L"A010 %08lx A014 %08lx A018 %08lx A020 %08lx A024 %08lx A034 %08lx",
        R32(0x0A00A010), R32(0x0A00A014), R32(0x0A00A018), R32(0x0A00A020),
        R32(0x0A00A024), R32(0x0A00A034));
    say(L"card status %08lx windows %08lx gate 111C %08lx",
        R32(0x0A001000), R32(0x0A001B20), R32(0x0A00111C));
    static const BYTE regs[] = {0x00, 0x0D, 0x0F, 0x11, 0x19, 0x26, 0x2B, 0x2C, 0x90, 0x91, 0xB2, 0xE2};
    WCHAR line[200]; int n = 0;
    for (unsigned i = 0; i < sizeof(regs); i++) n += wsprintf(line + n, L" %02x=%02x", regs[i], rd(regs[i]));
    say(L"bridge before:%s", line);
    wr(0x27, 0x5A); BYTE a = rd(0x27); wr(0x27, 0xA5); BYTE b = rd(0x27);
    say(L"slot 0x27 read-back: %02x %02x (%s)", a, b, a == 0x5A && b == 0xA5 ? L"ok" : L"FAILED");
    say(L"sensor ID before bring-up: %02x", sensor_read(0));

    static const WORD reset[] = {0x00C0, 0x0080, 0xB300, 0x1100, 0x1189, 0x2CBC, 0x2A01, 0x2BD8, 0x0F41};
    for (unsigned i = 0; i < sizeof(reset) / 2; i++) R16(CAM + 6) = reset[i];
    Sleep(30);
    wr(0x0F, 0x61); wr(0x19, 0xB1); wr(0x08, 0x00);
    wr(0x19, rd(0x19) | 0x04);
    wr(0xB0, 0x08); wr(0xB1, 0x38); wr(0xB2, 0x2C); wr(0xB3, 0x03); wr(0xE1, 0x09);
    BYTE id = sensor_read(0);
    say(L"sensor ID after bring-up: %02x (%s)", id, id == 0x18 ? L"camera responding" : L"no answer");
    for (int t = 0; t < 5 && id != 0x18; t++) { Sleep(200); id = sensor_read(0); }
    say(L"sensor ID after 1 s: %02x", id);
    if (id == 0x18) {
        sensor_write(0x1C, 0);
        n = 0;
        for (BYTE r = 0; r < 0x20; r++) n += wsprintf(line + n, L" %02x", sensor_read(r));
        say(L"sensor 00-1f:%s", line);
    }
    n = 0;
    for (unsigned i = 0; i < sizeof(regs); i++) n += wsprintf(line + n, L" %02x=%02x", regs[i], rd(regs[i]));
    say(L"bridge after:%s", line);
    set_kmode(was);
}

/* ---- Top Menu install, the shape of the device installer's entries
 * (be300 src/cbea_install.c reg_add_menu_entry) ---- */
#define APP_DIR     L"\\Nand Disk\\Program Files\\VeoTraveler"
#define MENU_KEY    L"SOFTWARE\\MENU\\VeoTraveler"
#define SETTINGS    L"Software\\Veo\\VeoTraveler\\Setting"

static void set_dword(HKEY key, const WCHAR *name, DWORD value)
{
    LONG r = RegSetValueEx(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    say(L"  %s = %lu (%ld)", name, value, r);
}

static HKEY create(HKEY root, const WCHAR *path)
{
    HKEY key; DWORD disp;
    LONG r = RegCreateKeyEx(root, path, 0, NULL, 0, 0, NULL, &key, &disp);
    say(L"%s\\%s: %ld", root == HKEY_LOCAL_MACHINE ? L"HKLM" : L"HKCU", path, r);
    return r ? NULL : key;
}

static BOOL copy_in(const WCHAR *name)
{
    WCHAR from[MAX_PATH], to[MAX_PATH];
    wsprintf(from, L"%s%s", folder, name);
    wsprintf(to, L"%s\\%s", APP_DIR, name);
    BOOL ok = CopyFile(from, to, FALSE);
    say(L"copy %s -> %s: %s (%lu)", from, to, ok ? L"ok" : L"FAILED", ok ? 0 : GetLastError());
    return ok;
}

static void list_dir(const WCHAR *dir);

/* Saves the RAM registry to \Nand Disk\Backup\System.reg, as Casio's
 * CoInstall.exe does after an install (it then soft-resets). CE's
 * RegFlushKey returns ERROR_NOT_SUPPORTED here. */
static void flush_registry(void)
{
    HMODULE dll = LoadLibrary(L"GetDisk.dll");
    void (*flush)(void) = dll ? (void (*)(void))GetProcAddress(dll, L"CGDFlushRegistry") : NULL;
    if (!flush) { say(L"GetDisk.dll CGDFlushRegistry: not found (%lu)", GetLastError()); return; }
    flush();
    say(L"CGDFlushRegistry: called");
}

static void app_install(void)
{
    CreateDirectory(L"\\Nand Disk\\Program Files", NULL);
    CreateDirectory(APP_DIR, NULL);
    if (!copy_in(L"VeoTraveler.exe") || !copy_in(L"veocam.dll")) return;
    if (GetFileAttributes(L"\\My Documents") == 0xFFFFFFFF)
        say(L"create \\My Documents: %d", CreateDirectory(L"\\My Documents", NULL));

    /* Veo's driver, loaded from the installed copy. */
    for (unsigned i = 0; i < 2; i++) {
        HKEY k = create(HKEY_LOCAL_MACHINE, install_keys[i]);
        if (!k) continue;
        set_string(k, L"Dll", APP_DIR L"\\veocam.dll");
        set_string(k, L"Prefix", L"CAM");
        RegCloseKey(k);
    }
    /* Veo's CAB defaults. */
    HKEY k = create(HKEY_CURRENT_USER, SETTINGS);
    if (k) {
        static const struct { const WCHAR *name; DWORD v; } d[] = {
            {L"avino", 1}, {L"jpgno", 1}, {L"resolution", 1}, {L"actionmode", 1},
            {L"recordwithaudio", 0}, {L"selftime", 0}, {L"imagequality", 1},
            {L"awb", 1}, {L"brightness", 50},
        };
        for (unsigned i = 0; i < sizeof(d) / sizeof(d[0]); i++) set_dword(k, d[i].name, d[i].v);
        set_string(k, L"defaultfolder", L"\\My Documents\\");
        RegCloseKey(k);
    }
    /* Installed apps launch from the \Program Disk view when it shows them. */
    const WCHAR *view = L"\\Program Disk\\VeoTraveler\\VeoTraveler.exe";
    BOOL in_view = GetFileAttributes(view) != 0xFFFFFFFF;
    say(L"%s: %s", view, in_view ? L"present" : L"absent, using the NAND path");
    k = create(HKEY_LOCAL_MACHINE, MENU_KEY);
    if (k) {
        static const WCHAR *const empty[] = {
            L"ACMND", L"ULOADFILE", L"SETTING_DLL", L"OP_DATA", L"SOUNDBMP", L"WAVE", L"ICON",
        };
        set_dword(k, L"APPNO", 8);
        set_dword(k, L"ULOAD", 1);
        set_string(k, L"ANAME", L"Photo Traveler");
        set_string(k, L"APATH", in_view ? view : APP_DIR L"\\VeoTraveler.exe");
        for (unsigned i = 0; i < sizeof(empty) / sizeof(empty[0]); i++) set_string(k, empty[i], L"");
        set_string(k, L"INSTALL_DIR", APP_DIR);
        RegCloseKey(k);
    }
    k = create(HKEY_LOCAL_MACHINE, MENU_KEY L"\\VERSION");
    if (k) { set_dword(k, L"MAJOR", 2); set_dword(k, L"MINOR", 0); RegCloseKey(k); }
    /* The BE-300 rebuilds its registry from \Nand Disk\Backup\System.reg at
     * every reset, and the Top Menu reads its entries only at boot. */
    list_dir(L"\\Nand Disk\\Backup");
    flush_registry();
    Sleep(3000);
    list_dir(L"\\Nand Disk\\Backup");
    say(L"installed: soft-reset to see Photo Traveler in the Top Menu");
}

static void app_uninstall(void)
{
    say(L"delete menu version: %ld", RegDeleteKey(HKEY_LOCAL_MACHINE, MENU_KEY L"\\VERSION"));
    say(L"delete menu entry: %ld", RegDeleteKey(HKEY_LOCAL_MACHINE, MENU_KEY));
    say(L"delete settings: %ld", RegDeleteKey(HKEY_CURRENT_USER, SETTINGS));
    uninstall();
    say(L"delete exe: %d", DeleteFile(APP_DIR L"\\VeoTraveler.exe"));
    say(L"delete dll: %d (fails while the driver is loaded)", DeleteFile(APP_DIR L"\\veocam.dll"));
    say(L"remove folder: %d", RemoveDirectory(APP_DIR));
    flush_registry();
}

/* Logs a key's values and, recursively, its subkeys. */
static void dump_key(HKEY root, const WCHAR *path, int depth)
{
    HKEY key;
    LONG r = RegOpenKeyEx(root, path, 0, 0, &key);
    if (r) { say(L"%s: open %ld", path, r); return; }
    say(L"[%s]", path);
    for (DWORD i = 0;; i++) {
        WCHAR name[64]; BYTE data[400]; DWORD nlen = 64, dlen = sizeof(data) - 2, type;
        if (RegEnumValue(key, i, name, &nlen, NULL, &type, data, &dlen)) break;
        if (type == REG_DWORD) say(L"  %s = dword %lu", name, *(DWORD *)data);
        else if (type == REG_SZ || type == REG_EXPAND_SZ) { data[dlen] = data[dlen + 1] = 0; say(L"  %s = \"%s\"", name, (WCHAR *)data); }
        else if (type == REG_MULTI_SZ) {
            data[dlen] = data[dlen + 1] = 0;
            for (WCHAR *w = (WCHAR *)data; *w; w += lstrlen(w) + 1) say(L"  %s = multi \"%s\"", name, w);
        } else say(L"  %s = type %lu, %lu bytes", name, type, dlen);
    }
    for (DWORD i = 0; depth < 3; i++) {
        WCHAR name[64], sub[MAX_PATH]; DWORD nlen = 64;
        if (RegEnumKeyEx(key, i, name, &nlen, NULL, NULL, NULL, NULL)) break;
        wsprintf(sub, L"%s\\%s", path, name);
        dump_key(root, sub, depth + 1);
    }
    RegCloseKey(key);
}

static void list_dir(const WCHAR *dir)
{
    WCHAR pattern[MAX_PATH]; WIN32_FIND_DATA f;
    wsprintf(pattern, L"%s\\*", dir);
    HANDLE h = FindFirstFile(pattern, &f);
    say(L"dir %s:%s", dir, h == INVALID_HANDLE_VALUE ? L" (none)" : L"");
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        SYSTEMTIME t;
        FileTimeToSystemTime(&f.ftLastWriteTime, &t);
        say(L"  %s%s %lu  %04u-%02u-%02u %02u:%02u:%02u", f.cFileName,
            f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? L"\\" : L"", f.nFileSizeLow,
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    } while (FindNextFile(h, &f));
    FindClose(h);
}

static void menu_dump(void)
{
    dump_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MENU", 0);
    dump_key(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SETINSTALL", 0);
    dump_key(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\CompDisk", 0);
    dump_key(HKEY_LOCAL_MACHINE, L"Loader", 0);
    list_dir(L"\\Program Disk");
    list_dir(L"\\Nand Disk\\Program Files");
    list_dir(L"\\Nand Disk\\Program Files\\VeoTraveler");
    list_dir(L"\\Nand Disk\\Backup");
}

/* Logs the inserted card's CIS from attribute memory (kernel/hw.h CF_CIS:
 * even bytes from 0x0A00D000, below the NAND port at 0x0A00D200). */
static void cis(void)
{
    HMODULE core = LoadLibrary(L"coredll.dll");
    BOOL (*set_kmode)(BOOL) = core ? (BOOL (*)(BOOL))GetProcAddress(core, L"SetKMode") : NULL;
    if (!set_kmode) { say(L"no SetKMode"); return; }
    BYTE t[0x100];
    BOOL was = set_kmode(TRUE);
    DWORD status = R32(0x0A00A008), a020 = R32(0x0A00A020);
    /* Read only: how pcmcia.dll powered this card (A034 is the socket's
     * supply selection, bits 2-5), for cards Glaze can't yet power. */
    DWORD ctrl = R32(0x0A00A000), reset = R32(0x0A00A004), a024 = R32(0x0A00A024);
    DWORD a028 = R32(0x0A00A028), a02c = R32(0x0A00A02C), a030 = R32(0x0A00A030);
    DWORD a034 = R32(0x0A00A034), card = R32(0x0A001000);
    for (unsigned i = 0; i < sizeof(t); i++) t[i] = *(volatile BYTE *)KSEG1(0x0A00D000UL + 2 * i);
    set_kmode(was);
    say(L"socket STATUS %08lx A020 %08lx", status, a020);
    say(L"socket CTRL %08lx RESET %08lx A024 %08lx A028 %08lx A02C %08lx A030 %08lx A034 %08lx card %08lx",
        ctrl, reset, a024, a028, a02c, a030, a034, card);
    for (unsigned i = 0; i < sizeof(t); i += 16) {
        WCHAR line[80]; int n = wsprintf(line, L"%03x:", i);
        for (unsigned j = 0; j < 16; j++) n += wsprintf(line + n, L" %02x", t[i + j]);
        say(L"%s", line);
    }
    for (unsigned i = 0; i + 1 < sizeof(t) && t[i] != 0xFF;) {
        unsigned code = t[i], len = t[i + 1];
        if (code == 0) { i++; continue; }
        WCHAR line[300]; int n = wsprintf(line, L"tuple %02x len %u:", code, len);
        for (unsigned j = 0; j < len && i + 2 + j < sizeof(t) && n < 280; j++) {
            BYTE c = t[i + 2 + j];
            n += (code == 0x15 && c >= 0x20 && c < 0x7F) ? wsprintf(line + n, L"%c", c) : wsprintf(line + n, L" %02x", c);
        }
        say(L"%s", line);
        i += 2 + len;
    }
    list_active();
}

/* Read only: sample the socket registers about every millisecond for 30 s
 * while a card is inserted, logging each change with the first CIS bytes,
 * to see how pcmcia.dll powers a card (a 5 V PC Card in a jacket) in the
 * window in which it reads the CIS. Never writes a register. */
static void watch(void)
{
    HMODULE core = LoadLibrary(L"coredll.dll");
    BOOL (*set_kmode)(BOOL) = core ? (BOOL (*)(BOOL))GetProcAddress(core, L"SetKMode") : NULL;
    if (!set_kmode) { say(L"no SetKMode"); return; }
    static const DWORD regs[] = {
        0x0A00A000, 0x0A00A004, 0x0A00A008, 0x0A00A01C, 0x0A00A020, 0x0A00A024,
        0x0A00A028, 0x0A00A02C, 0x0A00A030, 0x0A00A034, 0x0A001000, 0x0A001B20,
    };
#define NREGS (sizeof(regs) / sizeof(regs[0]))
    static struct { DWORD ms, v[NREGS]; BYTE cis[4]; WORD gpio[4]; } log[400];
    unsigned count = 0;
    DWORD last[NREGS + 3] = {0};
    say(L"watch: waiting up to 180 s for a card, then sampling 15 s");
    DWORD start = GetTickCount();
    BOOL was = set_kmode(TRUE);
    while ((R32(0x0A00A008) & 6) && GetTickCount() - start < 180000) {
        set_kmode(was);
        Sleep(2);
        set_kmode(TRUE);
    }
    start = GetTickCount();
    while (GetTickCount() - start < 15000 && count < 400) {
        DWORD v[NREGS + 3];
        for (unsigned i = 0; i < NREGS; i++) v[i] = R32(regs[i]);
        BYTE cis[4];
        for (unsigned i = 0; i < 4; i++) cis[i] = *(volatile BYTE *)KSEG1(0x0A00D000UL + 2 * i);
        v[NREGS] = cis[0] | cis[1] << 8 | cis[2] << 16 | (DWORD)cis[3] << 24;
        WORD gpio[4];
        for (unsigned i = 0; i < 4; i++) gpio[i] = R16(0x0F000140UL + 4 * i);
        v[NREGS + 1] = gpio[0] | (DWORD)gpio[1] << 16;
        v[NREGS + 2] = gpio[2] | (DWORD)gpio[3] << 16;
        if (!count || memcmp(v, last, sizeof(v))) {
            log[count].ms = GetTickCount() - start;
            memcpy(log[count].v, v, sizeof(log[count].v));
            memcpy(log[count].cis, cis, 4);
            memcpy(log[count].gpio, gpio, sizeof(gpio));
            count++;
            memcpy(last, v, sizeof(v));
        }
        set_kmode(was);
        Sleep(1);
        set_kmode(TRUE);
    }
    set_kmode(was);
    say(L"ms: CTRL RESET STATUS BRIDGE A020 A024 A028 A02C A030 A034 card win | CIS | GPIO 140 144 148 14C");
    for (unsigned n = 0; n < count; n++) {
        WCHAR line[300];
        int k = wsprintf(line, L"%5lu:", log[n].ms);
        for (unsigned i = 0; i < NREGS; i++) k += wsprintf(line + k, L" %lx", log[n].v[i]);
        k += wsprintf(line + k, L" | %02x %02x %02x %02x |", log[n].cis[0], log[n].cis[1], log[n].cis[2], log[n].cis[3]);
        for (unsigned i = 0; i < 4; i++) k += wsprintf(line + k, L" %x", log[n].gpio[i]);
        say(L"%s", line);
    }
#undef NREGS
}

/* Read only: how CE's PCMCIA layer reaches the inserted card: the socket
 * and NE2000 registry keys, the active drivers' keys, and every socket
 * register word from A000 and 1000. */
static void pcmcia_dump(void)
{
    dump_key(HKEY_LOCAL_MACHINE, L"Drivers\\PCMCIA", 1);
    dump_key(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\PCMCIA", 0);
    dump_key(HKEY_LOCAL_MACHINE, L"Drivers\\BuiltIn\\CardState", 0);
    dump_key(HKEY_LOCAL_MACHINE, L"Comm\\NE20001", 0);
    dump_key(HKEY_LOCAL_MACHINE, L"Comm\\NE2000", 0);
    dump_key(HKEY_LOCAL_MACHINE, L"Drivers\\Active", 1);
    HMODULE core = LoadLibrary(L"coredll.dll");
    BOOL (*set_kmode)(BOOL) = core ? (BOOL (*)(BOOL))GetProcAddress(core, L"SetKMode") : NULL;
    if (!set_kmode) { say(L"no SetKMode"); return; }
    DWORD a[16], b[16];
    BOOL was = set_kmode(TRUE);
    for (unsigned i = 0; i < 16; i++) a[i] = R32(0x0A00A000UL + 4 * i);
    for (unsigned i = 0; i < 16; i++) b[i] = R32(0x0A001000UL + 4 * i);
    set_kmode(was);
    for (unsigned i = 0; i < 16; i += 8)
        say(L"A%03x: %lx %lx %lx %lx %lx %lx %lx %lx", 4 * i, a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5], a[i + 6], a[i + 7]);
    for (unsigned i = 0; i < 16; i += 8)
        say(L"1%03x: %lx %lx %lx %lx %lx %lx %lx %lx", 4 * i, b[i], b[i + 1], b[i + 2], b[i + 3], b[i + 4], b[i + 5], b[i + 6], b[i + 7]);
}

/* Read only: every register block pcmcia.dll maps (its table at
 * 0x0198AA64) in the companion chip, as 16-bit words, to compare with a
 * card in and out. It also maps 0x0B000000-0x0B000557, read as 32-bit
 * words; those are left out until they are known to be safe to read. */
static void snap(void)
{
    static const struct { DWORD pa, size; } blocks[] = {
        {0x0A001000, 0x14}, {0x0A001B00, 0x5C}, {0x0A00A000, 0x40},
    };
    HMODULE core = LoadLibrary(L"coredll.dll");
    BOOL (*set_kmode)(BOOL) = core ? (BOOL (*)(BOOL))GetProcAddress(core, L"SetKMode") : NULL;
    if (!set_kmode) { say(L"no SetKMode"); return; }
    static WORD v[sizeof(blocks) / sizeof(blocks[0])][0x84 / 2];
    BOOL was = set_kmode(TRUE);
    for (unsigned b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++)
        for (unsigned i = 0; i < blocks[b].size / 2; i++) v[b][i] = R16(blocks[b].pa + 2 * i);
    set_kmode(was);
    for (unsigned b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++)
        for (unsigned i = 0; i < blocks[b].size / 2; i += 8) {
            WCHAR line[120]; int n = wsprintf(line, L"%08lx:", blocks[b].pa + 2 * i);
            for (unsigned j = i; j < i + 8 && j < blocks[b].size / 2; j++) n += wsprintf(line + n, L" %04x", v[b][j]);
            say(L"%s", line);
        }
    list_active();
}

/* Read only: whether CE's NE2000 (IoBaseAddress 0x300) answers in the card
 * I/O window Glaze uses (kernel/hw.h NE_BASE). Reads only the DP8390's
 * command register and page 0's ISR, which have no read side effects,
 * plus the socket and companion card status. */
static void ne_probe(void)
{
    HMODULE core = LoadLibrary(L"coredll.dll");
    BOOL (*set_kmode)(BOOL) = core ? (BOOL (*)(BOOL))GetProcAddress(core, L"SetKMode") : NULL;
    if (!set_kmode) { say(L"no SetKMode"); return; }
    BOOL was = set_kmode(TRUE);
    DWORD a008 = R32(0x0A00A008), a01c = R32(0x0A00A01C), c1000 = R32(0x0A001000), c1008 = R32(0x0A001008);
    BYTE cr = *(volatile BYTE *)KSEG1(0x0A00C300UL);
    BYTE isr = (cr & 0xC0) ? 0 : *(volatile BYTE *)KSEG1(0x0A00C307UL);
    set_kmode(was);
    say(L"A008 %lx A01C %lx 1000 %lx 1008 %lx", a008, a01c, c1000, c1008);
    say(L"C300 CR %02x ISR %02x%s", cr, isr, (cr & 0xC0) ? L" (not page 0)" : L"");
}

/* Read only: the socket 1 registers pcmcia.dll reads, as 32-bit words, at
 * just the offsets it uses (its mapping table at 0x0198AA64: 0x0B000500,
 * 0x0B000100, 0x0B000200, 0x0B000000 and the companion's 0x0A001000),
 * plus the CF socket's A008, to compare with a card in and out. */
static void snap1(void)
{
    static const DWORD regs[] = {
        0x0B000508, 0x0B000520, 0x0B000524,
        0x0B000108, 0x0B00010C, 0x0B000110, 0x0B00011C, 0x0B000128, 0x0B000140, 0x0B00014C, 0x0B000150,
        0x0B000204, 0x0B000208,
        0x0B000010,
        0x0A001000, 0x0A001008, 0x0A001044, 0x0A00104C,
        0x0A00A008,
    };
    HMODULE core = LoadLibrary(L"coredll.dll");
    BOOL (*set_kmode)(BOOL) = core ? (BOOL (*)(BOOL))GetProcAddress(core, L"SetKMode") : NULL;
    if (!set_kmode) { say(L"no SetKMode"); return; }
    DWORD v[sizeof(regs) / sizeof(regs[0])];
    BOOL was = set_kmode(TRUE);
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) v[i] = R32(regs[i]);
    set_kmode(was);
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) say(L"%08lx: %08lx", regs[i], v[i]);
    list_active();
}

/* Read only, with the card up under CE: socket 1's whole register blocks,
 * then its candidate card windows from pcmcia.dll's window table
 * (0x0198B9AC): CIS bytes at 0x0B600000 and 0x0B200000, and the NE2000's
 * command register at port 0x300 in 0x0B400000 and 0x0B200000. Each line
 * is logged before the next read, so a hang shows which access did it. */
static void s1probe(void)
{
    static const struct { DWORD pa, size; } blocks[] = {
        {0x0B000000, 0x14}, {0x0B000100, 0x58}, {0x0B000200, 0x10}, {0x0B000500, 0x58},
    };
    HMODULE core = LoadLibrary(L"coredll.dll");
    BOOL (*set_kmode)(BOOL) = core ? (BOOL (*)(BOOL))GetProcAddress(core, L"SetKMode") : NULL;
    if (!set_kmode) { say(L"no SetKMode"); return; }
    BOOL was = set_kmode(TRUE);
    DWORD detect = R32(0x0A001008);
    set_kmode(was);
    if (detect & 8) { say(L"no card on socket 1 (0x0A001008 %lx)", detect); return; }
    for (unsigned b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++)
        for (unsigned i = 0; i < blocks[b].size / 4; i += 8) {
            DWORD v[8]; unsigned n = 0;
            was = set_kmode(TRUE);
            for (unsigned j = i; j < i + 8 && j < blocks[b].size / 4; j++) v[n++] = R32(blocks[b].pa + 4 * j);
            set_kmode(was);
            WCHAR line[140]; int k = wsprintf(line, L"%08lx:", blocks[b].pa + 4 * i);
            for (unsigned j = 0; j < n; j++) k += wsprintf(line + k, L" %lx", v[j]);
            say(L"%s", line);
        }
    static const DWORD cis_at[] = {0x0B600000, 0x0B200000};
    for (unsigned w = 0; w < 2; w++) {
        say(L"reading CIS at %08lx", cis_at[w]);
        BYTE t[32];
        was = set_kmode(TRUE);
        for (unsigned i = 0; i < sizeof(t); i++) t[i] = *(volatile BYTE *)KSEG1(cis_at[w] + 2 * i);
        set_kmode(was);
        WCHAR line[140]; int k = wsprintf(line, L"  CIS:");
        for (unsigned i = 0; i < sizeof(t); i++) k += wsprintf(line + k, L" %02x", t[i]);
        say(L"%s", line);
    }
    static const DWORD io_at[] = {0x0B400300, 0x0B200300};
    for (unsigned w = 0; w < 2; w++) {
        say(L"reading NE2000 CR at %08lx", io_at[w]);
        was = set_kmode(TRUE);
        BYTE cr = *(volatile BYTE *)KSEG1(io_at[w]);
        set_kmode(was);
        say(L"  CR %02x", cr);
    }
}

int WINAPI WinMain(HINSTANCE app, HINSTANCE previous, LPWSTR command, int show)
{
    (void)previous; (void)show;
    DWORD n = GetModuleFileName(app, folder, MAX_PATH - 32);
    while (n && folder[n - 1] != L'\\') n--;
    folder[n] = 0;
    WCHAR path[MAX_PATH];
    wsprintf(path, L"%sveo-test.txt", folder);
    log_file = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE) return 1;
    say(L"VeoTest %s", command ? command : L"");
    if (command && !lstrcmp(command, L"install")) install();
    else if (command && !lstrcmp(command, L"capture")) capture();
    else if (command && !lstrcmp(command, L"hw")) hw();
    else if (command && !lstrcmp(command, L"uninstall")) uninstall();
    else if (command && !lstrcmp(command, L"app-install")) app_install();
    else if (command && !lstrcmp(command, L"app-uninstall")) app_uninstall();
    else if (command && !lstrcmp(command, L"menu")) menu_dump();
    else if (command && !lstrcmp(command, L"cis")) cis();
    else if (command && !lstrcmp(command, L"watch")) watch();
    else if (command && !lstrcmp(command, L"pcmcia")) pcmcia_dump();
    else if (command && !lstrcmp(command, L"snap")) snap();
    else if (command && !lstrcmp(command, L"ne")) ne_probe();
    else if (command && !lstrcmp(command, L"snap1")) snap1();
    else if (command && !lstrcmp(command, L"s1probe")) s1probe();
    else say(L"usage: VeoTest install|capture|hw|uninstall|app-install|app-uninstall|menu|cis");
    say(L"done");
    CloseHandle(log_file);
    return 0;
}
