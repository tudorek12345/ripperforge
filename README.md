# RipperForge

RipperForge is a native C++20 Windows desktop toolkit for game modding and reverse-engineering workflows.

## Current MVP foundation

- Process browser with search and auto-refresh
- DLL injector (`CreateRemoteThread + LoadLibraryW`)
- Lightweight memory tools:
  - read/write bytes at address
  - AoB pattern scan (`??` wildcard support)
- AssetRIpper bridge integration:
  - cloned upstream source under `external/AssetRIpper/`
  - writes `%TEMP%/asset_ripper_<pid>.cfg` like upstream injector
  - injects configured capture DLL and scans output directory for new assets
- Asset Ripper tab:
  - capture DLL + output directory inputs
  - texture/model discovery lists
  - DirectX 11 live preview window (texture + mesh modes)
  - real export actions: `.png`, `.obj`, `.fbx`
- Hook manager:
  - engine presets (Unity, Source, Unreal)
  - backend presets (MinHook, Detours)
  - hook DLL injection
  - backend-aware template generation + build notes
- Plugin system:
  - runtime `.dll` discovery from `plugins/`
  - required exports: `RF_PluginName`, `RF_OnLoad`, `RF_OnUnload`
- Persistent settings stored in `config/settings.json`

## Build (Visual Studio 2022)

1. Open `RipperForge.sln`
2. Select `Debug | x64` or `Release | x64`
3. Build the `RipperForge` project

Output binaries are generated under `build/<Configuration>/`.

## AssetRIpper backend build

The upstream capture backend is cloned in `external/AssetRIpper/`.

1. Open **x64 Native Tools Command Prompt for VS 2022**
2. `cd external\\AssetRIpper`
3. `build.bat` (expects Detours include/lib; see upstream README in that folder)
4. In RipperForge Asset Ripper tab, set **Capture DLL** to the produced `ripper_new6.dll`

## Plugin contract

Implement the exports below in your plugin DLL:

```cpp
extern "C" __declspec(dllexport) const char* __stdcall RF_PluginName();
extern "C" __declspec(dllexport) bool __stdcall RF_OnLoad(const rf::plugins::HostApi* hostApi);
extern "C" __declspec(dllexport) void __stdcall RF_OnUnload();
```

`HostApi::Log` lets plugins write directly into the RipperForge log console.

See `plugins/SamplePlugin.cpp` for a minimal plugin implementation.

## Next steps to ship

- Build `external/AssetRIpper/ripper.cpp` into your capture DLL (`ripper_new6.dll` or preferred name) and point the Asset Ripper tab to that DLL.
- Extend model preview loading beyond OBJ (currently OBJ is preview/export-ready; FBX export is generated from loaded mesh data).
- Add async worker jobs for long memory scans and large capture directory scans to keep UI responsive under heavy loads.
