; WinBoat GPU driver setup for any QEMU Windows VM with a virtio-gpu 3D device (virgl). Built by build.sh.
; Installs the same layout WinBoat's guest payload has (scripts\gpu-driver.ps1 next to drivers\) and lets
; gpu-driver.ps1 do the work: signing with a certificate made on this machine, test mode, the driver, the RDP GPU
; policy, the SPICE agent and resolution helper, the GPU check. Uninstall reverts what it recorded.
; makensis -DVERSION=<x.y.z.w> -DSOURCE=<guest_server dir> [-DWIN81=<the Windows 8.1 part>] [-DWIN7=<the Windows 7 part>]
;          [-DUCRT=<the Universal CRT, for either>] -DREADME=<the "Before you install" text> -DOUTFILE=<setup.exe>
;          installer.nsi

Unicode true
RequestExecutionLevel admin
SetCompressor /SOLID lzma
!include MUI2.nsh
!include LogicLib.nsh
!include x64.nsh
!include WinVer.nsh

Name "WinBoat GPU Driver"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\WinBoat\GPU Driver"
VIProductVersion "${VERSION}"
VIAddVersionKey "ProductName" "WinBoat GPU Driver"
VIAddVersionKey "FileDescription" "WinBoat GPU Driver setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "WinBoat contributors"

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\WinBoatGPUDriver"
; 64-bit PowerShell from this 32-bit setup (pnputil and the driver store aren't there for 32-bit processes). Without
; -InputFormat None, Windows 7's PowerShell 2.0 fails on nsExec's input ("Access to the path is denied")
!define PS '"$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -InputFormat None -ExecutionPolicy Bypass'

!ifdef WIN7
!define SUPPORTED "Windows 7, 8.1, 10 or 11"
!else ifdef WIN81
!define SUPPORTED "Windows 8.1, 10 or 11"
!else
!define SUPPORTED "Windows 10 or 11"
!endif
; The welcome page short (its text doesn't scroll), everything else on the next page's scrolling box (build.sh's README)
!define MUI_WELCOMEPAGE_TEXT "Hardware graphics for this VM on the host's GPU: Direct3D, OpenGL, Vulkan and video decoding, through QEMU's virtio-gpu with 3D.$\r$\n$\r$\nFor ${SUPPORTED}, 64-bit. The VM needs a virtio-gpu 3D device and Secure Boot off: the next page has the details.$\r$\n$\r$\nA restart is needed after installing."
!define MUI_PAGE_HEADER_TEXT "Before you install"
!define MUI_PAGE_HEADER_SUBTEXT "What the VM needs, and what setup changes."
!define MUI_LICENSEPAGE_TEXT_TOP "Scroll for the rest."
!define MUI_LICENSEPAGE_TEXT_BOTTOM "Click Install to install the driver."
!define MUI_LICENSEPAGE_BUTTON "&Install"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${README}"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "The WinBoat GPU Driver needs 64-bit Windows." /SD IDOK
        Abort
    ${EndIf}
    ${If} ${AtLeastWin10}
!ifdef WIN81
    ${OrIf} ${IsWin8.1}
!endif
!ifdef WIN7
    ${OrIf} ${IsWin7}
!endif
    ${Else}
        MessageBox MB_ICONSTOP "The WinBoat GPU Driver needs ${SUPPORTED}." /SD IDOK
        Abort
    ${EndIf}
    SetRegView 64
FunctionEnd

Section
    SetOutPath "$INSTDIR\scripts"
    File "${SOURCE}\scripts\gpu-driver.ps1"
    SetOutPath "$INSTDIR\drivers"
    File /r "${SOURCE}\drivers\spice"
    File /r "${SOURCE}\drivers\gpucheck"
!ifdef UCRT
    ${IfNot} ${AtLeastWin10}
        ; The GPU check with the Universal CRT 7 and 8.1 may lack (each half with its own: the 32-bit one in 32\, where
        ; dx-test looks too), and the older WDDM's package (gpu-driver.ps1 takes it there)
        SetOutPath "$INSTDIR\drivers\gpucheck"
        File "${UCRT}\x64\*.dll"
        SetOutPath "$INSTDIR\drivers\gpucheck\32"
        File "${UCRT}\x86\*.dll"
        Rename "$INSTDIR\drivers\gpucheck\winboat-dxtest32.exe" "$INSTDIR\drivers\gpucheck\32\winboat-dxtest32.exe"
!ifdef WIN81
        ${If} ${IsWin8.1}
            SetOutPath "$INSTDIR\drivers\viogpu3d-win81"
            File "${WIN81}\viogpu3d\*.*"
        ${EndIf}
!endif
!ifdef WIN7
        ${If} ${IsWin7}
            SetOutPath "$INSTDIR\drivers\viogpu3d-win7"
            File "${WIN7}\viogpu3d\*.*"
        ${EndIf}
!endif
    ${Else}
        File /r "${SOURCE}\drivers\viogpu3d"
    ${EndIf}
!else
    File /r "${SOURCE}\drivers\viogpu3d"
!endif
    WriteUninstaller "$INSTDIR\uninstall.exe"

    ; No virtio-gpu (VEN_1AF4&DEV_1050) present
    nsExec::ExecToLog '${PS} -Command "if (-not (Get-WmiObject Win32_PnPEntity | Where-Object { $$_.DeviceID -match \"VEN_1AF4&DEV_1050\" })) { exit 2 }"'
    Pop $0
    ${If} $0 == 2
        MessageBox MB_YESNO|MB_ICONEXCLAMATION "This VM has no virtio-gpu device, so the driver won't be used until it has one (QEMU: -device virtio-vga-gl). Install anyway?" /SD IDYES IDYES +3
        RMDir /r "$INSTDIR"
        Abort
    ${EndIf}

    DetailPrint "Installing the driver (signing, test mode, pnputil)..."
    nsExec::ExecToStack '${PS} -File "$INSTDIR\scripts\gpu-driver.ps1" -Action Install'
    Pop $0
    Pop $1
    DetailPrint $1
    ${If} $0 != 0
        ; (and for silent installs, which show no message, in %TEMP%)
        FileOpen $2 "$TEMP\winboat-gpu-driver-setup.log" w
        FileWrite $2 "Installing the driver failed ($0):$\r$\n$1"
        FileClose $2
        MessageBox MB_ICONSTOP "Installing the driver failed:$\r$\n$\r$\n$1" /SD IDOK
        RMDir /r "$INSTDIR"
        Abort
    ${EndIf}

    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "WinBoat GPU Driver"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "WinBoat"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
    SetRebootFlag true
SectionEnd

Function un.onInit
    SetRegView 64
FunctionEnd

Section "Uninstall"
    DetailPrint "Removing the driver..."
    nsExec::ExecToStack '${PS} -File "$INSTDIR\scripts\gpu-driver.ps1" -Action Uninstall'
    Pop $0
    Pop $1
    DetailPrint $1
    ${If} $0 != 0
        MessageBox MB_ICONEXCLAMATION "Removing the driver failed:$\r$\n$\r$\n$1" /SD IDOK
    ${EndIf}
    RMDir /r "$INSTDIR"
    RMDir "$PROGRAMFILES64\WinBoat"
    DeleteRegKey HKLM "${UNINSTALL_KEY}"
    SetRebootFlag true
SectionEnd
