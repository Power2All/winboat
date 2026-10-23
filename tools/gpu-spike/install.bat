@echo off
:: Runs once at the end of the unattended Windows install of the GPU spike VM.
:: Prepares a disposable VM for virtio-gpu driver testing. Never use this on a real WinBoat VM.

:: OpenSSH server, so the host can run commands and copy files (key-only login)
powershell -NoProfile -ExecutionPolicy Bypass -Command "Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Set-Service sshd -StartupType Automatic; Start-Service sshd"
copy /Y "C:\OEM\authorized_keys" "C:\ProgramData\ssh\administrators_authorized_keys"
icacls "C:\ProgramData\ssh\administrators_authorized_keys" /inheritance:r /grant "Administrators:F" /grant "SYSTEM:F"
reg add "HKLM\SOFTWARE\OpenSSH" /v DefaultShell /t REG_SZ /d "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" /f
netsh advfirewall firewall add rule name="OpenSSH Server (spike)" dir=in action=allow protocol=TCP localport=22

:: Allow test-signed drivers (takes effect after the next reboot)
bcdedit /set testsigning on

:: Allow any program as an RDP RemoteApp, like WinBoat does (guest_server/RDPApps.reg)
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Terminal Server\TSAppAllowList" /v fDisabledAllowList /t REG_DWORD /d 1 /f
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services" /v fAllowUnlistedRemotePrograms /t REG_DWORD /d 1 /f

:: Let RDP sessions render on a hardware GPU and prefer H.264/AVC 4:4:4 (same policies as WinBoat Phase 1)
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services" /v bEnumerateHWBeforeSW /t REG_DWORD /d 1 /f
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services" /v AVC444ModePreferred /t REG_DWORD /d 1 /f
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services" /v AVCHardwareEncodePreferred /t REG_DWORD /d 1 /f

echo done > "C:\OEM\spike-ready.txt"
