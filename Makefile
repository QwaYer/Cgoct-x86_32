CC      = gcc
LD      = ld

CFLAGS  = -m32 -ffreestanding -fPIE -fno-stack-protector -nostdlib \
          -I../CactLib-x86_32/include -Wall -Wextra
LDFLAGS = -m elf_i386 -pie --no-dynamic-linker --hash-style=both \
          -nostdlib -T link.ld

CACTLIB  = ../CactLib-x86_32
START_O  = $(CACTLIB)/build/pic/start.o
LIBC_SO  = $(CACTLIB)/libc.so

SRCS = src/main.c
OBJS = $(SRCS:.c=.o)

TARGET = cgoct

all: $(LIBC_SO) $(START_O) $(TARGET)

$(LIBC_SO):
	$(MAKE) -C $(CACTLIB)

$(START_O):
	$(MAKE) -C $(CACTLIB)

$(TARGET): $(OBJS) $(START_O) $(LIBC_SO) link.ld
	$(LD) $(LDFLAGS) $(START_O) $(OBJS) $(LIBC_SO) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
