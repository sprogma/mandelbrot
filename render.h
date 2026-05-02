#ifndef RENDER_H
#define RENDER_H


#include "math.h"
#include "inttypes.h"

#include "mandelbrot.h"


struct render_config
{
    double i_start_zoom;
    double i_end_zoom;
    double i_zoom_time;
    double i_zoom_ps;
    const char *output_filename;
    bool show_info;
    int64_t w, h;
    int32_t device_id;
    int64_t fps;
};


struct render;


struct render *init_render(const struct render_config *config);
void render_image(struct render *r, struct path_data *path);
void render_deinit(struct render *render);


#endif
