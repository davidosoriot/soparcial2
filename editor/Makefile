CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE -pthread -I../compress
LDFLAGS = -pthread
ARCHSALIDA = editor
SRCS = editor.c ../compress/huffman.c ../compress/pcompress.c
OBJS = $(SRCS:.c=.o)

all: $(ARCHSALIDA)

$(ARCHSALIDA): $(OBJS)
	$(CC) $(CFLAGS) -o $(ARCHSALIDA) $(OBJS) $(LDFLAGS)

editor.o: editor.c editor.h ../compress/pcompress.h
	$(CC) $(CFLAGS) -c editor.c -o editor.o

../compress/huffman.o: ../compress/huffman.c ../compress/huffman.h
	$(CC) $(CFLAGS) -c ../compress/huffman.c -o ../compress/huffman.o

../compress/pcompress.o: ../compress/pcompress.c ../compress/pcompress.h ../compress/huffman.h
	$(CC) $(CFLAGS) -c ../compress/pcompress.c -o ../compress/pcompress.o

clean:
	rm -f $(ARCHSALIDA) $(OBJS)

.PHONY: all clean
