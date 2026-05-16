#ifndef TEMPLAR_H
#define TEMPLAR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Normalization constants (from normalization.json) ─────────────────── */
#define NORM_MAX_AMOUNT            10000.0f
#define NORM_MAX_INSTALLMENTS      12.0f
#define NORM_AMOUNT_VS_AVG_RATIO   10.0f
#define NORM_MAX_MINUTES           1440.0f
#define NORM_MAX_KM                1000.0f
#define NORM_MAX_TX_COUNT_24H      20.0f
#define NORM_MAX_MERCHANT_AVG      10000.0f

#define VEC_DIM          14
#define KNN_K            5
#define FRAUD_THRESHOLD  0.6f
#define SENTINEL_NULL   -1.0f   /* dims 5,6 when last_transaction is null */

/* ── IVF index config ──────────────────────────────────────────────────── */
#define IVF_CLUSTERS     64     /* number of K-means clusters              */
#define IVF_PROBE        8      /* how many clusters to probe at query time */
#define INDEX_MAGIC      0x54454D50U  /* "TEMP" */
#define INDEX_VERSION    1

/* ── Vector (quantized int16) ──────────────────────────────────────────── */
#define QUANT_SCALE      10000
typedef int16_t qvec_t[VEC_DIM];

/* ── Reference entry in the index ─────────────────────────────────────── */
typedef struct {
    qvec_t  vec;      /* quantized 14-dim vector */
    uint8_t is_fraud; /* 1 = fraud, 0 = legit    */
    uint8_t _pad[1];
} RefEntry;

/* ── IVF cluster centroid ──────────────────────────────────────────────── */
typedef struct {
    float   centroid[VEC_DIM]; /* float centroid for distance comp */
    int32_t offset;            /* byte offset into entries array   */
    int32_t count;             /* entries in this cluster          */
} IVFCluster;

/* ── Full index header (written to index.bin) ──────────────────────────── */
typedef struct {
    uint32_t   magic;
    uint32_t   version;
    int32_t    n_clusters;
    int32_t    n_entries;
    /* followed by: IVFCluster[n_clusters], then RefEntry[n_entries]       */
} IndexHeader;

/* ── Transaction payload (parsed from JSON) ────────────────────────────── */
typedef struct {
    double  amount;
    int     installments;
    long    requested_at_epoch; /* seconds since epoch, UTC */

    double  customer_avg_amount;
    int     customer_tx_count_24h;
    char    known_merchants[32][24]; /* up to 32 known merchants */
    int     known_merchant_count;

    char    merchant_id[24];
    char    merchant_mcc[8];
    double  merchant_avg_amount;

    bool    terminal_is_online;
    bool    terminal_card_present;
    double  terminal_km_from_home;

    bool    has_last_tx;
    long    last_tx_epoch;
    double  last_tx_km_from_current;
} TxPayload;

/* ── Fraud result ──────────────────────────────────────────────────────── */
typedef struct {
    bool  approved;
    float fraud_score;
} FraudResult;

#endif /* TEMPLAR_H */