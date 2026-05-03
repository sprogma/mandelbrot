#include "stdio.h"
#include "stdlib.h"
#include "ctype.h"
#include "string.h"
#include "immintrin.h"
#include "math.h"
#include "errno.h"
#include "inttypes.h"


#include "mandelbrot.h"
#include "render.h"


double normal_strtod(const char *str, const char *flag_name)
{
    char *endptr;
    errno = 0; 
    
    double value = strtod(str, &endptr);
    
    if (str == endptr) 
    {
        printf("flag %s needs number after it.\n", flag_name);
        exit(1);
    }
    else if (errno == ERANGE) 
    {
        printf("flag %s have got too big number, it doesn't fits into double. Are you sure this is normal power of 10 or time in seconds?\n", flag_name);
        exit(1);
    }
    else 
    {
        while (isspace(*endptr)) {
            endptr++;
        }
        if (*endptr != '\0') 
        {
            printf("Flag %s contains garbage after number: what is is?? <%s>.\n", flag_name, endptr);
            exit(1);
        }
        
        return value;
    }
}

int64_t normal_strtoll(const char *str, const char *flag_name)
{
    char *endptr;
    errno = 0; 
    
    int64_t value = strtoll(str, &endptr, 10);
    
    if (str == endptr) 
    {
        printf("flag %s needs integer number after it.\n", flag_name);
        exit(1);
    }
    else if (errno == ERANGE) 
    {
        printf("flag %s have got too big number, it doesn't fits into int64_t. Are you sure this is normal resolution/device id?\n", flag_name);
        exit(1);
    }
    else 
    {
        while (isspace(*endptr)) {
            endptr++;
        }
        if (*endptr != '\0') 
        {
            printf("Flag %s contains garbage after number: what is is?? <%s>.\n", flag_name, endptr);
            exit(1);
        }
        
        return value;
    }
}

int main(int argc, const char **argv)
{
    struct render_config config = {
        .i_start_zoom = 0.0,
        .i_end_zoom = NAN,
        .i_zoom_time = NAN,
        .i_zoom_ps = 0.1,
        .output_filename = NULL,
        .show_info = true,
        .use_float64 = false,
        .use_accelerated_encoding = false,
        .w = 1920,
        .h = 1080,
        .device_id = -1,
        .fps = 60,
    };
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-?") == 0 || strcmp(argv[i], "-h") == 0)
        {
            printf(R"DOC(
------ mandelbrot ------

use -o filename to write video to file
use -s X to select intial zoom [power of 10]
use -e X to select destination zoov [power of 10]
use -t X to select zooming time [seconds]
use -z X to select zooming per second [power of 10]
use -l   to enable showing/logging information
use -r X Y to select resolution in pixels.
use -d X to select device by d.
use -f64 to enable float64.
use -ae to enable accelerated video encoding (if supported by device).

if you used -e:
    you can't use both -t and -z
if you don't used -e:
    if you don't used -t, zooming will be infinite.
    
if you don't used -s, initial zoom will be 1.0 [equal to -s 0.0]

default resolution is 1920/1080

default device is any discrete gpu if it exists, else one of other.

examples:

brainrot.exe # simple start
brainrot.exe -l # to see fps/current zoom/other information

)DOC");
            return 0;
        }
        else if (strcmp(argv[i], "-o") == 0)
        {
            if (i + 1 >= argc) { printf("-o parameter needs filename after it.\n"); return 1; }
            config.output_filename = argv[++i];
        }
        else if (strcmp(argv[i], "-s") == 0)
        {
            if (i + 1 >= argc) { printf("-s parameter needs number after it.\n"); return 1; }
            config.i_start_zoom = normal_strtod(argv[++i], "-s");
        }
        else if (strcmp(argv[i], "-e") == 0)
        {
            if (i + 1 >= argc) { printf("-e parameter needs number after it.\n"); return 1; }
            config.i_end_zoom = normal_strtod(argv[++i], "-e");
        }
        else if (strcmp(argv[i], "-z") == 0)
        {
            if (i + 1 >= argc) { printf("-z parameter needs number after it.\n"); return 1; }
            config.i_zoom_ps = normal_strtod(argv[++i], "-z");
        }
        else if (strcmp(argv[i], "-t") == 0)
        {
            if (i + 1 >= argc) { printf("-t parameter needs number after it.\n"); return 1; }
            config.i_zoom_time = normal_strtod(argv[++i], "-t");
        }
        else if (strcmp(argv[i], "-r") == 0)
        {
            if (i + 2 >= argc) { printf("-r parameter needs two number after it.\n"); return 1; }
            config.w = normal_strtod(argv[++i], "-r");
            config.h = normal_strtod(argv[++i], "-r");
        }
        else if (strcmp(argv[i], "-d") == 0)
        {
            if (i + 1 >= argc) { printf("-d parameter needs number after it.\n"); return 1; }
            config.device_id = normal_strtod(argv[++i], "-d");
        }
        else if (strcmp(argv[i], "-l") == 0)
        {
            config.show_info = true;
        }
        else if (strcmp(argv[i], "-f64") == 0)
        {
            config.use_float64 = true;
        }
        else if (strcmp(argv[i], "-ae") == 0)
        {
            config.use_accelerated_encoding = true;
        }
    }

    struct path_data data = {};
    data.total_images = 60 * config.i_zoom_time;
    data.zoom_step = 0.99;

    /*
        interesting points
        
        -0.743643887037158
        0.131825904206411


        -1.7687788
        0.0017389
    */

    init_path(&data, 1.0, -0.743643887037158, 0.131825904206411);

    optimize_depth(data.center[0], data.center[1], 60, true);
    
    auto render = init_render(&config);
    render_image(render, &data);
    render_deinit(render);

    return 0;
}
