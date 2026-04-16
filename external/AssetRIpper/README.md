# AssetRipper (Injector + Capture DLL)

AssetRipper is a two-part Windows x64 tool for capturing draw data from DirectX 9/11 applications. It consists of a GUI injector (`AssetRipper.exe`) and an injected hook DLL (default `ripper_new6.dll`) built with Detours.

## Features
- Lists running processes and injects the DLL into a selected target using `VirtualAllocEx` + `WriteProcessMemory` + `CreateRemoteThread(LoadLibraryW)`.
- Supports CLI launch mode to start a process suspended and inject before D3D device creation.
- Stores capture settings in `%TEMP%\\asset_ripper_<pid>.cfg` (output directory, auto-capture, overlay, and throttle settings).
- Hooks DirectX 9 `IDirect3DDevice9::DrawIndexedPrimitive` and DirectX 11 `ID3D11DeviceContext::DrawIndexed` via Detours.
- Captures vertex/index buffers, bound textures, and vertex/pixel shader bytecode; writes assets asynchronously to disk.
- Hotkeys inside target process: `F9` toggles frame capture (press once to start, press again to finalize), `F10` capture once, `F11` toggle every-draw capture, `F12` request shutdown/cleanup. Outputs are named sequentially (`model_###.obj`, `tex_###.dds`, `vs_###.dxbc`, `ps_###.dxbc`, `frame_####.glb`).
- DX module watcher attaches hooks even if `d3d9.dll`/`d3d11.dll` load after injection. Capture threads wake/exit cleanly when shutting down.
- Present hook bootstrap (dummy devices) detours D3D9/DXGI `Present`, so hooks can attach even when you inject long after the device was created.

## Building (C++17)
Requires Windows SDK/DirectX SDK and Detours libraries on the include/lib path. Example (Developer Command Prompt, x64):
```bat
cl /std:c++17 /EHsc /MD AssetRipper.cpp /link user32.lib gdi32.lib comdlg32.lib psapi.lib shell32.lib ole32.lib
cl /std:c++17 /EHsc /MD /LD ripper.cpp /link detours.lib d3d9.lib d3d11.lib dxgi.lib dxguid.lib
```

## Usage
1) Place `AssetRipper.exe` and `ripper_new6.dll` together (or browse to your DLL path from the UI).  
2) Run `AssetRipper.exe`, refresh the process list, select your target, choose an output folder, and click **Inject DLL**.  
3) In the target app, press `F9` to start a frame capture and press `F9` again to finalize, `F10` to capture the next draw, `F11` to toggle per-draw capture, `F12` to stop. Files are written under the chosen output directory.

CLI launch (early injection):
```
AssetRipper.exe --launch "C:\Path\Game.exe" --dll "C:\Path\ripper_new6.dll" --out "C:\Path\captures" --args "-force-d3d11 -screen-fullscreen 0 -screen-width 1280 -screen-height 720" --no-overlay --capture-frame
```

## Notes and limitations
- Hooks attach when new D3D9/D3D11 devices are created after injection; use the CLI `--launch` mode for early injection when input layouts are created very early.
- Present detour mitigates late injection: if you inject after the device exists, the `Present` hook installs draw hooks on the first frame that hits it. If the app never calls Present, hooking will not occur.
- Auto-capture can be throttled via config: `auto_capture_draws=<N>` and `auto_capture_seconds=<S>` to limit captures to N draws per S seconds (set draws to 0 to disable throttling).
- Frame capture can be triggered with `F9`, `--capture-frame`, or `capture_frame=1` in the temp config file.
- Texture export uses DX10 DDS headers and preserves DXGI formats/mips/arrays where supported; typeless, MSAA, and non-Texture2D SRVs are skipped.
- glTF frame export targets D3D11 triangle lists and supports only RGBA/BGRA 2D textures for embedded PNGs; other formats are skipped but still listed in `extras`.
- Shader dumps are raw DXBC bytecode (no disassembly). Filtering heuristics to skip UI overlays are minimal (skip tiny draws and invalid ranges).
- Ensure the target process matches the DLL architecture (x64) and has the required D3D runtime present. Detours must be available at link time for `ripper.dll`.
- F12 stops capture threads and signals the writer to flush; hooks detach on DLL unload (process exit/unload). F9 frame capture exports a single `.glb` with per-draw mesh data and materials; constant buffers, SRVs, samplers, and instance data are stored in `extras`.

## VS Code setup (MSVC + Windows SDK)
- Launch VS Code from a **x64 Native Tools for VS** Developer Command Prompt so `VCToolsInstallDir` and `WindowsSdkDir` env vars are set.
- `.vscode/c_cpp_properties.json` and `.vscode/settings.json` already point `compilerPath` to `${env:VCToolsInstallDir}bin/Hostx64/x64/cl.exe` and include Windows SDK + Detours include paths (`C:/detours/include`). Adjust if your Detours install differs.
- If you prefer fixed paths, replace the `${env:...}` entries with absolute paths to your MSVC and Windows SDK installations.

## Build script
- `build.bat` expects Detours at `C:\detours\include` and `C:\detours\lib.X64` (override with `DETOURS_INCLUDE` / `DETOURS_LIB` env vars).
- Run it from a **x64 Native Tools for VS** Developer Command Prompt:  
  `build.bat`
