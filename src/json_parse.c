#define _GNU_SOURCE
/*
 * json_parse.c — Minimal hand-rolled JSON parser for the fraud-score payload.
 *
 * Strategy: single-pass, zero-copy on string fields (pointer + length),
 * no memory allocation.  Only parses exactly what we need.
 */
#include "json_parse.h"
#include "templar.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/* ── helpers ─────────────────────────────────────────────────────────── */
static inline void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static inline bool match(const char **p, char c) {
    skip_ws(p);
    if (**p == c) { (*p)++; return true; }
    return false;
}

/* Read a JSON string into dst (max dst_len-1 chars).  Advances *p past closing " */
static int read_string(const char **p, char *dst, int dst_len) {
    skip_ws(p);
    if (**p != '"') return -1;
    (*p)++;
    int i = 0;
    while (**p && **p != '"') {
        if (**p == '\\') { (*p)++; }  /* skip escape, take next char as-is */
        if (i < dst_len - 1) dst[i++] = **p;
        (*p)++;
    }
    if (**p == '"') (*p)++;
    dst[i] = '\0';
    return i;
}

/* Read a JSON number (integer or float) into *val */
static int read_double(const char **p, double *val) {
    skip_ws(p);
    char *end;
    *val = strtod(*p, &end);
    if (end == *p) return -1;
    *p = end;
    return 0;
}

static int read_bool(const char **p, bool *val) {
    skip_ws(p);
    if (strncmp(*p, "true", 4) == 0)  { *val = true;  *p += 4; return 0; }
    if (strncmp(*p, "false", 5) == 0) { *val = false; *p += 5; return 0; }
    return -1;
}

/* Parse ISO 8601 UTC timestamp "2026-03-11T18:45:53Z" → epoch */
static long parse_iso8601(const char *s) {
    struct tm tm = {0};
    /* sscanf is good enough for a fixed format */
    if (sscanf(s, "%d-%d-%dT%d:%d:%dZ",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    tm.tm_isdst = 0;
#ifdef _WIN32
    return (long)_mkgmtime(&tm);
#else
    return (long)timegm(&tm);
#endif
}

/* Skip any JSON value (for unknown keys) */
static void skip_value(const char **p);

static void skip_object(const char **p) {
    skip_ws(p);
    if (**p != '{') return;
    (*p)++;
    int depth = 1;
    while (**p && depth > 0) {
        if (**p == '{') depth++;
        else if (**p == '}') depth--;
        else if (**p == '"') {
            (*p)++;
            while (**p && **p != '"') { if (**p == '\\') (*p)++; (*p)++; }
            if (**p == '"') (*p)++;
            continue;
        }
        if (depth > 0) (*p)++;
        else (*p)++;
    }
}

static void skip_array(const char **p) {
    skip_ws(p);
    if (**p != '[') return;
    (*p)++;
    int depth = 1;
    while (**p && depth > 0) {
        if (**p == '[') depth++;
        else if (**p == ']') depth--;
        else if (**p == '"') {
            (*p)++;
            while (**p && **p != '"') { if (**p == '\\') (*p)++; (*p)++; }
            if (**p == '"') (*p)++;
            continue;
        }
        if (depth > 0) (*p)++;
        else (*p)++;
    }
}

static void skip_value(const char **p) {
    skip_ws(p);
    switch (**p) {
        case '{': skip_object(p); break;
        case '[': skip_array(p); break;
        case '"': { char tmp[256]; read_string(p, tmp, sizeof(tmp)); break; }
        case 'n': *p += 4; break; /* null */
        case 't': *p += 4; break; /* true */
        case 'f': *p += 5; break; /* false */
        default: {
            while (**p && **p != ',' && **p != '}' && **p != ']') (*p)++;
            break;
        }
    }
}

/* ── Object field iterator ───────────────────────────────────────────── */
/* Calls callback for each "key": value pair inside a {} block.
   Caller advances *p to the opening '{' before calling. */

#define MAX_KEY 64

typedef void (*field_cb)(const char *key, const char **val_p, void *ctx);

static void iter_object(const char **p, field_cb cb, void *ctx) {
    skip_ws(p);
    if (**p != '{') { skip_value(p); return; }
    (*p)++; /* consume '{' */
    skip_ws(p);
    if (**p == '}') { (*p)++; return; }
    while (**p) {
        char key[MAX_KEY];
        if (read_string(p, key, sizeof(key)) < 0) break;
        if (!match(p, ':')) break;
        skip_ws(p);
        cb(key, p, ctx);
        skip_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == '}') { (*p)++; break; }
        break;
    }
}

/* ── Section parsers ─────────────────────────────────────────────────── */

static void parse_transaction_field(const char *key, const char **p, void *ctx) {
    TxPayload *tx = (TxPayload *)ctx;
    double dval;
    char sval[64];
    if (strcmp(key, "amount") == 0) {
        read_double(p, &dval); tx->amount = dval;
    } else if (strcmp(key, "installments") == 0) {
        read_double(p, &dval); tx->installments = (int)dval;
    } else if (strcmp(key, "requested_at") == 0) {
        read_string(p, sval, sizeof(sval));
        tx->requested_at_epoch = parse_iso8601(sval);
    } else {
        skip_value(p);
    }
}

static void parse_customer_field(const char *key, const char **p, void *ctx) {
    TxPayload *tx = (TxPayload *)ctx;
    double dval;
    if (strcmp(key, "avg_amount") == 0) {
        read_double(p, &dval); tx->customer_avg_amount = dval;
    } else if (strcmp(key, "tx_count_24h") == 0) {
        read_double(p, &dval); tx->customer_tx_count_24h = (int)dval;
    } else if (strcmp(key, "known_merchants") == 0) {
        /* parse string array */
        skip_ws(p);
        if (**p != '[') { skip_value(p); return; }
        (*p)++;
        tx->known_merchant_count = 0;
        while (**p && **p != ']') {
            skip_ws(p);
            if (**p == '"') {
                if (tx->known_merchant_count < 32)
                    read_string(p, tx->known_merchants[tx->known_merchant_count++],
                                sizeof(tx->known_merchants[0]));
                else { char tmp[24]; read_string(p, tmp, sizeof(tmp)); }
            } else { skip_value(p); }
            skip_ws(p);
            if (**p == ',') (*p)++;
        }
        if (**p == ']') (*p)++;
    } else {
        skip_value(p);
    }
}

static void parse_merchant_field(const char *key, const char **p, void *ctx) {
    TxPayload *tx = (TxPayload *)ctx;
    double dval;
    if (strcmp(key, "id") == 0) {
        read_string(p, tx->merchant_id, sizeof(tx->merchant_id));
    } else if (strcmp(key, "mcc") == 0) {
        read_string(p, tx->merchant_mcc, sizeof(tx->merchant_mcc));
    } else if (strcmp(key, "avg_amount") == 0) {
        read_double(p, &dval); tx->merchant_avg_amount = dval;
    } else {
        skip_value(p);
    }
}

static void parse_terminal_field(const char *key, const char **p, void *ctx) {
    TxPayload *tx = (TxPayload *)ctx;
    double dval;
    bool bval = false;
    if (strcmp(key, "is_online") == 0) {
        read_bool(p, &bval); tx->terminal_is_online = bval;
    } else if (strcmp(key, "card_present") == 0) {
        read_bool(p, &bval); tx->terminal_card_present = bval;
    } else if (strcmp(key, "km_from_home") == 0) {
        read_double(p, &dval); tx->terminal_km_from_home = dval;
    } else {
        skip_value(p);
    }
}

static void parse_last_tx_field(const char *key, const char **p, void *ctx) {
    TxPayload *tx = (TxPayload *)ctx;
    double dval;
    char sval[64];
    if (strcmp(key, "timestamp") == 0) {
        read_string(p, sval, sizeof(sval));
        tx->last_tx_epoch = parse_iso8601(sval);
    } else if (strcmp(key, "km_from_current") == 0) {
        read_double(p, &dval); tx->last_tx_km_from_current = dval;
    } else {
        skip_value(p);
    }
}

static void parse_root_field(const char *key, const char **p, void *ctx) {
    TxPayload *tx = (TxPayload *)ctx;
    if (strcmp(key, "transaction") == 0) {
        iter_object(p, parse_transaction_field, tx);
    } else if (strcmp(key, "customer") == 0) {
        iter_object(p, parse_customer_field, tx);
    } else if (strcmp(key, "merchant") == 0) {
        iter_object(p, parse_merchant_field, tx);
    } else if (strcmp(key, "terminal") == 0) {
        iter_object(p, parse_terminal_field, tx);
    } else if (strcmp(key, "last_transaction") == 0) {
        skip_ws(p);
        if (strncmp(*p, "null", 4) == 0) {
            *p += 4;
            tx->has_last_tx = false;
        } else {
            tx->has_last_tx = true;
            iter_object(p, parse_last_tx_field, tx);
        }
    } else {
        skip_value(p);
    }
}

/* ── Public entry point ──────────────────────────────────────────────── */
int json_parse_tx(const char *body, int body_len, TxPayload *out) {
    (void)body_len;
    memset(out, 0, sizeof(*out));
    const char *p = body;
    iter_object(&p, parse_root_field, out);
    return 0;
}