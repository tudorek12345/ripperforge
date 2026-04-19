# RipperForge

RipperForge is a native Windows toolkit for **asset capture, DLL injection, and reverse tooling**.
Built for modders, reverse engineers, and technical users who need a fast all-in-one desktop workflow.

## What it does

- Process browser with filter, refresh, and elevation flow
- DLL injector (targeted process injection)
- Asset capture panel with scan + export flow
- DirectX 11 preview for textures/models (with fallback if DX11 preview is unavailable)
- Export pipeline: `.png`, `.obj`, `.fbx`
- Hook template generator (MinHook / Detours)
- Reverse toolkit typed scans (`int32`, `int64`, `float`, `double`, utf8 string, byte array)
- Reverse toolkit first/next scan filters
- Reverse toolkit pointer-chain explorer and watch/freeze list
- Plugin loading from install directory plugins
- Plugin loading from `%LocalAppData%\\RipperForge\\plugins`

## Install (end users)

Download the installer from Releases and run:

- `RipperForge-Setup-x64.exe`

The installer places app binaries under `Program Files` and installs required VC++ runtime automatically.

## Runtime data location

RipperForge writes user data to:

- `%LocalAppData%\\RipperForge`

This includes settings, UI layout, captures, generated hooks, and user plugin drop-ins.

## Build from source

Requirements:

- Visual Studio 2022 C++ workload (`v143`, Windows SDK)

Build:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" RipperForge.sln /t:Build /p:Configuration=Release /p:Platform=x64 /m
```

Output binary:

- `build\\Release\\RipperForge.exe`

## Notes

Use this software only on binaries/processes you own or are explicitly authorized to inspect/modify.
