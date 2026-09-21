<#
.SYNOPSIS
    Makes this build the calculator Windows opens, and puts everything back
    when you are done with it.

.DESCRIPTION
    Nothing here overwrites a Windows file. C:\Windows\System32\calc.exe is
    owned by TrustedInstaller and protected by Windows Resource Protection, so
    replacing it means taking ownership of a system binary and then watching
    SFC or the next cumulative update quietly restore it. Instead this uses the
    redirection Windows already provides:

      * Image File Execution Options, so launching calc.exe -- the Run box,
        "calc" in a terminal, anything that shells out to it -- starts this
        build instead. Needs administrator; it is one registry value per image
        name, and the original value is saved first.
      * The AppKey mapping, so a keyboard's Calculator key opens this build.
      * A Start menu shortcut named Calculator, so search finds it.
      * A handler for the calculator: protocol, which is how the Start tile and
        other apps ask for a calculator. The Store app outranks this while it
        is installed -- see -RemoveStoreApp.

    Every change is written to install-state.json in the install directory
    along with whatever was there before, and -Action Uninstall restores those
    previous values rather than just deleting keys.

.PARAMETER Action
    Install (default), Uninstall, or Status to report what is currently in
    place without changing anything.

.PARAMETER SourceExe
    The Calculator.exe to install. Defaults to Calculator.exe beside this
    script, then ..\out\Calculator.exe.

.PARAMETER InstallDir
    Where to copy it. Defaults to %LOCALAPPDATA%\Programs\CompactCalculator.

.PARAMETER RemoveStoreApp
    Also unregister the Microsoft Store Calculator for the current user. This
    is what frees the Start tile and the calculator: protocol. It is per-user
    and reversible -- Uninstall re-registers it from the package files, which
    are left on disk. Windows Update may reinstall it on its own schedule.

.PARAMETER SkipIfeo
    Skip the one part that needs administrator. Everything else still applies.

.EXAMPLE
    .\Install-Calculator.ps1

.EXAMPLE
    .\Install-Calculator.ps1 -Action Status

.EXAMPLE
    .\Install-Calculator.ps1 -Action Uninstall
#>

[CmdletBinding()]
param(
    [ValidateSet('Install', 'Uninstall', 'Status')]
    [string]$Action = 'Install',

    [string]$SourceExe,

    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\CompactCalculator'),

    [switch]$RemoveStoreApp,

    [switch]$SkipIfeo
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The image names Windows has shipped for the calculator. win32calc.exe is the
# classic one, still present on Server SKUs and on machines upgraded from 7.
$ImageNames = @('calc.exe', 'win32calc.exe')

$IfeoRoot     = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options'
$AppKeyPath   = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\AppKey\18'
$ProtocolPath = 'HKCU:\Software\Classes\calculator'
$StateName    = 'install-state.json'
$ShortcutName = 'Calculator.lnk'
$StorePackage = 'Microsoft.WindowsCalculator'

# ---------------------------------------------------------------- utilities

function Write-Step    { param([string]$Message) Write-Host "  $Message" }
function Write-Heading { param([string]$Message) Write-Host "`n$Message" -ForegroundColor Cyan }
function Write-Note    { param([string]$Message) Write-Host "  $Message" -ForegroundColor DarkGray }
function Write-Warn    { param([string]$Message) Write-Host "  $Message" -ForegroundColor Yellow }

function Test-Windows {
    # Windows PowerShell 5.1 has no $IsWindows, and there the answer is yes.
    if (Test-Path Variable:\IsWindows) { return $IsWindows }
    return $true
}

function Test-Administrator {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function ConvertTo-Hashtable {
    # State goes out as a hashtable and comes back from ConvertFrom-Json as a
    # PSCustomObject, so everything that reads it normalises first.
    param($Object)
    $result = @{}
    if ($null -eq $Object) { return $result }
    if ($Object -is [System.Collections.IDictionary]) {
        foreach ($key in $Object.Keys) { $result[[string]$key] = $Object[$key] }
        return $result
    }
    foreach ($property in $Object.PSObject.Properties) { $result[$property.Name] = $property.Value }
    return $result
}

function Get-Prop {
    # StrictMode makes a missing property an error, and state files written by
    # an older copy of this script may not have every key.
    param($Object, [string]$Name, $Default = $null)
    if ($null -eq $Object) { return $Default }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $Default
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    return $property.Value
}

function Get-RegistryValue {
    # $null both for "key is not there" and "value is not set", which is what
    # the restore path wants: either way there was nothing of ours before.
    param([string]$Path, [string]$Name)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    # The registry provider spells the default value "(default)"; the .NET API
    # underneath it spells the same thing as an empty name.
    $lookup = if ($Name -eq '(default)') { '' } else { $Name }
    $key = Get-Item -LiteralPath $Path
    $value = $key.GetValue($lookup, $null)
    if ($null -eq $value) { return $null }
    return [string]$value
}

function Set-RegistryValue {
    param([string]$Path, [string]$Name, [string]$Value)
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -Path $Path -Force | Out-Null
    }
    New-ItemProperty -LiteralPath $Path -Name $Name -Value $Value -PropertyType String -Force | Out-Null
}

function Restore-RegistryValue {
    # A $Previous of $null or empty means there was no value before we wrote
    # one, so the restore is a removal.
    param([string]$Path, [string]$Name, [string]$Previous, [switch]$RemoveEmptyKey)
    if (-not [string]::IsNullOrEmpty($Previous)) {
        Set-RegistryValue -Path $Path -Name $Name -Value $Previous
        return
    }
    if (-not (Test-Path -LiteralPath $Path)) { return }
    Remove-ItemProperty -LiteralPath $Path -Name $Name -ErrorAction SilentlyContinue
    if ($RemoveEmptyKey) {
        $key = Get-Item -LiteralPath $Path
        if ($key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0) {
            Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
        }
    }
}

function Get-StatePath { param([string]$Directory) Join-Path $Directory $StateName }

function Read-State {
    param([string]$Directory)
    $path = Get-StatePath $Directory
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}

function Write-State {
    param([string]$Directory, $State)
    $State | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Get-StatePath $Directory) -Encoding UTF8
}

function Get-StartMenuShortcutPath {
    Join-Path ([Environment]::GetFolderPath('Programs')) $ShortcutName
}

function Stop-InstalledCalculator {
    # Copying over or deleting a running image fails with a sharing violation.
    param([string]$Target)
    Get-Process -Name 'Calculator' -ErrorAction SilentlyContinue | ForEach-Object {
        # A process we have no right to open is not one of ours.
        $path = $null
        try { $path = $_.Path } catch { }
        if ($path -ne $Target) { return }
        Write-Step "Closing the running copy (pid $($_.Id))."
        $_.CloseMainWindow() | Out-Null
        Start-Sleep -Milliseconds 700
        $_.Refresh()
        if (-not $_.HasExited) { $_.Kill() }
    }
}

function Get-StoreCalculator {
    # The Appx module is a Windows PowerShell component; on PowerShell 7 it is
    # reached through the compatibility layer, which is not always there.
    try {
        return Get-AppxPackage -Name $StorePackage -ErrorAction Stop | Select-Object -First 1
    } catch {
        Write-Warn "Could not query Store packages: $($_.Exception.Message)"
        Write-Warn 'Run this from Windows PowerShell 5.1 if you need the Store app handled.'
        return $null
    }
}

function Resolve-SourceExe {
    param([string]$Requested)
    if ($Requested) {
        if (-not (Test-Path -LiteralPath $Requested)) { throw "No such file: $Requested" }
        return (Resolve-Path -LiteralPath $Requested).Path
    }
    $here = Split-Path -Parent $PSCommandPath
    foreach ($candidate in @(
        (Join-Path $here 'Calculator.exe'),
        (Join-Path $here '..\out\Calculator.exe'))) {
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw 'Could not find Calculator.exe beside this script or in ..\out. Pass -SourceExe.'
}

function Assert-Executable {
    # A truncated or half-downloaded file would install fine and then fail to
    # start, by which point calc.exe is already pointed at it.
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $header = New-Object byte[] 2
        $read = $stream.Read($header, 0, 2)
        if ($read -ne 2 -or $header[0] -ne 0x4D -or $header[1] -ne 0x5A) {
            throw "$Path is not a Windows executable (no MZ header)."
        }
    } finally {
        $stream.Dispose()
    }
}

# ------------------------------------------------------------------ install

function Invoke-Install {
    $source = Resolve-SourceExe -Requested $SourceExe
    Assert-Executable -Path $source

    Write-Heading 'Installing'

    $existing = Read-State -Directory $InstallDir
    if ($existing) {
        Write-Note 'An earlier install is recorded here; the originals it saved are kept.'
    }

    if (-not (Test-Path -LiteralPath $InstallDir)) {
        New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    }
    $target = Join-Path $InstallDir 'Calculator.exe'

    Stop-InstalledCalculator -Target $target
    Copy-Item -LiteralPath $source -Destination $target -Force
    # Mark-of-the-web on a downloaded file makes SmartScreen prompt every time.
    Unblock-File -LiteralPath $target -ErrorAction SilentlyContinue
    Write-Step "Installed to $target"

    # Whatever a previous run saved as the original is still the original --
    # re-reading the registry now would capture our own settings and make
    # Uninstall "restore" them.
    $ifeoPrevious = ConvertTo-Hashtable (Get-Prop $existing 'Ifeo')
    $state = [ordered]@{
        Version         = 1
        InstalledAt     = (Get-Date).ToString('o')
        TargetExe       = $target
        Ifeo            = $ifeoPrevious
        AppKeyPrevious  = (Get-Prop $existing 'AppKeyPrevious')
        AppKeyApplied   = $false
        ProtocolApplied = $false
        ShortcutPath    = $null
        StoreApp        = (Get-Prop $existing 'StoreApp')
    }

    try {

    # --- calc.exe redirection (administrator) ------------------------------
    if ($SkipIfeo) {
        Write-Note 'Skipping the calc.exe redirection (-SkipIfeo).'
    } elseif (-not (Test-Administrator)) {
        Write-Warn 'Not running as administrator, so calc.exe still opens the Windows calculator.'
        Write-Warn 'Re-run this from an elevated PowerShell to redirect it, or pass -SkipIfeo.'
    } else {
        foreach ($image in $ImageNames) {
            $path = Join-Path $IfeoRoot $image
            if (-not $state.Ifeo.ContainsKey($image)) {
                $state.Ifeo[$image] = Get-RegistryValue -Path $path -Name 'Debugger'
            }
            Set-RegistryValue -Path $path -Name 'Debugger' -Value ('"{0}"' -f $target)
            Write-Step "$image now opens this build."
        }
    }

    # --- the keyboard Calculator key ---------------------------------------
    if ($null -eq $state.AppKeyPrevious) {
        $state.AppKeyPrevious = Get-RegistryValue -Path $AppKeyPath -Name 'ShellExecute'
    }
    Set-RegistryValue -Path $AppKeyPath -Name 'ShellExecute' -Value $target
    $state.AppKeyApplied = $true
    Write-Step 'The keyboard Calculator key now opens this build.'

    # --- calculator: protocol ----------------------------------------------
    Set-RegistryValue -Path $ProtocolPath -Name '(default)' -Value 'URL:Calculator Protocol'
    Set-RegistryValue -Path $ProtocolPath -Name 'URL Protocol' -Value ''
    Set-RegistryValue -Path (Join-Path $ProtocolPath 'shell\open\command') -Name '(default)' -Value ('"{0}"' -f $target)
    $state.ProtocolApplied = $true
    Write-Step 'Registered a handler for the calculator: protocol.'

    # --- Start menu ---------------------------------------------------------
    $shortcut = Get-StartMenuShortcutPath
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($shortcut)
    $link.TargetPath       = $target
    $link.WorkingDirectory = $InstallDir
    $link.Description      = 'Calculator'
    $link.Save()
    $state.ShortcutPath = $shortcut
    Write-Step 'Added a Start menu entry named Calculator.'

    # --- the Store app ------------------------------------------------------
    if ($RemoveStoreApp) {
        $package = Get-StoreCalculator
        if (-not $package) {
            Write-Note 'The Store Calculator is not installed for this user.'
        } else {
            $state.StoreApp = [ordered]@{
                PackageFullName = $package.PackageFullName
                InstallLocation = $package.InstallLocation
            }
            try {
                Remove-AppxPackage -Package $package.PackageFullName
                Write-Step "Unregistered $($package.PackageFullName) for this user."
            } catch {
                $state.StoreApp = $null
                Write-Warn "Could not unregister the Store Calculator: $($_.Exception.Message)"
            }
        }
    } else {
        Write-Note 'The Store Calculator is untouched, so it keeps the Start tile and the'
        Write-Note 'calculator: protocol. Pass -RemoveStoreApp to hand both over.'
    }

    } finally {
        # Whatever did land is recorded, so a failed run is still undoable.
        Write-State -Directory $InstallDir -State $state
    }

    Write-Heading 'Done'
    Write-Step 'Sign out and back in, or restart Explorer, for the Calculator key to take effect.'
    Write-Step '.\Install-Calculator.ps1 -Action Uninstall  puts it all back.'
}

# ---------------------------------------------------------------- uninstall

function Invoke-Uninstall {
    $state = Read-State -Directory $InstallDir
    if (-not $state) {
        throw "No install recorded in $InstallDir. Pass -InstallDir if it went somewhere else."
    }

    Write-Heading 'Removing'

    # --- calc.exe redirection ----------------------------------------------
    $ifeo = ConvertTo-Hashtable (Get-Prop $state 'Ifeo')
    $ifeoPending = $ifeo.Count -gt 0
    if ($ifeoPending) {
        if (-not (Test-Administrator)) {
            Write-Warn 'Not running as administrator, so calc.exe is still redirected.'
            Write-Warn 'Re-run this elevated to finish; everything else below is done.'
        } else {
            foreach ($image in @($ifeo.Keys)) {
                Restore-RegistryValue -Path (Join-Path $IfeoRoot $image) -Name 'Debugger' `
                    -Previous ([string]$ifeo[$image]) -RemoveEmptyKey
                Write-Step "$image restored."
            }
            $ifeo = @{}
            $ifeoPending = $false
        }
    }

    # --- the keyboard Calculator key ---------------------------------------
    if (Get-Prop $state 'AppKeyApplied' $false) {
        Restore-RegistryValue -Path $AppKeyPath -Name 'ShellExecute' `
            -Previous ([string](Get-Prop $state 'AppKeyPrevious')) -RemoveEmptyKey
        Write-Step 'Calculator key restored.'
    }

    # --- calculator: protocol ----------------------------------------------
    if ((Get-Prop $state 'ProtocolApplied' $false) -and (Test-Path -LiteralPath $ProtocolPath)) {
        Remove-Item -LiteralPath $ProtocolPath -Recurse -Force -ErrorAction SilentlyContinue
        Write-Step 'calculator: protocol handler removed.'
    }

    # --- Start menu ---------------------------------------------------------
    $shortcut = Get-Prop $state 'ShortcutPath'
    if ($shortcut -and (Test-Path -LiteralPath $shortcut)) {
        Remove-Item -LiteralPath $shortcut -Force
        Write-Step 'Start menu entry removed.'
    }

    # --- the Store app ------------------------------------------------------
    $storeApp = Get-Prop $state 'StoreApp'
    if ($storeApp) {
        $location = Get-Prop $storeApp 'InstallLocation'
        $manifest = if ($location) { Join-Path $location 'AppxManifest.xml' } else { $null }
        if ($manifest -and (Test-Path -LiteralPath $manifest)) {
            try {
                Add-AppxPackage -Register $manifest -DisableDevelopmentMode
                Write-Step 'Store Calculator re-registered.'
            } catch {
                Write-Warn "Could not re-register the Store Calculator: $($_.Exception.Message)"
                Write-Warn 'Reinstall it from the Microsoft Store (search for Windows Calculator).'
            }
        } else {
            Write-Warn 'The Store Calculator package files are gone; reinstall it from the Microsoft Store.'
        }
    }

    # --- the files ----------------------------------------------------------
    $target = Get-Prop $state 'TargetExe'
    if ($target -and (Test-Path -LiteralPath $target)) {
        Stop-InstalledCalculator -Target $target
        Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue
    }

    if ($ifeoPending) {
        # Keep the record, trimmed to what is left, so the elevated pass still
        # knows what to restore and does not redo the parts already undone.
        $remaining = [ordered]@{
            Version         = 1
            InstalledAt     = (Get-Prop $state 'InstalledAt')
            TargetExe       = $target
            Ifeo            = $ifeo
            AppKeyPrevious  = $null
            AppKeyApplied   = $false
            ProtocolApplied = $false
            ShortcutPath    = $null
            StoreApp        = $null
        }
        Write-State -Directory $InstallDir -State $remaining
    } else {
        Remove-Item -LiteralPath (Get-StatePath $InstallDir) -Force -ErrorAction SilentlyContinue
        if ((Test-Path -LiteralPath $InstallDir) -and
            -not (Get-ChildItem -LiteralPath $InstallDir -Force)) {
            Remove-Item -LiteralPath $InstallDir -Force -ErrorAction SilentlyContinue
        }
        Write-Heading 'Done'
    }
}

# ------------------------------------------------------------------- status

function Invoke-Status {
    Write-Heading 'Status'
    Write-Step ('{0,-20}{1}' -f 'Administrator:', (Test-Administrator))
    Write-Step ('{0,-20}{1}' -f 'Install directory:', $InstallDir)

    $state = Read-State -Directory $InstallDir
    $installedAt = if ($state) { Get-Prop $state 'InstalledAt' } else { 'none' }
    Write-Step ('{0,-20}{1}' -f 'Recorded install:', $installedAt)

    $target = if ($state) { Get-Prop $state 'TargetExe' } else { $null }
    if ($target) {
        $suffix = if (Test-Path -LiteralPath $target) { '' } else { '  (missing)' }
        Write-Step ('{0,-20}{1}{2}' -f 'Executable:', $target, $suffix)
    }

    foreach ($image in $ImageNames) {
        $value = Get-RegistryValue -Path (Join-Path $IfeoRoot $image) -Name 'Debugger'
        if (-not $value) { $value = 'not redirected' }
        Write-Step ('{0,-20}{1}' -f ($image + ':'), $value)
    }

    $appKey = Get-RegistryValue -Path $AppKeyPath -Name 'ShellExecute'
    if (-not $appKey) { $appKey = 'Windows default' }
    Write-Step ('{0,-20}{1}' -f 'Calculator key:', $appKey)

    $protocol = Get-RegistryValue -Path (Join-Path $ProtocolPath 'shell\open\command') -Name '(default)'
    if (-not $protocol) { $protocol = 'Windows default' }
    Write-Step ('{0,-20}{1}' -f 'calculator: opens', $protocol)

    $shortcut = Get-StartMenuShortcutPath
    $shortcutState = if (Test-Path -LiteralPath $shortcut) { $shortcut } else { 'none' }
    Write-Step ('{0,-20}{1}' -f 'Start menu entry:', $shortcutState)

    $package = Get-StoreCalculator
    $packageState = if ($package) { $package.PackageFullName } else { 'not installed for this user' }
    Write-Step ('{0,-20}{1}' -f 'Store Calculator:', $packageState)
    Write-Host ''
}

# --------------------------------------------------------------------- main

if (-not (Test-Windows)) {
    Write-Error 'This script configures Windows and only runs there.'
    exit 1
}

try {
    switch ($Action) {
        'Install'   { Invoke-Install }
        'Uninstall' { Invoke-Uninstall }
        'Status'    { Invoke-Status }
    }
} catch {
    Write-Host ''
    Write-Error $_.Exception.Message
    exit 1
}
