# NCM Driver for Windows (Linux Port)

This is a Windows port of the Linux NCM (Network Control Model) driver functionality, based on the [Microsoft Network-Adapter-Class-Extension sample](https://github.com/Microsoft/Network-Adapter-Class-Extension).

## Overview

This project provides Windows driver implementations for USB NCM (Network Control Model) devices, enabling network connectivity over USB interfaces. The implementation includes both host and function drivers that follow the NetAdapterCx framework.


## Development Scripts

The **[scripts/](scripts)** folder contains PowerShell scripts for development workflow:

- **create-and-sign.ps1** - Creates and digitally signs driver packages
- **install-driver.ps1** - Installs the built driver for testing
- **rebuild-driver.ps1** - Rebuilds the driver from source

## Building

1. Ensure you have Visual Studio with WDK (Windows Driver Kit) installed
2. Open `usbncm.sln` in Visual Studio
3. Build the solution

For detailed build instructions and technical documentation, refer to the [original Microsoft sample](https://github.com/Microsoft/Network-Adapter-Class-Extension).

## Usage

Use the provided PowerShell scripts in the `scripts/` folder to build, sign, and install the drivers for testing and development.

## License

This project follows the same license as the original Microsoft Network-Adapter-Class-Extension sample.
