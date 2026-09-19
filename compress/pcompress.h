#ifndef PCOMPRESS_H
#define PCOMPRESS_H

/*
 * ====================================================================================
 * Compresor/descompresor Huffman concurrente, en segundo plano (background job)
 * ====================================================================================
 * API publica consumida por el editor (editor.c). Un "CompressJob" encapsula
 * una tarea de compresion o descompresion que corre en su propio HILO
 * COORDINADOR, el cual a su vez reparte trabajo entre un pool de hilos
 * trabajadores para las fases paralelizables (conteo de frecuencias y
 * codificacion/decodificacion por bloques).
 *
 * Por que el CompressJob vive obligatoriamente en el HEAP (malloc), y no como
 * variable local de quien lo crea:
 *   job_start_compress()/job_start_decompress() lanzan el hilo coordinador y
 *   RETORNAN DE INMEDIATO (esa es la esencia de "background worker" que pide
 *   el enunciado: no bloquear el editor). Si el struct del job estuviera en
 *   el stack de la funcion que lo crea, esa memoria seria invalida en cuanto
 *   esa funcion retornara -- pero el hilo coordinador sigue vivo y
 *   escribiendo sobre ese mismo struct mucho despues. Por eso el job (y los
 *   arreglos de pthread_t / argumentos de cada hilo trabajador dentro de el)
 *   se reservan con malloc y se liberan explicitamente en job_join_and_free().
 */

typedef enum {
    JOB_STATE_NONE = 0,
    JOB_STATE_RUNNING,
    JOB_STATE_DONE,
    JOB_STATE_ERROR
} JobState;

typedef enum {
    JOB_KIND_COMPRESS,
    JOB_KIND_DECOMPRESS
} JobKind;

typedef struct CompressJob CompressJob; /* opaco: definido en pcompress.c */

/* Lanza la tarea en segundo plano. Retorna NULL si ni siquiera pudo crearse
 * el hilo coordinador (p.ej. malloc fallida); en ese caso no queda ningun
 * recurso pendiente por liberar. */
CompressJob *job_start_compress(const char *input_path, const char *output_path, int num_workers);
CompressJob *job_start_decompress(const char *input_path, const char *output_path, int num_workers);

int         job_is_running(CompressJob *job);
int         job_get_progress(CompressJob *job);      /* 0..100 */
JobKind     job_get_kind(CompressJob *job);
JobState    job_get_state(CompressJob *job);
const char *job_get_input_path(CompressJob *job);
const char *job_get_output_path(CompressJob *job);
const char *job_get_error(CompressJob *job);
long long   job_get_input_size(CompressJob *job);
long long   job_get_output_size(CompressJob *job);   /* valido solo cuando termina */

/* 1 si el job sigue corriendo y 'path' coincide con su archivo de entrada o
 * de salida (comparacion textual simple). Usado por el editor para impedir
 * ediciones concurrentes sobre un archivo que esta siendo leido/escrito por
 * el hilo coordinador -- la condicion de carrera explicita que pide evitar
 * el enunciado. */
int job_blocks_path(CompressJob *job, const char *path);

/* pthread_join() del hilo coordinador (y de su hilo "ticker" de progreso) y
 * liberacion de toda la memoria del job. Debe llamarse exactamente una vez
 * por cada job creado, tipicamente cuando ya termino (join/detach limpio),
 * o al cerrar el editor ('q') para no dejar hilos huerfanos. */
void job_join_and_free(CompressJob *job);

/* Mutex compartido de impresion: tanto el hilo "ticker" de progreso de un
 * job como el bucle principal del editor (prompt "edi> ") lo usan para que
 * sus salidas por stdout no se intercalen a mitad de linea. */
void ui_print_lock(void);
void ui_print_unlock(void);

#endif /* PCOMPRESS_H */
