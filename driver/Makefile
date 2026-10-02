obj-m += edu_driver.o

# Must be the same tree the guest kernel was built from (see scripts/build_kernel.sh).
KVER ?= 6.6.87
WORK ?= /home/$(shell id -un)/work
KDIR ?= $(WORK)/linux-$(KVER)

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
