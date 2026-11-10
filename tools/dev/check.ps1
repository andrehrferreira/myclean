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
    # cl.exe prints its /showIncludes notes ("Observação: incluindo arquivo:" on a Portuguese
    # MSVC) in the code page of the console ninja runs in, and ninja records a header dependency
    # only when those bytes match the prefix CMake wrote into rules.ninja. CMake 4.1 writes it in
    # UTF-8 and cannot configure under an OEM console at all, so configure and build both run
    # under UTF-8. (VSLANG=1033 is no way out: this MSVC has only its 1046 resources.)
    [Console]::OutputEncoding = [Text.Encoding]::UTF8
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

# A tree configured or built under an OEM console (this script did that until 2026-11-10) holds a
# prefix that is not UTF-8, or objects compiled with no recorded header dependency, so a header
# change left stale objects behind (seen as ASan layout aborts in tests). Such a tree is cleaned
# and configured again once.
function Reset-StaleBuildTree([string]$BuildDir) {
    $rules = Join-Path $BuildDir 'CMakeFiles\rules.ninja'
    if (-not (Test-Path -LiteralPath $rules)) { return }
    $strict = [Text.UTF8Encoding]::new($false, $true)
    try {
        $null = $strict.GetString([IO.File]::ReadAllBytes($rules))
        return
    } catch [Text.DecoderFallbackException] {
        Write-Host "== $BuildDir was configured under another code page; cleaning and configuring it again." -ForegroundColor Yellow
    }
    & ninja -C $BuildDir -t clean | Out-Null
    Remove-Item -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $BuildDir 'CMakeFiles') -Recurse -Force
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
    $tree = Join-Path $RepoRoot "build\$Preset"
    Reset-StaleBuildTree $tree
    if (-not (Test-Path -LiteralPath (Join-Path $tree 'CMakeCache.txt')) -or
        -not (Test-Path -LiteralPath (Join-Path $tree 'CMakeFiles\rules.ninja'))) {
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
