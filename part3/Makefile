obj-m := elevator.o

KERNEL := 4.19.98
KDIR := /lib/modules/$(KERNEL)/build

default:
	$(MAKE) -C $(KDIR) M=$(PWD)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
