# CDC_NCM with Apple Changes

Out-of-tree Linux kernel module that patches `cdc_ncm` to support Apple's weird USB NCM implementation.


## Prerequisites

```bash
# Ubuntu/Debian
sudo apt install build-essential linux-headers-$(uname -r)

# Fedora/RHEL
sudo dnf install kernel-devel kernel-headers gcc make
```

## Quick Install

```bash
./install.sh
```

This will:
1. Build the kernel module
2. Unload the stock `cdc_ncm`
3. Load the patched module

## Manual Build

```bash
make clean
make
sudo rmmod cdc_ncm 2>/dev/null || true
sudo insmod cdc_ncm.ko
```

## Usage

1. Run the installer on your Linux machine
2. Connect your Mac via USB-C cable
3. Check for new network interface:
   ```bash
   ip link
   # Should show something like: enx660a69900d8e
   ```

4. Configure IP on Linux:
   ```bash
   sudo ip addr add 192.168.100.2/24 dev enx660a69900d8e
   sudo ip link set enx660a69900d8e up
   ```

5. Configure IP on Mac:
   ```bash
   sudo ifconfig en4 192.168.100.1/24 up
   # or en5, check: networksetup -listallhardwareports
   ```

6. Test connectivity:
   ```bash
   ping 192.168.100.1
   ```

## Uninstall

```bash
./uninstall.sh
```

This restores the stock `cdc_ncm` module.

## DKMS Installation (Persistent)

For the module to survive kernel updates:

```bash
sudo cp -r . /usr/src/apple-ncm-1.0
sudo dkms add -m apple-ncm -v 1.0
sudo dkms build -m apple-ncm -v 1.0
sudo dkms install -m apple-ncm -v 1.0
```

## Troubleshooting

### Check if module is loaded
```bash
lsmod | grep cdc_ncm
```

### View driver messages
```bash
dmesg | grep -i 'apple\|ncm' | tail -20
```

### Check USB device detection
```bash
lsusb | grep Apple
# Should show: 05ac:1905 Apple, Inc. Mac
```

### Verify interface MAC address
The interface name includes the MAC address. Apple devices use MACs like:
- `66:0a:69:90:0d:XX`

## Technical Details

### Device Information
- Vendor ID: `0x05ac` (Apple)
- Product ID: `0x1905`
- USB Class: CDC (Communications Device Class)
- Subclass: NCM (Network Control Model)

### Kernel Changes
- Added `CDC_NCM_FLAG_NO_INTERRUPT` quirk flag
- Created `apple_ncm_info` driver_info without `FLAG_LINK_INTR`
- Modified `cdc_ncm_bind_common()` to skip status endpoint check
- Added `cdc_ncm_apple_bind()` that forces link up

### Performance
With USB 3.2 Gen 2x2, expect:
- Theoretical: 10 Gbps
- Practical: ~5-8 Gbps (depends on cable quality)

Test with iperf3:
```bash
# Mac (server)
iperf3 -s

# Linux (client)
iperf3 -c 192.168.100.1
```

## License

This module inherits the dual BSD/GPL license from the original `cdc_ncm` driver.
