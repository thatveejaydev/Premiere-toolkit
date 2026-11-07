# Bevel Alpha for Adobe Premiere Pro

A Windows x64 C++ effect built against Adobe's After Effects C/C++ Plug-in SDK. It shades the inside of a clip's alpha silhouette and preserves the original transparency. On a fully opaque clip, it bevels the frame boundary. Version 1.0.1 uses Euclidean distances and smooths the bevel surface before lighting to reduce ridges on curved and diagonal edges. The lighting is an independent approximation of Adobe's effect.

| Effect Control | Default |
| --- | --- |
| Edge Thickness | 8.00 px |
| Light Angle | -45° |
| Light Color | White |
| Light Intensity | 0.30 |
| Highlight Strength | 100% |
| Shadow Strength | 100% |

Highlight Strength and Shadow Strength independently scale the two sides of the bevel from 0% to 200%. Light Intensity still controls the overall amount. Both new controls default to 100% for older projects.

Version 1.3.0 limits distance and smoothing calculations to the visible region of transparent frames on the CPU and on CUDA for frames with at least 3840 × 2160 pixels. Transparent pixels keep their original RGB and alpha values.

## Build the Windows plug-in

1. Download the **Windows After Effects C/C++ Plug-in SDK** using **Get the SDKs** on the [Adobe After Effects Developer page](https://developer.adobe.com/after-effects/). Sign in when prompted. Extract the SDK and find the folder containing `Examples\Headers\AE_Effect.h` and `Examples\Resources\PiPLtool.exe`. Some SDK downloads contain an inner archive, which also needs extraction.
2. Install [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) with **Desktop development with C++**, including the MSVC x64 toolset and a Windows SDK. Install CMake 3.21 or newer.
3. Extract this ZIP. Open PowerShell in the extracted `BevelAlpha` directory. Replace the example SDK path below with your extracted path, then run:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DAE_SDK_ROOT=C:/path/to/AfterEffectsSDK"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
Get-Item -LiteralPath .\build\Release\BevelAlpha.aex
```

The CMake project generates the Adobe PiPL resource and links it into `build\Release\BevelAlpha.aex`. The Adobe SDK is a separate build input.

MinGW-w64 x64 with Ninja is also supported. From PowerShell:

```powershell
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release "-DAE_SDK_ROOT=C:/path/to/AfterEffectsSDK"
cmake --build build-mingw
ctest --test-dir build-mingw --output-on-failure
```

This produces `build-mingw\BevelAlpha.aex`. If Windows blocks Adobe's PiPLTool in a restricted shell, rerun the build from a regular PowerShell window.

## Build with Premiere Pro CUDA acceleration

The CUDA build retains the CPU renderer and adds a Premiere GPU path for NVIDIA cards with CUDA compute capability 7.5 or newer. It uses GPU-resident 16-bit and 32-bit floating-point BGRA frames. Premiere can select the CPU path when a CUDA device is unavailable or GPU initialization fails. The plug-in contains the compiled GPU code. The NVIDIA compiler is required only when building.

1. Download and extract the Windows [Premiere Pro C++ SDK](https://developer.adobe.com/premiere-pro/).
2. Extract NVIDIA's Windows x64 [NVRTC 11.8 archive](https://developer.download.nvidia.com/compute/cuda/redist/cuda_nvrtc/windows-x86_64/). Keep the NVRTC DLL and its companion DLLs together in the extracted `bin` directory.
3. Configure the build with both SDKs and the NVRTC DLL path. For example, with MinGW-w64 and Ninja:

```powershell
cmake -S . -B build-gpu -G Ninja -DCMAKE_BUILD_TYPE=Release -DBEVEL_BUILD_GPU=ON "-DAE_SDK_ROOT=C:/path/to/AfterEffectsSDK" "-DPR_SDK_ROOT=C:/path/to/PremiereSDK" "-DBEVEL_NVRTC_DLL=C:/path/to/nvrtc/bin/nvrtc64_112_0.dll"
cmake --build build-gpu
ctest --test-dir build-gpu --output-on-failure
```

Install `build-gpu\BevelAlpha.aex` using the steps below. The Windows build was tested with Premiere SDK 27.0 and an NVIDIA RTX 2070 Super. A standalone test loaded the GPU entry point and compared its CUDA output with the CPU renderer.

## Install and check in Premiere Pro

1. Close Premiere Pro.
2. Copy the built `BevelAlpha.aex` to `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore`. Windows may request administrator approval. [Adobe's MediaCore folder reference](https://helpx.adobe.com/premiere/desktop/troubleshooting/crash-issues/premiere-freezes-on-the-splash-screen.html)
3. Relaunch Premiere Pro, search **Bevel Alpha** in the **Effects** panel, and look under **Video Effects > Stylize**.
4. Apply it to a medium-gray logo or title with transparency. Confirm the four defaults in **Effect Controls**. At the default angle, upper-left edges should lighten and lower-right edges should darken. Change the angle by 180° to reverse the lighting. Set intensity to 0.00 to confirm the unchanged image. If another effect shares the same display name, this plug-in uses the match name `Codex Bevel Alpha`.
5. To remove the effect, close Premiere Pro, remove `BevelAlpha.aex` from MediaCore, and relaunch.

## Renderer checks

The tests run without the Adobe SDK. With CMake, Ninja, and a C++17 compiler on PATH:

```powershell
cmake -S . -B render-build -G Ninja -DBEVEL_BUILD_PLUGIN=OFF
cmake --build render-build
ctest --test-dir render-build --output-on-failure
```

The included tests exercise lighting direction, independent strengths, tint, transparency, opaque frame boundaries, partially transparent silhouettes, render scale, disabled controls, row padding, cancellation, and floating-point color headroom. Premiere can negotiate 32-bit float BGRA or 8-bit BGRA for CPU rendering. The legacy 8-bit ARGB path and After Effects' 16-bit path are also supported.

## Verification status

The CUDA plug-in compiled for Windows x64 with MinGW-w64, Adobe After Effects SDK 27.0.0, Premiere Pro SDK 27.0, and NVRTC 11.8. The CPU tests and standalone CUDA tests passed. The binary exports the Adobe effect and Premiere GPU entry points and contains the PiPL resource. The compiled `.aex` is distributed separately from this source ZIP.
