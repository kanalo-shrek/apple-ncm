# Rebuild NCM Driver
# Run from Developer PowerShell for VS

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "NCM Driver Rebuild" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host ""

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$solutionDir = (Get-Item $scriptDir).Parent.FullName
$driverOutputDir = Join-Path $solutionDir "x64\Debug"

# Create output directory if it doesn't exist
if (-not (Test-Path $driverOutputDir)) {
    New-Item -ItemType Directory -Path $driverOutputDir -Force | Out-Null
}

Write-Host "Solution directory: $solutionDir" -ForegroundColor Gray
Write-Host "Driver output: $driverOutputDir" -ForegroundColor Gray
Write-Host ""

# Check for msbuild
Write-Host "Checking for MSBuild..." -ForegroundColor Yellow
$msbuild = Get-Command msbuild -ErrorAction SilentlyContinue
if (-not $msbuild) {
    Write-Host "ERROR: MSBuild not found!" -ForegroundColor Red
    Write-Host "Please run this from Developer PowerShell for VS" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "To open Developer PowerShell:" -ForegroundColor White
    Write-Host "  1. Open Visual Studio" -ForegroundColor Gray
    Write-Host "  2. Tools -> Command Line -> Developer PowerShell" -ForegroundColor Gray
    pause
    exit 1
}
Write-Host "  Found: $($msbuild.Source)" -ForegroundColor Green
Write-Host ""

# Rebuild only the host project (function project has KMDF version issues we don't need)
Write-Host "Rebuilding host driver..." -ForegroundColor Yellow
Push-Location $solutionDir

$buildOutput = msbuild projects\host\host.vcxproj /t:Rebuild /p:Configuration=Debug /p:Platform=x64 2>&1

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build FAILED!" -ForegroundColor Red
    Write-Host ""
    Write-Host $buildOutput
    Pop-Location
    pause
    exit 1
}

Write-Host "Build SUCCESSFUL!" -ForegroundColor Green
Pop-Location
Write-Host ""

# Copy files from the actual build output folder to our driver folder
Write-Host "Copying files to driver folder..." -ForegroundColor Yellow
# MSBuild outputs to: projects\host\x64\Debug\host\
$buildOutputFolder = Join-Path $solutionDir "projects\host\x64\Debug\host"
Write-Host "  Build output: $buildOutputFolder" -ForegroundColor Gray

if (Test-Path "$buildOutputFolder\UsbNcmSample.sys") {
    Copy-Item "$buildOutputFolder\UsbNcmSample.sys" $driverOutputDir -Force
    Write-Host "  Copied UsbNcmSample.sys" -ForegroundColor Green
} else {
    Write-Host "  WARNING: UsbNcmSample.sys not found in build output!" -ForegroundColor Yellow
}
if (Test-Path "$buildOutputFolder\UsbNcmSample.inf") {
    Copy-Item "$buildOutputFolder\UsbNcmSample.inf" $driverOutputDir -Force
    Write-Host "  Copied UsbNcmSample.inf" -ForegroundColor Green
} else {
    Write-Host "  WARNING: UsbNcmSample.inf not found in build output!" -ForegroundColor Yellow
}
# Also copy the catalog file (MSBuild generates it as lowercase)
if (Test-Path "$buildOutputFolder\usbncmsample.cat") {
    Copy-Item "$buildOutputFolder\usbncmsample.cat" "$driverOutputDir\UsbNcmSample.cat" -Force
    Write-Host "  Copied usbncmsample.cat -> UsbNcmSample.cat" -ForegroundColor Green
} else {
    Write-Host "  WARNING: usbncmsample.cat not found in build output!" -ForegroundColor Yellow
}
Write-Host ""

# Show output files
Write-Host "Output files in $driverOutputDir :" -ForegroundColor Yellow
Get-ChildItem $driverOutputDir -Filter "UsbNcmSample.*" | ForEach-Object {
    Write-Host "  $($_.Name) - $($_.LastWriteTime)" -ForegroundColor White
}
Write-Host ""

Write-Host "=====================================" -ForegroundColor Green
Write-Host "Build Complete!" -ForegroundColor Green
Write-Host "=====================================" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor White
Write-Host "1. Run: .\create-and-sign.ps1  (as Admin)" -ForegroundColor White
Write-Host "2. Run: .\install-driver.ps1  (as Admin)" -ForegroundColor White
Write-Host ""

pause
