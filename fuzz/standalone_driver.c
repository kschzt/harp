/* Standalone driver for the fuzz targets on toolchains without the
 * libFuzzer runtime (Apple clang). Replays corpus files under ASan —
 * a regression runner, not a fuzzer; the coverage-guided runs happen on
 * Linux CI where libFuzzer links. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
    int ran = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n < 0) n = 0;
        uint8_t *buf = malloc((size_t)n + 1);
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            LLVMFuzzerTestOneInput(buf, (size_t)n);
            ran++;
        }
        free(buf);
        fclose(f);
    }
    printf("replayed %d input(s) clean\n", ran);
    return 0;
}
