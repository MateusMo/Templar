/*
 * builder.c — Offline IVF index builder.
 *
 * Reads references.json.gz, vectorizes all entries, runs K-means to build
 * IVF_CLUSTERS centroids, assigns each entry to its nearest centroid,
 * then writes index.bin.
 *
 * index.bin layout:
 *   IndexHeader
 *   IVFCluster[n_clusters]   (centroids + offset/count)
 *   RefEntry[n_entries]      (sorted by cluster)
 *
 * Compile: gcc -O3 -o build_index builder.c ../src/vectorize.c -lm -lz
 */
#define _GNU_SOURCE
#include "../src/templar.h"
#include "../src/vectorize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <zlib.h>

/* Stub for mcc_risk_lookup (vectorize.c calls this).
   We load the table here manually. */
#define MCC_BUCKETS 4096
typedef struct MccEnt { char mcc[8]; float risk; struct MccEnt *next; } MccEnt;
static MccEnt mcc_pool_b[2048];
static int    mcc_pool_b_used = 0;
static MccEnt *mcc_tbl[MCC_BUCKETS];

static uint32_t mcc_hash_b(const char *s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) ^ (uint8_t)*s++;
    return h & (MCC_BUCKETS - 1);
}

float mcc_risk_lookup(const char *mcc) {
    uint32_t h = mcc_hash_b(mcc);
    for (MccEnt *e = mcc_tbl[h]; e; e = e->next)
        if (strcmp(e->mcc, mcc) == 0) return e->risk;
    return 0.5f;
}

static void load_mcc_b(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "warn: cannot open %s\n", path); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1); fread(buf, 1, sz, f); buf[sz]=0; fclose(f);
    const char *p = buf;
    while (*p && *p != '{') p++; if (*p=='{') p++;
    while (*p) {
        while (*p && *p!='"' && *p!='}') p++;
        if (*p=='}' || !*p) break;
        p++;
        char mcc[8]; int mi=0;
        while (*p && *p!='"' && mi<7) mcc[mi++]=*p++;
        mcc[mi]=0; if (*p=='"') p++;
        while (*p && *p!=':') p++; if (*p==':') p++;
        while (*p==' ') p++;
        char *end; float risk=(float)strtod(p,&end); p=end;
        uint32_t h=mcc_hash_b(mcc);
        if (mcc_pool_b_used < 2048) {
            MccEnt *e=&mcc_pool_b[mcc_pool_b_used++];
            strncpy(e->mcc,mcc,7); e->mcc[7]=0; e->risk=risk;
            e->next=mcc_tbl[h]; mcc_tbl[h]=e;
        }
        while (*p && *p!=',' && *p!='}') p++;
        if (*p==',') p++;
    }
    free(buf);
}

/* ── Minimal JSON reader for references.json.gz ─────────────────────── */
/*
 * Each reference looks like:
 * {"vector":[0.1,0.2,...,0.14],"is_fraud":false}
 * or the flat fields format used by the spec.
 *
 * The official references.json.gz uses the full transaction format plus
 * a "is_fraud" field.  We parse the "vector" array directly (14 floats)
 * and "is_fraud".
 */

#define MAX_REFS 2000000  /* 2M references */

typedef struct {
    float    vec[VEC_DIM];
    uint8_t  is_fraud;
} RawRef;

static RawRef *g_refs     = NULL;
static int     g_n_refs   = 0;

/* gzip streaming reader */
#define GZ_CHUNK 131072

static int load_references(const char *gz_path) {
    gzFile gz = gzopen(gz_path, "rb");
    if (!gz) { fprintf(stderr, "Cannot open %s\n", gz_path); return -1; }

    /* Read entire decompressed content */
    size_t cap = 256 * 1024 * 1024UL; /* 256 MB initial */
    char  *buf = malloc(cap);
    size_t used = 0;
    int    n;
    while ((n = gzread(gz, buf + used, GZ_CHUNK)) > 0) {
        used += (size_t)n;
        if (used + GZ_CHUNK > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) { fprintf(stderr, "OOM\n"); return -1; }
        }
    }
    gzclose(gz);
    buf[used] = '\0';
    fprintf(stderr, "Decompressed %zu bytes\n", used);

    /* Allocate ref array */
    g_refs = malloc(MAX_REFS * sizeof(RawRef));
    if (!g_refs) { fprintf(stderr, "OOM refs\n"); return -1; }
    g_n_refs = 0;

    /* Parse JSON array of objects.
       Each object has either:
         a) "vector": [f,f,...,f]  and "is_fraud": bool
         b) Full transaction fields + "is_fraud": bool  (we vectorize on the fly)
       Strategy: try to find "vector" key first; if not present, skip for now.
       The official dataset has "vector" pre-computed. */

    const char *p = buf;
    /* skip to first '[' or '{' */
    while (*p && *p != '[' && *p != '{') p++;
    if (*p == '[') p++; /* skip array start */

    while (*p && g_n_refs < MAX_REFS) {
        /* skip to next '{' */
        while (*p && *p != '{' && *p != ']') p++;
        if (*p == ']' || !*p) break;
        /* find matching '}' */
        const char *obj_start = p;
        int depth = 0;
        do {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        } while (*p && depth > 0);
        /* obj_start..p is the object */
        const char *obj = obj_start;
        const char *obj_end = p;

        /* find "vector": [ ... ] */
        const char *vp = strstr(obj, "\"vector\"");
        bool has_vec = (vp && vp < obj_end);

        /* find "is_fraud": bool */
        const char *fp = strstr(obj, "\"is_fraud\"");
        if (!fp || fp >= obj_end) { /* skip */ continue; }
        fp += strlen("\"is_fraud\"");
        while (*fp == ':' || *fp == ' ') fp++;
        uint8_t is_fraud = (strncmp(fp, "true", 4) == 0) ? 1 : 0;

        RawRef *r = &g_refs[g_n_refs];
        r->is_fraud = is_fraud;

        if (has_vec) {
            vp += strlen("\"vector\"");
            while (*vp && *vp != '[') vp++;
            if (*vp == '[') vp++;
            for (int d = 0; d < VEC_DIM && vp < obj_end; d++) {
                while (*vp == ' ' || *vp == ',') vp++;
                char *end;
                r->vec[d] = (float)strtod(vp, &end);
                vp = end;
            }
        } else {
            /* If no pre-computed vector, skip this entry */
            continue;
        }
        g_n_refs++;
    }
    free(buf);
    fprintf(stderr, "Parsed %d references\n", g_n_refs);
    return 0;
}

/* ── K-means ─────────────────────────────────────────────────────────── */
static float centroids[IVF_CLUSTERS][VEC_DIM];
static int   assignments[2000000];

static float dist2_ff(const float *a, const float *b) {
    float s = 0.0f;
    for (int i = 0; i < VEC_DIM; i++) {
        float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

static void kmeans(int n_refs, int k, int max_iter) {
    srand(42);
    /* Initialize centroids: kmeans++ style (simplified: random sample) */
    for (int c = 0; c < k; c++) {
        int idx = rand() % n_refs;
        memcpy(centroids[c], g_refs[idx].vec, VEC_DIM * sizeof(float));
    }

    for (int iter = 0; iter < max_iter; iter++) {
        /* Assignment step */
        int changed = 0;
        for (int i = 0; i < n_refs; i++) {
            float best = 1e30f; int bi = 0;
            for (int c = 0; c < k; c++) {
                float d = dist2_ff(g_refs[i].vec, centroids[c]);
                if (d < best) { best = d; bi = c; }
            }
            if (assignments[i] != bi) changed++;
            assignments[i] = bi;
        }
        if (changed == 0) { fprintf(stderr, "kmeans converged at iter %d\n", iter); break; }

        /* Update step */
        double new_centroids[IVF_CLUSTERS][VEC_DIM] = {{0}};
        int    counts[IVF_CLUSTERS]                 = {0};
        for (int i = 0; i < n_refs; i++) {
            int c = assignments[i];
            for (int d = 0; d < VEC_DIM; d++)
                new_centroids[c][d] += g_refs[i].vec[d];
            counts[c]++;
        }
        for (int c = 0; c < k; c++) {
            if (counts[c] == 0) continue;
            for (int d = 0; d < VEC_DIM; d++)
                centroids[c][d] = (float)(new_centroids[c][d] / counts[c]);
        }
        fprintf(stderr, "kmeans iter %d: %d changed\n", iter, changed);
    }
}

/* ── Write index.bin ─────────────────────────────────────────────────── */
static int write_index(const char *path, int k, int n_refs) {
    /* Count entries per cluster */
    int counts[IVF_CLUSTERS] = {0};
    for (int i = 0; i < n_refs; i++) counts[assignments[i]]++;

    /* Build per-cluster sorted arrays */
    RefEntry **cluster_entries = malloc(k * sizeof(RefEntry *));
    int       *cluster_pos     = calloc(k, sizeof(int));
    for (int c = 0; c < k; c++)
        cluster_entries[c] = malloc(counts[c] * sizeof(RefEntry));

    for (int i = 0; i < n_refs; i++) {
        int c = assignments[i];
        RefEntry *e = &cluster_entries[c][cluster_pos[c]++];
        /* Quantize */
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

    IndexHeader hdr = {
        .magic      = INDEX_MAGIC,
        .version    = INDEX_VERSION,
        .n_clusters = k,
        .n_entries  = n_refs
    };
    fwrite(&hdr, sizeof(hdr), 1, f);

    /* Build cluster headers with offsets */
    IVFCluster cl_hdrs[IVF_CLUSTERS];
    int offset = 0;
    for (int c = 0; c < k; c++) {
        memcpy(cl_hdrs[c].centroid, centroids[c], VEC_DIM * sizeof(float));
        cl_hdrs[c].offset = offset;
        cl_hdrs[c].count  = counts[c];
        offset += counts[c];
    }
    fwrite(cl_hdrs, sizeof(IVFCluster), k, f);

    /* Write entries cluster by cluster */
    for (int c = 0; c < k; c++) {
        fwrite(cluster_entries[c], sizeof(RefEntry), counts[c], f);
        free(cluster_entries[c]);
    }
    free(cluster_entries);
    free(cluster_pos);
    fclose(f);
    fprintf(stderr, "Wrote index to %s\n", path);
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "Usage: build_index <references.json.gz> <mcc_risk.json> <index.bin>\n");
        return 1;
    }
    const char *refs_path  = argv[1];
    const char *mcc_path   = argv[2];
    const char *index_path = argv[3];

    load_mcc_b(mcc_path);

    if (load_references(refs_path) < 0) return 1;

    fprintf(stderr, "Running K-means (k=%d)...\n", IVF_CLUSTERS);
    memset(assignments, 0, sizeof(int) * g_n_refs);
    kmeans(g_n_refs, IVF_CLUSTERS, 30);

    if (write_index(index_path, IVF_CLUSTERS, g_n_refs) < 0) return 1;
    free(g_refs);
    return 0;
}