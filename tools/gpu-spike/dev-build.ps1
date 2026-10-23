# Development helper for the GPU spike VM: builds selected Mesa ninja targets (default: everything) for one
# architecture, after build-driver.ps1 has configured the build directories. Faster than a full build-driver.ps1 run
# while fixing compile errors.
#
#   dev-build.ps1 [-Arch amd64|x86] [-Targets "src/gallium/frontends/nine/libnine_st.a ..."]

param(
    [ValidateSet("amd64", "x86")] [string] $Arch = "amd64",
    [string] $Targets = ""
)

$ErrorActionPreference = "Stop"

$SrcDir = "C:\build\src"
$EwdkIso = "C:\build\EWDK_ge_release_26100_240331-1435.iso"
$image = Get-DiskImage -ImagePath $EwdkIso
if (-not $image.Attached) { $image = Mount-DiskImage -ImagePath $EwdkIso -PassThru }
$ewdk = (Get-Volume -DiskImage $image).DriveLetter + ":"

$sdk = "$ewdk\Program Files\Windows Kits\10"
$sdkVer = (Get-ChildItem "$sdk\Include" -Directory | Where-Object Name -match '^10\.' | Sort-Object Name | Select-Object -Last 1).Name
$lib = if ($Arch -eq "x86") { "x86" } else { "x64" }
$buildDir = if ($Arch -eq "x86") { "build32" } else { "build" }
$suffix = if ($Arch -eq "x86") { "_x86" } else { "" }
$buildEnv = "set `"INCLUDE=!VCToolsInstallDir!include;$sdk\Include\$sdkVer\ucrt;$sdk\Include\$sdkVer\um;$sdk\Include\$sdkVer\shared;$sdk\Include\$sdkVer\winrt`"&& " +
    "set `"LIB=!VCToolsInstallDir!lib\$lib;$sdk\Lib\$sdkVer\ucrt\$lib;$sdk\Lib\$sdkVer\um\$lib`"&& " +
    "set `"PATH=!PATH!;$sdk\bin\$sdkVer\$lib`"&& "

function Invoke-Ewdk($command) {
    cmd.exe /v:on /c "call `"$ewdk\BuildEnv\SetupBuildEnv.cmd`" $Arch >nul && $command"
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $command" }
}

# Turn on the D3D9 UMD once; ninja regenerates the build files by itself after that
$options = Get-Content "$SrcDir\mesa\$buildDir\meson-info\intro-buildoptions.json" -Raw | ConvertFrom-Json
if (($options | Where-Object name -eq "gallium-d3d9umd").value -ne $true) {
    Invoke-Ewdk "$buildEnv cd /d $SrcDir\mesa && meson configure $buildDir -Dgallium-d3d9umd=true -Dgallium-d3d9-dll-name=viogpu_d3d9$suffix"
}
Invoke-Ewdk "$buildEnv cd /d $SrcDir\mesa && ninja -C $buildDir -k 0 $Targets"
