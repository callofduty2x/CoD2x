# generate_mss32_symbols_h.ps1
# Post-build: reads nm output from the just-linked (pre-strip) mss32.build.dll
# and emits src/mss32/symbols_mss32.h (RVA-based, one-build-behind pattern).
# Must run BEFORE strip so the COFF symbol table is still intact.
param(
    [string]$DllPath,   # full path to the pre-strip mss32.build.dll
    [string]$OutHdr,    # full path to emit symbols_mss32.h into
    [string]$NmExe      # full path to nm.exe
)

# ---------------------------------------------------------------------------
# Simplify a demangled C++ name to just the qualified function name.
# Strips the parameter list (last balanced parens at depth 0) and the
# return-type prefix (everything before the last space not inside <> or ()).
# Returns $null for symbols that are not useful in a crash stack.
# ---------------------------------------------------------------------------
function Simplify-CppName {
    param([string]$raw)

    # Skip compiler/STL noise that is useless in a crash report
    $noisePatterns = @(
        '\{lambda',         # lambda wrappers
        '^vtable for ',
        '^typeinfo for ',
        '^typeinfo name for ',
        'non-virtual thunk',
        'virtual thunk',
        '^std::',           # STL internals (after a potential return-type prefix)
        '__gnu_cxx',
        '__cxx',
        '_GLOBAL__'
    )
    foreach ($pat in $noisePatterns) {
        if ($raw -match $pat) { return $null }
    }

    # Find the last '(' at angle-bracket depth 0 — that is the parameter list.
    $depth = 0
    $lastParen = -1
    for ($i = 0; $i -lt $raw.Length; $i++) {
        $c = $raw[$i]
        if     ($c -eq '<') { $depth++ }
        elseif ($c -eq '>') { $depth-- }
        elseif ($c -eq '(' -and $depth -eq 0) { $lastParen = $i }
    }

    $name = if ($lastParen -ge 0) { $raw.Substring(0, $lastParen).TrimEnd() } else { $raw }

    # Strip return-type prefix: last space not inside <> or ()
    $depth = 0
    $lastSpace = -1
    for ($i = 0; $i -lt $name.Length; $i++) {
        $c = $name[$i]
        if     ($c -eq '<' -or $c -eq '(') { $depth++ }
        elseif ($c -eq '>' -or $c -eq ')') { $depth-- }
        elseif ($c -eq ' ' -and $depth -eq 0) { $lastSpace = $i }
    }
    $name = if ($lastSpace -ge 0) { $name.Substring($lastSpace + 1) } else { $name }

    # After stripping, re-check that the result isn't an STL name
    foreach ($pat in @('^std::', '__gnu_cxx', '__cxx')) {
        if ($name -match $pat) { return $null }
    }

    # Skip anything that's still obviously not a plain function name
    if ($name.Length -eq 0) { return $null }

    return $name
}


# PE32 layout: e_lfanew -> "PE\0\0" (4) + IMAGE_FILE_HEADER (20) + ImageBase @ offset 28
#   => ImageBase is at file offset: e_lfanew + 4 + 20 + 28 = e_lfanew + 52 = e_lfanew + 0x34
# ---------------------------------------------------------------------------
$bytes     = [System.IO.File]::ReadAllBytes($DllPath)
$eLfanew   = [BitConverter]::ToUInt32($bytes, 0x3C)
$imageBase = [BitConverter]::ToUInt32($bytes, [int]$eLfanew + 0x34)
Write-Host "  ImageBase: 0x$('{0:X8}' -f $imageBase)" -ForegroundColor DarkCyan

# ---------------------------------------------------------------------------
# Run nm to extract all defined text-section symbols
# Output format per line: "XXXXXXXX [Tt] name"
# ---------------------------------------------------------------------------
$nmOutput = & $NmExe --defined-only --numeric-sort --demangle $DllPath 2>&1

$linePattern = '^([0-9a-fA-F]{8})\s+[Tt]\s+(.+)$'
$entries = [System.Collections.Generic.List[object]]::new()

foreach ($line in $nmOutput) {
    if ($line -match $linePattern) {
        $va   = [Convert]::ToUInt32($Matches[1], 16)
        $raw  = $Matches[2].Trim()

        # Skip section labels (e.g. ".text")
        if ($raw -match '^\.' ) { continue }

        # Strip the single leading underscore added by MinGW C calling convention.
        # Leave __double-underscore names (compiler internals) alone.
        if ($raw -match '^_[^_]') {
            $raw = $raw.Substring(1)
        }

        # Strip stdcall/fastcall size decoration (e.g. "Foo@12" -> "Foo")
        $raw = $raw -replace '@\d+$', ''

        # Simplify to just the qualified function name; skip noise symbols.
        $name = Simplify-CppName $raw
        if ($null -eq $name) { continue }

        $rva = $va - $imageBase
        $entries.Add([PSCustomObject]@{ Rva = $rva; Name = $name })
    }
}

# Deduplicate: if multiple symbols share the same RVA, keep only one.
# Prefer names that don't start with '_' or look like compiler internals.
$seen = @{}
$deduped = [System.Collections.Generic.List[object]]::new()
foreach ($e in $entries) {
    if (-not $seen.ContainsKey($e.Rva)) {
        $seen[$e.Rva] = $true
        $deduped.Add($e)
    }
}
$entries = @($deduped | Sort-Object Rva)

$entries = @($entries | Sort-Object Rva)
Write-Host "  Symbols found: $($entries.Count)" -ForegroundColor DarkCyan

# ---------------------------------------------------------------------------
# Compute function lengths as distance to the next entry.
# The last entry gets len=0 (unknown / accept anything >= its RVA).
# ---------------------------------------------------------------------------
for ($i = 0; $i -lt $entries.Count; $i++) {
    if ($i + 1 -lt $entries.Count) {
        $len = $entries[$i + 1].Rva - $entries[$i].Rva
    } else {
        $len = 0
    }
    $entries[$i] | Add-Member -NotePropertyName Len -NotePropertyValue $len
}

# ---------------------------------------------------------------------------
# Emit the header
# ---------------------------------------------------------------------------
Write-Host "  Generating $OutHdr ..." -ForegroundColor Cyan

$sb = [System.Text.StringBuilder]::new()

$null = $sb.AppendLine(@'
#ifndef SYMBOLS_MSS32_H
#define SYMBOLS_MSS32_H

// Auto-generated by generate_mss32_symbols_h.ps1 - do not edit manually.
// Maps mss32.dll function RVAs + lengths to symbol names.
// Use symbols_mss32_find(rva) to resolve any RVA inside a function.
// RVA = absolute_address - (uint32_t)(uintptr_t)GetModuleHandleA("mss32.dll")

#include <stdint.h>
#include <stddef.h>

struct Mss32Symbol
{
    uint32_t    rva;    // RVA from DLL image base
    uint32_t    len;    // function length in bytes (0 = unknown / last entry)
    const char* name;   // symbol name
};

'@)

$null = $sb.AppendLine("static const Mss32Symbol symbols_mss32[] =")
$null = $sb.AppendLine("{")

foreach ($e in $entries) {
    # Escape backslashes and double-quotes for use inside C string literals
    $escaped = $e.Name -replace '\\','\\' -replace '"','\"'
    $null = $sb.AppendLine("    { 0x$('{0:x8}' -f $e.Rva)u, 0x$('{0:x}' -f $e.Len)u, `"$escaped`" },")
}

$null = $sb.AppendLine("};")
$null = $sb.AppendLine("")

$null = $sb.AppendLine(@'
static const size_t symbols_mss32_count = sizeof(symbols_mss32) / sizeof(symbols_mss32[0]);

// Binary search: returns the Mss32Symbol whose range contains 'rva', or nullptr.
static inline const Mss32Symbol* symbols_mss32_find(uint32_t rva)
{
    int lo = 0;
    int hi = (int)symbols_mss32_count - 1;

    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        const Mss32Symbol& f = symbols_mss32[mid];

        if (rva < f.rva)
            hi = mid - 1;
        else if (f.len > 0 && rva >= f.rva + f.len)
            lo = mid + 1;
        else
            return &symbols_mss32[mid];
    }
    return nullptr;
}

// Returns the symbol name if 'rva' falls within a known function, or nullptr.
static inline const char* symbols_mss32_getName(uint32_t rva)
{
    const Mss32Symbol* f = symbols_mss32_find(rva);
    return f ? f->name : nullptr;
}

#endif // SYMBOLS_MSS32_H
'@)

$newContent = $sb.ToString()
$oldContent = if (Test-Path $OutHdr) { [System.IO.File]::ReadAllText($OutHdr, [System.Text.Encoding]::UTF8) } else { $null }

if ($newContent -eq $oldContent) {
    Write-Host "  Unchanged: $OutHdr (second build will be a no-op)" -ForegroundColor Gray
} else {
    [System.IO.File]::WriteAllText($OutHdr, $newContent, [System.Text.Encoding]::UTF8)
    Write-Host "  Updated: $OutHdr" -ForegroundColor Green
}
