# Templar — Rinha de Backend 2026

⠀⠀⠀⠀⢀⡠⣾⣳⡀⠀⠀⠀⠀⠀       
⠀⠀⡀⠀⠚⢿⣿⣿⡿⠙⠀⠀⠀⠀
⠀⣘⣿⣇⡀⢘⣿⣿⠀⢀⣠⣶⡀⠀
⠺⣿⣷⣝⣾⣿⣿⣿⣿⣿⣹⣷⣿⠆
⠀⠘⠟⠁⠀⠀⣿⣟⠀⠀⠙⠿⠁⠀
⠀⠀⠀⠀⠀⠀⣿⣿⠀⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⢠⣿⣿⠀⠀⠀⠀⠀⠀
⠀⠀⠀⠀⠀⢸⣿⡿⡄⠀⠀⠀⠀⠀
⠀⠀⠀⠠⣖⣿⣿⣻⡷⡄⠀⠀⠀⠀
⠀⠀⠀⠀⠀⠈⢻⡟⠁⠀⠀⠀⠀⠀


Detecção de fraude em C11 puro. Zero dependências externas na API (só libc + libpthread).

## Estratégia de performance

| Técnica | Impacto |
|---|---|
| Parser HTTP manual (zero-copy) | Sem overhead de framework |
| Parser JSON hand-rolled | Sem malloc por request |
| IVF Index (K-means offline) | Busca KNN sub-linear O(N/K × probe) |
| Quantização int16 (escala 10000) | Metade do espaço vs float32 |
| `SO_REUSEPORT` + epoll por thread | Sem lock no caminho crítico |
| `mmap` do índice | Página cacheada pelo kernel |
| Nginx keepalive upstream | Reuso de conexões HTTP |

## Arquitetura

```
Client → nginx :9999 (round-robin) → api1 :8080
                                   → api2 :8080
```

Cada instância da API:
- N threads, cada uma com seu próprio `epoll` + socket `SO_REUSEPORT`
- Kernel distribui conexões naturalmente
- Index lido via `mmap`, compartilhado pelo kernel entre threads

## Recursos (docker-compose)

| Serviço | CPU | Memória |
|---|---|---|
| nginx | 0.10 | 30 MB |
| api1 | 0.45 | 160 MB |
| api2 | 0.45 | 160 MB |
| **Total** | **1.00** | **350 MB** |

## Estrutura

```
.
├── src/
│   ├── main.c          # entry point, inicializa índice e servidor
│   ├── http.c          # servidor HTTP/1.1, epoll, SO_REUSEPORT
│   ├── json_parse.c    # parser JSON manual zero-copy
│   ├── vectorize.c     # vetorização 14 dimensões + quantização
│   ├── index.c         # IVF index: carga (mmap) e busca KNN
│   ├── fraud.c         # pipeline completo: parse → vetorizar → KNN
│   └── templar.h       # structs e constantes compartilhadas
├── build_index/
│   └── builder.c       # builder offline: K-means + gravação index.bin
├── Makefile
├── Dockerfile
├── docker-compose.yml
└── nginx.conf
```

## Como rodar

```bash
# 1. Coloque os dados em bench/
cp /path/to/references.json.gz bench/references.json.gz
cp /path/to/mcc_risk.json      bench/mcc_risk.json

# 2. Suba com docker (build do índice acontece dentro do Dockerfile)
make docker-up

# 3. Teste
curl http://localhost:9999/ready

curl -s -X POST http://localhost:9999/fraud-score \
  -H 'Content-Type: application/json' \
  -d '{
    "id": "tx-1",
    "transaction": {"amount": 41.12, "installments": 2, "requested_at": "2026-03-11T18:45:53Z"},
    "customer": {"avg_amount": 82.24, "tx_count_24h": 3, "known_merchants": ["MERC-003","MERC-016"]},
    "merchant": {"id": "MERC-016", "mcc": "5411", "avg_amount": 60.25},
    "terminal": {"is_online": false, "card_present": true, "km_from_home": 29.23},
    "last_transaction": null
  }'
# Esperado: {"approved":true,"fraud_score":0.0000}
```

## Build local (sem Docker)

```bash
# API
make build/templar_api

# Builder de índice
make build/build_index

# Gerar índice (requer bench/references.json.gz e bench/mcc_risk.json)
make index

# Rodar localmente
./build/templar_api 9999 4 build/index.bin bench/mcc_risk.json
```

## Tuning

- `IVF_CLUSTERS` (templar.h): mais clusters → busca mais rápida mas K-means mais lento no build
- `IVF_PROBE` (templar.h): mais probes → mais recall mas mais lento na query
- `threads` (CMD no Dockerfile): 2 por instância é ótimo com 0.45 CPU cada
- Para aumentar recall sem perder velocidade: aumentar `IVF_PROBE` de 8 para 12
