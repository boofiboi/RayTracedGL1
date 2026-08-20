// Copyright (c) 2021 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef UTILS_H_
#define UTILS_H_



#define M_PI        3.14159265358979323846
#define UINT32_MAX  0xFFFFFFFF
#define UINT16_MAX  65535
#define UINT8_MAX   255



vec4 unpackLittleEndianUintColor(uint c)
{
    return vec4(
         (c & 0x000000FF)        / 255.0,
        ((c & 0x0000FF00) >> 8)  / 255.0,
        ((c & 0x00FF0000) >> 16) / 255.0,
        ((c & 0xFF000000) >> 24) / 255.0
    );
}

uint packLittleEndianUintColor(const vec4 c)
{
    return
        (uint(c.r * 255.0) & 0x000000FF)        |
        (uint(c.g * 255.0) & 0x000000FF) << 8   |
        (uint(c.b * 255.0) & 0x000000FF) << 16  |
        (uint(c.a * 255.0) & 0x000000FF) << 24  ;
}

#define unpackUintColor unpackLittleEndianUintColor

float getLuminance(vec3 c)
{
    return 0.2125 * c.r + 0.7154 * c.g + 0.0721 * c.b;
}

float saturate(float a)
{
    return clamp(a, 0.0, 1.0);
}

float lengthSquared(const vec3 v)
{
    return dot(v, v);
}

float safePositiveRcp(float f)
{
    return f <= 0.0 ? 0.0 : 1.0 / f;
}

float square(float x)
{
    return x * x;
}



struct DirectionAndLength { vec3 dir; float len; };

DirectionAndLength calcDirectionAndLength(const vec3 start, const vec3 end)
{
    DirectionAndLength r;
    r.dir = end - start;
    r.len = length(r.dir);
    r.dir /= r.len;

    return r;
}

DirectionAndLength calcDirectionAndLengthSafe(const vec3 start, const vec3 end)
{
    DirectionAndLength r;
    r.dir = end - start;
    r.len = max(length(r.dir), 0.001);
    r.dir /= r.len;

    return r;
}



#define NORMAL_QUANTIZATION 65535.0

vec2 signNotZero(vec2 v)
{
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

uint encodeNormal(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 0.0001);

    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * signNotZero(n.xy);
    }

    const vec2 p = n.xy * 0.5 + 0.5;

    return (uint(round(p.x * NORMAL_QUANTIZATION)) << 16) | uint(round(p.y * NORMAL_QUANTIZATION));
}

vec3 decodeNormal(uint _packed)
{
    const vec2 p = vec2(_packed >> 16, _packed & 0xFFFF) * (2.0 / NORMAL_QUANTIZATION) - 1.0;

    vec3 n = vec3(p, 1.0 - abs(p.x) - abs(p.y));

    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * signNotZero(p);
    }

    return n * inversesqrt(max(dot(n, n), 1e-8));
}

vec3 safeNormalize(const vec3 v)
{
    const float d2 = dot(v, v);
    return d2 > 1e-6 ? v * inversesqrt(d2) : vec3(0, 1, 0);
}



// https://www.khronos.org/registry/OpenGL/extensions/EXT/EXT_texture_shared_exponent.txt

#define ENCODE_E5B9G9R9_EXPONENT_BITS 5
#define ENCODE_E5B9G9R9_MANTISSA_BITS 9
#define ENCODE_E5B9G9R9_MAX_VALID_BIASED_EXP 31
#define ENCODE_E5B9G9R9_EXP_BIAS 15

#define ENCODE_E5B9G9R9_MANTISSA_VALUES (1 << 9)
#define ENCODE_E5B9G9R9_MANTISSA_MASK (ENCODE_E5B9G9R9_MANTISSA_VALUES - 1)
// Equals to (((float)(MANTISSA_VALUES - 1))/MANTISSA_VALUES * (1<<(MAX_VALID_BIASED_EXP-EXP_BIAS)))
#define ENCODE_E5B9G9R9_SHAREDEXP_MAX 65408

uint encodeE5B9G9R9(vec3 unpacked)
{
    const int N = ENCODE_E5B9G9R9_MANTISSA_BITS;
    const int Np2 = 1 << N;
    const int B = ENCODE_E5B9G9R9_EXP_BIAS;

    unpacked = clamp(unpacked, vec3(0.0), vec3(ENCODE_E5B9G9R9_SHAREDEXP_MAX));
    float max_c = max(unpacked.r, max(unpacked.g, unpacked.b));

    // for log2
    if (max_c == 0.0)
    {
        return 0;
    }

    int exp_shared_p = max(-B-1, int(floor(log2(max_c)))) + 1 + B;
    int max_s = int(round(max_c * exp2(-(exp_shared_p - B - N))));

    int exp_shared = max_s != Np2 ? 
        exp_shared_p : 
        exp_shared_p + 1;

    float s = exp2(-(exp_shared - B - N));
    uvec3 rgb_s = uvec3(round(unpacked * s));

    return 
        (exp_shared << (3 * ENCODE_E5B9G9R9_MANTISSA_BITS)) |
        (rgb_s.b    << (2 * ENCODE_E5B9G9R9_MANTISSA_BITS)) |
        (rgb_s.g    << (1 * ENCODE_E5B9G9R9_MANTISSA_BITS)) |
        (rgb_s.r);
}

vec3 decodeE5B9G9R9(const uint _packed)
{
    const int N = ENCODE_E5B9G9R9_MANTISSA_BITS;
    const int B = ENCODE_E5B9G9R9_EXP_BIAS;

    int exp_shared = int(_packed >> (3 * ENCODE_E5B9G9R9_MANTISSA_BITS));
    float s = exp2(exp_shared - B - N);

    return s * vec3(
        (_packed                                       ) & ENCODE_E5B9G9R9_MANTISSA_MASK, 
        (_packed >> (1 * ENCODE_E5B9G9R9_MANTISSA_BITS)) & ENCODE_E5B9G9R9_MANTISSA_MASK,
        (_packed >> (2 * ENCODE_E5B9G9R9_MANTISSA_BITS)) & ENCODE_E5B9G9R9_MANTISSA_MASK
    );
}



#define TANGENT_HANDEDNESS_ENCODING_CONST 19
#define TANGENT_HANDEDNESS_ENCODING_THRESHOLD 3

// Encode normalized tangent vector with handedness (-1 or 1) to vec3
vec3 encodeTangent4(const vec3 tangent, float handedness)
{
    // handedness must be -1 or 1,
    //          then h is  1 or 0
    const float h = (-handedness + 1.0) * 0.5;

    // if handedness is  1, then tangent is a unit vector
    // if handedness is -1, then the length is (1.0 + TANGENT_HANDEDNESS_ENCODING_CONST)
    return tangent.xyz * (1.0 + h * TANGENT_HANDEDNESS_ENCODING_CONST);
}

vec4 decodeTangent4(const vec3 _packed)
{
    const float isUnitLen = float(dot(_packed, _packed) < TANGENT_HANDEDNESS_ENCODING_THRESHOLD);
    const float handedness = isUnitLen * 2.0 - 1.0;

    const float h = (-handedness + 1.0) * 0.5;

    return vec4(_packed / (1.0 + h * TANGENT_HANDEDNESS_ENCODING_CONST), handedness);
}

#endif // UTILS_H_