obj-$(CONFIG_HID_LOGITECH_DJ)    += hid-logitech-dj.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

install: hid-logitech-dj.ko
	/bin/bash install.sh

uninstall:
	/bin/bash restore.sh

clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean

