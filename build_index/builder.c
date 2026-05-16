#define _GNU_SOURCE
#include "../src/templar.h"
#include "../src/vectorize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ── MCC stub ───────────────────────────────────────────────────────── */
float mcc_risk_lookup(const char *mcc) { (void)mcc; return 0.5f; }

/* ── Reference loader — single linear pass ──────────────────────────── */
#define MAX_REFS 2000000
#define GZ_CHUNK 131072

typedef struct { float vec[VEC_DIM]; uint8_t is_fraud; } RawRef;
static RawRef *g_refs   = NULL;
static int     g_n_refs = 0;

static inline void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static inline float parse_float(const char **p) {
    char *end;
    float v = (float)strtod(*p, &end);
    *p = end;
    return v;
}

static int load_references(const char *gz_path) {
    gzFile gz = gzopen(gz_path, "rb");
    if (!gz) { fprintf(stderr, "Cannot open %s\n", gz_path); return -1; }

    size_t cap = 320 * 1024 * 1024UL;
    char  *buf = malloc(cap);
    if (!buf) { fprintf(stderr, "OOM buf\n"); return -1; }
    size_t used = 0;
    int n;
    while ((n = gzread(gz, buf + used, GZ_CHUNK)) > 0) {
        used += (size_t)n;
        if (used + GZ_CHUNK > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) { fprintf(stderr, "OOM realloc\n"); return -1; }
        }
    }
    gzclose(gz);
    buf[used] = '\0';
    fprintf(stderr, "Decompressed %zu bytes\n", used);

    g_refs = malloc(MAX_REFS * sizeof(RawRef));
    if (!g_refs) { fprintf(stderr, "OOM refs\n"); return -1; }
    g_n_refs = 0;

    const char *p = buf;

    /* skip opening '[' */
    while (*p && *p != '[') p++;
    if (*p == '[') p++;

    while (*p && g_n_refs < MAX_REFS) {
        /* skip to '{' */
        while (*p && *p != '{' && *p != ']') p++;
        if (*p == ']' || !*p) break;
        p++; /* skip '{' */

        RawRef *r = &g_refs[g_n_refs];
        r->is_fraud = 0;
        int vec_loaded = 0;

        /* parse key:value pairs until '}' */
        while (*p && *p != '}') {
            skip_ws(&p);
            if (*p != '"') { p++; continue; }
            p++; /* skip opening " */

            /* read key */
            char key[16]; int ki = 0;
            while (*p && *p != '"' && ki < 15) key[ki++] = *p++;
            key[ki] = '\0';
            if (*p == '"') p++;

            /* skip ':' */
            skip_ws(&p);
            if (*p == ':') p++;
            skip_ws(&p);

            if (strcmp(key, "vector") == 0) {
                /* parse float array */
                if (*p == '[') p++;
                for (int d = 0; d < VEC_DIM; d++) {
                    skip_ws(&p);
                    if (*p == ',') p++;
                    skip_ws(&p);
                    r->vec[d] = parse_float(&p);
                }
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
                vec_loaded = 1;
            } else if (strcmp(key, "label") == 0) {
                /* parse string value */
                if (*p == '"') p++;
                r->is_fraud = (*p == 'f') ? 1 : 0; /* "fraud" or "legit" */
                while (*p && *p != '"') p++;
                if (*p == '"') p++;
            } else {
                /* skip unknown value */
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"') p++;
                    if (*p == '"') p++;
                } else {
                    while (*p && *p != ',' && *p != '}') p++;
                }
            }

            skip_ws(&p);
            if (*p == ',') p++;
        }
        if (*p == '}') p++;

        if (vec_loaded) g_n_refs++;

        /* skip comma between objects */
        skip_ws(&p);
        if (*p == ',') p++;
    }
    free(buf);
    fprintf(stderr, "Parsed %d references\n", g_n_refs);
    return 0;
}

/* ── K-means ────────────────────────────────────────────────────────── */
static float centroids[IVF_CLUSTERS][VEC_DIM];
static int   assignments[MAX_REFS];

static float dist2_ff(const float *a, const float *b) {
    float s = 0.0f;
    for (int i = 0; i < VEC_DIM; i++) {
        if (i == 5 || i == 6) {
            if (a[i] < -0.5f && b[i] < -0.5f) continue;
            if (a[i] < -0.5f || b[i] < -0.5f) { s += 4.0f; continue; }
        }
        float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

static void kmeans(int n_refs, int k, int max_iter) {
    srand(42);

    int sample_size = n_refs < 200000 ? n_refs : 200000;
    int *sample_idx = malloc(sample_size * sizeof(int));
    for (int i = 0; i < sample_size; i++) sample_idx[i] = i;
    for (int i = sample_size; i < n_refs; i++) {
        int j = rand() % (i + 1);
        if (j < sample_size) sample_idx[j] = i;
    }
    fprintf(stderr, "K-means: %d amostras, k=%d, max_iter=%d\n", sample_size, k, max_iter);

    for (int c = 0; c < k; c++) {
        int idx = sample_idx[rand() % sample_size];
        memcpy(centroids[c], g_refs[idx].vec, VEC_DIM * sizeof(float));
    }

    int *sample_assign = calloc(sample_size, sizeof(int));
    for (int iter = 0; iter < max_iter; iter++) {
        int changed = 0;
        for (int i = 0; i < sample_size; i++) {
            float best = 1e30f; int bi = 0;
            const float *v = g_refs[sample_idx[i]].vec;
            for (int c = 0; c < k; c++) {
                float d = dist2_ff(v, centroids[c]);
                if (d < best) { best = d; bi = c; }
            }
            if (sample_assign[i] != bi) changed++;
            sample_assign[i] = bi;
        }
        fprintf(stderr, "  iter %2d: %d mudancas\n", iter, changed);
        if (changed == 0) break;

        double new_c[IVF_CLUSTERS][VEC_DIM] = {{0}};
        int    counts[IVF_CLUSTERS]          = {0};
        for (int i = 0; i < sample_size; i++) {
            int c = sample_assign[i];
            const float *v = g_refs[sample_idx[i]].vec;
            for (int d = 0; d < VEC_DIM; d++) new_c[c][d] += v[d];
            counts[c]++;
        }
        for (int c = 0; c < k; c++) {
            if (counts[c] == 0) continue;
            for (int d = 0; d < VEC_DIM; d++)
                centroids[c][d] = (float)(new_c[c][d] / counts[c]);
        }
    }
    free(sample_assign);
    free(sample_idx);

    fprintf(stderr, "Atribuindo %d refs...\n", n_refs);
    for (int i = 0; i < n_refs; i++) {
        float best = 1e30f; int bi = 0;
        for (int c = 0; c < k; c++) {
            float d = dist2_ff(g_refs[i].vec, centroids[c]);
            if (d < best) { best = d; bi = c; }
        }
        assignments[i] = bi;
        if (i % 500000 == 0)
            fprintf(stderr, "  %d/%d\n", i, n_refs);
    }
    fprintf(stderr, "Feito.\n");
}

/* ── Write index.bin ────────────────────────────────────────────────── */
static int write_index(const char *path, int k, int n_refs) {
    int counts[IVF_CLUSTERS] = {0};
    for (int i = 0; i < n_refs; i++) counts[assignments[i]]++;

    RefEntry **ce  = malloc(k * sizeof(RefEntry *));
    int       *pos = calloc(k, sizeof(int));
    for (int c = 0; c < k; c++)
        ce[c] = malloc(counts[c] * sizeof(RefEntry));

    for (int i = 0; i < n_refs; i++) {
        int c = assignments[i];
        RefEntry *e = &ce[c][pos[c]++];
        float *v = g_refs[i].vec;
        for (int d = 0; d < VEC_DIM; d++) {
            float x = v[d] * (float)QUANT_SCALE;
            if (x < -32768.0f) x = -32768.0f;
            if (x >  32767.0f) x =  32767.0f;
            e->vec[d] = (int16_t)(int)x;
        }
        e->is_fraud = g_refs[i].is_fraud;
        e->_pad[0]  = 0;
    }

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }

    IndexHeader hdr = { INDEX_MAGIC, INDEX_VERSION, k, n_refs };
    fwrite(&hdr, sizeof(hdr), 1, f);

    IVFCluster cl[IVF_CLUSTERS];
    int offset = 0;
    for (int c = 0; c < k; c++) {
        memcpy(cl[c].centroid, centroids[c], VEC_DIM * sizeof(float));
        cl[c].offset = offset;
        cl[c].count  = counts[c];
        offset += counts[c];
    }
    fwrite(cl, sizeof(IVFCluster), k, f);

    for (int c = 0; c < k; c++) {
        fwrite(ce[c], sizeof(RefEntry), counts[c], f);
        free(ce[c]);
    }
    free(ce); free(pos);
    fclose(f);
    fprintf(stderr, "Index gravado: %s\n", path);
    return 0;
}

/* ── main ───────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: build_index <references.json.gz> <mcc_risk.json> <index.bin>\n");
        return 1;
    }
    if (load_references(argv[1]) < 0) return 1;
    fprintf(stderr, "Rodando K-means (k=%d)...\n", IVF_CLUSTERS);
    memset(assignments, 0, sizeof(int) * g_n_refs);
    kmeans(g_n_refs, IVF_CLUSTERS, 50);
    if (write_index(argv[3], IVF_CLUSTERS, g_n_refs) < 0) return 1;
    free(g_refs);
    return 0;
}