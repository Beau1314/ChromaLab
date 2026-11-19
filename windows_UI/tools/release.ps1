<#
.SYNOPSIS
Packages and publishes a ChromaLab release.

.DESCRIPTION
The self-update channel's publishing end. Builds the app, refreshes the
deployed directory (windeployqt), zips it as one top-level folder, and
creates the GitHub Release the installed copies check (Updater talks to
repos/Beau1314/ChromaLab/releases/latest).

The upload token comes from the git credential store (`git credential fill`
for github.com) — the same credential that pushes — so no separate login is
required. Bump the version in the repo-root VERSION file first; the release
tag is read from it.

.PARAMETER Version
Override the version (default: the VERSION file).

.PARAMETER Notes
The release notes body (plain text). Consider -NotesFile for longer notes.

.PARAMETER NotesFile
A text file whose content becomes the release notes.

.PARAMETER SkipBuild
Package the current build/qt/app as-is (a hot re-publish of the same bits).

.EXAMPLE
powershell -File tools\release.ps1 -NotesFile docs\release-notes\v0.6.0.md
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$Notes,
    [string]$NotesFile,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent   # repo root (windows_UI/..)
if (-not $Version) { $Version = (Get-Content (Join-Path $repoRoot 'VERSION') -Raw).Trim() }
if (-not $Version -match '^\d+(\.\d+)*$') { throw "Version '$Version' is not dotted numbers; bump the VERSION file." }
if ($NotesFile) { $Notes = Get-Content $NotesFile -Raw }
$repo = 'Beau1314/ChromaLab'
$buildDir = Join-Path $repoRoot 'build/qt'
$appDir = Join-Path $buildDir 'app'
$deps = Join-Path (Split-Path $repoRoot -Parent) 'deps'
if (-not (Test-Path $deps)) { $deps = 'D:\spk-deps' }
$qt = Join-Path $deps 'qt-6.11.3/mingw_64'
$compiler = Join-Path $deps 'qt-6.11.3/Tools/mingw1310_64/bin'
$cmake = Join-Path $deps 'cmake-3.25.0-windows-x86_64/bin/cmake.exe'
foreach ($file in @($cmake, "$compiler/g++.exe", "$qt/bin/windeployqt.exe")) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Missing local dependency: $file" }
}

if (-not $SkipBuild) {
    Write-Host "== building v$Version" -ForegroundColor Cyan
    $env:PATH = "$compiler;$qt/bin;$env:PATH"
    & $cmake --build $buildDir --target SpektraLabQt spektralab_qt_resources
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    # Refresh the deployed runtime: a new Qt module (QtNetwork joined the
    # app in the update work) must land its DLL beside the exe.
    & "$qt/bin/windeployqt.exe" --qmldir (Join-Path $repoRoot 'windows_UI/qml') --release (Join-Path $appDir 'ChromaLab.exe')
    if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }
}

foreach ($probe in @('ChromaLab.exe','spklib-host.exe','resources','licenses')) {
    if (-not (Test-Path -LiteralPath (Join-Path $appDir $probe))) {
        throw "The app directory is missing $probe — build first (or see -SkipBuild)."
    }
}

Write-Host "== packaging" -ForegroundColor Cyan
$stage = Join-Path $env:TEMP "spk-release-$Version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$folder = "ChromaLab-Windows-v$Version-win64"
$payload = Join-Path $stage $folder
New-Item -ItemType Directory -Path $payload -Force | Out-Null
# Robocopy exit codes 0-7 are success grades; anything else stops the release.
robocopy $appDir $payload /E /XF '*.spk-old' '*.pdb' /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
# The GPL compliance set: the project's own licences and notices (LICENSE,
# NOTICE.md, SPEKTRAFILM/GUST/spklib texts) travel beside the components'
# set (licenses/), and a source-code offer names the exact tag the zip was
# built from (GPL-3.0 sections 5-6).
Copy-Item -Recurse -Force (Join-Path $repoRoot 'license') (Join-Path $payload 'license')
$sourceOffer = @"
# Source code

ChromaLab is free software under GPL-3.0-or-later. The complete source code
of this build is the tag v$Version of:

    https://github.com/Beau1314/ChromaLab/tree/v$Version

Licences and attribution: see the license/ and licenses/ folders beside this
file. ChromaLab is an unofficial port of SpektraLab; it is not affiliated
with or endorsed by the SpektraLab or spektrafilm projects.
"@
Set-Content -Path (Join-Path $payload 'SOURCE-CODE.md') -Value $sourceOffer -Encoding UTF8
$zip = Join-Path $stage "$folder.zip"
Compress-Archive -Path $payload -DestinationPath $zip
$size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "   $folder.zip ($size MB)"

Write-Host "== publishing release v$Version to $repo" -ForegroundColor Cyan
$token = "protocol=https`nhost=github.com`n`n" | git credential fill |
    Select-String '^password=' | ForEach-Object { $_.Line.Substring(9) }
if (-not $token) { throw 'No github.com credential in the git store; push once or gh auth login.' }
$headers = @{ Authorization = "Bearer $token"; Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }

$footer = "Licence: GPL-3.0-or-later (license/ and licenses/ inside the zip; full text included). Source code for this exact build: https://github.com/Beau1314/ChromaLab/tree/v$Version — ChromaLab is an unofficial port of SpektraLab (https://github.com/JamesQiu2005/SpektraLab), not affiliated with or endorsed by the SpektraLab or spektrafilm projects."

# Windows PowerShell's Invoke-RestMethod throws on 404 — the tag's absence is
# the expected first-release state, not an error.
$existing = $null
try { $existing = Invoke-RestMethod -Headers $headers "https://api.github.com/repos/$repo/releases/tags/v$Version" } catch { }
if ($existing -and $existing.tag_name) { throw "Release v$Version already exists; bump the VERSION file." }

$body = @{
    tag_name = "v$Version"
    name = "ChromaLab v$Version"
    body = if ($Notes) { "$Notes`n`n---`n$footer" } else { $footer }
    target_commitish = 'master'
} | ConvertTo-Json
# UTF-8 bytes, not the 5.1 default encoding, or the Chinese notes mojibake.
$release = Invoke-RestMethod -Method Post -Headers $headers -Uri "https://api.github.com/repos/$repo/releases" `
    -Body ([Text.Encoding]::UTF8.GetBytes($body)) -ContentType 'application/json; charset=utf-8'
Write-Host "   release #$($release.id) created"

$asset = Invoke-RestMethod -Method Post -Headers $headers `
    -Uri "https://uploads.github.com/repos/$repo/releases/$($release.id)/assets?name=$folder.zip" `
    -InFile $zip -ContentType 'application/zip'
Write-Host "   asset uploaded: $($asset.name) ($([math]::Round($asset.size/1MB,1)) MB)"
Write-Host "Done: https://github.com/$repo/releases/tag/v$Version" -ForegroundColor Green
