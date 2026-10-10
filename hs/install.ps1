# Installs the Half Sword half of the passthrough into the game folder; -Remove takes out exactly what it added.
#   powershell -ExecutionPolicy Bypass -File hs\install.ps1 [-Remove]
# Adds: HSPassthrough.addon64 (Win64\), MCPassthrough.fx (Win64\reshade-shaders\Shaders\), and
# MCPassthrough@MCPassthrough.fx at the front of the active preset's Techniques. Needs ReShade with add-on support
# already installed (dxgi.dll in Win64\). Close the game first.
param([switch]$Remove, [string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Half Sword")
$ErrorActionPreference = "Stop"
$win64 = Join-Path $Game "HalfswordUE5\Binaries\Win64"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$shaders = Join-Path $win64 "reshade-shaders\Shaders"
$technique = "MCPassthrough@MCPassthrough.fx"
if (Get-Process HalfSwordUE5-Win64-Shipping -ErrorAction SilentlyContinue) { throw "Half Sword is running: close it first" }
if (-not (Test-Path (Join-Path $win64 "dxgi.dll"))) { throw "ReShade (dxgi.dll) not found in $win64" }

# the preset ReShade.ini points at
$preset = Join-Path $win64 "ReShadePreset.ini"
$ini = Join-Path $win64 "ReShade.ini"
if (Test-Path $ini) {
	$line = Select-String -Path $ini -Pattern '^PresetPath=(.+)$' | Select-Object -First 1
	if ($line) { $preset = [IO.Path]::GetFullPath((Join-Path $win64 $line.Matches[0].Groups[1].Value)) }
}

function Set-Techniques([string]$path, [scriptblock]$edit) {
	if (-not (Test-Path $path)) { if ($Remove) { return }; Set-Content $path "Techniques=" -Encoding ascii }
	$lines = @(Get-Content $path)
	$i = [Array]::FindIndex($lines, [Predicate[string]] { param($l) $l -like "Techniques=*" })
	if ($i -lt 0) { $lines = @("Techniques=") + $lines; $i = 0 }
	$list = @($lines[$i].Substring(11).Split(",") | Where-Object { $_ -and $_ -ne $technique })
	$lines[$i] = "Techniques=" + ((& $edit $list) -join ",")
	Set-Content $path $lines -Encoding ascii
}

if ($Remove) {
	foreach ($f in @((Join-Path $win64 "HSPassthrough.addon64"), (Join-Path $win64 "HSPassthrough.log"), (Join-Path $shaders "MCPassthrough.fx"))) {
		if (Test-Path $f) { Remove-Item $f; "removed $f" }
	}
	Set-Techniques $preset { param($l) $l }
	"removed $technique from $preset"
	return
}
Copy-Item (Join-Path $here "build\HSPassthrough.addon64") $win64 -Force
New-Item -ItemType Directory -Force $shaders | Out-Null
Copy-Item (Join-Path $here "shaders\MCPassthrough.fx") $shaders -Force
if (-not (Test-Path (Join-Path $shaders "ReShade.fxh"))) { Copy-Item (Join-Path $here "third_party\ReShade.fxh"), (Join-Path $here "third_party\ReShadeUI.fxh") $shaders }
Set-Techniques $preset { param($l) @($technique) + $l }
"installed into $win64 (preset: $preset)"
