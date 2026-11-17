# Packages a Release build of RadPad as an MSIX for the Microsoft Store: dist\RadPad-<platform>.msix.
# The package holds the same files as the zip and installer (the app stays self-contained), plus the
# manifest from installer\msix\AppxManifest.xml and logos drawn by MakeIcon.ps1.
#
# Usage, after a Release build of the platform:
#   powershell -File tools/MakeMsix.ps1 -IdentityName <Package/Identity/Name> -Publisher "CN=..." -PublisherDisplayName <name> [-Platform x64]
# The identity values are on the app's Product identity page in Partner Center. Store uploads needn't be
# signed (the Store signs them); to install the package locally, pass -CertificatePath <pfx> as well, for a
# certificate whose subject is the same as -Publisher and which the PC trusts.
param(
    [Parameter(Mandatory)][string]$IdentityName,
    [Parameter(Mandatory)][string]$Publisher,
    [Parameter(Mandatory)][string]$PublisherDisplayName,
    [ValidateSet("x64", "ARM64", "Win32")][string]$Platform = "x64",
    [string]$SourceDir,
    [string]$CertificatePath,
    [string]$CertificatePassword
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
if (-not $SourceDir) { $SourceDir = Join-Path $root "$Platform\Release\RadPad" }
if (-not (Test-Path (Join-Path $SourceDir "RadPad.exe"))) { throw "RadPad.exe isn't in $SourceDir. Build Release|$Platform first." }

# The newest Windows SDK's packaging tools.
$sdkBin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*" -Directory | Sort-Object { [version]$_.Name } -Descending |
    Where-Object { Test-Path "$($_.FullName)\x64\makeappx.exe" } | Select-Object -First 1
if (-not $sdkBin) { throw "makeappx.exe wasn't found. Install the Windows SDK." }
$makeappx = "$($sdkBin.FullName)\x64\makeappx.exe"; $signtool = "$($sdkBin.FullName)\x64\signtool.exe"

# Version.h has major.minor.patch; the Store needs four parts, the last 0.
$header = Get-Content (Join-Path $root "RadPad\Version.h") -Raw
$part = { param($name) [regex]::Match($header, "RADPAD_VERSION_$name\s+(\d+)").Groups[1].Value }
$version = "$(& $part MAJOR).$(& $part MINOR).$(& $part PATCH).0"

# The file types RadPad adds itself to "Open with" for, taken from the installer so the two agree.
# Windows doesn't let packaged apps associate some types (scripts and the like), so those are left out.
$iss = Get-Content (Join-Path $root "installer\RadPad.iss") -Raw
$extensions = [regex]::Match($iss, '#define Extensions "([^"]+)"').Groups[1].Value -split " "
$blocked = "bat", "cmd", "inf", "js", "ps1", "psd1", "psm1", "reg"
$fileTypes = ($extensions | Where-Object { $_ -notin $blocked } | ForEach-Object { "              <uap:FileType>.$_</uap:FileType>" }) -join "`r`n"

# Stage the package's contents: the build output without development files, the manifest and the logos.
$stage = Join-Path $env:TEMP "RadPad-msix-$Platform"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage | Out-Null
robocopy $SourceDir $stage /E /XF *.pdb *.lib *.exp RadPad.winmd *.ico /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Copying $SourceDir failed (robocopy exit code $LASTEXITCODE)." }
New-Item -ItemType Directory "$stage\Assets" | Out-Null
# MakeIcon.ps1 compiles C# with Add-Type, which fails on folders in LIB that don't exist, as MSBuild's LIB
# for some platforms has (ARM64's atlmfc, say). It needs no libraries, so don't pass LIB on.
$env:LIB = $null
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "MakeIcon.ps1") -AssetDir "$stage\Assets" | Out-Null
if ($LASTEXITCODE) { throw "MakeIcon.ps1 failed." }

$architecture = @{ x64 = "x64"; ARM64 = "arm64"; Win32 = "x86" }[$Platform]
$manifest = (Get-Content (Join-Path $root "installer\msix\AppxManifest.xml") -Raw).
    Replace('$IdentityName$', [Security.SecurityElement]::Escape($IdentityName)).
    Replace('$Publisher$', [Security.SecurityElement]::Escape($Publisher)).
    Replace('$PublisherDisplayName$', [Security.SecurityElement]::Escape($PublisherDisplayName)).
    Replace('$Version$', $version).Replace('$Architecture$', $architecture).Replace('$FileTypes$', $fileTypes)
[IO.File]::WriteAllText("$stage\AppxManifest.xml", $manifest, (New-Object Text.UTF8Encoding $false))

$outDir = Join-Path $root "dist"
New-Item -ItemType Directory $outDir -Force | Out-Null
$msix = Join-Path $outDir "RadPad-$Platform.msix"
$log = & $makeappx pack /d $stage /p $msix /o
if ($LASTEXITCODE) { $log | Where-Object { $_ -notlike "Processing *" }; throw "makeappx failed (exit code $LASTEXITCODE)." }

if ($CertificatePath) {
    $passwordArgs = if ($CertificatePassword) { "/p", $CertificatePassword } else { @() }
    & $signtool sign /fd SHA256 /f $CertificatePath @passwordArgs $msix
    if ($LASTEXITCODE) { throw "signtool failed (exit code $LASTEXITCODE)." }
}
Remove-Item $stage -Recurse -Force
"RadPad $version ($architecture) packaged as $msix"
