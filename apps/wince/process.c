/* CE processes for direct NAND launches. Another executable starts as its
 * own Glaze app through SpringBoard (sb_wince_launch_args), in the CE
 * context Files would give it; its command line reaches the new instance's
 * GetCommandLineW and WinMain. A launch of a running app brings it forward,
 * as CE apps find their earlier window. Documents and URLs open with the
 * application the CE registry names (HKCR\<.ext> -> HKCR\<class>\Shell\Open\Command),
 * as ShellExecuteEx does on the device.
 * CreateProcess: https://learn.microsoft.com/en-us/previous-versions/windows/embedded/ms885189(v=msdn.10)
 * SHELLEXECUTEINFO: https://learn.microsoft.com/en-us/previous-versions/windows/embedded/ms885640(v=msdn.10) */
#include "runtime.h"
#include "source.h"
#include <glaze/notify.h>
#include <glaze/sb.h>

static WCHAR command[64];
static bool command_loaded;

/* The command line SpringBoard left in this context (read once, removed). */
const WCHAR *ce_command_line(void)
{
    if (command_loaded)
        return command;
    command_loaded = true;
    char path[FS_PATH_MAX], text[48];
    snprintf(path, sizeof(path), "%s/cmdline", res_bundle());
    int n = fs_read_file(path, text, sizeof(text) - 1);
    if (n > 0) {
        text[n] = 0;
        ce_widen(command, ARRAY_SIZE(command), text);
        fs_remove(path);
    }
    return command;
}
CE_API(GetCommandLineW)
{
    return WORD(ce_command_line());
}

/* Child process handles: a launch cannot report the child's exit, so they
 * stay STILL_ACTIVE (259) until closed. */
#define CE_PROCESS_HANDLE 0x7d000000u
static unsigned children;
bool ce_process_close(CW handle, bool *handled)
{
    *handled = (handle & 0xff000000u) == CE_PROCESS_HANDLE;
    return *handled;
}

/* Starts `image` (a CE path or name) with UTF-16 arguments `args`. */
static bool launch(const WCHAR *image, const WCHAR *args)
{
    char canonical[FS_PATH_MAX], found[FS_PATH_MAX], text[48] = "";
    if (!image || !ce_source_active() || !ce_canonical_path(canonical, image) ||
        !ce_source_executable(canonical, found)) {
        ce_error = 2; /* ERROR_FILE_NOT_FOUND */
        return false;
    }
    if (args)
        ce_narrow(text, sizeof(text), args);
    int rv = sb_wince_launch_args(found, text);
    if (rv) {
        printf("wince: cannot start %s (%d)\n", found, rv);
        ce_error = rv == GZ_ENOMEM ? 8 : 5;
        return false;
    }
    printf("wince: started %s %s\n", found, text);
    return true;
}
/* A command line's executable: quoted, or up to the first space. */
static const WCHAR *split_command(const WCHAR *line, WCHAR *exe, unsigned capacity)
{
    unsigned n = 0;
    while (*line == ' ')
        line++;
    bool quoted = *line == '"';
    if (quoted)
        line++;
    while (*line && (quoted ? *line != '"' : *line != ' ') && n + 1 < capacity)
        exe[n++] = *line++;
    exe[n] = 0;
    if (quoted && *line == '"')
        line++;
    while (*line == ' ')
        line++;
    return line;
}

/* CreateProcessW(image, command line, 4 ignored, environment, directory,
 * startup info, PROCESS_INFORMATION). With no image name, the command line's
 * first word is the executable. */
CE_API(CreateProcessW)
{
    WCHAR exe[FS_PATH_MAX];
    const WCHAR *image = PTR(WCHAR, a0), *args = PTR(WCHAR, a1);
    if (!image && args) {
        args = split_command(args, exe, ARRAY_SIZE(exe));
        image = exe;
    }
    if (!launch(image, args))
        return 0;
    if (a9) {
        CW *info = PTR(CW, a9);
        info[0] = CE_PROCESS_HANDLE | ++children;
        info[1] = CE_PROCESS_HANDLE | 0x800000u | children;
        info[2] = info[0];
        info[3] = info[1];
    }
    return 1;
}
CE_API(OpenProcess)
{
    if (a2 == (CW)gz_getpid() || a2 == 0x42u)
        return 0x42u;
    if ((a2 & 0xff000000u) == CE_PROCESS_HANDLE)
        return a2;
    ce_error = 87;
    return 0;
}
CE_API(GetExitCodeProcess)
{
    if (!a1 || (a0 != 0x42u && (a0 & 0xff000000u) != CE_PROCESS_HANDLE)) {
        ce_error = 6;
        return 0;
    }
    *PTR(CW, a1) = 259; /* STILL_ACTIVE */
    return 1;
}

/* The command a document or URL opens with: HKCR\<.ext or scheme> names a
 * class (or is one) whose Shell\Open\Command holds "app %1". */
static bool open_command(const WCHAR *file, WCHAR *exe, unsigned capacity, WCHAR *args, unsigned args_capacity)
{
    char name[FS_PATH_MAX], key[160], klass[64];
    ce_narrow(name, sizeof(name), file);
    const char *colon = strchr(name, ':'), *dot = strrchr(name, '.'), *slash = strrchr(name, '\\');
    if (colon && colon - name > 1 && colon - name < 16) /* a URL scheme, not a drive */
        snprintf(klass, sizeof(klass), "%.*s", (int)(colon - name), name);
    else if (dot && (!slash || dot > slash)) {
        WCHAR value[64];
        snprintf(key, sizeof(key), "HKCR\\%s", dot);
        if (!ce_registry_string(key, "", value, ARRAY_SIZE(value)))
            return false;
        ce_narrow(klass, sizeof(klass), value);
    } else
        return false;
    WCHAR line[FS_PATH_MAX];
    snprintf(key, sizeof(key), "HKCR\\%s\\Shell\\Open\\Command", klass);
    if (!ce_registry_string(key, "", line, ARRAY_SIZE(line)))
        return false;
    const WCHAR *rest = split_command(line, exe, capacity);
    unsigned n = 0;
    for (; *rest && n + 1 < args_capacity; rest++) {
        if (rest[0] == '%' && rest[1] == '1') { /* the document */
            for (const WCHAR *f = file; *f && n + 1 < args_capacity;)
                args[n++] = *f++;
            rest++;
        } else {
            args[n++] = *rest;
        }
    }
    args[n] = 0;
    return true;
}
/* ShellExecuteEx(SHELLEXECUTEINFO): an executable starts directly; other
 * files and URLs open with their registered application. SEE_MASK_NOCLOSEPROCESS
 * (0x40) returns a process handle at offset 56. */
CE_API(ShellExecuteEx)
{
    const uint8_t *info = PTR(uint8_t, a0);
    if (!info || ce_u32(info) < 60) {
        ce_error = 87;
        return 0;
    }
    const WCHAR *file = PTR(WCHAR, ce_u32(info + 16)), *params = PTR(WCHAR, ce_u32(info + 20));
    if (!file) {
        ce_error = 87;
        return 0;
    }
    char narrow[FS_PATH_MAX];
    ce_narrow(narrow, sizeof(narrow), file);
    size_t len = strlen(narrow);
    bool ok;
    if (len > 4 && !strcasecmp(narrow + len - 4, ".exe")) {
        ok = launch(file, params);
    } else {
        WCHAR exe[FS_PATH_MAX], args[FS_PATH_MAX];
        ok = open_command(file, exe, ARRAY_SIZE(exe), args, ARRAY_SIZE(args)) && launch(exe, args);
        if (!ok && !ce_error)
            ce_error = 1155; /* ERROR_NO_ASSOCIATION */
    }
    if (ok) {
        ce_put32(PTR(uint8_t, a0) + 32, 33); /* hInstApp: success is above 32 */
        if (ce_u32(info + 4) & 0x40u)
            ce_put32(PTR(uint8_t, a0) + 56, CE_PROCESS_HANDLE | ++children);
    }
    return ok;
}
/* COShellApi's CoshExecute(window, executable, arguments). */
CE_API(CoshExecute)
{
    return launch(PTR(WCHAR, a1), PTR(WCHAR, a2));
}

/* CoshMainMenuExec(window): return to Casio's main menu, the program named by
 * HKLM\SOFTWARE\CASIO\COShell\MainMenu (File, Args); without one, Glaze's
 * home screen, which is that menu's place here. */
CE_API(CoshMainMenuExec)
{
    WCHAR file[FS_PATH_MAX], args[64];
    const char *key = "HKLM\\SOFTWARE\\CASIO\\COShell\\MainMenu";
    if (ce_registry_string(key, "File", file, ARRAY_SIZE(file)) && file[0]) {
        if (!ce_registry_string(key, "Args", args, ARRAY_SIZE(args)))
            args[0] = 0;
        return launch(file, args);
    }
    struct gz_msg m = {{0}}, r = {{0}};
    m.w[0] = SB_GO_HOME;
    m.w[3] = (uint32_t)gz_getpid();
    return gz_send(ui.sb_port, &m, &r) >= 0 && !r.w[0];
}
/* CoshNotifyIconSys: COShell's tray icons; Glaze's status bar has none. */
CE_API(CoshNotifyIconSys) { return 0; }
void *ce_resolve_process(const char *dll, const char *name)
{
    static const struct ce_export core[] = {CE_EXPORT(GetCommandLineW), CE_EXPORT(CreateProcessW),
        CE_EXPORT(OpenProcess), CE_EXPORT(GetExitCodeProcess), CE_EXPORT(ShellExecuteEx)};
    if (!strcasecmp(dll, "coredll.dll")) {
        for (unsigned i = 0; i < ARRAY_SIZE(core); i++)
            if (!strcmp(name, core[i].name))
                return core[i].address;
    } else if (!strcasecmp(dll, "COShellApi.dll")) {
        if (!strcmp(name, "CoshExecute"))
            return (void *)ce_CoshExecute;
        if (!strcmp(name, "CoshNotifyIconSys"))
            return (void *)ce_CoshNotifyIconSys;
        if (!strcmp(name, "CoshMainMenuExec"))
            return (void *)ce_CoshMainMenuExec;
    }
    return NULL;
}
