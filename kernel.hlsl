
#include "common_defines.h"

struct PushConstants 
{
    float _1_zoom;
    float time;
    uint anchor_points;
    uint path_length;
    float2 center;
};

[[vk::push_constant]]
PushConstants params;

[[vk::binding(0, 0)]] 
RWTexture2D<float4> destImage;

[[vk::binding(1, 0)]] 
StructuredBuffer<float2> pathBuffer;

[numthreads(WORK_GROUP_SIZE_X, WORK_GROUP_SIZE_Y, 1)]
void main(uint3 dtid : SV_DispatchThreadID) 
{
    uint2 size;
    destImage.GetDimensions(size.x, size.y);
        
    if (any(dtid.xy > size))
    {
        return;
    }

    
    float2 position = params._1_zoom * float2(dtid.xy) + params.center;
    
    float4 data = float4(0,0,0,1);
    float min_dist = 1e30;
    for (uint i = 0; i < params.anchor_points; ++i)
    {
        float2 d = pathBuffer[i * params.path_length] - position;
        float l = dot(d, d);
        if (min_dist > l)
        {
            min_dist = l;
            data = float4(sin(i + params.time + position.y), 
                          cos(i - params.time - position.x), 
                          sin(i+1.0+params.time) - cos(i-1.0+params.time), 
                          1.0) * 0.5 + 0.5;
        }
    }
    destImage[dtid.xy] = data;
}

