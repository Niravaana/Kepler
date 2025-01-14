#include "Common.hlsl"

// ---[ Closest Hit Shader ]---

[shader("closesthit")]
void ClosestHit(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attrib)
{
	float3 hitPosition = HitWorldPosition();
	
	//triangles first index using prim Idx as 16 bit indices used
	uint indexSizeInBytes = 4;
    uint indicesPerTriangle = 3;
    uint triangleIndexStride = indicesPerTriangle * indexSizeInBytes;
    uint baseIndex = PrimitiveIndex() * triangleIndexStride;

	//Load indices
	const uint3 indices = Indices.Load3(baseIndex); 

	 float3 vertexNormals[3] = { 
        Vertices[indices[0]].normal, 
        Vertices[indices[1]].normal, 
        Vertices[indices[2]].normal 
    };

	float3 triangleNormal = HitAttribute(vertexNormals, attrib);

    float4 diffuseColor = CalculateDiffuseLighting(hitPosition, triangleNormal);
    float4 color = g_sceneCB.lightAmbientColor + diffuseColor;
    payload.ShadedColorAndHitT = color; //float4(color.rgb, RayTCurrent());
}