_ACTIVE := $(filter-out clean,$(or $(MAKECMDGOALS),all))

CACTLIB ?= $(abspath ../CactLib-x86_32)

ifneq ($(_ACTIVE),)
ifndef CACTLIB
$(error Set CACTLIB to the libc project root (built by CactOS-x86_32 target libc))
endif
endif

CC      = gcc
LD      = ld

CFLAGS  = -m32 -ffreestanding -fPIE -fno-stack-protector -nostdlib \
          -I$(CACTLIB)/include -Wall -Wextra
LDFLAGS = -m elf_i386 -pie --no-dynamic-linker --hash-style=both \
          -nostdlib -T link.ld

START_O  = $(CACTLIB)/build/pic/start.o
LIBC_SO  = $(CACTLIB)/libc.so

SRCS = src/main.c
OBJS = $(SRCS:.c=.o)

TARGET = cgoct

all: $(LIBC_SO) $(START_O) $(TARGET)

$(LIBC_SO) $(START_O):
	@test -f $(LIBC_SO) && test -f $(START_O) || (echo >&2 "Missing libc or start.o — build libc first (CACTLIB=$(CACTLIB))"; exit 1)

$(TARGET): $(OBJS) $(START_O) $(LIBC_SO) link.ld
	$(LD) $(LDFLAGS) $(START_O) $(OBJS) $(LIBC_SO) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
