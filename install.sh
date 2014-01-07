#!/bin/sh

apt-get install bluetooth bluez-utils
apt-get install libglib2.0-dev libdbus-1-dev libusb-dev libudev-dev libical-dev systemd libreadline-dev automake
cd /sensortag/bluez-5.12
make install

cp /sensortag/bluez-5.12/attrib/gatttool /usr/local/bin/