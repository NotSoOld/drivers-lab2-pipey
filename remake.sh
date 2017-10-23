#!/bin/bash
sudo rmmod pipey
make
sudo insmod ./pipey.ko
