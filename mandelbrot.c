#include "stdlib.h"
#include "stdio.h"

#include "render.h"
#include "mandelbrot.h"


int calculate_path(struct path_data *data)
{
    /* select points count based on scale */
    data->points_count = 1;
    /* select path_length based on scale */
    data->path_length = 1024;
    
    float (*buffer)[2] = data->data;
    for (int64_t p = 0; p < data->points_count; ++p, buffer += data->path_length)
    {
        for (int64_t i = 0; i < data->path_length; ++i)
        {
            buffer[i][0] = (float)rand() / RAND_MAX;
            buffer[i][1] = (float)rand() / RAND_MAX;
        }
    }
    return data->current_image++ < data->total_images;
}
