#include "util.h"

#include <stdio.h>
#include <math.h>

static int simple_pow(int x, int y)
{
    int result = 0;
    for (int i = 0; i < y; i++) {
        if (i == 0) {
            result = x;
        } else {
            result *= x;
        }
    }
    return result;
}

void float_to_str(float value, char *buf, uint8_t decimals)
{
    float int_val;
    float frac_val;
    float frac_multiplier;
    int disp_int;
    int disp_frac;

    if (isnanf(value)) {
        sprintf(buf, "nan");
    } else if (isinff(value)) {
        sprintf(buf, "inf");
    } else {
        frac_val = modff(value, &int_val);
        frac_multiplier = simple_pow(10, decimals);
        disp_int = (int)fabsf(lroundf(int_val));
        disp_frac = (int)fabsf(lroundf(frac_val * frac_multiplier));
        if (value < 0.0F) {
            sprintf(buf, "-%d.%0*d", disp_int, decimals, disp_frac);
        } else {
            sprintf(buf, "%d.%0*d", disp_int, decimals, disp_frac);
        }
    }
}