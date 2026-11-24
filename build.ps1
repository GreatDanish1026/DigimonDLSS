# Builds bin\DigimonDLSS.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# Usage: .\build.ps1 [-Install] [-Package] [-GameDir <folder of Digimon Story Time Stranger.exe>]
#   -Install  copies the add-on and the DLSS / FSR runtimes into the game folder (game closed), plus
#             ReShade 6.8.0 with add-on support as dxgi.dll if the folder has none and
#             $reshadeSource exists. The game folder: -GameDir, else the first line of gamedir.txt
#             (not in git).
#   -Package  builds bin\release\DigimonDLSS-<version>.zip.
param(
    [switch]$Install,
    [switch]$Package,
    [string]$GameDir = ""
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not $GameDir -and (Test-Path "$root\gamedir.txt")) { $GameDir = (Get-Content "$root\gamedir.txt" -TotalCount 1).Trim() }
if ($Install -and -not $GameDir) { throw "Pass -GameDir or put the game folder in gamedir.txt" }
$exe = "Digimon Story Time Stranger.exe"
$name = "DigimonDLSS"
# Any ReShade 6.8.0 add-on build on this PC; only used when the game folder has none.
$reshadeSource = "E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2\dxgi.dll"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
# vcvars64.bat runs vswhere by name; PATH entries with parentheses in odd places (a Steam tool's
# folder) break cmd's parsing of vcvars, so only the Windows folders are kept for the build.
$env:PATH = "$(Split-Path $vswhere);$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$addon = "$root\bin\$name.addon64"

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0.
$cmd = @(
    "call `"$vcvars`" >nul",
    "fxc /nologo /T cs_5_0 /E main /O3 /Vn kMotionCs /Fh `"$obj\motion_cs.h`" `"$root\src\motion.hlsl`" >nul",
    "fxc /nologo /T cs_5_0 /E main /O3 /Vn kCopyCs /Fh `"$obj\copy_cs.h`" `"$root\src\copy.hlsl`" >nul",
    "fxc /nologo /T cs_5_0 /E main /O3 /Vn kRcasCs /Fh `"$obj\rcas_cs.h`" `"$root\src\rcas.hlsl`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$obj`" /I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" /I `"$root\third_party\dlss\include`" " +
     "/I `"$root\third_party\fidelityfx\api\include`" /I `"$root\third_party\fidelityfx\upscalers\include`" " +
     "/Fo`"$obj\\`" `"$root\src\main.cpp`" `"$root\src\upscaler.cpp`" `"$root\src\dlss.cpp`" " +
     "`"$root\src\bridge.cpp`" `"$root\src\fsr.cpp`" " +
     "user32.lib advapi32.lib ole32.lib d3d11.lib d3d12.lib dxgi.lib `"$root\third_party\dlss\lib\nvsdk_ngx_s.lib`" " +
     "/link /DLL /OUT:`"$addon`" /PDB:`"$obj\$name.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "Built $addon"

if ($Install) {
    if (-not (Test-Path -LiteralPath "$GameDir\$exe")) { throw "$exe not found in $GameDir (pass -GameDir)" }
    if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) { throw "Close the game first." }
    if (-not (Test-Path -LiteralPath "$GameDir\dxgi.dll")) {
        Copy-Item -LiteralPath $reshadeSource "$GameDir\dxgi.dll"
        Write-Host "Installed ReShade 6.8.0 as dxgi.dll"
    }
    if (-not (Test-Path -LiteralPath "$GameDir\ReShade.ini")) {
        Set-Content -LiteralPath "$GameDir\ReShade.ini" -Encoding ASCII -Value @(
            "[$name]", "CaptureHotkey=1", "",
            "[ADDON]", "DisabledAddons=Generic Depth,Effect Runtime Sync", "",
            "[GENERAL]", "EffectSearchPaths=.\", "TextureSearchPaths=.\", "",
            "[INPUT]", "KeyOverlay=36,0,0,0")
        Write-Host "Wrote ReShade.ini (overlay on Home, capture on F10)"
    }
    # NVIDIA's signed DLSS runtime (310.9.16, from RE2DLSS-DX11; not in git).
    $ngx = "$root\third_party\bin\nvngx_dlss.dll"
    if (-not (Test-Path -LiteralPath $ngx)) { throw "nvngx_dlss.dll not found in third_party\bin" }
    # AMD's signed FSR runtime (FSR SDK 2.4.0, from RE2DLSS-DX11; not in git), next to the exe.
    $ffx = "$root\third_party\fidelityfx\bin"
    $ffxFiles = "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll"
    foreach ($f in $ffxFiles) { if (-not (Test-Path -LiteralPath "$ffx\$f")) { throw "$f not found in $ffx" } }
    Copy-Item -LiteralPath $addon, $ngx $GameDir -Force
    foreach ($f in $ffxFiles) { Copy-Item -LiteralPath "$ffx\$f" $GameDir -Force }
    Write-Host "Installed to $GameDir"
}

if ($Package) {
    $ngx = "$root\third_party\bin\nvngx_dlss.dll"
    $ffx = "$root\third_party\fidelityfx\bin"
    $ffxFiles = "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll"
    if (-not (Test-Path -LiteralPath $ngx)) { throw "nvngx_dlss.dll not found in third_party\bin" }
    foreach ($f in $ffxFiles) { if (-not (Test-Path -LiteralPath "$ffx\$f")) { throw "$f not found in $ffx" } }
    $version = [regex]::Match((Get-Content "$root\src\version.h" -Raw), 'RD_VERSION "([^"]+)"').Groups[1].Value
    $stage = "$root\bin\release\$name-$version"
    $mod = "$stage\$name"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force "$mod\$name-licenses" | Out-Null

    # Files that go into the game folder (where the game's exe is).
    Copy-Item -LiteralPath $addon, $ngx $mod
    foreach ($f in $ffxFiles) { Copy-Item -LiteralPath "$ffx\$f" $mod }
    Copy-Item "$root\README.md" "$mod\$name-README.md"
    Copy-Item "$root\THIRD_PARTY_NOTICES.txt" "$mod\$name-THIRD_PARTY_NOTICES.txt"
    Copy-Item "$root\LICENSE" "$mod\$name-licenses\$name-LICENSE.txt"
    Copy-Item "$root\third_party\dlss\LICENSE.txt" "$mod\$name-licenses\NVIDIA-DLSS-LICENSE.txt"
    Copy-Item "$root\third_party\reshade\LICENSE.md" "$mod\$name-licenses\ReShade-LICENSE.md"
    Copy-Item "$root\third_party\fidelityfx\license.md" "$mod\$name-licenses\AMD-FSR-LICENSE.txt"

    # Zip with forward-slash entry names (Compress-Archive in PowerShell 5.1 writes backslashes).
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zip = "$root\bin\release\$name-$version.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        Get-ChildItem $stage -Recurse -File | ForEach-Object {
            $entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
    Write-Host "Packaged $zip"
}
