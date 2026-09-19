<#
.SYNOPSIS
  Builds psdthumb.dll (the Explorer shell extension) and psdthumb.exe (CLI test tool) with MSVC.
.PARAMETER Config
  Release (default) or Debug.
.EXAMPLE
  .\build.ps1
  .\build.ps1 -Config Debug -Clean
#>
param(
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'build'
if ($Clean -and (Test-Path $out)) { Remove-Item -Recurse -Force $out }
foreach ($d in 'obj\dll', 'obj\cli') { New-Item -ItemType Directory -Force (Join-Path $out $d) | Out-Null }

# Locate vcvars64.bat through vswhere (any VS 2019/2022 edition or Build Tools with the C++ workload).
$vcvars = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsPath) { $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat' }
}
if (-not $vcvars -or -not (Test-Path $vcvars)) {
    throw 'MSVC not found. Install Visual Studio (or Build Tools) with the "Desktop development with C++" workload.'
}

$common = '/nologo /std:c++17 /W4 /EHsc /GS /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS'
if ($Config -eq 'Release') { $common += ' /O2 /MT /DNDEBUG' } else { $common += ' /Od /Zi /MTd /D_DEBUG' }
$libs = 'ole32.lib oleaut32.lib shlwapi.lib windowscodecs.lib gdi32.lib advapi32.lib user32.lib shell32.lib uuid.lib'
$dllSrc = 'src\dllmain.cpp src\PsdThumbnailProvider.cpp src\PsdDecoder.cpp src\StreamReader.cpp src\WicHelpers.cpp src\Log.cpp'
$cliSrc = 'tools\psdthumb_cli.cpp src\PsdDecoder.cpp src\StreamReader.cpp src\WicHelpers.cpp src\Log.cpp'

$script = @(
    '@echo off',
    "call `"$vcvars`" >nul 2>&1",
    'if errorlevel 1 exit /b 1',
    "cd /d `"$root`"",
    "cl $common /LD $dllSrc /Fo`"$out\obj\dll\\`" /Fd`"$out\obj\dll\\`" /Fe`"$out\psdthumb.dll`" /link /DEF:src\psdthumb.def /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT $libs",
    'if errorlevel 1 exit /b 1',
    "cl $common $cliSrc /Fo`"$out\obj\cli\\`" /Fd`"$out\obj\cli\\`" /Fe`"$out\psdthumb.exe`" /link /SUBSYSTEM:CONSOLE $libs",
    'if errorlevel 1 exit /b 1'
)
$cmdFile = Join-Path $out 'build.cmd'
Set-Content -Path $cmdFile -Value $script -Encoding ASCII
# Start-Process keeps cl's stderr chatter from becoming PowerShell error records. WaitForExit() (not -Wait)
# so we do not also wait for mspdbsrv.exe, which the compiler leaves running for a while.
$proc = Start-Process -FilePath cmd.exe -ArgumentList '/c', "`"$cmdFile`"" -PassThru -NoNewWindow
$null = $proc.Handle  # cache the handle, otherwise ExitCode is not populated after WaitForExit()
$proc.WaitForExit()
if ($proc.ExitCode -ne 0) { throw "Build failed (exit code $($proc.ExitCode))" }
Write-Host "Built $Config -> $out\psdthumb.dll, $out\psdthumb.exe"
