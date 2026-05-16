/*
 * vectorize.c — Converts a TxPayload into the 14-dimensional float vector
 *               and then quantizes it to int16 for the index.
 *
 * Spec: docs/br/REGRAS_DE_DETECCAO.md
 */
#include "vectorize.h"
#include "templar.h"
#include <math.h>
#include <string.h>
#include <time.h>

/* mcc_risk table is loaded once at startup (see index.c) */
extern float mcc_risk_lookup(const char *mcc);

static inline float clampf(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

/*
 * Decompose a UTC epoch into (hour_of_day, day_of_week).
 * day_of_week: Mon=0 … Sun=6
 */
static void epoch_to_hourdow(long epoch, int *hour, int *dow) {
    /* 1970-01-01 was a Thursday = 3 (Thu); Mon=0 baseline */
    /* days since epoch */
    long days = epoch / 86400;
    long secs  = epoch % 86400;
    *hour = (int)(secs / 3600);
    /* Thursday = 3, so (days + 3) % 7 gives Thu=3 Mon=0 */
    int raw_dow = (int)((days + 4) % 7); /* Sun=0 in ISO */
    /* convert Sun=0,Mon=1..Sat=6 → Mon=0..Sun=6 */
    *dow = (raw_dow + 6) % 7;
}

void vectorize(const TxPayload *tx, float vec[VEC_DIM]) {
    /* 0: amount */
    vec[0] = clampf((float)(tx->amount / NORM_MAX_AMOUNT));

    /* 1: installments */
    vec[1] = clampf((float)(tx->installments / NORM_MAX_INSTALLMENTS));

    /* 2: amount_vs_avg */
    float avg = (float)tx->customer_avg_amount;
    if (avg > 0.0f)
        vec[2] = clampf((float)(tx->amount / avg) / NORM_AMOUNT_VS_AVG_RATIO);
    else
        vec[2] = 1.0f; /* guard against division by zero */

    /* 3: hour_of_day  (0-23) / 23 */
    int hour, dow;
    epoch_to_hourdow(tx->requested_at_epoch, &hour, &dow);
    vec[3] = (float)hour / 23.0f;

    /* 4: day_of_week  (Mon=0..Sun=6) / 6 */
    vec[4] = (float)dow / 6.0f;

    /* 5: minutes_since_last_tx  — sentinel -1 if no last_tx */
    if (!tx->has_last_tx) {
        vec[5] = SENTINEL_NULL;
    } else {
        long diff_sec = tx->requested_at_epoch - tx->last_tx_epoch;
        if (diff_sec < 0) diff_sec = 0;
        float minutes = (float)(diff_sec / 60);
        vec[5] = clampf(minutes / NORM_MAX_MINUTES);
    }

    /* 6: km_from_last_tx — sentinel -1 if no last_tx */
    if (!tx->has_last_tx) {
        vec[6] = SENTINEL_NULL;
    } else {
        vec[6] = clampf((float)(tx->last_tx_km_from_current / NORM_MAX_KM));
    }

    /* 7: km_from_home */
    vec[7] = clampf((float)(tx->terminal_km_from_home / NORM_MAX_KM));

    /* 8: tx_count_24h */
    vec[8] = clampf((float)(tx->customer_tx_count_24h / NORM_MAX_TX_COUNT_24H));

    /* 9: is_online */
    vec[9] = tx->terminal_is_online ? 1.0f : 0.0f;

    /* 10: card_present */
    vec[10] = tx->terminal_card_present ? 1.0f : 0.0f;

    /* 11: unknown_merchant (1 = unknown, 0 = known) */
    bool known = false;
    for (int i = 0; i < tx->known_merchant_count; i++) {
        if (strcmp(tx->known_merchants[i], tx->merchant_id) == 0) {
            known = true; break;
        }
    }
    vec[11] = known ? 0.0f : 1.0f;

    /* 12: mcc_risk */
    vec[12] = mcc_risk_lookup(tx->merchant_mcc);

    /* 13: merchant_avg_amount */
    vec[13] = clampf((float)(tx->merchant_avg_amount / NORM_MAX_MERCHANT_AVG));
}

/* Quantize float vec → int16, scale 10000.
   Sentinel -1.0 stays -10000 (well outside [0,10000]). */
void quantize(const float vec[VEC_DIM], qvec_t out) {
    for (int i = 0; i < VEC_DIM; i++) {
        float v = vec[i] * (float)QUANT_SCALE;
        if (v < -32768.0f) v = -32768.0f;
        if (v >  32767.0f) v =  32767.0f;
        out[i] = (int16_t)(int)v;
    }
}

/* Dequantize back to float (for centroid distance) */
void dequantize(const qvec_t q, float out[VEC_DIM]) {
    for (int i = 0; i < VEC_DIM; i++)
        out[i] = (float)q[i] / (float)QUANT_SCALE;
}