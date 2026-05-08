#define ITEM_BITS 10 // since we always convert to i state for adam

// for mandelbrot number
#define BITS_EXP (BITS-64)
#define BITS (4*1024)


// for taylor series
#define BBITS_EXP 128
#define BBITS (1024*2)

#define WORK_GROUP_SIZE_X 8
#define WORK_GROUP_SIZE_Y 8

#define MAX_POINTS_COUNT 1
// #define MAX_PATH_LENGTH (1024*32)
#define MAX_PATH_LENGTH (1024*16)

// not used
// base: 4.0
// quality: 10000.0
#define DROP_TRESHHOLD 1000.0
