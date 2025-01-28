
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


inline void GenerateCameraRay(uint2 index, out float3 origin, out float3 direction)
{
    float2 xy = index + 0.5f; // center in the middle of the pixel.
    float2 screenPos = xy / DispatchRaysDimensions().xy * 2.0 - 1.0;

    // Invert Y for DirectX-style coordinates.
    screenPos.y = -screenPos.y;

    // Unproject the pixel coordinate into a ray.
    float4 world = mul(g_sceneCB.projectionToWorld, float4(screenPos, 0, 1));

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