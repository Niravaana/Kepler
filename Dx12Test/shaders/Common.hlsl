
typedef float3 XMFLOAT3;
typedef float4 XMFLOAT4;
typedef float4 XMVECTOR;
typedef float4x4 XMMATRIX;
typedef uint UINT;

//ToDo put this in common head between app and shader
struct HitInfo
{
	float4 ShadedColorAndHitT;
};

struct Attributes 
{
	float2 uv;
};

struct SceneConstantBuffer
{
    float4x4 projectionToWorld;
    float4 cameraPosition;
    float4 lightPosition;
    float4 lightAmbientColor;
    float4 lightDiffuseColor;
};

struct CubeConstantBuffer
{
    XMFLOAT4 albedo;
};

struct Vertex
{
    XMFLOAT3 position;
    XMFLOAT3 normal;
};

// ---[ Resources ]---
RWTexture2D<float4> RTOutput				: register(u0);
RaytracingAccelerationStructure SceneBVH	: register(t0, space0);
ByteAddressBuffer Indices					: register(t1, space0);
StructuredBuffer<Vertex> Vertices			: register(t2, space0);

// ---[ Constant Buffers ]---
ConstantBuffer<SceneConstantBuffer> g_sceneCB : register(b0, space1);
ConstantBuffer<CubeConstantBuffer> g_cubeCB : register(b1, space1);

// ---[ Helper Functions ]---

//uint3 Load3x16BitIndices(uint offsetBytes)
//{
//    uint3 indices;
//
//    // ByteAdressBuffer loads must be aligned at a 4 byte boundary.
//    // Since we need to read three 16 bit indices: { 0, 1, 2 } 
//    // aligned at a 4 byte boundary as: { 0 1 } { 2 0 } { 1 2 } { 0 1 } ...
//    // we will load 8 bytes (~ 4 indices { a b | c d }) to handle two possible index triplet layouts,
//    // based on first index's offsetBytes being aligned at the 4 byte boundary or not:
//    //  Aligned:     { 0 1 | 2 - }
//    //  Not aligned: { - 0 | 1 2 }
//    const uint dwordAlignedOffset = offsetBytes & ~3;    
//    const uint2 four16BitIndices = Indices.Load2(dwordAlignedOffset);
// 
//    // Aligned: { 0 1 | 2 - } => retrieve first three 16bit indices
//    if (dwordAlignedOffset == offsetBytes)
//    {
//        indices.x = four16BitIndices.x & 0xffff;
//        indices.y = (four16BitIndices.x >> 16) & 0xffff;
//        indices.z = four16BitIndices.y & 0xffff;
//    }
//    else // Not aligned: { - 0 | 1 2 } => retrieve last three 16bit indices
//    {
//        indices.x = (four16BitIndices.x >> 16) & 0xffff;
//        indices.y = four16BitIndices.y & 0xffff;
//        indices.z = (four16BitIndices.y >> 16) & 0xffff;
//    }
//
//    return indices;
//}

inline void GenerateCameraRay(uint2 index, out float3 origin, out float3 direction)
{
    float2 xy = index + 0.5f; // center in the middle of the pixel.
    float2 screenPos = xy / DispatchRaysDimensions().xy * 2.0 - 1.0;

    // Invert Y for DirectX-style coordinates.
    screenPos.y = -screenPos.y;

    // Unproject the pixel coordinate into a ray.
    float4x4 projToWorld = {{0.520699024, -9.01753872e-09, -0.520699084, 2.85857560e-09}, 
{0.108777761, 0.384587646, 0.108777747, 4.32953495e-09},
{3.50725055, -1.98400080, 3.50725007, -0.992000341}, 
{-2.87900329, 1.62861001, -2.87900233, 1.00000036} 
};
    float4 world = mul(float4(screenPos, 0, 1),  g_sceneCB.projectionToWorld);

    world.xyz /= world.w;
    origin = g_sceneCB.cameraPosition.xyz;
    direction = normalize(world.xyz - origin);
}

float3 HitAttribute(float3 vertexAttribute[3], BuiltInTriangleIntersectionAttributes attr)
{
    return vertexAttribute[0] +
        attr.barycentrics.x * (vertexAttribute[1] - vertexAttribute[0]) +
        attr.barycentrics.y * (vertexAttribute[2] - vertexAttribute[0]);
}

float4 CalculateDiffuseLighting(float3 hitPosition, float3 normal)
{
    float3 pixelToLight = normalize(g_sceneCB.lightPosition.xyz - hitPosition);

    float fNDotL = max(0.0f, dot(pixelToLight, normal));

    return float4(1.0f, 1.0f, 1.0f, 1.0f) * g_sceneCB.lightDiffuseColor * fNDotL;
}

float3 HitWorldPosition()
{
    return WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
}