param([string]$DevkitProRoot = 'C:\devkitPro')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$bashPath = Join-Path $DevkitProRoot 'msys2\usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bashPath)) {
    throw "MSYS2 bash not found: $bashPath"
}
& $bashPath --noprofile --norc (Join-Path $PSScriptRoot 'build.sh') $DevkitProRoot (Join-Path $repoRoot 'native')
if ($LASTEXITCODE -ne 0) { throw "Build failed (code $LASTEXITCODE)." }
Write-Output "NRO : $(Join-Path $repoRoot 'native\PokeMMO.nro')"
