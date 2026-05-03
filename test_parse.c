#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define NR5G_MASK_BYTES 64

static inline void nr5g_mask_set_band(uint8_t *mask, int band) {
    if (band > 0 && band <= NR5G_MASK_BYTES * 8) {
        int byte_idx = (band - 1) / 8;
        int bit_idx = (band - 1) % 8;
        mask[byte_idx] |= (1 << bit_idx);
    }
}

static int parse_nr_band_token(const char *tok) {
    const char *numstr = tok;
    if ((tok[0] == 'n' || tok[0] == 'N') && tok[1] >= '0' && tok[1] <= '9')
        numstr = tok + 1;
    int band = atoi(numstr);
    if (band > 0 && band <= 512) return band;
    return 0;
}

static void parse_nr_bands(const char *spec, uint8_t *nr_mask) {
    if (strchr(spec, ',') || strchr(spec, '+')) {
        char *tmp = strdup(spec);
        for (char *p = tmp; *p; p++) {
            if (*p == '+') *p = ',';
        }
        char *tok = strtok(tmp, ",");
        while (tok) {
            while (*tok == ' ') tok++;
            int band = parse_nr_band_token(tok);
            if (band > 0) nr5g_mask_set_band(nr_mask, band);
            tok = strtok(NULL, ",");
        }
        free(tmp);
        return;
    }

    int band = parse_nr_band_token(spec);
    if (band > 0) {
        nr5g_mask_set_band(nr_mask, band);
        return;
    }
}

int main() {
    uint8_t mask[64];
    memset(mask, 0, 64);
    parse_nr_bands("n78", mask);
    printf("n78: ");
    for(int i=0;i<16;i++) printf("%02x ", mask[i]);
    printf("\n");
    
    memset(mask, 0, 64);
    parse_nr_bands("n28+n78", mask);
    printf("n28+n78: ");
    for(int i=0;i<16;i++) printf("%02x ", mask[i]);
    printf("\n");

    /* Simulating nr_len calculation */
    int nr_len = NR5G_MASK_BYTES;
    while (nr_len > 8 && mask[nr_len - 1] == 0) nr_len--;
    nr_len = ((nr_len + 7) / 8) * 8;
    printf("nr_len for n28+n78 = %d\n", nr_len);

    return 0;
}
