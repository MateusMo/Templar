#ifndef VECTORIZE_H
#define VECTORIZE_H

#include "templar.h"

void vectorize(const TxPayload *tx, float vec[VEC_DIM]);
void quantize(const float vec[VEC_DIM], qvec_t out);
void dequantize(const qvec_t q, float out[VEC_DIM]);

#endif