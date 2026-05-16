#define _GNU_SOURCE
/*
 * index.c — IVF (Inverted File Index) for KNN search.
 *
 * At startup: mmap() the pre-built index.bin produced by build_index/builder.c
 * and the mcc_risk.json table.
 *
 * Query path (hot):
 *   1. Compute L2 distance from query float vector to each centroid.
 *   2. Select top-IVF_PROBE clusters.
 *   3. Linear scan within those clusters, keep top-K with a max-heap.
 *   4. Return KNN_K nearest neighbors with fraud labels.
 */
#include "index.h"
#include "templar.h"
#include "vectorize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── MCC risk table ─────────────────────────────────────────────────── */
#define MCC_BUCKETS 4096
typedef struct MccEntry { char mcc[8]; float risk; struct MccEntry *next; } MccEntry;
static MccEntry mcc_pool[2048];
static int      mcc_pool_used = 0;
static MccEntry *mcc_table[MCC_BUCKETS];

static uint32_t mcc_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) ^ (uint8_t)*s++;
    return h & (MCC_BUCKETS - 1);
}

/* Called from vectorize.c */
float mcc_risk_lookup(const char *mcc) {
    uint32_t h = mcc_hash(mcc);
    for (MccEntry *e = mcc_table[h]; e; e = e->next)
        if (strcmp(e->mcc, mcc) == 0) return e->risk;
    return 0.5f; /* default */
}

/* Parse mcc_risk.json: {"5411": 0.1, "7802": 0.75, ...} */
static int load_mcc_risk(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        fprintf(stderr, "warn: short read %s\n", path);
    }
    buf[sz] = '\0'; fclose(f);

    const char *p = buf;
    while (*p && *p != '{') p++;
    if (*p == '{') p++;
    while (*p) {
        while (*p && *p != '"' && *p != '}') p++;
        if (*p == '}' || !*p) break;
        p++; /* skip " */
        char mcc[8]; int mi = 0;
        while (*p && *p != '"' && mi < 7) mcc[mi++] = *p++;
        mcc[mi] = '\0';
        if (*p == '"') p++;
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        while (*p == ' ') p++;
        char *end; float risk = (float)strtod(p, &end); p = end;
        /* insert into hash table */
        uint32_t h = mcc_hash(mcc);
        if (mcc_pool_used < 2048) {
            MccEntry *e = &mcc_pool[mcc_pool_used++];
            strncpy(e->mcc, mcc, 7); e->mcc[7] = '\0';
            e->risk = risk; e->next = mcc_table[h]; mcc_table[h] = e;
        }
        while (*p && *p != ',' && *p != '}') p++;
        if (*p == ',') p++;
    }
    free(buf);
    return 0;
}

/* ── Index mmap ─────────────────────────────────────────────────────── */
static void         *g_map      = NULL;
static size_t        g_map_size = 0;
static IndexHeader  *g_hdr      = NULL;
static IVFCluster   *g_clusters = NULL;
static RefEntry     *g_entries  = NULL;

int index_load(const char *index_path, const char *mcc_path) {
    /* Load MCC risk */
    if (load_mcc_risk(mcc_path) < 0) return -1;

    /* mmap index.bin */
    int fd = open(index_path, O_RDONLY);
    if (fd < 0) { perror(index_path); return -1; }
    struct stat st;
    fstat(fd, &st);
    g_map_size = (size_t)st.st_size;
    g_map = mmap(NULL, g_map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (g_map == MAP_FAILED) { perror("mmap"); return -1; }

    g_hdr      = (IndexHeader *)g_map;
    if (g_hdr->magic != INDEX_MAGIC || g_hdr->version != INDEX_VERSION) {
        fprintf(stderr, "index: bad magic/version\n"); return -1;
    }
    g_clusters = (IVFCluster *)((char *)g_map + sizeof(IndexHeader));
    g_entries  = (RefEntry   *)((char *)g_clusters +
                                g_hdr->n_clusters * sizeof(IVFCluster));

    /* Advise sequential access within clusters for the probe phase */
    madvise(g_map, g_map_size, MADV_RANDOM);

    fprintf(stderr, "index: loaded %d clusters, %d entries\n",
            g_hdr->n_clusters, g_hdr->n_entries);
    return 0;
}

/* ── Distance ───────────────────────────────────────────────────────── */
/* Squared Euclidean distance between two float vectors.
   Special handling for sentinel -1 in dims 5,6:
   if both are -1 → distance contribution 0; else large penalty. */
static inline float dist2_vec(const float *a, const float *b) {
    float s = 0.0f;
    for (int i = 0; i < VEC_DIM; i++) {
        float da = a[i], db = b[i];
        /* sentinel handling */
        if (i == 5 || i == 6) {
            if (da < -0.5f && db < -0.5f) continue; /* both null → 0 */
            if (da < -0.5f || db < -0.5f) { s += 4.0f; continue; } /* one null */
        }
        float d = da - db;
        s += d * d;
    }
    return s;
}

/* Squared distance between query float vec and quantized RefEntry */
static inline float dist2_entry(const float *q, const RefEntry *e) {
    float s = 0.0f;
    for (int i = 0; i < VEC_DIM; i++) {
        float ea = (float)e->vec[i] / (float)QUANT_SCALE;
        float da = q[i], db = ea;
        if (i == 5 || i == 6) {
            if (da < -0.5f && db < -0.5f) continue;
            if (da < -0.5f || db < -0.5f) { s += 4.0f; continue; }
        }
        float d = da - db;
        s += d * d;
    }
    return s;
}

/* ── Simple max-heap for top-K ──────────────────────────────────────── */
typedef struct { float dist; uint8_t is_fraud; } HeapItem;

static void heap_push(HeapItem *h, int *sz, int cap, float d, uint8_t fr) {
    if (*sz < cap) {
        /* insert and sift up */
        int i = (*sz)++;
        h[i].dist = d; h[i].is_fraud = fr;
        while (i > 0) {
            int p = (i - 1) / 2;
            if (h[p].dist < h[i].dist) { HeapItem t=h[p];h[p]=h[i];h[i]=t; i=p; }
            else break;
        }
    } else if (d < h[0].dist) {
        /* replace root */
        h[0].dist = d; h[0].is_fraud = fr;
        int i = 0;
        for (;;) {
            int l=2*i+1, r=2*i+2, m=i;
            if (l < cap && h[l].dist > h[m].dist) m = l;
            if (r < cap && h[r].dist > h[m].dist) m = r;
            if (m == i) break;
            HeapItem t=h[i];h[i]=h[m];h[m]=t; i=m;
        }
    }
}

/* ── Public KNN query ───────────────────────────────────────────────── */
FraudResult index_query(const float query[VEC_DIM]) {
    FraudResult res = {true, 0.0f};
    if (!g_hdr) return res;

    int nc = g_hdr->n_clusters;

    /* Step 1: distance to each centroid */
    float cdist[IVF_CLUSTERS];
    if (nc > IVF_CLUSTERS) nc = IVF_CLUSTERS; /* safety */
    for (int i = 0; i < nc; i++)
        cdist[i] = dist2_vec(query, g_clusters[i].centroid);

    /* Step 2: select top IVF_PROBE cluster indices (partial sort) */
    int probe_idx[IVF_PROBE];
    int nprobe = (IVF_PROBE < nc) ? IVF_PROBE : nc;
    /* simple selection — nprobe is small */
    bool used[IVF_CLUSTERS] = {false};
    for (int p = 0; p < nprobe; p++) {
        float best = 1e30f; int bi = 0;
        for (int i = 0; i < nc; i++) {
            if (!used[i] && cdist[i] < best) { best = cdist[i]; bi = i; }
        }
        probe_idx[p] = bi; used[bi] = true;
    }

    /* Step 3: linear scan in probed clusters, keep top-K */
    HeapItem heap[KNN_K];
    int heap_sz = 0;
    for (int p = 0; p < nprobe; p++) {
        IVFCluster *cl = &g_clusters[probe_idx[p]];
        RefEntry   *base = g_entries + cl->offset;
        for (int i = 0; i < cl->count; i++) {
            float d = dist2_entry(query, &base[i]);
            heap_push(heap, &heap_sz, KNN_K, d, base[i].is_fraud);
        }
    }

    /* Step 4: count frauds */
    int frauds = 0;
    for (int i = 0; i < heap_sz; i++) frauds += heap[i].is_fraud;
    float score = (heap_sz > 0) ? (float)frauds / (float)heap_sz : 0.0f;
    res.fraud_score = score;
    res.approved    = score < FRAUD_THRESHOLD;
    return res;
}