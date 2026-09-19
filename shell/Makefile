CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE
ARCHSALIDA = eafitOS
SRCS = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c cat_editor.c
OBJS = $(SRCS:.c=.o)

# NOTA: el Makefile original del profesor (a) auto-ejecutaba el binario al
# final del target 'all' y (b) usaba $(TARGET) -no definido- en 'clean', por
# lo que 'make clean' nunca borraba el ejecutable. Ambas cosas se corrigieron
# aqui: 'make'/'make all' solo compila (no bloquea esperando stdin, necesario
# para el script de pruebas), y 'clean' usa $(ARCHSALIDA).

all: $(ARCHSALIDA)

$(ARCHSALIDA): $(OBJS)
	$(CC) $(CFLAGS) -o $(ARCHSALIDA) $(OBJS)

%.o: %.c shell.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(ARCHSALIDA) $(OBJS)

.PHONY: all clean
