CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 \
          -D_POSIX_C_SOURCE=200809L

TARGET  = msdos

SRCS = \
  src/kernel/fat.c      \
  src/kernel/disk.c     \
  src/kernel/fcb.c      \
  src/kernel/fileio.c   \
  src/kernel/chardev.c  \
  src/kernel/datetime.c \
  src/kernel/kernel.c   \
  src/command/command.c \
  src/host/bios_host.c  \
  src/host/main.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean format-test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Quick smoke-test: format a 720 KB image and boot it
format-test: $(TARGET)
	./$(TARGET) --format test.img --720
	./$(TARGET) test.img

clean:
	rm -f $(OBJS) $(TARGET) test.img
