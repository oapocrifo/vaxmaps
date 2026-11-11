; Vax Maps installer. Per user: no administrator rights, nothing outside the user's profile.
; makensis -DVERSION=0.6.0 -DEXE=path/VaxMaps.exe -DREADME=path/README.md -DOUTFILE=path/setup.exe tools/installer.nsi
Target amd64-unicode   ; the program is 64-bit only; a 64-bit installer also runs where 32-bit support is missing
SetCompressor /SOLID lzma
RequestExecutionLevel user

!define APP "Vax Maps"
!define KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\VaxMaps"
!define CLASS "VaxMaps.Project"

Name "${APP} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\Vax Maps"
InstallDirRegKey HKCU "${KEY}" "InstallLocation"

!include "MUI2.nsh"
!define MUI_ICON "${__FILEDIR__}/../icon/vaxmaps.ico"
!define MUI_UNICON "${__FILEDIR__}/../icon/vaxmaps.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\VaxMaps.exe"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "PortugueseBR"

Function .onInit
  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

Section "Install"
  SetOutPath "$INSTDIR"
  File "/oname=VaxMaps.exe" "${EXE}"
  File "/oname=README.md" "${README}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\Vax Maps.lnk" "$INSTDIR\VaxMaps.exe"

  ; double click on a .vaxmap opens it in Vax Maps
  WriteRegStr HKCU "Software\Classes\.vaxmap" "" "${CLASS}"
  WriteRegStr HKCU "Software\Classes\${CLASS}" "" "Vax Maps project"
  WriteRegStr HKCU "Software\Classes\${CLASS}\DefaultIcon" "" "$INSTDIR\VaxMaps.exe,0"
  WriteRegStr HKCU "Software\Classes\${CLASS}\shell\open\command" "" '"$INSTDIR\VaxMaps.exe" "%1"'

  WriteRegStr HKCU "${KEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${KEY}" "DisplayIcon" "$INSTDIR\VaxMaps.exe,0"
  WriteRegStr HKCU "${KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${KEY}" "NoRepair" 1
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'   ; Explorer shows the new file icon
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\VaxMaps.exe"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Vax Maps.lnk"
  ; .vaxmap is only released if it still points at Vax Maps
  ReadRegStr $0 HKCU "Software\Classes\.vaxmap" ""
  StrCmp $0 "${CLASS}" 0 +2
    DeleteRegKey HKCU "Software\Classes\.vaxmap"
  DeleteRegKey HKCU "Software\Classes\${CLASS}"
  DeleteRegKey HKCU "${KEY}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd
