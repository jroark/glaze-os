Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
Name "Glaze BE-300 Installer"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Glaze Installer"
RequestExecutionLevel user
SetCompressor /SOLID lzma
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\Glaze Installer.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open Glaze BE-300 Installer"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK "This installer requires 64-bit Windows 10 or later."
    Abort
  ${EndIf}
FunctionEnd
Section "Glaze Installer"
  SetOutPath "$INSTDIR"
  File /r "${PRODUCT_DIR}/*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\Glaze BE-300 Installer.lnk" "$INSTDIR\Glaze Installer.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\GlazeInstaller" "DisplayName" "Glaze BE-300 Installer"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\GlazeInstaller" "DisplayVersion" "0.1.0"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\GlazeInstaller" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
SectionEnd
Section "Uninstall"
  Delete "$SMPROGRAMS\Glaze BE-300 Installer.lnk"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\GlazeInstaller"
  RMDir /r "$INSTDIR"
SectionEnd
