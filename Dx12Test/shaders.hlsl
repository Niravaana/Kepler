//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
// const buffers must allocated with 256 byte alignment
cbuffer SceneConstantBuffer : register(b0)
{
    float4x4 World;
    float4x4 WorldView;
    float4x4 WorldViewProj;
};

struct VertexPos
{
    float3 Position : POSITION;
};

struct VertexShaderOutput
{
    float4 Position : SV_Position;
};

VertexShaderOutput VSMain(VertexPos In)
{
    VertexShaderOutput result;
    result.Position =  mul(float4(In.Position,1.0f), WorldViewProj);
    return result;
}

float4 PSMain(VertexShaderOutput input) : SV_TARGET
{
    return input.Position;
}
