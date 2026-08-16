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

#ifndef BRDF_H_
#define BRDF_H_

#include "Random.h"


float roughnessSquaredToSpecPower(in float alpha) {
    return max(0.01, 2.0f / (square(alpha) + 1e-4) - 2.0f);
}


// subsurfaceAlbedo -- 0 if all light is absorbed,
//                     1 if no light is absorbed
float evalBRDFLambertian(float subsurfaceAlbedo)
{
    return subsurfaceAlbedo / M_PI;
}

// u1, u2   -- uniform random numbers
vec3 sampleLambertian(const vec3 n, float u1, float u2, out float oneOverPdf)
{
    return sampleOrientedHemisphere(n, u1, u2, oneOverPdf);
}



#define BRDF_MIN_SPECULAR_COLOR 0.04

vec3 getSpecularColor(const vec3 albedo, float metallic)
{
    vec3 minSpec = vec3(BRDF_MIN_SPECULAR_COLOR);
    return mix(minSpec, albedo, metallic);
}

#define AO_ALBEDO_THRESHOLD 0.02

float getMaterialAmbient(const vec3 albedo)
{
    float l = getLuminance(albedo);

    return l > AO_ALBEDO_THRESHOLD ? 
        1.0 :
        1.0 - square((l - AO_ALBEDO_THRESHOLD) / AO_ALBEDO_THRESHOLD);
}

vec3 demodulateSpecular(const vec3 contrib, const vec3 surfSpecularColor)
{
    return contrib / max(vec3(0.01), surfSpecularColor);
}

// nl -- cos between surface normal and light direction
// specularColor -- reflectance color at zero angle
vec3 getFresnelSchlick(float nl, const vec3 specularColor)
{
    const float t = 1.0 - max(nl, 0);
    const float t5 = square(square(t)) * t;
    return specularColor + (vec3(1.0) - specularColor) * t5;
}

float getFresnelSchlick(float n1, float n2, const vec3 V, const vec3 N)
{
    float R0 = (n1 - n2) / (n1 + n2);
    R0 *= R0;

    const float t = 1.0 - abs(dot(N, V));
    const float t5 = square(square(t)) * t;

    return mix(R0, 1.0, t5);
}

float getFresnelDielectric(float n1, float n2, float cosThetaI, float cosThetaT)
{
    float rParallel = (n2 * cosThetaI - n1 * cosThetaT) / max(1e-4, n2 * cosThetaI + n1 * cosThetaT);
    float rPerp = (n1 * cosThetaI - n2 * cosThetaT) / max(1e-4, n1 * cosThetaI + n2 * cosThetaT);
    return clamp(0.5 * (rParallel * rParallel + rPerp * rPerp), 0.0, 1.0);
}

// GGX distribution
float D_GGX( float nm, float alpha )
{
#if SHIPPING_HACK
#ifdef FORCE_EVALBRDF_GGX_LOOSE
    // shipping hack: make ggx factor be non-zero, so we can reuse for reprojection,
    // let there be some lag, but at least less noise
    alpha = max( 0.2, alpha );
#endif
#endif

    const float alphaSq = square( alpha );

    nm = max( 0.0, nm );
    const float nm2 = nm * nm;
    return alphaSq / M_PI / square( nm2 * ( alphaSq - 1 ) + 1 );
}

// Smith G1 for GGX, Karis' approximation ("Real Shading in Unreal Engine 4")
// ns = dot( macrosurface normal, s ), where s is either v or l
float G1_GGX( float ns, float alpha )
{
    return 2 * ns * safePositiveRcp( ns * ( 2 - alpha ) + alpha );
}

#define MIN_GGX_ROUGHNESS 0.005

vec2 getEnvBRDFApprox(float nv, float alpha)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = alpha * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

float evalBRDFHammonDiffuse(const vec3 n, const vec3 v, const vec3 l, float alpha)
{
    float nl = max(dot(n, l), 0.0);
    float nv = max(dot(n, v), 0.0);
    if (nl <= 0.0 || nv <= 0.0)
    {
        return 0.0;
    }
    vec3 h = normalize(v + l);
    float vh = clamp(dot(v, h), 0.0, 1.0);
    float facing = 0.5 + 0.5 * dot(v, l);
    float rough = facing * (0.9 - 0.4 * facing) * safePositiveRcp(max(nl, nv) + 0.1) + 0.5;
    float tL = 1.0 - nl;
    float tV = 1.0 - nv;
    float tL5 = square(square(tL)) * tL;
    float tV5 = square(square(tV)) * tV;
    float smoothVal = 1.05 * (1.0 - tL5) * (1.0 - tV5);
    return mix(smoothVal, rough, alpha) / M_PI;
}

// n -- macrosurface normal
// v -- direction to viewer
// l -- direction to light
// alpha -- roughness
vec3 evalBRDFSmithGGX(const vec3 n, const vec3 v, const vec3 l, float alpha, const vec3 specularColor)
{
    alpha = max(alpha, MIN_GGX_ROUGHNESS);

    const float nl = max(dot(n, l), 0.0);
    const float nv = max(dot(n, v), 0.0);

    if (nl <= 0.0 || nv <= 0.0)
    {
        return vec3(0.0);
    }

    const vec3  h = normalize( v + l );
    const vec3  F = getFresnelSchlick(clamp(dot(v, h), 0.0, 1.0), specularColor);
    const float D = D_GGX( dot( n, h ), alpha );

    const float G2Modif = 0.5 / mix(2.0 * nl * nv, nl + nv, alpha);
    vec3 fSingle = F * G2Modif * D;

    vec2 dfgV = getEnvBRDFApprox(nv, alpha);
    vec2 dfgL = getEnvBRDFApprox(nl, alpha);
    float Ev = dfgV.x + dfgV.y;
    float El = dfgL.x + dfgL.y;
    float Eavg = mix(1.0, 0.45, alpha);

    vec3 Favg = specularColor + (vec3(1.0) - specularColor) * (1.0 / 21.0);
    vec3 fMs = (vec3(1.0 - Ev) * vec3(1.0 - El) / (M_PI * (1.0 - Eavg + 1e-4))) * (Favg * Eavg / max(vec3(1e-4), vec3(1.0) - Favg * (1.0 - Eavg)));

    return fSingle + fMs;
}



// "Sampling the GGX Distribution of Visible Normals", Heitz
// v        -- direction to viewer, normal's direction is (0,0,1)
// alpha    -- roughness
// u1, u2   -- uniform random numbers
// output   -- normal sampled with PDF D_v(Ne) = G1(v) * max(0, dot(v, Ne)) * D(Ne) / v.z
vec3 sampleGGXVNDF(const vec3 v, float alpha, float u1, float u2, out float oneOverPdf)
{
    alpha = max( alpha, MIN_GGX_ROUGHNESS );

    vec3 Vh = normalize(vec3(alpha * v.x, alpha * v.y, v.z));

    float phi = 2.0 * M_PI * u2;
    float z = (1.0 - u1) * (1.0 + Vh.z) - Vh.z;
    float sinTheta = sqrt(clamp(1.0 - z * z, 0.0, 1.0));
    float x = sinTheta * cos(phi);
    float y = sinTheta * sin(phi);
    vec3 c = vec3(x, y, z);
    vec3 Nh = c + Vh;

    const vec3 Ne = normalize(vec3(alpha * Nh.x, alpha * Nh.y, max(0.001, Nh.z)));

    const float nm = Ne.z;
    const float D = D_GGX( nm, alpha );
    const float nv = v.z;
    const float G1 = G1_GGX( nv, alpha );

    oneOverPdf = v.z * safePositiveRcp(G1 * max(0.0, dot(v, Ne)) * D);

    return Ne;
}

// Sample microfacet normal
// n        -- macrosurface normal, world space
// v        -- direction to viewer, world space
// alpha    -- roughness
// u1, u2   -- uniform random numbers
// Check Heitz's paper for the special representation of rendering equation term 
vec3 sampleSmithGGX(const vec3 n, const vec3 v, float alpha, float u1, float u2, out float oneOverPdf)
{
    const mat3 basis = getONB(n);

    // get v in normal's space, basis is orthogonal
    const vec3 ve = transpose(basis) * v;

    // microfacet normal
    const vec3 m = sampleGGXVNDF(ve, alpha, u1, u2, oneOverPdf);

    // reflect viewer dir by a microfacet
    const vec3 l = reflect( -ve, m );
    // reflection jacobian
    oneOverPdf *= 4 * dot( ve, m );

    // back to world space
    return basis * l;
}

#endif // BRDF_H_