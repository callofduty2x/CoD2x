<#
    Selects one test mod from tests/mods and prepares it for the game (see tests/mods/README.md):
      - zips tests/mods/<name> into bin/windows/mods/test/<name>.iwd (other iwd files there are deleted,
        so only the selected test mod is loaded)
      - writes bin/windows/mods/test/test.cfg, which execs <name>.cfg from the iwd

    The game is then started with:  +set fs_game mods/test +exec test.cfg   (VS Code launch "Win tests")

    select_test_mod.ps1                     asks which test mod (Enter = the last one)
    select_test_mod.ps1 -Mod test_killfeed  no question
    select_test_mod.ps1 -Run                also starts the game (without the debugger)
    select_test_mod.ps1 -Run -Dedicated     starts a dedicated server instead
    select_test_mod.ps1 -Run -GameArgs "+set scr_test_interval 1"   extra arguments for the game
#>
param(
    [string]$Mod = "",
    [switch]$Run,
    [switch]$Dedicated,
    [string]$GameArgs = ""
)

$ErrorActionPreference = "Stop"

$root     = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$testsDir = Join-Path $root "tests\mods"
$gameDir  = Join-Path $root "bin\windows"
$outDir   = Join-Path $gameDir "mods\test"
$zip      = Join-Path $root "tools\7zip\7za.exe"
$lastFile = Join-Path $outDir "last_test_mod.txt"

$mods = @(Get-ChildItem $testsDir -Directory | Sort-Object Name)
if ($mods.Count -eq 0) { throw "No test mods in $testsDir" }
$names = @($mods | ForEach-Object { $_.Name })

# The first comment line of <name>.cfg describes the test mod
function Get-Description($name) {
    $cfg = Join-Path $testsDir "$name\$name.cfg"
    if (-not (Test-Path $cfg)) { return "(missing $name.cfg)" }
    return ((Get-Content $cfg -TotalCount 1) -replace '^\s*//\s*', '')
}

if ($Mod -eq "") {
    $last = ""
    if (Test-Path $lastFile) { $last = (Get-Content $lastFile -TotalCount 1).Trim() }
    if ($names -notcontains $last) { $last = $names[0] }

    Write-Host ""
    Write-Host "Test mods in tests/mods:"
    for ($i = 0; $i -lt $names.Count; $i++) {
        $mark = if ($names[$i] -eq $last) { "*" } else { " " }
        Write-Host ("  {0} {1,2})  {2,-24} {3}" -f $mark, ($i + 1), $names[$i], (Get-Description $names[$i]))
    }
    Write-Host ""

    while ($true) {
        $answer = (Read-Host "Number or name (Enter = $last)").Trim()
        if ($answer -eq "") { $answer = $last }
        if ($answer -match '^\d+$' -and [int]$answer -ge 1 -and [int]$answer -le $names.Count) {
            $Mod = $names[[int]$answer - 1]
            break
        }
        if ($names -contains $answer) { $Mod = $answer; break }
        Write-Host "Unknown test mod '$answer'"
    }
}
elseif ($names -notcontains $Mod) {
    throw "Unknown test mod '$Mod' (available: $($names -join ', '))"
}

$src = Join-Path $testsDir $Mod
if (-not (Test-Path (Join-Path $src "$Mod.cfg"))) { throw "tests/mods/$Mod/$Mod.cfg is missing" }

# Only the selected test mod is in the folder
New-Item -ItemType Directory -Force $outDir | Out-Null
Get-ChildItem $outDir -Filter "*.iwd" | Remove-Item -Force

$iwd = Join-Path $outDir "$Mod.iwd"
& $zip a -tzip $iwd (Join-Path $src "*") | Out-Null
if ($LASTEXITCODE -ne 0) { throw "7za failed with exit code $LASTEXITCODE" }

Set-Content -Encoding ascii -Path (Join-Path $outDir "test.cfg") -Value @(
    "// Written by tools/test/select_test_mod.ps1 - the selected test mod",
    "exec $Mod.cfg"
)
Set-Content -Encoding ascii -Path $lastFile -Value $Mod

Write-Host "Test mod '$Mod' -> bin/windows/mods/test/$Mod.iwd"

if ($Run) {
    $arguments = "+set fs_game mods/test +exec test.cfg"
    if ($Dedicated) { $arguments = "+set dedicated 1 " + $arguments }
    if ($GameArgs -ne "") { $arguments += " " + $GameArgs }
    Write-Host "Starting CoD2MP_s.exe $arguments"
    Start-Process (Join-Path $gameDir "CoD2MP_s.exe") -WorkingDirectory $gameDir -ArgumentList $arguments
}
