# Test-signs and installs the viogpu3d package built by build-driver.ps1 in the GPU spike VM.
# Needs test signing to be active (bcdedit /set testsigning on + reboot, done by the spike's install.bat).
# Reboot afterwards so the new display driver takes over from the 2D VioGpuDod driver.

$ErrorActionPreference = "Stop"

$Src = "C:\build\src\kvm-guest-drivers-windows"
$EwdkIso = "C:\build\EWDK_ge_release_26100_240331-1435.iso"

$inf = Get-ChildItem -Recurse -Path "$Src\viogpu\viogpu3d" -Filter "viogpu3d.inf" |
    Where-Object { $_.FullName -match "objfre_.*amd64" -and (Test-Path "$($_.DirectoryName)\viogpu3d.cat") } |
    Select-Object -First 1
if (-not $inf) { throw "No built viogpu3d.inf found, run build-driver.ps1 first" }
$package = $inf.DirectoryName
Write-Host "Package: $package"
Get-ChildItem $package | ForEach-Object { Write-Host "  $($_.Name)" }

# signtool from the EWDK
$image = Get-DiskImage -ImagePath $EwdkIso
if (-not $image.Attached) { $image = Mount-DiskImage -ImagePath $EwdkIso -PassThru }
$ewdk = (Get-Volume -DiskImage $image).DriveLetter + ":"
$signtool = Get-ChildItem "$ewdk\Program Files\Windows Kits\10\bin\10.*\x64\signtool.exe" | Select-Object -Last 1

# Sign binaries first, then the catalog (same as virtio-win's build\signAll.bat). A package installed again keeps its
# signatures: no binary to sign, so the catalog (signed then; signtool doesn't sign it twice) neither
$signedBinaries = $false
foreach ($pattern in "*.sys", "*.dll", "*.cat") {
    if ($pattern -eq "*.cat" -and -not $signedBinaries) { continue }
    Get-ChildItem $package -Filter $pattern | ForEach-Object {
        # Files that come signed (the Khronos Vulkan loader) keep their publisher's signature
        if ($pattern -ne "*.cat" -and (Get-AuthenticodeSignature $_.FullName).Status -eq "Valid") { return }
        if ($pattern -ne "*.cat") { $script:signedBinaries = $true }
        & $signtool.FullName sign /fd SHA256 /f "$Src\build\VirtIOTestCert.pfx" $_.FullName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Signing $($_.Name) failed" }
        Write-Host "Signed $($_.Name)"
    }
}

# Trust virtio-win's test certificate
# (certutil, because Import-Certificate fails with "Access is denied" when run as SYSTEM from a scheduled task)
foreach ($store in "Root", "TrustedPublisher") {
    certutil -f -addstore $store "$Src\build\VirtIOTestCert.cer" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Adding the test certificate to $store failed" }
}

if ((bcdedit /enum "{current}" | Select-String "testsigning\s+Yes") -eq $null) {
    Write-Warning "Test signing is not enabled in the boot configuration, the driver won't load"
}

# The new package first (each build's version is higher: build-driver.ps1), onto the device as it runs; then the earlier
# packages out of the store. Removing them first (with /uninstall) put the device on the 2D VioGpuDod driver, and a
# device waiting for its restart kept that one, the new package installed onto nothing present
$earlier = (pnputil /enum-drivers | Out-String) -split "(\r?\n){2,}" | Where-Object { $_ -match "Original Name:\s+viogpu3d\.inf" } |
    ForEach-Object { if ($_ -match "Published Name:\s+(oem\d+\.inf)") { $Matches[1] } }
$added = pnputil /add-driver $inf.FullName /install | Out-String
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "pnputil failed ($LASTEXITCODE): $added" }
$new = if ($added -match "Published Name:\s+(oem\d+\.inf)") { $Matches[1] } else { "" }
foreach ($old in $earlier | Where-Object { $_ -ne $new }) {
    Write-Host "Removing the previous package $old"
    pnputil /delete-driver $old | Out-Null
}
Write-Host "Driver installed, reboot to activate it"
