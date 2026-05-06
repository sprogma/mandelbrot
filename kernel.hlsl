

struct FloatExp 
{
    float m; 
    int e;

    static FloatExp create(float val, int exp) 
    {
        FloatExp res; res.m = val; res.e = exp;
        res.normalize();
        return res;
    }
    
    void normalize() 
    {
        if (m == 0.0) { e = 0; return; }
        uint bits = asuint(m);
        int real_e = (int)((bits >> 23) & 0xFFu) - 127;
        m = asfloat((bits & 0x807FFFFFu) | 0x3F800000u);
        e += real_e;
    }

    FloatExp operator*(FloatExp other) 
    {
        FloatExp res;
        res.m = this.m * other.m;
        res.e = this.e + other.e;
        // res.normalize(); // it will be normalized after addition.
        return res;
    }

    FloatExp operator+(FloatExp other) 
    {
        FloatExp res;
        int diff = e - other.e;
        if (diff >= 0) { res.m = m + ldexp(other.m, -diff); res.e = e; } 
        else { res.m = other.m + ldexp(m, diff); res.e = other.e;}
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
    
    FloatExp2 ddt;
    ddt.x = FloatExp::create(1.0, 0); 
    ddt.y = FloatExp::create(0.0, 0);
    
    float log2_pixel = log2(params.zoom_m) + float(params.zoom_e);

    int threshold_e = int(ceil(-log2_pixel)) + 9;

    for (uint i = 1; i < params.path_length; ++i) 
    {   
        float2 input_zb = pathBuffer[i];
        FloatExp2 zb;
        zb.x = FloatExp::create(input_zb.x, 0);
        zb.y = FloatExp::create(input_zb.y, 0);

        FloatExp2 z_curr;
        z_curr.x = zb.x + dt.x;
        z_curr.y = zb.y + dt.y;

        FloatExp ddt_next_x = (z_curr.x * ddt.x - z_curr.y * ddt.y);
        FloatExp ddt_next_y = (z_curr.x * ddt.y + z_curr.y * ddt.x);
        
        ddt_next_x.e += 1; 
        ddt_next_y.e += 1;
        ddt.x = ddt_next_x + FloatExp::create(1.0, 0);
        ddt.y = ddt_next_y;
        ddt.x.normalize();
        ddt.y.normalize();

        if (ddt.x.e > threshold_e || ddt.y.e > threshold_e)
        {
            data = float4(0, 0, 0, 1);
            break;
        }

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
            FloatExp Zx = zb.x + dt.x;
            FloatExp Zy = zb.y + dt.y;

            FloatExp Zx2 = Zx * Zx;
            FloatExp Zy2 = Zy * Zy;
            FloatExp magSq = Zx2 + Zy2;

            float log2_Z = 0.5 * (log2(magSq.m) + float(magSq.e));

            FloatExp dZx = ddt.x;
            FloatExp dZy = ddt.y;

            FloatExp dZx2 = dZx * dZx;
            FloatExp dZy2 = dZy * dZy;
            FloatExp dMagSq = dZx2 + dZy2;

            float log2_dZ = 0.5 * (log2(dMagSq.m) + float(dMagSq.e));

            const float LOG2_LN2 = -0.528771f;

            float safe_logZ = max(log2_Z, 1e-10f);
            float log2_lnZ = log2(safe_logZ) + LOG2_LN2;

            float log2_dist = 1.0f + log2_Z + log2_lnZ - log2_dZ;

            float alpha = clamp(exp2(log2_dist - log2_pixel), 0.0f, 1.0f);

            float abs_z_safe = exp2(log2_Z);
            float r2 = abs_z_safe * abs_z_safe;
            float tt_smooth = log2(log2(max(r2, 1.0f)) * 0.5f);
            float t_val = float(i) + 1.0f - tt_smooth;

            data.x = sin(t_val * 0.2f) * alpha;
            data.y = cos(1.0f + t_val * 0.3f) * alpha;
            break;
        }

    }

    // !!! dest image is BGR0 if render is to file, and RGBA if to screen
    
    destImage[dtid.xy] = data;
}

