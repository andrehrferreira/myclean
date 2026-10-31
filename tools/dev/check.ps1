#Requires -Version 7.2
<#
.SYNOPSIS
    Local quality gate: build (type-check), format-check, lint and the full test suite.

.DESCRIPTION
    Run by .githooks/pre-commit and usable by hand. Enters the x64 MSVC developer environment when
    cl.exe is not on PATH, configures the preset when its build tree does not exist yet, and stops
    at the first failing step with a non-zero exit code. See docs/development.md.

.EXAMPLE
    pwsh -File tools\dev\check.ps1
.EXAMPLE
    pwsh -File tools\dev\check.ps1 -Preset release
#>
param([string]$Preset = 'debug')

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

function Initialize-DevShell {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'MSVC not found: install Visual Studio with "Desktop development with C++", or run from a Developer PowerShell.'
    }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio installation with the x64 C++ tools was found.' }
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

function Invoke-Step([string]$Name, [string]$FilePath, [string[]]$ArgumentList) {
    Write-Host "== $Name" -ForegroundColor Cyan
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        Write-Host "== $Name failed (exit code $LASTEXITCODE)" -ForegroundColor Red
        exit 1
    }
}

Push-Location $RepoRoot
try {
    Initialize-DevShell
    if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot "build\$Preset\CMakeCache.txt"))) {
        Invoke-Step 'Configure' cmake @('--preset', $Preset)
    }
    Invoke-Step 'Build' cmake @('--build', '--preset', $Preset)
    Invoke-Step 'Format check' cmake @('--build', '--preset', $Preset, '--target', 'format-check')
    Invoke-Step 'Lint' cmake @('--build', '--preset', $Preset, '--target', 'lint')
    Invoke-Step 'Tests' ctest @('--preset', $Preset)
    Write-Host '== All checks passed' -ForegroundColor Green
} finally {
    Pop-Location
}
