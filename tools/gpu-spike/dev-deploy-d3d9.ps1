# Development helper for the GPU spike VM: puts freshly built viogpu_d3d9(_x86).dll next to the installed
# viogpu3d driver and makes the D3D9 runtime use it (UserModeDriverName entry 0, the D3D9 driver; the other entries
# stay the D3D10/11 driver). Takes effect for newly started processes. Run as SYSTEM (the DriverStore is protected).
#
#   dev-deploy-d3d9.ps1 [-Trace]   # -Trace: log every DDI call to C:\viogpu_d3d9_trace.log

param([switch] $Trace)

$ErrorActionPreference = "Stop"

$installed = Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository" -Directory -Filter "viogpu3d.inf_*" |
    Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $installed) { throw "viogpu3d isn't installed" }

foreach ($build in @(@{ Dir = "build"; Name = "viogpu_d3d9.dll"; Value = "UserModeDriverName" },
                     @{ Dir = "build32"; Name = "viogpu_d3d9_x86.dll"; Value = "UserModeDriverNameWow" })) {
    $dll = "C:\build\src\mesa\$($build.Dir)\src\gallium\targets\d3d9umd\$($build.Name)"
    if (-not (Test-Path $dll)) { Write-Host "Not built: $dll"; continue }
    Copy-Item $dll $installed.FullName -Force
    $target = Join-Path $installed.FullName $build.Name

    # The display adapter's driver key
    $class = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"
    $key = Get-ChildItem $class -ErrorAction SilentlyContinue |
        Where-Object { (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).DriverDesc -like "Red Hat VirtIO GPU 3D controller*" } |
        Select-Object -First 1
    $names = [string[]](Get-ItemProperty $key.PSPath).($build.Value)
    if ($names -and $names[0] -ne $target) {
        $names[0] = $target
        Set-ItemProperty $key.PSPath -Name $build.Value -Value $names -Type MultiString
    }
    Write-Host "Deployed $($build.Name); $($build.Value)[0] = $target"
}

if ($Trace) {
    Set-Content C:\viogpu_d3d9_trace.txt "trace"
    Remove-Item C:\viogpu_d3d9_trace.log -ErrorAction SilentlyContinue
} else {
    Remove-Item C:\viogpu_d3d9_trace.txt -ErrorAction SilentlyContinue
}
