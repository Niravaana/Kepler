#include "Common.hlsl"

// ---[ Ray Generation Shader ]---

[shader("raygeneration")]
void RayGen()
{
	float3 rayDir;
    float3 origin;
    
    GenerateCameraRay(DispatchRaysIndex().xy, origin, rayDir);
	
	// Setup the ray
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = rayDir;
	ray.TMin = 0.001f;
	ray.TMax = 10000.f;	

	// Trace the ray
	HitInfo payload;
	payload.ShadedColorAndHitT = float4(0.f, 0.f, 0.f, 0.f);

	TraceRay(
		SceneBVH,
		RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
		~0,
		0,
		1,
		0,
		ray,
		payload);

	RTOutput[DispatchRaysIndex().xy] = payload.ShadedColorAndHitT; //float4(payload.ShadedColorAndHitT.rgb, 1.f);
}
