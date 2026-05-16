cmake_minimum_required(VERSION 3.18)
project(Templar C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native -funroll-loops -ffast-math -DNDEBUG")

# API server
add_executable(templar_api
        src/main.c
        src/http.c
        src/json_parse.c
        src/vectorize.c
        src/index.c
        src/fraud.c
)
target_link_libraries(templar_api m pthread)
target_compile_options(templar_api PRIVATE -O3 -march=native -funroll-loops -ffast-math)

# Offline index builder
add_executable(build_index
        build_index/builder.c
        src/vectorize.c
)
target_link_libraries(build_index m z)
target_compile_options(build_index PRIVATE -O3 -march=native)