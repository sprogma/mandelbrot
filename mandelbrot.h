#ifndef MANDELBROT_H
#define MANDELBROT_H



#include "inttypes.h"
#include "common_defines.h"
#include "lli.h"



struct path_data
{
    int64_t item_bits;
    int64_t bits;
    int64_t bit_exp;
    
    int64_t current_depth;
    
    int64_t points_count;
    int64_t path_length;
    float (*data)[2];

    double time;
    double zoom_step;

    int64_t calculated_depth[MAX_POINTS_COUNT];
    struct lli *calculated_depth_values[MAX_POINTS_COUNT][2];

    int64_t current_image;
    int64_t total_images;
    
    struct lli *center[2];
    struct lli *zoom;
    
    struct lli *tmp[8];

    int64_t bbits;
    int64_t bbit_exp;
    struct llf bAn_re, bAn_im;
    struct llf bBn_re, bBn_im;
    int64_t skip_steps;
    struct llf btmp[8];
};

int init_path(struct path_data *data, double start_zoom, double start_x, double start_y);
int update_zoom(struct path_data *data, double dzoom, double dx, double dy);
int calculate_path(struct path_data *data, float *out_zoom_m, int *out_zoom_e, float *out_center_x, float *out_center_y);
void optimize_depth(struct lli *x, struct lli *y, int64_t stime, bool ignore_starting_point);

#endif
