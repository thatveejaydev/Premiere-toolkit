#include "BevelCudaParams.h"

__device__ float halfToFloat(unsigned short bits) {
    float value;
    asm("cvt.f32.f16 %0, %1;" : "=f"(value) : "h"(bits));
    return value;
}

__device__ unsigned short floatToHalf(float value) {
    unsigned short bits;
    asm("cvt.rn.f16.f32 %0, %1;" : "=h"(bits) : "f"(value));
    return bits;
}

__device__ float alphaAt(const char* source, BevelCudaParams params, int x, int y) {
    const char* pixel = source + static_cast<long long>(y) * params.sourcePitch +
        x * (params.halfPixels ? 8 : 16);
    return params.halfPixels ? halfToFloat(reinterpret_cast<const ushort4*>(pixel)->w)
                             : reinterpret_cast<const float4*>(pixel)->w;
}

extern "C" __global__ void bevelBounds(const char* source, BevelCudaBounds* bounds,
                                       BevelCudaParams params) {
    // Two opposite occupied corners already guarantee full-frame bounds.
    if (alphaAt(source, params, 0, 0) > 0 &&
        alphaAt(source, params, params.width - 1, params.height - 1) > 0) {
        if (blockIdx.x == 0 && blockIdx.y == 0 && threadIdx.x == 0 && threadIdx.y == 0) {
            *bounds = {0, 0, params.width, params.height};
        }
        return;
    }
    __shared__ BevelCudaBounds tile;
    if (threadIdx.x == 0 && threadIdx.y == 0) tile = {params.width, params.height, 0, 0};
    __syncthreads();
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const bool visible = x < params.width && y < params.height && alphaAt(source, params, x, y) > 0;
    const unsigned mask = __ballot_sync(0xffffffffu, visible);
    if (threadIdx.x == 0 && mask) {
        atomicMin(&tile.left, x + __ffs(mask) - 1);
        atomicMax(&tile.right, x + 32 - __clz(mask));
        atomicMin(&tile.top, y);
        atomicMax(&tile.bottom, y + 1);
    }
    __syncthreads();
    if (threadIdx.x == 0 && threadIdx.y == 0 && tile.right > tile.left) {
        atomicMin(&bounds->left, tile.left);
        atomicMin(&bounds->top, tile.top);
        atomicMax(&bounds->right, tile.right);
        atomicMax(&bounds->bottom, tile.bottom);
    }
}

extern "C" __global__ void bevelSeed(const char* source, float* surface, BevelCudaParams params,
                                     int transpose) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int width = params.width + 2;
    const int height = params.height + 2;
    if (x >= width || y >= height) return;
    float distance = 0.0f;
    if (x > 0 && x < width - 1 && y > 0 && y < height - 1) {
        const int sx = x - 1;
        const int sy = y - 1;
        const float alpha = alphaAt(source, params, sx, sy);
        if (alpha > 0.0f) {
            distance = params.thickness;
            if (alpha < 1.0f &&
                (sx == 0 || sx == params.width - 1 || sy == 0 || sy == params.height - 1 ||
                 alphaAt(source, params, sx - 1, sy) <= 0.0f ||
                 alphaAt(source, params, sx + 1, sy) <= 0.0f ||
                 alphaAt(source, params, sx, sy - 1) <= 0.0f ||
                 alphaAt(source, params, sx, sy + 1) <= 0.0f)) {
                distance = fminf(distance, alpha * fminf(params.stepX, params.stepY));
            }
        }
    }
    surface[transpose ? x * height + y : y * width + x] = distance * distance;
}

// For short distances, a bounded search gives the same exact transform
// while using one thread per pixel. Seeds outside the radius cannot win.
extern "C" __global__ void bevelDistanceLocal(const float* input, float* output, int width,
                                              int height, float spacing, float thickness,
                                              int radius, int horizontal) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    const int pixel = y * width + x;
    const int position = horizontal ? x : y;
    const int last = horizontal ? width - 1 : height - 1;
    const int stride = horizontal ? 1 : width;
    float squared = fminf(input[pixel], thickness * thickness);
    if (squared > 0.0f) {
        for (int tap = max(-radius, -position); tap <= min(radius, last - position); ++tap) {
            const float offset = tap * spacing;
            squared = fminf(squared, input[pixel + tap * stride] + offset * offset);
        }
    }
    if (horizontal) {
        output[pixel] = squared;
    } else {
        const float t = sqrtf(squared) / thickness;
        output[pixel] = t * (2.0f - t) * thickness;
    }
}

// One thread solves each lower envelope. Adjacent threads access adjacent
// lines at a given position. Transpose between axes for coalesced reads.
extern "C" __global__ void bevelDistance(const float* input, float* output, int* sites,
                                        float* starts, int count, int lines,
                                        float spacing, float thickness, int horizontal) {
    const int line = blockIdx.x * blockDim.x + threadIdx.x;
    if (line >= lines) return;
    input += line;
    sites += line;
    starts += line;
    const float spacingSquared = spacing * spacing;
    int last = 0;
    sites[0] = 0;
    starts[0] = -__int_as_float(0x7f800000);
    for (int q = 1; q < count; ++q) {
        const float value = input[q * lines];
        float crossing;
        for (;;) {
            const int p = sites[last * lines];
            crossing = 0.5f * (static_cast<float>(q + p) +
                (value - input[p * lines]) / (spacingSquared * (q - p)));
            if (crossing > starts[last * lines]) break;
            --last;
        }
        ++last;
        sites[last * lines] = q;
        starts[last * lines] = crossing;
    }

    int active = 0;
    for (int q = 0; q < count; ++q) {
        while (active < last && starts[(active + 1) * lines] <= q) ++active;
        const int p = sites[active * lines];
        const float offset = (q - p) * spacing;
        const float squared = input[p * lines] + offset * offset;
        if (horizontal) {
            output[line * count + q] = squared;
        } else {
            const float t = sqrtf(fminf(squared, thickness * thickness)) / thickness;
            output[q * lines + line] = t * (2.0f - t) * thickness;
        }
    }
}

extern "C" __global__ void bevelBlur(const float* input, float* output, int width, int height,
                                     BevelCudaWeights weights, int horizontal) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    float sum = 0.0f;
    for (int tap = -weights.radius; tap <= weights.radius; ++tap) {
        const int sx = horizontal ? max(0, min(x + tap, width - 1)) : x;
        const int sy = horizontal ? y : max(0, min(y + tap, height - 1));
        sum += weights.values[tap + weights.radius] * input[sy * width + sx];
    }
    output[y * width + x] = sum;
}

__device__ float shadeChannel(float value, float tint, float alpha, float light) {
    if (light == 0.0f) return value;
    const float target = light > 0.0f ? alpha * tint : 0.0f;
    return value + fabsf(light) * (target - value);
}

extern "C" __global__ void bevelShade(const char* source, char* destination, const float* surface,
                                      BevelCudaParams params) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= params.width || y >= params.height) return;
    const int bytes = params.halfPixels ? 8 : 16;
    const char* src = source + static_cast<long long>(y) * params.sourcePitch + x * bytes;
    char* dst = destination + static_cast<long long>(y) * params.outputPitch + x * bytes;
    const float alpha = params.halfPixels ? halfToFloat(reinterpret_cast<const ushort4*>(src)->w)
                                          : reinterpret_cast<const float4*>(src)->w;
    if (!params.enabled || !(alpha > 0.0f)) {
        if (params.halfPixels) *reinterpret_cast<ushort4*>(dst) = *reinterpret_cast<const ushort4*>(src);
        else *reinterpret_cast<float4*>(dst) = *reinterpret_cast<const float4*>(src);
        return;
    }

    const int pitch = params.width + 2;
    const float* cell = surface + (y + 1) * pitch + x + 1;
    const float dx = (cell[1] - cell[-1]) / (2.0f * params.stepX);
    const float dy = (cell[pitch] - cell[-pitch]) / (2.0f * params.stepY);
    const float directional = -params.intensity * (dx * params.lightX + dy * params.lightY) /
                              sqrtf(1.0f + dx * dx + dy * dy);
    const float strength = directional > 0.0f ? params.highlight : params.shadow;
    const float light = fminf(fmaxf(directional * strength, -1.0f), 1.0f);
    if (params.halfPixels) {
        auto pixel = *reinterpret_cast<const ushort4*>(src);
        pixel.x = floatToHalf(shadeChannel(halfToFloat(pixel.x), params.blue, alpha, light));
        pixel.y = floatToHalf(shadeChannel(halfToFloat(pixel.y), params.green, alpha, light));
        pixel.z = floatToHalf(shadeChannel(halfToFloat(pixel.z), params.red, alpha, light));
        *reinterpret_cast<ushort4*>(dst) = pixel;
    } else {
        auto pixel = *reinterpret_cast<const float4*>(src);
        pixel.x = shadeChannel(pixel.x, params.blue, alpha, light);
        pixel.y = shadeChannel(pixel.y, params.green, alpha, light);
        pixel.z = shadeChannel(pixel.z, params.red, alpha, light);
        *reinterpret_cast<float4*>(dst) = pixel;
    }
}
