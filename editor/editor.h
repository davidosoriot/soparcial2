#ifndef EDITOR_H
#define EDITOR_H

#include <sys/types.h>
#include <stddef.h>
#include <limits.h>

/*
 * ====================================================================================
 * "edi" - Editor de texto CLI basado en llamadas al sistema POSIX
 * ====================================================================================
 * Equipo de 3 integrantes: implementa comandos base (o, p, a, d, q) + Nivel 2
 * (i: insercion arbitraria, s: busqueda) + Nivel 3 (m: metadatos via fstat,
 * y/x: portapapeles secuencial).
 *
 * Restriccion de I/O de archivo (impuesta por el enunciado): unicamente
 * open/read/write/lseek/ftruncate/close/fstat. fopen/fread/fwrite/fclose
 * quedan prohibidos. printf/fgets se usan solo para el dialogo con el
 * usuario (prompt, mensajes de error); el CONTENIDO del archivo que se
 * imprime en pantalla (comandos p y s) se escribe con write(STDOUT_FILENO,...)
 * para respetar la letra del enunciado ("read(), lseek(), write() (FD 1)").
 */

#define EDI_LINE_INPUT_MAX 4096   /* longitud maxima de una linea de comando leida por fgets */
#define EDI_READ_CHUNK     4096   /* tamano del bloque de lectura usado al indexar el archivo */

/*
 * Buffer: representa el archivo actualmente abierto por el editor.
 *
 * El archivo en disco es la unica fuente de verdad (no se mantiene una copia
 * completa del texto en RAM). Lo que SI se mantiene en memoria dinamica es un
 * indice de desplazamientos (line_offsets): la posicion en bytes donde
 * comienza cada linea. Ese indice se reconstruye con index_lines() despues de
 * cada operacion que modifica el archivo (a, d, i, x). Es una decision de
 * diseno deliberada: recalcular el indice es O(tamano del archivo), pero para
 * un editor de texto plano de uso interactivo esto es imperceptible y evita
 * la aritmetica de desplazamientos incrementales (fuente comun de bugs de
 * off-by-one al desplazar bytes a mano).
 */
typedef struct {
    char    filename[PATH_MAX];
    int     fd;                 /* -1 si no hay archivo abierto */

    off_t  *line_offsets;       /* line_offsets[i] = offset de inicio de la linea i (0-index interno) */
    size_t  line_count;
    size_t  line_capacity;

    char   *clipboard;          /* portapapeles secuencial: solo la ultima linea copiada con 'y' */
    size_t  clipboard_len;
} Buffer;

/* Ciclo de vida del buffer */
void buffer_init(Buffer *buf);
int  buffer_open(Buffer *buf, const char *filename);
void buffer_close(Buffer *buf);       /* cierra fd y libera line_offsets, conserva clipboard */
void buffer_free_all(Buffer *buf);    /* buffer_close() + libera clipboard (usado al salir) */

/* Indexado interno */
int  index_lines(Buffer *buf);
int  file_size(Buffer *buf, off_t *out_size);

/* Comandos base (Individual) */
int  cmd_print(Buffer *buf, const char *arg);          /* p [n] */
int  cmd_append(Buffer *buf, const char *arg);          /* a [texto] */
int  cmd_delete(Buffer *buf, const char *arg);           /* d [n] */

/* Comandos Nivel 2 (Parejas) */
int  cmd_insert(Buffer *buf, const char *arg);            /* i [n] [texto] */
int  cmd_search(Buffer *buf, const char *arg);              /* s [palabra] */

/* Comandos Nivel 3 (Tres) */
int  cmd_metadata(Buffer *buf);                                /* m */
int  cmd_yank(Buffer *buf, const char *arg);                    /* y [n] */
int  cmd_paste(Buffer *buf, const char *arg);                     /* x [n] */

/* Parcial 2: compresion Huffman concurrente en segundo plano (compress/pcompress.h) */
int  cmd_compress(Buffer *buf, const char *arg);      /* c [archivo_salida] */
int  cmd_decompress(Buffer *buf, const char *arg);    /* u <archivo.huf> [archivo_salida] */
int  cmd_job_status(Buffer *buf);                     /* j */

/* Dispatcher: parsea una linea cruda de comando y ejecuta la accion */
int  dispatch(Buffer *buf, char *raw_line);  /* retorna 1 si debe terminar el programa (q) */

#endif /* EDITOR_H */
