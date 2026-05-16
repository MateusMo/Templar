#ifndef FRAUD_H
#define FRAUD_H

#include "templar.h"

FraudResult fraud_score(const char *body, int body_len);

#endif