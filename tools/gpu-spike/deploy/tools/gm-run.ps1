# gm-run.ps1 [api] [seconds] [extra]: GravityMark 1.89's benchmark (1600x900, one pass) in the spike's desktop session
# (spike-run.sh 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\build\gm-run.ps1 gl 300'), with the drivers'
# logs on, killed after [seconds]; prints its exit and its log. api: vulkan, d3d12, d3d11 or gl. extra: GravityMark
# options, and "dumpspv" to save every SPIR-V module Mesa takes to C:\build\spvall
param($api = 'd3d12', $secs = 45, $extra = '')
Get-Process GravityMark -ErrorAction SilentlyContinue | Stop-Process -Force
$env:VIOGPU_D3D12_LOG = "C:\build\gm-umd.log"
$env:VKD3D_DEBUG = "warn"; $env:VKD3D_LOG_FILE = "C:\build\gm-vkd3d.log"
$env:MESA_SPIRV_FAIL_DUMP_PATH = "C:\build\spvfail"
$env:MESA_SPIRV_DUMP_PATH = $(if ($extra -match "dumpspv") { "C:\build\spvall" } else { $null })
$extra = $extra -replace "dumpspv", ""
New-Item -ItemType Directory -Force C:\build\spvfail, C:\build\spvall | Out-Null
Remove-Item C:\build\gm-umd.log, C:\build\gm-vkd3d.log, C:\build\spvfail\*, C:\build\spvall\* -ErrorAction SilentlyContinue
# the drivers' OutputDebugString messages (dbwin.cpp)
Start-Process C:\build\run\dbwin.exe -ArgumentList "C:\build\gm-ods.txt", ([int]$secs + 5) -WindowStyle Hidden; Start-Sleep 1
$log = "$env:USERPROFILE\.GravityMark\GravityMark.log"
Remove-Item $log -ErrorAction SilentlyContinue
$p = Start-Process 'C:\Program Files\GravityMark\bin\GravityMark.exe' -WorkingDirectory 'C:\Program Files\GravityMark\bin' `
    -ArgumentList "-$api -temporal 1 -fps 1 -info 1 -benchmark 1 -close 1 -count 1 -width 1600 -height 900 $extra" -PassThru
$p | Wait-Process -Timeout $secs -ErrorAction SilentlyContinue
"exited=$($p.HasExited) code=$(if ($p.HasExited) { '0x{0:X}' -f $p.ExitCode })"
$p | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Content $log | ? { $_ -notmatch 'Build|Name:|System:|Kernel|Memory:|Uptime|CPU:|Device: PCI|Desktop:|Screen 0' } | select -First 40
