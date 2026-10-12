# generate_cod2_symbols.ps1
# Single script that, for each configured module:
#   1. Scans the Binary Ninja disassembly dump to find function start addresses + lengths
#   2. Emits a C++ header with a sorted symbol table and binary-search lookup helpers
#
# Re-run whenever any .bndb_disassembly.txt or .functions.txt file is updated.
# generate_mss32_symbols_h.ps1 remains separate because it is invoked by CMake post-build.

# ── Per-module configuration ──────────────────────────────────────────────────
$modules = @(
    @{
        # Binary Ninja disassembly dump of the whole binary
        BigFile    = "$PSScriptRoot\CoD2MP_s.exe.bndb_disassembly.txt"
        # List of function names to extract (one per line)
        NamesFile  = "$PSScriptRoot\CoD2MP_s.exe.bndb_pseudo_c.functions.txt"
        # Output C++ header
        OutHdr     = "$PSScriptRoot\..\mss32\symbols_cod2mp_s.h"
        # C++ identifiers
        Guard      = "SYMBOLS_COD2MP_S_H"
        StructName = "Cod2mpSymbol"
        Prefix     = "symbols_cod2mp_s"
        # Human-readable module description used in header comments
        ModuleDesc = "CoD2MP_s.exe"
    },
    @{
        BigFile    = "$PSScriptRoot\gfx_d3d_mp_x86_s.dll.bndb_disassembly.txt"
        NamesFile  = "$PSScriptRoot\gfx_d3d_mp_x86_s.dll.bndb_pseudo_c.functions.txt"
        OutHdr     = "$PSScriptRoot\..\mss32\symbols_gfx_d3d_mp_x86_s.h"
        Guard      = "SYMBOLS_GFX_D3D_MP_X86_S_H"
        StructName = "GfxD3dSymbol"
        Prefix     = "symbols_gfx_d3d_mp_x86_s"
        ModuleDesc = "gfx_d3d_mp_x86_s.dll"
    }
)

# ── Shared parser: parse one BN disassembly dump ─────────────────────────────
# Returns a hashtable: functionName -> List of @{ AddrHex; AddrInt; Len }
function Get-FunctionLookup([string]$bigFile) {
    $lookup      = @{}
    $functions   = [System.Collections.Generic.List[hashtable]]::new()
    $seen        = [System.Collections.Generic.HashSet[string]]::new()
    $lineCount   = 0
    $progressEveryLines = 250000

    # BN disassembly declaration lines look like one of these:
    # 00466460    int32_t __stdcall WinMain(HINSTANCE a, ...)
    # 004011e0    void __convention("regparm") sub_4011e0(...)
    # 00547c36    int32_t (* Direct3DCreate9)() = 0x...
    # We intentionally require 4 spaces after the address to avoid instruction lines.
    # NOTE: use greedy matching so we capture the function token nearest the
    # actual argument list, not earlier tokens like __convention.
    $declWithParenPattern  = '^([0-9a-fA-F]{8})\s{4}.*\b([^\s(=]+)\s*\(.*$'
    $declWithAssignPattern = '^([0-9a-fA-F]{8})\s{4}.*\b([^\s(=]+)\s*=\s*.*$'
    $pointerDeclPattern    = '^([0-9a-fA-F]{8})\s{4}.*\(\*\s*([^\s)]+)\s*\).*='

    function Store-Func([string]$name, [long]$addrInt, [long]$len) {
        if (-not $lookup.ContainsKey($name)) {
            $lookup[$name] = [System.Collections.Generic.List[hashtable]]::new()
        }
        $null = $lookup[$name].Add(@{
            AddrHex = '0x{0:x8}' -f $addrInt
            AddrInt = $addrInt
            Len     = $len      # -1 = unknown (last function without next start)
        })
    }

    foreach ($line in [System.IO.File]::ReadLines($bigFile)) {
        $lineCount++
        if (($lineCount % $progressEveryLines) -eq 0) {
            Write-Host ("  [scan] lines={0:n0} symbols={1:n0}" -f $lineCount, $functions.Count) -ForegroundColor DarkGray
        }

        $addrInt = $null
        $name = $null

        if ($line -match $pointerDeclPattern) {
            $addrInt = [Convert]::ToInt64($Matches[1], 16)
            $name = $Matches[2]
        }
        elseif ($line -match $declWithParenPattern) {
            $addrInt = [Convert]::ToInt64($Matches[1], 16)
            $name = $Matches[2]
        }
        elseif ($line -match $declWithAssignPattern) {
            $addrInt = [Convert]::ToInt64($Matches[1], 16)
            $name = $Matches[2]
        }

        if ($null -ne $addrInt -and $null -ne $name -and $name -ne '') {
            $key = ('{0:x8}:{1}' -f $addrInt, $name)
            if (-not $seen.Contains($key)) {
                $seen.Add($key) | Out-Null
                $null = $functions.Add(@{
                    AddrInt = $addrInt
                    Name    = $name
                })
            }
        }
    }

    Write-Host ("  [scan] complete lines={0:n0} symbols={1:n0}" -f $lineCount, $functions.Count) -ForegroundColor DarkGray

    $functions = @($functions | Sort-Object AddrInt, Name)

    # Build next-distinct-address map (O(n)) to avoid nested scans.
    $uniqueAddrs = @($functions | ForEach-Object { [long]$_['AddrInt'] } | Sort-Object -Unique)
    $nextAddrByAddr = @{}
    for ($i = 0; $i -lt $uniqueAddrs.Count; $i++) {
        $next = -1L
        if ($i -lt $uniqueAddrs.Count - 1) {
            $next = [long]$uniqueAddrs[$i + 1]
        }
        [void]($nextAddrByAddr[[long]$uniqueAddrs[$i]] = $next)
    }

    # Compute function lengths from the next distinct function start.
    for ($i = 0; $i -lt $functions.Count; $i++) {
        if ($i -gt 0 -and ($i % 200000) -eq 0) {
            Write-Host ("  [range] processed {0:n0}/{1:n0}" -f $i, $functions.Count) -ForegroundColor DarkGray
        }

        $cur  = $functions[$i]
        $nextAddr = [long]$nextAddrByAddr[[long]$cur.AddrInt]

        if ($nextAddr -gt $cur.AddrInt) {
            $len = $nextAddr - $cur.AddrInt
            Store-Func $cur.Name $cur.AddrInt $len
        }
        else {
            Store-Func $cur.Name $cur.AddrInt -1
        }
    }

    Write-Host ("  [range] complete {0:n0}/{0:n0}" -f $functions.Count) -ForegroundColor DarkGray

    return $lookup
}

function Normalize-SymbolName([string]$name) {
    if ($null -eq $name) { return "" }

    $n = $name.Trim()
    $n = $n -replace '\\n', ''
    $n = $n -replace '"', ''
    $n = $n -replace '\(\)$', ''

    return $n
}

function Get-ResolveCandidates([string]$name) {
    $candidates = [System.Collections.Generic.List[string]]::new()
    if ([string]::IsNullOrWhiteSpace($name)) {
        return $candidates
    }

    $n = $name.Trim()
    $null = $candidates.Add($n)
    $null = $candidates.Add(($n -replace '\\n', ''))

    if ($n.EndsWith('?')) {
        $null = $candidates.Add($n.Substring(0, $n.Length - 1))
    }
    else {
        $null = $candidates.Add($n + '?')
    }

    if ($n.StartsWith('_')) {
        $null = $candidates.Add($n.TrimStart('_'))
    }
    else {
        $null = $candidates.Add('_' + $n)
    }

    return @($candidates | Sort-Object -Unique)
}

# ── Shared header emitter ─────────────────────────────────────────────────────
function Write-SymbolHeader([PSCustomObject[]]$entries, [hashtable]$mod) {
    $guard      = $mod.Guard
    $structName = $mod.StructName
    $prefix     = $mod.Prefix
    $moduleDesc = $mod.ModuleDesc
    $outHdr     = $mod.OutHdr

    $sb = [System.Text.StringBuilder]::new()
    $null = $sb.AppendLine("#ifndef $guard")
    $null = $sb.AppendLine("#define $guard")
    $null = $sb.AppendLine("")
    $null = $sb.AppendLine("// Auto-generated by generate_cod2_symbols.ps1 - do not edit manually.")
    $null = $sb.AppendLine("// Maps $moduleDesc function start addresses + lengths to symbol names.")
    $null = $sb.AppendLine("// Use ${prefix}_getName(addr) to resolve any address inside a function.")
    $null = $sb.AppendLine("")
    $null = $sb.AppendLine("#include <stdint.h>")
    $null = $sb.AppendLine("#include <stddef.h>")
    $null = $sb.AppendLine("")
    $null = $sb.AppendLine("struct $structName")
    $null = $sb.AppendLine("{")
    $null = $sb.AppendLine("    uint32_t    addr;   // function start address")
    $null = $sb.AppendLine("    uint32_t    len;    // function length in bytes")
    $null = $sb.AppendLine("    const char* name;   // symbol name from Binary Ninja")
    $null = $sb.AppendLine("};")
    $null = $sb.AppendLine("")
    $null = $sb.AppendLine("static const $structName ${prefix}[] =")
    $null = $sb.AppendLine("{")

    foreach ($e in $entries) {
        $line = "    { 0x" + ('{0:x8}' -f $e.Addr) + "u, 0x" + ('{0:x}' -f $e.Len) + "u, `"" + $e.Name + "`" },"
        $null = $sb.AppendLine($line)
    }

    $null = $sb.AppendLine("};")
    $null = $sb.AppendLine("")
    $null = $sb.AppendLine("static const size_t ${prefix}_count = sizeof(${prefix}) / sizeof(${prefix}[0]);")
    $null = $sb.AppendLine("")
    $null = $sb.AppendLine("// Returns the $structName entry whose range contains 'addr', or nullptr.")
    $null = $sb.AppendLine("static inline const ${structName}* ${prefix}_find(uint32_t addr)")
    $null = $sb.AppendLine("{")
    $null = $sb.AppendLine("    int lo = 0;")
    $null = $sb.AppendLine("    int hi = (int)${prefix}_count - 1;")
    $null = $sb.AppendLine("")
    $null = $sb.AppendLine("    while (lo <= hi)")
    $null = $sb.AppendLine("    {")
    $null = $sb.AppendLine("        int mid = lo + (hi - lo) / 2;")
    $null = $sb.AppendLine("        const ${structName}& f = ${prefix}[mid];")
    $null = $sb.AppendLine("")
    $null = $sb.AppendLine("        if (addr < f.addr)")
    $null = $sb.AppendLine("            hi = mid - 1;")
    $null = $sb.AppendLine("        else if (addr >= f.addr + f.len)")
    $null = $sb.AppendLine("            lo = mid + 1;")
    $null = $sb.AppendLine("        else")
    $null = $sb.AppendLine("            return &${prefix}[mid];")
    $null = $sb.AppendLine("    }")
    $null = $sb.AppendLine("    return nullptr;")
    $null = $sb.AppendLine("}")
    $null = $sb.AppendLine("")
    $null = $sb.AppendLine("// Returns the symbol name if 'addr' falls within a known function's range,")
    $null = $sb.AppendLine("// or nullptr if not found.")
    $null = $sb.AppendLine("static inline const char* ${prefix}_getName(uint32_t addr)")
    $null = $sb.AppendLine("{")
    $null = $sb.AppendLine("    const ${structName}* f = ${prefix}_find(addr);")
    $null = $sb.AppendLine("    return f ? f->name : nullptr;")
    $null = $sb.AppendLine("}")
    $null = $sb.AppendLine("")
    $null = $sb.AppendLine("#endif // $guard")

    [System.IO.File]::WriteAllText($outHdr, $sb.ToString(), [System.Text.Encoding]::UTF8)
    Write-Host "  -> Written: $outHdr" -ForegroundColor Green
}

# ── Main loop: process each module ───────────────────────────────────────────
foreach ($mod in $modules) {
    Write-Host ""
    Write-Host "=== $($mod.ModuleDesc) ===" -ForegroundColor Magenta

    # ── Phase 1: build address lookup from disassembly dump ──────────────────
    Write-Host "Scanning: $($mod.BigFile)" -ForegroundColor Cyan
    $lookup = Get-FunctionLookup $mod.BigFile
    Write-Host "  -> $($lookup.Keys.Count) unique function definitions indexed" -ForegroundColor Gray

    $normalizedLookup = @{}
    foreach ($k in $lookup.Keys) {
        $nk = Normalize-SymbolName $k
        if (-not $normalizedLookup.ContainsKey($nk)) {
            $normalizedLookup[$nk] = $k
        }
    }

    # ── Phase 2: resolve wanted names ────────────────────────────────────────
    $wantedRaw = Get-Content $mod.NamesFile |
                 Where-Object { $_.Trim() -ne '' } |
                 Sort-Object -Unique

    Write-Host "Searching for $($wantedRaw.Count) names from: $($mod.NamesFile)" -ForegroundColor Cyan

    $found    = 0
    $notFound = 0
    $notFoundNames = [System.Collections.Generic.List[string]]::new()

    $wantedTotal = $wantedRaw.Count
    $wantedIndex = 0

    $entries = @(foreach ($raw in $wantedRaw) {
        $wantedIndex++
        if (($wantedIndex % 200) -eq 0) {
            Write-Host ("  [resolve] {0:n0}/{1:n0}" -f $wantedIndex, $wantedTotal) -ForegroundColor DarkGray
        }

        $name = ($raw -replace '\(.*', '').Trim()
        if ($name -eq '') { continue }

        $resolvedKey = $null
        if ($lookup.ContainsKey($name)) {
            $resolvedKey = $name
        }
        else {
            foreach ($candidate in (Get-ResolveCandidates $name)) {
                if ($lookup.ContainsKey($candidate)) {
                    $resolvedKey = $candidate
                    break
                }

                $nc = Normalize-SymbolName $candidate
                if ($normalizedLookup.ContainsKey($nc)) {
                    $resolvedKey = $normalizedLookup[$nc]
                    break
                }
            }
        }

        if ($null -ne $resolvedKey) {
            $found++
            foreach ($entry in $lookup[$resolvedKey]) {
                if ($entry.Len -ge 0) {
                    [PSCustomObject]@{
                        Addr = [uint32]$entry.AddrInt
                        Len  = [uint32]$entry.Len
                        Name = $name -replace '\\','\\' -replace '"','\"'
                    }
                }
            }
        } else {
            $notFound++
            $notFoundNames.Add($name) | Out-Null
        }
    }) | Sort-Object Addr

    Write-Host ("  [resolve] complete {0:n0}/{0:n0}" -f $wantedTotal) -ForegroundColor DarkGray

    Write-Host "  -> $found found, $notFound not found" -ForegroundColor Yellow
    if ($notFoundNames.Count -gt 0) {
        Write-Host "  -> Missing names:" -ForegroundColor DarkYellow
        foreach ($missing in ($notFoundNames | Sort-Object -Unique)) {
            Write-Host "     $missing" -ForegroundColor DarkYellow
        }
    }
    Write-Host "Generating header with $($entries.Count) entries" -ForegroundColor Cyan
    Write-SymbolHeader $entries $mod
}

Write-Host ""
Write-Host "All modules done." -ForegroundColor Green
