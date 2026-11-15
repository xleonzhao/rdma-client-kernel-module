KDIR ?= /lib/modules/$(shell uname -r)/build
MODULE_NAME = rdma_kclient

SRCS = kclient.c krdma.c
OBJS = $(SRCS:.c=.o)

obj-m += $(MODULE_NAME).o
$(MODULE_NAME)-y = $(OBJS)

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# same build with debug info and pr_debug()/dynamic debug output enabled
debug:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules KCFLAGS="-g -DDEBUG"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all debug clean
