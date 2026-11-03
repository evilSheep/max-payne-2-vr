[CmdletBinding()]
param([string]$InputDirectory, [switch]$Restore)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Steam Frame: SteamVR's default binding sends right B, X and Y (and left D-pad
# Up, Left and Right) to the same legacy button, so the game can't tell them
# apart. This copies that default for Max Payne 2 and moves only right B and
# left D-pad Left to the unused legacy Axis4, which MP2VR reads as the weapon
# wheel button. Every other route stays exactly as SteamVR ships it.

if (Get-Process -Name maxpayne2,vrserver,vrcompositor -ErrorAction SilentlyContinue) {
    throw 'Close Max Payne 2 and exit SteamVR, then run this again.'
}
if (!$InputDirectory) {
    $registryPath = Join-Path $env:LOCALAPPDATA 'openvr/openvrpaths.vrpath'
    if (!(Test-Path -LiteralPath $registryPath)) { throw 'Start SteamVR once, exit it, then run this again.' }
    $runtimePaths = (Get-Content -LiteralPath $registryPath -Raw | ConvertFrom-Json).runtime
    foreach ($runtimePath in $runtimePaths) {
        $candidate = Join-Path $runtimePath 'drivers/frame_controller/resources/input'
        if (Test-Path -LiteralPath (Join-Path $candidate 'frame_controller_profile.json')) {
            $InputDirectory = $candidate
            break
        }
    }
    if (!$InputDirectory) { throw 'Could not find the Steam Frame controller files in the registered SteamVR runtime. Update SteamVR, then run this again.' }
}
$inputRoot = (Resolve-Path -LiteralPath $InputDirectory).Path
$profilePath = Join-Path $inputRoot 'frame_controller_profile.json'
$stockPath = Join-Path $inputRoot 'legacy_bindings_frame_controller.json'
$editions = @(
    [pscustomobject]@{app_key='steam.app.12150'; binding_url='mp2vr_frame_weapon_wheel.json'},
    [pscustomobject]@{app_key='steam.app.210410'; binding_url='mp2vr_frame_weapon_wheel_de.json'}
)
$backupPath = Join-Path $inputRoot 'frame_controller_profile.json.mp2vr-original'
$customPaths = @($editions | ForEach-Object { Join-Path $inputRoot $_.binding_url })
foreach ($path in (@($profilePath,$stockPath,$backupPath) + $customPaths)) {
    if ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing redirected binding file: $path"
    }
}
function Write-BindingJson([string]$Path, $Document) {
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Path)) -ne $inputRoot) { throw 'Binding path escaped the Frame input directory.' }
    $temporary = $Path + ".mp2vr-$PID.tmp"
    try {
        [IO.File]::WriteAllText($temporary, ($Document | ConvertTo-Json -Depth 100) + "`r`n", [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
}
$profile = Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json
if ($profile.controller_type -ne 'frame_controller') { throw 'Unexpected controller profile; expected Steam Frame (frame_controller).' }
$defaultsProperty = $profile.PSObject.Properties['default_bindings']
$defaults = if ($defaultsProperty) { @($defaultsProperty.Value) } else { @() }
function Test-OwnedOverride($Entry) {
    foreach ($edition in $editions) {
        if ($Entry.app_key -eq $edition.app_key -and $Entry.binding_url -eq $edition.binding_url) { return $true }
    }
    return $false
}
if ($Restore) {
    $existing = @($defaults | Where-Object { Test-OwnedOverride $_ })
    if ($existing.Count -eq 0) { Write-Output 'The MP2VR Frame weapon wheel binding is already off.'; exit 0 }
    $remaining = @($defaults | Where-Object { !(Test-OwnedOverride $_) })
    if ($remaining.Count -eq 0) { $profile.PSObject.Properties.Remove('default_bindings') }
    else { $profile.default_bindings = $remaining }
    Write-BindingJson $profilePath $profile
    Write-Output 'Removed the MP2VR Frame binding for standard/German Steam editions. Restart SteamVR. Other bindings were preserved.'
    exit 0
}
foreach ($edition in $editions) {
    $existing = @($defaults | Where-Object { $_.app_key -eq $edition.app_key })
    if ($existing.Count -gt 1 -or ($existing.Count -eq 1 -and $existing[0].binding_url -ne $edition.binding_url)) {
        throw "A different Max Payne 2 driver-default binding already exists for $($edition.app_key). It was preserved; remove that override before applying this fix."
    }
}
$binding = Get-Content -LiteralPath $stockPath -Raw | ConvertFrom-Json
if ($binding.controller_type -ne 'frame_controller') { throw 'Unexpected Frame legacy binding format.' }
$legacyProperty = $binding.bindings.PSObject.Properties['/actions/legacy']
if (!$legacyProperty) { throw 'The Frame legacy binding has no /actions/legacy set.' }
$legacy = $legacyProperty.Value
foreach ($wheel in @(
    [pscustomobject]@{hand='right'; component='b'},
    [pscustomobject]@{hand='left'; component='dpad_left'}
)) {
    $prefix = "/actions/legacy/in/$($wheel.hand)_"
    # Axis4 must be free on this hand, or the wheel would share a button again.
    foreach ($source in $legacy.sources) {
        foreach ($route in $source.inputs.PSObject.Properties) {
            if ($route.Value.output -like "${prefix}axis4_*" -and $source.path -ne "/user/hand/$($wheel.hand)/input/$($wheel.component)") {
                throw "SteamVR's Frame binding already uses $($wheel.hand) Axis4; review it before applying this fix."
            }
        }
    }
    $sources = @($legacy.sources | Where-Object { $_.path -eq "/user/hand/$($wheel.hand)/input/$($wheel.component)" })
    if ($sources.Count -ne 1 -or $sources[0].mode -ne 'button') {
        throw "Expected exactly one $($wheel.hand) $($wheel.component) button source."
    }
    $click = $sources[0].inputs.PSObject.Properties['click']
    if (!$click -or $click.Value.output -notin @("${prefix}applicationmenu_press", "${prefix}axis4_press")) {
        throw "Unexpected $($wheel.hand) $($wheel.component) route; review it before applying this fix."
    }
    $click.Value.output = "${prefix}axis4_press"
    $touch = $sources[0].inputs.PSObject.Properties['touch']
    if ($touch) {
        if ($touch.Value.output -notin @("${prefix}applicationmenu_touch", "${prefix}axis4_touch")) {
            throw "Unexpected $($wheel.hand) $($wheel.component) touch route; review it before applying this fix."
        }
        $touch.Value.output = "${prefix}axis4_touch"
    }
    # The eject (magazine release) button must still exist on this hand.
    $eject = @($legacy.sources | Where-Object {
        $_.inputs.PSObject.Properties['click'] -and $_.inputs.click.output -eq "${prefix}applicationmenu_press"
    })
    if ($eject.Count -eq 0) { throw "No $($wheel.hand) eject button would remain; review the binding before applying this fix." }
}
$binding.name = 'Max Payne 2 VR - Frame weapon wheel'
$profile | Add-Member -NotePropertyName default_bindings -NotePropertyValue (@($defaults | Where-Object { $_.app_key -notin $editions.app_key }) + $editions) -Force

# Write the bindings first so a failure can't leave the profile pointing at a
# missing file. Keep a byte-for-byte backup of the original profile.
if (!(Test-Path -LiteralPath $backupPath)) { Copy-Item -LiteralPath $profilePath -Destination $backupPath }
foreach ($edition in $editions) {
    $binding | Add-Member -NotePropertyName app_key -NotePropertyValue $edition.app_key -Force
    Write-BindingJson (Join-Path $inputRoot $edition.binding_url) $binding
}
Write-BindingJson $profilePath $profile
Write-Output 'Frame weapon wheel binding installed for Max Payne 2. Restart SteamVR, then start the game from Steam.'
Write-Output "Standard Steam (12150) and German Steam (210410). Profile: $profilePath"
Write-Output 'Right B opens the right hand weapon wheel; left D-pad Left opens the left hand one.'
Write-Output 'X / Y and D-pad Up / Right still eject. Everything else is unchanged.'
Write-Output 'If you selected a custom SteamVR binding for Max Payne 2 before, select Default again.'
Write-Output 'Run this again after a SteamVR update if the wheel buttons stop working.'
