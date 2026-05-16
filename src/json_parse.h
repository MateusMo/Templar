#ifndef JSON_PARSE_H
#define JSON_PARSE_H

#include "templar.h"

int json_parse_tx(const char *body, int body_len, TxPayload *out);

#endif