CC = gcc
CFLAGS = -Wall -g
LIBS = -lpthread

all: servidor

servidor: servidor.c
	$(CC) $(CFLAGS) -o servidor servidor.c $(LIBS)

clean:
	rm -f servidor
