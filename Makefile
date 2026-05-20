obj-m += simplefs.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	$(MAKE) -C userspace

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C userspace clean
