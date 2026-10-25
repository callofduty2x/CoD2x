<#
.SYNOPSIS
    Crash-handler end-to-end test runner for the CoD2x Windows client.

.DESCRIPTION
    For each selected EXCEPTION_TEST mode this launches the client with
    "+set exception_test <mode>", waits for it to crash and exit, then checks the newest
    crash report (CoD2MP_s.exe.crash.*.txt) for the markers that mode should produce.

    Requires a build with the self test compiled in:
        make build_win_test DEBUG=0
    The modes and their expected reports are documented in src/mss32/exception.cpp.

    In test mode the crash handler writes the .txt / .dmp and exits without the dialog,
    telemetry or restart, so this runs unattended (it does need a desktop - the client
    opens a window, and modes 3/4 need the renderer).

.PARAMETER Modes
    Which modes to run, as a comma- or space-separated string (default "1,2,3,4,5,6,7,8,9").

.PARAMETER GameDir
    Folder containing CoD2MP_s.exe and mss32.dll (default: <repo>\bin\windows).

.PARAMETER ExtraArgs
    Extra command-line arguments passed to the client (e.g. "+set r_fullscreen 0").

.PARAMETER TimeoutSec
    How long to wait for the client to crash and exit before killing it (default 45).

.EXAMPLE
    pwsh tools/test/run_crash_tests.ps1
    pwsh tools/test/run_crash_tests.ps1 -Modes "1,2,5" -ExtraArgs "+set r_fullscreen 0"
#>
param(
    [string] $Modes = "1,2,3,4,5,6,7,8,9",
    [string] $GameDir = (Join-Path $PSScriptRoot "..\..\bin\windows"),
    [string] $ExtraArgs = "+set r_fullscreen 0",
    [int] $TimeoutSec = 45
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $GameDir "CoD2MP_s.exe"

# Parse the mode list from a string (commas and/or spaces), so it survives shell/argument quirks.
$modeList = @($Modes -split '[,\s]+' | Where-Object { $_ -ne "" } | ForEach-Object { [int] $_ })
if ($modeList.Count -eq 0) { Write-Error "No valid modes in -Modes '$Modes'" }

# Per-mode checks. 'All' markers must all be present; 'Any' (if set) requires at least one.
# Matching is case-insensitive substring. "Test mode:    <n>" confirms the report is from this run/mode.
$expected = @{
    1 = @{ Name = "main thread / main module";   All = @("ACCESS_VIOLATION","CoD2MP_s.exe","CoD2 MainThread","CL_Frame") }
    2 = @{ Name = "other thread / mss32.dll";     All = @("ACCESS_VIOLATION","mss32.dll","CoD2x ExceptionTest") }
    3 = @{ Name = "renderer module";              All = @("ACCESS_VIOLATION","gfx_d3d_mp_x86_s.dll","CoD2 RenderBackend") }
    4 = @{ Name = "system/graphics module";       All = @("ACCESS_VIOLATION","ddraw") }
    5 = @{ Name = "stack overflow";               All = @("STACK_OVERFLOW") }
    6 = @{ Name = "heap corruption";              Expect = "noreport" }  # __fastfail bypasses the handler on Win10/11
    7 = @{ Name = "abort() (SIGABRT)";            All = @("COD2X_FATAL","abort() called") }
    8 = @{ Name = "std::terminate";               All = @("COD2X_FATAL","terminate") }
    9 = @{ Name = "secondary exception";          All = @("Secondary exception") }
}

if (-not (Test-Path $exe)) {
    Write-Error "Client not found: $exe`nBuild a test client first:  make build_win_test DEBUG=0"
}

Write-Host "Client:   $exe"
Write-Host "Modes:    $($modeList -join ', ')"
Write-Host ""

$results = @()

foreach ($mode in $modeList) {
    $info = $expected[$mode]
    $label = if ($info) { $info.Name } else { "unknown" }
    Write-Host ("=== Mode {0}: {1} ===" -f $mode, $label)

    $cutoff = Get-Date
    Start-Sleep -Milliseconds 50   # ensure new files sort strictly after $cutoff

    # The env var is the reliable activation channel (inherited by the child); the command line is a convenience.
    $env:COD2X_EXCEPTION_TEST = "$mode"
    $argLine = "+set exception_test $mode $ExtraArgs"
    $launchedAt = Get-Date
    $proc = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory $GameDir -PassThru

    $exited = $proc.WaitForExit($TimeoutSec * 1000)
    $ranFor = (New-TimeSpan -Start $launchedAt -End (Get-Date)).TotalSeconds
    if ($exited -and $ranFor -lt 3) {
        Write-Host ("  note: client exited after {0:N1}s - it may not have started (another instance running? wrong dir?)" -f $ranFor)
    }
    if (-not $exited) {
        Write-Host "  client did not exit within ${TimeoutSec}s, killing it"
        try { $proc | Stop-Process -Force -ErrorAction SilentlyContinue } catch {}
        $proc.WaitForExit(5000) | Out-Null
    }
    Start-Sleep -Milliseconds 300  # let the last file write flush

    # Newest crash .txt produced by this run
    $report = Get-ChildItem -Path $GameDir -Filter "CoD2MP_s.exe.crash.*.txt" -ErrorAction SilentlyContinue |
              Where-Object { $_.LastWriteTime -ge $cutoff } |
              Sort-Object LastWriteTime -Descending | Select-Object -First 1

    $pass = $true
    $notes = @()
    $expectNoReport = ($info.ContainsKey("Expect") -and $info.Expect -eq "noreport")

    # Init marker: confirms the client ran and which mode it resolved (written before the crash).
    $marker = Join-Path $GameDir "CoD2MP_s.exe.exception_test_init.log"
    $markerFresh = (Test-Path $marker) -and ((Get-Item $marker).LastWriteTime -ge $cutoff)
    $ranThisMode = $markerFresh -and ((Get-Content -Raw -LiteralPath $marker) -match "resolved mode=$mode\b")

    if ($expectNoReport) {
        # This mode is expected to terminate the process without a report (uncatchable fast-fail).
        if (-not $ranThisMode) {
            $pass = $false
            $notes += "client did not run mode $mode (marker missing/stale)"
        } elseif ($report) {
            $pass = $false
            $notes += "unexpected report produced (this mode should fast-fail uncatchably): $($report.Name)"
        } else {
            $notes += "no report as expected (heap fast-fail bypasses the handler); client exited in $([math]::Round($ranFor,1))s"
        }
    } elseif (-not $report) {
        $pass = $false
        $notes += "no crash report was produced"
        if ($markerFresh) {
            $notes += "init: " + ((Get-Content -Raw -LiteralPath $marker).Trim())
        } elseif (Test-Path $marker) {
            $notes += "init marker is stale (client's exception_init did not run this launch)"
        } else {
            $notes += "no init marker (not a test build, or client never reached Com_Init)"
        }
    } else {
        $text = Get-Content -Raw -LiteralPath $report.FullName
        $missing = @()
        foreach ($m in @("Test mode:") + $info.All) {
            if ($text -notmatch [regex]::Escape($m)) { $missing += $m }
        }
        if ($info.ContainsKey("Any") -and $info.Any.Count -gt 0) {
            $anyHit = $false
            foreach ($m in $info.Any) { if ($text -match [regex]::Escape($m)) { $anyHit = $true } }
            if (-not $anyHit) { $missing += "one of [$($info.Any -join ', ')]" }
        }
        # Confirm the report's own "Test mode: N" matches the requested mode
        if ($text -notmatch "Test mode:\s+$mode\b") { $missing += "Test mode: $mode" }
        # Telemetry must have been delivered to the backend (the whole point of the E2E run)
        if ($text -match "Telemetry:\s*sent") { $notes += "telemetry: sent" }
        else { $missing += "telemetry not sent" }

        if ($missing.Count -gt 0) {
            $pass = $false
            $notes += "missing: " + ($missing -join "; ")
        }
        $notes += "report: $($report.Name)"
    }

    Remove-Item Env:\COD2X_EXCEPTION_TEST -ErrorAction SilentlyContinue

    $status = if ($pass) { "PASS" } else { "FAIL" }
    Write-Host ("  {0}  {1}" -f $status, ($notes -join " | "))
    Write-Host ""
    $results += [pscustomobject]@{ Mode = $mode; Label = $label; Result = $status }
}

Write-Host "===== Summary ====="
$results | Format-Table -AutoSize | Out-String | Write-Host
$failed = @($results | Where-Object { $_.Result -eq "FAIL" })
Write-Host ("{0}/{1} passed" -f ($results.Count - $failed.Count), $results.Count)
if ($failed.Count -gt 0) { exit 1 } else { exit 0 }
