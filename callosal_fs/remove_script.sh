#!/bin/sh
sudo umount test
sudo rmmod simplefs
sudo rm -rf test
rm test.img
