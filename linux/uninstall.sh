#!/bin/bash
# Uninstall Apple NCM driver and restore stock module
set -e

echo "=== Apple NCM Driver Uninstaller ==="
echo ""

echo "Unloading patched cdc_ncm module..."
sudo rmmod cdc_ncm 2>/dev/null || true

echo "Restoring stock cdc_ncm module..."
sudo modprobe cdc_ncm

echo ""
echo "=== Uninstallation Complete ==="
echo "Stock cdc_ncm module has been restored."


