# Based on LABSN/sound-ci-helpers windows/setup_sound.ps1:
# https://github.com/LABSN/sound-ci-helpers/blob/20a2b9bbd21fd005d8fe89933901506dba84ea4e/windows/setup_sound.ps1
#
# The original helper downloaded VB-CABLE Pack43, trusted the VB-Audio
# certificate, and installed vbMmeCable64_win7.inf with devcon. This local
# version keeps that behavior without cloning the helper repository at CI
# runtime. The vendored devcon.exe is from:
# https://github.com/LABSN/sound-ci-helpers/blob/20a2b9bbd21fd005d8fe89933901506dba84ea4e/windows/devcon.exe

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$driverUrl = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip"
$expectedSha256 = "66fd0a4d9f4896ff41632b7e3d53892c085c4561f53e8ae8d0f0bc10eedd1cdd"
$devconExpectedSha256 = "97cff42f8c0fe4fbdf991273159516bf78090625a933c3983ebd6f62284e329a"
$hardwareId = "VBAudioVACWDM"
$devcon = Join-Path $PSScriptRoot "..\tools\windows\devcon.exe"
$runnerTemp = $env:RUNNER_TEMP
if ([string]::IsNullOrEmpty($runnerTemp)) {
  $runnerTemp = [System.IO.Path]::GetTempPath()
}
$workDir = Join-Path $runnerTemp "vbcable"
$cacheDir = $env:VBCABLE_CACHE_DIR
if ([string]::IsNullOrEmpty($cacheDir)) {
  $cacheDir = Join-Path $runnerTemp "vbcable-cache"
}
$zipName = "VBCABLE_Driver_Pack43.zip"
$cachedZipPath = Join-Path $cacheDir $zipName
$zipPath = Join-Path $workDir $zipName
$extractDir = Join-Path $workDir "driver"

function Test-ExpectedHash {
  param(
    [Parameter(Mandatory = $true)]
    [string] $Path,
    [Parameter(Mandatory = $true)]
    [string] $ExpectedSha256
  )

  if (!(Test-Path $Path)) {
    return $false
  }

  $actualSha256 = (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
  return $actualSha256 -eq $ExpectedSha256
}

function Invoke-Checked {
  param(
    [Parameter(Mandatory = $true)]
    [string] $FilePath,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Arguments
  )

  Write-Host "Running: $FilePath $($Arguments -join ' ')"
  & $FilePath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "$FilePath failed with exit code $LASTEXITCODE."
  }
}

if (!(Test-ExpectedHash -Path $devcon -ExpectedSha256 $devconExpectedSha256)) {
  $actualSha256 = if (Test-Path $devcon) {
    (Get-FileHash -Algorithm SHA256 -Path $devcon).Hash.ToLowerInvariant()
  } else {
    "<missing>"
  }
  throw "Unexpected devcon.exe hash. Expected $devconExpectedSha256, got $actualSha256."
}

New-Item -ItemType Directory -Force -Path $workDir, $cacheDir | Out-Null
if (Test-Path $extractDir) {
  Remove-Item -Recurse -Force $extractDir
}

if (Test-ExpectedHash -Path $cachedZipPath -ExpectedSha256 $expectedSha256) {
  Write-Host "Using cached VB-CABLE package: $cachedZipPath"
  Copy-Item -Force $cachedZipPath $zipPath
} else {
  if (Test-Path $cachedZipPath) {
    Write-Host "Ignoring cached VB-CABLE package with unexpected hash: $cachedZipPath"
    Remove-Item -Force $cachedZipPath
  }

  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  Write-Host "Downloading $driverUrl"
  Invoke-WebRequest -Uri $driverUrl -OutFile $zipPath -MaximumRetryCount 3 -RetryIntervalSec 2

  if (!(Test-ExpectedHash -Path $zipPath -ExpectedSha256 $expectedSha256)) {
    $actualSha256 = (Get-FileHash -Algorithm SHA256 -Path $zipPath).Hash.ToLowerInvariant()
    throw "Unexpected VB-CABLE package hash. Expected $expectedSha256, got $actualSha256."
  }

  Copy-Item -Force $zipPath $cachedZipPath
}

Expand-Archive -LiteralPath $zipPath -DestinationPath $extractDir

$catalogPath = Join-Path $extractDir "vbaudio_cable64_win7.cat"
$signature = Get-AuthenticodeSignature -FilePath $catalogPath
if ($null -eq $signature.SignerCertificate) {
  throw "Could not read signer certificate from $catalogPath."
}

$certPath = Join-Path $workDir "vbcable.cer"
[System.IO.File]::WriteAllBytes(
  $certPath,
  $signature.SignerCertificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
)

& certutil.exe -addstore -f "TrustedPublisher" $certPath
if ($LASTEXITCODE -ne 0) {
  throw "certutil.exe failed with exit code $LASTEXITCODE."
}

$infPath = Join-Path $extractDir "vbMmeCable64_win7.inf"
Invoke-Checked $devcon "install" $infPath $hardwareId
