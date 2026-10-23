# w81swap.ps1: the files in C:\Users\spike\win81pkg over the installed Windows 8.1 package (in-use ones renamed away
# first): viogpu3d.sys to drivers, the 64-bit user-mode drivers to System32, the _x86 ones to SysWOW64. Restart after
$stamp = Get-Date -Format yyyyMMddHHmmss
foreach ($f in Get-ChildItem C:\Users\spike\win81pkg -Include *.sys,*.dll -Recurse) {
   $dir = if ($f.Extension -eq '.sys') { 'C:\Windows\System32\drivers' } elseif ($f.Name -match '_x86\.dll$') { 'C:\Windows\SysWOW64' } else { 'C:\Windows\System32' }
   $target = Join-Path $dir $f.Name
   if (Test-Path $target) {
      try { Copy-Item $f.FullName $target -Force -ErrorAction Stop }
      catch { Rename-Item $target ($f.Name + '.old-' + $stamp); Copy-Item $f.FullName $target }
   } else { Copy-Item $f.FullName $target }
   $f.Name + ' -> ' + $dir
}
