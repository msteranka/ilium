#!/bin/bash
# Unloads ilium
# Must be run as root

mod="ilium"
rmmod $mod
rm -f /dev/$mod
dmesg | tail -20
