/* The spectrum's FFT (src/hfft.s, HippoPlayer's) under vamos: a sine at a
 * known bin comes out at that bin. Prints each case's loudest bins. */
#include <stdio.h>
#include <exec/types.h>

void hfft_window(WORD *re, const WORD *sine);
void hfft_fft(WORD *re, WORD *im, const WORD *sine);
void vis_tables(WORD *sine);

static WORD sine[1024], re[128], im[128];

int main(void)
{
    static const int bins[] = { 3, 10, 31, 50 };
    int c, i, j;
    vis_tables(sine);
    for (c = 0; c < 4; c++) {
        LONG best[3] = { -1, -1, -1 }, bm[3] = { 0, 0, 0 };
        for (i = 0; i < 128; i++)
            re[i] = (WORD)(sine[(i * bins[c] * 8) & 1023] * 2 / 3);   /* bins[c] cycles in 128 */
        hfft_window(re, sine);
        hfft_fft(re, im, sine);
        for (i = 0; i < 64; i++) {
            LONG m = (LONG)re[i] * re[i] + (LONG)im[i] * im[i];
            for (j = 0; j < 3; j++)
                if (m > bm[j]) {
                    int k;
                    for (k = 2; k > j; k--) {
                        bm[k] = bm[k - 1];
                        best[k] = best[k - 1];
                    }
                    bm[j] = m;
                    best[j] = i;
                    break;
                }
        }
        printf("sine at %ld: loudest %ld (%ld), then %ld, %ld\n", (long)bins[c], (long)best[0],
               (long)bm[0], (long)best[1], (long)best[2]);
    }
    return 0;
}
