// The D3D12 runtime the Windows game asks for: the Agility SDK's
// D3D12Core.dll in D3D12\ beside the program (scripts/fetch_d3d12_agility.py
// fetches it, CMake copies it there). d3d12.dll reads these two exports from
// the program itself when it loads. Plume's backend needs interfaces
// (ID3D12GraphicsCommandList7) that Windows 10's own runtime lacks; Plume's
// copies of the exports name the building SDK's version rather than the
// shipped runtime's, so CMake renames those and these are the ones seen.
#include <cstdint>

extern "C" {
__declspec(dllexport) extern const uint32_t D3D12SDKVersion = SFR_D3D12_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
