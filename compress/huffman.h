#ifndef HUFFMAN_H
#define HUFFMAN_H

/*
 * ====================================================================================
 * Codificacion de Huffman: arbol, tabla de codigos y E/S de bits en memoria.
 * ====================================================================================
 * Este modulo es deliberadamente SECUENCIAL y de un solo hilo: construir un
 * arbol de Huffman a partir de una tabla de 256 frecuencias es una operacion
 * O(256^2) en el peor caso (con la seleccion ingenua de dos minimos que se usa
 * aqui), imperceptible frente al tiempo de E/S de un archivo real. Paralelizar
 * esta etapa no aportaria nada y complicaria la logica sin necesidad; el
 * paralelismo real del proyecto vive en pcompress.c, en la ITERACION sobre los
 * bloques del archivo (conteo de frecuencias y codificacion), que es lo unico
 * que efectivamente se reparte entre hilos.
 *
 * El arbol se SERIALIZA explicitamente dentro del archivo comprimido (en vez
 * de reconstruirlo en el descompresor a partir de las frecuencias). Esto evita
 * por completo cualquier problema de desempate no determinista entre
 * compresor y descompresor: el descompresor nunca "adivina" la forma del
 * arbol, la lee tal cual la escribio el compresor.
 */

#include <stdint.h>
#include <stddef.h>

#define HUFFMAN_ALPHABET 256

typedef struct HuffNode {
    struct HuffNode *left;
    struct HuffNode *right;
    unsigned long    freq;
    unsigned char    symbol;   /* valido solo si is_leaf */
    int              is_leaf;
} HuffNode;

typedef struct {
    uint32_t bits;    /* codigo, MSB primero, solo los 'length' bits bajos importan */
    uint8_t  length;  /* 0 = el simbolo no aparece en el bloque/archivo */
} HuffCode;

/* ------------------------------------------------------------------------
 * Escritor/lector de bits en memoria (orden MSB-first dentro de cada byte).
 * Cada bloque del archivo se codifica en su PROPIO BitWriter y se alinea a
 * byte al terminar, para que los bloques puedan procesarse y escribirse en
 * paralelo/independientemente sin compartir un puntero de bit global.
 * ------------------------------------------------------------------------ */
typedef struct {
    unsigned char *data;
    size_t         capacity;  /* bytes reservados en 'data' */
    size_t         byte_len;  /* bytes completamente llenos */
    int            bit_pos;   /* 0..7: proximo bit libre dentro de data[byte_len] */
} BitWriter;

typedef struct {
    const unsigned char *data;
    size_t total_bits;
    size_t bit_index;
} BitReader;

void bitwriter_init(BitWriter *bw);
int  bitwriter_put_bit(BitWriter *bw, int bit);
int  bitwriter_put_bits(BitWriter *bw, uint32_t bits, int length);
size_t bitwriter_total_bits(const BitWriter *bw);
void bitwriter_free(BitWriter *bw);

void bitreader_init(BitReader *br, const unsigned char *data, size_t total_bits);
int  bitreader_get_bit(BitReader *br, int *out_bit); /* retorna -1 en EOF, 0 ok */

/* Construccion del arbol a partir de una tabla de frecuencias ya reducida
 * (ver pcompress.c: cada hilo cuenta en su propia copia local y el hilo
 * coordinador suma -reduce- esas copias antes de llamar aqui). */
HuffNode *huffman_build_tree(const unsigned long freq[HUFFMAN_ALPHABET]);
void      huffman_free_tree(HuffNode *root);
void      huffman_build_codes(const HuffNode *root, HuffCode codes[HUFFMAN_ALPHABET]);

/* Serializacion del arbol en preorden: 1 bit de tipo (1=hoja,0=interno) y,
 * si es hoja, 8 bits con el simbolo. Se escribe una sola vez en el
 * encabezado del archivo comprimido. */
int       huffman_serialize_tree(const HuffNode *root, BitWriter *bw);
HuffNode *huffman_deserialize_tree(BitReader *br);

#endif /* HUFFMAN_H */
