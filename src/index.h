#ifndef INDEX_H
#define INDEX_H

#include "templar.h"

int index_load(const char *index_path, const char *mcc_path);
FraudResult index_query(const float query[VEC_DIM]);

#endif