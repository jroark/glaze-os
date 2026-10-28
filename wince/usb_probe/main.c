/* Harmless bootstrap validation: a small file beside the executable, then
 * return to CE. No kernel mode, raw disk access, UI or hardware changes. */
#include <windows.h>
int WINAPI WinMain(HINSTANCE app, HINSTANCE previous, LPWSTR command, int show)
{
    (void)previous; (void)show;
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileName(app, path, MAX_PATH - 32);
    while (n && path[n - 1] != L'\\') n--;
    lstrcpy(path + n, L"usb-probe.txt");
    HANDLE f = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 1;
    char text[100] = "Glaze USB CE execution verified: ";
    DWORD size = sizeof("Glaze USB CE execution verified: ") - 1;
    for (DWORD i = 0; command && command[i] && i < 32; i++)
        text[size++] = command[i] < 128 ? (char)command[i] : '?';
    text[size++] = '\r'; text[size++] = '\n';
    DWORD done = 0;
    BOOL ok = WriteFile(f, text, size, &done, NULL);
    CloseHandle(f);
    return ok && done == size ? 0 : 2;
}
