#include "entry.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <Windows.h>

#include <winrt/base.h>

#include <dxgi1_4.h>
#include <d3d12.h>

#if defined(_DEBUG)
#include <dxgidebug.h>
#endif

#include <DirectXMath.h>
#include <DirectXColors.h>

#include "d3dx12.h"

const int MAX_FRAMES_IN_FLIGHT = 2;

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

UINT g_rtvDescriptorSize;

UINT g_backBufferIndex = 0;

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

	// Create the command queue.
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

	for (UINT i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		winrt::check_hresult(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, g_commandAllocators[i].put_void()));
	}

	winrt::check_hresult(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[0].get(), nullptr, IID_ID3D12CommandList, g_commandList.put_void()));
	winrt::check_hresult(g_commandList->Close());

	// Fence
	winrt::check_hresult(g_device->CreateFence(g_fenceValues[g_backBufferIndex], D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, g_fence.put_void()));
	g_fenceValues[g_backBufferIndex]++;

	g_fenceEvent.attach(CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE));
	if (!g_fenceEvent)
	{
		throw std::system_error(std::error_code(static_cast<int>(GetLastError()), std::system_category()), "CreateEventEx");
	}

	// Check shader model 6 support
	D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_SHADER_MODEL_6_0 };
	if ((FAILED(g_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) || (shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_0)))
	{
		throw std::runtime_error("Shader Model 6.0 is not supported!");
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