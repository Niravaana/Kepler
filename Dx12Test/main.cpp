#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN			
#endif

/*
*   ToDos 
 1. currently two command list created for each frame though we only use 0th commandlist
 2. Material if not mentioned loader currently throws exception
 3. Resource clean up is still remaining
 4. change root/desc heap in order of most freq used to least freq used for perf reasons
*/

/* Next Target
1. Load cube model 
2. Check out how they are passing vertex and index buffer 
3. Make appropriate shader changes.
4. Point light cube demo with RT should be working
*/


#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN	
#endif
#include <Windows.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include <dxgi1_6.h>
#include <d3d12.h>

#include <string>
#include <wrl.h>
#include <shellapi.h>
#include <stdexcept>
#include <shlobj.h>
#include <strsafe.h>
#include <unordered_map>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#include "StepTimer.h"
#include "dxc/dxcapi.h"
#include "dxc/dxcapi.use.h"

#define NAME_D3D_RESOURCES 1
#define SAFE_RELEASE( x ) { if ( x ) { x->Release(); x = NULL; } }
#define SAFE_DELETE( x ) { if( x ) delete x; x = NULL; }
#define SAFE_DELETE_ARRAY( x ) { if( x ) delete[] x; x = NULL; }
#define ALIGN(_alignment, _val) (((_val + _alignment - 1) / _alignment) * _alignment)

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace std;

class HrException : public std::runtime_error
{
    inline std::string HrToString(HRESULT hr)
    {
        char s_str[64] = {};
        sprintf_s(s_str, "HRESULT of 0x%08X", static_cast<UINT>(hr));
        return std::string(s_str);
    }
public:
    HrException(HRESULT hr) : std::runtime_error(HrToString(hr)), m_hr(hr) {}
    HRESULT Error() const { return m_hr; }
private:
    const HRESULT m_hr;
};

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        throw HrException(hr);
    }
}

inline void ThrowIfFailed(HRESULT hr, const wchar_t* msg)
{
    if (FAILED(hr))
    {
        OutputDebugString(msg);
        throw HrException(hr);
    }
}

/*
 ------------------------------Common Types------------------------------------
*/
constexpr UINT gFrameCount = 2;

static const D3D12_HEAP_PROPERTIES UploadHeapProperties =
{
	D3D12_HEAP_TYPE_UPLOAD,
	D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
	D3D12_MEMORY_POOL_UNKNOWN,
	0, 0
};

static const D3D12_HEAP_PROPERTIES DefaultHeapProperties =
{
	D3D12_HEAP_TYPE_DEFAULT,
	D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
	D3D12_MEMORY_POOL_UNKNOWN,
	0, 0
};

constexpr UINT g_frameCount = 2;

struct GlobalState
{
    UINT width = 1280;
    UINT height = 720;
    BOOL vsync = false;
    
}gAppState;

struct CubeConstantBuffer
{
    XMFLOAT4 albedo;
};

struct SceneConstantBuffer
{
    XMMATRIX projectionToWorld;
    XMVECTOR cameraPosition;
    XMVECTOR lightPosition;
    XMVECTOR lightAmbientColor;
    XMVECTOR lightDiffuseColor;
};

struct TextureInfo
{
	std::vector<UINT8> pixels;
	int width = 0;
	int height = 0;
	int stride = 0;
	int offset = 0;
};

struct D3D12ShaderInfo 
{
	LPCWSTR		filename = nullptr;
	LPCWSTR		entryPoint = nullptr;
	LPCWSTR		targetProfile = nullptr;
	LPCWSTR*	arguments = nullptr;
	DxcDefine*	defines = nullptr;
	UINT32		argCount = 0;
	UINT32		defineCount = 0;

	D3D12ShaderInfo() {}
	D3D12ShaderInfo(LPCWSTR inFilename, LPCWSTR inEntryPoint, LPCWSTR inProfile)
	{
		filename = inFilename;
		entryPoint = inEntryPoint;
		targetProfile = inProfile;
	}
};

struct D3D12ShaderCompilerInfo
{
	dxc::DxcDllSupport		DxcDllHelper;
	IDxcCompiler* compiler = nullptr;
	IDxcLibrary* library = nullptr;

	static void CompileShader(D3D12ShaderCompilerInfo& compilerInfo, D3D12ShaderInfo& info, IDxcBlob** blob)
	{
		HRESULT hr;
		UINT32 code(0);
		IDxcBlobEncoding* pShaderText(nullptr);

		ThrowIfFailed(compilerInfo.library->CreateBlobFromFile(info.filename, &code, &pShaderText), L"Failed to create blob from shader file");

		ComPtr<IDxcIncludeHandler> dxcIncludeHandler;
		ThrowIfFailed(compilerInfo.library->CreateIncludeHandler(&dxcIncludeHandler), L"Failed to create include handler");

		IDxcOperationResult* result;
		ThrowIfFailed(compilerInfo.compiler->Compile(
			pShaderText,
			info.filename,
			info.entryPoint,
			info.targetProfile,
			info.arguments,
			info.argCount,
			info.defines,
			info.defineCount,
			dxcIncludeHandler.Get(),
			&result), L"Failed to compile shader");

		result->GetStatus(&hr);

		if (FAILED(hr))
		{
			IDxcBlobEncoding* error;
			ThrowIfFailed(result->GetErrorBuffer(&error), L"Failed to get shader compiler error buffer");

			vector<char> infoLog(error->GetBufferSize() + 1);
			memcpy(infoLog.data(), error->GetBufferPointer(), error->GetBufferSize());
			infoLog[error->GetBufferSize()] = 0;

			string errorMsg = "Shader Compiler Error:\n";
			errorMsg.append(infoLog.data());

			MessageBoxA(nullptr, errorMsg.c_str(), "Error!", MB_OK);
			return;
		}

		ThrowIfFailed(result->GetResult(blob), L"Failed to get shader blob result");
	}
};

struct Material 
{
	std::string name = "defaultMaterial";
	std::string texturePath = "";
	float  textureResolution = 512;
};

/*
 ------------------------------Utility Functions------------------------------------
*/

namespace Utility
{
    static bool CompareVector3WithEpsilon(const DirectX::XMFLOAT3& lhs, const DirectX::XMFLOAT3& rhs)
    {
        const DirectX::XMFLOAT3 vector3Epsilon = DirectX::XMFLOAT3(0.00001f, 0.00001f, 0.00001f);
        return DirectX::XMVector3NearEqual(DirectX::XMLoadFloat3(&lhs), DirectX::XMLoadFloat3(&rhs), DirectX::XMLoadFloat3(&vector3Epsilon)) == TRUE;
    }

    static bool CompareVector2WithEpsilon(const DirectX::XMFLOAT2& lhs, const DirectX::XMFLOAT2& rhs)
    {
        const DirectX::XMFLOAT2 vector2Epsilon = DirectX::XMFLOAT2(0.00001f, 0.00001f);
        return DirectX::XMVector3NearEqual(DirectX::XMLoadFloat2(&lhs), DirectX::XMLoadFloat2(&rhs), DirectX::XMLoadFloat2(&vector2Epsilon)) == TRUE;
    }

	static void FormatTexture(TextureInfo& info, UINT8* pixels)
	{
		const UINT numPixels = (info.width * info.height);
		const UINT oldStride = info.stride;
		const UINT oldSize = (numPixels * info.stride);

		const UINT newStride = 4;				// uploading textures to GPU as DXGI_FORMAT_R8G8B8A8_UNORM
		const UINT newSize = (numPixels * newStride);
		info.pixels.resize(newSize);

		for (UINT i = 0; i < numPixels; i++)
		{
			info.pixels[i * newStride] = pixels[i * oldStride];		// R
			info.pixels[i * newStride + 1] = pixels[i * oldStride + 1];	// G
			info.pixels[i * newStride + 2] = pixels[i * oldStride + 2];	// B
			info.pixels[i * newStride + 3] = 0xFF;							// A (always 1)
		}

		info.stride = newStride;
	}

	static TextureInfo LoadTexture(string filepath)
	{
		TextureInfo result = {};

		// Load image pixels with stb_image
		UINT8* pixels = stbi_load(filepath.c_str(), &result.width, &result.height, &result.stride, STBI_default);
		if (!pixels)
		{
			throw runtime_error("Error: failed to load image!");
		}

		FormatTexture(result, pixels);
		stbi_image_free(pixels);
		return result;
	}

}



struct Vertex
{
	DirectX::XMFLOAT3 position;
	DirectX::XMFLOAT3 normal;
	DirectX::XMFLOAT2 uv;

	bool operator==(const Vertex &v) const 
	{
		if (Utility::CompareVector3WithEpsilon(position, v.position)) 
		{
			if (Utility::CompareVector2WithEpsilon(uv, v.uv)) return true;
			return true;
		}
		return false;
	}

	Vertex& operator=(const Vertex& v) 
	{
		position = v.position;
		uv = v.uv;
		return *this;
	}

};

namespace std
{
	void hash_combine(size_t &seed, size_t hash)
	{
		hash += 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= hash;
	}

	template<> struct hash<Vertex> 
	{
		size_t operator()(Vertex const &vertex) const 
		{
			size_t seed = 0;
			hash<float> hasher;
			hash_combine(seed, hasher(vertex.position.x));
			hash_combine(seed, hasher(vertex.position.y));
			hash_combine(seed, hasher(vertex.position.z));

			hash_combine(seed, hasher(vertex.uv.x));
			hash_combine(seed, hasher(vertex.uv.y));

			return seed;
		}
	};
}

struct Mesh
{
	std::vector<Vertex> vertices;
	std::vector<UINT> indices;
    Material material;

	static void LoadModel(string filepath, Mesh& model)
	{
		tinyobj::attrib_t attrib;
		std::vector<tinyobj::shape_t> shapes;
		std::vector<tinyobj::material_t> materials;
		std::string err;
        std::string warn;

		// Load the OBJ and MTL files
		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filepath.c_str(), "Materials\\"))
		{
			throw std::runtime_error(err);
		}

		if (materials.empty())
		{
			throw std::runtime_error(err);
		}

		// Get the first material
		// Only support a single material right now
		model.material.name = materials[0].name;
		model.material.texturePath = materials[0].diffuse_texname;

		// Parse the model and store the unique vertices
		unordered_map<Vertex, uint32_t> uniqueVertices = {};
		for (const auto& shape : shapes)
		{
			for (const auto& index : shape.mesh.indices)
			{
				Vertex vertex = {};
				vertex.position =
				{
					attrib.vertices[3 * index.vertex_index + 2],
					attrib.vertices[3 * index.vertex_index + 1],
					attrib.vertices[3 * index.vertex_index + 0]
				};

				vertex.uv =
				{
					attrib.texcoords[2 * index.texcoord_index + 0],
					1 - attrib.texcoords[2 * index.texcoord_index + 1]
				};

				// Fast find unique vertices using a hash
				if (uniqueVertices.count(vertex) == 0)
				{
					uniqueVertices[vertex] = static_cast<uint32_t>(model.vertices.size());
					model.vertices.push_back(vertex);
				}

				model.indices.push_back(uniqueVertices[vertex]);
			}
		}
	}
};



/*
 ------------------------------Device Specific Resources------------------------------------
*/

struct DeviceResources
{
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
	ID3D12Device5* device = nullptr;
    ID3D12CommandQueue*		cmdQueue = nullptr;
	ID3D12CommandAllocator*	cmdAllocator[g_frameCount] = { nullptr, nullptr };
    ID3D12GraphicsCommandList4* cmdList[g_frameCount] = { nullptr, nullptr };
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValues[g_frameCount] = { 0, 0 };
    HANDLE fenceEvent;
    UINT frameIndex = 0;
    IDXGISwapChain3* swapChain3 = nullptr;
    ID3D12Resource* backBuffer[2] = { nullptr, nullptr };
	ID3D12Resource* DXROutput;
};

/*
 ------------------------------Ray tracing Specific Resources------------------------------------
*/

struct RtProgram
{
	D3D12ShaderInfo			info = {};
	IDxcBlob*				blob = nullptr;
	ID3D12RootSignature*	pRootSignature = nullptr;

	D3D12_DXIL_LIBRARY_DESC	dxilLibDesc;
	D3D12_EXPORT_DESC		exportDesc;
	D3D12_STATE_SUBOBJECT	subobject;
	std::wstring			exportName;

	RtProgram()
	{
		exportDesc.ExportToRename = nullptr;
	}

	RtProgram(D3D12ShaderInfo shaderInfo)
	{
		info = shaderInfo;
		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
		exportName = shaderInfo.entryPoint;
		exportDesc.ExportToRename = nullptr;
		exportDesc.Flags = D3D12_EXPORT_FLAG_NONE;
	}

	void SetBytecode()
	{
		exportDesc.Name = exportName.c_str();

		dxilLibDesc.NumExports = 1;
		dxilLibDesc.pExports = &exportDesc;
		dxilLibDesc.DXILLibrary.BytecodeLength = blob->GetBufferSize();
		dxilLibDesc.DXILLibrary.pShaderBytecode = blob->GetBufferPointer();

		subobject.pDesc = &dxilLibDesc;
	}

	void CompileProgram(D3D12ShaderCompilerInfo& compilerInfo)
	{
		D3D12ShaderCompilerInfo::CompileShader(compilerInfo, info, &blob);
		SetBytecode();
	}

};

struct HitProgram
{
	RtProgram ahs;
	RtProgram chs;

	std::wstring exportName;
	D3D12_HIT_GROUP_DESC desc = {};
	D3D12_STATE_SUBOBJECT subobject = {};

	HitProgram() {}
	HitProgram(LPCWSTR name) : exportName(name)
	{
		desc = {};
		desc.HitGroupExport = exportName.c_str();
		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
		subobject.pDesc = &desc;
	}

	void SetExports(bool anyHit)
	{
		desc.HitGroupExport = exportName.c_str();
		if (anyHit) desc.AnyHitShaderImport = ahs.exportDesc.Name;
		desc.ClosestHitShaderImport = chs.exportDesc.Name;
	}

};

struct AccelerationStructureBuffer
{
	ID3D12Resource* pScratch = nullptr;
	ID3D12Resource* pResult = nullptr;
	ID3D12Resource* pInstanceDesc = nullptr;	// only used in top-level AS
};

struct RayTracingResources
{
	AccelerationStructureBuffer	TLAS;
	AccelerationStructureBuffer	BLAS;
	UINT64 tlasSize;
	RtProgram rayGenProg;
	RtProgram missProg;
	HitProgram hitProg;
	ID3D12StateObject* rtpso = nullptr;
	ID3D12StateObjectProperties* rtpsoInfo = nullptr;
	ID3D12Resource* shaderTable = nullptr;
	uint32_t shaderTableRecordSize = 0;
};

/*
 ------------------------------Application Specific Resources------------------------------------
*/

struct AppResources
{
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    UINT rtvDescSize = 0;
    ID3D12Resource* vertexBuffer = nullptr;
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    ID3D12Resource* indexBuffer = nullptr;
	D3D12_INDEX_BUFFER_VIEW indexBufferView;
    ID3D12Resource* texture = nullptr;
	ID3D12Resource* textureUploadResource = nullptr;
	ID3D12DescriptorHeap* descriptorHeap = nullptr;

	//camera params
	StepTimer timer;
    float curRotationAngleRad;
    XMVECTOR eye;
    XMVECTOR at;
    XMVECTOR up;

	//scene light pos mat buffers
	SceneConstantBuffer sceneParams[gFrameCount];
	ID3D12Resource* sceneParamsCB = nullptr;
	SceneConstantBuffer* sceneParamsMappedPtr = nullptr;

    CubeConstantBuffer cubeParams;
	ID3D12Resource* cubeParamsCB = nullptr;
	CubeConstantBuffer* cubeParamsMappedPtr = nullptr;
};

/*
 ------------------------------Application settings------------------------------------
*/

struct Application;
struct Application
{
    Application() = default;
	~Application()
	{
		//ToDo clean up all the resource
	}

    void Init(UINT width, UINT height, BOOL vsync, std::string meshFilePath);
    void Render();
    void Update();
	void InitializeSceneParams();
	void UpdateCameraMatrices();
    void InitShaderCompiler()
	{
		ThrowIfFailed(shaderCompiler.DxcDllHelper.Initialize());
		ThrowIfFailed(shaderCompiler.DxcDllHelper.CreateInstance(CLSID_DxcCompiler, &shaderCompiler.compiler));
		ThrowIfFailed((shaderCompiler.DxcDllHelper.CreateInstance(CLSID_DxcLibrary, &shaderCompiler.library)));
	}

    HWND hwnd;
    HINSTANCE instance;
    Mesh mesh;
    D3D12ShaderCompilerInfo shaderCompiler;
	DeviceResources dr;
	AppResources ar;
	RayTracingResources rt;
};

static void WaitForGPU(DeviceResources& dr) 
{
	ThrowIfFailed(dr.cmdQueue->Signal(dr.fence, dr.fenceValues[dr.frameIndex]), L"Failed to signal the fence");

	ThrowIfFailed(dr.fence->SetEventOnCompletion(dr.fenceValues[dr.frameIndex], dr.fenceEvent), L"Failed to set the event");
	WaitForSingleObjectEx(dr.fenceEvent, INFINITE, FALSE);
	dr.fenceValues[dr.frameIndex]++;
}

static void MoveToNextFrame(DeviceResources& dr) 
{
	const UINT64 currentFenceValue = dr.fenceValues[dr.frameIndex];
	ThrowIfFailed(dr.cmdQueue->Signal(dr.fence, currentFenceValue), L"Failed to signal command queue");
	dr.frameIndex = dr.swapChain3->GetCurrentBackBufferIndex();

	if (dr.fence->GetCompletedValue() < dr.fenceValues[dr.frameIndex])
	{
		ThrowIfFailed(dr.fence->SetEventOnCompletion(dr.fenceValues[dr.frameIndex], dr.fenceEvent), L"Failed to set fence value");
		WaitForSingleObjectEx(dr.fenceEvent, INFINITE, FALSE);
	}

	dr.fenceValues[dr.frameIndex] = currentFenceValue + 1;
}

static void DestroyResources(DeviceResources& dr)
{
	for (UINT i = 0; i < gFrameCount; i++)
	{
		SAFE_RELEASE(dr.backBuffer[i]);
		SAFE_RELEASE(dr.cmdAllocator[i]);
		SAFE_RELEASE(dr.cmdList[i]);
	}
	SAFE_RELEASE(dr.fence);
	SAFE_RELEASE(dr.swapChain3);
	SAFE_RELEASE(dr.cmdQueue);
	SAFE_RELEASE(dr.device);
	SAFE_RELEASE(dr.adapter);
	SAFE_RELEASE(dr.factory);
}


static void CreateDevice(DeviceResources& dr)
{
#if defined _DEBUG
    //enable debug layers
    {
        ID3D12Debug* debugController;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
        }
    }
#endif

    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&dr.factory)));

    // Create the device
	for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != dr.factory->EnumAdapters1(adapterIndex, &dr.adapter); ++adapterIndex)
	{
		DXGI_ADAPTER_DESC1 adapterDesc;
		dr.adapter->GetDesc1(&adapterDesc);

		if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		{			
			continue;	
		}

		if (SUCCEEDED(D3D12CreateDevice(dr.adapter, D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device5), (void**)&dr.device)))
		{
			// Check if the device supports ray tracing.
			D3D12_FEATURE_DATA_D3D12_OPTIONS5 features = {};
			HRESULT hr = dr.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &features, sizeof(features));
			if (FAILED(hr) || features.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
			{
				SAFE_RELEASE(dr.device);
				dr.device = nullptr;
				continue;
			}

#if NAME_D3D_RESOURCES
			dr.device->SetName(L"DXR Enabled Device");
			printf("Running on DXGI Adapter %S\n", adapterDesc.Description);
#endif
			break;
		}

		if (dr.device == nullptr)
		{
            ThrowIfFailed(E_FAIL, L"failed to create device.\n");
		}
	}
}

static void CreateCommandQueue(DeviceResources& dr)
{
	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	ThrowIfFailed(dr.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&dr.cmdQueue)), L"Failed to create command queue");
#if NAME_D3D_RESOURCES
	dr.cmdQueue->SetName(L"D3D12 Command Queue");
#endif
}

static void CreateCommandAllocator(DeviceResources& dr) 
{
	for (UINT n = 0; n < g_frameCount; n++)
	{
		ThrowIfFailed(dr.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&dr.cmdAllocator[n])), L"Failed to create command allocator");
#if NAME_D3D_RESOURCES
		if(n == 0) dr.cmdAllocator[n]->SetName(L"D3D12 Command Allocator 0");
		else dr.cmdAllocator[n]->SetName(L"D3D12 Command Allocator 1");
#endif
	}
}

static void CreateCommandList(DeviceResources& dr) 
{
    for (UINT n = 0; n < g_frameCount; n++)
    {
        ThrowIfFailed(dr.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dr.cmdAllocator[n], nullptr, IID_PPV_ARGS(&dr.cmdList[n])), L"Failed to create command list");
        ThrowIfFailed(dr.cmdList[n]->Close());
#if NAME_D3D_RESOURCES
        if(n == 0) dr.cmdList[n]->SetName(L"D3D12 Command List 0");
		else dr.cmdList[n]->SetName(L"D3D12 Command List 1");
#endif
    }
}

static void ResetCommandList(DeviceResources& dr) 
{
	// Reset the command allocator for the current frame
	ThrowIfFailed(dr.cmdAllocator[dr.frameIndex]->Reset(), L"Failed to reset command allocator");
	// Reset the command list for the current frame
	ThrowIfFailed(dr.cmdList[0]->Reset(dr.cmdAllocator[dr.frameIndex], nullptr), L"Failed to reset command list");
}

static void CreateFence(DeviceResources& dr) 
{
	ThrowIfFailed(dr.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dr.fence)), L"Failed to create fence");
#if NAME_D3D_RESOURCES
	dr.fence->SetName(L"D3D12 Fence");
#endif

	dr.fenceValues[dr.frameIndex]++;

	// Create an event handle to use for frame synchronization
	dr.fenceEvent = CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
	if (dr.fenceEvent == nullptr) 
	{
		HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
		ThrowIfFailed(hr, L"Error: failed to create fence event!");
	}
}

static void CreateSwapChain(DeviceResources& dr, HWND &window) 
{
	// Describe the swap chain
	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.BufferCount = 2;
	desc.Width = gAppState.width;
	desc.Height = gAppState.height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.SampleDesc.Count = 1;

	// Create the swap chain
	IDXGISwapChain1* swapChain1;
	ThrowIfFailed(dr.factory->CreateSwapChainForHwnd(dr.cmdQueue, window, &desc, nullptr, nullptr, &swapChain1),L"Failed to create swapchain");
	ThrowIfFailed(dr.factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), L"Failed in swapchain window association");

	ThrowIfFailed(swapChain1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&dr.swapChain3)), L"Failed in swapchain query");
	SAFE_RELEASE(swapChain1);
    dr.frameIndex = dr.swapChain3->GetCurrentBackBufferIndex();
}

static void CreateRTVDescHeap(DeviceResources& dr, AppResources& ar)
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
	rtvDesc.NumDescriptors = gFrameCount;
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ThrowIfFailed(dr.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&ar.rtvHeap)), L"Failed in creating RTV desc heap");
#if NAME_D3D_RESOURCES
	ar.rtvHeap->SetName(L"RTV Descriptor Heap");
#endif

	ar.rtvDescSize = dr.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

static void CreateRTVBackbuffers(DeviceResources& dr, AppResources& ar)
{
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;

	rtvHandle = ar.rtvHeap->GetCPUDescriptorHandleForHeapStart();

	// Create a RTV for each back buffer
	for (UINT n = 0; n < gFrameCount; n++)
	{
		ThrowIfFailed(dr.swapChain3->GetBuffer(n, IID_PPV_ARGS(&dr.backBuffer[n])), L"Failed to get swapchain buffer");

		dr.device->CreateRenderTargetView(dr.backBuffer[n], nullptr, rtvHandle);

#if NAME_D3D_RESOURCES
		if (n == 0) dr.backBuffer[n]->SetName(L"Back Buffer 0");
		else dr.backBuffer[n]->SetName(L"Back Buffer 1");
#endif

		rtvHandle.ptr += ar.rtvDescSize;
	}
}

static void CreateBuffer(
    DeviceResources& dr,
    UINT64 buffSize, 
    const D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT, 
    const D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_COMMON,
    const D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_NONE,
    UINT64 buffAlignment = 0,
    ID3D12Resource** ppResource = nullptr
    )
{
    D3D12_HEAP_PROPERTIES heapDesc = {};
	heapDesc.Type = heapType;
	heapDesc.CreationNodeMask = 1;
	heapDesc.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC resourceDesc = {};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Alignment = buffAlignment;
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.SampleDesc.Quality = 0;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resourceDesc.Width = buffSize;
	resourceDesc.Flags = resourceFlags;

    ThrowIfFailed(dr.device->CreateCommittedResource(&heapDesc, D3D12_HEAP_FLAG_NONE, &resourceDesc, resourceState, nullptr, IID_PPV_ARGS(ppResource)),L"Failed to create buff resource");
}

static void CreateVertexBuffer(DeviceResources& dr, AppResources& ar, Application& app)
{
    UINT64 buffSize = (UINT)app.mesh.vertices.size() * sizeof(Vertex);
    const D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_GENERIC_READ;
    const D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_NONE;
    UINT64 buffAlignment = 0;

    CreateBuffer(dr, buffSize, heapType, resourceState, resourceFlags, buffAlignment, &ar.vertexBuffer);

#if NAME_D3D_RESOURCES
	ar.vertexBuffer->SetName(L"Vertex Buffer");
#endif

    //copy data to mapped buffer 
    UINT8* vtxMappedPtr;
    D3D12_RANGE readRange = {};
    ThrowIfFailed(ar.vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&vtxMappedPtr)), L"Failed to map vtx buffer");

    memcpy(vtxMappedPtr, app.mesh.vertices.data(), buffSize);
    ar.vertexBuffer->Unmap(0, nullptr);

    //Init vertex buffer view 
    ar.vertexBufferView.BufferLocation = ar.vertexBuffer->GetGPUVirtualAddress();
	ar.vertexBufferView.StrideInBytes = sizeof(Vertex);
	ar.vertexBufferView.SizeInBytes = static_cast<UINT>(buffSize);
}

static void CreateIndexBuffer(DeviceResources& dr, AppResources& ar, Application& app)
{
    UINT64 buffSize = (UINT)app.mesh.indices.size() * sizeof(UINT);
    const D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_GENERIC_READ;
    const D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_NONE;
    UINT64 buffAlignment = 0;

    CreateBuffer(dr, buffSize, heapType, resourceState, resourceFlags, buffAlignment, &ar.indexBuffer);

    #if NAME_D3D_RESOURCES
	ar.indexBuffer->SetName(L"Index Buffer");
#endif

    //copy data to mapped buffer 
    UINT8* idxMappedPtr;
    D3D12_RANGE readRange = {};
    ThrowIfFailed(ar.indexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&idxMappedPtr)), L"Failed to map index buffer");

    memcpy(idxMappedPtr, app.mesh.indices.data(), buffSize);
    ar.indexBuffer->Unmap(0, nullptr);

    //Init vertex buffer view 
    ar.indexBufferView.BufferLocation = ar.indexBuffer->GetGPUVirtualAddress();
	ar.indexBufferView.SizeInBytes = static_cast<UINT>(buffSize);
	ar.indexBufferView.Format = DXGI_FORMAT_R32_UINT;
}

void UploadTexture(DeviceResources& dr, ID3D12Resource* destResource, ID3D12Resource* srcResource, const TextureInfo &texture)
{
	UINT8* pData;
	HRESULT hr = srcResource->Map(0, nullptr, reinterpret_cast<void**>(&pData));
	memcpy(pData, texture.pixels.data(), texture.width * texture.height * texture.stride);
	srcResource->Unmap(0, nullptr);

	D3D12_SUBRESOURCE_FOOTPRINT subresource = {};
	subresource.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	subresource.Width = texture.width;
	subresource.Height = texture.height;
	subresource.RowPitch = (texture.width * texture.stride);
	subresource.Depth = 1;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	footprint.Offset = texture.offset;
	footprint.Footprint = subresource;

	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = srcResource;
	source.PlacedFootprint = footprint;
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

	D3D12_TEXTURE_COPY_LOCATION destination = {};
	destination.pResource = destResource;
	destination.SubresourceIndex = 0;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

	// Copy the buffer resource from the upload heap to the texture resource on the default heap
	dr.cmdList[0]->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

	// Transition the texture to a shader resource
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = destResource;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	dr.cmdList[0]->ResourceBarrier(1, &barrier);
}

static void CreateTexture(DeviceResources& dr, AppResources& ar, Application& app)
{
    TextureInfo texture = Utility::LoadTexture(app.mesh.material.texturePath);
	app.mesh.material.textureResolution = static_cast<float>(texture.width);

	D3D12_RESOURCE_DESC textureDesc = {};
	textureDesc.Width = texture.width;
	textureDesc.Height = texture.height;
	textureDesc.MipLevels = 1;
	textureDesc.DepthOrArraySize = 1;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

	// Create the texture resource
	ThrowIfFailed(dr.device->CreateCommittedResource(&DefaultHeapProperties, D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ar.texture)), L"Failed to create texture resource");
#if NAME_D3D_RESOURCES
	ar.texture->SetName(L"Texture");
#endif

	D3D12_RESOURCE_DESC resourceDesc = {};
	resourceDesc.Width = (texture.width * texture.height * texture.stride);
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;

	// Create the upload heap : temporary place where texture data resides before it is copied to actual resource 
	ThrowIfFailed(dr.device->CreateCommittedResource(&UploadHeapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ar.textureUploadResource)), L"Failed to create upload heap");
#if NAME_D3D_RESOURCES
	ar.textureUploadResource->SetName(L"Texture Upload Buffer");
#endif

	// Upload the texture to the GPU
	UploadTexture(dr, ar.texture, ar.textureUploadResource, texture);
}

static void CreateConstBuffer(DeviceResources& dr, ID3D12Resource** buffer, UINT64 buffSize)
{
    const D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_GENERIC_READ;
    const D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_NONE;
    UINT64 buffAlignment = 0;

	CreateBuffer(dr, (buffSize + 255) & ~255, heapType, resourceState, resourceFlags, buffAlignment, buffer);
}

static void CreateSceneParamsConstBuffer(DeviceResources& dr, AppResources& ar)
{
	CreateConstBuffer(dr, &ar.sceneParamsCB, sizeof(SceneConstantBuffer));
#if NAME_D3D_RESOURCES
	ar.sceneParamsCB->SetName(L"Scene params constant buffer");
#endif

	ThrowIfFailed(ar.sceneParamsCB->Map(0, nullptr, reinterpret_cast<void**>(&ar.sceneParamsMappedPtr)), L"Failed to map scene params buffer");
	memcpy(ar.sceneParamsMappedPtr, &ar.sceneParams, sizeof(ar.sceneParams));
}

static void CreateCubeParamsConstBuffer(DeviceResources& dr, AppResources& ar, Application& app)
{
	CreateConstBuffer(dr, &ar.cubeParamsCB, sizeof(CubeConstantBuffer));
#if NAME_D3D_RESOURCES
	ar.cubeParamsCB->SetName(L"Material Constant Buffer");
#endif

	ThrowIfFailed(ar.cubeParamsCB->Map(0, nullptr, reinterpret_cast<void**>(&ar.cubeParamsMappedPtr)), L"Failed to map cube const buffer");

	memcpy(ar.cubeParamsMappedPtr, &ar.cubeParams, sizeof(ar.cubeParams));
}

/*
 ------------------------------Ray Tracing Related Function Definitions------------------------------------
*/
static void CreateBlas(DeviceResources& dr, AppResources& ar, Application& app, RayTracingResources& rt)
{
	D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc;
	geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geometryDesc.Triangles.VertexBuffer.StartAddress = ar.vertexBuffer->GetGPUVirtualAddress();
	geometryDesc.Triangles.VertexBuffer.StrideInBytes = ar.vertexBufferView.StrideInBytes;
	geometryDesc.Triangles.VertexCount = static_cast<UINT>(app.mesh.vertices.size());
	geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	geometryDesc.Triangles.IndexBuffer = ar.indexBuffer->GetGPUVirtualAddress();
	geometryDesc.Triangles.IndexFormat = ar.indexBufferView.Format;
	geometryDesc.Triangles.IndexCount = static_cast<UINT>(app.mesh.indices.size());
	geometryDesc.Triangles.Transform3x4 = 0;
	geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
	
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

	// Get the size requirements for the BLAS buffers
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ASInputs = {};
	ASInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	ASInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;	
	ASInputs.pGeometryDescs = &geometryDesc;
	ASInputs.NumDescs = 1;
	ASInputs.Flags = buildFlags;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ASPreBuildInfo = {};
	dr.device->GetRaytracingAccelerationStructurePrebuildInfo(&ASInputs, &ASPreBuildInfo);

	ASPreBuildInfo.ScratchDataSizeInBytes = ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, ASPreBuildInfo.ScratchDataSizeInBytes);
	ASPreBuildInfo.ResultDataMaxSizeInBytes = ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, ASPreBuildInfo.ResultDataMaxSizeInBytes);

	// Create the BLAS scratch buffer
	UINT64 buffSize = ASPreBuildInfo.ScratchDataSizeInBytes;
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    UINT64 buffAlignment = max(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

	CreateBuffer(dr, buffSize, heapType, resourceState, resourceFlags, buffAlignment, &rt.BLAS.pScratch);
#if NAME_D3D_RESOURCES
	rt.BLAS.pScratch->SetName(L"DXR BLAS Scratch");
#endif

	// Create the BLAS buffer
	buffSize = ASPreBuildInfo.ResultDataMaxSizeInBytes;
	resourceState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
	CreateBuffer(dr, buffSize, heapType, resourceState, resourceFlags, buffAlignment, &rt.BLAS.pResult);
#if NAME_D3D_RESOURCES
	rt.BLAS.pResult->SetName(L"DXR BLAS");
#endif

	// Describe and build the bottom level acceleration structure
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.Inputs = ASInputs;	
	buildDesc.ScratchAccelerationStructureData = rt.BLAS.pScratch->GetGPUVirtualAddress();
	buildDesc.DestAccelerationStructureData = rt.BLAS.pResult->GetGPUVirtualAddress();

	dr.cmdList[0]->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	// Wait for the BLAS build to complete
	D3D12_RESOURCE_BARRIER uavBarrier;
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = rt.BLAS.pResult;
	uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	dr.cmdList[0]->ResourceBarrier(1, &uavBarrier);
}

static void CreateTlas(DeviceResources& dr, AppResources& ar, Application& app, RayTracingResources& rt)
{
	D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
	instanceDesc.InstanceID = 0;
	instanceDesc.InstanceContributionToHitGroupIndex = 0;
	instanceDesc.InstanceMask = 0xFF;
	instanceDesc.Transform[0][0] = instanceDesc.Transform[1][1] = instanceDesc.Transform[2][2] = 1;
	instanceDesc.AccelerationStructure = rt.BLAS.pResult->GetGPUVirtualAddress();
	instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;

	UINT64 buffSize = sizeof(instanceDesc);
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_GENERIC_READ;
    D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_NONE;
	UINT64 buffAlignment = 0;

	CreateBuffer(dr, buffSize, heapType, resourceState, resourceFlags, buffAlignment, &rt.TLAS.pInstanceDesc);
#if NAME_D3D_RESOURCES
	rt.TLAS.pInstanceDesc->SetName(L"DXR TLAS Instance Descriptors");
#endif

	UINT8* pData;
	rt.TLAS.pInstanceDesc->Map(0, nullptr, (void**)&pData);
	memcpy(pData, &instanceDesc, sizeof(instanceDesc));
	rt.TLAS.pInstanceDesc->Unmap(0, nullptr);

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

	// Get the size requirements for the TLAS buffers
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ASInputs = {};
	ASInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	ASInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	ASInputs.InstanceDescs = rt.TLAS.pInstanceDesc->GetGPUVirtualAddress();
	ASInputs.NumDescs = 1;
	ASInputs.Flags = buildFlags;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ASPreBuildInfo = {};
	dr.device->GetRaytracingAccelerationStructurePrebuildInfo(&ASInputs, &ASPreBuildInfo);

	ASPreBuildInfo.ResultDataMaxSizeInBytes = ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, ASPreBuildInfo.ResultDataMaxSizeInBytes);
	ASPreBuildInfo.ScratchDataSizeInBytes = ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, ASPreBuildInfo.ScratchDataSizeInBytes);

	// Set TLAS size
	rt.tlasSize = ASPreBuildInfo.ResultDataMaxSizeInBytes;

	// Create TLAS scratch buffer
	buffSize = ASPreBuildInfo.ScratchDataSizeInBytes;
    heapType = D3D12_HEAP_TYPE_DEFAULT;
    resourceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	buffAlignment = max(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);;

	CreateBuffer(dr, buffSize, heapType, resourceState, resourceFlags, buffAlignment ,&rt.TLAS.pScratch);
#if NAME_D3D_RESOURCES
	rt.TLAS.pScratch->SetName(L"DXR TLAS Scratch");
#endif

	// Create the TLAS buffer
	buffSize = ASPreBuildInfo.ResultDataMaxSizeInBytes;
	resourceState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
	CreateBuffer(dr, buffSize, heapType, resourceState, resourceFlags, buffAlignment, &rt.TLAS.pResult);
#if NAME_D3D_RESOURCES
	rt.TLAS.pResult->SetName(L"DXR TLAS");
#endif

	// Describe and build the TLAS
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.Inputs = ASInputs;
	buildDesc.ScratchAccelerationStructureData = rt.TLAS.pScratch->GetGPUVirtualAddress();
	buildDesc.DestAccelerationStructureData = rt.TLAS.pResult->GetGPUVirtualAddress();

	dr.cmdList[0]->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	// Wait for the TLAS build to complete
	D3D12_RESOURCE_BARRIER uavBarrier;
	uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = rt.TLAS.pResult;
	uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	dr.cmdList[0]->ResourceBarrier(1, &uavBarrier);
}

static void CreateDXROutputTexture(DeviceResources& dr, AppResources& ar, RayTracingResources& rt)
{
	//Note texture format should match that of swapchain as later we will copy this texture to swapchain for presenting it hence, we init it as copy resource
	D3D12_RESOURCE_DESC desc = {};
	desc.DepthOrArraySize = 1;
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	desc.Width = gAppState.width;
	desc.Height = gAppState.height;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;

	ThrowIfFailed(dr.device->CreateCommittedResource(&DefaultHeapProperties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&dr.DXROutput)), L"Failed to creat DXR output texture");
#if NAME_D3D_RESOURCES
	dr.DXROutput->SetName(L"DXR Output Buffer");
#endif
}

static void CreateRootSignature(DeviceResources& dr, D3D12_ROOT_SIGNATURE_DESC& rootDesc, ID3D12RootSignature** outRootSig)
{
	ID3DBlob* serializedSig;
	ID3DBlob* error;
	ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedSig, &error), L"Failed to serialize root signature");
	ThrowIfFailed(dr.device->CreateRootSignature(0, serializedSig->GetBufferPointer(), serializedSig->GetBufferSize(), IID_PPV_ARGS(outRootSig)), L"Failed to create root signature");

	SAFE_RELEASE(serializedSig);
	SAFE_RELEASE(error);
}

// This heap holds desc for resources of ray tracing
static void CreateRTDescriptorHeap(DeviceResources& dr, AppResources& ar, RayTracingResources& rt, Application& app)
{
	// Describe the CBV/SRV/UAV heap
	// Need 7 entries:
	// 1 CBV for the ViewParamsCB
	// 1 CBV for the matParamsCB
	// 1 UAV for the RT output
	// 1 SRV for the Scene BVH
	// 1 SRV for the index buffer
	// 1 SRV for the vertex buffer
	// 1 SRV for the texture

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = 7;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	ThrowIfFailed(dr.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&ar.descriptorHeap)), L"Failed to create RT descriptor heap");

	D3D12_CPU_DESCRIPTOR_HANDLE handle = ar.descriptorHeap->GetCPUDescriptorHandleForHeapStart();
	UINT handleIncrement = dr.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
#if NAME_D3D_RESOURCES
	ar.descriptorHeap->SetName(L"DXR Descriptor Heap");
#endif

	// Create the Scene parameters CBV
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.SizeInBytes = ALIGN(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, sizeof(ar.sceneParams));
	cbvDesc.BufferLocation = ar.sceneParamsCB->GetGPUVirtualAddress();

	dr.device->CreateConstantBufferView(&cbvDesc, handle);

	// Create the MaterialParams CBV
	cbvDesc.SizeInBytes = ALIGN(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, sizeof(ar.cubeParams));
	cbvDesc.BufferLocation = ar.cubeParamsCB->GetGPUVirtualAddress();

	handle.ptr += handleIncrement;
	dr.device->CreateConstantBufferView(&cbvDesc, handle);

	// Create the DXR output buffer UAV
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

	handle.ptr += handleIncrement;
	dr.device->CreateUnorderedAccessView(dr.DXROutput, nullptr, &uavDesc, handle);

	// Create the DXR Top Level Acceleration Structure SRV
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.RaytracingAccelerationStructure.Location = rt.TLAS.pResult->GetGPUVirtualAddress();

	handle.ptr += handleIncrement;
	dr.device->CreateShaderResourceView(nullptr, &srvDesc, handle);

	D3D12_SHADER_RESOURCE_VIEW_DESC indexSRVDesc;
	indexSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	indexSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	indexSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	indexSRVDesc.Buffer.StructureByteStride = 0;
	indexSRVDesc.Buffer.FirstElement = 0;
	indexSRVDesc.Buffer.NumElements = (static_cast<UINT>(app.mesh.indices.size()) * sizeof(UINT)) / sizeof(float);
	indexSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	handle.ptr += handleIncrement;
	dr.device->CreateShaderResourceView(ar.indexBuffer, &indexSRVDesc, handle);

	// Create the vertex buffer SRV
	D3D12_SHADER_RESOURCE_VIEW_DESC vertexSRVDesc;
	vertexSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	vertexSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	vertexSRVDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	vertexSRVDesc.Buffer.StructureByteStride = 0;
	vertexSRVDesc.Buffer.FirstElement = 0;
	vertexSRVDesc.Buffer.NumElements = (static_cast<UINT>(app.mesh.vertices.size()) * sizeof(Vertex)) / sizeof(float);
	vertexSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	handle.ptr += handleIncrement;
	dr.device->CreateShaderResourceView(ar.vertexBuffer, &vertexSRVDesc, handle);

	// Create the material texture SRV
	D3D12_SHADER_RESOURCE_VIEW_DESC textureSRVDesc = {};
	textureSRVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	textureSRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	textureSRVDesc.Texture2D.MipLevels = 1;
	textureSRVDesc.Texture2D.MostDetailedMip = 0;
	textureSRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	handle.ptr += handleIncrement;
	dr.device->CreateShaderResourceView(ar.texture, &textureSRVDesc, handle);
}

static void CreateRayGenProgram(DeviceResources& dr, RayTracingResources& rt, Application& app)
{
	rt.rayGenProg = RtProgram(D3D12ShaderInfo(L"shaders\\RayGen.hlsl", L"", L"lib_6_3"));
	rt.rayGenProg.CompileProgram(app.shaderCompiler);

	/*
	* Describing local root signature
	* Global root signature is generally visible to all shaders vs/ps 
	* Local root signature is very specific when we want to give info about resources accessible only to ray tracing shaders.
	* Must match the way desc heap created
	*    1 CBV for the ViewParamsCB
		 1 CBV for the matParamsCB
		 1 UAV for the RT output
		 1 SRV for the Scene BVH
		 1 SRV for the index buffer
		 1 SRV for the vertex buffer
		 1 SRV for the texture
	*/

	D3D12_DESCRIPTOR_RANGE ranges[3];

	ranges[0].BaseShaderRegister = 0;
	ranges[0].NumDescriptors = 2;
	ranges[0].RegisterSpace = 0;
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;

	ranges[1].BaseShaderRegister = 0;
	ranges[1].NumDescriptors = 1;
	ranges[1].RegisterSpace = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].OffsetInDescriptorsFromTableStart = 2;

	ranges[2].BaseShaderRegister = 0;
	ranges[2].NumDescriptors = 4;
	ranges[2].RegisterSpace = 0;
	ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[2].OffsetInDescriptorsFromTableStart = 3;

	D3D12_ROOT_PARAMETER param0 = {};
	param0.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	param0.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	param0.DescriptorTable.NumDescriptorRanges = _countof(ranges);
	param0.DescriptorTable.pDescriptorRanges = ranges;

	D3D12_ROOT_PARAMETER rootParams[1] = { param0 };

	D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
	rootDesc.NumParameters = _countof(rootParams);
	rootDesc.pParameters = rootParams;
	rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

	// Create the root signature
	CreateRootSignature(dr, rootDesc, &rt.rayGenProg.pRootSignature);
#if NAME_D3D_RESOURCES
	rt.rayGenProg.pRootSignature->SetName(L"DXR RGS Root Signature");
#endif
}

static void CreateMissProgram(DeviceResources& dr, RayTracingResources& rt, Application& app)
{
	rt.missProg = RtProgram(D3D12ShaderInfo(L"shaders\\Miss.hlsl", L"", L"lib_6_3"));
	rt.missProg.CompileProgram(app.shaderCompiler);
}

static void CreateClosestHitProgram(DeviceResources& dr, RayTracingResources& rt, Application& app)
{
	rt.hitProg = HitProgram(L"Hit");
	rt.hitProg.chs = RtProgram(D3D12ShaderInfo(L"shaders\\ClosestHit.hlsl", L"", L"lib_6_3"));
	rt.hitProg.chs.CompileProgram(app.shaderCompiler);
}

static void CreateRTPipelineStateObject(DeviceResources& dr, RayTracingResources& rt)
{
	// Need 10 subobjects:
	// 1 for RGS program
	// 1 for Miss program
	// 1 for CHS program
	// 1 for Hit Group
	// 2 for RayGen Root Signature (root-signature and association)
	// 2 for Shader Config (config and association)
	// 1 for Global Root Signature for us this is null for now
	// 1 for Pipeline Config	
	UINT index = 0;
	vector<D3D12_STATE_SUBOBJECT> subobjects;
	subobjects.resize(10);

	//Ray Gen Subobject
	{
		D3D12_EXPORT_DESC rgsExportDesc = {};
		rgsExportDesc.Name = L"RayGen_12";
		rgsExportDesc.ExportToRename = L"RayGen";
		rgsExportDesc.Flags = D3D12_EXPORT_FLAG_NONE;

		D3D12_DXIL_LIBRARY_DESC	rgsLibDesc = {};
		rgsLibDesc.DXILLibrary.BytecodeLength = rt.rayGenProg.blob->GetBufferSize();
		rgsLibDesc.DXILLibrary.pShaderBytecode = rt.rayGenProg.blob->GetBufferPointer();
		rgsLibDesc.NumExports = 1;
		rgsLibDesc.pExports = &rgsExportDesc;

		D3D12_STATE_SUBOBJECT rgs = {};
		rgs.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
		rgs.pDesc = &rgsLibDesc;

		subobjects[index++] = rgs;
	}

	//Miss program Subobject
	{
		D3D12_EXPORT_DESC msExportDesc = {};
		msExportDesc.Name = L"Miss_5";
		msExportDesc.ExportToRename = L"Miss";
		msExportDesc.Flags = D3D12_EXPORT_FLAG_NONE;

		D3D12_DXIL_LIBRARY_DESC	msLibDesc = {};
		msLibDesc.DXILLibrary.BytecodeLength = rt.missProg.blob->GetBufferSize();
		msLibDesc.DXILLibrary.pShaderBytecode = rt.missProg.blob->GetBufferPointer();
		msLibDesc.NumExports = 1;
		msLibDesc.pExports = &msExportDesc;

		D3D12_STATE_SUBOBJECT ms = {};
		ms.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
		ms.pDesc = &msLibDesc;

		subobjects[index++] = ms;
	}

	//Closest hit program subobject
	{
		D3D12_EXPORT_DESC chsExportDesc = {};
		chsExportDesc.Name = L"ClosestHit_76";
		chsExportDesc.ExportToRename = L"ClosestHit";
		chsExportDesc.Flags = D3D12_EXPORT_FLAG_NONE;

		D3D12_DXIL_LIBRARY_DESC	chsLibDesc = {};
		chsLibDesc.DXILLibrary.BytecodeLength = rt.hitProg.chs.blob->GetBufferSize();
		chsLibDesc.DXILLibrary.pShaderBytecode = rt.hitProg.chs.blob->GetBufferPointer();
		chsLibDesc.NumExports = 1;
		chsLibDesc.pExports = &chsExportDesc;

		D3D12_STATE_SUBOBJECT chs = {};
		chs.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
		chs.pDesc = &chsLibDesc;

		subobjects[index++] = chs;
	}

	//Hit Group Subobject
	{
		D3D12_HIT_GROUP_DESC hitGroupDesc = {};
		hitGroupDesc.ClosestHitShaderImport = L"ClosestHit_76";
		hitGroupDesc.HitGroupExport = L"HitGroup";

		D3D12_STATE_SUBOBJECT hitGroup = {};
		hitGroup.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
		hitGroup.pDesc = &hitGroupDesc;

		subobjects[index++] = hitGroup;
	}

	//Shader payload and Association
	{
		
		D3D12_RAYTRACING_SHADER_CONFIG shaderDesc = {};
		shaderDesc.MaxPayloadSizeInBytes = sizeof(XMFLOAT4);	// RGB + HitT
		shaderDesc.MaxAttributeSizeInBytes = D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES;

		D3D12_STATE_SUBOBJECT shaderConfigObject = {};
		shaderConfigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
		shaderConfigObject.pDesc = &shaderDesc;
		subobjects[index++] = shaderConfigObject;

		const WCHAR* shaderExports[] = { L"RayGen_12", L"Miss_5", L"HitGroup" };

		// Add a state subobject for the association between shaders and the payload
		D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION shaderPayloadAssociation = {};
		shaderPayloadAssociation.NumExports = _countof(shaderExports);
		shaderPayloadAssociation.pExports = shaderExports;
		shaderPayloadAssociation.pSubobjectToAssociate = &subobjects[(index - 1)];

		D3D12_STATE_SUBOBJECT shaderPayloadAssociationObject = {};
		shaderPayloadAssociationObject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
		shaderPayloadAssociationObject.pDesc = &shaderPayloadAssociation;

		subobjects[index++] = shaderPayloadAssociationObject;
	}

	//Local root signature  and association subobject
	{
		D3D12_STATE_SUBOBJECT rayGenRootSigObject = {};
		rayGenRootSigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
		rayGenRootSigObject.pDesc = &rt.rayGenProg.pRootSignature;
		subobjects[index++] = rayGenRootSigObject;

		const WCHAR* rootSigExports[] = { L"RayGen_12", L"HitGroup", L"Miss_5" };
		D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION rayGenShaderRootSigAssociation = {};
		rayGenShaderRootSigAssociation.NumExports = _countof(rootSigExports);
		rayGenShaderRootSigAssociation.pExports = rootSigExports;
		rayGenShaderRootSigAssociation.pSubobjectToAssociate = &subobjects[(index - 1)];

		D3D12_STATE_SUBOBJECT rayGenShaderRootSigAssociationObject = {};
		rayGenShaderRootSigAssociationObject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
		rayGenShaderRootSigAssociationObject.pDesc = &rayGenShaderRootSigAssociation;
		subobjects[index++] = rayGenShaderRootSigAssociationObject;
	}

	//Global root Signature
	{
		D3D12_STATE_SUBOBJECT globalRootSig;
		globalRootSig.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
		globalRootSig.pDesc = &rt.missProg.pRootSignature;
		subobjects[index++] = globalRootSig;
	}

	//Ray tracing config max depth etc
	{
		D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
		pipelineConfig.MaxTraceRecursionDepth = 1;

		D3D12_STATE_SUBOBJECT pipelineConfigObject = {};
		pipelineConfigObject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
		pipelineConfigObject.pDesc = &pipelineConfig;
		subobjects[index++] = pipelineConfigObject;
	}

	//Create RTPSO
	{
		D3D12_STATE_OBJECT_DESC pipelineDesc = {};
		pipelineDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
		pipelineDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
		pipelineDesc.pSubobjects = subobjects.data();

		ThrowIfFailed(dr.device->CreateStateObject(&pipelineDesc, IID_PPV_ARGS(&rt.rtpso)), L"Failed to create state object");
#if NAME_D3D_RESOURCES
		rt.rtpso->SetName(L"DXR Pipeline State Object");
#endif

		ThrowIfFailed(rt.rtpso->QueryInterface(IID_PPV_ARGS(&rt.rtpsoInfo)), L"Failed to get rtpso info");
	}
}

//We will provide desc heap address as argument to DispatchRays directly via shader table
static void CreateShaderTable(DeviceResources& dr, AppResources& ar, RayTracingResources& rt)
{
	/*
	The Shader Table layout is as follows:
		Entry 0 - Ray Generation shader
		Entry 1 - Miss shader
		Entry 2 - Closest Hit shader
	All shader records in the Shader Table must have the same size, so shader record size will be based on the largest required entry.
	The ray generation program requires the largest entry: 
		32 bytes - D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES 
	  +  8 bytes - a CBV/SRV/UAV descriptor table pointer (64-bits)
	  = 40 bytes ->> aligns to 64 bytes
	The entry size must be aligned up to D3D12_RAYTRACING_SHADER_BINDING_TABLE_RECORD_BYTE_ALIGNMENT
	*/

	uint32_t shaderIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
	uint32_t shaderTableSize = 0;

	rt.shaderTableRecordSize = shaderIdSize;
	rt.shaderTableRecordSize += 8;							// CBV/SRV/UAV descriptor table
	rt.shaderTableRecordSize = ALIGN(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT, rt.shaderTableRecordSize);

	shaderTableSize = (rt.shaderTableRecordSize * 3);		// 3 shader records in the table
	shaderTableSize = ALIGN(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, shaderTableSize);


	UINT64 buffSize = shaderTableSize;
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_GENERIC_READ;
    D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_NONE;
	UINT64 buffAlignment = 0;
	CreateBuffer(dr, buffSize, heapType, resourceState, resourceFlags, buffAlignment, &rt.shaderTable);
#if NAME_D3D_RESOURCES
	rt.shaderTable->SetName(L"DXR Shader Table");
#endif

	uint8_t* pData;
	ThrowIfFailed(rt.shaderTable->Map(0, nullptr, (void**)&pData), L"Failed to map shader table buffer");

	//Record 0 : Ray gen Id and heap pointer
	memcpy(pData, rt.rtpsoInfo->GetShaderIdentifier(L"RayGen_12"), shaderIdSize);
	*reinterpret_cast<D3D12_GPU_DESCRIPTOR_HANDLE*>(pData + shaderIdSize) = ar.descriptorHeap->GetGPUDescriptorHandleForHeapStart();
	pData += rt.shaderTableRecordSize;
	
	//Record 1 : Miss shader id
	memcpy(pData, rt.rtpsoInfo->GetShaderIdentifier(L"Miss_5"), shaderIdSize);
	pData += rt.shaderTableRecordSize;

	//Record 2 : HitGroup id and heap pointer
	memcpy(pData, rt.rtpsoInfo->GetShaderIdentifier(L"HitGroup"), shaderIdSize);
	*reinterpret_cast<D3D12_GPU_DESCRIPTOR_HANDLE*>(pData + shaderIdSize) = ar.descriptorHeap->GetGPUDescriptorHandleForHeapStart();

	rt.shaderTable->Unmap(0, nullptr);
}

static void SubmitCommandList(DeviceResources& dr)
{
	dr.cmdList[0]->Close();
	ID3D12CommandList* pGraphicsList = { dr.cmdList[0]};
	dr.cmdQueue->ExecuteCommandLists(1, &pGraphicsList);
	dr.fenceValues[dr.frameIndex]++;
	dr.cmdQueue->Signal(dr.fence, dr.fenceValues[dr.frameIndex]);
}

static void Present(DeviceResources& dr)
{
	ThrowIfFailed(dr.swapChain3->Present(gAppState.vsync, 0), L"Failed to present");
}

static void BuildCommandList(DeviceResources& dr, AppResources& ar, RayTracingResources& rt)
{
	D3D12_RESOURCE_BARRIER OutputBarriers[2] = {};
	D3D12_RESOURCE_BARRIER CounterBarriers[2] = {};
	D3D12_RESOURCE_BARRIER UAVBarriers[3] = {};

	// Transition the back buffer to a copy destination
	OutputBarriers[0].Transition.pResource = dr.backBuffer[dr.frameIndex];
	OutputBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	OutputBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	OutputBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	// Transition the DXR output buffer to a copy source
	OutputBarriers[1].Transition.pResource = dr.DXROutput;
	OutputBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	OutputBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	OutputBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	// Wait for the transitions to complete
	dr.cmdList[0]->ResourceBarrier(2, OutputBarriers);

	// Set the UAV/SRV/CBV and sampler heaps
	ID3D12DescriptorHeap* ppHeaps[] = { ar.descriptorHeap };
	dr.cmdList[0]->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

	// Dispatch rays
	D3D12_DISPATCH_RAYS_DESC desc = {};
	desc.RayGenerationShaderRecord.StartAddress = rt.shaderTable->GetGPUVirtualAddress();
	desc.RayGenerationShaderRecord.SizeInBytes = rt.shaderTableRecordSize;

	desc.MissShaderTable.StartAddress = rt.shaderTable->GetGPUVirtualAddress() + rt.shaderTableRecordSize;
	desc.MissShaderTable.SizeInBytes = rt.shaderTableRecordSize;		// Only a single Miss program entry
	desc.MissShaderTable.StrideInBytes = rt.shaderTableRecordSize;

	desc.HitGroupTable.StartAddress = rt.shaderTable->GetGPUVirtualAddress() + (rt.shaderTableRecordSize * 2);
	desc.HitGroupTable.SizeInBytes = rt.shaderTableRecordSize;			// Only a single Hit program entry
	desc.HitGroupTable.StrideInBytes = rt.shaderTableRecordSize;

	desc.Width = gAppState.width;
	desc.Height = gAppState.height;
	desc.Depth = 1;

	dr.cmdList[0]->SetPipelineState1(rt.rtpso);
	dr.cmdList[0]->DispatchRays(&desc);

	// Transition DXR output to a copy source
	OutputBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	OutputBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	
	// Wait for the transitions to complete
	dr.cmdList[0]->ResourceBarrier(1, &OutputBarriers[1]);

	// Copy the DXR output to the back buffer
	dr.cmdList[0]->CopyResource(dr.backBuffer[dr.frameIndex], dr.DXROutput);

	// Transition back buffer to present
	OutputBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	OutputBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	
	// Wait for the transitions to complete
	dr.cmdList[0]->ResourceBarrier(1, &OutputBarriers[0]);

	// Submit the command list and wait for the GPU to idle
	SubmitCommandList(dr);
	WaitForGPU(dr);
}

/*
 ------------------------------Function Definitions------------------------------------
*/

void Application::InitializeSceneParams()
{
	auto frameIndex = dr.frameIndex;

	//Materials
	{
		ar.cubeParams.albedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	}

	// Setup camera.
    {
        ar.eye = { 0.0f, 2.0f, -5.0f, 1.0f };
        ar.at = { 0.0f, 0.0f, 0.0f, 1.0f };
        XMVECTOR right = { 1.0f, 0.0f, 0.0f, 0.0f };

        XMVECTOR direction = XMVector4Normalize(ar.at - ar.eye);
        ar.up = XMVector3Normalize(XMVector3Cross(direction, right));

        // Rotate camera around Y axis.
        XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(45.0f));
        ar.eye = XMVector3Transform(ar.eye, rotate);
        ar.up = XMVector3Transform(ar.up, rotate);
        
        UpdateCameraMatrices();
    }
}

void Application::UpdateCameraMatrices()
{
	auto frameIndex = dr.frameIndex;
	ar.sceneParams[frameIndex].cameraPosition = ar.eye;
	float fovAngleY = 45.0f;
	//ToDo handle window resize
	float aspectRatio = static_cast<float>(gAppState.width) / static_cast<float>(gAppState.height);
	float nearPlane = 1.0f;
	float farPlane = 125.0f;
	XMMATRIX view = XMMatrixLookAtLH(ar.eye, ar.at, ar.up);
	XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(fovAngleY), aspectRatio, nearPlane, farPlane);
	XMMATRIX viewProj = view * proj;

	ar.sceneParams[frameIndex].projectionToWorld = XMMatrixInverse(nullptr, viewProj);
}

void Application::Init(UINT width, UINT height, BOOL vsync, std::string meshFilePath)
{
    gAppState.width = width;
    gAppState.height = height;
    gAppState.vsync = vsync;

    Mesh::LoadModel(meshFilePath, mesh);
    InitShaderCompiler();

	//Initialise device resources
	CreateDevice(dr);
	CreateCommandQueue(dr);
	CreateCommandAllocator(dr);
	CreateFence(dr);
	CreateSwapChain(dr, hwnd);
	CreateCommandList(dr);
	ResetCommandList(dr);

	//Create App specific resources
	CreateRTVDescHeap(dr, ar);
	CreateRTVBackbuffers(dr, ar);
	CreateVertexBuffer(dr, ar, *this);
	CreateIndexBuffer(dr, ar, *this);
	CreateTexture(dr, ar, *this);
	CreateSceneParamsConstBuffer(dr, ar);
	CreateCubeParamsConstBuffer(dr, ar, *this);

	//Create ray tracing specific resources 
	CreateBlas(dr, ar, *this, rt);
	CreateTlas(dr, ar, *this, rt);
	CreateDXROutputTexture(dr, ar, rt);
	CreateRTDescriptorHeap(dr, ar, rt, *this);
	CreateRayGenProgram(dr, rt, *this);
	CreateMissProgram(dr, rt, *this);
	CreateClosestHitProgram(dr, rt, *this);
	CreateRTPipelineStateObject(dr, rt);
	CreateShaderTable(dr, ar, rt);

	//ToDo handle multiple command list submission
	dr.cmdList[0]->Close();
	ID3D12CommandList* pCommandLists = { dr.cmdList[0] };
	dr.cmdQueue->ExecuteCommandLists(1, &pCommandLists);

	WaitForGPU(dr);
	ResetCommandList(dr);
}

void Application::Render()
{
	BuildCommandList(dr, ar, rt);
	Present(dr);
	MoveToNextFrame(dr);
	ResetCommandList(dr);
}

void Application::Update()
{
	
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) 
{
	PAINTSTRUCT ps;
    switch( message ) 
	{
        case WM_PAINT:
            BeginPaint( hWnd, &ps );
            EndPaint( hWnd, &ps );
            break;
		case WM_KEYUP:
			if (wParam == VK_ESCAPE) PostQuitMessage(0);
			break;
        case WM_DESTROY:
            PostQuitMessage( 0 );
            break;
        default:
            return DefWindowProc( hWnd, message, wParam, lParam );
    }
    return 0;
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    LocalFree(argv);

    //Create window 
    WNDCLASSEX windowClass = { 0 };
    windowClass.cbSize = sizeof(WNDCLASSEX);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = hInstance;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.lpszClassName = L"DXSampleClass";
    RegisterClassEx(&windowClass);

    UINT width = 1280;
    UINT height = 720;
    BOOL vsync = TRUE;

	Application app;
	

    RECT windowRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    app.hwnd = CreateWindow(
        windowClass.lpszClassName,
		L"DXR Engine",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr,        // We have no parent window.
        nullptr,        // We aren't using menus.
        hInstance,
        nullptr);

    //OnInit 
    app.Init(width, height, vsync,"Meshes\\quad.obj");

    ShowWindow(app.hwnd, nCmdShow);

    //Render/Message Loop
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        app.Update();
        app.Render();
    }

	DestroyResources(app.dr);
    return static_cast<char>(msg.wParam);
}
