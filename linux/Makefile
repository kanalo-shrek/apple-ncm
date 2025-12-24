# Apple NCM Driver - Out-of-tree kernel module
# Supports Apple Mac USB NCM devices (VID 0x05ac, PID 0x1905)
# that lack the required interrupt endpoint

obj-m += cdc_ncm.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	sudo rmmod cdc_ncm 2>/dev/null || true
	sudo insmod cdc_ncm.ko
	@echo "Module loaded. Check 'dmesg | tail' for status."

uninstall:
	sudo rmmod cdc_ncm 2>/dev/null || true
	sudo modprobe cdc_ncm
	@echo "Stock cdc_ncm module restored."

.PHONY: all clean install uninstall


