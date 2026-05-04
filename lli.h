#ifndef LLI_H
#define LLI_H

#include "inttypes.h"
#include "mm_malloc.h"


struct lli
{
    /* at 0 */
    int8_t creation_flags;
    int8_t sign;
    int8_t ops_cnt; // count of multiplications since last conversion
    int8_t max_ops_cnt;
    int8_t state;
    int8_t bits;
    /* at 8 */
    uint64_t length;
    /* at 16 */
    uint64_t bits_mask;
    /* at 24 */
    uint64_t _pad;
    /* at 32 */
    union
    {
        double fdata[]; // idata + rdata in this order.
        uint64_t idata[];
    };
};

struct llf
{
    struct lli *val;
    int64_t exp;
};


// use num_free_multiplications < 0 to default values
#define LLI_GET_BASE_BITS_OPTIMIZE_MULTIPLICATION 0x1
int64_t lli_get_base_bits(int64_t bits, int64_t num_free_multiplications, int64_t flags);


#define LLI_CREATE_OPTIMIZE_MULTIPLICATION 0x1
#define LLI_CREATE_STACK_ALLOCATION 0x4
#define lli_create(bits, base_bits, flags, value) \
    ({ \
        uint64_t __lli__size = bits/base_bits + 1, __lli__sizeff; struct lli *__lli__data; \
        __lli__size = (__lli__size + 7) & -8; \
        if (flags & LLI_CREATE_OPTIMIZE_MULTIPLICATION) \
        { \
            __lli__size--; \
            __lli__size |= __lli__size >> 1ull; \
            __lli__size |= __lli__size >> 2ull; \
            __lli__size |= __lli__size >> 4ull; \
            __lli__size |= __lli__size >> 8ull; \
            __lli__size |= __lli__size >> 16ull; \
            __lli__size |= __lli__size >> 32ull; \
            __lli__size++; \
            __lli__sizeff = 2 * sizeof(double[2]); \
        } \
        else __lli__sizeff = sizeof(uint64_t); \
        if (flags & LLI_CREATE_STACK_ALLOCATION) __lli__data = (struct lli *)(((uint64_t)(alloca(__lli__size * __lli__sizeff + sizeof(struct lli) + 32)) + 31) & (-32)); \
        else __lli__data = (struct lli *)_mm_malloc(__lli__size * __lli__sizeff + sizeof(struct lli), 32); \
        __lli__data->idata[0] = value; \
        _lli_create(__lli__size, base_bits, flags, (struct lli *)__lli__data); \
    })
    
struct lli *_lli_create(int64_t length, int64_t base_bits, int64_t flags, struct lli *data);

void lli_free(struct lli *);


/* base */

#define LLI_MUL_USE_I_STATE 0x1
#define LLI_MUL_USE_F_STATE 0x2
void lli_mul(struct lli *, struct lli *, int64_t flags);

#define LLI_ADD_USE_I_STATE 0x1
#define LLI_ADD_USE_F_STATE 0x2
void lli_add(struct lli *, struct lli *, int64_t flags);

void lli_neg(struct lli *, int64_t flags);
void lli_adam(struct lli *, int64_t n);

void lli_zero(struct lli *);
void lli_copy(struct lli *, struct lli *);

double lli_as_double(struct lli *, int64_t exp2);
void lli_as_double2(struct lli *, int64_t exp2, double *out_m, int64_t *out_e);
void lli_load_double(struct lli *, double value, int64_t exp2);

#ifdef LLI_EXPORT_MACROS
    #define lmul lli_mul
    #define ladd lli_add
    #define lneg lli_neg
#endif

#ifdef LLI_SOURCE

#include "malloc.h"
#include "string.h"
#include "immintrin.h"
#include "assert.h"
#include "math.h"

#define LLI_STATE_F 0
#define LLI_STATE_I 1

#define LLI_SIGN_POS 0x00
#define LLI_SIGN_NEG (int8_t)0xFF

#define ALIGN32(x) x = __builtin_assume_aligned(x, 32);

int64_t lli_get_base_bits(int64_t bits, int64_t num_free_multiplications, int64_t flags)
{
    if (flags & LLI_GET_BASE_BITS_OPTIMIZE_MULTIPLICATION)
    {
        int64_t T = (num_free_multiplications <= 0 ? 8 : num_free_multiplications); // 8 free multiplications
        
        /* calculate bits */
        /*
            (bits/item_bits) * 4^item_bits * T < 2^52   
        */
        bits += 64; // add reserve
        uint64_t item_bits = 0;
        for (uint64_t i = 1ull; i < 53ull; ++i)
        {
            if ((bits/i + 1ull) * (1ull<<(i * (T+1))) > (1ull<<52ull))
            {
                item_bits = i - 1;
                break;
            }
        }
        return item_bits;
    }
    return 64;
}


struct lli *_lli_create(int64_t length, int64_t base_bits, int64_t flags, struct lli *data)
{
    ALIGN32(data);
    
    int64_t value = data->idata[0];
    memset(data->idata, 0, sizeof(*data->idata) * length);
    if (value < 0) { data->idata[0] = -value; } else {data->idata[0] = value;}
    
    uint64_t bits_mask = (base_bits == 64 ? -1ll : (1ll<<base_bits) - 1ll);
    data->state = LLI_STATE_I;
    data->length = length;
    data->ops_cnt = 0;
    data->max_ops_cnt = 0;
    data->bits_mask = bits_mask;
    data->bits = base_bits;
    data->creation_flags = flags;
    data->sign = (value > 0 ? LLI_SIGN_POS : LLI_SIGN_NEG);
    if (flags & LLI_CREATE_OPTIMIZE_MULTIPLICATION)
    {
        while (length * (1ull<<(base_bits*(data->max_ops_cnt+1))) <= (1ull<<52ull))
        {
            data->max_ops_cnt++;
        }
        data->max_ops_cnt--;
    }
    return data;
}

void lli_copy(struct lli * __restrict__ a, struct lli * __restrict__ b)
{
    if (b->state == LLI_STATE_I)
    {
        memcpy(a, b, sizeof(a) + sizeof(*b->idata) * b->length);
    }
    else
    {
        memcpy(a, b, sizeof(a) + sizeof(*b->fdata) * 4 * b->length); // copy 4n data
    }
}

void lli_zero(struct lli * __restrict__ a)
{
    if (a->state == LLI_STATE_I)
    {
        memset(a->idata, 0, sizeof(*a->idata) * a->length);
        a->ops_cnt = 0;
    }
    else
    {
        memset(a->fdata, 0, sizeof(*a->fdata) * 4 * a->length); // set 4n data
        a->ops_cnt = 0;
    }
}


void _lli_algo_mul_i(struct lli * __restrict__ a, struct lli * __restrict__ b, int64_t carry);
void _lli_algo_mul_f(struct lli * __restrict__ a, struct lli * __restrict__ b);
void _lli_algo_add_i(struct lli * __restrict__ a, struct lli * __restrict__ b, int64_t carry);
void _lli_algo_add_f(struct lli * __restrict__ a, struct lli * __restrict__ b);
void _lli_algo_sub_i(struct lli * __restrict__ a, struct lli * __restrict__ b, int64_t carry);
void _lli_algo_sub_f(struct lli * __restrict__ a, struct lli * __restrict__ b);


void _lli_i_norm_state(struct lli * __restrict__ a);
void _lli_to_i_state(struct lli * __restrict__ a, int normalize);
void _lli_to_f_state(struct lli * __restrict__ a);


#define LLI_MUL_USE_I_STATE 0x1
#define LLI_MUL_USE_F_STATE 0x2
void lli_mul(struct lli * __restrict__ a, struct lli * __restrict__ b, int64_t flags)
{
    ALIGN32(a);
    ALIGN32(b);
    
    a->sign ^= b->sign;

    int simple = 0;
    simple |= flags & LLI_MUL_USE_I_STATE;
    // simple |= a->state == LLI_STATE_I && b->state == LLI_STATE_I && a->length <= 256 && !(flags & LLI_MUL_USE_F_STATE);
    if (a->ops_cnt + b->ops_cnt + 1 > a->max_ops_cnt)
    {
        if (b->ops_cnt > 0)
        {
            _lli_to_i_state(b, 1);
        }
        if (a->ops_cnt + 1 > a->max_ops_cnt)
        {
            _lli_to_i_state(a, 1);
        }
    }
    if (simple)
    {
        assert(!(flags & LLI_MUL_USE_F_STATE) && "This multiplication was forced to use I state, but have USE_F_STATE flag");

        _lli_to_i_state(a, 0);
        _lli_to_i_state(b, 0);
        a->ops_cnt += b->ops_cnt + 1;
        _lli_algo_mul_i(a, b, 0);
    }
    else // in multiplication, usage of F state is always better, if one of number is already in I state
    {
        _lli_to_f_state(a);
        _lli_to_f_state(b);

        a->ops_cnt += b->ops_cnt + 1;
        _lli_algo_mul_f(a, b);
        _lli_to_i_state(b, 1); // TODO: fix bug when without this
    }
}


#define LLI_ADD_USE_I_STATE 0x1
#define LLI_ADD_USE_F_STATE 0x2
void lli_add(struct lli * __restrict__ a, struct lli * __restrict__ b, int64_t flags)
{
    ALIGN32(a);
    ALIGN32(b);
        
    int simple = 0;
    simple |= flags & LLI_ADD_USE_I_STATE;
    simple |= (a->state == LLI_STATE_I || b->state == LLI_STATE_I) && !(flags & LLI_ADD_USE_F_STATE);
    simple |= a->ops_cnt + 1 > a->max_ops_cnt || b->ops_cnt + 1 > a->max_ops_cnt;
    if (simple)
    {
        assert(!(flags & LLI_ADD_USE_F_STATE) && "This addition was forced to use I state, but have USE_F_STATE flag");

        if (a->ops_cnt + 1 > a->max_ops_cnt || b->ops_cnt + 1 > a->max_ops_cnt)
        {
            _lli_to_i_state(a, 1);
            _lli_to_i_state(b, 1);
            if (a->sign == b->sign) _lli_algo_add_i(a, b, 1);
            else                    _lli_algo_sub_i(a, b, 1);
        }
        else
        {
            _lli_to_i_state(a, 0);
            _lli_to_i_state(b, 0);
            if (a->ops_cnt < b->ops_cnt)
            {
                a->ops_cnt = b->ops_cnt;
            }
            a->ops_cnt += 1;
            if (a->sign == b->sign) _lli_algo_add_i(a, b, 0);
            else                    _lli_algo_sub_i(a, b, 0);
        }
    }
    else // in addition, usage of F state is always worse, if one of number is already in I state
    {
        _lli_to_f_state(a);
        _lli_to_f_state(b);
        
        if (a->ops_cnt < b->ops_cnt)
        {
            a->ops_cnt = b->ops_cnt;
        }
        a->ops_cnt += 1;
        if (a->sign == b->sign) _lli_algo_add_f(a, b);
        else                    _lli_algo_sub_f(a, b);
    }    
}


void lli_neg(struct lli * __restrict__ a, int64_t flags)
{
    ALIGN32(a);
    
    (void)flags;
    a->sign ^= LLI_SIGN_NEG;
}


void lli_as_double2(struct lli * __restrict__ a, int64_t exp2, double * __restrict__ out_m, int64_t * __restrict__ out_e)
{
    ALIGN32(a);

    _lli_to_i_state(a, 0);
    _lli_i_norm_state(a);

    exp2 = -exp2;
    
    int64_t idx = a->length - 1;
    uint64_t hvalue = 0;
    while (idx >= 0) 
    {
        hvalue = a->idata[idx] & a->bits_mask;
        if (hvalue != 0)
        {
            break;
        }
        idx--;
    }

    if (idx < 0)
    {
        *out_m = 0.0;
        *out_e = 1;
        return;
    }

    int bincell = 63 - __builtin_clzll(hvalue);
    int64_t total_bit_pos = (idx * a->bits) + bincell;

    uint64_t m = 0;
    int64_t current = 0;
    int64_t target_bits = 54;

    {
        int64_t fst = bincell + 1;
        int64_t take = (fst > target_bits) ? target_bits : fst;
        m = (hvalue >> (fst - take));
        current += take;
        idx--;
    }

    while (current < target_bits && idx >= 0) 
    {
        uint64_t value = a->idata[idx] & a->bits_mask;
        int needed = target_bits - current;
        
        if (a->bits <= needed) 
        {
            m = (m << a->bits) | value;
            current += a->bits;
        } 
        else 
        {
            m = (m << needed) | (value >> (a->bits - needed));
            current += needed;
        }
        idx--;
    }

    if (current < target_bits) 
    {
        m <<= (target_bits - current);
    }

    m = (m >> 1) + (m & 1); // 54 bits -> 53 bits
    if (m & (1ull << 53))
    {
        m >>= 1;
        exp2++;
    }

    *out_m = (a->sign == LLI_SIGN_NEG) ? -(double)m : (double)m;
    *out_e = exp2 + total_bit_pos - 52;
    return;
}

double lli_as_double(struct lli * __restrict__ a, int64_t exp2) 
{
    ALIGN32(a);
    
    int64_t _exp;
    double _res;

    lli_as_double2(a, exp2, &_res, &_exp);

    return ldexp(_res, _exp);
}


void lli_load_double(struct lli * __restrict__ a, double value, int64_t exp2)
{
    ALIGN32(a);

    a->state = LLI_STATE_I;

    memset(a->idata, 0, a->length * sizeof(*a->idata));

    if (value == 0.0) 
    {
        a->sign = LLI_SIGN_POS;
        return;
    }

    union { double d; uint64_t u; } conv = { .d = value };

    a->sign = (conv.u >> 63) ? LLI_SIGN_NEG : LLI_SIGN_POS;
    int raw_exp = (int)((conv.u >> 52) & 0x7FF);
    uint64_t mantissa = conv.u & 0xFFFFFFFFFFFFFULL;

    int real_exp;
    if (raw_exp == 0) 
    {
        real_exp = 1 - 1023; // denorm
    } 
    else 
    {
        mantissa |= 0x10000000000000ULL;
        real_exp = raw_exp - 1023;
    }

    int64_t total_shift = real_exp + exp2 - 52;

    if (total_shift < 0) 
    {
        return; 
    }

    uint64_t cell_idx = total_shift / a->bits;
    uint32_t bit_offset = total_shift % a->bits;

    int needed = 53;
    uint64_t current_mantissa = mantissa;

    for (uint64_t i = cell_idx; i < a->length && needed > 0; i++) 
    {
        a->idata[i] |= (current_mantissa << bit_offset) & a->bits_mask;
        
        int sft = a->bits - bit_offset;
        
        if (needed > sft) 
        {
            current_mantissa >>= sft;
            needed -= sft;
        } 
        else 
        {
            needed = 0;
        }
        
        bit_offset = 0;
    }

    return;
}


void lli_adam(struct lli * __restrict__ a, int64_t n)
{
    ALIGN32(a);
    
    if (n == 0) return;

    _lli_to_i_state(a, 0);
    _lli_i_norm_state(a);
    
    uint64_t cell_shift = n / a->bits;
    uint32_t bit_shift = n % a->bits;
    
    if (cell_shift >= a->length)
    {
        memset(a->idata, 0, a->length * sizeof(*a->idata));
        return;
    }

    uint64_t limit = a->length - cell_shift;

    for (uint64_t i = 0; i < a->length; i++) 
    {
        if (i < limit) 
        {
            uint64_t current = a->idata[i + cell_shift] & a->bits_mask;
            
            uint64_t res = current >> bit_shift;
            
            if (bit_shift > 0 && (i + cell_shift + 1) < a->length) 
            {
                uint64_t next = a->idata[i + cell_shift + 1] & a->bits_mask;
                res |= (next << (a->bits - bit_shift)) & a->bits_mask;
            }
            a->idata[i] = res;
        } 
        else 
        {
            a->idata[i] = 0;
        }
    }
}


void lli_free(struct lli * __restrict__ a)
{
    ALIGN32(a);
    
    if (!(a->creation_flags & LLI_CREATE_STACK_ALLOCATION))
    {
        _mm_free(a);
    }
}

/// ------------------------------------------ algos ------------------------------------------


#define LLI_FFT_FORWARD 0
#define LLI_FFT_INVERSE 1
// :)
#define LLI_PI 3.1415926535897932384626433832795028841971693993751058209749445923
#include "math.h"

// double (fft_wr *)[2][lenid];

double cos_table[2][64];
double sin_table[2][64];

__attribute__((constructor)) 
void _lli_tables(void)
{
    for (int64_t direction = 0; direction <= 1; ++direction)
    {
        uint64_t len = 2;
        for (int64_t i = 0; i < 64; ++i)
        {
            double ang = 2.0 * LLI_PI / len * (direction == LLI_FFT_FORWARD ? 1 : -1);

            cos_table[direction][i] = cos(ang);
            sin_table[direction][i] = sin(ang);
            
            len <<= 1;
        }
    }
}

void _lli_fft_core(struct lli * __restrict__ a, int direction) 
{
    ALIGN32(a);

    uint64_t n = 2*a->length;
    if (n < 2) return;

    double *idata = a->fdata;
    double *rdata = a->fdata + n;

    for (uint64_t len = 2, lenid = 0; len <= n; len <<= 1, lenid++) 
    {
        double wlen_r = cos_table[direction][lenid];
        double wlen_i = sin_table[direction][lenid];
        
        if (len > 8) // len / 2 > 4
        {
            __m256d step4_r = _mm256_set1_pd(cos_table[direction][lenid - 2]);
            __m256d step4_i = _mm256_set1_pd(sin_table[direction][lenid - 2]);
            
            _Alignas(32) double sw_r[4];
            _Alignas(32) double sw_i[4];
            double isw_r = 1.0, isw_i = 0.0;
            
            #pragma clang loop unroll_count(4)
            for (uint64_t t = 0; t < 4; ++t)
            {
                sw_r[t] = isw_r;
                sw_i[t] = isw_i;
                if (t != 3)
                {
                    double next_isw_r = isw_r * wlen_r - isw_i * wlen_i;
                    double next_isw_i = isw_r * wlen_i + isw_i * wlen_r;
                    isw_r = next_isw_r;
                    isw_i = next_isw_i;
                }
            }
        
            /* simd version */
            for (uint64_t i = 0; i < n; i += len) 
            {
                __m256d w_r = _mm256_load_pd(sw_r);
                __m256d w_i = _mm256_load_pd(sw_i);
                
                uint64_t size = len / 2;
                uint64_t limit = i + size;
                for (uint64_t j = i; j < limit; j += 4)
                {
                    uint64_t idx1 = j;
                    uint64_t idx2 = j + size;

                    __m256d u_r = _mm256_load_pd(rdata + idx1);
                    __m256d u_i = _mm256_load_pd(idata + idx1);
                    
                    __m256d rv_r = _mm256_load_pd(rdata + idx2);
                    __m256d rv_i = _mm256_load_pd(idata + idx2);

                    __m256d v_r = _mm256_fmsub_pd(rv_r, w_r, _mm256_mul_pd(rv_i, w_i));
                    __m256d v_i = _mm256_fmadd_pd(rv_r, w_i, _mm256_mul_pd(rv_i, w_r));

                    _mm256_store_pd(rdata + idx1, _mm256_add_pd(u_r, v_r));
                    _mm256_store_pd(idata + idx1, _mm256_add_pd(u_i, v_i));
                    _mm256_store_pd(rdata + idx2, _mm256_sub_pd(u_r, v_r));
                    _mm256_store_pd(idata + idx2, _mm256_sub_pd(u_i, v_i));

                    __m256d next_w_r = _mm256_fmsub_pd(w_r, step4_r, _mm256_mul_pd(w_i, step4_i));
                    __m256d next_w_i = _mm256_fmadd_pd(w_r, step4_i, _mm256_mul_pd(w_i, step4_r));
                    w_r = next_w_r;
                    w_i = next_w_i;
                }
            }
        }
        else
        {                        
            for (uint64_t i = 0; i < n; i += len) 
            {
                double w_r = 1.0;
                double w_i = 0.0;
                for (uint64_t j = 0; j < len / 2; j++) 
                {
                    uint64_t idx1 = i + j;
                    uint64_t idx2 = i + j + len / 2;

                    double u_r = rdata[idx1];
                    double u_i = idata[idx1];
                    
                    double v_r = rdata[idx2] * w_r - idata[idx2] * w_i;
                    double v_i = rdata[idx2] * w_i + idata[idx2] * w_r;

                    rdata[idx1] = u_r + v_r;
                    idata[idx1] = u_i + v_i;
                    rdata[idx2] = u_r - v_r;
                    idata[idx2] = u_i - v_i;

                    double next_w_r = w_r * wlen_r - w_i * wlen_i;
                    double next_w_i = w_r * wlen_i + w_i * wlen_r;
                    w_r = next_w_r;
                    w_i = next_w_i;
                }
            }
        }
    }

    if (direction == LLI_FFT_INVERSE) 
    {
        double inv_n = 1.0 / n;
        #pragma clang loop vectorize(enable) interleave(enable)
        for (uint64_t i = 0; i < n; i++) {
            rdata[i] *= inv_n;
        }
        #pragma clang loop vectorize(enable) interleave(enable)
        for (uint64_t i = 0; i < n; i++) {
            idata[i] *= inv_n;
        }
    }
}

#define _lli_algo_addsub_i(op_cmd) \
    uint64_t shift = a->bits; \
    uint64_t mask = a->bits_mask; \
    int64_t c = 0; \
 \
    if (shift == 64) \
    { \
        for (uint64_t i = 0; i < a->length; ++i) \
        { \
            __int128 res = (__int128)a->idata[i] op_cmd + c; \
            a->idata[i] = (uint64_t)res; \
            c = (int64_t)(res >> 64);  \
        } \
    } \
    else \
    { \
        for (uint64_t i = 0; i < a->length; ++i) \
        { \
            uint64_t sum = a->idata[i] op_cmd + (uint64_t)c; \
            uint64_t res = sum & mask; \
            a->idata[i] = res; \
            c = (int64_t)((int64_t)(sum - res) >> (int64_t)shift); \
        } \
    } \
 \
    if (c < 0) \
    { \
        a->sign ^= LLI_SIGN_NEG; \
 \
         \
        __m256d v_ms1 = (__m256d)_mm256_set1_epi64x(-1LL); \
        __m256d v_mask = (__m256d)_mm256_set1_epi64x(mask); \
        for (uint64_t i = 0; i < a->length; i += 4)  \
        { \
            __m256i v_data = _mm256_loadu_si256((__m256i*)&a->idata[i]); \
            __m256i v_inv = _mm256_andnot_si256(v_data, v_mask); \
            _mm256_storeu_si256((__m256i*)&a->idata[i], v_inv); \
        } \
 \
        /* find tail */ \
        for (uint64_t i = 0; i < a->length; i += 4)  \
        { \
            __m256i v_data = _mm256_loadu_si256((__m256i*)&a->idata[i]); \
            __m256i v_cmp = _mm256_cmpeq_epi64(v_data, v_mask); \
             \
            if (_mm256_testc_pd((__m256d)v_cmp, v_ms1))  \
            { \
                _mm256_storeu_si256((__m256i *)&a->idata[i], _mm256_setzero_si256()); \
            }  \
            else  \
            { \
                int move_mask = _mm256_movemask_pd((__m256d)v_cmp); \
                int first_not_mask = __builtin_ctz((~move_mask) & 0xF); \
                for (int j = 0; j < first_not_mask; ++j)  \
                { \
                    a->idata[i + j] = 0; \
                } \
                a->idata[i + first_not_mask] += 1; \
                break; \
            } \
        } \
    } \
    a->ops_cnt = 0;


void _lli_i_norm_state(struct lli * __restrict__ a)
{
    ALIGN32(a);
    _lli_algo_addsub_i() // carrry and sign
}


void _lli_to_i_state(struct lli * __restrict__ a, int normalize)
{
    ALIGN32(a);
    if (a->state == LLI_STATE_I) return;

    // full space buffer fly
    uint64_t n = 2*a->length;
    for (uint64_t i = 1, j = 0; i < n; i++) 
    {
        uint64_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) 
        {
            double temp_r = a->fdata[n + i];
            double temp_i = a->fdata[i];
            a->fdata[n + i] = a->fdata[n + j];
            a->fdata[i] = a->fdata[j];
            a->fdata[n + j] = temp_r;
            a->fdata[j] = temp_i;
        }
    }

    _lli_fft_core(a, LLI_FFT_INVERSE);

    /* copy back to ibuffer */
    for (uint64_t i = 0; i < a->length; ++i)
    {
        a->idata[i] = a->fdata[n + i] + 0.5; // round up
    }

    if (normalize) { _lli_i_norm_state(a); }
    
    a->state = LLI_STATE_I;
}


void _lli_to_f_state(struct lli * __restrict__ a)
{
    ALIGN32(a);
    if (a->state == LLI_STATE_F) return;

    uint64_t n = 2*a->length;

    memset(a->fdata + n, 0, n * sizeof(*a->fdata));

    a->fdata[n] = a->idata[0]; // copy first
    for (uint64_t i = 1, j = 0; i < a->length; i++)  // copy first length numbers
    {
        uint64_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;

        a->fdata[n + j] = a->idata[i];
    }
    
    memset(a->fdata, 0, n * sizeof(*a->fdata));
    
    _lli_fft_core(a, LLI_FFT_FORWARD);

    a->state = LLI_STATE_F;
}


#include "stdio.h"
void _lli_algo_mul_i(struct lli * __restrict__ a, struct lli * __restrict__ b, int64_t carry)
{
    ALIGN32(a);
    ALIGN32(b);

    (void)a;
    (void)b;
    (void)carry;
    
    printf("Not implemented: Imul [in I state]\n");
    exit(1);
}

void _lli_algo_mul_f(struct lli * __restrict__ a, struct lli * __restrict__ b)
{
    ALIGN32(a);
    ALIGN32(b);

    uint64_t n = 2*a->length;
    
    double * __restrict__ aidata = a->fdata;
    double * __restrict__ ardata = a->fdata + n;
    const double * __restrict__ bidata = b->fdata;
    const double * __restrict__ brdata = b->fdata + n;
        
    #pragma clang loop vectorize(enable) interleave(enable)
    for (uint64_t i = 0; i < n; ++i)
    {
        double tmp1 = ardata[i] * brdata[i] - aidata[i] * bidata[i];
        double tmp2 = ardata[i] * bidata[i] + aidata[i] * brdata[i];
        ardata[i] = tmp1;
        aidata[i] = tmp2;
    }
}
    

void _lli_algo_add_i(struct lli * __restrict__ a, struct lli * __restrict__ b, int64_t carry)
{
    ALIGN32(a);
    ALIGN32(b);
    
    if (!carry)
    {
        #pragma clang loop vectorize(enable) interleave(enable)
        for (uint64_t i = 0; i < a->length; ++i)
        {
            a->idata[i] += b->idata[i];
        }
        return;
    }
    _lli_algo_addsub_i(+ b->idata[i])
}

void _lli_algo_add_f(struct lli * __restrict__ a, struct lli * __restrict__ b)
{
    ALIGN32(a);
    ALIGN32(b);
        
    #pragma clang loop vectorize(enable) interleave(enable)
    for (uint64_t i = 0; i < 4*a->length; ++i)
    {
        a->fdata[i] += b->fdata[i];
    }
}

void _lli_algo_sub_i(struct lli * __restrict__ a, struct lli * __restrict__ b, int64_t carry)
{
    ALIGN32(a);
    ALIGN32(b);
    
    if (!carry)
    {
        #pragma clang loop vectorize(enable) interleave(enable)
        for (uint64_t i = 0; i < a->length; ++i)
        {
            a->idata[i] -= b->idata[i];
        }
        return;
    }
    _lli_algo_addsub_i(-  b->idata[i])    
}

void _lli_algo_sub_f(struct lli * __restrict__ a, struct lli * __restrict__ b)
{
    ALIGN32(a);
    ALIGN32(b);

    #pragma clang loop vectorize(enable) interleave(enable)
    for (uint64_t i = 0; i < 4*a->length; ++i)
    {
        a->fdata[i] -= b->fdata[i];
    }
}

#endif


#endif
