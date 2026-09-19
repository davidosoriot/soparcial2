#include "editor.h"
#include "pcompress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

#define INITIAL_LINE_CAPACITY 32

/* ====================================================================================
 * Parcial 2: estado de la tarea de compresion/descompresion en segundo plano
 * ====================================================================================
 * Solo se permite UNA tarea concurrente a la vez (coherente con que este es
 * un editor de un unico buffer). El puntero vive en el HEAP (lo crea
 * job_start_compress/job_start_decompress dentro de pcompress.c) porque el
 * hilo coordinador de la tarea sigue vivo mucho despues de que la funcion
 * cmd_compress/cmd_decompress que lo lanzo ya retorno al bucle principal del
 * editor -- ver la justificacion completa en compress/pcompress.h.
 */
static CompressJob *g_active_job = NULL;

static int default_num_workers(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 8) n = 8; /* tope razonable para un editor interactivo */
    return (int)n;
}

/* Punto unico de verificacion de la condicion de carrera que exige evitar el
 * enunciado: si hay una tarea en segundo plano leyendo/escribiendo
 * 'filename', ninguna operacion de edicion puede tocar ese mismo archivo
 * hasta que termine. */
static int check_job_lock(const Buffer *buf) {
    if (g_active_job != NULL && job_blocks_path(g_active_job, buf->filename)) {
        fprintf(stderr,
                "edi: archivo bloqueado: hay una tarea en segundo plano sobre '%s' (%d%%). "
                "Espere a que finalice o consulte 'j'.\n",
                buf->filename, job_get_progress(g_active_job));
        return -1;
    }
    return 0;
}

/* Si la tarea previa ya termino (DONE/ERROR) pero no se ha liberado, hace el
 * pthread_join (inmediato, porque el hilo ya salio) y libera su memoria,
 * dejando el slot libre para una tarea nueva. */
static void reap_finished_job(void) {
    if (g_active_job != NULL && !job_is_running(g_active_job)) {
        job_join_and_free(g_active_job);
        g_active_job = NULL;
    }
}

/* ====================================================================================
 * Utilidades de bajo nivel: read/write "completos"
 * ====================================================================================
 * read() y write() no garantizan transferir todos los bytes solicitados en una
 * sola llamada (pueden ser interrumpidos por una senal, o el kernel puede
 * decidir entregar menos). Estas envolturas repiten la operacion hasta
 * completarla o toparse con un error/EOF real, evitando corrupcion silenciosa
 * de datos al desplazar bytes dentro del archivo.
 */
static ssize_t read_full(int fd, void *buf, size_t count) {
    size_t total = 0;
    char *p = (char *)buf;
    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break; /* EOF */
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static ssize_t write_full(int fd, const void *buf, size_t count) {
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < count) {
        ssize_t n = write(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/* ====================================================================================
 * Ciclo de vida del Buffer
 * ==================================================================================== */
void buffer_init(Buffer *buf) {
    memset(buf, 0, sizeof(*buf));
    buf->fd = -1;
}

void buffer_close(Buffer *buf) {
    if (buf->fd != -1) {
        if (close(buf->fd) == -1) {
            perror("edi: close");
        }
        buf->fd = -1;
    }
    free(buf->line_offsets);
    buf->line_offsets = NULL;
    buf->line_count = 0;
    buf->line_capacity = 0;
    buf->filename[0] = '\0';
}

void buffer_free_all(Buffer *buf) {
    buffer_close(buf);
    free(buf->clipboard);
    buf->clipboard = NULL;
    buf->clipboard_len = 0;
}

/* Agrega un desplazamiento al indice dinamico de lineas, duplicando la
 * capacidad (realloc) cuando se agota el espacio reservado. */
static int buffer_push_offset(Buffer *buf, off_t offset) {
    if (buf->line_count == buf->line_capacity) {
        size_t new_cap = buf->line_capacity == 0 ? INITIAL_LINE_CAPACITY : buf->line_capacity * 2;
        off_t *tmp = realloc(buf->line_offsets, new_cap * sizeof(off_t));
        if (tmp == NULL) {
            perror("edi: realloc");
            return -1;
        }
        buf->line_offsets = tmp;
        buf->line_capacity = new_cap;
    }
    buf->line_offsets[buf->line_count++] = offset;
    return 0;
}

/* Reconstruye el indice de offsets recorriendo el archivo completo con
 * read(). Se invoca tras cada operacion que modifica el archivo (a, d, i, x).
 * Es deliberadamente simple: recalcular es O(tamano del archivo), pero evita
 * la aritmetica de desplazamientos incrementales a mano, fuente comun de
 * bugs de off-by-one. Para un editor interactivo de texto plano el costo es
 * imperceptible. */
int index_lines(Buffer *buf) {
    if (buf->fd == -1) return -1;

    if (lseek(buf->fd, 0, SEEK_SET) == (off_t)-1) {
        perror("edi: lseek");
        return -1;
    }

    buf->line_count = 0; /* conserva la memoria ya reservada (line_capacity) para reutilizarla */

    char chunk[EDI_READ_CHUNK];
    off_t pos = 0;
    int line_open = 0; /* hay una linea en curso cuyo inicio ya fue registrado */
    ssize_t n;

    while ((n = read(buf->fd, chunk, sizeof(chunk))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (!line_open) {
                if (buffer_push_offset(buf, pos) == -1) return -1;
                line_open = 1;
            }
            if (chunk[i] == '\n') {
                line_open = 0;
            }
            pos++;
        }
    }
    if (n < 0) {
        perror("edi: read");
        return -1;
    }
    return 0;
}

int buffer_open(Buffer *buf, const char *filename) {
    if (filename == NULL || filename[0] == '\0') {
        fprintf(stderr, "edi: uso: o <archivo>\n");
        return -1;
    }

    /* O_CREAT con mode_t 0644: si el archivo no existe se crea con permisos
     * rw-r--r--; si ya existe, se abre tal cual (sin O_TRUNC: no se pierde
     * el contenido previo). */
    int fd = open(filename, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        perror("edi: open");
        return -1;
    }

    /* Si ya habia un archivo abierto en este buffer, se cierra limpiamente
     * antes de reemplazarlo (el equipo de 3 no requiere multi-buffer). */
    buffer_close(buf);

    buf->fd = fd;
    strncpy(buf->filename, filename, sizeof(buf->filename) - 1);
    buf->filename[sizeof(buf->filename) - 1] = '\0';

    if (index_lines(buf) == -1) {
        fprintf(stderr, "edi: no se pudo indexar '%s'\n", filename);
        buffer_close(buf);
        return -1;
    }

    printf("Archivo '%s' abierto (%zu linea%s).\n",
           filename, buf->line_count, buf->line_count == 1 ? "" : "s");
    return 0;
}

int file_size(Buffer *buf, off_t *out_size) {
    if (buf->fd == -1) return -1;
    off_t cur = lseek(buf->fd, 0, SEEK_CUR);
    off_t end = lseek(buf->fd, 0, SEEK_END);
    if (end == (off_t)-1) {
        perror("edi: lseek");
        return -1;
    }
    if (cur != (off_t)-1) {
        lseek(buf->fd, cur, SEEK_SET); /* restaurar la posicion original del cursor */
    }
    *out_size = end;
    return 0;
}

/* Garantiza que el archivo termine en '\n' antes de escribir una linea
 * nueva a partir de EOF. Un archivo de texto "mal formado" (ultima linea sin
 * salto de linea final, comun si se creo con otra herramienta) haria que la
 * proxima escritura en EOF quedara pegada al final de esa linea en vez de
 * empezar una nueva. Costo: 1 byte extra solo cuando hace falta. */
static int ensure_trailing_newline(Buffer *buf) {
    off_t sz;
    if (file_size(buf, &sz) == -1) return -1;
    if (sz == 0) return 0;

    char last;
    if (lseek(buf->fd, sz - 1, SEEK_SET) == (off_t)-1) {
        perror("edi: lseek");
        return -1;
    }
    if (read_full(buf->fd, &last, 1) != 1) {
        fprintf(stderr, "edi: lectura incompleta al verificar el fin de archivo\n");
        return -1;
    }
    if (last != '\n') {
        if (lseek(buf->fd, 0, SEEK_END) == (off_t)-1) {
            perror("edi: lseek");
            return -1;
        }
        if (write_full(buf->fd, "\n", 1) != 1) {
            perror("edi: write");
            return -1;
        }
    }
    return 0;
}

static int require_open(Buffer *buf) {
    if (buf->fd == -1) {
        fprintf(stderr, "edi: no hay archivo abierto. Use 'o <archivo>' primero.\n");
        return -1;
    }
    return 0;
}

/* Lee el contenido completo de la linea 'idx' (0-index interno) en un buffer
 * malloc'd, null-terminado y SIN el '\n' final. El llamador es responsable
 * de hacer free(). Se reutiliza en p <n>, s <palabra> y y <n>. */
static int read_line_range(Buffer *buf, size_t idx, char **out, size_t *out_len) {
    off_t start = buf->line_offsets[idx];
    off_t end;
    if (idx + 1 < buf->line_count) {
        end = buf->line_offsets[idx + 1];
    } else {
        if (file_size(buf, &end) == -1) return -1;
    }

    size_t len = (size_t)(end - start);
    char *b = malloc(len + 1);
    if (b == NULL) {
        perror("edi: malloc");
        return -1;
    }
    if (lseek(buf->fd, start, SEEK_SET) == (off_t)-1) {
        perror("edi: lseek");
        free(b);
        return -1;
    }
    ssize_t got = read_full(buf->fd, b, len);
    if (got < 0 || (size_t)got != len) {
        fprintf(stderr, "edi: lectura incompleta de linea\n");
        free(b);
        return -1;
    }
    b[len] = '\0';
    if (len > 0 && b[len - 1] == '\n') {
        b[len - 1] = '\0';
        len--;
    }
    *out = b;
    *out_len = len;
    return 0;
}

/* Inserta 'text' como una nueva linea ANTES de la posicion 'n' (1-indexado).
 * n == line_count + 1 equivale a un append al final. Comparte esta logica
 * el comando 'i' (insercion arbitraria) y 'x' (pegar desde el portapapeles).
 *
 * Estrategia (buffer dinamico + lseek, tal como exige el reto del Nivel 2):
 *   1. Leer en memoria (malloc) todos los bytes que hay DESPUES del punto
 *      de insercion (la "cola" del archivo).
 *   2. Reescribir desde el punto de insercion: primero la linea nueva, luego
 *      la cola leida en el paso 1.
 * Esto nunca pierde datos porque la cola completa se copia a RAM antes de
 * sobrescribir esa zona del archivo. */
static int insert_at(Buffer *buf, size_t n, const char *text) {
    if (check_job_lock(buf) == -1) return -1;
    if (ensure_trailing_newline(buf) == -1) return -1;

    size_t text_len = strlen(text);
    char *newline_buf = malloc(text_len + 2);
    if (newline_buf == NULL) {
        perror("edi: malloc");
        return -1;
    }
    memcpy(newline_buf, text, text_len);
    newline_buf[text_len] = '\n';
    newline_buf[text_len + 1] = '\0';
    size_t newline_len = text_len + 1;

    off_t old_size;
    if (file_size(buf, &old_size) == -1) {
        free(newline_buf);
        return -1;
    }
    off_t insert_offset = (n <= buf->line_count) ? buf->line_offsets[n - 1] : old_size;
    size_t tail_len = (size_t)(old_size - insert_offset);

    char *tail = NULL;
    if (tail_len > 0) {
        tail = malloc(tail_len);
        if (tail == NULL) {
            perror("edi: malloc");
            free(newline_buf);
            return -1;
        }
        if (lseek(buf->fd, insert_offset, SEEK_SET) == (off_t)-1) {
            perror("edi: lseek");
            free(tail);
            free(newline_buf);
            return -1;
        }
        if (read_full(buf->fd, tail, tail_len) != (ssize_t)tail_len) {
            fprintf(stderr, "edi: lectura incompleta al desplazar bytes\n");
            free(tail);
            free(newline_buf);
            return -1;
        }
    }

    if (lseek(buf->fd, insert_offset, SEEK_SET) == (off_t)-1) {
        perror("edi: lseek");
        free(tail);
        free(newline_buf);
        return -1;
    }
    if (write_full(buf->fd, newline_buf, newline_len) != (ssize_t)newline_len) {
        perror("edi: write");
        free(tail);
        free(newline_buf);
        return -1;
    }
    free(newline_buf);

    if (tail_len > 0) {
        if (write_full(buf->fd, tail, tail_len) != (ssize_t)tail_len) {
            perror("edi: write");
            free(tail);
            return -1;
        }
        free(tail);
    }

    if (index_lines(buf) == -1) return -1;
    printf("Linea insertada en la posicion %zu (ahora %zu linea%s).\n",
           n, buf->line_count, buf->line_count == 1 ? "" : "s");
    return 0;
}

/* ====================================================================================
 * Comandos base: p, a, d
 * ==================================================================================== */
int cmd_print(Buffer *buf, const char *arg) {
    if (require_open(buf) == -1) return -1;

    if (arg == NULL || arg[0] == '\0') {
        /* Sin parametros: recorre TODO el archivo con lseek()+read(), y lo
         * imprime con write() directo sobre FD 1 (STDOUT), tal como pide
         * la tabla de syscalls recomendadas del enunciado. */
        if (lseek(buf->fd, 0, SEEK_SET) == (off_t)-1) {
            perror("edi: lseek");
            return -1;
        }
        char chunk[EDI_READ_CHUNK];
        ssize_t n;
        size_t total = 0;
        while ((n = read(buf->fd, chunk, sizeof(chunk))) > 0) {
            if (write_full(STDOUT_FILENO, chunk, (size_t)n) == -1) {
                perror("edi: write");
                return -1;
            }
            total += (size_t)n;
        }
        if (n < 0) {
            perror("edi: read");
            return -1;
        }
        if (total == 0) printf("(archivo vacio)\n");
        return 0;
    }

    char *end;
    long n_l = strtol(arg, &end, 10);
    if (end == arg || *end != '\0' || n_l <= 0) {
        fprintf(stderr, "edi: numero de linea invalido: '%s'\n", arg);
        return -1;
    }
    size_t n = (size_t)n_l;
    if (n > buf->line_count) {
        fprintf(stderr, "edi: la linea %zu no existe (el archivo tiene %zu linea%s)\n",
                n, buf->line_count, buf->line_count == 1 ? "" : "s");
        return -1;
    }

    char *content;
    size_t len;
    if (read_line_range(buf, n - 1, &content, &len) == -1) return -1;

    /* printf() usa un buffer de stdio; write() va directo al kernel. Sin un
     * fflush() aqui, la etiqueta "n: " podria aparecer en pantalla DESPUES
     * del contenido (ya escrito por write) la proxima vez que stdio decida
     * vaciar su buffer -- notorio sobre todo cuando stdout no es una
     * terminal (p.ej. al correr el editor dentro de un script de pruebas). */
    printf("%3zu: ", n);
    fflush(stdout);
    write_full(STDOUT_FILENO, content, len);
    write_full(STDOUT_FILENO, "\n", 1);
    free(content);
    return 0;
}

int cmd_append(Buffer *buf, const char *arg) {
    if (require_open(buf) == -1) return -1;
    if (check_job_lock(buf) == -1) return -1;
    if (arg == NULL || arg[0] == '\0') {
        fprintf(stderr, "edi: uso: a <texto>\n");
        return -1;
    }

    if (ensure_trailing_newline(buf) == -1) return -1;

    size_t text_len = strlen(arg);
    char *line = malloc(text_len + 2); /* texto + '\n' + '\0' */
    if (line == NULL) {
        perror("edi: malloc");
        return -1;
    }
    memcpy(line, arg, text_len);
    line[text_len] = '\n';
    line[text_len + 1] = '\0';

    if (lseek(buf->fd, 0, SEEK_END) == (off_t)-1) {
        perror("edi: lseek");
        free(line);
        return -1;
    }
    ssize_t written = write_full(buf->fd, line, text_len + 1);
    free(line);
    if (written == -1 || (size_t)written != text_len + 1) {
        perror("edi: write");
        return -1;
    }

    if (index_lines(buf) == -1) return -1;
    printf("Linea agregada (ahora %zu linea%s).\n", buf->line_count, buf->line_count == 1 ? "" : "s");
    return 0;
}

int cmd_delete(Buffer *buf, const char *arg) {
    if (require_open(buf) == -1) return -1;
    if (check_job_lock(buf) == -1) return -1;
    if (arg == NULL || arg[0] == '\0') {
        fprintf(stderr, "edi: uso: d <n>\n");
        return -1;
    }
    char *end;
    long n_l = strtol(arg, &end, 10);
    if (end == arg || *end != '\0' || n_l <= 0) {
        fprintf(stderr, "edi: numero de linea invalido: '%s'\n", arg);
        return -1;
    }
    size_t n = (size_t)n_l;
    if (buf->line_count == 0 || n > buf->line_count) {
        fprintf(stderr, "edi: la linea %zu no existe\n", n);
        return -1;
    }

    off_t start = buf->line_offsets[n - 1];
    off_t old_size;
    if (file_size(buf, &old_size) == -1) return -1;
    off_t region_end = (n < buf->line_count) ? buf->line_offsets[n] : old_size;
    size_t tail_len = (size_t)(old_size - region_end);

    /* Igual estrategia que insert_at pero a la inversa: se lee a RAM la cola
     * que esta DESPUES de la linea borrada y se reescribe justo donde
     * empezaba la linea eliminada; luego se recorta el sobrante con
     * ftruncate(). */
    if (tail_len > 0) {
        char *tail = malloc(tail_len);
        if (tail == NULL) {
            perror("edi: malloc");
            return -1;
        }
        if (lseek(buf->fd, region_end, SEEK_SET) == (off_t)-1) {
            perror("edi: lseek");
            free(tail);
            return -1;
        }
        if (read_full(buf->fd, tail, tail_len) != (ssize_t)tail_len) {
            fprintf(stderr, "edi: lectura incompleta al desplazar bytes\n");
            free(tail);
            return -1;
        }
        if (lseek(buf->fd, start, SEEK_SET) == (off_t)-1) {
            perror("edi: lseek");
            free(tail);
            return -1;
        }
        if (write_full(buf->fd, tail, tail_len) != (ssize_t)tail_len) {
            perror("edi: write");
            free(tail);
            return -1;
        }
        free(tail);
    }

    if (ftruncate(buf->fd, start + (off_t)tail_len) == -1) {
        perror("edi: ftruncate");
        return -1;
    }

    if (index_lines(buf) == -1) return -1;
    printf("Linea %zu eliminada (ahora %zu linea%s).\n", n, buf->line_count, buf->line_count == 1 ? "" : "s");
    return 0;
}

/* ====================================================================================
 * Nivel 2: i (insercion arbitraria), s (busqueda)
 * ==================================================================================== */
int cmd_insert(Buffer *buf, const char *arg) {
    if (require_open(buf) == -1) return -1;
    if (arg == NULL || arg[0] == '\0') {
        fprintf(stderr, "edi: uso: i <n> <texto>\n");
        return -1;
    }

    char *arg_copy = strdup(arg);
    if (arg_copy == NULL) {
        perror("edi: strdup");
        return -1;
    }

    /* Separa el numero de linea del texto que le sigue */
    char *cursor = arg_copy;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    char *num_start = cursor;
    while (isdigit((unsigned char)*cursor)) cursor++;
    if (cursor == num_start) {
        fprintf(stderr, "edi: uso: i <n> <texto> (falta el numero de linea)\n");
        free(arg_copy);
        return -1;
    }
    char saved = *cursor;
    *cursor = '\0';
    long n_l = strtol(num_start, NULL, 10);
    if (saved != '\0') cursor++;
    while (*cursor == ' ' || *cursor == '\t') cursor++;

    if (n_l <= 0 || *cursor == '\0') {
        fprintf(stderr, "edi: uso: i <n> <texto>\n");
        free(arg_copy);
        return -1;
    }
    size_t n = (size_t)n_l;
    if (n > buf->line_count + 1) {
        fprintf(stderr, "edi: no se puede insertar en la linea %zu (maximo permitido: %zu)\n",
                n, buf->line_count + 1);
        free(arg_copy);
        return -1;
    }

    int rc = insert_at(buf, n, cursor);
    free(arg_copy);
    return rc;
}

int cmd_search(Buffer *buf, const char *arg) {
    if (require_open(buf) == -1) return -1;
    if (arg == NULL || arg[0] == '\0') {
        fprintf(stderr, "edi: uso: s <palabra>\n");
        return -1;
    }

    size_t matches = 0;
    for (size_t i = 0; i < buf->line_count; i++) {
        char *content;
        size_t len;
        if (read_line_range(buf, i, &content, &len) == -1) return -1;
        if (strstr(content, arg) != NULL) {
            matches++;
            printf("%3zu: ", i + 1);
            fflush(stdout); /* ver comentario en cmd_print: sincroniza printf() con el write() que sigue */
            write_full(STDOUT_FILENO, content, len);
            write_full(STDOUT_FILENO, "\n", 1);
        }
        free(content);
    }
    if (matches == 0) {
        printf("Sin coincidencias para '%s'.\n", arg);
    } else {
        printf("%zu coincidencia%s encontrada%s.\n", matches, matches == 1 ? "" : "s", matches == 1 ? "" : "s");
    }
    return 0;
}

/* ====================================================================================
 * Nivel 3: m (metadatos via fstat), y/x (portapapeles secuencial)
 * ==================================================================================== */
int cmd_metadata(Buffer *buf) {
    if (require_open(buf) == -1) return -1;

    struct stat st;
    if (fstat(buf->fd, &st) == -1) {
        perror("edi: fstat");
        return -1;
    }

    char perms[11];
    perms[0] = S_ISDIR(st.st_mode) ? 'd' : '-';
    perms[1] = (st.st_mode & S_IRUSR) ? 'r' : '-';
    perms[2] = (st.st_mode & S_IWUSR) ? 'w' : '-';
    perms[3] = (st.st_mode & S_IXUSR) ? 'x' : '-';
    perms[4] = (st.st_mode & S_IRGRP) ? 'r' : '-';
    perms[5] = (st.st_mode & S_IWGRP) ? 'w' : '-';
    perms[6] = (st.st_mode & S_IXGRP) ? 'x' : '-';
    perms[7] = (st.st_mode & S_IROTH) ? 'r' : '-';
    perms[8] = (st.st_mode & S_IWOTH) ? 'w' : '-';
    perms[9] = (st.st_mode & S_IXOTH) ? 'x' : '-';
    perms[10] = '\0';

    char time_buf[64];
    struct tm tm_info;
    localtime_r(&st.st_mtime, &tm_info);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    printf("--- Metadatos de '%s' (fstat) ---\n", buf->filename);
    printf("  Inodo:               %lu\n", (unsigned long)st.st_ino);
    printf("  Permisos:            %s (%o)\n", perms, st.st_mode & 0777);
    printf("  Tamano:              %lld bytes\n", (long long)st.st_size);
    printf("  Lineas indexadas:    %zu\n", buf->line_count);
    printf("  Ultima modificacion: %s\n", time_buf);
    printf("  Enlaces (nlink):     %lu\n", (unsigned long)st.st_nlink);
    printf("------------------------------------\n");
    return 0;
}

int cmd_yank(Buffer *buf, const char *arg) {
    if (require_open(buf) == -1) return -1;
    if (arg == NULL || arg[0] == '\0') {
        fprintf(stderr, "edi: uso: y <n>\n");
        return -1;
    }
    char *end;
    long n_l = strtol(arg, &end, 10);
    if (end == arg || *end != '\0' || n_l <= 0 || (size_t)n_l > buf->line_count) {
        fprintf(stderr, "edi: numero de linea invalido para copiar: '%s'\n", arg);
        return -1;
    }

    char *content;
    size_t len;
    if (read_line_range(buf, (size_t)n_l - 1, &content, &len) == -1) return -1;

    /* Portapapeles SECUENCIAL: una sola ranura, cada 'y' sobrescribe la
     * copia anterior (no es una pila de historial). */
    free(buf->clipboard);
    buf->clipboard = content;
    buf->clipboard_len = len;

    printf("Linea %ld copiada al portapapeles (%zu bytes).\n", n_l, len);
    return 0;
}

int cmd_paste(Buffer *buf, const char *arg) {
    if (require_open(buf) == -1) return -1;
    if (buf->clipboard == NULL) {
        fprintf(stderr, "edi: el portapapeles esta vacio. Use 'y <n>' primero.\n");
        return -1;
    }

    size_t n;
    if (arg == NULL || arg[0] == '\0') {
        n = buf->line_count + 1; /* sin argumento: pegar al final */
    } else {
        char *end;
        long n_l = strtol(arg, &end, 10);
        if (end == arg || *end != '\0' || n_l <= 0 || (size_t)n_l > buf->line_count + 1) {
            fprintf(stderr, "edi: numero de linea invalido para pegar: '%s'\n", arg);
            return -1;
        }
        n = (size_t)n_l;
    }

    return insert_at(buf, n, buf->clipboard);
}

/* ====================================================================================
 * Parcial 2: compresion Huffman concurrente en segundo plano
 * ====================================================================================
 * 'c' y 'u' solo LANZAN la tarea (job_start_compress/job_start_decompress
 * arrancan el hilo coordinador y retornan de inmediato) y devuelven el
 * control al REPL al instante: el editor sigue respondiendo a otros
 * comandos mientras la compresion corre en paralelo, tal como exige el
 * enunciado ("no deben bloquear la interfaz de usuario"). El hilo "ticker"
 * de pcompress.c va imprimiendo el progreso en tiempo real sin que el
 * usuario tenga que pedirlo; 'j' lo consulta bajo demanda.
 * ==================================================================================== */
int cmd_compress(Buffer *buf, const char *arg) {
    if (require_open(buf) == -1) return -1;
    reap_finished_job();
    if (g_active_job != NULL) {
        fprintf(stderr, "edi: ya hay una tarea en segundo plano en curso. Use 'j' para ver el progreso.\n");
        return -1;
    }

    char output_path[PATH_MAX];
    if (arg != NULL && arg[0] != '\0') {
        strncpy(output_path, arg, sizeof(output_path) - 1);
        output_path[sizeof(output_path) - 1] = '\0';
    } else {
        /* buf->filename ya esta acotado a PATH_MAX-1 (ver buffer_open); si
         * agregar ".huf" se saliera del buffer, se trunca el nombre base en
         * vez de desbordar. Caso practicamente imposible en uso real. */
        size_t base_len = strlen(buf->filename);
        if (base_len + 4 >= sizeof(output_path)) base_len = sizeof(output_path) - 5;
        memcpy(output_path, buf->filename, base_len);
        memcpy(output_path + base_len, ".huf", 4);
        output_path[base_len + 4] = '\0';
    }

    int num_workers = default_num_workers();
    g_active_job = job_start_compress(buf->filename, output_path, num_workers);
    if (g_active_job == NULL) {
        fprintf(stderr, "edi: no se pudo iniciar la tarea de compresion.\n");
        return -1;
    }

    printf("Compresion de '%s' -> '%s' iniciada en segundo plano (%d hilo%s trabajador%s). "
           "El editor sigue disponible; use 'j' para ver el progreso.\n",
           buf->filename, output_path, num_workers,
           num_workers == 1 ? "" : "s", num_workers == 1 ? "" : "es");
    return 0;
}

int cmd_decompress(Buffer *buf, const char *arg) {
    (void)buf;
    if (arg == NULL || arg[0] == '\0') {
        fprintf(stderr, "edi: uso: u <archivo.huf> [archivo_salida]\n");
        return -1;
    }
    reap_finished_job();
    if (g_active_job != NULL) {
        fprintf(stderr, "edi: ya hay una tarea en segundo plano en curso. Use 'j' para ver el progreso.\n");
        return -1;
    }

    char arg_copy[EDI_LINE_INPUT_MAX];
    strncpy(arg_copy, arg, sizeof(arg_copy) - 1);
    arg_copy[sizeof(arg_copy) - 1] = '\0';

    char *input_path = strtok(arg_copy, " \t");
    char *output_arg = strtok(NULL, " \t");
    if (input_path == NULL) {
        fprintf(stderr, "edi: uso: u <archivo.huf> [archivo_salida]\n");
        return -1;
    }

    char output_path[PATH_MAX];
    if (output_arg != NULL && output_arg[0] != '\0') {
        strncpy(output_path, output_arg, sizeof(output_path) - 1);
        output_path[sizeof(output_path) - 1] = '\0';
    } else {
        size_t len = strlen(input_path);
        if (len > 4 && strcmp(input_path + len - 4, ".huf") == 0) {
            memcpy(output_path, input_path, len - 4);
            output_path[len - 4] = '\0';
        } else {
            snprintf(output_path, sizeof(output_path), "%s.dec", input_path);
        }
    }

    int num_workers = default_num_workers();
    g_active_job = job_start_decompress(input_path, output_path, num_workers);
    if (g_active_job == NULL) {
        fprintf(stderr, "edi: no se pudo iniciar la tarea de descompresion.\n");
        return -1;
    }

    printf("Descompresion de '%s' -> '%s' iniciada en segundo plano (%d hilo%s trabajador%s). "
           "El editor sigue disponible; use 'j' para ver el progreso.\n",
           input_path, output_path, num_workers,
           num_workers == 1 ? "" : "s", num_workers == 1 ? "" : "es");
    return 0;
}

int cmd_job_status(Buffer *buf) {
    (void)buf;
    if (g_active_job == NULL) {
        printf("No hay ninguna tarea de compresion/descompresion en curso.\n");
        return 0;
    }

    JobState st = job_get_state(g_active_job);
    int p = job_get_progress(g_active_job);
    const char *kind = job_get_kind(g_active_job) == JOB_KIND_COMPRESS ? "Compresion" : "Descompresion";

    switch (st) {
        case JOB_STATE_RUNNING:
            printf("%s en curso: '%s' -> '%s' (%d%%).\n",
                   kind, job_get_input_path(g_active_job), job_get_output_path(g_active_job), p);
            break;
        case JOB_STATE_DONE:
            printf("%s completada: '%s' (%lld bytes) -> '%s' (%lld bytes).\n",
                   kind, job_get_input_path(g_active_job), job_get_input_size(g_active_job),
                   job_get_output_path(g_active_job), job_get_output_size(g_active_job));
            break;
        case JOB_STATE_ERROR:
            printf("%s fallo: %s\n", kind, job_get_error(g_active_job));
            break;
        default:
            printf("Sin tarea activa.\n");
    }
    return 0;
}

/* ====================================================================================
 * Dispatcher y bucle principal (REPL propio del editor)
 * ==================================================================================== */
static char *ltrim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void rtrim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

int dispatch(Buffer *buf, char *raw_line) {
    rtrim_newline(raw_line);
    char *line = ltrim(raw_line);
    if (line[0] == '\0') return 0; /* linea vacia: ignorar */

    char cmd = line[0];
    char *rest = ltrim(line + 1);

    switch (cmd) {
        case 'o': buffer_open(buf, rest); break;
        case 'p': cmd_print(buf, rest); break;
        case 'a': cmd_append(buf, rest); break;
        case 'd': cmd_delete(buf, rest); break;
        case 'i': cmd_insert(buf, rest); break;
        case 's': cmd_search(buf, rest); break;
        case 'm': cmd_metadata(buf); break;
        case 'y': cmd_yank(buf, rest); break;
        case 'x': cmd_paste(buf, rest); break;
        case 'c': cmd_compress(buf, rest); break;
        case 'u': cmd_decompress(buf, rest); break;
        case 'j': cmd_job_status(buf); break;
        case 'q':
            /* Apagado limpio: si hay una tarea en segundo plano, se espera
             * a que termine con pthread_join (dentro de job_join_and_free)
             * en vez de dejar hilos huerfanos al salir con exit(). */
            if (g_active_job != NULL) {
                if (job_is_running(g_active_job)) {
                    printf("Esperando a que finalice la tarea en segundo plano antes de salir...\n");
                    fflush(stdout);
                }
                job_join_and_free(g_active_job);
                g_active_job = NULL;
            }
            /* Syscalls exigidas por el enunciado para 'q': close() + exit() */
            buffer_free_all(buf);
            printf("Hasta luego.\n");
            exit(0);
        default:
            fprintf(stderr, "edi: comando desconocido: '%c'. Comandos: o p a d i s m y x c u j q\n", cmd);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "Uso: %s [archivo]\n", argv[0]);
        return 1;
    }

    Buffer buf;
    buffer_init(&buf);

    if (argc == 2) {
        buffer_open(&buf, argv[1]);
    }

    printf("=== edi - editor de texto CLI (SO2026B) ===\n");
    printf("Comandos: o[archivo] p[n] a[texto] d[n] i[n][texto] s[palabra] m y[n] x[n] q\n");
    printf("Parcial 2: c[archivo_salida] (comprimir en 2do plano) u<archivo.huf>[salida] (descomprimir) j (estado)\n\n");

    char line[EDI_LINE_INPUT_MAX];
    while (1) {
        ui_print_lock();
        printf("edi> ");
        fflush(stdout);
        ui_print_unlock();
        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break; /* EOF (Ctrl+D): salir sin dejar fugas */
        }
        dispatch(&buf, line);
    }

    buffer_free_all(&buf);
    return 0;
}
