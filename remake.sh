#!/bin/bash
sudo rmmod pipey
make
sudo insmod ./pipey.ko
./reader_example just.txt
