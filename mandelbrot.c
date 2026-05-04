#define LLI_SOURCE

#include "stdlib.h"
#include "time.h"
#include "math.h"
#include "stdio.h"

#include "render.h"
#include "mandelbrot.h"
#include "common_defines.h"


#include "lli.h"


int init_path(struct path_data *data, double start_zoom, double start_x, double start_y)
{
    data->item_bits = ITEM_BITS; // 1 multiplication - ok
    data->bits = BITS;
    data->bit_exp = BITS_EXP;

    data->bbits = BBITS;
    data->bbit_exp = BBITS_EXP;

    data->zoom = lli_create(data->bits, data->item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);
    data->center[0] = lli_create(data->bits, data->item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);
    data->center[1] = lli_create(data->bits, data->item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);

    data->bAn_re.val = lli_create(data->bbits, data->item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);
    data->bAn_im.val = lli_create(data->bbits, data->item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);
    data->bBn_re.val = lli_create(data->bbits, data->item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);
    data->bBn_im.val = lli_create(data->bbits, data->item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);

    for (int i = 0; i < 8; ++i) data->btmp[i].val = lli_create(data->bbits, data->item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);

    for (int i = 0; i < 8; ++i) data->tmp[i] = lli_create(data->bits, data->item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);
    for (int i = 0; i < MAX_POINTS_COUNT; ++i) data->calculated_depth_values[i][0] = lli_create(data->bits, data->item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);
    for (int i = 0; i < MAX_POINTS_COUNT; ++i) data->calculated_depth_values[i][1] = lli_create(data->bits, data->item_bits, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);

    lli_load_double(data->zoom, start_zoom, data->bit_exp);
    lli_load_double(data->center[0], start_x, data->bit_exp);
    lli_load_double(data->center[1], start_y, data->bit_exp);

    memset(data->calculated_depth, 0, sizeof(data->calculated_depth));

    return 0;
}


int update_zoom(struct path_data *data, double dzoom, double dx, double dy)
{
    if (dx != 0.0 || dy != 0.0)
    {
        memset(data->calculated_depth, 0, sizeof(data->calculated_depth));
    }

    lli_load_double(data->tmp[0], dzoom, data->bit_exp);
    lli_load_double(data->tmp[1], dx, data->bit_exp);
    lli_load_double(data->tmp[2], dy, data->bit_exp);

    // data->zoom *= dzoom;
    // data->center[0] += data->zoom * dx;
    // data->center[1] += data->zoom * dy;

    lli_mul(data->zoom, data->tmp[0], 0);
    lli_adam(data->zoom, data->bit_exp);

    lli_mul(data->tmp[1], data->zoom, 0);
    lli_adam(data->tmp[1], data->bit_exp);

    lli_mul(data->tmp[2], data->zoom, 0);
    lli_adam(data->tmp[2], data->bit_exp);

    lli_add(data->center[0], data->tmp[1], 0);
    lli_add(data->center[1], data->tmp[2], 0);

    return 0;
}


void llf_add(struct llf *res, struct llf *a, struct llf *b, struct path_data *data)
{
    int64_t diff = a->exp - b->exp;
    if (diff > data->bbits) { lli_copy(res->val, a->val); res->exp = a->exp; return; }
    if (diff < -data->bbits) { lli_copy(res->val, b->val); res->exp = b->exp; return; }

    if (diff >= 0)
    {
        lli_copy(data->btmp[0].val, b->val);
        lli_adam(data->btmp[0].val, data->bbit_exp + diff);
        lli_copy(res->val, a->val);
        lli_add(res->val, data->btmp[0].val, 0);
        res->exp = a->exp;
    }
    else
    {
        lli_copy(data->btmp[0].val, a->val);
        lli_adam(data->btmp[0].val, data->bbit_exp - diff);
        lli_copy(res->val, b->val);
        lli_add(res->val, data->btmp[0].val, 0);
        res->exp = b->exp;
    }
}

void llf_mul(struct llf *res, struct llf *a, struct llf *b, struct path_data *data)
{
    lli_copy(res->val, a->val);
    lli_mul(res->val, b->val, 0);
    lli_adam(res->val, data->bbit_exp);
    res->exp = a->exp + b->exp;
}

void llf_mul_double(struct llf *res, struct llf *a, double val, struct path_data *data)
{
    int e;
    double m = frexp(val, &e);
    lli_copy(res->val, a->val);
    lli_load_double(data->btmp[0].val, m, data->bbit_exp);
    lli_mul(res->val, data->btmp[0].val, 0);
    lli_adam(res->val, data->bbit_exp);
    res->exp = a->exp + (e - 1);
}






int calculate_path(struct path_data *data, float *out_zoom_m, int *out_zoom_e, float *out_center_x, float *out_center_y)
{
    /* select points count based on scale */
    data->points_count = 1;
    /* select path_length based on scale */
    data->path_length = 1024;

    data->time = (double)data->current_image / data->total_images;


    int64_t zoom_e;
    double zoom_m;

    lli_as_double2(data->zoom, data->bit_exp, &zoom_m, &zoom_e);

    data->points_count = 1;
    data->path_length = 10 + 70 * llabs(zoom_e);
    static bool first_time = true;
    if (data->path_length > MAX_PATH_LENGTH)
    {
        if (first_time)
        {
            first_time = false;
            printf("waring: MAX_PATH_LENGTH REACHED\n");
        }
        data->path_length = MAX_PATH_LENGTH;
    }
    data->path_length = MAX_PATH_LENGTH;

    /* take center, and calculate it's position */

    float (*buffer)[2] = data->data;
    for (int64_t p = 0; p < data->points_count; ++p, buffer += data->path_length)
    {
        // long double pr
        // long double pi
        lli_copy(data->tmp[0], data->center[0]);
        lli_copy(data->tmp[1], data->center[1]);
        // zr, zi
        if (data->calculated_depth[p] == 0)
        {
            lli_zero(data->tmp[2]);
            lli_zero(data->tmp[3]);
        }
        else
        {
            lli_copy(data->tmp[2], data->calculated_depth_values[p][0]);
            lli_copy(data->tmp[3], data->calculated_depth_values[p][1]);
        }

        // store first values
        buffer[0][0] = 0.0;//lli_as_double(data->tmp[0], data->bit_exp);
        buffer[0][1] = 0.0;//lli_as_double(data->tmp[1], data->bit_exp); // [0] = center

        // printf("start: %f %f\n", buffer[0][0], buffer[0][1]);
        for (int64_t i = data->calculated_depth[p]; i < data->path_length; ++i)
        {
            // printf("%lld/%lld\n", i, data->path_length);
            buffer[i+1][0] = lli_as_double(data->tmp[2], data->bit_exp);
            buffer[i+1][1] = lli_as_double(data->tmp[3], data->bit_exp); // [1..] = path

            if (fabs(buffer[i+1][0]) > 1000.0 || fabs(buffer[i+1][1]) > 1000.0)
            {
                data->current_depth = i;
                break;
            }

            // printf("%f %f\n", buffer[i][0], buffer[i][1]);

            // long double nzi = zr * zi * 2.0 + pi;
            // long double nzr = (zr * zr) - (zi * zi) + pr;

            lli_copy(data->tmp[4], data->tmp[2]);    // = zr
            lli_mul(data->tmp[4], data->tmp[3], 0);  // = zr * zi
            lli_adam(data->tmp[4], data->bit_exp);

            lli_copy(data->tmp[5], data->tmp[2]);    // = zr
            lli_mul(data->tmp[2], data->tmp[5], 0);  // zr = zr * zr
            lli_adam(data->tmp[2], data->bit_exp);
            lli_copy(data->tmp[5], data->tmp[3]);    // = zi
            lli_mul(data->tmp[5], data->tmp[3], 0);  // = zi * zi
            lli_adam(data->tmp[5], data->bit_exp);
            lli_neg(data->tmp[5], 0);                // = - zi * zi
            lli_add(data->tmp[2], data->tmp[5], 0);  // zr = zr * zr - zi * zi

            lli_copy(data->tmp[3], data->tmp[4]);    // zi = zr * zi
            lli_add(data->tmp[3], data->tmp[4], 0);  // zi = zr * zi * 2.0

            lli_add(data->tmp[2], data->tmp[0], 0);  // zr += pr
            lli_add(data->tmp[3], data->tmp[1], 0);  // zi += pi
        }

        // calculate An Bn based on buffer

        data->calculated_depth[p] = data->path_length;
        lli_copy(data->calculated_depth_values[p][0], data->tmp[2]);
        lli_copy(data->calculated_depth_values[p][1], data->tmp[3]);
    }

    /* calculate zoom to be near 1.0 */


    *out_zoom_m = zoom_m;
    *out_zoom_e = zoom_e;
    *out_center_x = 0.0;//lli_as_double(data->center[0], data->bit_exp);
    *out_center_y = 0.0;//lli_as_double(data->center[1], data->bit_exp);

    return data->current_image++ < data->total_images;
}




int64_t get_depth(struct lli *x, struct lli *y, struct lli *tmp[8], int64_t bit_exp)
{
    lli_copy(tmp[0], x);
    lli_copy(tmp[1], y);
    // zr, zi
    lli_zero(tmp[2]);
    lli_zero(tmp[3]);

    int64_t i = 0;
    for (; i < MAX_PATH_LENGTH; ++i)
    {
        // printf("%lld/%lld\n", i, data->path_length);
        double dx, dy;
        int64_t ex, ey;
        lli_as_double2(tmp[2], bit_exp, &dx, &ex);
        lli_as_double2(tmp[3], bit_exp, &dy, &ey); // [1..] = path

        if (ex > 4 || ey > 4)
        {
            return i + 1;
        }

        // printf("%f %f\n", buffer[i][0], buffer[i][1]);

        // long double nzi = zr * zi * 2.0 + pi;
        // long double nzr = (zr * zr) - (zi * zi) + pr;

        lli_copy(tmp[4], tmp[2]);    // = zr
        lli_mul(tmp[4], tmp[3], 0);  // = zr * zi
        lli_adam(tmp[4], bit_exp);

        lli_copy(tmp[5], tmp[2]);    // = zr
        lli_mul(tmp[2], tmp[5], 0);  // zr = zr * zr
        lli_adam(tmp[2], bit_exp);
        lli_copy(tmp[5], tmp[3]);    // = zi
        lli_mul(tmp[5], tmp[3], 0);  // = zi * zi
        lli_adam(tmp[5], bit_exp);
        lli_neg(tmp[5], 0);          // = - zi * zi
        lli_add(tmp[2], tmp[5], 0);  // zr = zr * zr - zi * zi

        lli_copy(tmp[3], tmp[4]);    // zi = zr * zi
        lli_add(tmp[3], tmp[4], 0);  // zi = zr * zi * 2.0

        lli_add(tmp[2], tmp[0], 0);  // zr += pr
        lli_add(tmp[3], tmp[1], 0);  // zi += pi
    }
    return 0;
}

#include "windows.h"

struct searcher_info
{
    SRWLOCK lock;
    volatile _Atomic int64_t tryes;
    volatile int64_t best;
    volatile int64_t search;
    volatile struct lli *best_x;
    volatile struct lli *best_y;
    volatile struct lli *best_z;
    volatile bool ignore_starting_point;
    volatile double zf;
    volatile int64_t ze;
};

#define NNN 10

DWORD depth_searcher(void *params)
{
    struct searcher_info *info = params;

    struct lli *x, *y, *z;
    struct lli *tmp[8];

    x = lli_create(BITS, ITEM_BITS, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);
    y = lli_create(BITS, ITEM_BITS, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);
    for (int i = 0; i < 8; ++i) tmp[i] = lli_create(BITS, ITEM_BITS, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);

    z = lli_create(BITS, ITEM_BITS, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0);
    lli_load_double(z, 0.5, BITS_EXP);

    if (!info->ignore_starting_point)
    {
        AcquireSRWLockExclusive(&info->lock);
        
        lli_copy(x, (void *)info->best_x);
        lli_copy(y, (void *)info->best_y);

        lli_load_double(z, 0.01, BITS_EXP);
            
        ReleaseSRWLockExclusive(&info->lock);
    }
    else
    {
        double dx = 6.0 * ((double)rand() / RAND_MAX * 2.0 - 1.0);
        double dy = 6.0 * ((double)rand() / RAND_MAX * 2.0 - 1.0);
        lli_load_double(x, dx, BITS_EXP);
        lli_load_double(y, dy, BITS_EXP);
    }

    int64_t cnt = 0, bbest, tt = 1;
    while (info->search)
    {
        int64_t res = get_depth(x, y, tmp, BITS_EXP);
        
        cnt++;
        
        AcquireSRWLockExclusive(&info->lock);
        bbest = info->best;
        info->tryes ++;
        // printf("FOUND %lld vs %lld at zoom %g\n", res, info->best, lli_as_double(z, BITS_EXP));
        if (info->best < res)
        {
            tt = 1;
            info->best = res;
            lli_copy((void *)info->best_x, x);
            lli_copy((void *)info->best_y, y);
            lli_copy((void *)info->best_z, z);
            lli_as_double2(z, BITS_EXP, (void *)&info->zf, (void *)&info->ze);
        }
        else if (cnt % 30 == 0)
        {
            tt++;
            if (info->best < NNN)
            {
                lli_copy(x, (void *)info->best_x);
                lli_copy(y, (void *)info->best_y);

                if (!info->ignore_starting_point)
                {
                    lli_load_double(z, 0.05, BITS_EXP);
                }
                else
                {
                    lli_load_double(z, 1.0, BITS_EXP);
                }
            }
            else
            {
                lli_copy(x, (void *)info->best_x);
                lli_copy(y, (void *)info->best_y);
                lli_copy(z, (void *)info->best_z);


                if (tt < 20)
                {
                    lli_load_double(tmp[0], 0.5 / tt / tt, BITS_EXP);
                    lli_mul(z, tmp[0], 0);
                    lli_adam(z, BITS_EXP);
                }
                else if (tt < 50)
                {
                    lli_load_double(tmp[0], 0.5 * (tt - 19) * (tt - 19), BITS_EXP);
                    lli_mul(z, tmp[0], 0);
                    lli_adam(z, BITS_EXP);
                }
                else
                {
                    lli_load_double(tmp[0], 0.5 * (tt - 40) * (tt - 40) * (tt - 40), BITS_EXP);
                    lli_mul(z, tmp[0], 0);
                    lli_adam(z, BITS_EXP);
                    if (tt > 200)
                    {
                        tt = 0;
                    }
                }

                if (rand() % 10 == 0)
                {
                    lli_load_double(tmp[0], 0.00001, BITS_EXP);
                    lli_add(z, tmp[0], 0);
                }
            }
        }
        ReleaseSRWLockExclusive(&info->lock);

        /* adjust zoom */
        lli_load_double(tmp[0], sqrt((double)rand() / RAND_MAX) * 0.3 + 0.7, BITS_EXP);
        lli_mul(z, tmp[0], 0);
        lli_adam(z, BITS_EXP);

        /* move x and y */
        if (bbest < NNN && info->ignore_starting_point) // search for random good point
        {
            double dx = 6.0 * ((double)rand() / RAND_MAX * 2.0 - 1.0);
            double dy = 6.0 * ((double)rand() / RAND_MAX * 2.0 - 1.0);
            lli_load_double(x, dx, BITS_EXP);
            lli_load_double(y, dy, BITS_EXP);
        }
        else
        {
            /* try to load random number? */
            double dx = ((double)rand() / RAND_MAX * 2.0 - 1.0);
            double dy = ((double)rand() / RAND_MAX * 2.0 - 1.0);
            lli_load_double(tmp[0], dx, BITS_EXP);
            lli_load_double(tmp[1], dy, BITS_EXP);
            lli_mul(tmp[0], z, 0);
            lli_mul(tmp[1], z, 0);
            lli_adam(tmp[0], BITS_EXP);
            lli_adam(tmp[1], BITS_EXP);
            lli_add(x, tmp[0], 0);
            lli_add(y, tmp[1], 0);
        }
    }

    lli_free(x);
    lli_free(y);
    lli_free(z);
    for (int64_t i = 0; i < 8; ++i) lli_free(tmp[i]);

    return 0;
}


void optimize_depth(struct lli *x, struct lli *y, int64_t stime, bool ignore_starting_point)
{
    int workers = 8;
    srand(time(NULL));
    
    struct searcher_info info = {
        .lock = SRWLOCK_INIT,
        .tryes = 0,
        .best = 0,
        .search = 1,
        .best_x = x,
        .best_y = y,
        .best_z = lli_create(BITS, ITEM_BITS, LLI_CREATE_OPTIMIZE_MULTIPLICATION, 0),
        .ignore_starting_point = ignore_starting_point,
        .zf = 0.0,
        .ze = 0,
    };
    
    DWORD threadId[64];

    HANDLE hThread[64];
    for (int i = 0; i < workers; ++i)
    {
        hThread[i] = CreateThread(NULL, 0, depth_searcher, &info, 0, threadId+i);
    }

    printf("Waiting...\n");
    int64_t t = time(NULL);
    while (time(NULL) - t < stime)
    {
        printf("Found:  depth=%10lld = %7.1f%% | checks=%10lld | zoom ~= 2 ^%10.2f | %lld s. left\n", info.best, info.best * 100.0 / MAX_PATH_LENGTH, info.tryes, -(log2(0.1 + info.zf) + info.ze), stime - (time(NULL) - t));
        Sleep(500);
    }

    printf("Ending...\n");
    info.search = 0;
    printf("Result: depth=%10lld = %7.1f%% | checks=%10lld | zoom ~= 2 ^%10.2f\n", info.best, info.best * 100.0 / MAX_PATH_LENGTH, info.tryes, -(log2(0.1 + info.zf) + info.ze));

    for (int i = 0; i < workers; ++i)
    {
        WaitForSingleObject(hThread[i], INFINITE);
        CloseHandle(hThread);
    }
}





