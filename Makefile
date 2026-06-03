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
  src/command/edit.c   \
  src/command/basic.c  \
  src/host/bios_host.c  \
  src/host/main.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean format-test test demo

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Create the demo disk (720 KB, pre-loaded with BASIC programs + help files)
demo: $(TARGET)
	python3 basic/make_disk.py demo.img

# Quick smoke-test: format a 720 KB image and boot it
format-test: $(TARGET)
	./$(TARGET) --format test.img --720
	./$(TARGET) test.img

test: tests/test_fat tests/test_basic tests/test_edit tests/test_command
	./tests/test_fat
	./tests/test_basic
	./tests/test_edit >&2
	./tests/test_command

tests/test_fat: tests/test_fat.c src/kernel/fat.c src/kernel/disk.c
	$(CC) $(CFLAGS) -I. -o $@ $^

tests/test_basic: tests/test_basic.c src/command/basic.c
	$(CC) $(CFLAGS) -I. -o $@ $^ -lm

tests/test_edit: tests/test_edit.c
	$(CC) $(CFLAGS) -I. -o $@ $^

tests/test_command: tests/test_command.c \
    src/kernel/fat.c src/kernel/disk.c src/kernel/fcb.c \
    src/kernel/fileio.c src/kernel/chardev.c src/kernel/datetime.c \
    src/kernel/kernel.c src/command/command.c
	$(CC) $(CFLAGS) -I. -o $@ $^

clean:
	rm -f $(OBJS) $(TARGET) test.img demo.img \
	    tests/test_fat tests/test_basic tests/test_edit tests/test_command
