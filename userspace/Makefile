CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

all: simplefs_cli

simplefs_cli: simplefs_cli.c ../simplefs_ioctl.h
	$(CC) $(CFLAGS) -o $@ simplefs_cli.c

clean:
	rm -f simplefs_cli
