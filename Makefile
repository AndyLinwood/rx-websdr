CC=gcc
CFLAGS=-O2 -Wall -D_GNU_SOURCE $(shell pkg-config --cflags libwebsockets fftw3f libbsd)
LDFLAGS=$(shell pkg-config --libs libwebsockets fftw3f libbsd) -liniparser -lpthread -lm -lz

SRC=$(wildcard src/*.c)
OBJ=$(SRC:.c=.o)

websdr-server: $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) websdr-server
