# Builds MagicAdapter.exe with the MSYS2 UCRT64 g++ toolchain.
#   build.ps1                     -> MagicAdapter.exe next to this script
#   build.ps1 -Out x.exe -UiTest  -> a test build (used only for taking screenshots of the window)
param([string]$Out = "$PSScriptRoot\MagicAdapter.exe", [switch]$UiTest)
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$Out = [System.IO.Path]::GetFullPath($Out)
Push-Location "$PSScriptRoot\src"      # relative paths only: windres dislikes spaces in paths
try {
    windres app.rc -O coff -o app.res
    if ($LASTEXITCODE -ne 0) { "Resource build failed"; exit 1 }
    $defs = @(); if ($UiTest) { $defs += '-DUITEST' }
    g++ -Os -s -static -mwindows -fno-exceptions -fno-rtti -fno-math-errno -Wall @defs `
        main.cpp app.res -o $Out `
        -lhid -lsetupapi -lshell32 -ladvapi32 -lgdi32 -luser32
    if ($LASTEXITCODE -eq 0) { "Built $Out" } else { "Build failed"; exit 1 }
} finally {
    Remove-Item app.res -ErrorAction SilentlyContinue
    Pop-Location
}
