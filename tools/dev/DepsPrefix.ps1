# Dot-sourced by tools/dev/check.ps1 and tools/deploy/deploy.ps1; needs cl.exe and ninja on PATH.
#
# cl.exe prints its /showIncludes notes ("Observação: incluindo arquivo:" on a Portuguese MSVC) in
# the code page of its console, or in the OEM code page when it has none, and ninja records a
# header dependency only when those bytes match the msvc_deps_prefix CMake wrote into rules.ninja.
# CMake 4.1 decodes the note in another code page and writes a garbled prefix (UTF-8 of
# "Observa├º├úo", or the OEM bytes). Objects compiled while it did not match have no header
# dependency, so a header edit leaves them stale: false ASan aborts in debug tests on 2026-11-10,
# heap corruption in release tests of the governor on 2026-11-11. So the prefix line is rewritten
# with what cl.exe prints in this console, every other byte of rules.ninja kept, and the objects
# compiled without dependencies are rebuilt.
function Repair-DepsPrefix([string]$BuildDir) {
    $rules = Join-Path $BuildDir 'CMakeFiles\rules.ninja'
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
    $text = [IO.File]::ReadAllText($rules)
    $fixed = [regex]::Replace($text, '(?m)^(\s*msvc_deps_prefix = )[^\r\n]*', { param($m) $m.Groups[1].Value + $prefix + ' ' })
    if ($fixed -ne $text) { [IO.File]::WriteAllText($rules, $fixed, [Text.UTF8Encoding]::new($false)) }
    $stale = & ninja -C $BuildDir -t deps 2>$null | Where-Object { $_ -match '^(\S+\.cpp\.obj): #deps 0,' } | ForEach-Object { $Matches[1] }
    $stale = @($stale)
    for ($first = 0; $first -lt $stale.Count; $first += 200) {
        $batch = $stale[$first..([Math]::Min($first + 199, $stale.Count - 1))]
        & ninja -C $BuildDir -t clean @batch | Out-Null
    }
}
