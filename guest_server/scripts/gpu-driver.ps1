#Requires -Version 2.0
<#
WinBoat virtual GPU driver (viogpu3d: virtio-gpu 3D kernel driver + Mesa OpenGL/D3D9/D3D10/D3D11 user-mode drivers)

    gpu-driver.ps1 -Action Status|Install|Uninstall [-PackageDir <dir>]

Prints JSON. The driver is test-signed, so Install:
  - signs the bundled package with a certificate created here and trusts only that certificate: its private key is
    deleted right after signing, so nothing else can be signed with it;
  - turns Windows test signing on (needs a reboot);
  - lets RDP sessions render on the GPU (bEnumerateHWBeforeSW);
  - raises RDP's frame rate from 30 to 60 fps and prefers H.264/AVC 444. Windows applies both only to full desktop
    sessions (WinBoat's "Windows Desktop"): RemoteApp windows stay at 30 fps without H.264;
  - installs the SPICE display route's helpers from drivers\spice (idle with RDP): the SPICE agent, which passes the
    viewer's size (and later the clipboard), and viogpuap.exe, which applies the size the driver reports, started at
    logon by a scheduled task;
  - installs the GPU check from drivers\gpucheck (what the GPU can do: Direct3D 9-12, Vulkan, OpenGL, OpenCL,
    DirectML, video) in Program Files, with a Start menu shortcut.
Everything Install changes is recorded under HKLM\SOFTWARE\WinBoat\VirtualGPU, and Uninstall reverts only that (test
signing stays on if it was on before).

Reads structured sources (Get-WindowsDriver, Get-PnpDevice, the registry) rather than pnputil/bcdedit text, which is
localized.

Windows 8.1 (PowerShell 4) gets the WDDM 1.3 package (drivers\viogpu3d-win81, the standalone installer's) and the
older tools' ways where Windows 10's are missing. Windows 7 (PowerShell 2) likewise its WDDM 1.1 one (drivers\viogpu3d-win7),
SHA-1 signatures (7 without KB3033929 can't check SHA-2 ones) and no SPICE helpers.
#>
param(
    [ValidateSet("Status", "Install", "Uninstall")] [string] $Action = "Status",
    [string] $PackageDir
)

$ErrorActionPreference = "Stop"
$Win81 = [Environment]::OSVersion.Version.Major -lt 10  # (Windows 7 too)
$Win7 = [Environment]::OSVersion.Version -lt (New-Object Version 6, 2)
if ($PSVersionTable.PSVersion.Major -lt 3) {
    # PowerShell 2 (Windows 7) runs scripts without $PSScriptRoot, [ordered] and ConvertTo-Json: plain hashtables
    # instead, and enough JSON for them
    $PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
    $accelerators = [psobject].Assembly.GetType("System.Management.Automation.TypeAccelerators")
    $accelerators::Add("ordered", [hashtable])
    function ConvertTo-Json($InputObject, $Depth, [switch] $Compress) {
        $value = $InputObject
        if ($null -eq $value) { return "null" }
        if ($value -is [bool]) { return "$value".ToLower() }
        if ($value -is [ValueType]) { return "$value" }
        if ($value -is [Collections.IDictionary]) {
            return "{" + (@($value.Keys | ForEach-Object { (ConvertTo-Json "$_") + ":" + (ConvertTo-Json $value[$_]) }) -join ",") + "}"
        }
        return '"' + ("$value" -replace '\\', '\\' -replace '"', '\"' -replace "`r", '\r' -replace "`n", '\n' -replace "`t", '\t') + '"'
    }
}
if (-not $PackageDir) {
    $PackageDir = Join-Path $PSScriptRoot "..\drivers\$(if ($Win7) { "viogpu3d-win7" } elseif ($Win81) { "viogpu3d-win81" } else { "viogpu3d" })"
}
# Windows 7 can't check SHA-2 signatures without KB3033929
$SignHash = if ($Win7) { "SHA1" } else { "SHA256" }
$StateKey = "HKLM:\SOFTWARE\WinBoat\VirtualGPU"
$TsPolicyKey = "HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services"
$GraphicsKey = "HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers"
$WinStationsKey = "HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations"
$CertSubject = "CN=WinBoat virtio-gpu driver"
# msiexec can't open a path with ".." in it
$SpiceDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\drivers\spice"))
$HelperTask = "WinBoat display resolution"
$HelperPath = Join-Path $env:ProgramData "WinBoat\viogpuap.exe"
$InfName = "viogpu3d.inf"
$CheckSource = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\drivers\gpucheck"))
$OpenClSource = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\drivers\opencl"))
$CheckDir = Join-Path $env:ProgramFiles "WinBoat\GPU Check"
$CheckShortcut = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\WinBoat GPU Check.lnk"
# (and on every user's desktop: the Public one)
$CheckDesktopShortcut = Join-Path ([Environment]::GetEnvironmentVariable("PUBLIC")) "Desktop\WinBoat GPU Check.lnk"

# System-wide DWORDs Install changes; their previous values are kept as state "Previous<Name>" for Uninstall. Entries
# without a Value are set by the INF.
$TrackedValues = @(
    @{ Key = "$GraphicsKey\Scheduler"; Name = "EnablePreemption" },
    @{ Key = $GraphicsKey; Name = "TdrDebugMode" },
    @{ Key = $GraphicsKey; Name = "TdrDelay" },
    @{ Key = $WinStationsKey; Name = "DWMFRAMEINTERVAL"; Value = 15 },  # 1000 / 15 ms = 60 fps
    @{ Key = $TsPolicyKey; Name = "AVC444ModePreferred"; Value = 1 }
)

function Get-State([string] $Name) {
    (Get-ItemProperty -Path $StateKey -Name $Name -ErrorAction SilentlyContinue).$Name
}

function Set-State([string] $Name, $Value) {
    if (-not (Test-Path $StateKey)) { New-Item -Path $StateKey -Force | Out-Null }
    if ($Value -is [string]) {
        Set-ItemProperty -Path $StateKey -Name $Name -Value $Value
    } else {
        Set-ItemProperty -Path $StateKey -Name $Name -Value ([int]$Value) -Type DWord
    }
}

function Get-InfVersion([string] $Inf) {
    $line = Select-String -Path $Inf -Pattern "^\s*DriverVer\s*=" | Select-Object -First 1
    if ($line -and $line.Line -match "=\s*([^,]+),\s*(\S+)") { return "$($Matches[2]) ($($Matches[1].Trim()))" }
    return $null
}

# Rebuilds keep the INF's version and date, so packages are told apart by their catalog, which lists every file's
# hash (taken before signing)
function Get-BundleFingerprint {
    $cat = Join-Path $PackageDir "viogpu3d.cat"
    if (-not (Test-Path $cat)) { return $null }
    $hash = [Security.Cryptography.SHA256]::Create().ComputeHash([IO.File]::ReadAllBytes($cat))  # (no Get-FileHash before PowerShell 4)
    return [BitConverter]::ToString($hash).Replace("-", "").ToLower()
}

function Get-InstalledPackages {
    if ($Win7) {
        # No DISM module: the published copies of our INF (oem<n>.inf, the ones naming viogpu3d.sys)
        return @(Select-String -Path "$env:SystemRoot\inf\oem*.inf" -Pattern "viogpu3d.sys" -SimpleMatch -List |
            ForEach-Object {
                $version = Select-String -Path $_.Path -Pattern "^\s*DriverVer\s*=\s*([^,]+),\s*(\S+)" | Select-Object -First 1
                New-Object PSObject -Property @{
                    Driver  = [IO.Path]::GetFileName($_.Path)
                    Version = $version.Matches[0].Groups[2].Value
                    Date    = [datetime]::ParseExact($version.Matches[0].Groups[1].Value.Trim(), "MM/dd/yyyy",
                        [Globalization.CultureInfo]::InvariantCulture)
                }
            })
    }
    @(Get-WindowsDriver -Online | Where-Object { [IO.Path]::GetFileName($_.OriginalFileName) -eq $InfName })
}

function Get-GpuDevice {
    if ($Win81) {
        # No PnpDevice module: WMI's devices (the present ones)
        return Get-WmiObject Win32_PnPEntity | Where-Object { $_.DeviceID -match "VEN_1AF4&DEV_1050" } |
            Select-Object -First 1 | ForEach-Object {
                New-Object PSObject -Property @{ FriendlyName = $_.Name; Status = $_.Status; Problem = $_.ConfigManagerErrorCode }
            }
    }
    Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
        Where-Object { $_.HardwareID -match "VEN_1AF4&DEV_1050" } |
        # The connected one: switching display routes moves the GPU to another PCI slot, and Windows keeps the old
        # one as a disconnected device (problem 45)
        Sort-Object -Property Present -Descending | Select-Object -First 1
}

function Test-TestSigningActive {
    # The options the running system booted with (bcdedit's text output is localized)
    $options = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control" -Name SystemStartOptions).SystemStartOptions
    return $options -match "TESTSIGNING"
}

function Invoke-Native([string] $File, [string[]] $Arguments, [int[]] $OkCodes = @(0)) {
    $output = & $File @Arguments 2>&1 | Out-String
    if ($OkCodes -notcontains $LASTEXITCODE) { throw "$File $($Arguments -join ' ') failed ($LASTEXITCODE): $output" }
    return $LASTEXITCODE
}

# Windows 8.1's New-SelfSignedCertificate makes no code signing certificates: CertEnroll does, the same way
function New-CodeSigningCertificate([string] $Subject) {
    $name = New-Object -ComObject X509Enrollment.CX500DistinguishedName
    $name.Encode($Subject, 0)
    $key = New-Object -ComObject X509Enrollment.CX509PrivateKey
    $key.ProviderName = "Microsoft Enhanced RSA and AES Cryptographic Provider"
    $key.MachineContext = $true
    $key.Length = 2048
    $key.KeySpec = 2       # signature
    $key.ExportPolicy = 0  # not exportable
    $key.Create()
    $codeSigning = New-Object -ComObject X509Enrollment.CObjectId
    $codeSigning.InitializeFromValue("1.3.6.1.5.5.7.3.3")
    $usages = New-Object -ComObject X509Enrollment.CObjectIds
    $usages.Add($codeSigning)
    $eku = New-Object -ComObject X509Enrollment.CX509ExtensionEnhancedKeyUsage
    $eku.InitializeEncode($usages)
    $hash = New-Object -ComObject X509Enrollment.CObjectId
    $hash.InitializeFromAlgorithmName(1, 0, 0, $SignHash)  # a hash algorithm
    $request = New-Object -ComObject X509Enrollment.CX509CertificateRequestCertificate
    $request.InitializeFromPrivateKey(2, $key, "")  # the machine's
    $request.Subject = $name
    $request.Issuer = $name
    $request.NotBefore = (Get-Date).AddDays(-1)
    $request.NotAfter = (Get-Date).AddYears(30)
    $request.X509Extensions.Add($eku)
    $request.HashAlgorithm = $hash
    $request.Encode()
    $enrollment = New-Object -ComObject X509Enrollment.CX509Enrollment
    $enrollment.InitializeFromRequest($request)
    $enrollment.InstallResponse(2, $enrollment.CreateRequest(0), 0, "")  # self-signed: untrusted until trusted below
    $thumbprint = (New-Object Security.Cryptography.X509Certificates.X509Certificate2(
        , [Convert]::FromBase64String($enrollment.Certificate(1)))).Thumbprint
    return Get-Item "Cert:\LocalMachine\My\$thumbprint"
}

# Windows 8.1's pnputil deletes the package but leaves the device on it (code 52 after the restart): removing the
# device, as devcon remove does (DIF_REMOVE, done at the restart while it runs), has Windows find it again then, onto
# its basic display driver
function Remove-GpuDevice {
    $device = Get-WmiObject Win32_PnPEntity | Where-Object { $_.DeviceID -match "VEN_1AF4&DEV_1050" }
    if (-not $device) { return }
    Add-Type -Namespace WinBoat -Name SetupApi -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
[DllImport("setupapi.dll", SetLastError = true)] public static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr classGuid, IntPtr parent);
[DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool SetupDiOpenDeviceInfo(IntPtr set, string instanceId, IntPtr parent, int flags, ref SP_DEVINFO_DATA data);
[DllImport("setupapi.dll", SetLastError = true)] public static extern bool SetupDiCallClassInstaller(int function, IntPtr set, ref SP_DEVINFO_DATA data);
[DllImport("setupapi.dll")] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
'@
    foreach ($id in @($device | ForEach-Object { $_.DeviceID })) {
        $set = [WinBoat.SetupApi]::SetupDiCreateDeviceInfoList([IntPtr]::Zero, [IntPtr]::Zero)
        try {
            $data = New-Object WinBoat.SetupApi+SP_DEVINFO_DATA
            $data.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($data)
            if (-not [WinBoat.SetupApi]::SetupDiOpenDeviceInfo($set, $id, [IntPtr]::Zero, 0, [ref]$data) -or
                -not [WinBoat.SetupApi]::SetupDiCallClassInstaller(5, $set, [ref]$data)) {  # DIF_REMOVE
                throw "Removing the device $id failed ($([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
            }
        } finally {
            [WinBoat.SetupApi]::SetupDiDestroyDeviceInfoList($set) | Out-Null
        }
    }
}

function Remove-OurCertificates {
    foreach ($store in "Root", "TrustedPublisher", "My") {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -like "$CertSubject*" } | ForEach-Object {
            # certutil: the certificate provider can't change the machine stores when run as SYSTEM
            Invoke-Native certutil.exe @("-delstore", $store, $_.Thumbprint) | Out-Null
        }
    }
}

function Install-SpiceHelpers {
    $msi = Join-Path $SpiceDir "spice-vdagent-x64.msi"
    if ((Test-Path $msi) -and -not (Get-Service spice-agent -ErrorAction SilentlyContinue)) {
        $setup = Start-Process msiexec.exe -ArgumentList "/i", "`"$msi`"", "/qn", "/norestart" -Wait -PassThru
        if (@(0, 3010) -notcontains $setup.ExitCode) { throw "Installing the SPICE agent failed ($($setup.ExitCode))" }
        Set-State "SpiceAgentInstalled" 1
    }

    $helper = Join-Path $SpiceDir "viogpuap.exe"
    if (Test-Path $helper) {
        Unregister-ScheduledTask -TaskName $HelperTask -Confirm:$false -ErrorAction SilentlyContinue
        # gone before the copy (Stop-Process doesn't wait: an update over a running helper failed on its exe)
        Get-Process viogpuap -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit(10000) | Out-Null }
        New-Item -ItemType Directory -Path (Split-Path $HelperPath) -Force | Out-Null
        Copy-Item $helper $HelperPath -Force
        # Elevated in each administrator's session (it opens the driver's event); a console program, so headless
        $action = if ($Win81) {
            # No headless console host there: PowerShell's window hides itself, the helper shares it
            New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -WindowStyle Hidden -Command & '$HelperPath'"
        } else {
            New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\conhost.exe" -Argument "--headless `"$HelperPath`""
        }
        $principal = New-ScheduledTaskPrincipal -GroupId "S-1-5-32-544" -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries `
            -DontStopIfGoingOnBatteries -MultipleInstances Parallel
        Register-ScheduledTask -TaskName $HelperTask -Action $action -Trigger (New-ScheduledTaskTrigger -AtLogOn) `
            -Principal $principal -Settings $settings | Out-Null
    }
}

function Uninstall-SpiceHelpers {
    Unregister-ScheduledTask -TaskName $HelperTask -Confirm:$false -ErrorAction SilentlyContinue
    Stop-Process -Name viogpuap -Force -ErrorAction SilentlyContinue
    Remove-Item $HelperPath -Force -ErrorAction SilentlyContinue
    $msi = Join-Path $SpiceDir "spice-vdagent-x64.msi"
    if ((Get-State "SpiceAgentInstalled") -eq 1 -and (Test-Path $msi)) {
        Start-Process msiexec.exe -ArgumentList "/x", "`"$msi`"", "/qn", "/norestart" -Wait | Out-Null
    }
}

function Install-GpuCheck {
    if (-not (Test-Path (Join-Path $CheckSource "winboat-dxtest.exe"))) { return }
    New-Item -ItemType Directory -Path $CheckDir -Force | Out-Null
    Copy-Item (Join-Path $CheckSource "*") $CheckDir -Recurse -Force  # (Windows 8.1's: the 32-bit part in 32\)
    $shell = New-Object -ComObject WScript.Shell
    foreach ($path in $CheckShortcut, $CheckDesktopShortcut) {
        $link = $shell.CreateShortcut($path)
        $link.TargetPath = Join-Path $CheckDir "winboat-dxtest.exe"
        $link.Arguments = "--gui"  # a window with a row per API
        $link.WorkingDirectory = $CheckDir
        $link.WindowStyle = 7      # minimized: the console it starts with closes at once, unseen
        $link.Description = "What the GPU can do: Direct3D, Vulkan, OpenGL, OpenCL, DirectML, video"
        $link.Save()
    }
}

# Windows 10 has no OpenCL.dll (11 has one): Khronos' loader (tools/opencl-loader), with which Microsoft's OpenCL,
# OpenGL and Vulkan Compatibility Pack (OpenCLOn12, on this driver's Direct3D 12) runs OpenCL on the GPU. Only where
# Windows has none or it's ours (state OpenClLoader<x64|x86>); never over Windows 11's or a GPU vendor's
function Install-OpenClLoader {
    if ($Win81 -or -not (Test-Path (Join-Path $OpenClSource "x64\OpenCL.dll"))) { return }
    $targets = @(@{ From = "x64"; To = "$env:SystemRoot\System32" })
    if (Test-Path "$env:SystemRoot\SysWOW64") { $targets += @{ From = "x86"; To = "$env:SystemRoot\SysWOW64" } }
    foreach ($t in $targets) {
        $dest = Join-Path $t.To "OpenCL.dll"
        if ((Test-Path $dest) -and -not (Get-State "OpenClLoader$($t.From)")) { continue }
        try {
            Copy-Item (Join-Path $OpenClSource "$($t.From)\OpenCL.dll") $dest -Force
            Set-State "OpenClLoader$($t.From)" 1
        } catch {
            # (in use by a running OpenCL program: the one there stays until the next install)
        }
    }
}

function Uninstall-OpenClLoader {
    foreach ($t in @(@{ From = "x64"; To = "$env:SystemRoot\System32" }, @{ From = "x86"; To = "$env:SystemRoot\SysWOW64" })) {
        if (Get-State "OpenClLoader$($t.From)") { Remove-Item (Join-Path $t.To "OpenCL.dll") -Force -ErrorAction SilentlyContinue }
    }
}

function Uninstall-GpuCheck {
    Remove-Item $CheckShortcut, $CheckDesktopShortcut -Force -ErrorAction SilentlyContinue
    Remove-Item $CheckDir -Recurse -Force -ErrorAction SilentlyContinue
    $parent = Split-Path $CheckDir  # WinBoat's folder, when nothing else is in it
    if ((Test-Path $parent) -and -not (Get-ChildItem $parent -Force)) { Remove-Item $parent -Force }
}

function Get-Status {
    $bundledInf = Join-Path $PackageDir $InfName
    $installed = Get-InstalledPackages | Sort-Object { $_.Driver } | Select-Object -Last 1
    $device = Get-GpuDevice
    $installedAt = Get-State "InstalledAt"
    $lastBoot = [Management.ManagementDateTimeConverter]::ToDateTime((Get-WmiObject Win32_OperatingSystem).LastBootUpTime)
    $rebootRequired = $false
    if ($installedAt -and ([datetime]::Parse($installedAt) -gt $lastBoot)) { $rebootRequired = $true }
    if ($device -and $device.Problem -eq 14) { $rebootRequired = $true }  # CM_PROB_NEED_RESTART
    [ordered]@{
        bundled        = [ordered]@{
            present     = Test-Path $bundledInf
            version     = if (Test-Path $bundledInf) { Get-InfVersion $bundledInf } else { $null }
            fingerprint = Get-BundleFingerprint
        }
        installed      = [ordered]@{
            present     = [bool]$installed
            version     = if ($installed) { "$($installed.Version) ($($installed.Date.ToString('MM/dd/yyyy')))" } else { $null }
            package     = if ($installed) { $installed.Driver } else { $null }
            fingerprint = if ($installed) { Get-State "InstalledFingerprint" } else { $null }
        }
        device         = if ($device) {
            [ordered]@{
                name    = $device.FriendlyName
                status  = "$($device.Status)"
                problem = [int]$device.Problem
            }
        } else { $null }
        testSigning    = Test-TestSigningActive
        rdpHardwareGpu = (Get-ItemProperty $TsPolicyKey -Name bEnumerateHWBeforeSW -ErrorAction SilentlyContinue).bEnumerateHWBeforeSW -eq 1
        spiceHelpers   = -not $Win7 -and [bool](Get-ScheduledTask -TaskName $HelperTask -ErrorAction SilentlyContinue) -and
            [bool](Get-Service spice-agent -ErrorAction SilentlyContinue)
        # Windows signs in by itself (the SPICE display route turns it on; WinBoat's installer leaves it off)
        autoLogon      = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" -Name AutoAdminLogon -ErrorAction SilentlyContinue).AutoAdminLogon -eq "1"
        rebootRequired = $rebootRequired
    }
}

function Install-Driver {
    $bundledInf = Join-Path $PackageDir $InfName
    if (-not (Test-Path $bundledInf)) { throw "No driver package in $PackageDir" }
    # Test signing can't be turned on under Secure Boot (Confirm-SecureBootUEFI throws on BIOS firmware)
    $secureBoot = try { Confirm-SecureBootUEFI } catch { $false }
    if ($secureBoot) {
        throw "Secure Boot is on, so Windows won't load the test-signed driver. Turn Secure Boot off in the VM's " +
            "firmware (OVMF: press Esc at boot, Device Manager > Secure Boot Configuration) or boot it with an OVMF " +
            "build without Secure Boot, then install again."
    }

    # Sign a copy: the bundled package stays as shipped
    $work = Join-Path $env:ProgramData "WinBoat\viogpu3d-package"
    if (Test-Path $work) { Remove-Item $work -Recurse -Force }
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    Copy-Item (Join-Path $PackageDir "*") $work -Recurse

    # A certificate for this VM only. Trust it before signing, then delete its private key.
    Remove-OurCertificates
    $cert = if ($Win81) { New-CodeSigningCertificate "$CertSubject ($env:COMPUTERNAME)" } else {
        New-SelfSignedCertificate -Type CodeSigningCert -Subject "$CertSubject ($env:COMPUTERNAME)" `
            -CertStoreLocation Cert:\LocalMachine\My -HashAlgorithm SHA256 -KeyExportPolicy NonExportable `
            -NotBefore (Get-Date).AddDays(-1) -NotAfter (Get-Date).AddYears(30)  # a day back: the clock can still move after boot
    }
    try {
        $cer = Join-Path $work "winboat-viogpu3d.cer"
        [IO.File]::WriteAllBytes($cer, $cert.Export("Cert"))
        foreach ($store in "Root", "TrustedPublisher") {
            Invoke-Native certutil.exe @("-f", "-addstore", $store, $cer) | Out-Null
        }
        # Binaries first, then the catalog that lists their hashes
        foreach ($pattern in "*.sys", "*.dll", "*.cat") {
            foreach ($file in Get-ChildItem $work -Filter $pattern) {
                # Files that come signed (the Khronos Vulkan loader) keep their publisher's signature
                if ($pattern -ne "*.cat" -and (Get-AuthenticodeSignature $file.FullName).Status -eq "Valid") { continue }
                $result = Set-AuthenticodeSignature -FilePath $file.FullName -Certificate $cert -HashAlgorithm $SignHash
                if ($result.Status -ne "Valid") { throw "Signing $($file.Name) failed: $($result.StatusMessage)" }
            }
        }
    } finally {
        if ($Win7) {
            # (PowerShell 2's certificate provider deletes nothing: the key's container, then the certificate)
            $cert.PrivateKey.PersistKeyInCsp = $false
            $cert.PrivateKey.Clear()
            & certutil.exe -delstore My $cert.Thumbprint 2>&1 | Out-Null
        } else {
            Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey -ErrorAction SilentlyContinue
        }
    }

    # Test signing, remembering whether it was already on
    if ($null -eq (Get-State "TestSigningWasOn")) { Set-State "TestSigningWasOn" (Test-TestSigningActive) }
    Invoke-Native bcdedit.exe @("/set", "testsigning", "on") | Out-Null

    # Remember the previous values so Uninstall can put them back
    foreach ($value in $TrackedValues) {
        $stateName = "Previous$($value.Name)"
        if ($null -eq (Get-State $stateName)) {
            $previous = (Get-ItemProperty $value.Key -Name $value.Name -ErrorAction SilentlyContinue).($value.Name)
            Set-State $stateName $(if ($null -eq $previous) { "absent" } else { "$previous" })
        }
    }

    # The new package first, onto the device as it runs (each build's version is higher), then the earlier packages
    # out of the store. Removing them first (with /uninstall) put the device on the 2D VioGpuDod driver, and a device
    # still waiting for its restart could keep that one: the new package installed onto nothing present
    $earlier = @(Get-InstalledPackages | ForEach-Object { $_.Driver })
    # (Windows 8.1's pnputil has the older switches, and exits with 259 when it installed the package on no device:
    # none there, or the device has it already)
    if ($Win81) {
        Invoke-Native pnputil.exe @("-i", "-a", (Join-Path $work $InfName)) @(0, 3010, 259) | Out-Null
    } else {
        Invoke-Native pnputil.exe @("/add-driver", (Join-Path $work $InfName), "/install") @(0, 3010) | Out-Null
    }
    $added = @(Get-InstalledPackages | Where-Object { $earlier -notcontains $_.Driver })
    if ($added.Count) {
        foreach ($old in $earlier) {
            if ($Win81) { & pnputil.exe -d $old 2>&1 | Out-Null } else {
                & pnputil.exe /delete-driver $old 2>&1 | Out-Null   # one still in use stays (exit code ignored)
            }
        }
    }

    # RDP sessions (how WinBoat shows apps) use the hardware GPU instead of the software renderer
    if ($null -eq (Get-State "RdpPolicyWasSet")) {
        $was = (Get-ItemProperty $TsPolicyKey -Name bEnumerateHWBeforeSW -ErrorAction SilentlyContinue).bEnumerateHWBeforeSW
        Set-State "RdpPolicyWasSet" ($was -eq 1)
    }
    if (-not (Test-Path $TsPolicyKey)) { New-Item -Path $TsPolicyKey -Force | Out-Null }
    Set-ItemProperty -Path $TsPolicyKey -Name bEnumerateHWBeforeSW -Value 1 -Type DWord

    # RDP display settings (read at boot)
    foreach ($value in $TrackedValues | Where-Object { $null -ne $_.Value }) {
        if (-not (Test-Path $value.Key)) { New-Item -Path $value.Key -Force | Out-Null }
        Set-ItemProperty -Path $value.Key -Name $value.Name -Value $value.Value -Type DWord
    }

    if (-not $Win7) { Install-SpiceHelpers }  # (Windows 7: no ScheduledTasks module; untested there)
    Install-GpuCheck
    Install-OpenClLoader

    Set-State "InstalledAt" (Get-Date).ToString("o")
    Set-State "InstalledFingerprint" (Get-BundleFingerprint)
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

function Uninstall-Driver {
    foreach ($package in Get-InstalledPackages) {
        $deleteArgs = if ($Win81) { @("-f", "-d", $package.Driver) } else { @("/delete-driver", $package.Driver, "/uninstall", "/force") }
        Invoke-Native pnputil.exe $deleteArgs @(0, 3010) | Out-Null
    }
    if ($Win81) { Remove-GpuDevice }
    # The INF's AMF runtime stays in System32 (pnputil leaves copied files): without the driver, programs would find
    # AMF and fail to encode. Only ours (AMD's own carries AMD's company name)
    $amf = "$env:SystemRoot\System32\amfrt64.dll"
    if ((Test-Path $amf) -and (Get-Item $amf).VersionInfo.CompanyName -eq "WinBoat") {
        Remove-Item $amf -Force -ErrorAction SilentlyContinue
    }
    # The Media Foundation encoders likewise, their COM classes and Media Foundation entries unregistered first
    # (regsvr32 is a GUI program: Start-Process to wait for it)
    $mft = "$env:SystemRoot\System32\winboat_mft64.dll"
    if ((Test-Path $mft) -and (Get-Item $mft).VersionInfo.CompanyName -eq "WinBoat") {
        Start-Process regsvr32.exe -ArgumentList "/s", "/u", "`"$mft`"" -Wait -WindowStyle Hidden
        Remove-Item $mft -Force -ErrorAction SilentlyContinue
    }
    Remove-OurCertificates

    if ((Get-State "TestSigningWasOn") -eq 0) {
        Invoke-Native bcdedit.exe @("/set", "testsigning", "off") | Out-Null
    }
    if ((Get-State "RdpPolicyWasSet") -eq 0) {
        Remove-ItemProperty -Path $TsPolicyKey -Name bEnumerateHWBeforeSW -ErrorAction SilentlyContinue
    }
    foreach ($value in $TrackedValues) {
        $previous = Get-State "Previous$($value.Name)"
        if ($previous -eq "absent") {
            Remove-ItemProperty -Path $value.Key -Name $value.Name -ErrorAction SilentlyContinue
        } elseif ($null -ne $previous) {
            # Removing the driver can take the Scheduler key with it
            if (-not (Test-Path $value.Key)) { New-Item -Path $value.Key -Force | Out-Null }
            Set-ItemProperty -Path $value.Key -Name $value.Name -Value ([int]$previous) -Type DWord
        }
    }
    if (-not $Win7) { Uninstall-SpiceHelpers }
    Uninstall-GpuCheck
    Uninstall-OpenClLoader
    Remove-Item $StateKey -Recurse -Force -ErrorAction SilentlyContinue
    # Reboot-pending marker for Status: the device falls back to the basic display driver on the next boot
    New-Item -Path $StateKey -Force | Out-Null
    Set-State "InstalledAt" (Get-Date).ToString("o")
}

# (PowerShell 2 wraps output at the console's width, inside JSON strings too: written to stdout as it is there)
function Write-Json($Value) {
    $json = ConvertTo-Json -InputObject $Value -Depth 4 -Compress
    if ($PSVersionTable.PSVersion.Major -lt 3) { [Console]::Out.WriteLine($json) } else { $json }
}

try {
    switch ($Action) {
        "Install" { Install-Driver }
        "Uninstall" { Uninstall-Driver }
    }
    $status = Get-Status
    if ($Action -ne "Status") { $status.rebootRequired = $true }
    Write-Json $status
} catch {
    Write-Json ([ordered]@{ error = "$($_.Exception.Message)" })
    exit 1
}
