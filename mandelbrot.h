#ifndef MANDELBROT_H
#define MANDELBROT_H



#include "inttypes.h"



struct path_data
{
    int64_t points_count;
    int64_t path_length;
    float (*data)[2];

    int64_t current_image;
    int64_t total_images;
    
    float start_zoom;
    float current_zoom;
    float zoom_step;
};


int calculate_path(struct path_data *data);

#endif
