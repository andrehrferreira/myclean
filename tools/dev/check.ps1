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
param([string]$Preset = 'debug', [switch]$InOwnConsole)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

# cl.exe prints its /showIncludes notes ("Observação: incluindo arquivo:" on a Portuguese MSVC) in
# the code page of its console, or in the OEM code page when it has none (a git hook started
# without a terminal), and ninja records a header dependency only when those bytes match the
# msvc_deps_prefix CMake wrote into rules.ninja. CMake 4.1 writes it in UTF-8, cannot configure
# under an OEM console, and corrupts it when ninja makes it regenerate under one. Builds ran with
# mismatched bytes, objects had no header dependencies, and header edits left stale objects behind
# (false ASan layout aborts in tests on 2026-11-10). So the gate always runs in a console of its
# own, without a window, set to UTF-8: configure, regenerate and build then agree wherever the
# gate is started from. (VSLANG=1033 is no way out: this MSVC has only its 1046 resources.)
if (-not $InOwnConsole) {
    $info = [Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path)
    foreach ($argument in @('-NoProfile', '-File', $PSCommandPath, '-Preset', $Preset, '-InOwnConsole')) {
        $info.ArgumentList.Add($argument)
    }
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::Start($info)
    while ($null -ne ($line = $process.StandardOutput.ReadLine())) { Write-Host $line }
    $process.WaitForExit()
    exit $process.ExitCode
}
[Console]::OutputEncoding = [Text.Encoding]::UTF8

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

# A tree whose rules.ninja does not hold the UTF-8 prefix cl.exe prints here (configured, or
# regenerated, under another code page) is cleaned and configured again, once.
function Reset-MismatchedBuildTree([string]$BuildDir) {
    $rules = Join-Path $BuildDir 'CMakeFiles\rules.ninja'
    if (-not (Test-Path -LiteralPath $rules)) { return }
    $dir = Join-Path ([IO.Path]::GetTempPath()) "myclean-showincludes-$PID"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    try {
        [IO.File]::WriteAllText((Join-Path $dir 'probe.h'), "`n")
        [IO.File]::WriteAllText((Join-Path $dir 'probe.cpp'), "#include `"probe.h`"`n")
        Push-Location $dir
        try { $notes = & cl.exe /nologo /showIncludes /Zs probe.cpp 2>&1 } finally { Pop-Location }
    } finally {
        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    }
    $note = $notes | Where-Object { "$_" -like '*probe.h' } | Select-Object -First 1
    if (-not $note) { throw 'cl.exe printed no /showIncludes note.' }
    $prefix = ("$note" -replace '\s*[A-Za-z]:\\.*$', '')
    $recorded = [regex]::Match([IO.File]::ReadAllText($rules), '(?m)^\s*msvc_deps_prefix = ([^\r\n]*?)\s*$')
    if ($recorded.Success -and $recorded.Groups[1].Value -eq $prefix) { return }
    Write-Host "== $BuildDir expects another showIncludes prefix than cl.exe prints; cleaning and configuring it again." -ForegroundColor Yellow
    & ninja -C $BuildDir -t clean | Out-Null
    Remove-Item -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $BuildDir 'CMakeFiles') -Recurse -Force
}

function Invoke-Step([string]$Name, [string]$FilePath, [string[]]$ArgumentList) {
    Write-Host "== $Name" -ForegroundColor Cyan
    & $FilePath @ArgumentList 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Host "== $Name failed (exit code $LASTEXITCODE)" -ForegroundColor Red
        exit 1
    }
}

Push-Location $RepoRoot
try {
    Initialize-DevShell
    $tree = Join-Path $RepoRoot "build\$Preset"
    Reset-MismatchedBuildTree $tree
    if (-not (Test-Path -LiteralPath (Join-Path $tree 'CMakeCache.txt'))) {
        Invoke-Step 'Configure' cmake @('--preset', $Preset)
    }
    Invoke-Step 'Build' cmake @('--build', '--preset', $Preset)
    Invoke-Step 'Format check' cmake @('--build', '--preset', $Preset, '--target', 'format-check')
    Invoke-Step 'Lint' cmake @('--build', '--preset', $Preset, '--target', 'lint')
    Invoke-Step 'Tests' ctest @('--preset', $Preset)
    Write-Host '== All checks passed' -ForegroundColor Green
} catch {
    Write-Host "== $($_.Exception.Message)" -ForegroundColor Red
    exit 1
} finally {
    Pop-Location
}
