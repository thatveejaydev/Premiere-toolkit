#include <windows.h>

#include <array>
#include <climits>
#include <cstdint>
#include <memory>

#include "PrSDKGPUFilter.h"
#include "PrSDKGPUDeviceSuite.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKVideoSegmentSuite.h"
#include "SPBasic.h"

#include "BevelCudaParams.h"
#include "BevelCudaPtx.h"
#include "BevelRender.h"

namespace {

using DevicePointer = std::uint64_t;

struct CudaDriver {
    HMODULE library = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    template<class Function>
    Function load(const char* name) const {
        return library ? reinterpret_cast<Function>(GetProcAddress(library, name)) : nullptr;
    }

    int (*initialize)(unsigned) = load<decltype(initialize)>("cuInit");
    int (*push)(void*) = load<decltype(push)>("cuCtxPushCurrent_v2");
    int (*pop)(void**) = load<decltype(pop)>("cuCtxPopCurrent_v2");
    int (*loadModule)(void**, const void*) = load<decltype(loadModule)>("cuModuleLoadData");
    int (*unloadModule)(void*) = load<decltype(unloadModule)>("cuModuleUnload");
    int (*getKernel)(void**, void*, const char*) = load<decltype(getKernel)>("cuModuleGetFunction");
    int (*launch)(void*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                  unsigned, void*, void**, void**) = load<decltype(launch)>("cuLaunchKernel");
    int (*sync)(void*) = load<decltype(sync)>("cuStreamSynchronize");
    int (*upload)(DevicePointer, const void*, std::size_t, void*) =
        load<decltype(upload)>("cuMemcpyHtoDAsync_v2");
    int (*download)(void*, DevicePointer, std::size_t, void*) =
        load<decltype(download)>("cuMemcpyDtoHAsync_v2");

    bool ready() const {
        return initialize && push && pop && loadModule && unloadModule && getKernel && launch && sync;
    }
    ~CudaDriver() { if (library) FreeLibrary(library); }
};

struct Context {
    const CudaDriver& driver;
    bool active;
    Context(const CudaDriver& api, void* context) : driver(api), active(context && !api.push(context)) {}
    ~Context() { if (active) { void* previous = nullptr; driver.pop(&previous); } }
};

template<class Suite>
struct SuiteLease {
    SPBasicSuite* basic = nullptr;
    const char* name = nullptr;
    int version = 0;
    const Suite* suite = nullptr;
    bool acquire(SPBasicSuite* sp, const char* key, int requested) {
        const void* acquired = nullptr;
        if (sp->AcquireSuite(key, requested, &acquired) || !acquired) return false;
        basic = sp;
        name = key;
        version = requested;
        suite = static_cast<const Suite*>(acquired);
        return true;
    }
    ~SuiteLease() { if (suite) basic->ReleaseSuite(name, version); }
    const Suite* operator->() const { return suite; }
};

struct GpuAllocation {
    const PrSDKGPUDeviceSuite* device;
    csSDK_uint32 index;
    void* memory = nullptr;
    ~GpuAllocation() { if (memory) device->FreeDeviceMemory(index, memory); }
};

struct GpuInstance {
    CudaDriver cuda;
    SuiteLease<PrSDKGPUDeviceSuite> device;
    SuiteLease<PrSDKPPixSuite> ppix;
    SuiteLease<PrSDKVideoSegmentSuite> video;
    PrGPUDeviceInfo info{};
    csSDK_uint32 deviceIndex = 0;
    csSDK_int32 node = 0;
    void* module = nullptr;
    void* boundsKernel = nullptr;
    void* seed = nullptr;
    void* distanceLocal = nullptr;
    void* distance = nullptr;
    void* blur = nullptr;
    void* shade = nullptr;

    ~GpuInstance() {
        if (module) {
            Context context(cuda, info.outContextHandle);
            if (context.active) cuda.unloadModule(module);
        }
    }

    bool initialize(PrGPUFilterInstance& instance) {
        if (!instance.piSuites || !instance.piSuites->utilFuncs ||
            !instance.piSuites->utilFuncs->getSPBasicSuite) return false;
        auto* basic = instance.piSuites->utilFuncs->getSPBasicSuite();
        if (!basic || !device.acquire(basic, kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion) ||
            !ppix.acquire(basic, kPrSDKPPixSuite, kPrSDKPPixSuiteVersion)) return false;
        for (int version = kPrSDKVideoSegmentSuiteVersion; version >= 4; --version) {
            if (video.acquire(basic, kPrSDKVideoSegmentSuite, version)) break;
        }
        if (!video.suite) return false;

        deviceIndex = instance.inDeviceIndex;
        node = instance.inNodeID;
        if (device->GetDeviceInfo(kPrSDKGPUDeviceSuiteVersion, deviceIndex, &info) ||
            info.outDeviceFramework != PrGPUDeviceFramework_CUDA ||
            !info.outMeetsMinimumRequirementsForAcceleration || !cuda.ready() || cuda.initialize(0)) {
            return false;
        }
        Context context(cuda, info.outContextHandle);
        return context.active && !cuda.loadModule(&module, bevelCudaPtx) &&
            !cuda.getKernel(&boundsKernel, module, "bevelBounds") &&
            !cuda.getKernel(&seed, module, "bevelSeed") &&
            !cuda.getKernel(&distanceLocal, module, "bevelDistanceLocal") &&
            !cuda.getKernel(&distance, module, "bevelDistance") &&
            !cuda.getKernel(&blur, module, "bevelBlur") &&
            !cuda.getKernel(&shade, module, "bevelShade");
    }

    bool getSettings(PrTime time, bevel::Settings& settings) const {
        std::array<PrParam, 4> params{};
        for (int i = 0; i < static_cast<int>(params.size()); ++i) {
            if (video->GetParam(node, i, time, &params[i])) return false;
        }
        const auto numeric = [](const PrParam& param, float& value) {
            if (param.mType == kPrParamType_Float32) value = param.mFloat32;
            else if (param.mType == kPrParamType_Float64) value = static_cast<float>(param.mFloat64);
            else return false;
            return std::isfinite(value);
        };
        if (!numeric(params[0], settings.thickness) || !numeric(params[3], settings.intensity)) return false;
        if (params[1].mType == kPrParamType_Int32) {
            settings.angle = static_cast<float>(params[1].mInt32) / 65536.0f;
        } else if (!numeric(params[1], settings.angle)) {
            return false;
        }

        if (params[2].mType == kPrParamType_Int64) {
            const auto color = static_cast<std::uint64_t>(params[2].mInt64);
            settings.red = static_cast<float>((color >> 32) & 0xffff) / 65280.0f;
            settings.green = static_cast<float>((color >> 16) & 0xffff) / 65280.0f;
            settings.blue = static_cast<float>(color & 0xffff) / 65280.0f;
        } else if (params[2].mType == kPrParamType_Int32) {
            const auto color = static_cast<std::uint32_t>(params[2].mInt32);
            settings.red = static_cast<float>((color >> 16) & 255) / 255.0f;
            settings.green = static_cast<float>((color >> 8) & 255) / 255.0f;
            settings.blue = static_cast<float>(color & 255) / 255.0f;
        } else {
            return false;
        }
        settings.red = std::min(settings.red, 1.0f);
        settings.green = std::min(settings.green, 1.0f);
        settings.blue = std::min(settings.blue, 1.0f);
        settings.thickness = std::clamp(settings.thickness, 0.0f, 256.0f);
        settings.intensity = std::clamp(settings.intensity, 0.0f, 1.0f);
        const auto strength = [&](int index, float fallback) {
            PrParam param{};
            float percent = 0.0f;
            return !video->GetParam(node, index, time, &param) && numeric(param, percent) ?
                std::clamp(percent, 0.0f, 200.0f) * 0.01f : fallback;
        };
        settings.highlight = strength(4, settings.highlight);
        settings.shadow = strength(5, settings.shadow);
        return true;
    }

    bool launch2D(void* kernel, int width, int height, void** arguments) const {
        constexpr unsigned blockX = 32;
        constexpr unsigned blockY = 8;
        return !cuda.launch(kernel, (width + blockX - 1) / blockX,
            (height + blockY - 1) / blockY, 1, blockX, blockY, 1, 0,
            info.outCommandQueueHandle, arguments, nullptr);
    }

    bool launchDistance(DevicePointer input, DevicePointer output, DevicePointer sites,
                        DevicePointer starts, int count, int lines, float spacing,
                        float thickness, int horizontal) const {
        void* args[]{&input, &output, &sites, &starts, &count, &lines, &spacing, &thickness, &horizontal};
        constexpr unsigned block = 32;
        return !cuda.launch(distance, (lines + block - 1) / block, 1, 1, block, 1, 1,
            0, info.outCommandQueueHandle, args, nullptr);
    }

    bool launchLocalDistance(DevicePointer input, DevicePointer output, int width, int height,
                             float spacing, float thickness, int radius, int horizontal) const {
        void* args[]{&input, &output, &width, &height, &spacing, &thickness, &radius, &horizontal};
        return launch2D(distanceLocal, width, height, args);
    }

    static BevelCudaWeights makeWeights(float thickness, float spacing) {
        const auto weights = bevel::gaussianWeights(thickness, spacing);
        BevelCudaWeights values{};
        values.radius = static_cast<int>(weights.size() / 2);
        std::copy(weights.begin(), weights.end(), values.values);
        return values;
    }

    bool launchBlur(DevicePointer input, DevicePointer output, int width, int height,
                    BevelCudaWeights values, int horizontal) const {
        void* args[]{&input, &output, &width, &height, &values, &horizontal};
        return launch2D(blur, width, height, args);
    }

    bool findBounds(DevicePointer source, BevelCudaParams params, BevelCudaBounds& result) const {
        if (!cuda.upload || !cuda.download || !device->AllocateHostMemory || !device->FreeHostMemory) {
            return false;
        }
        GpuAllocation scratch{device.suite, deviceIndex};
        if (device->AllocateDeviceMemory(deviceIndex, sizeof(result), &scratch.memory) ||
            !scratch.memory) return false;
        void* host = nullptr;
        if (device->AllocateHostMemory(deviceIndex, sizeof(result), &host) || !host) return false;
        auto address = reinterpret_cast<DevicePointer>(scratch.memory);
        auto* bounds = static_cast<BevelCudaBounds*>(host);
        *bounds = {params.width, params.height, 0, 0};
        void* stream = info.outCommandQueueHandle;
        void* args[]{&source, &address, &params};
        const bool launched = !cuda.upload(address, bounds, sizeof(result), stream) &&
            launch2D(boundsKernel, params.width, params.height, args) &&
            !cuda.download(bounds, address, sizeof(result), stream);
        const bool complete = !cuda.sync(stream);
        if (launched && complete) result = *bounds;
        device->FreeHostMemory(deviceIndex, host);
        return launched && complete;
    }

    bool renderSurface(DevicePointer source, DevicePointer destination, BevelCudaParams params) const {
        if (!params.enabled && source == destination) return true;
        GpuAllocation scratch{device.suite, deviceIndex};
        DevicePointer surface = 0;
        bool success = true;
        if (params.enabled) {
            BevelCudaWeights horizontal, vertical;
            try {
                horizontal = makeWeights(params.thickness, params.stepX);
                vertical = makeWeights(params.thickness, params.stepY);
            } catch (...) {
                return false;
            }
            const int width = params.width + 2;
            const int height = params.height + 2;
            const int radiusX = static_cast<int>(params.thickness / params.stepX);
            const int radiusY = static_cast<int>(params.thickness / params.stepY);
            const bool local = std::max(radiusX, radiusY) <= 16;
            const std::size_t bytes = static_cast<std::size_t>(width) * height * sizeof(float);
            if (device->AllocateDeviceMemory(deviceIndex, bytes * (local ? 2 : 4), &scratch.memory) ||
                !scratch.memory) {
                return false;
            }
            surface = reinterpret_cast<DevicePointer>(scratch.memory);
            const auto temp = surface + bytes;
            const auto sites = temp + bytes;
            const auto starts = sites + bytes;
            int transpose = local ? 0 : 1;
            void* seedArgs[]{&source, &surface, &params, &transpose};
            success = launch2D(seed, width, height, seedArgs);
            if (local) {
                success = success &&
                    launchLocalDistance(surface, temp, width, height, params.stepX, params.thickness, radiusX, 1) &&
                    launchLocalDistance(temp, surface, width, height, params.stepY, params.thickness, radiusY, 0);
            } else {
                success = success &&
                    launchDistance(surface, temp, sites, starts, width, height, params.stepX, params.thickness, 1) &&
                    launchDistance(temp, surface, sites, starts, height, width, params.stepY, params.thickness, 0);
            }
            success = success &&
                launchBlur(surface, temp, width, height, horizontal, 1) &&
                launchBlur(temp, surface, width, height, vertical, 0);
        }
        void* shadeArgs[]{&source, &destination, &surface, &params};
        success = success && launch2D(shade, params.width, params.height, shadeArgs);
        // Scratch must remain alive until all submitted work has finished.
        const int completed = cuda.sync(info.outCommandQueueHandle);
        return success && !completed;
    }

    bool renderPixels(DevicePointer source, DevicePointer destination, BevelCudaParams params) const {
        Context context(cuda, info.outContextHandle);
        if (!context.active) return false;
        BevelCudaBounds bounds{};
        if (!params.enabled || static_cast<std::int64_t>(params.width) * params.height < 3840LL * 2160 ||
            !findBounds(source, params, bounds)) {
            return renderSurface(source, destination, params);
        }
        const bool visible = bounds.right > bounds.left && bounds.bottom > bounds.top;
        if (visible) {
            const int padX = bevel::contourPadding(params.thickness, params.stepX);
            const int padY = bevel::contourPadding(params.thickness, params.stepY);
            bounds.left = std::max(0, bounds.left - padX);
            bounds.top = std::max(0, bounds.top - padY);
            bounds.right = std::min(params.width, bounds.right + padX);
            bounds.bottom = std::min(params.height, bounds.bottom + padY);
            if (bounds.left == 0 && bounds.top == 0 &&
                bounds.right == params.width && bounds.bottom == params.height) {
                return renderSurface(source, destination, params);
            }
        }
        // Preserve RGB under zero alpha and handle separate input/output frames.
        auto copy = params;
        copy.enabled = 0;
        if (!renderSurface(source, destination, copy)) return false;
        if (!visible) return true;
        const auto pixelOffset = static_cast<DevicePointer>(bounds.left) * (params.halfPixels ? 8 : 16);
        source += static_cast<DevicePointer>(bounds.top) * params.sourcePitch + pixelOffset;
        destination += static_cast<DevicePointer>(bounds.top) * params.outputPitch + pixelOffset;
        params.width = bounds.right - bounds.left;
        params.height = bounds.bottom - bounds.top;
        return renderSurface(source, destination, params);
    }

    prSuiteError render(const PrGPUFilterRenderParams& frame, PPixHand source, PPixHand& result) const {
        bevel::Settings settings;
        if (!getSettings(frame.inClipTime, settings)) return suiteError_Fail;
        prRect bounds{};
        PrPixelFormat format = PrPixelFormat_Invalid;
        csSDK_int32 sourcePitch = 0;
        void* sourceData = nullptr;
        if (!source || ppix->GetBounds(source, &bounds) || ppix->GetPixelFormat(source, &format) ||
            ppix->GetRowBytes(source, &sourcePitch) || device->GetGPUPPixData(source, &sourceData) ||
            !sourceData) return suiteError_Fail;

        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const bool half = format == PrPixelFormat_GPU_BGRA_4444_16f;
        const int pixelBytes = half ? 8 : 16;
        if ((!half && format != PrPixelFormat_GPU_BGRA_4444_32f) || width <= 0 || height <= 0 ||
            width > 32766 || height > 32766 || sourcePitch < width * pixelBytes ||
            sourcePitch % pixelBytes) return suiteError_Fail;

        PPixHand target = result;
        if (!target && device->CreateGPUPPix(deviceIndex, format, width, height, frame.inRenderPARNum,
                frame.inRenderPARDen, frame.inRenderFieldType, &target)) return suiteError_Fail;
        if (!target) return suiteError_Fail;

        csSDK_int32 outputPitch = 0;
        void* outputData = nullptr;
        prRect outputBounds{};
        PrPixelFormat outputFormat = PrPixelFormat_Invalid;
        const bool valid = !ppix->GetBounds(target, &outputBounds) &&
            outputBounds.right - outputBounds.left == width &&
            outputBounds.bottom - outputBounds.top == height &&
            !ppix->GetPixelFormat(target, &outputFormat) && outputFormat == format &&
            !ppix->GetRowBytes(target, &outputPitch) && outputPitch >= width * pixelBytes &&
            !(outputPitch % pixelBytes) && !device->GetGPUPPixData(target, &outputData) && outputData;
        const auto spacing = [](float scale) {
            return scale >= 1.0f / 64.0f && scale <= 1.0f ? 1.0f / scale : 1.0f;
        };
        constexpr float radiansPerDegree = 0.017453292519943295f;
        const float angle = settings.angle * radiansPerDegree;
        const BevelCudaParams params{width, height, sourcePitch, outputPitch, half,
            settings.enabled(), settings.thickness,
            spacing(frame.inDownsampleFactorX), spacing(frame.inDownsampleFactorY),
            std::sin(angle), -std::cos(angle), settings.intensity, settings.red, settings.green, settings.blue,
            settings.highlight, settings.shadow};
        if (valid && renderPixels(reinterpret_cast<DevicePointer>(sourceData),
                reinterpret_cast<DevicePointer>(outputData), params)) {
            result = target;
            return suiteError_NoError;
        }
        if (!result) ppix->Dispose(target);
        return suiteError_Fail;
    }
};

prSuiteError createInstance(PrGPUFilterInstance* instance) {
    if (!instance) return suiteError_InvalidParms;
    try {
        auto gpu = std::make_unique<GpuInstance>();
        if (!gpu->initialize(*instance)) return suiteError_NotImplemented;
        instance->outIsRealtime = false;
        instance->ioPrivatePluginData = gpu.release();
        return suiteError_NoError;
    } catch (...) {
        return suiteError_Fail;
    }
}

prSuiteError disposeInstance(PrGPUFilterInstance* instance) {
    if (!instance) return suiteError_InvalidParms;
    delete static_cast<GpuInstance*>(instance->ioPrivatePluginData);
    instance->ioPrivatePluginData = nullptr;
    return suiteError_NoError;
}

prSuiteError dependencies(PrGPUFilterInstance*, const PrGPUFilterRenderParams*,
                          csSDK_int32*, PrGPUFilterFrameDependency*) {
    return suiteError_NotImplemented;
}

prSuiteError precompute(PrGPUFilterInstance*, const PrGPUFilterRenderParams*, csSDK_int32, PPixHand) {
    return suiteError_NotImplemented;
}

prSuiteError render(PrGPUFilterInstance* instance, const PrGPUFilterRenderParams* params,
                    const PPixHand* frames, csSDK_size_t count, PPixHand* output) {
    if (!instance || !instance->ioPrivatePluginData || !params || !frames || !count || !output) {
        return suiteError_InvalidParms;
    }
    try {
        return static_cast<GpuInstance*>(instance->ioPrivatePluginData)->render(*params, frames[0], *output);
    } catch (...) {
        return suiteError_Fail;
    }
}

} // namespace

extern "C" __declspec(dllexport) prSuiteError xGPUFilterEntry(
    csSDK_uint32 hostVersion, csSDK_int32* index, prBool startup, piSuitesPtr,
    PrGPUFilter* filter, PrGPUFilterInfo* info) {
    if (!startup) return suiteError_NoError;
    if (!index || *index != 0 || !filter || !info || hostVersion < PrSDKGPUFilterInterfaceVersion) {
        return suiteError_InvalidParms;
    }
    *filter = {createInstance, disposeInstance, dependencies, precompute, render};
    info->outInterfaceVersion = PrSDKGPUFilterInterfaceVersion;
    info->outMatchName = PrSDKString{}; // Associate this path with the CPU effect's PiPL match name.
    return suiteError_NoError;
}
