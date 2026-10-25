#!/usr/bin/env pwsh
# PowerShell entry point for the cross-platform screenshot helper.
param(
    [Parameter(Mandatory = $true)][string]$SourcePath,
    [int]$SecondsTotal = 70,
    [int]$IntervalSeconds = 1,
    [string]$Machine = "next50",
    [string]$OutDir = "snapshots",
    [string]$Inspeccy = "",
    [string]$DotArg = "",
    [string]$Vfs = "",
    [string]$Rom48128 = "",
    [string]$Rom48 = "",
    [string]$Rom128 = "",
    [string]$RomPlus3 = "",
    [switch]$Force
)
$ErrorActionPreference = "Stop"
$SnapshotArgs = @(
    (Join-Path $PSScriptRoot "snapshot_periodic.py"),
    "--source", $SourcePath, "--seconds-total", $SecondsTotal,
    "--interval-seconds", $IntervalSeconds, "--machine", $Machine, "--out-dir", $OutDir
)
foreach ($Pair in @(
    @("--inspeccy", $Inspeccy), @("--dot-arg", $DotArg), @("--vfs", $Vfs),
    @("--rom48128", $Rom48128), @("--rom48", $Rom48), @("--rom128", $Rom128), @("--rom-plus3", $RomPlus3)
)) {
    if ($Pair[1]) { $SnapshotArgs += $Pair }
}
if ($Force) { $SnapshotArgs += "--force" }
& python @SnapshotArgs
exit $LASTEXITCODE
