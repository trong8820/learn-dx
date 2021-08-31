#include "entry.h"

#include <fstream>

#include <spng.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <Windows.h>

#include <winrt/base.h>

#include <dxgi1_4.h>
#include <d3d12.h>

#include <d3dcompiler.h>

#if defined(_DEBUG)
#include <dxgidebug.h>
#endif

#include <DirectXMath.h>
#include <DirectXColors.h>

#include "d3dx12.h"

const int MAX_FRAMES_IN_FLIGHT = 2;

const char* vertexShaderSource = R"(
static float4 gl_Position;
static float2 vTexCoord;
static float2 aTexCoord;
static float4 vColor;
static float4 aColor;
static float2 aPos;

struct SPIRV_Cross_Input
{
	float2 aPos : POSITION0;
	float2 aTexCoord : TEXCOORD0;
	float4 aColor : COLOR0;
};

struct SPIRV_Cross_Output
{
	float2 vTexCoord : TEXCOORD0;
	float4 vColor : COLOR0;
	float4 gl_Position : SV_Position;
};

void vert_main()
{
	vTexCoord = aTexCoord;
	vColor = aColor;
	gl_Position = float4(aPos, 0.0f, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
	aTexCoord = stage_input.aTexCoord;
	aColor = stage_input.aColor;
	aPos = stage_input.aPos;
	vert_main();
	SPIRV_Cross_Output stage_output;
	stage_output.gl_Position = gl_Position;
	stage_output.vTexCoord = vTexCoord;
	stage_output.vColor = vColor;
	return stage_output;
}
)";

const char* fragmentShaderSource = R"(
Texture2D<float4> texture0 : register(t0);
SamplerState _texture0_sampler : register(s0);

static float4 FragColor;
static float4 vColor;
static float2 vTexCoord;

struct SPIRV_Cross_Input
{
	float2 vTexCoord : TEXCOORD0;
	float4 vColor : COLOR0;
};

struct SPIRV_Cross_Output
{
	float4 FragColor : SV_Target0;
};

void frag_main()
{
	FragColor = vColor * texture0.Sample(_texture0_sampler, vTexCoord);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
	vColor = stage_input.vColor;
	vTexCoord = stage_input.vTexCoord;
	frag_main();
	SPIRV_Cross_Output stage_output;
	stage_output.FragColor = FragColor;
	return stage_output;
}
)";

HWND g_window;

winrt::com_ptr<IDXGIFactory4> g_factory;

winrt::com_ptr<ID3D12Device>				g_device;
winrt::com_ptr<ID3D12CommandQueue>			g_commandQueue;
winrt::com_ptr<ID3D12DescriptorHeap>		g_rtvDescriptorHeap;
winrt::com_ptr<ID3D12DescriptorHeap>		g_dsvDescriptorHeap;

winrt::com_ptr<ID3D12CommandAllocator>		g_commandAllocators[MAX_FRAMES_IN_FLIGHT];
winrt::com_ptr<ID3D12GraphicsCommandList>   g_commandList;

// Rendering resources
winrt::com_ptr<IDXGISwapChain3>				g_swapChain;
winrt::com_ptr<ID3D12Resource>				g_renderTargets[MAX_FRAMES_IN_FLIGHT];
winrt::com_ptr<ID3D12Resource>				g_depthStencil;

winrt::com_ptr<ID3D12Fence>					g_fence;
UINT64                                      g_fenceValues[MAX_FRAMES_IN_FLIGHT];
winrt::handle								g_fenceEvent;

// Triangle
winrt::com_ptr<ID3D12RootSignature>			g_rootSignature;
winrt::com_ptr<ID3D12PipelineState>			g_pipeline;
winrt::com_ptr<ID3D12Resource>				g_vertexBuffer;
D3D12_VERTEX_BUFFER_VIEW					g_vertexBufferView;
winrt::com_ptr<ID3D12Resource>				g_indexBuffer;
D3D12_INDEX_BUFFER_VIEW						g_indexBufferView;

// Other
UINT g_rtvDescriptorSize;

UINT g_backBufferIndex = 0;

// Texture
winrt::com_ptr<ID3D12Resource>				g_texture;
winrt::com_ptr<ID3D12DescriptorHeap>		g_srvDescriptorHeap;

std::vector<unsigned char> g_pixels;
UINT g_textureWidth;
UINT g_textureHeight;

void onDeviceLost();

void waitForGpu() noexcept
{
	if (g_commandQueue && g_fence && g_fenceEvent)
	{
		UINT64 fenceValue = g_fenceValues[g_backBufferIndex];
		if (SUCCEEDED(g_commandQueue->Signal(g_fence.get(), fenceValue)))
		{
			if (SUCCEEDED(g_fence->SetEventOnCompletion(fenceValue, g_fenceEvent.get())))
			{
				WaitForSingleObjectEx(g_fenceEvent.get(), INFINITE, FALSE);

				g_fenceValues[g_backBufferIndex]++;
			}
		}
	}
}

void createDevice()
{
	DWORD dxgiFactoryFlags = 0;
#if defined(_DEBUG)
	{
		winrt::com_ptr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_ID3D12Debug, debugController.put_void())))
		{
			debugController->EnableDebugLayer();
		}

		winrt::com_ptr<IDXGIInfoQueue> dxgiInfoQueue;
		if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_IDXGIInfoQueue, dxgiInfoQueue.put_void())))
		{
			dxgiFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;

			dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, true);
			dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);
		}
	}
#endif

	winrt::check_hresult(CreateDXGIFactory2(dxgiFactoryFlags, IID_IDXGIFactory4, g_factory.put_void()));

	winrt::com_ptr<IDXGIAdapter1> adapter;
	for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != g_factory->EnumAdapters1(adapterIndex, adapter.put()); adapterIndex++)
	{
		DXGI_ADAPTER_DESC1 adapterDesc;
		winrt::check_hresult(adapter->GetDesc1(&adapterDesc));

		if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
		if (SUCCEEDED(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, g_device.put_void()))) break;
	}

	/*
	// Check shader model 6 support
	D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_SHADER_MODEL_6_0 };
	if ((FAILED(g_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) || (shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_0)))
	{
		throw std::runtime_error("Shader Model 6.0 is not supported!");
	}
	*/

	// Root signature
	CD3DX12_DESCRIPTOR_RANGE range;
	range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

	CD3DX12_ROOT_PARAMETER rootParameter;
	rootParameter.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);

	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	sampler.MipLODBias = 0;
	sampler.MaxAnisotropy = 0;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	sampler.MinLOD = 0.0f;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init(1, &rootParameter, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	winrt::com_ptr<ID3DBlob> signature;
	winrt::check_hresult(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), nullptr));
	winrt::check_hresult(g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_ID3D12RootSignature, g_rootSignature.put_void()));

	// Graphics pipeline
	//std::vector<char> vertexShader;
	//std::vector<char> pixelShader;

#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = 0;
#endif

	winrt::com_ptr<ID3DBlob> vertexShader;
	winrt::com_ptr<ID3DBlob> pixelShader;
	winrt::com_ptr<ID3DBlob> shaderError;
	winrt::check_hresult(D3DCompile(vertexShaderSource, std::strlen(vertexShaderSource), nullptr, nullptr, nullptr, "main", "vs_5_0", compileFlags, 0, vertexShader.put(), nullptr));
	winrt::check_hresult(D3DCompile(fragmentShaderSource, std::strlen(fragmentShaderSource), nullptr, nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, pixelShader.put(), nullptr));

	/*
	{
		std::ifstream ifs("data/basic_triangle.vert.cso", std::ios::binary | std::ios::ate);
		std::streamsize size = ifs.tellg();
		ifs.seekg(0, std::ios::beg);

		vertexShader.resize(size);
		if (ifs.read(vertexShader.data(), size))
		{
		}
		ifs.close();
	}
	{
		std::ifstream ifs("data/basic_triangle.frag.cso", std::ios::binary | std::ios::ate);
		std::streamsize size = ifs.tellg();
		ifs.seekg(0, std::ios::beg);

		pixelShader.resize(size);
		if (ifs.read(pixelShader.data(), size))
		{
		}
		ifs.close();
	}
	*/

	// Define the vertex input layout.
	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
	{
		{ "POSITION",	0, DXGI_FORMAT_R32G32_FLOAT,		0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",	0, DXGI_FORMAT_R32G32_FLOAT,		0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",		0, DXGI_FORMAT_R32G32B32A32_FLOAT,	0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
	// Describe and create the graphics pipeline state object (PSO).
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = g_rootSignature.get();
	//psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.data(), vertexShader.size());
	//psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.data(), pixelShader.size());
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.get());
	psoDesc.RasterizerState = rasterizerDesc;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;
	winrt::check_hresult(g_device->CreateGraphicsPipelineState(&psoDesc, IID_ID3D12PipelineState, g_pipeline.put_void()));

	// Create the command queue.
#if defined(_DEBUG)
	winrt::com_ptr<ID3D12InfoQueue> infoQueue = g_device.as<ID3D12InfoQueue>();
	infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
	infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);

	D3D12_MESSAGE_ID hide[] =
	{
		D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
		D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE
	};
	D3D12_INFO_QUEUE_FILTER infoQueueFilter = {};
	infoQueueFilter.DenyList.NumIDs = static_cast<UINT>(std::size(hide));
	infoQueueFilter.DenyList.pIDList = hide;
	infoQueue->AddStorageFilterEntries(&infoQueueFilter);
#endif

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	winrt::check_hresult(g_device->CreateCommandQueue(&queueDesc, IID_ID3D12CommandQueue, g_commandQueue.put_void()));

	// Render target views and depth stencil views
	// Create descriptor heaps for render target views and depth stencil views.
	D3D12_DESCRIPTOR_HEAP_DESC rtvDescriptorHeapDesc = {};
	rtvDescriptorHeapDesc.NumDescriptors = MAX_FRAMES_IN_FLIGHT;
	rtvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

	D3D12_DESCRIPTOR_HEAP_DESC dsvDescriptorHeapDesc = {};
	dsvDescriptorHeapDesc.NumDescriptors = 1;
	dsvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

	winrt::check_hresult(g_device->CreateDescriptorHeap(&rtvDescriptorHeapDesc, IID_ID3D12DescriptorHeap, g_rtvDescriptorHeap.put_void()));
	winrt::check_hresult(g_device->CreateDescriptorHeap(&dsvDescriptorHeapDesc, IID_ID3D12DescriptorHeap, g_dsvDescriptorHeap.put_void()));

	D3D12_DESCRIPTOR_HEAP_DESC srvDescriptorHeapDesc = {};
	srvDescriptorHeapDesc.NumDescriptors = 1;
	srvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvDescriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	winrt::check_hresult(g_device->CreateDescriptorHeap(&srvDescriptorHeapDesc, IID_ID3D12DescriptorHeap, g_srvDescriptorHeap.put_void()));

	g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	for (UINT i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		winrt::check_hresult(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, g_commandAllocators[i].put_void()));
	}

	winrt::check_hresult(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[0].get(), nullptr, IID_ID3D12CommandList, g_commandList.put_void()));
	//winrt::check_hresult(g_commandList->Close());

	// Triangle
	float triangleVertices[] =
	{
		0.5f, 0.5f,			1.0f, 0.0f,		1.0f, 0.0f, 0.0f, 1.0f,
		0.5f, -0.5f,		1.0f, 1.0f,		0.0f, 1.0f, 0.0f, 1.0f,
		-0.5f, -0.5f,		0.0f, 1.0f,		0.0f, 0.0f, 1.0f, 1.0f,
		-0.5f, 0.5f,		0.0f, 0.0f,		0.0f, 0.0f, 1.0f, 1.0f
	};

	const UINT vertexBufferSize = 4 * 8 * sizeof(float);

	const UINT indices[] = {
		0, 1, 3,
		1, 2, 3
	};
	const UINT indexBufferSize = 3 * 2 * sizeof(UINT);

	{
		winrt::check_hresult(g_device->CreateCommittedResource
		(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_ID3D12Resource,
			g_vertexBuffer.put_void()
		));

		// Copy the triangle data to the vertex buffer.
		UINT8* pVertexDataBegin;
		CD3DX12_RANGE readRange(0, 0);        // We do not intend to read from this resource on the CPU.
		winrt::check_hresult(g_vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
		memcpy(pVertexDataBegin, triangleVertices, sizeof(triangleVertices));
		g_vertexBuffer->Unmap(0, nullptr);

		g_vertexBufferView.BufferLocation = g_vertexBuffer->GetGPUVirtualAddress();
		g_vertexBufferView.StrideInBytes = 8 * sizeof(float);
		g_vertexBufferView.SizeInBytes = vertexBufferSize;
	}

	{
		winrt::check_hresult(g_device->CreateCommittedResource
		(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_ID3D12Resource,
			g_indexBuffer.put_void()
		));

		// Copy the triangle data to the vertex buffer.
		UINT8* pIndexDataBegin;
		CD3DX12_RANGE readRange(0, 0);        // We do not intend to read from this resource on the CPU.
		winrt::check_hresult(g_indexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pIndexDataBegin)));
		memcpy(pIndexDataBegin, indices, sizeof(indices));
		g_indexBuffer->Unmap(0, nullptr);

		g_indexBufferView.BufferLocation = g_indexBuffer->GetGPUVirtualAddress();
		g_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
		g_indexBufferView.SizeInBytes = indexBufferSize;
	}

	// Texture
	winrt::com_ptr<ID3D12Resource> textureUploadHeap;
	{
		D3D12_RESOURCE_DESC textureDesc = {};
		textureDesc.MipLevels = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.Width = g_textureWidth;
		textureDesc.Height = g_textureHeight;
		textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
		textureDesc.DepthOrArraySize = 1;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.SampleDesc.Quality = 0;
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

		winrt::check_hresult(g_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_ID3D12Resource,
			g_texture.put_void()
		));

		const UINT64 uploadBufferSize = GetRequiredIntermediateSize(g_texture.get(), 0, 1);

		winrt::check_hresult(g_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_ID3D12Resource,
			textureUploadHeap.put_void()
		));

		D3D12_SUBRESOURCE_DATA textureData{};
		textureData.pData = g_pixels.data();
		textureData.RowPitch = g_textureWidth * 4U;
		textureData.SlicePitch = textureData.RowPitch * g_textureHeight;

		UpdateSubresources(g_commandList.get(), g_texture.get(), textureUploadHeap.get(), 0, 0, 1, &textureData);
		g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_texture.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

		// Describe and create a SRV for the texture.
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = textureDesc.Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		g_device->CreateShaderResourceView(g_texture.get(), &srvDesc, g_srvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	}

	// Close the command list and execute it to begin the initial GPU setup.
	winrt::check_hresult(g_commandList->Close());
	ID3D12CommandList* ppCommandLists[] = {g_commandList.get() };
	g_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	// Fence
	winrt::check_hresult(g_device->CreateFence(g_fenceValues[g_backBufferIndex], D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, g_fence.put_void()));
	g_fenceValues[g_backBufferIndex]++;

	g_fenceEvent.attach(CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE));
	if (!g_fenceEvent)
	{
		throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "CreateEventEx");
	}

	waitForGpu();
}

void createResources()
{
	// Wait until all previous GPU work is complete.
	waitForGpu();

	for (UINT i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		g_renderTargets[i] = nullptr;
		g_fenceValues[i] = g_fenceValues[g_backBufferIndex];
	}

	const DXGI_FORMAT backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
	const DXGI_FORMAT depthBufferFormat = DXGI_FORMAT_D32_FLOAT;
	const UINT backBufferWidth = static_cast<UINT>(gWidth);
	const UINT backBufferHeight = static_cast<UINT>(gHeight);

	if (g_swapChain)
	{
		HRESULT hr = g_swapChain->ResizeBuffers(MAX_FRAMES_IN_FLIGHT, backBufferWidth, backBufferHeight, backBufferFormat, 0);
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
			onDeviceLost();
			return;
		}
		else
		{
			winrt::check_hresult(hr);
		}
	}
	else
	{
		DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
		swapChainDesc.Width = backBufferWidth;
		swapChainDesc.Height = backBufferHeight;
		swapChainDesc.Format = backBufferFormat;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.BufferCount = MAX_FRAMES_IN_FLIGHT;
		swapChainDesc.SampleDesc.Count = 1;
		swapChainDesc.SampleDesc.Quality = 0;
		swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsSwapChainDesc = {};
		fsSwapChainDesc.Windowed = TRUE;

		winrt::com_ptr<IDXGISwapChain1> swapChain;
		winrt::check_hresult(g_factory->CreateSwapChainForHwnd(
			g_commandQueue.get(),
			g_window,
			&swapChainDesc,
			&fsSwapChainDesc,
			nullptr,
			swapChain.put()
		));
		g_swapChain = swapChain.as<IDXGISwapChain3>();
		winrt::check_hresult(g_factory->MakeWindowAssociation(g_window, DXGI_MWA_NO_ALT_ENTER));
	}

	for (UINT i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		winrt::check_hresult(g_swapChain->GetBuffer(i, IID_ID3D12Resource, g_renderTargets[i].put_void()));

		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvDescriptor
		(
			g_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
			static_cast<INT>(i),
			g_rtvDescriptorSize
		);
		g_device->CreateRenderTargetView(g_renderTargets[i].get(), nullptr, rtvDescriptor);
	}

	g_backBufferIndex = g_swapChain->GetCurrentBackBufferIndex();

	CD3DX12_HEAP_PROPERTIES depthHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC depthStencilDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		depthBufferFormat,
		backBufferWidth,
		backBufferHeight,
		1, // This depth stencil view has only one texture.
		1  // Use a single mipmap level.
	);
	depthStencilDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
	depthOptimizedClearValue.Format = depthBufferFormat;
	depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
	depthOptimizedClearValue.DepthStencil.Stencil = 0;

	winrt::check_hresult(g_device->CreateCommittedResource(
		&depthHeapProperties,
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&depthOptimizedClearValue,
		IID_ID3D12Resource,
		g_depthStencil.put_void()
	));

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Format = depthBufferFormat;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

	g_device->CreateDepthStencilView(g_depthStencil.get(), &dsvDesc, g_dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
}

void clear()
{
	winrt::check_hresult(g_commandAllocators[g_backBufferIndex]->Reset());
	winrt::check_hresult(g_commandList->Reset(g_commandAllocators[g_backBufferIndex].get(), nullptr));

	D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(g_renderTargets[g_backBufferIndex].get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	g_commandList->ResourceBarrier(1, &barrier);

	// Clear the views.
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvDescriptor
	(
		g_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
		static_cast<INT>(g_backBufferIndex),
		g_rtvDescriptorSize
	);
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvDescriptor(g_dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	g_commandList->OMSetRenderTargets(1, &rtvDescriptor, FALSE, &dsvDescriptor);
	g_commandList->ClearRenderTargetView(rtvDescriptor, DirectX::Colors::CornflowerBlue, 0, nullptr);
	g_commandList->ClearDepthStencilView(dsvDescriptor, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// Set the viewport and scissor rect.
	D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(gWidth), static_cast<float>(gHeight), D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
	D3D12_RECT scissorRect = { 0, 0, static_cast<LONG>(gWidth), static_cast<LONG>(gHeight) };
	g_commandList->RSSetViewports(1, &viewport);
	g_commandList->RSSetScissorRects(1, &scissorRect);
}

void moveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const UINT64 currentFenceValue = g_fenceValues[g_backBufferIndex];
	winrt::check_hresult(g_commandQueue->Signal(g_fence.get(), currentFenceValue));

	// Update the back buffer index.
	g_backBufferIndex = g_swapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready.
	if (g_fence->GetCompletedValue() < g_fenceValues[g_backBufferIndex])
	{
		winrt::check_hresult(g_fence->SetEventOnCompletion(g_fenceValues[g_backBufferIndex], g_fenceEvent.get()));
		WaitForSingleObjectEx(g_fenceEvent.get(), INFINITE, FALSE);
	}

	// Set the fence value for the next frame.
	g_fenceValues[g_backBufferIndex] = currentFenceValue + 1;
}

void present()
{
	// Transition the render target to the state that allows it to be presented to the display.
	D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(g_renderTargets[g_backBufferIndex].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	g_commandList->ResourceBarrier(1, &barrier);

	// Send the command list off to the GPU for processing.
	winrt::check_hresult(g_commandList->Close());
	ID3D12CommandList* cmdLists[] = { g_commandList.get() };
	g_commandQueue->ExecuteCommandLists(1, cmdLists);

	// The first argument instructs DXGI to block until VSync, putting the application
	// to sleep until the next VSync. This ensures we don't waste any cycles rendering
	// frames that will never be displayed to the screen.
	HRESULT hr = g_swapChain->Present(1, 0);

	// If the device was reset we must completely reinitialize the renderer.
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
	{
		onDeviceLost();
	}
	else
	{
		winrt::check_hresult(hr);

		moveToNextFrame();
	}
}

bool init()
{
	// Read image
	std::ifstream ifs("data/pokemon.png", std::ios::binary | std::ios::ate);
	std::streamsize size = ifs.tellg();
	ifs.seekg(0, std::ios::beg);

	std::vector<char> buffer(size);
	if (ifs.read(buffer.data(), size))
	{
		spng_ctx* ctx = spng_ctx_new(0);

		spng_set_png_buffer(ctx, buffer.data(), buffer.size());

		spng_ihdr ihdr;
		spng_get_ihdr(ctx, &ihdr);
		g_textureWidth = ihdr.width;
		g_textureHeight = ihdr.height;

		size_t out_size;
		spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &out_size);

		//std::vector<unsigned char> pixels(out_size);
		g_pixels.resize(out_size);
		spng_decode_image(ctx, g_pixels.data(), out_size, SPNG_FMT_RGBA8, 0);

		spng_ctx_free(ctx);
	}
	ifs.close();

	g_window = glfwGetWin32Window(g_pWindow);
	createDevice();
	createResources();

	return true;
}

void on_size()
{
}

void on_key(int key, int action)
{
}

void on_mouse(double xpos, double ypos)
{
}

void size()
{
	createResources();
}

void update()
{

}

void draw()
{
	clear();

	g_commandList->SetPipelineState(g_pipeline.get());
	g_commandList->SetGraphicsRootSignature(g_rootSignature.get());

	ID3D12DescriptorHeap* ppHeaps[] = { g_srvDescriptorHeap.get() };
	g_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

	g_commandList->SetGraphicsRootDescriptorTable(0, g_srvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	g_commandList->IASetVertexBuffers(0, 1, &g_vertexBufferView);
	g_commandList->IASetIndexBuffer(&g_indexBufferView);
	g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_commandList->DrawIndexedInstanced(6, 1, 0, 0, 0);

	present();
}

int main()
{
	return run();
}

void onDeviceLost()
{
	for (UINT i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		g_commandAllocators[i] = nullptr;
		g_renderTargets[i] = nullptr;
	}

	g_depthStencil = nullptr;
	g_fence = nullptr;
	g_commandList = nullptr;
	g_swapChain = nullptr;
	g_rtvDescriptorHeap = nullptr;
	g_dsvDescriptorHeap = nullptr;
	g_commandQueue = nullptr;
	g_device = nullptr;
	g_factory = nullptr;

	createDevice();
	createResources();
}