obj-y += sys_call.o

obj-m += elevator.o

PWD := $(shell pwd)
KERNEL := linux-4.19.98
KDIR := /lib/modules/$(KERNEL)/build

default:
	$(MAKE) –C $(KDIR) SUBDIRS= $(PWD) modules

clean:
	rm –f *.o *.ko *.mod.* Module.* modules.*
