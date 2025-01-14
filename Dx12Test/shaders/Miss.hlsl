#include "Common.hlsl"

// ---[ Miss Shader ]---

[shader("miss")]
void Miss(inout HitInfo payload)
{
    payload.ShadedColorAndHitT = float4(0.0f, 0.2f, 0.4f, 1.0f);
}