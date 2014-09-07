obj-m	+= hid-logitech-dj.o
obj-m	+= hid-logitech-hidpp.o
obj-m	+= hid-logitech-wtp.o
obj-m	+= hid-logitech-m560.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

install: hid-logitech-dj.ko hid-logitech-hidpp.ko hid-logitech-wtp.ko
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules_install
	/bin/bash install.sh

uninstall:
	/bin/bash restore.sh

clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean

