
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
        if (m == 0.0) { e = -10000; return; }
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
        res.normalize();
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
#ifdef FLOAT64
    double2 relative_center64;
#endif
};

[[vk::push_constant]]
PushConstants params;

[[vk::binding(0, 0)]] 
RWTexture2D<float4> destImage;

[[vk::binding(1, 0)]] 
StructuredBuffer<float2> pathBuffer;

void DeepRender(uint2 dtid, float2 pixel_offset, float log2_pixel)
{
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

    int threshold_e = int(ceil(-log2_pixel)) + 9;

    int biased_diff_log2 = (int)floor(log2_pixel) + 12;

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

void FloatRender(uint2 dtid, float2 pixel_offset, float log2_pixel)
{
    float2 c = params.relative_center + pixel_offset * ldexp(params.zoom_m, params.zoom_e);

    float2 z = 0;
    float2 zold = 0;
    float2 dz = float2(1.0, 0.0);
    float4 data = float4(0, 0, 0, 1);
    
    float threshold_val = pow(2.0, -log2_pixel + 10);

    uint power = 8;

    for (uint i = 1; i < min(10000, max(512, params.path_length)); ++i) 
    {
        float2 next_dz;
        next_dz.x = 2.0 * (z.x * dz.x - z.y * dz.y) + 1.0;
        next_dz.y = 2.0 * (z.x * dz.y + z.y * dz.x);
        dz = next_dz;

        float2 next_z;
        next_z.x = z.x * z.x - z.y * z.y + c.x;
        next_z.y = 2.0 * z.x * z.y + c.y;
        z = next_z;

        float r2 = dot(z, z);
        float d2 = dot(dz, dz);

        if (d2 > threshold_val * threshold_val) 
        {
            break;
        }

        if (r2 > 16.0)
        {
            float log2_Z = 0.5 * log2(r2);
            float log2_dZ = 0.5 * log2(d2);
            
            const float LOG2_LN2 = -0.528771;
            float safe_logZ = max(log2_Z, 1e-10);
            float log2_lnZ = log2(safe_logZ) + LOG2_LN2;

            float log2_dist = 1.0 + log2_Z + log2_lnZ - log2_dZ;

            float alpha = clamp(exp2(log2_dist - log2_pixel), 0.0, 1.0);

            float tt_smooth = log2(log2_Z);
            float t_val = float(i) + 1.0 - tt_smooth;

            data.x = sin(t_val * 0.2) * alpha;
            data.y = cos(1.0 + t_val * 0.3) * alpha;
            data.z = 0.0;
            data.w = 1.0;
            break;
        }

        float2 diff = z - zold;
        if (dot(diff, diff) < 5e-7) 
        { 
            break;
        }
        
        if (i == power) 
        {
            zold = z;
            power *= 2;
        }
    }

    destImage[dtid.xy] = data;
}

#ifdef FLOAT64

#define PI 3.14159265358979323846L
#define TAU 6.28318530717958647692L

double sin_db(double x) 
{
    x = x - TAU * (double)((int)((x + PI) / TAU)); 
    double x2 = x * x;
    return x * (1.0L + x2 * (-1.66666666666666666667e-1L + x2 * (8.33333333333333333333e-3L + x2 * 
               (-1.98412698412698412698e-4L + x2 * (2.75573192239858906526e-6L + x2 * -2.50521083854417187751e-8L)))));
}

double cos_db(double x) 
{
    x = x - TAU * (double)((int)((x + PI) / TAU)); 
    double x2 = x * x;
    return 1.0L + x2 * (-5.0e-1L + x2 * (4.16666666666666666667e-2L + x2 * (-1.38888888888888888889e-3L + x2 * 
           (2.48015873015873015873e-5L + x2 * (-2.73015873015873015873e-7L + x2 * 2.08767569878680989792e-9L)))));
}

double exp2_db(double x) 
{
    int i = (int)x;
    if (x < (double)i) i -= 1;
    double f = x - (double)i;

    double res = 1.0L + f * (0.6931471805599453L + f * (0.2402265069591007L + f * (0.05550410866482158L + f * 
                 (0.009618129107628477L + f * (0.001333355814642844L + f * 0.0001540353039338161L)))));
    
    double m = 1.0L;
    double base = (i < 0) ? 0.5L : 2.0L;
    int abs_i = abs(i);
    while (abs_i > 0) 
    {
        if (abs_i % 2 != 0) m *= base;
        base *= base;
        abs_i /= 2;
    }
    return res * m;
}

double log2_db(double x) 
{
    if (x <= 0.0L) return -1.0L / 0.0L;
    double y = (double)log2((float)x);
    for(int i = 0; i < 2; i++) {
        double loexp = exp2_db(y);
        y += 2.0L * (x - loexp) / (x + loexp) / 0.693147180559945309417L;
    }
    return y;
}


void DoubleRender(uint2 dtid, float2 pixel_offset, double log2_pixel)
{
    double2 c = params.relative_center64 + double2(pixel_offset) * ((double)params.zoom_m * exp2(params.zoom_e));

    double2 z = 0;
    double2 zold = 0;
    double2 dz = double2(1.0, 0.0);
    float4 data = float4(0, 0, 0, 1);
    
    double threshold_val = exp2_db(-log2_pixel + 10.0L);

    uint power = 8;

    for (uint i = 1; i < min(10000, max(512, params.path_length)); ++i) 
    {
        double2 next_dz;
        next_dz.x = 2.0 * (z.x * dz.x - z.y * dz.y) + 1.0;
        next_dz.y = 2.0 * (z.x * dz.y + z.y * dz.x);
        dz = next_dz;

        double2 next_z;
        next_z.x = z.x * z.x - z.y * z.y + c.x;
        next_z.y = 2.0 * z.x * z.y + c.y;
        z = next_z;

        double r2 = dot(z, z);
        double d2 = dot(dz, dz);

        if (d2 > threshold_val * threshold_val) 
        {
            break;
        }

        if (r2 > 16.0)
        {
            double log2_Z = 0.5 * log2_db(r2);
            double log2_dZ = 0.5 * log2_db(d2);
            
            const double LOG2_LN2 = -0.528771;
            double safe_logZ = max(log2_Z, 1e-10);
            double log2_lnZ = log2_db(safe_logZ) + LOG2_LN2;

            double log2_dist = 1.0 + log2_Z + log2_lnZ - log2_dZ;

            double alpha = clamp(exp2_db(log2_dist - log2_pixel), 0.0, 1.0);

            double tt_smooth = log2_db(log2_Z);
            double t_val = double(i) + 1.0 - tt_smooth;

            data.x = (float)(sin_db(t_val * 0.2) * alpha);
            data.y = (float)(cos_db(1.0 + t_val * 0.3) * alpha);
            data.z = 1.0;
            data.w = 1.0;
            break;
        }

        double2 diff = z - zold;
        if (dot(diff, diff) < 5e-7) 
        { 
            break;
        }
        
        if (i == power) 
        {
            zold = z;
            power *= 2;
        }
    }

    destImage[dtid.xy] = data;
}
#endif

[numthreads(WORK_GROUP_SIZE_X, WORK_GROUP_SIZE_Y, 1)]
void main(uint3 dtid : SV_DispatchThreadID) 
{
    uint2 size;
    destImage.GetDimensions(size.x, size.y);
        
    if (any(dtid.xy > size))
    {
        return;
    }
    
    float log2_pixel = log2(params.zoom_m) + float(params.zoom_e);
    float2 pixel_offset = float2((int2)(dtid.xy - size / 2));

    // early exit
    if (log2_pixel > -24.0)
    {
        float2 xy = params.relative_center + pixel_offset * ldexp(params.zoom_m, params.zoom_e);
        
        float x_minus_025 = xy.x - 0.25;
        float q = x_minus_025 * x_minus_025 + xy.y * xy.y;
        bool in_cardioid = q * (q + x_minus_025) < 0.25 * xy.y * xy.y;

        float x_plus_1 = xy.x + 1.0;
        bool in_circle = x_plus_1 * x_plus_1 + xy.y * xy.y < 0.0625;

        if (in_cardioid || in_circle)
        {
            destImage[dtid.xy] = float4(0, 0, 0, 1);
            return;
        }
    }

    if (log2_pixel > -21.0)
    {
        // pixel size fits into float. Use it to speed up compuctations.
        FloatRender(dtid.xy, pixel_offset, log2_pixel);
    }
#ifdef FLOAT64
    else if (log2_pixel > -40.0)
    {
        // pixel size fits into float. Use it to speed up compuctations.
        DoubleRender(dtid.xy, pixel_offset, log2_pixel);
    }
#endif
    else
    {
        DeepRender(dtid.xy, pixel_offset, log2_pixel);
    }
}

