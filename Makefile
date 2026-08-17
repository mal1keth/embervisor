# embervisor: a from-scratch KVM virtual machine monitor
CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=gnu11 -MMD -MP
LDFLAGS += -pthread

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)

all: embervisor guests/hello.bin guests/hello32.bin

embervisor: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

# Fully static build, for dropping into an initramfs (see scripts/run-nested.sh)
embervisor-static: $(SRC)
	$(CC) $(CFLAGS) -static -o $@ $(SRC) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Bare-metal guest payload: 16-bit code, linked flat at 0x1000
guests/hello.bin: guests/hello.S
	as --32 -o guests/hello.o $<
	ld -m elf_i386 -Ttext 0x1000 --oformat binary -e _start -o $@ guests/hello.o

# 32-bit payload at 1 MiB, entered with the Linux boot-protocol state.
# Assembled as x86-64 because it climbs into long mode (.code32/.code64 mix).
guests/hello32.bin: guests/hello32.S
	as -o guests/hello32.o $<
	ld -m elf_x86_64 -Ttext 0x100000 --oformat binary -e _start -o $@ guests/hello32.o

clean:
	rm -f embervisor embervisor-static $(OBJ) $(DEP) \
	    guests/hello.o guests/hello.bin guests/hello32.o guests/hello32.bin

-include $(DEP)
.PHONY: all clean
