# Downloads a pinned daukle and runs it, so a project needs no daukle installed to build.
# The Windows half of the wrapper; see the comment at the top of `daukle` for the rest.

$ErrorActionPreference = 'Stop'

function Fail($message) {
    [Console]::Error.WriteLine("daukle wrapper: $message")
    exit 1
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$pinFile = Join-Path $scriptDir '.daukle/wrapper.toml'

# Deliberately not a TOML parser, same as the POSIX half: it reads the three keys this file is
# allowed to contain, so a pin carrying anything else is not silently half-understood.
function Read-Key($lines, $key) {
    foreach ($line in $lines) {
        if ($line -match "^\s*$key\s*=\s*`"([^`"]*)`"") { return $Matches[1] }
    }
    return $null
}

function Fetch($url, $destination) {
    try {
        # Explicitly not Invoke-WebRequest's default: its progress bar makes a large download
        # several times slower in a non-interactive host.
        $previous = $ProgressPreference
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $url -OutFile $destination -UseBasicParsing
        $ProgressPreference = $previous
    } catch {
        Fail "could not download $url : $($_.Exception.Message)"
    }
}

# `.\daukle.ps1 wrapper update [version]`; see the POSIX half for why this lives in the wrapper
# rather than in daukle, and why it is explicit rather than automatic. D-68.
function Invoke-WrapperUpdate($requested) {
    if ($requested) {
        $base = "https://github.com/daukle/daukle/releases/download/$requested"
    } else {
        $base = 'https://github.com/daukle/daukle/releases/latest/download'
    }

    $suffix = "update.$PID"
    $staged = @{
        'wrapper.toml' = Join-Path $scriptDir "wrapper.toml.$suffix"
        'daukle'       = Join-Path $scriptDir "daukle.$suffix"
        'daukle.ps1'   = Join-Path $scriptDir "daukle.ps1.$suffix"
    }
    try {
        foreach ($name in $staged.Keys) { Fetch "$base/$name" $staged[$name] }

        # All three or none. A half-replaced wrapper is a project whose scripts and pin disagree.
        foreach ($name in $staged.Keys) {
            if ((Get-Item -LiteralPath $staged[$name]).Length -eq 0) {
                Fail "$base returned an empty $name, so nothing was replaced."
            }
        }
        $newVersion = Read-Key (Get-Content -LiteralPath $staged['wrapper.toml']) 'version'
        if (-not $newVersion) {
            Fail "what $base/wrapper.toml returned names no version, so it is not a pin and nothing was replaced."
        }

        New-Item -ItemType Directory -Force -Path (Join-Path $scriptDir '.daukle') | Out-Null
        Move-Item -LiteralPath $staged['wrapper.toml'] -Destination $pinFile -Force
        Move-Item -LiteralPath $staged['daukle'] -Destination (Join-Path $scriptDir 'daukle') -Force
        Move-Item -LiteralPath $staged['daukle.ps1'] -Destination (Join-Path $scriptDir 'daukle.ps1') -Force
    } finally {
        foreach ($name in $staged.Keys) {
            if (Test-Path -LiteralPath $staged[$name]) {
                Remove-Item -LiteralPath $staged[$name] -Force
            }
        }
    }

    Write-Output "daukle wrapper: updated to $newVersion. Review the diff and commit all three files."
}

# Only the two-word form is intercepted, same as the POSIX half.
if ($args.Count -ge 2 -and $args[0] -eq 'wrapper' -and $args[1] -eq 'update') {
    if ($args.Count -gt 3) { Fail 'usage: .\daukle.ps1 wrapper update [version]' }
    Invoke-WrapperUpdate $(if ($args.Count -eq 3) { $args[2] } else { $null })
    exit 0
}

if (-not (Test-Path -LiteralPath $pinFile)) {
    Fail "no pin file at $pinFile. It is committed beside this script and names the version and its digests."
}
$pin = Get-Content -LiteralPath $pinFile

# PowerShell runs only on Windows here; the POSIX script covers the other two. The architecture
# spelling is daukle's own, from fr_lua_host_arch.
switch ($env:PROCESSOR_ARCHITECTURE) {
    'AMD64' { $hostArch = 'x86_64' }
    'ARM64' { $hostArch = 'aarch64' }
    'x86'   { $hostArch = 'x86' }
    default { Fail "unsupported architecture $($env:PROCESSOR_ARCHITECTURE)." }
}
$hostKey = "windows/$hostArch"

$version = Read-Key $pin 'version'
if (-not $version) { Fail "the pin file names no version: $pinFile" }

$section = @()
$inside = $false
foreach ($line in $pin) {
    if ($line -match '^\s*\[') {
        $inside = $line.Trim() -eq "[assets.`"$hostKey`"]"
        continue
    }
    if ($inside) { $section += $line }
}

if ($section.Count -eq 0) {
    $have = ($pin | ForEach-Object {
        if ($_ -match '^\s*\[assets\."(.*)"\]') { $Matches[1] }
    }) -join ', '
    Fail "no published daukle for $hostKey. This release covers: $have."
}

$asset = Read-Key $section 'asset'
$sha256 = Read-Key $section 'sha256'
if (-not $asset -or -not $sha256) { Fail "the entry for $hostKey is missing asset or sha256 in $pinFile" }

# The same branches as cache_root_dir in src/cache/cache.c, Windows half. The duplication is real
# and is the likeliest place for the wrapper to drift from core.
if ($env:DAUKLE_CACHE_DIR) {
    $cacheRoot = $env:DAUKLE_CACHE_DIR
} elseif ($env:LOCALAPPDATA) {
    $cacheRoot = Join-Path $env:LOCALAPPDATA 'daukle/cache'
} else {
    Fail 'no cache directory available: set DAUKLE_CACHE_DIR'
}

$binaryDir = Join-Path $cacheRoot "wrapper/$version/$hostKey"
$binary = Join-Path $binaryDir $asset

function Digest-Of($path) {
    # Built in since PowerShell 4, so Windows needs no fallback the way macOS does.
    (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# A cached file whose digest does not match is a corrupt cache and is replaced. A mismatch AFTER a
# fresh download is fatal: that is the pin doing its job.
if (-not (Test-Path -LiteralPath $binary) -or (Digest-Of $binary) -ne $sha256) {
    New-Item -ItemType Directory -Force -Path $binaryDir | Out-Null
    $partial = "$binary.partial.$PID"
    Fetch "https://github.com/daukle/daukle/releases/download/$version/$asset" $partial
    $actual = Digest-Of $partial
    if ($actual -ne $sha256) {
        Remove-Item -LiteralPath $partial -Force
        Fail "the downloaded $asset has sha256 $actual, and the pin says $sha256. Nothing was run."
    }
    # Named only once verified, so an interrupted run leaves nothing a later run would reuse.
    Move-Item -LiteralPath $partial -Destination $binary -Force
}

# Stop governs the wrapper's own failures and must not govern daukle's. Windows PowerShell 5.1
# wraps a native command's stderr in an ErrorRecord whenever the caller redirects it, and under
# Stop that record TERMINATES: `.\daukle.ps1 build 2>&1 > log.txt` killed the calling script on an
# ordinary daukle diagnostic. The wrapping is intrinsic to 5.1 and stays; what this restores is
# that it does not abort the caller, and that daukle's exit code still propagates. Measured.
$ErrorActionPreference = 'Continue'
& $binary @args
exit $LASTEXITCODE
