#!/bin/bash
# win81-installer-files.sh: tools/standalone-installer/win81 from win81-package.sh's package (viogpu3d) and the
# EWDK's redistributable Universal CRT (tools/standalone-installer/ucrt/x64 and x86, Windows 7's too), fetched from the
# spike (running)
set -euo pipefail
T=$(dirname "$0")
I=~/Coding/winboat/tools/standalone-installer
rm -rf "$I/win81" "$I/ucrt" && mkdir -p "$I/win81" "$I/ucrt/x64" "$I/ucrt/x86"
cp -r ~/winboat-gpu-spike/win81/package "$I/win81/viogpu3d"
"$T/sp.sh" '$iso = "C:\build\EWDK_ge_release_26100_240331-1435.iso"; $img = Get-DiskImage -ImagePath $iso; if (-not $img.Attached) { $img = Mount-DiskImage -ImagePath $iso -PassThru }
$ewdk = (Get-Volume -DiskImage $img).DriveLetter + ":"
$redist = Get-ChildItem "$ewdk\Program Files\Windows Kits\10\Redist\10.*\ucrt\DLLs" -Directory | Select-Object -Last 1
Remove-Item -Recurse -Force C:\build\ucrt -ErrorAction SilentlyContinue
Copy-Item $redist.FullName C:\build\ucrt -Recurse
Get-ChildItem C:\build\ucrt -Recurse -File | ForEach-Object { $_.FullName.Substring(14) }' | tr -d '\r' > "$I/ucrt/files.txt"
for f in $(cat "$I/ucrt/files.txt"); do
   case "$f" in x64\\*|x86\\*) "$T/sp.sh" --get "C:/build/ucrt/${f//\\//}" "$I/ucrt/${f//\\//}" ;; esac
done
rm "$I/ucrt/files.txt"
ls "$I/ucrt/x64" | wc -l; ls "$I/ucrt/x86" | wc -l
