# windows/test/mouse/build.ps1 — MOVED: the mouse encoder and its test are the core's (core/input/mouse.hpp,
# core/test/input). This wrapper runs that test, so the old command still works.
#   powershell -NoProfile -ExecutionPolicy Bypass -File windows/test/mouse/build.ps1 [-Regen]
param([switch]$Regen)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$a = @('-Only', 'mouse'); if ($Regen) { $a += '-Regen' }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'core\test\input\build.ps1') @a
exit $LASTEXITCODE
