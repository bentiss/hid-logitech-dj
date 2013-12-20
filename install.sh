#!/bin/bash

if [[ `id -u` != 0 ]]
then
  echo "Must be run as root"
  exit 1
fi

WORKING_DIR=$(pwd)
PRIMARY_TARGET=hid-logitech-dj.ko
TARGETS="hid-logitech-dj.ko hid-logitech-hidpp.ko hid-logitech-wtp.ko"

INSTALL_PATH=/lib/modules/`uname -r`/kernel/drivers/hid

# check if the modules are compiled
if [[ ! -e $WORKING_DIR/${PRIMARY_TARGET} ]]
then
  echo "please run make before install."
  echo "Aborting"
  exit 1
fi

# backup shipped modules
cd $INSTALL_PATH
for TARGET in TARGETS
do
	if [[ ! -e ${TARGET}.orig ]]
	then
	  mv ${TARGET} ${TARGET}.orig
	fi


	cd $WORKING_DIR
	echo ${TARGET} "->" ${INSTALL_PATH}/${TARGET}
	cp ${TARGET} ${INSTALL_PATH}/${TARGET}
done

echo "depmod -a"
depmod -a

echo "update-initramfs -u"
update-initramfs -u
