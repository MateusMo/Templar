/*
* main.c — Templar: Rinha de Backend 2026 entry point.
 *
 * Usage: templar_api [port] [threads] [index.bin] [mcc_risk.json]
 *
 * Defaults:
 *   port        = 8080  (nginx LB will forward from 9999)
 *   threads     = 4
 *   index.bin   = /data/index.bin
 *   mcc_risk    = /data/mcc_risk.json
 */
#include "index.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    int         port       = 8080;
    int         threads    = 4;
    const char *index_path = "/data/index.bin";
    const char *mcc_path   = "/data/mcc_risk.json";

    if (argc > 1) port       = atoi(argv[1]);
    if (argc > 2) threads    = atoi(argv[2]);
    if (argc > 3) index_path = argv[3];
    if (argc > 4) mcc_path   = argv[4];

    fprintf(stderr, "Templar starting — port=%d threads=%d index=%s mcc=%s\n",
            port, threads, index_path, mcc_path);

    if (index_load(index_path, mcc_path) < 0) {
        fprintf(stderr, "FATAL: could not load index\n");
        return 1;
    }

    http_set_ready(true);
    fprintf(stderr, "Templar ready.\n");

    return http_serve(port, threads);
}