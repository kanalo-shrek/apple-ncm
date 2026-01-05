# Self-Sign Driver for Secure Boot
# Run as Administrator

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Self-Signing NCM Driver" -ForegroundColor Cyan
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

$certName = "NCM Driver Self-Signed"
$certPath = Join-Path $driverPath "NCMDriver.cer"
$pfxPath = Join-Path $driverPath "NCMDriver.pfx"
$password = "NCMDriver2025"

# Check required files exist
$sysFile = Join-Path $driverPath "UsbNcmSample.sys"
$infFile = Join-Path $driverPath "UsbNcmSample.inf"

if (-not (Test-Path $sysFile)) {
    Write-Host "ERROR: UsbNcmSample.sys not found!" -ForegroundColor Red
    Write-Host "Run rebuild-driver.ps1 first." -ForegroundColor Yellow
    pause
    exit 1
}

if (-not (Test-Path $infFile)) {
    Write-Host "ERROR: UsbNcmSample.inf not found!" -ForegroundColor Red
    Write-Host "Run rebuild-driver.ps1 first." -ForegroundColor Yellow
    pause
    exit 1
}

# Check if certificate already exists
$existingCert = Get-ChildItem -Path Cert:\LocalMachine\My | Where-Object { $_.Subject -like "*$certName*" }

if ($existingCert) {
    Write-Host "Step 1: Using existing certificate..." -ForegroundColor Yellow
    $cert = $existingCert[0]
    Write-Host "  Thumbprint: $($cert.Thumbprint)" -ForegroundColor Green
} else {
    Write-Host "Step 1: Creating self-signed certificate..." -ForegroundColor Yellow
    
    $cert = New-SelfSignedCertificate `
        -Type Custom `
        -Subject "CN=$certName" `
        -KeyUsage DigitalSignature `
        -FriendlyName "$certName" `
        -CertStoreLocation "Cert:\LocalMachine\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}") `
        -KeyExportPolicy Exportable `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -NotAfter (Get-Date).AddYears(5)
    
    Write-Host "  Certificate created: $($cert.Thumbprint)" -ForegroundColor Green
}
Write-Host ""

# Export certificate
Write-Host "Step 2: Exporting certificate..." -ForegroundColor Yellow
Export-Certificate -Cert $cert -FilePath $certPath -Type CERT | Out-Null
$securePassword = ConvertTo-SecureString -String $password -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $securePassword | Out-Null
Write-Host "  Exported to: $certPath" -ForegroundColor Green
Write-Host ""

# Install certificate to Trusted Root and Trusted Publishers
Write-Host "Step 3: Installing certificate to trusted stores..." -ForegroundColor Yellow

$rootStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root", "LocalMachine")
$rootStore.Open("ReadWrite")
$rootCert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($certPath)
$rootStore.Add($rootCert)
$rootStore.Close()
Write-Host "  Installed to Trusted Root Certification Authorities" -ForegroundColor Green

$pubStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("TrustedPublisher", "LocalMachine")
$pubStore.Open("ReadWrite")
$pubCert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($certPath)
$pubStore.Add($pubCert)
$pubStore.Close()
Write-Host "  Installed to Trusted Publishers" -ForegroundColor Green
Write-Host ""

# Find tools
$wdkBin = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0"
$signtool = Join-Path $wdkBin "x64\signtool.exe"
$inf2cat = Join-Path $wdkBin "x86\inf2cat.exe"

if (-not (Test-Path $signtool)) {
    # Try to find it
    $found = Get-ChildItem -Path "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $signtool = $found.FullName }
}

if (-not (Test-Path $inf2cat)) {
    $found = Get-ChildItem -Path "C:\Program Files (x86)\Windows Kits\10\bin\*\x86\inf2cat.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $inf2cat = $found.FullName }
}

if (-not (Test-Path $signtool)) {
    Write-Host "ERROR: signtool.exe not found!" -ForegroundColor Red
    pause
    exit 1
}

if (-not (Test-Path $inf2cat)) {
    Write-Host "ERROR: inf2cat.exe not found!" -ForegroundColor Red
    pause
    exit 1
}

Write-Host "Step 4: Tools found" -ForegroundColor Yellow
Write-Host "  signtool: $signtool" -ForegroundColor Gray
Write-Host "  inf2cat:  $inf2cat" -ForegroundColor Gray
Write-Host ""

# Generate catalog file
Write-Host "Step 5: Generating catalog file..." -ForegroundColor Yellow
$catFile = Join-Path $driverPath "UsbNcmSample.cat"

# Remove old catalog if exists
if (Test-Path $catFile) {
    Remove-Item $catFile -Force
}

# Run inf2cat to generate catalog
Push-Location $driverPath
$inf2catOutput = & $inf2cat /driver:. /os:10_x64 2>&1
Pop-Location

if (Test-Path $catFile) {
    Write-Host "  Catalog generated: UsbNcmSample.cat" -ForegroundColor Green
} else {
    Write-Host "  WARNING: Catalog generation may have issues" -ForegroundColor Yellow
    Write-Host "  Output: $inf2catOutput" -ForegroundColor Gray
}
Write-Host ""

# Sign the driver files
Write-Host "Step 6: Signing driver files..." -ForegroundColor Yellow

Write-Host "  Signing UsbNcmSample.sys..." -ForegroundColor Yellow
$signResult = & $signtool sign /v /fd sha256 /f $pfxPath /p $password /tr http://timestamp.digicert.com /td sha256 $sysFile 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "    OK" -ForegroundColor Green
} else {
    Write-Host "    Failed: $signResult" -ForegroundColor Red
}

if (Test-Path $catFile) {
    Write-Host "  Signing UsbNcmSample.cat..." -ForegroundColor Yellow
    $signResult = & $signtool sign /v /fd sha256 /f $pfxPath /p $password /tr http://timestamp.digicert.com /td sha256 $catFile 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "    OK" -ForegroundColor Green
    } else {
        Write-Host "    Failed: $signResult" -ForegroundColor Red
    }
}
Write-Host ""

# Verify signatures
Write-Host "Step 7: Verifying signatures..." -ForegroundColor Yellow
Write-Host "  UsbNcmSample.sys:" -ForegroundColor Yellow
& $signtool verify /pa /v $sysFile 2>&1 | Select-String -Pattern "Successfully|SignTool Error|Number of files" | ForEach-Object { Write-Host "    $_" -ForegroundColor Gray }

if (Test-Path $catFile) {
    Write-Host "  UsbNcmSample.cat:" -ForegroundColor Yellow
    & $signtool verify /pa /v $catFile 2>&1 | Select-String -Pattern "Successfully|SignTool Error|Number of files" | ForEach-Object { Write-Host "    $_" -ForegroundColor Gray }
}
Write-Host ""

Write-Host "=====================================" -ForegroundColor Green
Write-Host "Self-Signing Complete!" -ForegroundColor Green
Write-Host "=====================================" -ForegroundColor Green
Write-Host ""
Write-Host "Next step: .\install-driver.ps1" -ForegroundColor White
Write-Host ""

pause
