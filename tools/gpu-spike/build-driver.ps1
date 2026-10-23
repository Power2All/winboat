# Builds viogpu3d inside the GPU spike VM: Mesa (virgl OpenGL, D3D9 and D3D10 user-mode drivers, and the Venus Vulkan
# driver, 64- and 32-bit), then the kernel driver package that bundles them. Expects the sources in C:\build\src (copied from the host) and setup-build-env.ps1 done.
#
# Output: C:\build\src\kvm-guest-drivers-windows\viogpu\viogpu3d\objfre_win10_amd64\amd64\viogpu3d
# viogpu3d only has a Win10 configuration (the solution maps Win11 to it, without building the Win10 VirtioLib it
# links against), and that package installs on Windows 11 as well.

param(
    [ValidateSet("Win10")] [string] $Target = "Win10",
    # Development shortcut: rebuild only Mesa and copy its DLLs over the installed driver's copies, so the next app
    # start uses them (no reinstall or reboot). Only for the spike VM; the package's catalog no longer matches.
    [switch] $UserModeOnly,
    # Development shortcut: only the kernel driver package, with the Mesa DLLs of the last full build
    [switch] $KernelOnly,
    # The kernel driver for Windows 8.1: WDDM 1.3 (VIOGPU_WDDM1_3: no GPU virtual addresses, no Direct3D 12/Venus
    # engine) at the Windows 8.1 API level (this EWDK targets Windows 10: the project's version defines are replaced).
    # It lands where the Windows 10 one does
    [switch] $Win81,
    [switch] $Win7,
    # Diagnostics: more compiler options for the kernel driver (e.g. /DCOM_DEBUG: its trace to the COM1 port)
    [string] $ExtraDefines = ""
)

$ErrorActionPreference = "Stop"

$BuildDir = "C:\build"
$SrcDir = "$BuildDir\src"
$MesaPrefix = "$BuildDir\mesa_prefix"
$MesaPrefix32 = "$BuildDir\mesa_prefix32"
$EwdkIso = "$BuildDir\EWDK_ge_release_26100_240331-1435.iso"

# Mount the EWDK ISO if it isn't already
$image = Get-DiskImage -ImagePath $EwdkIso
if (-not $image.Attached) { $image = Mount-DiskImage -ImagePath $EwdkIso -PassThru }
$ewdk = (Get-Volume -DiskImage $image).DriveLetter + ":"
Write-Host "EWDK mounted at $ewdk"

# Runs a command inside the EWDK build environment (MSVC, SDK and WDK on PATH) for a target architecture
function Invoke-Ewdk($command, $arch = "amd64") {
    cmd.exe /v:on /c "call `"$ewdk\BuildEnv\SetupBuildEnv.cmd`" $arch >nul && $command"
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $command" }
}

# The EWDK only sets up MSBuild driver builds; Meson calls cl.exe directly and needs the user-mode headers/libs.
# !VAR! (delayed expansion) because %VAR% would expand before SetupBuildEnv.cmd has run
$sdk = "$ewdk\Program Files\Windows Kits\10"
$sdkVer = (Get-ChildItem "$sdk\Include" -Directory | Where-Object Name -match '^10\.' | Sort-Object Name | Select-Object -Last 1).Name
function Get-UserModeEnv($libArch) {
    "set `"INCLUDE=!VCToolsInstallDir!include;$sdk\Include\$sdkVer\ucrt;$sdk\Include\$sdkVer\um;$sdk\Include\$sdkVer\shared;$sdk\Include\$sdkVer\winrt`"&& " +
    "set `"LIB=!VCToolsInstallDir!lib\$libArch;$sdk\Lib\$sdkVer\ucrt\$libArch;$sdk\Lib\$sdkVer\um\$libArch`"&& " +
    "set `"PATH=!PATH!;$sdk\bin\$sdkVer\$libArch`"&& "
}

# The driver's version: virtio-win's 100.6.101.58000, its last part 58000 plus the minute of the day the package is
# built (stampinf dates it with the day). Every build then has a version of its own that grows with the build time,
# which is how Windows ranks drivers (date, then version); a part can't exceed 65535. The user-mode drivers carry it
# too (their version resource: what the Direct3D runtimes report as the driver version).
$buildMinor = 58000 + [int][math]::Floor((Get-Date).TimeOfDay.TotalMinutes)
# Never below the day's previous build: the VM's clock can jump back (a time zone or host time change), and a lower
# version the same day would rank under the driver it replaces
$lastBuild = "C:\build\driver-version.txt"
$today = (Get-Date).ToString("yyyy-MM-dd")
if (Test-Path $lastBuild) {
    $day, $minor = (Get-Content $lastBuild -Raw).Trim() -split " "
    if ($day -eq $today -and [int]$minor -ge $buildMinor) { $buildMinor = [int]$minor + 1 }
}
if (-not $UserModeOnly) { Set-Content $lastBuild "$today $buildMinor" }

# 64-bit drivers for 64-bit apps, and 32-bit ones (with an _x86 suffix, they share the package folder) for 32-bit apps.
# zlib is linked statically, so the two builds don't need conflicting z-1.dll copies.
$mesaBuilds = @(
    @{ Arch = "amd64"; Lib = "x64"; Dir = "build"; Prefix = $MesaPrefix; Suffix = "" },
    @{ Arch = "x86"; Lib = "x86"; Dir = "build32"; Prefix = $MesaPrefix32; Suffix = "_x86" }
)
foreach ($build in $(if ($KernelOnly) { @() } else { $mesaBuilds })) {
    Write-Host "Building Mesa for $($build.Arch) (virgl WGL + D3D9 and D3D10 UMDs)..."
    $buildEnv = Get-UserModeEnv $build.Lib
    $mesaDir = "$SrcDir\mesa"
    $meson = "-Dgallium-drivers=virgl -Dgallium-d3d10umd=true -Dgallium-wgl-dll-name=viogpu_wgl$($build.Suffix) " +
        "-Dgallium-d3d10-dll-name=viogpu_d3d10$($build.Suffix) -Dgallium-d3d9umd=true " +
        "-Dgallium-d3d9-dll-name=viogpu_d3d9$($build.Suffix) -Db_vscrt=mt -Dvulkan-drivers= -Dllvm=disabled " +
        "-Dbuildtype=release -Dzlib:default_library=static -Dwinboat-driver-version=100.6.101.$buildMinor"
    if (Test-Path "$mesaDir\$($build.Dir)\build.ninja") {
        # An existing build directory: reconfigure first (it reads options added since), then apply the options
        # (meson setup --reconfigure refuses options its last configuration didn't have)
        Invoke-Ewdk "$buildEnv cd /d $mesaDir && meson setup $($build.Dir) --reconfigure" $build.Arch
        Invoke-Ewdk "$buildEnv cd /d $mesaDir && meson configure $($build.Dir) --prefix=$($build.Prefix) $meson" $build.Arch
    } else {
        Invoke-Ewdk "$buildEnv cd /d $mesaDir && meson setup $($build.Dir) --prefix=$($build.Prefix) $meson" $build.Arch
    }

    # win_flex.exe uses fixed temp file names, so parallel runs collide: one fails ("error deleting file
    # ~flex_out_main_2") and the others can silently write each other's output. Generate the lexers one at a time.
    $lexers = "src/compiler/glsl/glcpp/glcpp-lex.c", "src/compiler/glsl/glsl_lexer.cpp", "src/mesa/program/lex.yy.c"
    Invoke-Ewdk "$buildEnv cd /d $mesaDir && ninja -C $($build.Dir) -j1 $($lexers -join ' ')" $build.Arch
    # Retry anyway, ninja resumes where it stopped
    for ($attempt = 1; ; $attempt++) {
        try {
            Invoke-Ewdk "$buildEnv cd /d $mesaDir && ninja -C $($build.Dir) install" $build.Arch
            break
        } catch {
            if ($attempt -ge 3) { throw }
            Write-Host "Mesa build failed (attempt $attempt), retrying..."
        }
    }
}

# Vulkan: Mesa's Venus driver, with clang-cl (Venus needs C11 atomics and typeof, which the EWDK's MSVC lacks), into
# the same prefixes as viogpu_vk[_x86].dll + .json. The package also installs the Khronos loader (Windows has none).
# Venus comes from a Mesa release of its own (mesa-venus): it forwards much more of the host's Vulkan (ray tracing,
# mesh shaders, the shader features of D3D12's shader models 6.6-6.8) than the Mesa the other drivers are built from.
$llvm = "C:\Program Files\LLVM\bin"
foreach ($build in $(if ($KernelOnly) { @() } else { $mesaBuilds })) {
    Write-Host "Building Mesa's Vulkan driver for $($build.Arch)..."
    $buildEnv = (Get-UserModeEnv $build.Lib) + "set `"PATH=$llvm;!PATH!`"&& "
    $mesaDir = "$SrcDir\mesa-venus"
    $vkDir = "vk$($build.Dir)"
    # 32-bit: -m32 in the compiler command (meson then sees an x86 compiler and archives for x86) and a cross build
    # (meson takes the host machine from the build machine otherwise)
    $m32 = if ($build.Arch -eq "x86") { ", '-m32'" } else { "" }
    $machineFile = "$BuildDir\clang-$($build.Arch).ini"
    $machine = "[binaries]`nc = ['$($llvm -replace '\\', '/')/clang-cl.exe'$m32]`ncpp = ['$($llvm -replace '\\', '/')/clang-cl.exe'$m32]`n" +
        "c_ld = 'lld-link'`ncpp_ld = 'lld-link'`nar = '$($llvm -replace '\\', '/')/llvm-lib.exe'`n"
    if ($build.Arch -eq "x86") { $machine += "[host_machine]`nsystem = 'windows'`ncpu_family = 'x86'`ncpu = 'i686'`nendian = 'little'`n" }
    Set-Content -Encoding ascii -Path $machineFile -Value $machine
    $machineArg = if ($build.Arch -eq "x86") { "--cross-file $machineFile" } else { "--native-file $machineFile" }
    # zlib off: its resource file would go through rc.exe with the compiler's -m32
    $meson = "-Dgallium-drivers= -Dvulkan-drivers=virtio -Dplatforms=windows -Dopengl=false -Degl=disabled " +
        "-Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dgallium-d3d10umd=false " +
        "-Db_vscrt=mt -Dbuildtype=release -Dzlib=disabled " +
        "-Dvulkan-virtio-dll-name=viogpu_vk$($build.Suffix)"
    if (Test-Path "$mesaDir\$vkDir\build.ninja") {
        Invoke-Ewdk "$buildEnv cd /d $mesaDir && meson setup $vkDir --reconfigure" $build.Arch
        Invoke-Ewdk "$buildEnv cd /d $mesaDir && meson configure $vkDir --prefix=$($build.Prefix) $meson" $build.Arch
    } else {
        Invoke-Ewdk "$buildEnv cd /d $mesaDir && meson setup $vkDir $machineArg --prefix=$($build.Prefix) $meson" $build.Arch
    }
    Invoke-Ewdk "$buildEnv cd /d $mesaDir && ninja -C $vkDir install" $build.Arch
    $loaderArch = if ($build.Arch -eq "x86") { "x86" } else { "x64" }
    Copy-Item "$BuildDir\tools\VulkanRT\$loaderArch\vulkan-1.dll" "$($build.Prefix)\bin\vulkan-1$($build.Suffix).dll" -Force
    # The loader is Apache-2.0/MIT: its license goes along with it
    Copy-Item "$BuildDir\tools\VulkanRT\VulkanRT-License.txt" "$($build.Prefix)\bin\vulkan-1-license.txt" -Force
}
# The AMF runtime (tools/amf-runtime, 64-bit: built on the host with MinGW and copied to C:\build\tools\amf-runtime):
# AMF encoders reach the host GPU through it. Its AMF SDK headers are MIT: their license goes along with it
if (-not $KernelOnly) {
    foreach ($f in "amfrt64.dll", "amfrt64-license.txt") {
        Copy-Item "$BuildDir\tools\amf-runtime\$f" "$MesaPrefix\bin\$f" -Force
    }
    # The Media Foundation encoders (tools/mf-encoder, 64-bit, MinGW on the host like the AMF runtime): Windows'
    # capture/recording and *_mf encoders reach the host GPU through them
    Copy-Item "$BuildDir\tools\mf-encoder\winboat_mft64.dll" "$MesaPrefix\bin\winboat_mft64.dll" -Force
}

# Direct3D 12: vkd3d-proton (on the Vulkan driver) with viogpu's user-mode driver on top, viogpu_d3d12[_x86].dll,
# with clang-cl (the machine files of the Vulkan step). glslang and widl (setup-build-env.ps1) generate its shaders and
# headers.
if (-not $KernelOnly) {
    $vkd3dDir = "$SrcDir\vkd3d-proton"
    foreach ($build in $mesaBuilds) {
        Write-Host "Building the Direct3D 12 driver (vkd3d-proton) for $($build.Arch)..."
        $buildEnv = (Get-UserModeEnv $build.Lib) + "set `"PATH=$llvm;$BuildDir\tools\bin;!PATH!`"&& "
        $machineFile = "$BuildDir\clang-$($build.Arch).ini"
        $machineArg = if ($build.Arch -eq "x86") { "--cross-file $machineFile" } else { "--native-file $machineFile" }
        $dll = "viogpu_d3d12$($build.Suffix)"
        $meson = "-Denable_viogpu_umd=true -Dviogpu_umd_name=$dll -Db_vscrt=mt -Dbuildtype=release"
        if (Test-Path "$vkd3dDir\$($build.Dir)\build.ninja") {
            Invoke-Ewdk "$buildEnv cd /d $vkd3dDir && meson setup $($build.Dir) --reconfigure" $build.Arch
            Invoke-Ewdk "$buildEnv cd /d $vkd3dDir && meson configure $($build.Dir) $meson" $build.Arch
        } else {
            Invoke-Ewdk "$buildEnv cd /d $vkd3dDir && meson setup $($build.Dir) $machineArg $meson" $build.Arch
        }
        Invoke-Ewdk "$buildEnv cd /d $vkd3dDir && ninja -C $($build.Dir) libs/viogpu_umd/$dll.dll" $build.Arch
        Copy-Item "$vkd3dDir\$($build.Dir)\libs\viogpu_umd\$dll.dll" "$($build.Prefix)\bin\$dll.dll" -Force
    }
    # It links vkd3d-proton (LGPL-2.1: the sources and our patches are published), dxil-spirv, dxbc-spirv (MIT) and
    # the SPIR-V headers (MIT): their licenses go along with it
    $notices = @(
        @("vkd3d-proton", "$vkd3dDir\LICENSE"), @("vkd3d-proton", "$vkd3dDir\COPYING"),
        @("dxil-spirv", "$vkd3dDir\subprojects\dxil-spirv\LICENSE.MIT"),
        @("dxbc-spirv", "$vkd3dDir\subprojects\dxil-spirv\subprojects\dxbc-spirv\LICENSE"),
        @("SPIRV-Headers", "$vkd3dDir\khronos\SPIRV-Headers\LICENSE"))
    $text = "viogpu_d3d12.dll and viogpu_d3d12_x86.dll contain the following software. Sources: " +
        "https://github.com/HansKristian-Work/vkd3d-proton (release v3.0.1 with its submodules) and WinBoat's patches to it.`r`n"
    foreach ($n in $notices) { $text += "`r`n==== $($n[0]): $(Split-Path -Leaf $n[1]) ====`r`n`r`n" + (Get-Content -Raw $n[1]) }
    Set-Content -Encoding utf8 -Path "$MesaPrefix\bin\viogpu_d3d12-licenses.txt" -Value $text
}

if ($UserModeOnly) {
    $installed = Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository" -Directory -Filter "viogpu3d.inf_*" |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $installed) { throw "viogpu3d isn't installed, run install-driver.ps1 first" }
    foreach ($build in $mesaBuilds) {
        foreach ($dll in "viogpu_wgl$($build.Suffix).dll", "viogpu_d3d9$($build.Suffix).dll",
                         "viogpu_d3d10$($build.Suffix).dll", "viogpu_vk$($build.Suffix).dll",
                         "viogpu_d3d12$($build.Suffix).dll") {
            if (-not (Test-Path "$($build.Prefix)\bin\$dll")) { continue }
            $dest = Join-Path $installed.FullName $dll
            try {
                try {
                    Copy-Item "$($build.Prefix)\bin\$dll" $dest -Force
                } catch {
                    # Loaded somewhere (dwm.exe keeps the D3D10 driver): a loaded DLL can still be renamed. Running
                    # processes keep the old copy, new ones load the new one.
                    Rename-Item $dest "$dll.old-$(Get-Date -Format yyyyMMddHHmmss)"
                    Copy-Item "$($build.Prefix)\bin\$dll" $dest -Force
                }
                Write-Host "Updated $dll in $($installed.FullName)"
            } catch {
                Write-Warning "Could not replace ${dll}: $_"
            }
        }
    }
    return
}

Write-Host "Building the viogpu3d kernel driver package (version 100.6.101.$buildMinor)..."
# amd64 only: VIRTIO_WIN_NO_ARM skips the ARM64 pass in buildAll.bat
$win81Env = "set _CL_=/DVIOGPU_WDDM1_3 /UNTDDI_VERSION /U_WIN32_WINNT /UWINVER /DNTDDI_VERSION=0x06030000 " +
    "/D_WIN32_WINNT=0x0603 /DWINVER=0x0603&& "
# Windows 7 (-Win7): the same WDDM 1.3 driver offering WDDM 1.1, at Windows 7's API level
if ($Win7) {
    $Win81 = $true
    $win81Env = "set _CL_=/DVIOGPU_WDDM1_3 /DVIOGPU_WIN7=1 /UNTDDI_VERSION /U_WIN32_WINNT /UWINVER " +
        "/DNTDDI_VERSION=0x06010000 /D_WIN32_WINNT=0x0601 /DWINVER=0x0601&& set VIOGPU_WIN7=1&& "
}
if ($ExtraDefines) { $win81Env = $win81Env -replace '^set _CL_=', "set _CL_=$ExtraDefines " }
Invoke-Ewdk ($(if ($Win81) { $win81Env } elseif ($ExtraDefines) { "set _CL_=$ExtraDefines&& " } else { "" }) +
    "set MESA_PREFIX=$MesaPrefix&& set MESA_PREFIX32=$MesaPrefix32&& set VIRTIO_WIN_NO_ARM=1&& " +
    "set _BUILD_MINOR_VERSION_=$buildMinor&& " +
    "cd /d $SrcDir\kvm-guest-drivers-windows\viogpu && build_AllNoSdv.bat $Target amd64")

Get-ChildItem -Recurse -Path "$SrcDir\kvm-guest-drivers-windows\viogpu\viogpu3d" -Filter "*.inf" |
    Where-Object { $_.FullName -match "objfre" } | ForEach-Object { Write-Host "Built: $($_.FullName)" }
