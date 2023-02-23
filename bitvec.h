/*
 * Bitvector library by Claire Wagner
 * Reference: CSCI 245 Project 5
 */

typedef struct _bitvec
{
  size_t nbits;
  size_t nbytes;
  unsigned char *vec;
} bitvec_t;

int bitvec_init(bitvec_t *bv, size_t num_bytes);
void bitvec_destroy(bitvec_t *bv);

int bitvec_get(bitvec_t *bv, int i);
void bitvec_set(bitvec_t *bv, int i);
void bitvec_clear(bitvec_t *bv, int i);

int bitvec_first_cleared_index(bitvec_t *bv);
