.PHONY: all build docker-up docker-down clean index

CC      = gcc
CFLAGS  = -std=c11 -O3 -march=native -funroll-loops -ffast-math -Wall -Wextra -DNDEBUG
LDFLAGS = -lm -lpthread
ZFLAGS  = -lz

SRC_DIR   = src
BUILD_DIR = build
IDX_DIR   = build_index

API_SRCS  = $(SRC_DIR)/main.c $(SRC_DIR)/http.c $(SRC_DIR)/json_parse.c \
            $(SRC_DIR)/vectorize.c $(SRC_DIR)/index.c $(SRC_DIR)/fraud.c
IDX_SRCS  = $(IDX_DIR)/builder.c $(SRC_DIR)/vectorize.c

all: $(BUILD_DIR)/templar_api $(BUILD_DIR)/build_index

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/templar_api: $(API_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $(API_SRCS) $(LDFLAGS)

$(BUILD_DIR)/build_index: $(IDX_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $(IDX_SRCS) $(LDFLAGS) $(ZFLAGS)

# Build the index from references.json.gz
index: $(BUILD_DIR)/build_index
	@echo "Building index from bench/references.json.gz ..."
	$(BUILD_DIR)/build_index bench/references.json.gz bench/mcc_risk.json build/index.bin
	@echo "Index built at build/index.bin"

docker-up:
	docker compose up --build -d

docker-down:
	docker compose down

clean:
	rm -rf $(BUILD_DIR)