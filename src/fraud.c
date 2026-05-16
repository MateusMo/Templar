/*
* fraud.c — Ties together JSON parsing, vectorization and index query.
 */
#include "fraud.h"
#include "json_parse.h"
#include "vectorize.h"
#include "index.h"
#include "templar.h"
#include <string.h>

FraudResult fraud_score(const char *body, int body_len) {
    TxPayload tx;
    FraudResult bad = {false, 1.0f};

    if (json_parse_tx(body, body_len, &tx) < 0)
        return bad;

    float vec[VEC_DIM];
    vectorize(&tx, vec);

    return index_query(vec);
}