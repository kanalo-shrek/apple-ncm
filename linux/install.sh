#!/bin/bash
# Install Apple NCM driver module
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Apple NCM Driver Installer ==="
echo ""

# Check for kernel headers
if [ ! -d "/lib/modules/$(uname -r)/build" ]; then
    echo "ERROR: Kernel headers not found. Install with:"
    echo "  sudo apt install linux-headers-$(uname -r)"
    exit 1
fi

echo "Building module for kernel $(uname -r)..."
make clean
make

echo ""
echo "Unloading stock cdc_ncm module..."
sudo rmmod cdc_ncm 2>/dev/null || true

echo "Loading patched cdc_ncm module..."
sudo insmod cdc_ncm.ko

echo ""
echo "=== Installation Complete ==="
echo ""
echo "The Apple NCM driver is now loaded."
echo "Connect your Mac via USB-C and check 'ip link' for a new interface."
echo ""
echo "To configure networking:"
echo "  sudo ip addr add 192.168.100.2/24 dev <interface>"
echo "  sudo ip link set <interface> up"
echo ""
echo "On Mac:"
echo "  sudo ifconfig en4 192.168.100.1/24 up"
echo ""
echo "Check dmesg for driver messages:"
echo "  dmesg | grep -i 'apple\|ncm' | tail -20"


