# apple-ncm

USB-C networking between Mac and PC. 5+ Gbps over a single cable. 

## Why is it needed?

When you connect a Mac to a PC via USB-C, the Mac presents itself as a USB network adapter (CDC NCM). But it doesn't work out of the box because Apple's implementation is non-standard—missing an endpoint that Linux and Windows expect.

This project provides patches to fix that.

## Platform support

| Platform | Status |
|----------|--------|
| [Linux](./linux/) | ✅ Working |
| Windows | 🚧 Coming soon |

## The story

→ [Read the full blog post](https://arewecooked.dev/blog/mac-to-pc-usb-c-networking)

## License

GPL-2.0 (Linux patch must match kernel license)