#!/bin/bash

declare -A mod_names
mod_names["hid-logitech-wtp"]="wtp-touch"
mod_names["hid-logitech-dj"]="logitech-djreceiver"

BLK_DRIVER=hid-generic

UDEV_RULES_PATH=/etc/udev/rules.d
SYSTEMD_SERVICES_PATH=/etc/systemd/system

TEST=$1

if [ ! x"$TEST" == x"" ]
then
	UDEV_PATH=.
	RULES_DIR=.
fi

UDEV_RULE=${UDEV_RULES_PATH}/01-${BLK_DRIVER}-blacklist.rules
SYSTEMD_SERVICE=${SYSTEMD_SERVICES_PATH}/${BLK_DRIVER}-blacklist@.service

if [ x"$TEST" == x"" ]
then
	if [[ `id -u` != 0 ]]
	then
		echo "Must be run as root"
		exit 1
	fi
fi

SED_EXPR="s/MODULE_ALIAS(\"hid:b\(.*\)g\(.*\)v0000\(.*\)p0000\(.*\)\");/KERNEL==\"\1:\3:\4.*\", TAG+=\"systemd\", ENV{SYSTEMD_WANTS}+=\"${BLK_DRIVER}-blacklist@%kGOOD_DRIVER_NAME.service\"/"

cat > ${UDEV_RULE} <<EOF
ACTION!="add|change", GOTO="hid_generic_end"
DRIVER!="hid-generic", GOTO="hid_generic_end"

EOF

for mod in *.mod.c
do
	DRIVER=${mod/.mod.c/}
	GOOD_NAME=${mod_names["${DRIVER}"]}
	grep MODULE_ALIAS ${mod} | grep "g\*" | \
		sed "${SED_EXPR/GOOD_DRIVER_NAME/${GOOD_NAME}}" | \
		grep -v MODULE_ALIAS | \
		sort \
			>> ${UDEV_RULE}
done

cat >> ${UDEV_RULE} <<EOF

LABEL="hid_generic_end"
EOF

cat > ${SYSTEMD_SERVICE} <<EOF
[Unit]
Description=blacklist for hid-generic devices

[Service]
Type=simple
ExecStart=/bin/bash -c "\\
	echo \`expr match %i '\(....:....:....\.....\).*'\` > /sys/bus/hid/drivers/\`expr match %p '\(.*\)-blacklist'\`/unbind ;\\
	echo \`expr match %i '\(....:....:....\.....\).*'\` > /sys/bus/hid/drivers/\`expr match %i '....:....:....\.....\(.*\)'\`/bind ;\\
	"

#echo ${DEVICE} > ${HID_DRV_PATH}/${BLK_DRIVER}/unbind
#echo ${DEVICE} > ${HID_DRV_PATH}/${GOOD_DRIVER}/bind
EOF
