struct FloatExp 
{
    float m; 
    int e;

    void normalize() 
    {
        if (m == 0.0) { e = 0; return; }
        uint bits = asuint(m);
        int real_e = (int)((bits >> 23) & 0xFF) - 127;
        e += real_e;
        bits = (bits & 0x807FFFFFu) | (127u << 23);
        m = asfloat(bits);
    }

    static FloatExp create(float val, int exp) 
    {
        FloatExp res; res.m = val; res.e = exp;
        res.normalize();
        return res;
    }

    FloatExp operator*(FloatExp other) 
    {
        FloatExp res;
        res.m = this.m * other.m;
        res.e = this.e + other.e;
        res.normalize();
        return res;
    }

    FloatExp operator+(FloatExp other) 
    {
        if (abs(m) < 1e-10) return other;
        if (abs(other.m) < 1e-10) return this;
        FloatExp res;
        int diff = e - other.e;
        if (diff > 25) return this;
        if (diff < -25) return other;
        if (diff >= 0) { res.m = m + other.m * pow(2.0, -diff); res.e = e; }
        else { res.m = other.m + m * pow(2.0, diff); res.e = other.e; }
        res.normalize();
        return res;
    }

    FloatExp operator-(FloatExp other) 
    {
        FloatExp neg_other = other;
        neg_other.m = -other.m;
        return this.operator+(neg_other);
    }
};

struct FloatExp2 { 
    FloatExp x, y; 

    FloatExp2 operator+(FloatExp2 other) 
    {
        FloatExp2 res = { x + other.x, y + other.y };
        return res;
    }
};





#include "common_defines.h"

struct PushConstants 
{
    float zoom_m;
    int zoom_e;
    
    float time;
    uint anchor_points;
    uint path_length;
    float2 relative_center;
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

    float2 pixel_offset = float2((int2)(dtid.xy - size / 2));
    float4 data = float4(0, 0, 0, 1);

    FloatExp2 dc;
    dc.x = FloatExp::create(pixel_offset.x * params.zoom_m, params.zoom_e);
    dc.y = FloatExp::create(pixel_offset.y * params.zoom_m, params.zoom_e);

    FloatExp2 dt;
    dt.x = FloatExp::create(0, 0);
    dt.y = FloatExp::create(0, 0);
    
    for (uint i = 1; i < params.path_length; ++i) 
    {   
        float2 input_zb = pathBuffer[i];

        FloatExp2 zb;
        zb.x = FloatExp::create(input_zb.x, 0);
        zb.y = FloatExp::create(input_zb.y, 0);

        FloatExp dtx2 = dt.x * dt.x;
        FloatExp dty2 = dt.y * dt.y;
        FloatExp dtxdy = dt.x * dt.y;

        FloatExp t = (zb.x * dt.x) - (zb.y * dt.y);
        FloatExp next_dt_x = t + t + (dtx2 - dty2) + dc.x;
        FloatExp tt = (zb.x * dt.y) + (zb.y * dt.x) + (dtxdy);
        FloatExp next_dt_y = tt + tt + dc.y;

        dt.x = next_dt_x;
        dt.y = next_dt_y;

        if (dt.x.e > 2 || dt.y.e > 2) 
        {
            float re = input_zb.x + (dt.x.m * pow(2.0, dt.x.e));
            float im = input_zb.y + (dt.y.m * pow(2.0, dt.y.e));
            float d = re*re + im*im;

            float tt = log2(log2(max(d, 1.0)) * 0.5); 
            float t = float(i) + 1.0 - tt;
            data.x = sin(t * 0.2);
            data.y = cos(1.0 + t * 0.3);
            break;
        }
    }


    // !!! dest image is BGR0 if render is to file, and RGBA if to screen
    
    destImage[dtid.xy] = data;
}

