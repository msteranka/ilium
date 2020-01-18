#!/bin/bash
# Loads ilium
# Must be run as root

mod="ilium"
make
insmod $mod.ko
dev=$(grep $mod /proc/devices) # Find device listing in /proc/devices
major="${dev:0:3}" # Keep only first three characters
mknod /dev/$major c $short 0
dmesg | tail -20
