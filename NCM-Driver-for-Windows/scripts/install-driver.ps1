# Install NCM Driver
# Run as Administrator

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "NCM Driver Install" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host ""

# Check if running as Administrator
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "ERROR: This script must be run as Administrator!" -ForegroundColor Red
    pause
    exit 1
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$solutionDir = (Get-Item $scriptDir).Parent.FullName
$driverPath = Join-Path $solutionDir "x64\Debug"

Write-Host "Driver path: $driverPath" -ForegroundColor Gray
Write-Host ""

$infFile = Join-Path $driverPath "UsbNcmSample.inf"
$sysFile = Join-Path $driverPath "UsbNcmSample.sys"
$catFile = Join-Path $driverPath "UsbNcmSample.cat"
$devcon = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe"

# Verify driver files exist
Write-Host "Checking driver files..." -ForegroundColor Yellow
$filesOk = $true
if (-not (Test-Path $sysFile)) { Write-Host "  MISSING: UsbNcmSample.sys" -ForegroundColor Red; $filesOk = $false }
else { Write-Host "  OK: UsbNcmSample.sys" -ForegroundColor Green }
if (-not (Test-Path $infFile)) { Write-Host "  MISSING: UsbNcmSample.inf" -ForegroundColor Red; $filesOk = $false }
else { Write-Host "  OK: UsbNcmSample.inf" -ForegroundColor Green }
if (-not (Test-Path $catFile)) { Write-Host "  MISSING: UsbNcmSample.cat" -ForegroundColor Red; $filesOk = $false }
else { Write-Host "  OK: UsbNcmSample.cat" -ForegroundColor Green }

if (-not $filesOk) {
    Write-Host ""
    Write-Host "ERROR: Driver files missing. Run rebuild-driver.ps1 and create-and-sign.ps1 first." -ForegroundColor Red
    pause
    exit 1
}
Write-Host ""

# Step 1: DISABLE devices first to safely unload any active driver
Write-Host "Step 1: Disabling Apple NCM devices (if connected)..." -ForegroundColor Yellow
$appleDevices = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "*VID_05AC&PID_1905*" }

if ($appleDevices) {
    foreach ($dev in $appleDevices) {
        Write-Host "  Disabling: $($dev.InstanceId)" -ForegroundColor Gray
        try {
            Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
        } catch {
            Write-Host "    (already disabled or not stoppable)" -ForegroundColor DarkGray
        }
    }
    Start-Sleep -Seconds 2  # Give time for driver to unload
    Write-Host "  Done" -ForegroundColor Green
} else {
    Write-Host "  No Apple devices connected (safe to proceed)" -ForegroundColor Gray
}
Write-Host ""

# Step 2: Remove ALL old NCM sample drivers from store
Write-Host "Step 2: Removing old drivers from store..." -ForegroundColor Yellow
$oemDrivers = pnputil /enum-drivers 2>&1 | Out-String

# Find all matching drivers (case insensitive)
$driverMatches = [regex]::Matches($oemDrivers, "Published Name\s*:\s*(oem\d+\.inf)[\s\S]*?Original Name\s*:\s*([^\r\n]+)", [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)

$removed = 0
foreach ($match in $driverMatches) {
    $oemName = $match.Groups[1].Value.Trim()
    $origName = $match.Groups[2].Value.Trim()
    
    if ($origName -like "*UsbNcm*" -or $origName -like "*usbncm*") {
        Write-Host "  Removing: $oemName ($origName)" -ForegroundColor Gray
        $result = pnputil /delete-driver $oemName /uninstall /force 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host "    Removed successfully" -ForegroundColor Green
            $removed++
        } else {
            Write-Host "    Force removal attempted" -ForegroundColor Yellow
        }
    }
}

if ($removed -eq 0) {
    Write-Host "  No existing NCM sample drivers found" -ForegroundColor Gray
}
Write-Host ""

# Step 3: Add new driver to store
Write-Host "Step 3: Adding driver to store..." -ForegroundColor Yellow
$result = pnputil /add-driver $infFile /install 2>&1
Write-Host "  Result: $result" -ForegroundColor Gray
if ($LASTEXITCODE -eq 0) {
    Write-Host "  Driver added successfully" -ForegroundColor Green
} else {
    Write-Host "  Warning: Driver add returned code $LASTEXITCODE" -ForegroundColor Yellow
}
Write-Host ""

# Step 4: Re-enable devices 
Write-Host "Step 4: Re-enabling Apple NCM devices..." -ForegroundColor Yellow
$appleDevices = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "*VID_05AC&PID_1905*" }

if ($appleDevices) {
    foreach ($dev in $appleDevices) {
        Write-Host "  Enabling: $($dev.InstanceId)" -ForegroundColor Gray
        try {
            Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
        } catch {
            Write-Host "    (enable failed, may need replug)" -ForegroundColor Yellow
        }
    }
    Start-Sleep -Seconds 2
    Write-Host "  Done" -ForegroundColor Green
} else {
    Write-Host "  No Apple devices to re-enable" -ForegroundColor Gray
}
Write-Host ""

# Step 5: Force update using devcon if available
Write-Host "Step 5: Forcing driver update on COMPOSITE PARENT device..." -ForegroundColor Yellow
if (Test-Path $devcon) {
    # Target the COMPOSITE PARENT device (no MI_xx suffix)
    # This should bypass USBCCGP and give us all interfaces
    $result = & $devcon update $infFile "USB\VID_05AC&PID_1905" 2>&1
    Write-Host "  Result: $result" -ForegroundColor Gray
    Write-Host "  Driver update forced for composite device" -ForegroundColor Green
} else {
    Write-Host "  devcon.exe not found, triggering rescan..." -ForegroundColor Yellow
    pnputil /scan-devices 2>&1 | Out-Null
}
Write-Host ""

# Step 6: Show current status
Write-Host "Step 6: Current device status..." -ForegroundColor Yellow
Start-Sleep -Seconds 1
$devices = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "*VID_05AC&PID_1905*" }
if ($devices) {
    foreach ($dev in $devices) {
        $color = switch ($dev.Status) {
            "OK" { "Green" }
            "Error" { "Red" }
            "Degraded" { "Yellow" }
            default { "Yellow" }
        }
        Write-Host "  $($dev.InstanceId)" -ForegroundColor White
        Write-Host "    Status: $($dev.Status)" -ForegroundColor $color
        Write-Host "    Class: $($dev.Class)" -ForegroundColor Gray
        
        # Show which driver is loaded
        $driverInfo = Get-PnpDeviceProperty -InstanceId $dev.InstanceId -ErrorAction SilentlyContinue | 
            Where-Object { $_.KeyName -eq "DEVPKEY_Device_DriverInfPath" }
        if ($driverInfo -and $driverInfo.Data) {
            $isOurs = $driverInfo.Data -like "*usbncmsample*"
            Write-Host "    Driver: $($driverInfo.Data)" -ForegroundColor $(if ($isOurs) { "Green" } else { "Gray" })
        }
    }
} else {
    Write-Host "  No Apple NCM devices found" -ForegroundColor Gray
}
Write-Host ""

Write-Host "=====================================" -ForegroundColor Green
Write-Host "Installation Complete" -ForegroundColor Green
Write-Host "=====================================" -ForegroundColor Green
Write-Host ""
Write-Host "TIP: If you see 'Error' status, check DebugView for details." -ForegroundColor Cyan
Write-Host "     Unplug and replug the device if status doesn't update." -ForegroundColor Cyan
Write-Host ""

pause
