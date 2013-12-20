#!/bin/bash

MODULE_NAMES="hid_logitech_dj hid_logitech_hidpp hid_logitech_wtp"

if [[ `id -u` != 0 ]]
then
  echo "Must be run as root"
  exit 1
fi

for MODULE_NAME in ${MODULE_NAMES}
do
  TARGET=${MODULE_NAME}.ko

  INSTALL_PATH=/lib/modules/`uname -r`/extra

  INSTALLED_TARGET=`find ${INSTALL_PATH} -name ${TARGET}`
  if [[ -e ${INSTALLED_TARGET} ]]
  then
    echo "Removing installed module" ${INSTALLED_TARGET}
    rm ${INSTALLED_TARGET}
  fi
done

echo "depmod -a"
depmod -a
