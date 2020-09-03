#include "pch.h"
#include "Overlay.h"
#include "Loader.h"
#include "achievement_manager_ui.h"
#include <thread>
#include <future>

#define POPUP_DURATION_MS	3000

// Forward declaration, as suggested by imgui_impl_win32.cpp#L270
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Adapted from: https://github.com/rdbo/ImGui-DirectX-11-Kiero-Hook
namespace Overlay {

HRESULT(__stdcall* originalPresent) (IDXGISwapChain*, UINT, UINT);
HRESULT(__stdcall* originalResizeBuffers) (IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
LRESULT(__stdcall* originalWindowProc)(HWND, UINT, WPARAM, LPARAM);

HWND gWindow = nullptr;
ID3D11Device* gD3D11Device = nullptr;
ID3D11RenderTargetView* gRenderTargetView = nullptr;

bool bInit = false;
bool bShowAchievementManager = false;
bool bShowInitPopup = true;

LRESULT __stdcall WindowProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if(uMsg == WM_KEYDOWN) {
		// Shift + F5 pressed?
		if(GetKeyState(VK_SHIFT) & 0x8000 && wParam == VK_F5) {
			bShowInitPopup = false; // Hide the popup
			bShowAchievementManager = !bShowAchievementManager; // Toggle the overlay
		}
	}

	if(bShowAchievementManager) {
		// Civilization VI mouse input fix
		switch(uMsg)
		{
			case WM_POINTERDOWN:
				uMsg = WM_LBUTTONDOWN;
				break;
			case WM_POINTERUP:
				uMsg = WM_LBUTTONUP;
				break;
			case WM_POINTERWHEEL:
				uMsg = WM_MOUSEWHEEL;
				break;
			case WM_POINTERUPDATE:
				uMsg = WM_SETCURSOR;
				break;
		}
		ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
		return true;
	} else {
		return CallWindowProc(originalWindowProc, hWnd, uMsg, wParam, lParam);
	}
}

HRESULT __stdcall hookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
	static ID3D11DeviceContext* pContext = nullptr;

	if(!bInit) {
		if(SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**) &gD3D11Device))) {
			gD3D11Device->GetImmediateContext(&pContext);
			DXGI_SWAP_CHAIN_DESC sd;
			pSwapChain->GetDesc(&sd);
			gWindow = sd.OutputWindow;
			ID3D11Texture2D* pBackBuffer;
			pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*) &pBackBuffer);
#pragma warning(suppress: 6387)
			gD3D11Device->CreateRenderTargetView(pBackBuffer, NULL, &gRenderTargetView);
			pBackBuffer->Release();
			originalWindowProc = (WNDPROC) SetWindowLongPtr(gWindow, GWLP_WNDPROC, (LONG_PTR) WindowProc);
			if(originalWindowProc == NULL){
				Logger::error("Failed to SetWindowLongPtr. Error code: %d", GetLastError());
				return originalPresent(pSwapChain, SyncInterval, Flags);
			}
			AchievementManagerUI::initImGui(gWindow, gD3D11Device, pContext);
			bInit = true;
		} else {
			return originalPresent(pSwapChain, SyncInterval, Flags);
		}
	}

	// Now that we are hooked, it's time to render the Achivement Manager

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	if(bShowInitPopup)
		AchievementManagerUI::drawInitPopup();

	if(bShowAchievementManager)
		AchievementManagerUI::drawAchievementList();

	ImGui::Render();

	pContext->OMSetRenderTargets(1, &gRenderTargetView, NULL);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	return originalPresent(pSwapChain, SyncInterval, Flags);
}

/**
 * We are hooking ResizeBuffer function in order to release allocated resources
 * and reset init flag, so that our overlay can reinitialize with new window size.
 * Without it, the game will crash on window resize.
 */
HRESULT __stdcall hookedResizeBuffer(IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags){
	AchievementManagerUI::shutdownImGui();

	// Restore original WndProc. Crashes without it.
	SetWindowLongPtr(gWindow, GWLP_WNDPROC, (LONG_PTR) originalWindowProc);

	// Release RTV according to: https://www.unknowncheats.me/forum/2638258-post8.html
	gRenderTargetView->Release();
	gRenderTargetView = nullptr;

	bInit = false;
	return originalResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

// Table with method indices: https://github.com/Rebzzel/kiero/blob/master/METHODSTABLE.txt
#define D3D11_Present		8
#define D3D11_ResizeBuffers	13

void initThread(LPVOID lpReserved) {
	while(kiero::init(kiero::RenderType::D3D11) != kiero::Status::Success);
	Logger::ovrly("Kiero: Successfully initialized");

	// Hook Present
	kiero::bind(D3D11_Present, (void**) &originalPresent, hookedPresent);
	Logger::ovrly("Kiero: Successfully hooked Present");

	// Hook ResizeBuffers
	kiero::bind(D3D11_ResizeBuffers, (void**) &originalResizeBuffers, hookedResizeBuffer);
	Logger::ovrly("Kiero: Successfully hooked ResizeBuffers");

	// Hide the popup after POPUP_DURATION_MS time
	static auto hidePopupJob = std::async(std::launch::async, [&]() {
		Sleep(POPUP_DURATION_MS);
		bShowInitPopup = false;
	});
}

void Overlay::init(HMODULE hMod, Achievements& achievements, UnlockAchievementFunction* unlockAchievement) {
	AchievementManagerUI::init(achievements, unlockAchievement);
	std::thread(initThread, hMod).detach();
}

void Overlay::shutdown() {
	AchievementManagerUI::shutdownImGui();
	SetWindowLongPtr(gWindow, GWLP_WNDPROC, (LONG_PTR) originalWindowProc);
	kiero::shutdown();
	Logger::ovrly("Kiero: Shutdown");
	// TODO: Clear the achievement vector as well?
}

}