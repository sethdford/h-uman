/* human_bench: main entry and benchmark runner. */
#include "bench.h"
#include <stdio.h>

extern void run_bench_json(void);
extern void run_bench_memory(void);
extern void run_bench_config(void);

int main(void) {
    printf("=== human core benchmarks ===\n\n");

    printf("--- JSON ---\n");
    run_bench_json();
    printf("\n");

    printf("--- Memory ---\n");
    run_bench_memory();
    printf("\n");

    printf("--- Config ---\n");
    run_bench_config();
    printf("\n");

    printf("=== done ===\n");
    return 0;
}
