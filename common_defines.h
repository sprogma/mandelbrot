#define ITEM_BITS 10

// for mandelbrot number
#define BITS_EXP 16384
#define BITS (BITS_EXP*2)


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
