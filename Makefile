ifneq ($(KERNELRELEASE),)
obj-m:=pipey.o
else
KDIR:=/home/notsoold-laptop-vm/Downloads/linux-4.13.3

all:
	$(MAKE) -C $(KDIR) M=$$PWD
	gcc reader_example.c -o reader_example
	gcc writer_example.c -o writer_example
endif
