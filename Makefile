CC=gcc
CFLAGS=$(shell pkg-config --cflags libsystemd)
LDFLAGS=$(shell pkg-config --libs libsystemd)


all: uidallocd uidalloc

%.o: src/%.c
	$(CC) -c -o $@ $< $(CFLAGS)

uidallocd: main.o hashmap.o siphash24.o
	gcc -o $@ $^ $(LDFLAGS)

uidalloc: client.o
	gcc -o $@ $^ $(CFLAGS) $(LDFLAGS)

.PHONY: clean

clean:
	rm -f *.o