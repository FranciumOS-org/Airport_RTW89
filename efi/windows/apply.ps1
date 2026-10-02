# Adds the old Wi-Fi stack to the OpenCore volume, from Windows.
# Run from an administrator PowerShell:
#   powershell -ExecutionPolicy Bypass -File S:\rtw89-wifistack\apply.ps1
# It only goes ahead if the stick's config.plist is the one this was prepared
# from; the previous config is kept as config.plist.pre-wifistack.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$expected = '@EXPECTED@'
$patched = (Get-FileHash "$here\config.patched.plist" -Algorithm SHA256).Hash

$oc = $null
foreach ($d in Get-PSDrive -PSProvider FileSystem) {
    $p = Join-Path $d.Root 'EFI\OC\config.plist'
    if (Test-Path $p) { $oc = Join-Path $d.Root 'EFI\OC'; break }
}
if (-not $oc) { Write-Host 'No drive with EFI\OC\config.plist found. Is the OpenCore stick plugged in?'; exit 1 }
Write-Host "OpenCore found at $oc"

$current = (Get-FileHash "$oc\config.plist" -Algorithm SHA256).Hash
if ($current -eq $patched) { Write-Host 'This config already has the Wi-Fi stack. Nothing to do.'; exit 0 }
if ($current -ne $expected) {
    Copy-Item "$oc\config.plist" "$here\config.from-stick.plist" -Force
    Write-Host 'The config.plist on the stick is not the one this was prepared from.'
    Write-Host "Nothing was changed. A copy of it is now at $here\config.from-stick.plist:"
    Write-Host 'boot macOS again and the change will be prepared from that copy.'
    exit 1
}

if (-not (Test-Path "$oc\config.plist.pre-wifistack")) {
    Copy-Item "$oc\config.plist" "$oc\config.plist.pre-wifistack"
    Write-Host 'Saved the current config as config.plist.pre-wifistack'
}
foreach ($k in 'IOSkywalkFamily.kext', 'IO80211FamilyLegacy.kext', 'AMFIPass.kext') {
    if (Test-Path "$oc\Kexts\$k") {
        Write-Host "$k is already in EFI\OC\Kexts, left as it is"
    } else {
        Copy-Item "$here\Kexts\$k" "$oc\Kexts\$k" -Recurse
        Write-Host "Copied $k"
    }
}
Copy-Item "$here\config.patched.plist" "$oc\config.plist" -Force
if ((Get-FileHash "$oc\config.plist" -Algorithm SHA256).Hash -ne $patched) {
    Copy-Item "$oc\config.plist.pre-wifistack" "$oc\config.plist" -Force
    Write-Host 'The new config did not copy correctly; the previous one is back.'
    exit 1
}
Write-Host 'Done. Eject the stick safely, then restart into macOS.'
Write-Host 'To undo: copy config.plist.pre-wifistack over config.plist in EFI\OC on the stick.'
