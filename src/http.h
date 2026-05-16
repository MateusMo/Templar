#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>

int http_serve(int port, int n_threads);
void http_set_ready(bool ready);

#endif