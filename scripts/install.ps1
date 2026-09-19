<#
.SYNOPSIS
  Registers or removes the PSD thumbnail provider.
.DESCRIPTION
  By default installs for the current user only (HKCU, no admin prompt) into %LOCALAPPDATA%\psd-thumbnails.
  -System installs for all users into %ProgramFiles%\psd-thumbnails (run from an elevated prompt).
.EXAMPLE
  .\scripts\install.ps1                 # install for the current user
  .\scripts\install.ps1 -Uninstall      # remove
  .\scripts\install.ps1 -ClearCache     # also reset the Explorer thumbnail cache (restarts Explorer)
#>
param(
    [switch]$Uninstall,
    [switch]$System,
    [switch]$ClearCache,
    [string]$Dll
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Dll) { $Dll = Join-Path $root 'build\psdthumb.dll' }

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($System -and -not $isAdmin) { throw 'Use an elevated (administrator) PowerShell for -System.' }

$installDir = if ($System) { Join-Path $env:ProgramFiles 'psd-thumbnails' } else { Join-Path $env:LOCALAPPDATA 'psd-thumbnails' }
$target = Join-Path $installDir 'psdthumb.dll'
$regsvrArgs = if ($System) { @() } else { @('/n', '/i:user') }

function Stop-ThumbnailHosts {
    # Explorer runs thumbnail providers inside COM surrogate processes (dllhost.exe).
    # Stop the ones that have our DLL loaded so the file can be replaced or deleted.
    foreach ($p in Get-Process -Name dllhost -ErrorAction SilentlyContinue) {
        $loaded = $false
        try { $loaded = [bool]($p.Modules | Where-Object { $_.ModuleName -ieq 'psdthumb.dll' }) } catch { }
        if ($loaded) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            Write-Host "Stopped thumbnail host dllhost.exe (PID $($p.Id))"
        }
    }
}

function Invoke-Regsvr {
    param([string[]]$Arguments)
    $p = Start-Process -FilePath regsvr32.exe -ArgumentList (@('/s') + $Arguments) -Wait -PassThru -NoNewWindow
    return $p.ExitCode
}

if ($Uninstall) {
    if (Test-Path $target) {
        Stop-ThumbnailHosts
        $rc = Invoke-Regsvr (@('/u') + $regsvrArgs + @("`"$target`""))
        if ($rc -ne 0) { Write-Warning "regsvr32 /u returned $rc" }
        Remove-Item -Force $target -ErrorAction SilentlyContinue
        Remove-Item -Force -Recurse $installDir -ErrorAction SilentlyContinue
        Write-Host "Removed $target"
    } else {
        Write-Host "Nothing installed at $target"
    }
} else {
    if (-not (Test-Path $Dll)) { throw "DLL not found: $Dll (run .\build.ps1 first)" }
    Stop-ThumbnailHosts
    New-Item -ItemType Directory -Force $installDir | Out-Null
    Copy-Item -Force $Dll $target
    $rc = Invoke-Regsvr ($regsvrArgs + @("`"$target`""))
    if ($rc -ne 0) { throw "regsvr32 failed with exit code $rc" }
    $scope = if ($System) { 'all users' } else { 'the current user' }
    Write-Host "Registered $target for $scope."
    Write-Host 'Open a folder with .psd files in Explorer (Large icons view). Already-open folders may need F5.'
}

if ($ClearCache) {
    Write-Host 'Clearing the Explorer thumbnail cache (Explorer restarts)...'
    Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    Remove-Item "$env:LOCALAPPDATA\Microsoft\Windows\Explorer\thumbcache_*.db" -Force -ErrorAction SilentlyContinue
    Start-Process explorer.exe
}
