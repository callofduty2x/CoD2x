// CoD2x: color pass over the finished 3D scene (see filmtweak.cpp)
//
// The compiled bytecode is in filmtweak_ps.h, regenerate it after a change with (Windows SDK):
//   fxc /nologo /T ps_2_0 /E main /O3 /Vn filmtweak_psBytecode /Fh src/mss32/filmtweak_ps.h src/mss32/filmtweak.hlsl

sampler2D sceneSampler : register(s0);  // copy of the back buffer

float4 contrast   : register(c0);   // xyz = contrast
float4 bias       : register(c1);   // xyz = brightness + 0.5 - 0.5 * contrast
float4 tintDark   : register(c2);   // xyz = dark tint * color multipliers
float4 tintDelta  : register(c3);   // xyz = (light tint - dark tint) * color multipliers
float4 params     : register(c4);   // x = saturation, y = 1 / gamma

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float3 color = tex2D(sceneSampler, uv).rgb;

    // Contrast around the middle grey and brightness
    color = color * contrast.xyz + bias.xyz;

    // Saturation: 0 = grey, 1 = unchanged, 2 = doubled
    float luminance = dot(color, float3(0.299, 0.587, 0.114));
    color = lerp(luminance.xxx, color, params.x);

    // Tint by the brightness of the pixel (dark tint in shadows, light tint in highlights) and the color multipliers
    color *= tintDark.xyz + tintDelta.xyz * saturate(luminance);

    // Gamma
    color = pow(saturate(color), params.y);

    return float4(color, 1);
}
