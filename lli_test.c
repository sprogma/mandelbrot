#include <stdio.h>

#define LLI_SOURCE
#include "lli.h"

void lli_print_hex(struct lli *a, const char *name) {
    _lli_to_i_state(a, 1); 
    printf("%s (hex): ", name);
    if (a->sign == LLI_SIGN_NEG) printf("-");
    printf("{ ops=%d/%d ", a->ops_cnt, a->max_ops_cnt);
    bool pr = false;
    for (int64_t i = a->length - 1; i >= 0; i--) 
    {
        if (pr || a->idata[i] > 0 || i == 0) 
        { 
            pr = true;
            printf(" %016llx", a->idata[i]);
        }
    }
    printf(" }\n");
}

int main() 
{
    
    int64_t item_bits = 10; // 4 multiplication - ok
    int64_t bits = 5120;
    int64_t bit_exp = 1000;

    struct lli *tmp[8];

    for (int i = 0; i < 8; ++i) tmp[i] = lli_create(bits, item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);

    lli_load_double(tmp[0], 1.0, bit_exp);
    lli_load_double(tmp[1], 1.0, bit_exp);

    printf("%f\n", lli_as_double(tmp[0], bit_exp));

    lli_mul(tmp[0], tmp[1], 0);
    lli_adam(tmp[0], bit_exp);
    lli_mul(tmp[0], tmp[1], 0);
    lli_adam(tmp[0], bit_exp);
    lli_mul(tmp[0], tmp[1], 0);
    lli_adam(tmp[0], bit_exp);
    
    lli_mul(tmp[2], tmp[0], 0);
    lli_adam(tmp[2], bit_exp);
    lli_mul(tmp[2], tmp[0], 0);
    lli_mul(tmp[2], tmp[0], 0);
    lli_mul(tmp[2], tmp[0], 0);
    
    printf("%f\n", lli_as_double(tmp[0], bit_exp));
    printf("%f\n", lli_as_double(tmp[0], bit_exp));
    printf("%f\n", lli_as_double(tmp[0], bit_exp));
    printf("%f\n", lli_as_double(tmp[0], bit_exp));

    


    return 0;
}

