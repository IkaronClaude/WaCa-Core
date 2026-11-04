# Builds this repository's release zip: dist\<Repo>-<version>.zip, laid out from the game's folder down
# (Wrc\Binaries\Win64\ue4ss\Mods\<Mod>\...) - extract it into the game's folder ("Warrior Cats Clans of the Forest").
# The same script is in every WaCa repository; it packs every mod folder under Mods\ (WaCa Core also ships the shared
# Lua API, Mods\shared\WaCa, and its native DLL, native\out\main.dll - build it first with native\build.bat).
# Each mod gets an enabled.txt: UE4SS turns it on without editing mods.txt. Content paks (dist\content\*_P.*, built
# by content\build_content.ps1) go to Wrc\Content\Paks\~mods.
param([string]$Version = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$repo = Split-Path $root -Leaf
$mods = Join-Path $root "Mods"

# the version: the main mod's VERSION line (local VERSION = "1.2.3"), unless given
if (-not $Version) {
    foreach ($main in Get-ChildItem $mods -Directory | ForEach-Object { Join-Path $_.FullName "Scripts\main.lua" }) {
        if (Test-Path $main) {
            $m = Select-String -Path $main -Pattern 'local VERSION = "([^"]+)"' | Select-Object -First 1
            if ($m) { $Version = $m.Matches[0].Groups[1].Value; break }
        }
    }
}
if (-not $Version) { throw "no version (pass -Version)" }

$name = "$repo-$Version"
$out = Join-Path $root "dist\$name"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
$target = Join-Path $out "Wrc\Binaries\Win64\ue4ss\Mods"
New-Item -ItemType Directory -Force $target | Out-Null

foreach ($mod in Get-ChildItem $mods -Directory) {
    Copy-Item $mod.FullName $target -Recurse
    if ($mod.Name -ne "shared") {
        $dest = Join-Path $target $mod.Name
        Set-Content (Join-Path $dest "enabled.txt") "" -Encoding ascii
        Copy-Item (Join-Path $root "LICENSE") (Join-Path $dest "LICENSE.txt")
        if (Test-Path (Join-Path $root "README.md")) { Copy-Item (Join-Path $root "README.md") (Join-Path $dest "README.md") }
        # the README's links (docs\..., examples\...) work in the zip too: those folders beside it (an example mod in
        # there is only a copy to look at - UE4SS loads mods from Mods\ itself)
        if (Test-Path (Join-Path $root "CONTRIBUTING.md")) { Copy-Item (Join-Path $root "CONTRIBUTING.md") $dest }
        foreach ($extra in "docs", "examples") {
            if (Test-Path (Join-Path $root $extra)) { Copy-Item (Join-Path $root $extra) $dest -Recurse }
        }
    }
}

# a native half (WaCa Core): its DLL in the mod's dlls folder
$dll = Join-Path $root "native\out\main.dll"
if (Test-Path (Join-Path $root "native")) {
    if (-not (Test-Path $dll)) { throw "native\out\main.dll is missing - run native\build.bat first" }
    $first = Get-ChildItem $mods -Directory | Where-Object { $_.Name -ne "shared" } | Select-Object -First 1
    $dlls = Join-Path $target "$($first.Name)\dlls"
    New-Item -ItemType Directory -Force $dlls | Out-Null
    Copy-Item $dll (Join-Path $dlls "main.dll")
}

# content paks, when built
$content = Join-Path $root "dist\content"
if (Test-Path $content) {
    $paks = Get-ChildItem $content -File | Where-Object { $_.Name -match "_P\.(pak|utoc|ucas)$" }
    if ($paks) {
        $dest = Join-Path $out "Wrc\Content\Paks\~mods"
        New-Item -ItemType Directory -Force $dest | Out-Null
        $paks | Copy-Item -Destination $dest
    }
}

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
# (Windows' own tar: Compress-Archive of PowerShell 5.1 writes backslashes in the zip's paths, which some tools
# extract as file names with backslashes in them)
Push-Location $out
try { & (Join-Path $env:SystemRoot "System32\tar.exe") -a -cf $zip Wrc; if ($LASTEXITCODE) { throw "tar failed" } } finally { Pop-Location }
Write-Host "built $zip"
