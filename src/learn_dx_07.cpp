#include "entry.h"

#include <fstream>

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

const char* computeShaderSource = R"(
static const uint3 gl_WorkGroupSize = uint3(1u, 1u, 1u);

StructuredBuffer<float4> _34 : register(t0);
RWStructuredBuffer<float4> _48 : register(u0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
	uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
	uint idx = (gl_GlobalInvocationID.y * 3 * gl_WorkGroupSize.x) + gl_GlobalInvocationID.x;
	float3 p = _34[idx].xyz;
	_48[idx] = float4(p.x + 0.001000000047497451305389404296875f, p.y, 0.0f, 1.0f);
}

[numthreads(1, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
	gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
	comp_main();
}
)";

const char* vertexShaderSource = R"(
static float4 gl_Position;
static float4 aPos;

struct SPIRV_Cross_Input
{
	float4 aPos : POSITION0;
};

struct SPIRV_Cross_Output
{
	float4 gl_Position : SV_Position;
};

void vert_main()
{
	gl_Position = float4(aPos.xy, 0.0f, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
	aPos = stage_input.aPos;
	vert_main();
	SPIRV_Cross_Output stage_output;
	stage_output.gl_Position = gl_Position;
	return stage_output;
}
)";

const char* fragmentShaderSource = R"(
static float4 FragColor;

struct SPIRV_Cross_Output
{
	float4 FragColor : SV_Target0;
};

void frag_main()
{
	FragColor = float4(1.0f, 0.0f, 0.0f, 1.0f);
}

SPIRV_Cross_Output main()
{
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
//winrt::com_ptr<ID3D12Resource>			g_vertexBuffer;
//D3D12_VERTEX_BUFFER_VIEW					g_vertexBufferView;

// Other
UINT g_rtvDescriptorSize;

UINT g_backBufferIndex = 0;

// Compute
winrt::com_ptr<ID3D12RootSignature>			g_computeRootSignature;
winrt::com_ptr<ID3D12PipelineState>			g_computePipeline;
winrt::com_ptr<ID3D12Resource>				g_computeBuffer0;
winrt::com_ptr<ID3D12Resource>				g_computeBuffer1;
winrt::com_ptr<ID3D12Resource>				g_computeBufferUpload0;
winrt::com_ptr<ID3D12Resource>				g_computeBufferUpload1;

winrt::com_ptr<ID3D12DescriptorHeap>		g_srvUavHeap;
UINT										g_srvUavDescriptorSize;

int g_readBuferId = 0;

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
	CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init(0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	winrt::com_ptr<ID3DBlob> signature;
	winrt::check_hresult(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), nullptr));
	winrt::check_hresult(g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_ID3D12RootSignature, g_rootSignature.put_void()));

	// Compute root signature
	{
		CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

		CD3DX12_ROOT_PARAMETER1 parameters[2];
		parameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);
		parameters[1].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_ALL);

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
		rootSignatureDesc.Init_1_1(_countof(parameters), parameters, 0, nullptr);

		winrt::com_ptr<ID3DBlob> signature;
		winrt::check_hresult(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, signature.put(), nullptr));
		winrt::check_hresult(g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_ID3D12RootSignature, g_computeRootSignature.put_void()));
	}

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
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	// Describe and create the graphics pipeline state object (PSO).
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = g_rootSignature.get();
	//psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.data(), vertexShader.size());
	//psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.data(), pixelShader.size());
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.get());
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;
	winrt::check_hresult(g_device->CreateGraphicsPipelineState(&psoDesc, IID_ID3D12PipelineState, g_pipeline.put_void()));

	// Compute pipeline
	{
		winrt::com_ptr<ID3DBlob> computeShader;
		winrt::check_hresult(D3DCompile(computeShaderSource, std::strlen(computeShaderSource), nullptr, nullptr, nullptr, "main", "cs_5_0", compileFlags, 0, computeShader.put(), nullptr));
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineStateDesc{};
		computePipelineStateDesc.pRootSignature = g_computeRootSignature.get();
		computePipelineStateDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.get());

		winrt::check_hresult(g_device->CreateComputePipelineState(&computePipelineStateDesc, IID_ID3D12PipelineState, g_computePipeline.put_void()));
	}

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

	g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	g_srvUavDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	for (UINT i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		winrt::check_hresult(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, g_commandAllocators[i].put_void()));
	}

	// Describe and create a shader resource view (SRV) and unordered
	// access view (UAV) descriptor heap.
	D3D12_DESCRIPTOR_HEAP_DESC srvUavHeapDesc = {};
	srvUavHeapDesc.NumDescriptors = 4;
	srvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	winrt::check_hresult(g_device->CreateDescriptorHeap(&srvUavHeapDesc, IID_ID3D12DescriptorHeap, g_srvUavHeap.put_void()));

	winrt::check_hresult(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[0].get(), nullptr, IID_ID3D12CommandList, g_commandList.put_void()));
	//winrt::check_hresult(g_commandList->Close());

	// Triangle
	float triangleVertices[] =
	{
		0.0f, 0.25f, 0.0f, 0.0f,
		0.25f, -0.25f, 0.0f, 0.0f,
		-0.25f, -0.25f,	0.0f, 0.0f,
	};

	const UINT vertexBufferSize = 3 * 4 * sizeof(float);

	/*
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
	*/

	//g_vertexBufferView.BufferLocation = g_vertexBuffer->GetGPUVirtualAddress();
	//g_vertexBufferView.StrideInBytes = 4 * sizeof(float);
	//g_vertexBufferView.SizeInBytes = vertexBufferSize;

	// Compute buffer
	{
		winrt::check_hresult(g_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_ID3D12Resource,
			g_computeBuffer0.put_void()));

		winrt::check_hresult(g_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_ID3D12Resource,
			g_computeBuffer1.put_void()));

		winrt::check_hresult(g_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_ID3D12Resource,
			g_computeBufferUpload0.put_void()));

		winrt::check_hresult(g_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_ID3D12Resource,
			g_computeBufferUpload1.put_void()));

		D3D12_SUBRESOURCE_DATA particleData{};
		particleData.pData = triangleVertices;
		particleData.RowPitch = vertexBufferSize;
		particleData.SlicePitch = particleData.RowPitch;

		UpdateSubresources<1>(g_commandList.get(), g_computeBuffer0.get(), g_computeBufferUpload0.get(), 0, 0, 1, &particleData);
		UpdateSubresources<1>(g_commandList.get(), g_computeBuffer1.get(), g_computeBufferUpload1.get(), 0, 0, 1, &particleData);
		g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_computeBuffer0.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_computeBuffer1.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		//g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_computeBuffer0.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
		//g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_computeBuffer1.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

		// Close the command list and execute it to begin the initial GPU setup.
		winrt::check_hresult(g_commandList->Close());
		ID3D12CommandList* ppCommandLists[] = { g_commandList.get() };
		g_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

		// Resource view desc
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 3;
		srvDesc.Buffer.StructureByteStride = 4 * sizeof(float);
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

		CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle0(g_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 2, g_srvUavDescriptorSize);
		CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle1(g_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 3, g_srvUavDescriptorSize);
		g_device->CreateShaderResourceView(g_computeBuffer0.get(), &srvDesc, srvHandle0);
		g_device->CreateShaderResourceView(g_computeBuffer1.get(), &srvDesc, srvHandle1);

		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.NumElements = 3;
		uavDesc.Buffer.StructureByteStride = 4 * sizeof(float);
		uavDesc.Buffer.CounterOffsetInBytes = 0;
		uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

		CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle0(g_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, g_srvUavDescriptorSize);
		CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle1(g_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 1, g_srvUavDescriptorSize);
		g_device->CreateUnorderedAccessView(g_computeBuffer0.get(), nullptr, &uavDesc, uavHandle0);
		g_device->CreateUnorderedAccessView(g_computeBuffer1.get(), nullptr, &uavDesc, uavHandle1);
	}

	// Fence
	winrt::check_hresult(g_device->CreateFence(g_fenceValues[g_backBufferIndex], D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, g_fence.put_void()));
	g_fenceValues[g_backBufferIndex]++;

	g_fenceEvent.attach(CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE));
	if (!g_fenceEvent)
	{
		throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "CreateEventEx");
	}
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

	for (size_t i=0; i<10; i++)
	{
		UINT srvIndex;
		UINT uavIndex;
		winrt::com_ptr<ID3D12Resource> uavResource;
		if (g_readBuferId == 0)
		{
			srvIndex = 2U;
			uavIndex = 1U;
			uavResource = g_computeBuffer1;
		}
		else
		{
			srvIndex = 3U;
			uavIndex = 0U;
			uavResource = g_computeBuffer0;
		}
		g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(uavResource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		g_commandList->SetComputeRootSignature(g_computeRootSignature.get());
		g_commandList->SetPipelineState(g_computePipeline.get());

		ID3D12DescriptorHeap* ppHeaps[] = { g_srvUavHeap.get() };
		g_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

		CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(g_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), srvIndex, g_srvUavDescriptorSize);
		CD3DX12_GPU_DESCRIPTOR_HANDLE uavHandle(g_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), uavIndex, g_srvUavDescriptorSize);

		g_commandList->SetComputeRootDescriptorTable(0, srvHandle);
		g_commandList->SetComputeRootDescriptorTable(1, uavHandle);
		g_commandList->Dispatch(3, 1, 1);

		g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(uavResource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		g_readBuferId = 1 - g_readBuferId;
	}

	D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
	if (g_readBuferId == 0)
	{
		g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_computeBuffer0.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
		vertexBufferView.BufferLocation = g_computeBuffer0->GetGPUVirtualAddress();
	}
	else
	{
		g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_computeBuffer1.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
		vertexBufferView.BufferLocation = g_computeBuffer1->GetGPUVirtualAddress();
	}

	g_commandList->SetPipelineState(g_pipeline.get());
	g_commandList->SetGraphicsRootSignature(g_rootSignature.get());

	vertexBufferView.StrideInBytes = 4 * sizeof(float);
	vertexBufferView.SizeInBytes = 3 * 4 * sizeof(float);
	g_commandList->IASetVertexBuffers(0, 1, &vertexBufferView);

	g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_commandList->DrawInstanced(3, 1, 0, 0);

	if (g_readBuferId == 0)
	{
		g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_computeBuffer0.get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	}
	else
	{
		g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_computeBuffer1.get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	}

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