# Installs the toolchain for building the viogpu3d driver (kernel driver + Mesa user-mode drivers) inside the
# GPU spike VM. Safe to re-run: every step is skipped when already done.
#
# Toolchain, following https://virtio-win.github.io/Development/Building-the-drivers-using-Windows-11-24H2-EWDK
#   - Windows 11 24H2 EWDK (Visual Studio Build Tools 17.8.6 + SDK + WDK), kept as an ISO and mounted at build time
#   - Python 3.12 + meson, ninja, mako, packaging, pyyaml (for Mesa)
#   - win_flex_bison (for Mesa)
#   - LLVM 18 (clang-cl, for Mesa's Vulkan driver: Venus needs C11 atomics and typeof, which this MSVC lacks)
#   - the Khronos Vulkan runtime's loader (vulkan-1.dll), which the driver package installs
#   - glslang and widl (for vkd3d-proton, the base of the Direct3D 12 driver: its shaders and IDL headers)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$BuildDir = "C:\build"
$ToolsDir = "$BuildDir\tools"
$EwdkIso = "$BuildDir\EWDK_ge_release_26100_240331-1435.iso"
$EwdkUrl = "https://software-static.download.prss.microsoft.com/dbazure/888969d5-f34g-4e03-ac9d-1f9786c66749/EWDK_ge_release_26100_240331-1435.iso"
$EwdkSize = 19100119040
$PythonUrl = "https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe"
$FlexBisonUrl = "https://github.com/lexxmark/winflexbison/releases/download/v2.5.25/win_flex_bison-2.5.25.zip"
$LlvmUrl = "https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/LLVM-18.1.8-win64.exe"
$LlvmSha256 = "94af030060d88cc17e9f00ef1663ebdc1126b35e16bebdfa1e807984b70abd8f"
$VulkanRtUrl = "https://sdk.lunarg.com/sdk/download/1.3.296.0/windows/VulkanRT-1.3.296.0-Components.zip"
$VulkanRtSha256 = "e0429f91cf356fe99e381f7d22e568ac81945669f1be1772d933a0d189230452"
# glslang's Windows build needs a VC++ runtime from VS 2022 17.10 or newer (older msvcp140.dll copies crash it)
$VcRedistUrl = "https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B/VC_redist.x64.exe"
$VcRedistSha256 = "cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b"
$GlslangUrl = "https://github.com/KhronosGroup/glslang/releases/download/16.5.0/glslang-16.5.0-windows-x86_64-release.zip"
$GlslangSha256 = "06b71298b750268c127f2ee7ae0ef7525e2068120c6c8a3a08b2f58ca6f325ce"
# widl.exe from MSYS2's mingw-w64 tools (self-contained: kernel32 + msvcrt)
$WidlUrl = "https://repo.msys2.org/mingw/mingw64/mingw-w64-x86_64-tools-git-13.0.0.r380.gb83511db8-1-any.pkg.tar.zst"
$WidlSha256 = "bb38c7a5ad0b0bae2abae0517fdd2779de27344a7dd02c887d951e6e16c6d6bb"

New-Item -ItemType Directory -Force -Path $BuildDir, $ToolsDir | Out-Null

# Downloads a file and checks it against its pinned SHA-256
function Get-PinnedFile($url, $file, $sha256) {
    curl.exe -fsSL -o $file $url
    if ((Get-FileHash -Algorithm SHA256 $file).Hash -ne $sha256) { Remove-Item $file; throw "$url doesn't match its pinned hash" }
}

function Add-MachinePath($dir) {
    $path = [Environment]::GetEnvironmentVariable("Path", "Machine")
    if (($path -split ";") -notcontains $dir) {
        [Environment]::SetEnvironmentVariable("Path", "$path;$dir", "Machine")
    }
    if (($env:Path -split ";") -notcontains $dir) { $env:Path += ";$dir" }
}

# Python
$python = "C:\Program Files\Python312\python.exe"
if (-not (Test-Path $python)) {
    Write-Host "Installing Python 3.12..."
    curl.exe -fsSL -o "$ToolsDir\python-installer.exe" $PythonUrl
    Start-Process -Wait -FilePath "$ToolsDir\python-installer.exe" -ArgumentList "/quiet InstallAllUsers=1 PrependPath=1 Include_test=0"
}
Add-MachinePath "C:\Program Files\Python312"
Add-MachinePath "C:\Program Files\Python312\Scripts"
Write-Host "Installing Python build packages..."
& $python -m pip install --quiet --upgrade pip meson ninja mako packaging pyyaml

# win_flex_bison
$flexDir = "$ToolsDir\winflexbison"
if (-not (Test-Path "$flexDir\win_bison.exe")) {
    Write-Host "Installing win_flex_bison..."
    curl.exe -fsSL -o "$ToolsDir\winflexbison.zip" $FlexBisonUrl
    Expand-Archive -Force "$ToolsDir\winflexbison.zip" $flexDir
}
Add-MachinePath $flexDir

# LLVM (clang-cl, lld-link, llvm-lib). Not on PATH: build-driver.ps1 names it for the Vulkan build only
if (-not (Test-Path "C:\Program Files\LLVM\bin\clang-cl.exe")) {
    Write-Host "Installing LLVM 18..."
    Get-PinnedFile $LlvmUrl "$ToolsDir\llvm-installer.exe" $LlvmSha256
    Start-Process -Wait -FilePath "$ToolsDir\llvm-installer.exe" -ArgumentList "/S", "/D=C:\Program Files\LLVM"
}

# Vulkan runtime components: x64\vulkan-1.dll and x86\vulkan-1.dll (Apache-2.0, see VulkanRT-License.txt)
$vulkanRtDir = "$ToolsDir\VulkanRT"
if (-not (Test-Path "$vulkanRtDir\x64\vulkan-1.dll")) {
    Write-Host "Fetching the Vulkan runtime components..."
    Get-PinnedFile $VulkanRtUrl "$ToolsDir\VulkanRT.zip" $VulkanRtSha256
    Expand-Archive -Force "$ToolsDir\VulkanRT.zip" "$ToolsDir\VulkanRT.tmp"
    Move-Item (Get-ChildItem "$ToolsDir\VulkanRT.tmp" -Directory | Select-Object -First 1).FullName $vulkanRtDir
    Remove-Item -Recurse "$ToolsDir\VulkanRT.tmp"
}

# glslang and widl for vkd3d-proton (build-driver.ps1 puts $ToolsDir\bin on PATH for that build)
$toolsBin = "$ToolsDir\bin"
New-Item -ItemType Directory -Force -Path $toolsBin | Out-Null
if (-not (Test-Path "$env:SystemRoot\System32\msvcp140.dll") -or
    [version](Get-Item "$env:SystemRoot\System32\msvcp140.dll").VersionInfo.FileVersion -lt [version]"14.40") {
    Write-Host "Installing the VC++ 2015-2022 runtime (14.44)..."
    Get-PinnedFile $VcRedistUrl "$ToolsDir\VC_redist.x64.exe" $VcRedistSha256
    Start-Process -Wait -FilePath "$ToolsDir\VC_redist.x64.exe" -ArgumentList "/install", "/quiet", "/norestart"
}
if (-not (Test-Path "$toolsBin\glslang.exe")) {
    Write-Host "Fetching glslang..."
    Get-PinnedFile $GlslangUrl "$ToolsDir\glslang.zip" $GlslangSha256
    Expand-Archive -Force "$ToolsDir\glslang.zip" "$ToolsDir\glslang.tmp"
    Copy-Item "$ToolsDir\glslang.tmp\bin\glslang.exe" $toolsBin
    Remove-Item -Recurse "$ToolsDir\glslang.tmp"
}
if (-not (Test-Path "$toolsBin\widl.exe")) {
    Write-Host "Fetching widl..."
    Get-PinnedFile $WidlUrl "$ToolsDir\mingw-tools.pkg.tar.zst" $WidlSha256
    New-Item -ItemType Directory -Force -Path "$ToolsDir\mingw-tools.tmp" | Out-Null
    tar.exe -xf "$ToolsDir\mingw-tools.pkg.tar.zst" -C "$ToolsDir\mingw-tools.tmp" mingw64/bin/widl.exe
    if ($LASTEXITCODE) { throw "Could not unpack widl" }
    Copy-Item "$ToolsDir\mingw-tools.tmp\mingw64\bin\widl.exe" $toolsBin
    Remove-Item -Recurse "$ToolsDir\mingw-tools.tmp"
}

# EWDK (19 GB), resumable download
if (-not (Test-Path $EwdkIso) -or (Get-Item $EwdkIso).Length -ne $EwdkSize) {
    Write-Host "Downloading the Windows 11 24H2 EWDK (19 GB)..."
    curl.exe -fSL --retry 5 -C - -o $EwdkIso $EwdkUrl
    if ((Get-Item $EwdkIso).Length -ne $EwdkSize) { throw "EWDK download is incomplete" }
}

Write-Host "Build environment ready."
