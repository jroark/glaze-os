/* GUI launcher for the private, bundled CPython runtime. No system Python. */
#include <windows.h>
#include <wchar.h>
#include <stdlib.h>
int WINAPI wWinMain(HINSTANCE a,HINSTANCE b,PWSTR args,int show) {
    (void)a;(void)b;(void)show;
    wchar_t base[32768],python[32768],command[65536];
    DWORD n=GetModuleFileNameW(NULL,base,32768);
    if(!n || n>=32768) return 1;
    wchar_t *slash=wcsrchr(base,L'\\'); if(!slash)return 1; *slash=0;
    if(swprintf(python,32768,L"%ls\\runtime\\pythonw.exe",base)<0 ||
       swprintf(command,65536,L"\"%ls\" \"%ls\\app\\app.py\" --resources \"%ls\\resources\" %ls",python,base,base,args)<0) return 1;
    STARTUPINFOW si={0};si.cb=sizeof(si);PROCESS_INFORMATION pi={0};
    if(!CreateProcessW(python,command,NULL,NULL,FALSE,0,NULL,base,&si,&pi)) {
        MessageBoxW(NULL,L"Unable to start the bundled Glaze installer runtime. Reinstall this download.",L"Glaze Installer",MB_OK|MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return 0;
}
