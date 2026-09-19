#include "huffman.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================================
 * BitWriter / BitReader
 * ==================================================================================== */
#define BW_INITIAL_CAP 64

void bitwriter_init(BitWriter *bw) {
    memset(bw, 0, sizeof(*bw));
}

static int bitwriter_ensure_capacity(BitWriter *bw, size_t needed_bytes) {
    if (needed_bytes <= bw->capacity) return 0;
    size_t new_cap = bw->capacity == 0 ? BW_INITIAL_CAP : bw->capacity * 2;
    while (new_cap < needed_bytes) new_cap *= 2;
    unsigned char *tmp = realloc(bw->data, new_cap);
    if (tmp == NULL) return -1;
    memset(tmp + bw->capacity, 0, new_cap - bw->capacity);
    bw->data = tmp;
    bw->capacity = new_cap;
    return 0;
}

int bitwriter_put_bit(BitWriter *bw, int bit) {
    if (bitwriter_ensure_capacity(bw, bw->byte_len + 1) == -1) return -1;
    if (bit) {
        bw->data[bw->byte_len] |= (unsigned char)(1u << (7 - bw->bit_pos));
    }
    bw->bit_pos++;
    if (bw->bit_pos == 8) {
        bw->bit_pos = 0;
        bw->byte_len++;
    }
    return 0;
}

int bitwriter_put_bits(BitWriter *bw, uint32_t bits, int length) {
    for (int i = length - 1; i >= 0; i--) {
        int bit = (bits >> i) & 1;
        if (bitwriter_put_bit(bw, bit) == -1) return -1;
    }
    return 0;
}

size_t bitwriter_total_bits(const BitWriter *bw) {
    return bw->byte_len * 8 + (size_t)bw->bit_pos;
}

void bitwriter_free(BitWriter *bw) {
    free(bw->data);
    bw->data = NULL;
    bw->capacity = bw->byte_len = 0;
    bw->bit_pos = 0;
}

void bitreader_init(BitReader *br, const unsigned char *data, size_t total_bits) {
    br->data = data;
    br->total_bits = total_bits;
    br->bit_index = 0;
}

int bitreader_get_bit(BitReader *br, int *out_bit) {
    if (br->bit_index >= br->total_bits) return -1;
    size_t byte_idx = br->bit_index / 8;
    int shift = 7 - (int)(br->bit_index % 8);
    *out_bit = (br->data[byte_idx] >> shift) & 1;
    br->bit_index++;
    return 0;
}

/* ====================================================================================
 * Construccion del arbol de Huffman
 * ====================================================================================
 * Seleccion ingenua de los dos nodos de menor frecuencia en una lista (O(n)
 * por combinacion, O(n^2) en total con n <= 257). Deliberado: el alfabeto es
 * de a lo sumo 256 simbolos + 1 posible simbolo sintetico, por lo que un
 * heap binario no aporta ganancia medible y esta version es mas facil de
 * auditar para la sustentacion.
 */
static HuffNode *new_node(void) {
    HuffNode *n = calloc(1, sizeof(HuffNode));
    if (n == NULL) {
        perror("huffman: calloc");
    }
    return n;
}

void huffman_free_tree(HuffNode *root) {
    if (root == NULL) return;
    huffman_free_tree(root->left);
    huffman_free_tree(root->right);
    free(root);
}

HuffNode *huffman_build_tree(const unsigned long freq[HUFFMAN_ALPHABET]) {
    HuffNode *nodes[HUFFMAN_ALPHABET + 1];
    int n = 0;

    for (int b = 0; b < HUFFMAN_ALPHABET; b++) {
        if (freq[b] > 0) {
            HuffNode *leaf = new_node();
            if (leaf == NULL) goto fail;
            leaf->is_leaf = 1;
            leaf->symbol = (unsigned char)b;
            leaf->freq = freq[b];
            nodes[n++] = leaf;
        }
    }

    /* Caso borde: 0 o 1 simbolo distinto. Un codigo Huffman valido necesita
     * al menos DOS hojas (de lo contrario el unico simbolo recibiria un
     * codigo de longitud 0, que no es decodificable). Se agrega una hoja
     * sintetica de frecuencia 0 con un valor de byte garantizado distinto
     * (complemento a 1: b ^ 0xFF nunca es igual a b). */
    if (n == 0) {
        return NULL; /* bloque/archivo vacio: no hay nada que codificar */
    }
    if (n == 1) {
        HuffNode *dummy = new_node();
        if (dummy == NULL) goto fail;
        dummy->is_leaf = 1;
        dummy->symbol = (unsigned char)(nodes[0]->symbol ^ 0xFF);
        dummy->freq = 0;
        nodes[n++] = dummy;
    }

    while (n > 1) {
        int i_min1 = 0;
        for (int i = 1; i < n; i++) {
            if (nodes[i]->freq < nodes[i_min1]->freq) i_min1 = i;
        }
        HuffNode *min1 = nodes[i_min1];
        nodes[i_min1] = nodes[n - 1];
        n--;

        int i_min2 = 0;
        for (int i = 1; i < n; i++) {
            if (nodes[i]->freq < nodes[i_min2]->freq) i_min2 = i;
        }
        HuffNode *min2 = nodes[i_min2];

        HuffNode *parent = new_node();
        if (parent == NULL) goto fail;
        parent->is_leaf = 0;
        parent->freq = min1->freq + min2->freq;
        parent->left = min1;
        parent->right = min2;

        nodes[i_min2] = parent;
    }

    return nodes[0];

fail:
    for (int i = 0; i < n; i++) huffman_free_tree(nodes[i]);
    return NULL;
}

static void build_codes_rec(const HuffNode *node, HuffCode codes[HUFFMAN_ALPHABET],
                             uint32_t bits, uint8_t length) {
    if (node == NULL) return;
    if (node->is_leaf) {
        /* Caso especial: arbol de una sola hoja (archivo con 1 unico byte
         * distinto tras eliminar el simbolo sintetico ya no puede ocurrir,
         * porque build_tree siempre agrega la hoja dummy). length nunca es 0
         * en un arbol real de >=2 hojas. */
        codes[node->symbol].bits = bits;
        codes[node->symbol].length = length > 0 ? length : 1;
        return;
    }
    build_codes_rec(node->left, codes, (bits << 1) | 0u, (uint8_t)(length + 1));
    build_codes_rec(node->right, codes, (bits << 1) | 1u, (uint8_t)(length + 1));
}

void huffman_build_codes(const HuffNode *root, HuffCode codes[HUFFMAN_ALPHABET]) {
    memset(codes, 0, sizeof(HuffCode) * HUFFMAN_ALPHABET);
    build_codes_rec(root, codes, 0, 0);
}

/* ====================================================================================
 * Serializacion / deserializacion del arbol (preorden)
 * ==================================================================================== */
int huffman_serialize_tree(const HuffNode *root, BitWriter *bw) {
    if (root == NULL) return 0; /* arbol vacio: nada que escribir (archivo vacio) */
    if (root->is_leaf) {
        if (bitwriter_put_bit(bw, 1) == -1) return -1;
        if (bitwriter_put_bits(bw, root->symbol, 8) == -1) return -1;
        return 0;
    }
    if (bitwriter_put_bit(bw, 0) == -1) return -1;
    if (huffman_serialize_tree(root->left, bw) == -1) return -1;
    if (huffman_serialize_tree(root->right, bw) == -1) return -1;
    return 0;
}

HuffNode *huffman_deserialize_tree(BitReader *br) {
    int bit;
    if (bitreader_get_bit(br, &bit) == -1) return NULL;

    HuffNode *node = new_node();
    if (node == NULL) return NULL;

    if (bit == 1) {
        node->is_leaf = 1;
        uint32_t symbol = 0;
        for (int i = 0; i < 8; i++) {
            int b;
            if (bitreader_get_bit(br, &b) == -1) {
                free(node);
                return NULL;
            }
            symbol = (symbol << 1) | (uint32_t)b;
        }
        node->symbol = (unsigned char)symbol;
        return node;
    }

    node->is_leaf = 0;
    node->left = huffman_deserialize_tree(br);
    node->right = huffman_deserialize_tree(br);
    if (node->left == NULL || node->right == NULL) {
        huffman_free_tree(node);
        return NULL;
    }
    return node;
}
