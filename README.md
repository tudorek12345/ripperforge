# RipperForge

RipperForge is a native C++20 Windows desktop toolkit for game modding and reverse-engineering workflows.

## Current MVP foundation

- Process browser with search and auto-refresh
- DLL injector (`CreateRemoteThread + LoadLibraryW`)
- Lightweight memory tools:
  - read/write bytes at address
  - AoB pattern scan (`??` wildcard support)
  - async worker jobs for long pattern scans (non-blocking UI)
- AssetRIpper bridge integration:
  - cloned upstream source under `external/AssetRIpper/`
  - writes `%TEMP%/asset_ripper_<pid>.cfg` like upstream injector
  - injects configured capture DLL and scans output directory for new assets
- Asset Ripper tab:
  - capture DLL + output directory inputs
  - texture/model discovery lists
  - async capture directory scan workers + auto-scan while capture is running
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

## Prerequisites (first-time setup)

Install Visual Studio 2022 Build Tools with C++:

- Workload: `Desktop development with C++` (`Microsoft.VisualStudio.Workload.VCTools`)
- Toolset: `MSVC v143`
- SDK: Windows 10/11 SDK

Without that workload, builds fail with `Microsoft.Cpp.Default.props not found`.

### Install VS C++ workload (required, admin)

Run this in an **elevated** PowerShell or Command Prompt:

```powershell
"C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended
```

## Build and run (first-time test)

1. Open **x64 Native Tools Command Prompt for VS 2022**.
2. Build:
   - `msbuild RipperForge.sln /t:Build /p:Configuration=Debug /p:Platform=x64 /m`
3. Run:
   - `build\\Debug\\RipperForge.exe`

Output binaries are generated under `build/<Configuration>/`.

### Quick smoke test checklist

1. In **Injector** tab, refresh process list and confirm your game process is visible.
2. In **Asset Ripper** tab, set:
   - **Capture DLL** -> your built `ripper_new6.dll`
   - **Output Directory** -> a writable folder
3. Inject capture DLL and verify new assets appear in the output folder.
4. Select one texture/model and validate:
   - preview renders in DX11 panel
   - export buttons write `.png`, `.obj`, `.fbx`
   - UI stays responsive while asset scan is running (`Scan Assets` button shows `Scanning...`)
5. In **Hook Manager**, generate a MinHook/Detours template and inject hook DLL into a test process.

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

## Package for public release

1. Build `Release | x64`:
   - `msbuild RipperForge.sln /t:Build /p:Configuration=Release /p:Platform=x64 /m`
2. Build capture backend (`external\\AssetRIpper\\build.bat`) and include resulting capture DLL.
3. Create release folder, for example:
   - `RipperForge\\RipperForge.exe`
   - `RipperForge\\plugins\\` (optional plugin DLLs)
   - `RipperForge\\capture\\ripper_new6.dll`
   - `RipperForge\\config\\settings.json` (optional defaults)
   - `RipperForge\\README.md`
4. Include/install **Microsoft Visual C++ Redistributable 2015-2022 (x64)** for end users.
5. Zip the release folder (`RipperForge-vX.Y.Z-win64.zip`), publish on GitHub Releases, and attach checksums.
6. Optional but recommended: code-sign `RipperForge.exe` and your DLLs before publishing.

## Next steps to ship

- Build `external/AssetRIpper/ripper.cpp` into your capture DLL (`ripper_new6.dll` or preferred name) and point the Asset Ripper tab to that DLL.
- Extend model preview loading beyond OBJ (currently OBJ is preview/export-ready; FBX export is generated from loaded mesh data).
