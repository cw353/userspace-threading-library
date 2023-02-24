/*
 * Bitvector library by Claire Wagner
 * Reference: CSCI 245 Project 5
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "bitvec.h"

int bitvec_init(bitvec_t *bv, size_t n)
{
  assert(n > 0);
  bv->nbits = n;
  bv->nbytes = (7 + n)/8;
  bv->vec = calloc(bv->nbytes, sizeof(unsigned char));
  if (!bv->vec) {
    return -1;
  }
  return 0;
}

void bitvec_destroy(bitvec_t *bv)
{
  free(bv->vec);
}

// get value of ith bit
int bitvec_get(bitvec_t *bv, int i)
{
  return (bv->vec[i/8] & (1 << (i%8))) >> (i%8);
}

// set ith bit to 1
void bitvec_set(bitvec_t *bv, int i)
{
  bv->vec[i/8] = bv->vec[i/8] | (1 << (i%8));
}

// set ith bit to 0
void bitvec_clear(bitvec_t *bv, int i)
{
  bv->vec[i/8] = bv->vec[i/8] & ~(1 << (i%8));
}

// returns index of first cleared bit (or -1 if no such bit exists)
int bitvec_first_cleared_index(bitvec_t *bv)
{
  int i, j, start;
  for (i = 0; i < bv->nbytes; i++) {
    if (~bv->vec[i]) {
      // at least one empty bit in this byte
      start = i*8;
      for (j = start; j < start+8; j++) {
        if (!bitvec_get(bv, j)) {
          return j;
        }
      }
    }
  }
  return -1;
}
