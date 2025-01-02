#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN			// Exclude rarely-used items from Windows headers.
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
#include "tiny_obj_loader.h""

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

/*
 ------------------------------Common Types------------------------------------
*/
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

struct TextureInfo
{
	std::vector<UINT8> pixels;
	int width = 0;
	int height = 0;
	int stride = 0;
	int offset = 0;
};

struct D3D12ShaderCompilerInfo 
{
	dxc::DxcDllSupport		DxcDllHelper;
	IDxcCompiler*			compiler = nullptr;
	IDxcLibrary*			library = nullptr;
};

struct Material 
{
	std::string name = "defaultMaterial";
	std::string texturePath = "";
	float  textureResolution = 512;
};

struct Vertex
{
	DirectX::XMFLOAT3 position;
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

struct Mesh
{
	std::vector<Vertex> vertices;
	std::vector<UINT> indices;
    Material material;

	static void LoadModel(string filepath, Mesh& model)
	{
        Material material;
		tinyobj::attrib_t attrib;
		std::vector<tinyobj::shape_t> shapes;
		std::vector<tinyobj::material_t> materials;
		std::string err;
        std::string warn;

		// Load the OBJ and MTL files
		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filepath.c_str(), "materials\\"))
		{
			throw std::runtime_error(err);
		}

		// Get the first material
		// Only support a single material right now
		material.name = materials[0].name;
		material.texturePath = materials[0].diffuse_texname;

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
};

/*
 ------------------------------Application settings------------------------------------
*/

struct Application
{
    Application() = default;
    ~Application() = default;

    void Init(UINT width, UINT height, BOOL vsync, std::string meshFilePath);
    void Render();
    void Update();
	
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
};

/*
 ------------------------------Function Definitions------------------------------------
*/

void Application::Init(UINT width, UINT height, BOOL vsync, std::string meshFilePath)
{
    gAppState.width = width;
    gAppState.height = height;
    gAppState.vsync = vsync;

    Mesh::LoadModel(meshFilePath, mesh);

    InitShaderCompiler();
}

void Application::Render()
{

}

void Application::Update()
{

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
	ThrowIfFailed(dr.cmdList[dr.frameIndex]->Reset(dr.cmdAllocator[dr.frameIndex], nullptr), L"Failed to reset command list");
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

static void CreateRtvDescHeap(DeviceResources& dr, AppResources& ar)
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
    ID3D12Resource** ppResource
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
	dr.cmdList[dr.frameIndex]->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

	// Transition the texture to a shader resource
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = destResource;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	dr.cmdList[dr.frameIndex]->ResourceBarrier(1, &barrier);
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

    Application app;
    UINT width = 1280;
    UINT height = 720;
    BOOL vsync = TRUE;

    RECT windowRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    app.hwnd = CreateWindow(
        windowClass.lpszClassName,
        g_appName.c_str(),
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
    app.Init(width, height, vsync);

    ShowWindow(g_hwnd, nCmdShow);

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

    return static_cast<char>(msg.wParam);
}
