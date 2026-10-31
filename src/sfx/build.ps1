# Builds static/sfx/stub-{x64,x86,arm64}.exe with the zig compiler.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File src/sfx/build.ps1
#
# The first run downloads zig into the temporary directory. The stubs are checked into the
# repository, so you only need to run this after changing stub.c.

$ErrorActionPreference = 'Stop'

$zigVersion = '0.17.0'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$toolsDirectory = Join-Path $env:TEMP 'turbowarp-packager-sfx-tools'
$zigDirectory = Join-Path $toolsDirectory "zig-x86_64-windows-$zigVersion"

if (-not (Test-Path (Join-Path $zigDirectory 'zig.exe'))) {
  New-Item -ItemType Directory -Force -Path $toolsDirectory | Out-Null
  $zipPath = Join-Path $toolsDirectory "zig-$zigVersion.zip"
  if (-not (Test-Path $zipPath)) {
    # ziglang.org is the official source but it can be very slow. The hexops mirror
    # (which the Mach engine project maintains) hosts the exact same files.
    $urls = @(
      "https://pkg.hexops.org/zig/$zigVersion/zig-x86_64-windows-$zigVersion.zip",
      "https://ziglang.org/download/$zigVersion/zig-x86_64-windows-$zigVersion.zip"
    )
    foreach ($url in $urls) {
      Write-Host "Downloading $url"
      curl.exe -L -sS --ssl-no-revoke -o $zipPath $url
      if ($LASTEXITCODE -eq 0 -and (Test-Path $zipPath) -and (Get-Item $zipPath).Length -gt 50MB) {
        break
      }
      Write-Host "Download failed, trying the next mirror"
      Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path $zipPath)) {
      throw "Failed to download zig"
    }
  }
  Write-Host "Extracting zig"
  tar -xf $zipPath -C $toolsDirectory
}

$zig = Join-Path $zigDirectory 'zig.exe'
$outputDirectory = Join-Path $root 'static\sfx'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

$targets = @(
  @{ Name = 'x64'; ZigTarget = 'x86_64-windows-gnu' },
  @{ Name = 'x86'; ZigTarget = 'x86-windows-gnu' },
  @{ Name = 'arm64'; ZigTarget = 'aarch64-windows-gnu' }
)

foreach ($target in $targets) {
  $output = Join-Path $outputDirectory "stub-$($target.Name).exe"
  Write-Host "Building $output ($($target.ZigTarget))"
  & $zig cc -target $target.ZigTarget -O2 -s -Wall -ffunction-sections -fdata-sections -o $output `
    (Join-Path $PSScriptRoot 'stub.c') (Join-Path $PSScriptRoot 'miniz.c') `
    '-Wl,--gc-sections' '-Wl,--subsystem,windows' -luser32 -lshell32
  if ($LASTEXITCODE -ne 0) {
    throw "zig cc failed for $($target.ZigTarget)"
  }
}

Write-Host ''
Write-Host 'SHA-256 checksums (update these in src/packager/large-assets.js):'
Get-ChildItem (Join-Path $outputDirectory 'stub-*.exe') | ForEach-Object {
  $hash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLower()
  Write-Host "  $($_.Name): '$hash'"
}
