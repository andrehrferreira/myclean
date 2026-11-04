#Requires -Version 7.2
<#
.SYNOPSIS
    Build, test and hot-deploy MyClean on this PC (developer install, no installer).

.DESCRIPTION
    Default run: configure + build + test with the CMake preset. Only when all of
    that is green AND the staged binaries differ from the installed ones, the new
    build is copied to a new versioned folder under %ProgramFiles%\MyClean, the
    service is restarted on it, the tray app is restarted, and both are
    health-checked. Any failure rolls back to the previous version. A run with no
    changes does nothing.

    The privileged half (service, Program Files) runs in an elevated copy of this
    script, so a deploy shows one UAC prompt. The tray app is always started from
    the non-elevated half, with the user's own token.

    Layout and the contracts the binaries must honor: docs/deploy.md.

.EXAMPLE
    pwsh -File tools\deploy\deploy.ps1                 # build + test + deploy if changed
.EXAMPLE
    pwsh -File tools\deploy\deploy.ps1 -Watch          # redeploy after every good change
.EXAMPLE
    pwsh -File tools\deploy\deploy.ps1 -Status
.EXAMPLE
    pwsh -File tools\deploy\deploy.ps1 -Rollback
.EXAMPLE
    pwsh -File tools\deploy\deploy.ps1 -Uninstall [-Purge]
#>
[CmdletBinding(DefaultParameterSetName = 'Deploy')]
param(
    [Parameter(ParameterSetName = 'Deploy')][switch]$Watch,
    [Parameter(ParameterSetName = 'Deploy')][switch]$Force,
    [Parameter(ParameterSetName = 'Deploy')][string]$Preset = 'release',
    [Parameter(ParameterSetName = 'Deploy')][int]$QuietSeconds = 10,

    [Parameter(Mandatory, ParameterSetName = 'Status')][switch]$Status,
    [Parameter(Mandatory, ParameterSetName = 'Rollback')][switch]$Rollback,
    [Parameter(Mandatory, ParameterSetName = 'Uninstall')][switch]$Uninstall,
    [Parameter(ParameterSetName = 'Uninstall')][switch]$Purge,

    # Internal: the elevated half. Started by this script, not by hand.
    [Parameter(Mandatory, ParameterSetName = 'Privileged')]
    [ValidateSet('activate', 'uninstall', 'purge')][string]$Privileged,
    [Parameter(ParameterSetName = 'Privileged')][string]$Target,
    [Parameter(Mandatory, ParameterSetName = 'Privileged')][string]$ResultFile,
    [Parameter(ParameterSetName = 'Privileged')][string]$OwnerSid,

    [int]$KeepVersions = 5,
    [int]$HealthSeconds = 5
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$ServiceName    = 'MyClean'
$SvcExe         = 'myclean-svc.exe'
$TrayExe        = 'myclean-tray.exe'
$CliExe         = 'myclean.exe'
$TrayQuitEvent  = 'Local\MyClean.Tray.Quit'
$RunKey         = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$RunValue       = 'MyCleanTray'
# ProgramW6432 is the real 64-bit Program Files even when this runs in a 32-bit pwsh.
$InstallRoot    = Join-Path ($env:ProgramW6432 ? $env:ProgramW6432 : $env:ProgramFiles) 'MyClean'
$VersionsDir    = Join-Path $InstallRoot 'versions'
$CurrentLink    = Join-Path $InstallRoot 'current'
$DeployDataDir  = Join-Path $env:ProgramData 'MyClean\deploy'
$LogFile        = Join-Path $DeployDataDir 'deploy.log'
$HistoryFile    = Join-Path $DeployDataDir 'history.jsonl'
$RepoRoot       = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$StageDir       = Join-Path $RepoRoot 'out\stage'
$WatchDirs      = @('src', 'data', 'tests', 'cmake')
$WatchRootFiles = @('CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json')

# ---------------------------------------------------------------- helpers

function Write-Step([string]$Message) { Write-Host "[$(Get-Date -Format HH:mm:ss)] $Message" -ForegroundColor Cyan }
function Write-Ok([string]$Message)   { Write-Host "[$(Get-Date -Format HH:mm:ss)] $Message" -ForegroundColor Green }
function Write-Fail([string]$Message) { Write-Host "[$(Get-Date -Format HH:mm:ss)] $Message" -ForegroundColor Red }

function Write-Log([string]$Message) {
    New-Item -ItemType Directory -Force -Path $DeployDataDir | Out-Null
    Add-Content -LiteralPath $LogFile -Value ('{0:u} {1}' -f (Get-Date), $Message)
}

function Invoke-Native([string]$FilePath, [string[]]$ArgumentList = @()) {
    & $FilePath @ArgumentList | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$FilePath $($ArgumentList -join ' ') failed with exit code $LASTEXITCODE" }
}

function Test-IsAdmin {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-NormalizedPath([string]$Path) { [IO.Path]::GetFullPath($Path).TrimEnd('\') }

function Get-CurrentVersionDir {
    $item = Get-Item -LiteralPath $CurrentLink -Force -ErrorAction SilentlyContinue
    if (-not $item) { return $null }
    if ($item.LinkType -ne 'Junction') { throw "$CurrentLink exists but is not a junction; refusing to touch it." }
    $linkTarget = $item.Target | Select-Object -First 1
    if (-not $linkTarget) { throw "$CurrentLink is a junction without a target." }
    if ($linkTarget.StartsWith('\??\')) { $linkTarget = $linkTarget.Substring(4) }
    Get-NormalizedPath $linkTarget
}

function Read-Manifest([string]$Dir) {
    $path = Join-Path $Dir 'manifest.json'
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}

# Hash of every deployable file (path + content), excluding the manifest itself.
function Get-ContentHash([string]$Dir) {
    $root = (Get-NormalizedPath $Dir) + '\'
    $entries = foreach ($file in Get-ChildItem -LiteralPath $Dir -Recurse -File) {
        $relative = $file.FullName.Substring($root.Length)
        if ($relative -eq 'manifest.json') { continue }
        '{0}:{1}' -f $relative.ToLowerInvariant(), (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    $sorted = [string[]]@($entries)
    [Array]::Sort($sorted, [StringComparer]::Ordinal)
    $bytes = [Text.Encoding]::UTF8.GetBytes($sorted -join "`n")
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
}

# ---------------------------------------------------------------- build side (non-elevated)

function Initialize-DevShell {
    # MSVC writes its /showIncludes notes in the console's output code page, and ninja records a
    # header dependency only when the note starts with the prefix CMake detected in the OEM code
    # page ("Observação: incluindo arquivo:" on a Portuguese MSVC). Under a UTF-8 console every object
    # was left with no dependencies, so header changes did not rebuild it.
    [Console]::OutputEncoding = [Text.Encoding]::GetEncoding(
        [Globalization.CultureInfo]::CurrentCulture.TextInfo.OEMCodePage)
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

function Get-BinaryDir {
    $presets = Get-Content -LiteralPath (Join-Path $RepoRoot 'CMakePresets.json') -Raw | ConvertFrom-Json
    $configure = if ($presets.PSObject.Properties['configurePresets']) {
        $presets.configurePresets | Where-Object { $_.name -eq $Preset } | Select-Object -First 1
    }
    if (-not $configure) { throw "Configure preset '$Preset' not found in CMakePresets.json." }
    $dir = if ($configure.PSObject.Properties['binaryDir']) { $configure.binaryDir } else { '${sourceDir}/build/${presetName}' }
    $dir = $dir.Replace('${sourceDir}', $RepoRoot).Replace('${presetName}', $Preset)
    if ($dir -match '\$\{') { throw "binaryDir '$dir' uses CMake macros this script does not expand." }
    if (-not [IO.Path]::IsPathRooted($dir)) { $dir = Join-Path $RepoRoot $dir }
    Get-NormalizedPath $dir
}

# The quality gate: a deploy only happens if all three steps succeed.
function Invoke-Gate {
    Write-Step "Configure (preset '$Preset')"
    Invoke-Native cmake @('--preset', $Preset)
    Write-Step 'Build'
    Invoke-Native cmake @('--build', '--preset', $Preset)
    Write-Step 'Test'
    Invoke-Native ctest @('--preset', $Preset, '--output-on-failure')
}

function New-Stage {
    if (Test-Path -LiteralPath $StageDir) { Remove-Item -LiteralPath $StageDir -Recurse -Force }
    Invoke-Native cmake @('--install', (Get-BinaryDir), '--prefix', $StageDir)
    $bin = Join-Path $StageDir 'bin'
    $found = @($SvcExe, $TrayExe, $CliExe | Where-Object { Test-Path -LiteralPath (Join-Path $bin $_) })
    if ($found.Count -eq 0) { throw "cmake --install produced no MyClean executable in $bin." }
    $found
}

function Get-GitInfo {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { return $null }
    if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot '.git'))) { return $null }
    $commit = git -C $RepoRoot rev-parse --short HEAD 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    [ordered]@{ commit = $commit; dirty = [bool](git -C $RepoRoot status --porcelain) }
}

# ---------------------------------------------------------------- tray (non-elevated)

function Stop-Tray {
    $session = (Get-Process -Id $PID).SessionId
    $procs = @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($TrayExe)) -ErrorAction SilentlyContinue |
               Where-Object { $_.SessionId -eq $session })
    if ($procs.Count -eq 0) { return }
    Write-Step 'Stopping tray'
    $signal = $null
    if ([Threading.EventWaitHandle]::TryOpenExisting($TrayQuitEvent, [ref]$signal)) {
        [void]$signal.Set()
        $signal.Dispose()
    }
    foreach ($proc in $procs) {
        if (-not $proc.WaitForExit(10000)) {
            Write-Warning "Tray (pid $($proc.Id)) did not quit within 10 s; killing it."
            $proc.Kill()
            $proc.WaitForExit()
        }
    }
}

function Start-Tray {
    $exe = Join-Path $CurrentLink "bin\$TrayExe"
    if (-not (Test-Path -LiteralPath $exe)) { return $null }
    if (Test-IsAdmin) {
        Write-Warning 'This shell is elevated, so the tray is not started (it must run with your normal token). It starts at next logon, or run the script from a normal shell.'
        return $null
    }
    Write-Step 'Starting tray'
    $proc = Start-Process -FilePath $exe -PassThru
    $null = $proc.Handle   # keep the handle so ExitCode is readable later
    $proc
}

function Set-TrayAutostart {
    $exe = Join-Path $CurrentLink "bin\$TrayExe"
    if (Test-Path -LiteralPath $exe) {
        Set-ItemProperty -Path $RunKey -Name $RunValue -Value ('"{0}"' -f $exe)
    } elseif (Get-ItemProperty -Path $RunKey -Name $RunValue -ErrorAction SilentlyContinue) {
        Remove-ItemProperty -Path $RunKey -Name $RunValue
    }
}

# Returns $null when healthy, otherwise a description of the problem.
function Test-UserSide($TrayProcess) {
    if ($TrayProcess) {
        Start-Sleep -Seconds $HealthSeconds
        if ($TrayProcess.HasExited) { return "tray exited with code $($TrayProcess.ExitCode) within $HealthSeconds s" }
    }
    $cli = Join-Path $CurrentLink "bin\$CliExe"
    if (Test-Path -LiteralPath $cli) {
        $out = Join-Path ([IO.Path]::GetTempPath()) 'myclean-deploy-status.json'
        $proc = Start-Process -FilePath $cli -ArgumentList 'status', '--json' -NoNewWindow -PassThru -RedirectStandardOutput $out
        $null = $proc.Handle
        if (-not $proc.WaitForExit(20000)) { $proc.Kill(); return "'myclean status --json' timed out after 20 s" }
        if ($proc.ExitCode -ne 0) { return "'myclean status --json' exited with $($proc.ExitCode): $(Get-Content -LiteralPath $out -Raw)" }
    }
    $null
}

# Runs this script elevated for the privileged half and returns its result object.
function Invoke-Privileged([string]$Action, [string]$TargetPath) {
    $resultPath = Join-Path ([IO.Path]::GetTempPath()) ('myclean-deploy-{0}.json' -f [guid]::NewGuid())
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
                 '-Privileged', $Action, '-ResultFile', "`"$resultPath`"",
                 '-KeepVersions', $KeepVersions, '-HealthSeconds', $HealthSeconds,
                 '-OwnerSid', [Security.Principal.WindowsIdentity]::GetCurrent().User.Value)
    if ($TargetPath) { $argList += @('-Target', "`"$TargetPath`"") }
    Write-Step "Elevating for '$Action' (UAC prompt)"
    try {
        $proc = Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList ($argList -join ' ') `
                              -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    } catch {
        throw "Elevation was refused or failed: $($_.Exception.Message)"
    }
    if (-not (Test-Path -LiteralPath $resultPath)) {
        throw "The elevated step exited with code $($proc.ExitCode) and wrote no result; see $LogFile."
    }
    try { Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json }
    finally { Remove-Item -LiteralPath $resultPath -Force }
}

# ---------------------------------------------------------------- privileged side (elevated)

function Get-ServiceProcessId {
    (Get-CimInstance Win32_Service -Filter "Name='$ServiceName'").ProcessId
}

function Stop-MyCleanService {
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if (-not $svc -or $svc.Status -eq 'Stopped') { return }
    Write-Log 'Stopping service'
    $svcPid = Get-ServiceProcessId
    try { $svc.Stop() } catch { Write-Log "Stop request failed: $($_.Exception.Message)" }
    try {
        $svc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    } catch {
        if ($svcPid) {
            Write-Log "Service did not stop within 30 s; killing pid $svcPid"
            Stop-Process -Id $svcPid -Force -ErrorAction SilentlyContinue
        }
        $svc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(10))
    }
}

function Register-MyCleanService {
    $binPath = '"{0}"' -f (Join-Path $CurrentLink "bin\$SvcExe")
    $existing = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'"
    if (-not $existing) {
        Write-Log "Registering service $ServiceName"
        New-Service -Name $ServiceName -BinaryPathName $binPath -DisplayName 'MyClean' `
                    -Description 'MyClean monitoring, resource governor and maintenance service' `
                    -StartupType Automatic | Out-Null
        Invoke-Native sc.exe @('failure', $ServiceName, 'reset=', '86400', 'actions=', 'restart/5000/restart/5000/restart/30000')
        # Also restart when the service stops itself with an error (a failed start, contract C7).
        Invoke-Native sc.exe @('failureflag', $ServiceName, '1')
    } elseif ($existing.PathName -ne $binPath) {
        Write-Log "Updating service path to $binPath"
        $change = Invoke-CimMethod -InputObject $existing -MethodName Change -Arguments @{ PathName = $binPath }
        if ($change.ReturnValue -ne 0) { throw "Win32_Service.Change failed with code $($change.ReturnValue)." }
    }
}

function Start-MyCleanService {
    $exe = Join-Path (Get-CurrentVersionDir) "bin\$SvcExe"
    if (-not (Test-Path -LiteralPath $exe)) {
        if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
            Write-Log 'This version has no service binary; the service stays stopped.'
        }
        return
    }
    Register-MyCleanService
    $svc = Get-Service -Name $ServiceName
    # The SCM may have restarted a killed instance on its own; stop it cleanly first.
    if ($svc.Status -ne 'Stopped') { Stop-MyCleanService; $svc.Refresh() }
    Write-Log 'Starting service'
    $svc.Start()
    $svc.WaitForStatus('Running', [TimeSpan]::FromSeconds(20))
    $startedPid = Get-ServiceProcessId
    Start-Sleep -Seconds $HealthSeconds
    $svc.Refresh()
    if ($svc.Status -ne 'Running') { throw "service is '$($svc.Status)' $HealthSeconds s after start" }
    if ((Get-ServiceProcessId) -ne $startedPid) { throw "service crashed and was restarted by the SCM within $HealthSeconds s" }
}

# The service trusts %ProgramData%\MyClean\owner.sid only when SYSTEM or Administrators own it,
# which is the case for a file created here, in the elevated half. Never overwritten.
function Set-OwnerSid {
    if (-not $OwnerSid) { return }
    $stateDir = Join-Path $env:ProgramData 'MyClean'
    $file = Join-Path $stateDir 'owner.sid'
    if (Test-Path -LiteralPath $file) { return }
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
    [IO.File]::WriteAllText($file, "$OwnerSid`n")
    Write-Log "Recorded owner SID $OwnerSid"
}

function Set-CurrentLink([string]$VersionDir) {
    $item = Get-Item -LiteralPath $CurrentLink -Force -ErrorAction SilentlyContinue
    if ($item) {
        if ($item.LinkType -ne 'Junction') { throw "$CurrentLink is not a junction; refusing to remove it." }
        [IO.Directory]::Delete($CurrentLink)   # removes the link only, never the target
    }
    New-Item -ItemType Junction -Path $CurrentLink -Target $VersionDir | Out-Null
}

function Add-History([string]$Version, [string]$State) {
    New-Item -ItemType Directory -Force -Path $DeployDataDir | Out-Null
    $line = [ordered]@{ time = (Get-Date).ToString('o'); version = $Version; state = $State } | ConvertTo-Json -Compress
    Add-Content -LiteralPath $HistoryFile -Value $line
}

function Remove-OldVersions([int]$Keep, [string[]]$Protect) {
    $dirs = @(Get-ChildItem -LiteralPath $VersionsDir -Directory | Sort-Object Name -Descending)
    foreach ($dir in ($dirs | Select-Object -Skip $Keep)) {
        if ($Protect -contains (Get-NormalizedPath $dir.FullName)) { continue }
        try {
            Remove-Item -LiteralPath $dir.FullName -Recurse -Force
            Write-Log "Pruned $($dir.Name)"
        } catch {
            Write-Log "Could not prune $($dir.Name) (still in use?): $($_.Exception.Message)"
        }
    }
}

function Invoke-Activate([string]$Source, $Result) {
    New-Item -ItemType Directory -Force -Path $VersionsDir | Out-Null
    $previous = Get-CurrentVersionDir
    $Result.previous = $previous
    $source = Get-NormalizedPath $Source

    if ($source.StartsWith($VersionsDir + '\', [StringComparison]::OrdinalIgnoreCase)) {
        $versionDir = $source                                    # rollback to an installed version
    } else {
        $manifest = Read-Manifest $source
        if (-not $manifest) { throw "No manifest.json in $source." }
        $versionDir = Join-Path $VersionsDir $manifest.version
        if (Test-Path -LiteralPath $versionDir) { throw "Version folder already exists: $versionDir." }
        Write-Log "Copying $source -> $versionDir"
        Copy-Item -LiteralPath $source -Destination $versionDir -Recurse
    }
    if (-not (Read-Manifest $versionDir)) { throw "$versionDir is not a MyClean version folder." }

    $Result.version = Split-Path $versionDir -Leaf
    Write-Log "Activating $($Result.version) (previous: $previous)"
    Set-OwnerSid
    Stop-MyCleanService
    Set-CurrentLink $versionDir
    try {
        Start-MyCleanService
    } catch {
        $Result.error = "service failed on $($Result.version): $($_.Exception.Message)"
        Write-Log $Result.error
        Add-History $Result.version 'failed'
        if ($previous -and $previous -ne $versionDir) {
            Stop-MyCleanService
            Set-CurrentLink $previous
            try {
                Start-MyCleanService
                $Result.rolledBack = $true
                Write-Log "Rolled back to $previous"
            } catch {
                $Result.error += " | rollback to $previous also failed: $($_.Exception.Message)"
                Write-Log $Result.error
            }
        }
        return
    }
    Add-History $Result.version 'active'
    Remove-OldVersions -Keep $KeepVersions -Protect @($versionDir, $previous)
    $Result.ok = $true
}

function Invoke-UninstallPrivileged([switch]$PurgeAll) {
    Stop-MyCleanService
    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        Invoke-Native sc.exe @('delete', $ServiceName)
        Write-Log 'Service deleted'
    }
    $item = Get-Item -LiteralPath $CurrentLink -Force -ErrorAction SilentlyContinue
    if ($item) {
        if ($item.LinkType -ne 'Junction') { throw "$CurrentLink is not a junction; refusing to remove it." }
        [IO.Directory]::Delete($CurrentLink)
        Write-Log 'Removed current link'
    }
    if ($PurgeAll) {
        if (Test-Path -LiteralPath $InstallRoot) { Remove-Item -LiteralPath $InstallRoot -Recurse -Force }
        $data = Join-Path $env:ProgramData 'MyClean'
        if (Test-Path -LiteralPath $data) { Remove-Item -LiteralPath $data -Recurse -Force }
    }
}

function Invoke-PrivilegedMain {
    $result = [ordered]@{ ok = $false; version = $null; previous = $null; rolledBack = $false; error = $null }
    try {
        if (-not (Test-IsAdmin)) { throw 'The privileged phase must run elevated.' }
        switch ($Privileged) {
            'activate'  { Invoke-Activate -Source $Target -Result $result }
            'uninstall' { Invoke-UninstallPrivileged; $result.ok = $true }
            'purge'     { Invoke-UninstallPrivileged -PurgeAll; $result.ok = $true }
        }
    } catch {
        $result.error = $_.Exception.Message
        if ($Privileged -ne 'purge') { Write-Log "ERROR: $($result.error)" }
    } finally {
        $result | ConvertTo-Json | Set-Content -LiteralPath $ResultFile -Encoding utf8
    }
}

# ---------------------------------------------------------------- commands (non-elevated)

function Invoke-DeployOnce {
    $mutex = [Threading.Mutex]::new($false, 'Local\MyClean.Deploy')
    try { $acquired = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $acquired = $true }
    if (-not $acquired) { $mutex.Dispose(); throw 'Another deploy is already running.' }
    Push-Location $RepoRoot
    try {
        if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot 'CMakePresets.json'))) {
            throw 'No CMakePresets.json yet: there is nothing to build and deploy.'
        }
        Initialize-DevShell
        Invoke-Gate

        Write-Step 'Staging'
        $components = New-Stage
        $hash = Get-ContentHash $StageDir
        $currentDir = Get-CurrentVersionDir
        $current = if ($currentDir) { Read-Manifest $currentDir } else { $null }
        if ($current -and $current.hash -eq $hash -and -not $Force) {
            Write-Ok "Up to date ($($current.version)); nothing to deploy."
            return
        }

        $git = Get-GitInfo
        $version = '{0:yyyyMMdd-HHmmss}-{1}' -f (Get-Date), $hash.Substring(0, 8).ToLowerInvariant()
        [ordered]@{
            version    = $version
            hash       = $hash
            builtAt    = (Get-Date).ToString('o')
            preset     = $Preset
            git        = $git
            components = $components
        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $StageDir 'manifest.json') -Encoding utf8

        Stop-Tray
        try {
            $activation = Invoke-Privileged 'activate' $StageDir
        } catch {
            $null = Start-Tray   # elevation refused: nothing changed, the installed tray comes back
            throw
        }
        if (-not $activation.ok) {
            $null = Start-Tray   # the previous tray after a rollback, else whatever is installed
            $note = if ($activation.rolledBack) { " (rolled back to $(Split-Path $activation.previous -Leaf))" } else { '' }
            throw "Deploy of $version failed: $($activation.error)$note"
        }

        Set-TrayAutostart
        $problem = Test-UserSide (Start-Tray)
        if ($problem) {
            if (-not $activation.previous) { throw "Deploy of $version failed: $problem (no previous version to roll back to)" }
            Write-Warning "$problem; rolling back"
            Stop-Tray
            $back = Invoke-Privileged 'activate' $activation.previous
            Set-TrayAutostart
            $null = Start-Tray
            $note = if ($back.ok) { "rolled back to $(Split-Path $activation.previous -Leaf)" } else { "rollback ALSO failed: $($back.error)" }
            throw "Deploy of $version failed: $problem ($note)"
        }

        $gitNote = if ($git) { " from $($git.commit)$(if ($git.dirty) { ' (uncommitted changes)' })" } else { '' }
        Write-Ok "Deployed $version$gitNote [$($components -join ', ')]"
    } finally {
        Pop-Location
        $mutex.ReleaseMutex()
        $mutex.Dispose()
    }
}

function Test-RelevantPath([string]$Path) {
    $relative = [IO.Path]::GetRelativePath($RepoRoot, $Path)
    $first = ($relative -split '[\\/]', 2)[0]
    ($WatchDirs -contains $first) -or ($WatchRootFiles -contains $relative)
}

function Start-Watch {
    $watcher = [IO.FileSystemWatcher]::new($RepoRoot)
    $watcher.IncludeSubdirectories = $true
    $watcher.NotifyFilter = [IO.NotifyFilters]'FileName, DirectoryName, LastWrite'
    $ids = foreach ($eventName in 'Changed', 'Created', 'Deleted', 'Renamed') {
        $id = "MyCleanWatch.$eventName"
        Register-ObjectEvent -InputObject $watcher -EventName $eventName -SourceIdentifier $id | Out-Null
        $id
    }
    $watcher.EnableRaisingEvents = $true
    Write-Step "Watching $RepoRoot ($($WatchDirs -join ', '), CMake files). Ctrl+C stops."
    try {
        try { Invoke-DeployOnce } catch { Write-Fail $_.Exception.Message }
        while ($true) {
            $null = Wait-Event
            $events = @(Get-Event)
            $relevant = @($events | Where-Object { Test-RelevantPath $_.SourceEventArgs.FullPath })
            $events | Remove-Event
            if ($relevant.Count -eq 0) { continue }
            Write-Step "Change detected; waiting $QuietSeconds s for edits to settle"
            while (Wait-Event -Timeout $QuietSeconds) { Get-Event | Remove-Event }
            try { Invoke-DeployOnce } catch { Write-Fail $_.Exception.Message }
        }
    } finally {
        $watcher.EnableRaisingEvents = $false
        foreach ($id in $ids) { Unregister-Event -SourceIdentifier $id -ErrorAction SilentlyContinue }
        $watcher.Dispose()
    }
}

function Show-Status {
    $currentDir = Get-CurrentVersionDir
    $manifest = if ($currentDir) { Read-Manifest $currentDir } else { $null }
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    $session = (Get-Process -Id $PID).SessionId
    $tray = @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($TrayExe)) -ErrorAction SilentlyContinue |
              Where-Object { $_.SessionId -eq $session })

    Write-Host "Install root : $InstallRoot"
    Write-Host "Version      : $(if ($manifest) { $manifest.version } else { 'not installed' })"
    if ($manifest -and $manifest.git) {
        Write-Host "Commit       : $($manifest.git.commit)$(if ($manifest.git.dirty) { ' (dirty)' })"
    }
    Write-Host "Service      : $(if ($svc) { $svc.Status } else { 'not registered' })"
    Write-Host "Tray         : $(if ($tray.Count) { "running (pid $($tray.Id -join ', '))" } else { 'not running' })"
    if (Test-Path -LiteralPath $VersionsDir) {
        Write-Host 'Versions     :'
        foreach ($dir in Get-ChildItem -LiteralPath $VersionsDir -Directory | Sort-Object Name -Descending) {
            $mark = if ($currentDir -and (Get-NormalizedPath $dir.FullName) -eq $currentDir) { '*' } else { ' ' }
            Write-Host "  $mark $($dir.Name)"
        }
    }
    if (Test-Path -LiteralPath $HistoryFile) {
        Write-Host 'History      :'
        Get-Content -LiteralPath $HistoryFile -Tail 10 | ForEach-Object { Write-Host "  $_" }
    }
}

function Invoke-Rollback {
    $currentDir = Get-CurrentVersionDir
    if (-not $currentDir) { throw 'Nothing is installed.' }
    $currentName = Split-Path $currentDir -Leaf
    $older = Get-ChildItem -LiteralPath $VersionsDir -Directory |
             Where-Object { [string]::CompareOrdinal($_.Name, $currentName) -lt 0 } |
             Sort-Object Name -Descending | Select-Object -First 1
    if (-not $older) { throw 'There is no older version to roll back to.' }
    Stop-Tray
    try {
        $result = Invoke-Privileged 'activate' $older.FullName
    } finally {
        Set-TrayAutostart
        $null = Start-Tray   # also when elevation was refused
    }
    if (-not $result.ok) { throw "Rollback failed: $($result.error)" }
    Write-Ok "Rolled back from $currentName to $($older.Name)"
}

function Invoke-Uninstall {
    if ($Purge) {
        $answer = Read-Host "This deletes $InstallRoot and $(Join-Path $env:ProgramData 'MyClean') (all versions, data and history). Type PURGE to confirm"
        if ($answer -cne 'PURGE') { Write-Host 'Cancelled.'; return }
    }
    Stop-Tray
    if (Get-ItemProperty -Path $RunKey -Name $RunValue -ErrorAction SilentlyContinue) {
        Remove-ItemProperty -Path $RunKey -Name $RunValue
    }
    $result = Invoke-Privileged ($Purge ? 'purge' : 'uninstall') $null
    if (-not $result.ok) { throw "Uninstall failed: $($result.error)" }
    Write-Ok ($Purge ? 'MyClean removed, including all versions and data.' : "MyClean service and tray removed; versions kept in $VersionsDir.")
}

# ---------------------------------------------------------------- entry point

switch ($PSCmdlet.ParameterSetName) {
    'Privileged' { Invoke-PrivilegedMain }
    'Status'     { Show-Status }
    'Rollback'   { Invoke-Rollback }
    'Uninstall'  { Invoke-Uninstall }
    default      { if ($Watch) { Start-Watch } else { Invoke-DeployOnce } }
}
