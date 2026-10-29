# Regenerates licenses/LICENSES-sea-g2p-crates.txt: the crates linked into sea_g2p_rs.dll (sea-g2p v0.10.0, built with
# --no-default-features --features capi), their copyright lines and one copy of every distinct licence text.
# Downloads Cargo.lock and the .crate source tarballs once into build/downloads/crates (verified against the Cargo.lock
# checksums); nothing from them is executed or built. Usage: pwsh tools/gen_crate_licenses.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
$root = Split-Path $PSScriptRoot -Parent
$dl = Join-Path $root 'build/downloads/crates'
$out = Join-Path $root 'licenses/LICENSES-sea-g2p-crates.txt'
$lockUrl = 'https://raw.githubusercontent.com/pnnbao97/sea-g2p/v0.10.0/Cargo.lock'
$lockSha = '2dbd4228dee3b83c9b4b82dda8289d0d5e835f24c08285d111cb3e11732b4d93'
$rootName = 'sea-g2p-rs'
$rootDeps = 'regex', 'fancy-regex', 'once_cell', 'memmap2', 'rayon', 'unicode-normalization'
$maxEntry = 2MB

function Get-Verified([string]$Url, [string]$Sha, [string]$File) {
    if (-not (Test-Path $File) -or (Get-FileHash $File -Algorithm SHA256).Hash -ne $Sha.ToUpper()) {
        New-Item -ItemType Directory -Force (Split-Path $File) | Out-Null
        Invoke-WebRequest -Uri $Url -OutFile "$File.part" -UseBasicParsing
        if ((Get-FileHash "$File.part" -Algorithm SHA256).Hash -ne $Sha.ToUpper()) { Remove-Item "$File.part"; throw "SHA256 mismatch: $Url" }
        Move-Item "$File.part" $File -Force
    }
}

function Read-Lock([string]$Path) {
    $pk = @{}
    $cur = $null
    foreach ($l in Get-Content $Path) {
        if ($l -eq '[[package]]') { $cur = @{ deps = @(); sum = '' }; continue }
        if (-not $cur) { continue }
        if ($l -match '^name = "(.+)"') { $cur.name = $Matches[1] }
        elseif ($l -match '^version = "(.+)"') { $cur.ver = $Matches[1]; $pk["$($cur.name)"] = $cur }
        elseif ($l -match '^checksum = "(.+)"') { $cur.sum = $Matches[1] }
        elseif ($l -match '^\s+"([^" ]+)(?: [^"]+)?",?$') { $cur.deps += $Matches[1] }
    }
    $pk
}

# Minimal tar.gz reader: returns name -> bytes for the few small files we need.
function Read-Crate([string]$Path, [scriptblock]$Want) {
    $files = @{}
    $fs = [IO.File]::OpenRead($Path)
    try {
        $gz = New-Object IO.Compression.GZipStream($fs, [IO.Compression.CompressionMode]::Decompress)
        $hdr = New-Object byte[] 512
        $long = $null
        while ($true) {
            $n = 0
            while ($n -lt 512) { $r = $gz.Read($hdr, $n, 512 - $n); if ($r -le 0) { break }; $n += $r }
            if ($n -lt 512 -or $hdr[0] -eq 0) { break }
            $name = [Text.Encoding]::UTF8.GetString($hdr, 0, 100).TrimEnd([char]0)
            $pre = [Text.Encoding]::UTF8.GetString($hdr, 345, 155).TrimEnd([char]0)
            if ($pre) { $name = "$pre/$name" }
            $size = [Convert]::ToInt64(([Text.Encoding]::ASCII.GetString($hdr, 124, 11)).Trim([char]0, ' '), 8)
            $type = [char]$hdr[156]
            $take = ($type -eq 'L') -or (($type -eq '0' -or $type -eq [char]0) -and (& $Want ($(if ($long) { $long } else { $name }))))
            if ($size -gt $maxEntry) { $take = $false }
            $pad = ([int](512 * [math]::Ceiling($size / 512.0)))
            $buf = New-Object byte[] $pad
            $n = 0
            while ($n -lt $pad) { $r = $gz.Read($buf, $n, $pad - $n); if ($r -le 0) { break }; $n += $r }
            if ($type -eq 'L') { $long = [Text.Encoding]::UTF8.GetString($buf, 0, [int]$size).TrimEnd([char]0); continue }
            if ($take) { $files[$(if ($long) { $long } else { $name })] = [Text.Encoding]::UTF8.GetString($buf, 0, [int]$size) }
            $long = $null
        }
    } finally { $fs.Dispose() }
    $files
}

function Get-NormalDeps([string]$Toml) {
    $normal = @(); $sec = ''
    foreach ($l in $Toml -split "`r?`n") {
        if ($l -match '^\s*\[(.+)\]\s*$') { $sec = $Matches[1]; $s = $sec
            if ($s -match "^(target\..+\.)?dependencies\.([\w\-]+)$" -and $s -notmatch 'cfg\((unix|target_os = "(linux|macos|android)")') { $normal += $Matches[2] }
            continue }
        if ($sec -match '^(target\..+\.)?dependencies$' -and $sec -notmatch 'cfg\(unix\)' -and $l -match '^([\w\-]+)\s*=') { $normal += $Matches[1] }
    }
    $normal | Select-Object -Unique
}

New-Item -ItemType Directory -Force $dl | Out-Null
$lockFile = Join-Path $dl 'sea-g2p-0.10.0.Cargo.lock'
Get-Verified $lockUrl $lockSha $lockFile
$lock = Read-Lock $lockFile
$linked = [System.Collections.Generic.SortedSet[string]]::new()
$queue = [System.Collections.Generic.Queue[string]]::new()
$rootDeps | ForEach-Object { $queue.Enqueue($_) }
$meta = @{}
while ($queue.Count) {
    $n = $queue.Dequeue()
    if (-not $linked.Add($n)) { continue }
    $c = $lock[$n]; if (-not $c) { throw "not in lock: $n" }
    $file = Join-Path $dl "$n-$($c.ver).crate"
    Get-Verified "https://static.crates.io/crates/$n/$n-$($c.ver).crate" $c.sum $file
    $prefix = "$n-$($c.ver)/"
    $files = Read-Crate $file { param($x) $x.StartsWith($prefix) -and $x.Substring($prefix.Length) -match '^(Cargo\.toml|(LICENSE|LICENCE|COPYING|UNLICENSE|NOTICE)[^/]*)$' }
    $toml = $files["${prefix}Cargo.toml"]
    $lic = if ($toml -match '(?m)^license\s*=\s*"([^"]+)"') { $Matches[1] } else { '' }
    $texts = @{}
    foreach ($k in $files.Keys | Sort-Object) { if ($k -ne "${prefix}Cargo.toml") { $texts[$k.Substring($prefix.Length)] = $files[$k] } }
    $meta[$n] = @{ ver = $c.ver; lic = $lic; texts = $texts }
    $allowed = $lock[$n].deps
    foreach ($d in Get-NormalDeps $toml) { if ($allowed -contains $d) { $queue.Enqueue($d) } }
}

function Get-Copyright([string]$Text) {
    if ($Text -match 'Apache License') { return }
    $Text -split "`r?`n" | ForEach-Object { $_.Trim() } |
        Where-Object { $_ -cmatch '^(Copyright|\(C\)|©)' } | Select-Object -Unique
}

function Get-Key([string]$Text) {
    $body = ($Text -split "`r?`n" | Where-Object { $_.Trim() -notmatch '^Copyright' }) -join ' '
    $h = [Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes(($body -replace '\s+', ' ').Trim()))
    [BitConverter]::ToString($h)
}

$sb = [System.Text.StringBuilder]::new()
$nl = "`r`n"
[void]$sb.Append("LICENCES OF THE RUST CRATES IN sea_g2p_rs.dll (sea-g2p v0.10.0)$nl")
[void]$sb.Append("========================================================$nl")
[void]$sb.Append("Crates statically linked into the Windows DLL: normal dependencies of sea-g2p-rs built with --no-default-features$nl")
[void]$sb.Append("--features capi, taken from its Cargo.lock; build-only, dev-only and unix-only crates are excluded. Optional crates the$nl")
[void]$sb.Append("lock enables for some feature are kept (superset). Copyright lines and texts come from each crate's own licence files$nl")
[void]$sb.Append("at the pinned version (crates.io source tarball). Regenerate with tools/gen_crate_licenses.ps1.$nl$nl")
$groups = [ordered]@{}
$missing = @()
foreach ($n in $linked) {
    $m = $meta[$n]
    [void]$sb.Append("$n $($m.ver)  [$($m.lic)]$nl")
    $any = $false
    foreach ($f in $m.texts.Keys | Sort-Object) {
        if ($f -match '^NOTICE') { continue }
        foreach ($cl in Get-Copyright $m.texts[$f]) { [void]$sb.Append("    $cl$nl"); $any = $true }
        $k = Get-Key $m.texts[$f]
        if (-not $groups.Contains($k)) { $groups[$k] = @{ crates = @(); text = $m.texts[$f]; file = $f } }
        $groups[$k].crates += "$n ($f)"
    }
    if ($m.texts.Count -eq 0) { $missing += $n }
    if (-not $any) { [void]$sb.Append("    (no copyright line in the licence files)$nl") }
}
[void]$sb.Append("${nl}Rust standard library (linked into the DLL): MIT OR Apache-2.0, Copyright (c) The Rust Project Developers.$nl")
[void]$sb.Append("The MIT and Apache-2.0 texts below apply to it; its source is https://github.com/rust-lang/rust .$nl")
$i = 0
foreach ($g in $groups.Values) {
    $i++
    [void]$sb.Append("$nl$nl################################################################################$nl")
    [void]$sb.Append("LICENCE TEXT $i of $($groups.Count), used by:$nl")
    foreach ($c in $g.crates) { [void]$sb.Append("  $c$nl") }
    [void]$sb.Append("(the copyright line of each crate is listed above; the text below is the first copy)$nl")
    [void]$sb.Append("################################################################################$nl$nl")
    [void]$sb.Append(($g.text.Trim() -replace "`r?`n", $nl) + $nl)
}
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
[IO.File]::WriteAllText($out, $sb.ToString(), (New-Object Text.UTF8Encoding($false)))
"crates: $($linked.Count); distinct texts: $($groups.Count); without licence file: $($missing -join ', ')"
