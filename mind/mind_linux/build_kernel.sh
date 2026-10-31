#!/bin/bash
echo "Remove kernel logs"
sudo rm /var/log/kern.log
sudo rm /var/log/syslog
echo "Remoce cache"
sudo rm .cache.mk
echo "Start"

taskset --cpu-list 0-11 make bzImage -j11 && sudo make install
# ./build_kernel_modules.sh

#sudo reboot
