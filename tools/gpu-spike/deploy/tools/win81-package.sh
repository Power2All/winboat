#!/bin/bash
# win81-package.sh: the Windows 8.1 driver package (roadmap D) in ~/winboat-gpu-spike/win81/package: the kernel driver
# built in the spike with build-driver.ps1 -Win81, the Direct3D 9/10/11 and OpenGL user-mode drivers of the exported
# Windows 10 package (guest_server/drivers/viogpu3d), tools/gpu-spike/win81/viogpu3d.inf; the catalog made and
# everything test-signed in the spike (virtio-win's VirtIOTestCert, its .cer next to the package)
set -euo pipefail
T=$(dirname "$0")
OUT=~/winboat-gpu-spike/win81/package
PKG=~/Coding/winboat/guest_server/drivers/viogpu3d
OBJ='C:\build\src\kvm-guest-drivers-windows\viogpu\viogpu3d\objfre_win10_amd64'
"$T/sp.sh" "Remove-Item -Recurse -Force $OBJ -ErrorAction SilentlyContinue; & powershell -NoProfile -ExecutionPolicy Bypass -File C:\OEM\build-driver.ps1 -KernelOnly -Win81 ${WIN81_DEFINES:+-ExtraDefines '$WIN81_DEFINES'} *> C:\build\build-win81.log; Get-Content C:\build\build-win81.log | Select-String -Pattern ': error|error C|error LNK' | Select-Object -First 20 | ForEach-Object { \$_.Line }" | tr -d '\r'
rm -rf "$OUT" && mkdir -p "$OUT"
"$T/sp.sh" --get "C:/build/src/kvm-guest-drivers-windows/viogpu/viogpu3d/objfre_win10_amd64/amd64/viogpu3d/viogpu3d.sys" "$OUT/viogpu3d.sys"
# its symbols (for a debugger), kept next to the package
"$T/sp.sh" --get "C:/build/src/kvm-guest-drivers-windows/viogpu/viogpu3d/objfre_win10_amd64/amd64/viogpu3d.pdb" ~/winboat-gpu-spike/win81/viogpu3d.pdb
# the Windows 10 build's objects mustn't be mixed with these (MSBuild doesn't see _CL_ change)
"$T/sp.sh" "Remove-Item -Recurse -Force $OBJ -ErrorAction SilentlyContinue"
for f in viogpu_d3d9 viogpu_d3d10 viogpu_wgl; do cp "$PKG/$f.dll" "$PKG/${f}_x86.dll" "$OUT/"; done
ver=$(sed -n 's/^DriverVer *= *\([^ \r]*\).*/\1/p' "$PKG/viogpu3d.inf" | tr -d '\r')
sed "s|@DRIVERVER@|$ver|" ~/Coding/winboat/tools/gpu-spike/win81/viogpu3d.inf > "$OUT/viogpu3d.inf"
"$T/sp.sh" 'Remove-Item -Recurse -Force C:\build\win81pkg -ErrorAction SilentlyContinue; New-Item -ItemType Directory C:\build\win81pkg | Out-Null'
for f in "$OUT"/*; do "$T/sp.sh" --put "$f" "C:/build/win81pkg/$(basename "$f")"; done
"$T/sp.sh" '$iso = "C:\build\EWDK_ge_release_26100_240331-1435.iso"; $img = Get-DiskImage -ImagePath $iso; if (-not $img.Attached) { $img = Mount-DiskImage -ImagePath $iso -PassThru }
$ewdk = (Get-Volume -DiskImage $img).DriveLetter + ":"
$bin = Get-ChildItem "$ewdk\Program Files\Windows Kits\10\bin\10.*" -Directory | Select-Object -Last 1
$cert = "C:\build\src\kvm-guest-drivers-windows\build\VirtIOTestCert.pfx"
Get-ChildItem C:\build\win81pkg -Include *.sys,*.dll -Recurse | ForEach-Object { & "$($bin.FullName)\x64\signtool.exe" sign /fd SHA256 /f $cert $_.FullName | Out-Null; if ($LASTEXITCODE) { throw "signing $($_.Name)" } }
& "$($bin.FullName)\x86\Inf2Cat.exe" /driver:C:\build\win81pkg /os:6_3_X64 | Select-Object -Last 2
& "$($bin.FullName)\x64\signtool.exe" sign /fd SHA256 /f $cert C:\build\win81pkg\viogpu3d.cat | Out-Null; if ($LASTEXITCODE) { throw "signing the catalog" }
"signed"' | tr -d '\r'
"$T/sp.sh" --get C:/build/win81pkg/viogpu3d.cat "$OUT/viogpu3d.cat"
for f in viogpu3d.sys viogpu_d3d9.dll viogpu_d3d10.dll viogpu_wgl.dll viogpu_d3d9_x86.dll viogpu_d3d10_x86.dll viogpu_wgl_x86.dll; do "$T/sp.sh" --get "C:/build/win81pkg/$f" "$OUT/$f"; done
"$T/sp.sh" --get 'C:/build/src/kvm-guest-drivers-windows/build/VirtIOTestCert.cer' ~/winboat-gpu-spike/win81/VirtIOTestCert.cer
ls -la "$OUT"
# The standalone installer's 8.1 part (tools/standalone-installer/win81): this package, and the Windows SDK's
# redistributable Universal CRT for the GPU check (8.1 without KB2999226 has none; mingw's builds need it)
"$T/win81-installer-files.sh"
