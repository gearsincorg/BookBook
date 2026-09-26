# Runs idf.py with this project's ESP-IDF 5.5, whatever IDF_PATH / IDF_TOOLS_PATH the shell has.
#
#     .\tools\idf.ps1 build
#     .\tools\idf.ps1 -p COM5 flash monitor
#
# The IDF 5.5 source is C:\Users\13015\esp\v5.5\esp-idf and its tools live in D:\Users\Phil\esp\.espressif
# (the IDF 4.2 and 6.0 environments there are not used). Edit the two paths below if they move.
$env:IDF_PATH = "C:\Users\13015\esp\v5.5\esp-idf"
$env:IDF_TOOLS_PATH = "D:\Users\Phil\esp\.espressif"

# export.ps1 puts the 5.5 toolchain and idf.py on PATH for this process only.
. "$env:IDF_PATH\export.ps1" *> $null
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) { throw "ESP-IDF 5.5 did not activate: run install.ps1 in $env:IDF_PATH" }

Set-Location (Split-Path -Parent $PSScriptRoot)
idf.py @args
exit $LASTEXITCODE
