# Cuts a release: tag, build, verify the image reports the version, push, GitHub release, publish the firmware.
#
#     .\tools\release.ps1 -Version v1.3.1 -Title "Ask which version she is running" -NotesFile notes.md
#     .\tools\release.ps1 -Version v1.3.1 -Title "..." -NotesFile notes.md -DryRun
#
# Everything must already be committed (the version string comes from `git describe`, so a dirty tree would
# make the image report something else). The order is chosen so nothing public happens until the build is
# known good, and the firmware is published last, once the tag and release exist:
#   1 tag locally   2 reconfigure + build   3 check the image says $Version   4 push master and the tag
#   5 GitHub release   6 publish bookbook.bin / bookbook.json to Azure
# -DryRun stops after step 3 and removes the local tag again: nothing is pushed, released or published.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$NotesFile,
    [switch]$DryRun
)
$ErrorActionPreference = "Continue"  # native tools (idf.py, git) write progress to stderr; exit codes are checked explicitly
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Fail($msg) { Write-Host "release stopped: $msg" -ForegroundColor Red; exit 1 }
function Step($n, $msg) { Write-Host "`n[$n] $msg" -ForegroundColor Cyan }

if ($Version -notmatch '^v\d+\.\d+\.\d+$') { Fail "version must look like v1.3.1" }
if (-not (Test-Path $NotesFile)) { Fail "notes file not found: $NotesFile" }
if ((git status --porcelain) -ne $null -and (git status --porcelain).Length -gt 0) { Fail "uncommitted changes: commit them first (the image's version string comes from git)" }
if ((git rev-parse --abbrev-ref HEAD) -ne "master") { Fail "not on master" }
if (git tag -l $Version) { Fail "tag $Version already exists" }
git fetch --tags --quiet
if (git tag -l $Version) { Fail "tag $Version already exists on GitHub" }

Step 1 "tag $Version on $(git rev-parse --short HEAD)"
git tag $Version
$tagged = $true
try {
    Step 2 "reconfigure and build"
    & "$PSScriptRoot\idf.ps1" reconfigure | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "reconfigure failed" }
    & "$PSScriptRoot\idf.ps1" build | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { throw "build failed" }

    Step 3 "check the image reports $Version"
    $line = python "$PSScriptRoot\publish_firmware.py" --dry-run | Select-Object -First 1
    Write-Host $line
    if ($line -notmatch "^Image: $([regex]::Escape($Version)) ") { throw "the image does not report $Version (it says: $line)" }
} catch {
    git tag -d $Version | Out-Null
    Fail $_.Exception.Message
}

if ($DryRun) {
    git tag -d $Version | Out-Null
    Write-Host "`nDry run finished: the build is good and reports $Version. Nothing pushed, released or published; local tag removed." -ForegroundColor Green
    exit 0
}

Step 4 "push master and the tag"
git push origin master
if ($LASTEXITCODE -ne 0) { Fail "push failed (the local tag $Version is still there)" }
git push origin $Version
if ($LASTEXITCODE -ne 0) { Fail "pushing the tag failed" }

Step 5 "GitHub release"
gh release create $Version --verify-tag --title "Librarian ${Version}: $Title" --notes-file $NotesFile
if ($LASTEXITCODE -ne 0) { Fail "creating the release failed (the tag is pushed; firmware NOT published)" }

Step 6 "publish the firmware"
python "$PSScriptRoot\publish_firmware.py"
if ($LASTEXITCODE -ne 0) { Fail "publishing the firmware failed (release and tag exist; run tools\publish_firmware.py to retry)" }

Write-Host "`nReleased $Version. Ask a unit 'is there an update?' to see it." -ForegroundColor Green
