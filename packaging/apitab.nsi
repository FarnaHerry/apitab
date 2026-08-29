; apitab.nsi - per-user NSIS installer for apitab.
; Build with: makensis /DAPP_VERSION=x.y.z packaging\apitab.nsi
Unicode true
!include "MUI2.nsh"
!ifndef APP_VERSION
  !error "APP_VERSION not defined"
!endif
Name "apitab ${APP_VERSION}"
; 相对路径以脚本目录（packaging/）为基准，写回仓库根供 CI 上传
OutFile "..\apitab-v${APP_VERSION}-win64-setup.exe"
InstallDir "$LOCALAPPDATA\Programs\apitab"
RequestExecutionLevel user
SetCompressor /SOLID lzma
!define MUI_ABORTWARNING
; NSIS 的相对路径以脚本所在目录（packaging/）为基准
!define MUI_ICON "..\assets\icon.ico"
!define MUI_UNICON "..\assets\icon.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\apitab.exe"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "apitab" SecMain
    SetOutPath "$INSTDIR"
    File /r "..\dist\*"
    CreateDirectory "$SMPROGRAMS\apitab"
    CreateShortCut "$SMPROGRAMS\apitab\apitab.lnk" "$INSTDIR\apitab.exe" "" "$INSTDIR\assets\icon.ico"
    CreateShortCut "$SMPROGRAMS\apitab\Uninstall apitab.lnk" "$INSTDIR\Uninstall.exe"
    CreateShortCut "$DESKTOP\apitab.lnk" "$INSTDIR\apitab.exe" "" "$INSTDIR\assets\icon.ico"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\apitab" "DisplayName" "apitab ${APP_VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\apitab" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\apitab" "Publisher" "FarnaHerry"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\apitab" "DisplayIcon" "$INSTDIR\apitab.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\apitab" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\apitab" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\apitab" "NoRepair" 1
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir /r "$INSTDIR"
    Delete "$SMPROGRAMS\apitab\apitab.lnk"
    Delete "$SMPROGRAMS\apitab\Uninstall apitab.lnk"
    RMDir "$SMPROGRAMS\apitab"
    Delete "$DESKTOP\apitab.lnk"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\apitab"
SectionEnd
