#!/usr/bin/env bash
mkdir -p test
dd if=/dev/zero of=test.img bs=1M count=50
./mkfs.simplefs test.img
sudo mount -o loop -t simplefs test.img test
