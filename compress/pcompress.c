#include "pcompress.h"
#include "huffman.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_WORKERS_HARD_CAP 16
#define DEFAULT_BLOCK_SIZE   ((uint32_t)(1u << 20)) /* 1 MiB por bloque */
#define HUF_MAGIC   0x50465548u  /* "HUFP" */
#define HUF_VERSION 1u

/* ====================================================================================
 * Utilidades de E/S de bajo nivel
 * ==================================================================================== */
static ssize_t read_full(int fd, void *buf, size_t count) {
    size_t total = 0;
    char *p = (char *)buf;
    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) break;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static ssize_t write_full(int fd, const void *buf, size_t count) {
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < count) {
        ssize_t n = write(fd, p + total, count - total);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static ssize_t pread_full(int fd, void *buf, size_t count, off_t offset) {
    size_t total = 0;
    char *p = (char *)buf;
    while (total < count) {
        ssize_t n = pread(fd, p + total, count - total, offset + (off_t)total);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) break;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static ssize_t pwrite_full(int fd, const void *buf, size_t count, off_t offset) {
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < count) {
        ssize_t n = pwrite(fd, p + total, count - total, offset + (off_t)total);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static int write_u32(int fd, uint32_t v) {
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                            (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    return write_full(fd, b, 4) == 4 ? 0 : -1;
}
static int write_u64(int fd, uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i));
    return write_full(fd, b, 8) == 8 ? 0 : -1;
}
static int read_u32(int fd, uint32_t *out) {
    unsigned char b[4];
    if (read_full(fd, b, 4) != 4) return -1;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}
static int read_u64(int fd, uint64_t *out) {
    unsigned char b[8];
    if (read_full(fd, b, 8) != 8) return -1;
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    *out = v;
    return 0;
}

/* Mutex de impresion compartido entre el bucle principal del editor y los
 * hilos "ticker" de progreso de los jobs (ver pcompress.h). */
static pthread_mutex_t g_print_mutex = PTHREAD_MUTEX_INITIALIZER;
void ui_print_lock(void)   { pthread_mutex_lock(&g_print_mutex); }
void ui_print_unlock(void) { pthread_mutex_unlock(&g_print_mutex); }

/* ====================================================================================
 * Definicion del job (struct completo, privado a este .c)
 * ==================================================================================== */
struct CompressJob {
    JobKind          kind;
    JobState         state;          /* protegido por state_mutex */
    int              progress;       /* protegido por state_mutex, 0..100 */
    pthread_mutex_t  state_mutex;
    char             error_msg[256]; /* protegido por state_mutex */

    /* input_path/output_path/num_workers se fijan UNA VEZ antes de arrancar
     * los hilos y nunca se modifican despues: pueden leerse concurrentemente
     * sin lock (comentado explicitamente en cada sitio donde se hace). */
    char input_path[PATH_MAX];
    char output_path[PATH_MAX];
    int  num_workers;

    long long input_size;   /* se fija antes de terminar; solo el coordinador escribe */
    long long output_size;

    pthread_t coordinator;
    pthread_t ticker;
    int       ticker_started;
};

static void job_set_progress(CompressJob *job, int p) {
    pthread_mutex_lock(&job->state_mutex);
    job->progress = p;
    pthread_mutex_unlock(&job->state_mutex);
}
static void job_set_state(CompressJob *job, JobState s) {
    pthread_mutex_lock(&job->state_mutex);
    job->state = s;
    pthread_mutex_unlock(&job->state_mutex);
}
static void job_set_error(CompressJob *job, const char *msg) {
    pthread_mutex_lock(&job->state_mutex);
    snprintf(job->error_msg, sizeof(job->error_msg), "%s", msg);
    job->state = JOB_STATE_ERROR;
    pthread_mutex_unlock(&job->state_mutex);
}

/* ====================================================================================
 * Fase 1 (compresion): conteo de frecuencias en paralelo, con reduccion local
 * ====================================================================================
 * Cada hilo trabajador cuenta en SU PROPIA copia local de freq[256] (sin
 * ningun lock durante el conteo, porque nadie mas escribe esa memoria) y el
 * hilo coordinador suma ("reduce") las N copias al final. Los bloques que
 * cada hilo debe procesar se reparten dinamicamente mediante un contador
 * compartido (TaskCounter) protegido por un mutex: cada hilo pide "el
 * siguiente bloque libre" en vez de recibir un rango fijo de antemano, lo
 * que balancea la carga si algunos bloques tardan mas que otros (por cache
 * de disco, por ejemplo).
 * ==================================================================================== */
typedef struct {
    int      fd;
    uint64_t file_size;
    uint32_t block_size;
    size_t   num_blocks;
    pthread_mutex_t mutex;
    size_t   next_index;
} TaskCounter;

static void task_counter_init(TaskCounter *tc, int fd, uint64_t file_size,
                               uint32_t block_size, size_t num_blocks) {
    tc->fd = fd;
    tc->file_size = file_size;
    tc->block_size = block_size;
    tc->num_blocks = num_blocks;
    tc->next_index = 0;
    pthread_mutex_init(&tc->mutex, NULL);
}

/* Retorna el indice de bloque que le toca procesar al llamador, o -1 si ya
 * no quedan bloques. Esta es LA UNICA seccion critica de la fase de conteo:
 * es corta (una comparacion y un incremento) y nunca se queda esperando. */
static int task_counter_next(TaskCounter *tc) {
    int idx;
    pthread_mutex_lock(&tc->mutex);
    idx = (tc->next_index < tc->num_blocks) ? (int)tc->next_index++ : -1;
    pthread_mutex_unlock(&tc->mutex);
    return idx;
}

static void block_range(uint64_t file_size, uint32_t block_size, size_t idx,
                         off_t *out_start, size_t *out_len) {
    off_t start = (off_t)((uint64_t)idx * block_size);
    uint64_t remaining = file_size - (uint64_t)start;
    size_t len = (remaining < block_size) ? (size_t)remaining : (size_t)block_size;
    *out_start = start;
    *out_len = len;
}

typedef struct {
    TaskCounter  *tc;
    unsigned long freq[HUFFMAN_ALPHABET]; /* resultado LOCAL de este hilo */
    int           error;
} FreqWorkerArg;

static void *freq_worker(void *arg_) {
    FreqWorkerArg *arg = (FreqWorkerArg *)arg_;
    memset(arg->freq, 0, sizeof(arg->freq));

    unsigned char *buf = malloc(arg->tc->block_size);
    if (buf == NULL) { arg->error = 1; return NULL; }

    int idx;
    while ((idx = task_counter_next(arg->tc)) != -1) {
        off_t start; size_t len;
        block_range(arg->tc->file_size, arg->tc->block_size, (size_t)idx, &start, &len);

        if (pread_full(arg->tc->fd, buf, len, start) != (ssize_t)len) {
            arg->error = 1;
            break;
        }
        /* Esta es la ITERACION que se paraleliza en esta fase: recorrer los
         * bytes del bloque asignado y acumular su frecuencia. */
        for (size_t i = 0; i < len; i++) arg->freq[buf[i]]++;
    }

    free(buf);
    return NULL;
}

/* ====================================================================================
 * Fase 2 (compresion): codificacion paralela por bloques + escritura
 * ordenada por un unico hilo coordinador (productor-consumidor con
 * variable de condicion, SIN espera activa).
 * ====================================================================================
 * - Los hilos trabajadores son los PRODUCTORES: toman el siguiente bloque
 *   pendiente (misma tecnica de reparto dinamico que en la fase 1, con su
 *   propio mutex 'task_mutex'), lo codifican con la tabla de codigos
 *   (compartida, de SOLO LECTURA -> no necesita proteccion) y dejan el
 *   resultado listo en su casilla de 'slots[idx]'.
 * - El hilo coordinador (el mismo hilo que corre coordinator_compress_main)
 *   es el CONSUMIDOR: recorre las casillas en orden 0,1,2,... y para cada
 *   una espera con pthread_cond_wait() -- bloqueando el hilo, sin consumir
 *   CPU -- hasta que ESA casilla especifica este lista, sin importar el
 *   orden real en que los trabajadores terminaron. Asi se garantiza que los
 *   bloques queden en el archivo de salida en el orden secuencial correcto
 *   aunque se hayan codificado en paralelo y fuera de orden.
 * ==================================================================================== */
typedef struct {
    int            ready;
    unsigned char *data;      /* buffer heap con el bloque ya codificado */
    size_t         byte_len;
    size_t         bit_count;
    uint64_t       original_len;
} EncodeBlockSlot;

typedef struct {
    int          fd_in;
    uint64_t     file_size;
    uint32_t     block_size;
    size_t       num_blocks;
    const HuffCode *codes;

    pthread_mutex_t task_mutex; /* protege next_to_encode (reparto de bloques) */
    size_t          next_to_encode;

    pthread_mutex_t out_mutex;  /* protege 'slots' y 'error' */
    pthread_cond_t  out_cond;
    EncodeBlockSlot *slots;
    int             error;
} EncodeShared;

static void *encode_worker(void *arg_) {
    EncodeShared *sh = (EncodeShared *)arg_;
    unsigned char *buf = malloc(sh->block_size);
    if (buf == NULL) {
        pthread_mutex_lock(&sh->out_mutex);
        sh->error = 1;
        pthread_cond_broadcast(&sh->out_cond);
        pthread_mutex_unlock(&sh->out_mutex);
        return NULL;
    }

    for (;;) {
        pthread_mutex_lock(&sh->task_mutex);
        int stop = (sh->next_to_encode >= sh->num_blocks);
        size_t idx = stop ? 0 : sh->next_to_encode++;
        pthread_mutex_unlock(&sh->task_mutex);
        if (stop) break;

        off_t start; size_t len;
        block_range(sh->file_size, sh->block_size, idx, &start, &len);

        if (pread_full(sh->fd_in, buf, len, start) != (ssize_t)len) {
            pthread_mutex_lock(&sh->out_mutex);
            sh->error = 1;
            pthread_cond_broadcast(&sh->out_cond);
            pthread_mutex_unlock(&sh->out_mutex);
            break;
        }

        BitWriter bw;
        bitwriter_init(&bw);
        int ok = 1;
        /* Iteracion paralelizada de esta fase: emitir el codigo Huffman de
         * cada byte del bloque asignado a este hilo. */
        for (size_t i = 0; i < len && ok; i++) {
            HuffCode c = sh->codes[buf[i]];
            if (bitwriter_put_bits(&bw, c.bits, c.length) == -1) ok = 0;
        }
        if (!ok) {
            bitwriter_free(&bw);
            pthread_mutex_lock(&sh->out_mutex);
            sh->error = 1;
            pthread_cond_broadcast(&sh->out_cond);
            pthread_mutex_unlock(&sh->out_mutex);
            break;
        }

        pthread_mutex_lock(&sh->out_mutex);
        sh->slots[idx].data = bw.data;
        sh->slots[idx].byte_len = bw.byte_len + (bw.bit_pos > 0 ? 1 : 0);
        sh->slots[idx].bit_count = bitwriter_total_bits(&bw);
        sh->slots[idx].original_len = len;
        sh->slots[idx].ready = 1;
        pthread_cond_broadcast(&sh->out_cond); /* despierta al coordinador si esperaba esta casilla */
        pthread_mutex_unlock(&sh->out_mutex);
    }

    free(buf);
    return NULL;
}

static int encode_and_write_blocks(CompressJob *job, int fd_in, int fd_out, uint64_t file_size,
                                    uint32_t block_size, size_t num_blocks,
                                    const HuffCode *codes, int num_workers) {
    EncodeShared sh;
    memset(&sh, 0, sizeof(sh));
    sh.fd_in = fd_in;
    sh.file_size = file_size;
    sh.block_size = block_size;
    sh.num_blocks = num_blocks;
    sh.codes = codes;
    pthread_mutex_init(&sh.task_mutex, NULL);
    pthread_mutex_init(&sh.out_mutex, NULL);
    pthread_cond_init(&sh.out_cond, NULL);

    sh.slots = calloc(num_blocks, sizeof(EncodeBlockSlot));
    if (sh.slots == NULL) {
        pthread_mutex_destroy(&sh.task_mutex);
        pthread_mutex_destroy(&sh.out_mutex);
        pthread_cond_destroy(&sh.out_cond);
        return -1;
    }

    /* Pool de hilos EN EL HEAP: ver justificacion detallada en pcompress.h
     * (cabecera del archivo) sobre por que un job/pool asincrono no puede
     * vivir en el stack de quien lo crea. */
    pthread_t *tids = malloc(sizeof(pthread_t) * (size_t)num_workers);
    if (tids == NULL) {
        free(sh.slots);
        pthread_mutex_destroy(&sh.task_mutex);
        pthread_mutex_destroy(&sh.out_mutex);
        pthread_cond_destroy(&sh.out_cond);
        return -1;
    }

    int spawned = 0;
    for (int i = 0; i < num_workers; i++) {
        if (pthread_create(&tids[i], NULL, encode_worker, &sh) != 0) break;
        spawned++;
    }

    int rc = (spawned == num_workers) ? 0 : -1;

    /* Bucle del hilo coordinador: consumidor ordenado, sin espera activa. */
    for (size_t next_to_write = 0; rc == 0 && next_to_write < num_blocks; next_to_write++) {
        pthread_mutex_lock(&sh.out_mutex);
        while (!sh.slots[next_to_write].ready && !sh.error) {
            pthread_cond_wait(&sh.out_cond, &sh.out_mutex); /* bloquea el hilo, no consume CPU */
        }
        int block_ready = sh.slots[next_to_write].ready;
        EncodeBlockSlot slot = sh.slots[next_to_write];
        pthread_mutex_unlock(&sh.out_mutex);

        if (!block_ready) { rc = -1; break; }

        int ok = write_u64(fd_out, slot.original_len) == 0 &&
                 write_u64(fd_out, (uint64_t)slot.bit_count) == 0 &&
                 (slot.byte_len == 0 || write_full(fd_out, slot.data, slot.byte_len) == (ssize_t)slot.byte_len);
        free(slot.data);
        if (!ok) { rc = -1; break; }

        job_set_progress(job, 30 + (int)(((next_to_write + 1) * 70) / num_blocks));
    }

    for (int i = 0; i < spawned; i++) pthread_join(tids[i], NULL);
    free(tids);
    if (sh.error) rc = -1;

    free(sh.slots);
    pthread_mutex_destroy(&sh.task_mutex);
    pthread_mutex_destroy(&sh.out_mutex);
    pthread_cond_destroy(&sh.out_cond);
    return rc;
}

/* ====================================================================================
 * Descompresion: decodificacion paralela SIN necesidad de orden.
 * ====================================================================================
 * Ya que el encabezado guarda, para cada bloque, su longitud original y su
 * cantidad de bits comprimidos, el hilo coordinador puede calcular POR
 * ADELANTADO (antes de lanzar los trabajadores) el offset exacto donde cada
 * bloque decodificado debe escribirse en el archivo de salida (suma
 * acumulada de tamanos originales). Con ese offset conocido de antemano,
 * cada trabajador puede usar pwrite() posicional para escribir su bloque de
 * forma independiente, sin ningun mutex ni variable de condicion de por
 * medio: cada uno escribe en una region disjunta del archivo. El arbol de
 * Huffman se comparte entre todos los hilos, pero solo se LEE (nunca se
 * modifica) durante la decodificacion, por lo que tampoco requiere lock.
 * ==================================================================================== */
typedef struct {
    uint64_t original_len;
    uint64_t bit_count;
    uint64_t payload_offset; /* posicion en el archivo .huf donde inicia el payload */
    uint64_t output_offset;  /* posicion en el archivo de salida para este bloque */
} DecodeBlockMeta;

typedef struct {
    int fd_in;
    int fd_out;
    const HuffNode *tree;
    DecodeBlockMeta *blocks;
    size_t num_blocks;
    CompressJob *job;

    pthread_mutex_t task_mutex;
    size_t next_to_decode;

    pthread_mutex_t stat_mutex; /* protege 'completed' y 'error' */
    size_t completed;
    int error;
} DecodeShared;

static void *decode_worker(void *arg_) {
    DecodeShared *sh = (DecodeShared *)arg_;

    for (;;) {
        pthread_mutex_lock(&sh->task_mutex);
        int stop = (sh->next_to_decode >= sh->num_blocks);
        size_t idx = stop ? 0 : sh->next_to_decode++;
        pthread_mutex_unlock(&sh->task_mutex);
        if (stop) break;

        pthread_mutex_lock(&sh->stat_mutex);
        int already_failed = sh->error;
        pthread_mutex_unlock(&sh->stat_mutex);
        if (already_failed) break; /* no perder tiempo si otro hilo ya fallo */

        DecodeBlockMeta m = sh->blocks[idx];
        size_t payload_bytes = (size_t)((m.bit_count + 7) / 8);
        unsigned char *payload = NULL;
        unsigned char *outbuf = NULL;
        int local_err = 0;

        if (payload_bytes > 0) {
            payload = malloc(payload_bytes);
            if (payload == NULL ||
                pread_full(sh->fd_in, payload, payload_bytes, (off_t)m.payload_offset) != (ssize_t)payload_bytes) {
                local_err = 1;
            }
        }

        if (!local_err && m.original_len > 0) {
            outbuf = malloc((size_t)m.original_len);
            if (outbuf == NULL) {
                local_err = 1;
            } else {
                BitReader br;
                bitreader_init(&br, payload, (size_t)m.bit_count);
                /* Iteracion paralelizada de la descompresion: recorrer los
                 * bits del bloque asignado, caminando el arbol compartido
                 * (solo lectura) hasta cada hoja. */
                for (uint64_t produced = 0; produced < m.original_len && !local_err; produced++) {
                    const HuffNode *node = sh->tree;
                    if (node != NULL && node->is_leaf) {
                        outbuf[produced] = node->symbol;
                        continue;
                    }
                    while (node != NULL && !node->is_leaf) {
                        int bit;
                        if (bitreader_get_bit(&br, &bit) == -1) { local_err = 1; break; }
                        node = bit ? node->right : node->left;
                    }
                    if (local_err || node == NULL) { local_err = 1; break; }
                    outbuf[produced] = node->symbol;
                }
                if (!local_err &&
                    pwrite_full(sh->fd_out, outbuf, (size_t)m.original_len, (off_t)m.output_offset)
                        != (ssize_t)m.original_len) {
                    local_err = 1;
                }
            }
        }

        free(payload);
        free(outbuf);

        pthread_mutex_lock(&sh->stat_mutex);
        if (local_err) {
            sh->error = 1;
        } else {
            sh->completed++;
        }
        size_t done = sh->completed;
        pthread_mutex_unlock(&sh->stat_mutex);

        if (local_err) break;
        job_set_progress(sh->job, 25 + (int)((done * 75) / sh->num_blocks));
    }

    return NULL;
}

/* ====================================================================================
 * Hilo coordinador de compresion (cuerpo completo del job JOB_KIND_COMPRESS)
 * ==================================================================================== */
static void *coordinator_compress_main(void *arg) {
    CompressJob *job = (CompressJob *)arg;

    int fd_in = open(job->input_path, O_RDONLY);
    if (fd_in == -1) { job_set_error(job, "no se pudo abrir el archivo de entrada"); return NULL; }

    struct stat st;
    if (fstat(fd_in, &st) == -1) {
        close(fd_in);
        job_set_error(job, "fstat fallo sobre el archivo de entrada");
        return NULL;
    }
    uint64_t file_size = (uint64_t)st.st_size;
    job->input_size = (long long)file_size;

    uint32_t block_size = DEFAULT_BLOCK_SIZE;
    size_t num_blocks = (file_size == 0) ? 0 : (size_t)((file_size + block_size - 1) / block_size);

    unsigned long freq[HUFFMAN_ALPHABET];
    memset(freq, 0, sizeof(freq));

    if (num_blocks > 0) {
        TaskCounter tc;
        task_counter_init(&tc, fd_in, file_size, block_size, num_blocks);

        int nw = job->num_workers;
        if ((size_t)nw > num_blocks) nw = (int)num_blocks;
        if (nw < 1) nw = 1;

        pthread_t *tids = malloc(sizeof(pthread_t) * (size_t)nw);
        FreqWorkerArg *args = malloc(sizeof(FreqWorkerArg) * (size_t)nw);
        if (tids == NULL || args == NULL) {
            free(tids); free(args);
            pthread_mutex_destroy(&tc.mutex);
            close(fd_in);
            job_set_error(job, "malloc fallo al preparar el conteo de frecuencias");
            return NULL;
        }

        int spawned = 0;
        for (int i = 0; i < nw; i++) {
            args[i].tc = &tc;
            args[i].error = 0;
            if (pthread_create(&tids[i], NULL, freq_worker, &args[i]) != 0) break;
            spawned++;
        }
        for (int i = 0; i < spawned; i++) pthread_join(tids[i], NULL);

        int freq_error = (spawned != nw);
        for (int i = 0; i < spawned; i++) {
            if (args[i].error) freq_error = 1;
            for (int b = 0; b < HUFFMAN_ALPHABET; b++) freq[b] += args[i].freq[b]; /* reduccion local -> global */
        }

        free(tids);
        free(args);
        pthread_mutex_destroy(&tc.mutex);

        if (freq_error) {
            close(fd_in);
            job_set_error(job, "fallo el conteo de frecuencias en uno o mas hilos");
            return NULL;
        }
    }
    job_set_progress(job, 30);

    HuffNode *tree = (num_blocks > 0) ? huffman_build_tree(freq) : NULL;
    HuffCode codes[HUFFMAN_ALPHABET];
    if (tree != NULL) huffman_build_codes(tree, codes);
    else memset(codes, 0, sizeof(codes));

    int fd_out = open(job->output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out == -1) {
        huffman_free_tree(tree);
        close(fd_in);
        job_set_error(job, "no se pudo crear el archivo de salida");
        return NULL;
    }

    BitWriter tree_bits;
    bitwriter_init(&tree_bits);
    if (tree != NULL && huffman_serialize_tree(tree, &tree_bits) == -1) {
        bitwriter_free(&tree_bits);
        huffman_free_tree(tree);
        close(fd_in); close(fd_out);
        job_set_error(job, "fallo al serializar el arbol de Huffman");
        return NULL;
    }
    uint32_t tree_bit_count = (uint32_t)bitwriter_total_bits(&tree_bits);
    size_t tree_byte_len = tree_bits.byte_len + (tree_bits.bit_pos > 0 ? 1 : 0);

    int hdr_ok =
        write_u32(fd_out, HUF_MAGIC) == 0 &&
        write_u32(fd_out, HUF_VERSION) == 0 &&
        write_u32(fd_out, (uint32_t)num_blocks) == 0 &&
        write_u64(fd_out, file_size) == 0 &&
        write_u32(fd_out, block_size) == 0 &&
        write_u32(fd_out, tree_bit_count) == 0 &&
        (tree_byte_len == 0 || write_full(fd_out, tree_bits.data, tree_byte_len) == (ssize_t)tree_byte_len);

    bitwriter_free(&tree_bits);

    if (!hdr_ok) {
        huffman_free_tree(tree);
        close(fd_in); close(fd_out);
        job_set_error(job, "fallo al escribir el encabezado del archivo comprimido");
        return NULL;
    }

    int rc = 0;
    if (num_blocks > 0) {
        int nw = job->num_workers;
        if ((size_t)nw > num_blocks) nw = (int)num_blocks;
        if (nw < 1) nw = 1;
        rc = encode_and_write_blocks(job, fd_in, fd_out, file_size, block_size, num_blocks, codes, nw);
    }

    huffman_free_tree(tree);
    close(fd_in);

    if (rc == -1) {
        close(fd_out);
        job_set_error(job, "fallo la codificacion o escritura ordenada de bloques");
        return NULL;
    }

    struct stat out_st;
    if (fstat(fd_out, &out_st) == 0) job->output_size = (long long)out_st.st_size;
    close(fd_out);

    job_set_progress(job, 100);
    job_set_state(job, JOB_STATE_DONE);
    return NULL;
}

/* ====================================================================================
 * Hilo coordinador de descompresion (cuerpo completo del job JOB_KIND_DECOMPRESS)
 * ==================================================================================== */
static void *coordinator_decompress_main(void *arg) {
    CompressJob *job = (CompressJob *)arg;

    int fd_in = open(job->input_path, O_RDONLY);
    if (fd_in == -1) { job_set_error(job, "no se pudo abrir el archivo .huf"); return NULL; }

    struct stat in_st;
    if (fstat(fd_in, &in_st) == 0) job->input_size = (long long)in_st.st_size;

    uint32_t magic, version, num_blocks_u32, block_size, tree_bit_count;
    uint64_t original_size;
    if (read_u32(fd_in, &magic) == -1 || magic != HUF_MAGIC ||
        read_u32(fd_in, &version) == -1 || version != HUF_VERSION ||
        read_u32(fd_in, &num_blocks_u32) == -1 ||
        read_u64(fd_in, &original_size) == -1 ||
        read_u32(fd_in, &block_size) == -1 ||
        read_u32(fd_in, &tree_bit_count) == -1) {
        close(fd_in);
        job_set_error(job, "formato .huf invalido o corrupto");
        return NULL;
    }
    (void)block_size;
    size_t num_blocks = num_blocks_u32;

    HuffNode *tree = NULL;
    if (tree_bit_count > 0) {
        size_t tree_byte_len = ((size_t)tree_bit_count + 7) / 8;
        unsigned char *tree_bytes = malloc(tree_byte_len);
        if (tree_bytes == NULL || read_full(fd_in, tree_bytes, tree_byte_len) != (ssize_t)tree_byte_len) {
            free(tree_bytes);
            close(fd_in);
            job_set_error(job, "encabezado del arbol de Huffman corrupto");
            return NULL;
        }
        BitReader br;
        bitreader_init(&br, tree_bytes, tree_bit_count);
        tree = huffman_deserialize_tree(&br);
        free(tree_bytes);
        if (tree == NULL) {
            close(fd_in);
            job_set_error(job, "no se pudo reconstruir el arbol de Huffman");
            return NULL;
        }
    }
    job_set_progress(job, 15);

    DecodeBlockMeta *blocks = NULL;
    if (num_blocks > 0) {
        blocks = malloc(sizeof(DecodeBlockMeta) * num_blocks);
        if (blocks == NULL) {
            huffman_free_tree(tree); close(fd_in);
            job_set_error(job, "malloc fallo al preparar la descompresion");
            return NULL;
        }

        uint64_t running_offset = 0;
        int meta_ok = 1;
        for (size_t i = 0; i < num_blocks; i++) {
            uint64_t orig_len, bit_count;
            if (read_u64(fd_in, &orig_len) == -1 || read_u64(fd_in, &bit_count) == -1) { meta_ok = 0; break; }
            uint64_t payload_bytes = (bit_count + 7) / 8;
            off_t payload_offset = lseek(fd_in, 0, SEEK_CUR);
            if (payload_offset == (off_t)-1) { meta_ok = 0; break; }

            blocks[i].original_len = orig_len;
            blocks[i].bit_count = bit_count;
            blocks[i].payload_offset = (uint64_t)payload_offset;
            blocks[i].output_offset = running_offset;
            running_offset += orig_len;

            if (lseek(fd_in, (off_t)payload_bytes, SEEK_CUR) == (off_t)-1) { meta_ok = 0; break; }
        }
        if (!meta_ok) {
            free(blocks); huffman_free_tree(tree); close(fd_in);
            job_set_error(job, "metadatos de bloque corruptos");
            return NULL;
        }
    }
    job_set_progress(job, 25);

    int fd_out = open(job->output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out == -1) {
        free(blocks); huffman_free_tree(tree); close(fd_in);
        job_set_error(job, "no se pudo crear el archivo de salida");
        return NULL;
    }
    if (original_size > 0 && ftruncate(fd_out, (off_t)original_size) == -1) {
        free(blocks); huffman_free_tree(tree); close(fd_in); close(fd_out);
        job_set_error(job, "ftruncate fallo al reservar el archivo de salida");
        return NULL;
    }

    int rc = 0;
    if (num_blocks > 0) {
        DecodeShared sh;
        memset(&sh, 0, sizeof(sh));
        sh.fd_in = fd_in;
        sh.fd_out = fd_out;
        sh.tree = tree;
        sh.blocks = blocks;
        sh.num_blocks = num_blocks;
        sh.job = job;
        pthread_mutex_init(&sh.task_mutex, NULL);
        pthread_mutex_init(&sh.stat_mutex, NULL);

        int nw = job->num_workers;
        if ((size_t)nw > num_blocks) nw = (int)num_blocks;
        if (nw < 1) nw = 1;

        pthread_t *tids = malloc(sizeof(pthread_t) * (size_t)nw);
        int spawned = 0;
        if (tids != NULL) {
            for (int i = 0; i < nw; i++) {
                if (pthread_create(&tids[i], NULL, decode_worker, &sh) != 0) break;
                spawned++;
            }
        }
        if (spawned == 0) rc = -1;

        for (int i = 0; i < spawned; i++) pthread_join(tids[i], NULL);
        free(tids);
        if (sh.error) rc = -1;

        pthread_mutex_destroy(&sh.task_mutex);
        pthread_mutex_destroy(&sh.stat_mutex);
    }

    free(blocks);
    huffman_free_tree(tree);
    close(fd_in);

    if (rc == -1) {
        close(fd_out);
        job_set_error(job, "fallo la decodificacion paralela de bloques");
        return NULL;
    }

    struct stat out_st;
    if (fstat(fd_out, &out_st) == 0) job->output_size = (long long)out_st.st_size;
    close(fd_out);

    job_set_progress(job, 100);
    job_set_state(job, JOB_STATE_DONE);
    return NULL;
}

/* ====================================================================================
 * Hilo "ticker": imprime el progreso en tiempo real sin que el usuario
 * tenga que pedirlo. Es puramente una comodidad de interfaz -- el muestreo
 * periodico (nanosleep) que hace este hilo NO es el mecanismo de
 * sincronizacion real del programa (ese vive en los mutex/condvar de arriba
 * sobre datos compartidos); aqui solo se lee progreso/estado ya protegidos
 * por 'state_mutex' a traves de los getters publicos.
 * ==================================================================================== */
static void *progress_ticker(void *arg_) {
    CompressJob *job = (CompressJob *)arg_;
    int last_shown = -1;

    for (;;) {
        struct timespec ts = { 0, 400L * 1000 * 1000 }; /* 400 ms */
        nanosleep(&ts, NULL);

        JobState st = job_get_state(job);
        int p = job_get_progress(job);

        if (p != last_shown || st != JOB_STATE_RUNNING) {
            ui_print_lock();
            if (st == JOB_STATE_RUNNING) {
                printf("\n[job] %s '%s' -> '%s': %d%%\n",
                       job->kind == JOB_KIND_COMPRESS ? "comprimiendo" : "descomprimiendo",
                       job->input_path, job->output_path, p);
            } else if (st == JOB_STATE_DONE) {
                printf("\n[job] completado: '%s' (%lld bytes) -> '%s' (%lld bytes)\n",
                       job->input_path, job->input_size, job->output_path, job->output_size);
            } else if (st == JOB_STATE_ERROR) {
                printf("\n[job] ERROR: %s\n", job->error_msg);
            }
            printf("edi> ");
            fflush(stdout);
            ui_print_unlock();
            last_shown = p;
        }

        if (st != JOB_STATE_RUNNING) break;
    }
    return NULL;
}

/* ====================================================================================
 * API publica
 * ==================================================================================== */
static CompressJob *job_alloc_common(JobKind kind, const char *input_path,
                                      const char *output_path, int num_workers) {
    /* calloc: el job entero vive en el HEAP porque job_start_* retorna de
     * inmediato mientras el hilo coordinador sigue corriendo y escribiendo
     * sobre esta memoria mucho despues (ver justificacion completa en
     * pcompress.h). */
    CompressJob *job = calloc(1, sizeof(CompressJob));
    if (job == NULL) return NULL;

    job->kind = kind;
    job->state = JOB_STATE_RUNNING;
    job->progress = 0;
    pthread_mutex_init(&job->state_mutex, NULL);
    snprintf(job->input_path, sizeof(job->input_path), "%s", input_path);
    snprintf(job->output_path, sizeof(job->output_path), "%s", output_path);
    job->num_workers = num_workers < 1 ? 1
                     : (num_workers > MAX_WORKERS_HARD_CAP ? MAX_WORKERS_HARD_CAP : num_workers);
    job->input_size = -1;
    job->output_size = -1;
    return job;
}

CompressJob *job_start_compress(const char *input_path, const char *output_path, int num_workers) {
    CompressJob *job = job_alloc_common(JOB_KIND_COMPRESS, input_path, output_path, num_workers);
    if (job == NULL) return NULL;
    if (pthread_create(&job->coordinator, NULL, coordinator_compress_main, job) != 0) {
        pthread_mutex_destroy(&job->state_mutex);
        free(job);
        return NULL;
    }
    if (pthread_create(&job->ticker, NULL, progress_ticker, job) == 0) job->ticker_started = 1;
    return job;
}

CompressJob *job_start_decompress(const char *input_path, const char *output_path, int num_workers) {
    CompressJob *job = job_alloc_common(JOB_KIND_DECOMPRESS, input_path, output_path, num_workers);
    if (job == NULL) return NULL;
    if (pthread_create(&job->coordinator, NULL, coordinator_decompress_main, job) != 0) {
        pthread_mutex_destroy(&job->state_mutex);
        free(job);
        return NULL;
    }
    if (pthread_create(&job->ticker, NULL, progress_ticker, job) == 0) job->ticker_started = 1;
    return job;
}

int job_is_running(CompressJob *job) {
    if (job == NULL) return 0;
    pthread_mutex_lock(&job->state_mutex);
    int running = (job->state == JOB_STATE_RUNNING);
    pthread_mutex_unlock(&job->state_mutex);
    return running;
}

int job_get_progress(CompressJob *job) {
    if (job == NULL) return 0;
    pthread_mutex_lock(&job->state_mutex);
    int p = job->progress;
    pthread_mutex_unlock(&job->state_mutex);
    return p;
}

JobKind job_get_kind(CompressJob *job) { return job->kind; } /* fijado una vez, no muta */

JobState job_get_state(CompressJob *job) {
    if (job == NULL) return JOB_STATE_NONE;
    pthread_mutex_lock(&job->state_mutex);
    JobState s = job->state;
    pthread_mutex_unlock(&job->state_mutex);
    return s;
}

const char *job_get_input_path(CompressJob *job)  { return job ? job->input_path : ""; }
const char *job_get_output_path(CompressJob *job) { return job ? job->output_path : ""; }
const char *job_get_error(CompressJob *job)       { return job ? job->error_msg : ""; }
long long job_get_input_size(CompressJob *job)    { return job ? job->input_size : -1; }
long long job_get_output_size(CompressJob *job)   { return job ? job->output_size : -1; }

int job_blocks_path(CompressJob *job, const char *path) {
    if (job == NULL || path == NULL) return 0;
    if (!job_is_running(job)) return 0;
    return strcmp(job->input_path, path) == 0 || strcmp(job->output_path, path) == 0;
}

void job_join_and_free(CompressJob *job) {
    if (job == NULL) return;
    pthread_join(job->coordinator, NULL);
    if (job->ticker_started) pthread_join(job->ticker, NULL);
    pthread_mutex_destroy(&job->state_mutex);
    free(job);
}
