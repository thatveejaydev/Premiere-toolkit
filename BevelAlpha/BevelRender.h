#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace bevel {

struct Settings {
    float thickness = 8.0f;
    float angle = -45.0f;
    float intensity = 0.30f;
    float red = 1.0f;
    float green = 1.0f;
    float blue = 1.0f;
    float highlight = 1.0f;
    float shadow = 1.0f;

    bool enabled() const {
        return thickness > 0 && intensity > 0 && (highlight > 0 || shadow > 0);
    }
};

template<class Pixel>
struct Frame {
    Pixel* data;
    int width;
    int height;
    std::ptrdiff_t rowBytes;

    Pixel* row(int y) const {
        using Byte = std::conditional_t<std::is_const_v<Pixel>, const char, char>;
        return reinterpret_cast<Pixel*>(reinterpret_cast<Byte*>(data) + y * rowBytes);
    }
};

// Each sample defines a parabola. Its lower envelope gives the exact
// squared distance along this axis, without favoring grid directions.
inline void distanceLine(const float* input, float* output, std::size_t stride,
                         int count, float spacing, int* sites, float* starts) {
    const float spacingSquared = spacing * spacing;
    int last = 0;
    sites[0] = 0;
    starts[0] = -std::numeric_limits<float>::infinity();
    for (int q = 1; q < count; ++q) {
        const float value = input[q * stride];
        float crossing;
        do {
            const int p = sites[last];
            crossing = 0.5f * (static_cast<float>(q) + static_cast<float>(p) +
                (value - input[p * stride]) / (spacingSquared * (q - p)));
            if (crossing > starts[last]) break;
            --last;
        } while (true);
        ++last;
        sites[last] = q;
        starts[last] = crossing;
    }

    int active = 0;
    for (int q = 0; q < count; ++q) {
        while (active < last && starts[active + 1] <= q) ++active;
        const int p = sites[active];
        const float offset = (q - p) * spacing;
        output[q * stride] = input[p * stride] + offset * offset;
    }
}

inline float gaussianSigma(float thickness, float spacing) {
    return std::clamp(0.20f * thickness / spacing, 0.6f, 5.0f);
}

inline int contourPadding(float thickness, float spacing) {
    // Blur support plus the one-pixel gradient tap. Outside the silhouette
    // the distance surface is zero, regardless of the bevel thickness.
    return static_cast<int>(std::ceil(2.5f * gaussianSigma(thickness, spacing))) + 1;
}

inline std::vector<float> gaussianWeights(float thickness, float spacing) {
    const float sigma = gaussianSigma(thickness, spacing);
    const int radius = contourPadding(thickness, spacing) - 1;
    std::vector<float> weights(2 * radius + 1);
    float total = 0;
    for (int i = -radius; i <= radius; ++i) {
        const float scaled = i / sigma;
        weights[i + radius] = std::exp(-0.5f * scaled * scaled);
        total += weights[i + radius];
    }
    for (auto& weight : weights) weight /= total;
    return weights;
}

template<class CheckAbort>
auto smoothSurface(std::vector<float>& surface, std::vector<float>& scratch,
                   int width, int height, float thickness, float stepX, float stepY,
                   CheckAbort checkAbort) -> decltype(checkAbort()) {
    const auto horizontal = gaussianWeights(thickness, stepX);
    const auto vertical = gaussianWeights(thickness, stepY);
    const int radiusX = static_cast<int>(horizontal.size() / 2);
    const int radiusY = static_cast<int>(vertical.size() / 2);
    for (int y = 0; y < height; ++y) {
        if (const auto error = checkAbort()) return error;
        const std::size_t row = static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            float sum = 0;
            for (int tap = -radiusX; tap <= radiusX; ++tap) {
                sum += horizontal[tap + radiusX] * surface[row + std::clamp(x + tap, 0, width - 1)];
            }
            scratch[row + x] = sum;
        }
    }
    for (int y = 0; y < height; ++y) {
        if (const auto error = checkAbort()) return error;
        const std::size_t row = static_cast<std::size_t>(y) * width;
        float* output = surface.data() + row;
        std::fill_n(output, width, 0.0f);
        for (int tap = -radiusY; tap <= radiusY; ++tap) {
            const std::size_t sampleRow = std::clamp(y + tap, 0, height - 1);
            const float* input = scratch.data() + sampleRow * width;
            const float weight = vertical[tap + radiusY];
            // Contiguous rows let the compiler process multiple pixels at once.
            for (int x = 0; x < width; ++x) {
                output[x] += weight * input[x];
            }
        }
    }
    return {};
}

// Render an inward bevel from a rounded, smoothed distance surface.
template<unsigned MaxChannel, bool Premultiplied = true, class Pixel, class CheckAbort>
auto renderSurface(Frame<const Pixel> input, Frame<Pixel> output, Settings settings,
            float stepX, float stepY, CheckAbort checkAbort) -> decltype(checkAbort()) {
    const int width = input.width;
    const int height = input.height;
    const bool enabled = settings.enabled();
    const std::size_t stride = static_cast<std::size_t>(width) + 2;
    const std::size_t rows = static_cast<std::size_t>(height) + 2;
    std::vector<float> surface;

    if (enabled && width > 0 && height > 0) {
        if (stride > surface.max_size() / rows ||
            stride > std::numeric_limits<int>::max() - 13 ||
            rows > std::numeric_limits<int>::max() - 13) {
            throw std::length_error("Frame is too large");
        }
        surface.resize(stride * rows);
        std::vector<float> scratch(surface.size());
        std::vector<int> sites(std::max(stride, rows));
        std::vector<float> starts(sites.size());
        const float limitSquared = settings.thickness * settings.thickness;
        const float coverageStep = std::min(stepX, stepY);
        for (int y = 0; y < height; ++y) {
            if (const auto error = checkAbort()) return error;
            const Pixel* source = input.row(y);
            float* cell = surface.data() + (static_cast<std::size_t>(y) + 1) * stride + 1;
            for (int x = 0; x < width; ++x, ++cell) {
                const auto alpha = source[x].alpha;
                // Only use partial alpha as a distance seed at the contour.
                // A uniformly translucent interior still gets a full-width bevel.
                float seedSquared = limitSquared;
                if (!(alpha > 0)) {
                    seedSquared = 0;
                } else if (alpha < MaxChannel &&
                           (x == 0 || x == width - 1 || y == 0 || y == height - 1 ||
                            source[x - 1].alpha <= 0 || source[x + 1].alpha <= 0 ||
                            input.row(y - 1)[x].alpha <= 0 || input.row(y + 1)[x].alpha <= 0)) {
                    const float edge = alpha * (coverageStep / MaxChannel);
                    seedSquared = std::min(edge * edge, limitSquared);
                }
                *cell = seedSquared;
            }
        }
        for (std::size_t y = 0; y < rows; ++y) {
            if (const auto error = checkAbort()) return error;
            const std::size_t row = y * stride;
            distanceLine(surface.data() + row, scratch.data() + row, 1,
                         static_cast<int>(stride), stepX, sites.data(), starts.data());
        }
        for (std::size_t x = 0; x < stride; ++x) {
            if (const auto error = checkAbort()) return error;
            distanceLine(scratch.data() + x, surface.data() + x, stride,
                         static_cast<int>(rows), stepY, sites.data(), starts.data());
        }
        for (std::size_t y = 0; y < rows; ++y) {
            if (const auto error = checkAbort()) return error;
            float* row = surface.data() + y * stride;
            for (std::size_t x = 0; x < stride; ++x) {
                const float t = std::sqrt(std::min(row[x], limitSquared)) / settings.thickness;
                row[x] = t * (2.0f - t) * settings.thickness;
            }
        }
        // Smooth before taking gradients so lighting stays continuous where
        // the nearest contour changes, especially at corners and thin strokes.
        if (const auto error = smoothSurface(surface, scratch, static_cast<int>(stride),
                static_cast<int>(rows), settings.thickness, stepX, stepY, checkAbort)) {
            return error;
        }
    }

    constexpr float radiansPerDegree = 0.017453292519943295f;
    const float lightX = std::sin(settings.angle * radiansPerDegree);
    const float lightY = -std::cos(settings.angle * radiansPerDegree);

    for (int y = 0; y < output.height; ++y) {
        if (const auto error = checkAbort()) return error;
        Pixel* dest = output.row(y);
        const int count = y < height ? std::min(width, output.width) : 0;
        if (count > 0) {
            const Pixel* source = input.row(y);
            for (int x = 0; x < count; ++x) {
                Pixel pixel = source[x];
                if (!surface.empty() && pixel.alpha > 0) {
                    const float* cell = surface.data() +
                        (static_cast<std::size_t>(y) + 1) * stride + x + 1;
                    const float dx = (cell[1] - cell[-1]) / (2 * stepX);
                    const float dy = (*(cell + stride) - *(cell - stride)) / (2 * stepY);
                    const float directional = -settings.intensity * (dx * lightX + dy * lightY) /
                                               std::sqrt(1.0f + dx * dx + dy * dy);
                    const float strength = directional > 0 ? settings.highlight : settings.shadow;
                    const float light = std::clamp(directional * strength, -1.0f, 1.0f);
                    const auto shade = [&](auto channel, float tint) {
                        const float white = Premultiplied ? pixel.alpha : MaxChannel;
                        const float target = light > 0 ? white * tint : 0.0f;
                        const float mixed = light == 0 ? channel :
                            channel + std::abs(light) * (target - channel);
                        if constexpr (std::is_floating_point_v<decltype(channel)>) {
                            return mixed;
                        } else {
                            return static_cast<decltype(channel)>(
                                std::clamp(mixed, 0.0f, static_cast<float>(MaxChannel)) + 0.5f);
                        }
                    };
                    pixel.red = shade(pixel.red, settings.red);
                    pixel.green = shade(pixel.green, settings.green);
                    pixel.blue = shade(pixel.blue, settings.blue);
                }
                dest[x] = pixel;
            }
        }
        std::fill(dest + count, dest + output.width, Pixel{});
    }
    return {};
}

template<unsigned MaxChannel, bool Premultiplied = true, class Pixel, class CheckAbort>
auto render(Frame<const Pixel> input, Frame<Pixel> output, Settings settings,
            float stepX, float stepY, CheckAbort checkAbort) -> decltype(checkAbort()) {
    const int width = input.width;
    const int height = input.height;
    if (!settings.enabled() || width <= 0 || height <= 0 ||
        width != output.width || height != output.height ||
        (input.row(0)[0].alpha > 0 && input.row(height - 1)[width - 1].alpha > 0)) {
        return renderSurface<MaxChannel, Premultiplied>(input, output, settings, stepX, stepY, checkAbort);
    }

    int left = width, top = height, right = 0, bottom = 0;
    for (int y = 0; y < height; ++y) {
        if (const auto error = checkAbort()) return error;
        const Pixel* row = input.row(y);
        int first = 0;
        while (first < width && !(row[first].alpha > 0)) ++first;
        if (first == width) continue;
        int last = width - 1;
        while (last > first && !(row[last].alpha > 0)) --last;
        left = std::min(left, first);
        right = std::max(right, last + 1);
        top = std::min(top, y);
        bottom = y + 1;
    }

    const bool visible = right > left && bottom > top;
    if (visible) {
        const int padX = contourPadding(settings.thickness, stepX);
        const int padY = contourPadding(settings.thickness, stepY);
        left = std::max(0, left - padX);
        top = std::max(0, top - padY);
        right += std::min(padX, width - right);
        bottom += std::min(padY, height - bottom);
        if (left == 0 && top == 0 && right == width && bottom == height) {
            return renderSurface<MaxChannel, Premultiplied>(input, output, settings, stepX, stepY, checkAbort);
        }
    }

    for (int y = 0; y < height; ++y) {
        if (const auto error = checkAbort()) return error;
        const auto* source = input.row(y);
        auto* destination = output.row(y);
        if (source == destination) continue;
        if (visible && y >= top && y < bottom) {
            std::copy_n(source, left, destination);
            std::copy_n(source + right, width - right, destination + right);
        } else {
            std::copy_n(source, width, destination);
        }
    }
    if (!visible) return {};

    const Frame<const Pixel> croppedInput{input.row(top) + left, right - left, bottom - top, input.rowBytes};
    const Frame<Pixel> croppedOutput{output.row(top) + left, right - left, bottom - top, output.rowBytes};
    return renderSurface<MaxChannel, Premultiplied>(croppedInput, croppedOutput, settings,
                                                   stepX, stepY, checkAbort);
}

} // namespace bevel
