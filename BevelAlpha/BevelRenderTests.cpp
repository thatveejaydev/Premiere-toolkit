#include "BevelRender.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

template<class Channel>
struct Pixel {
    Channel alpha, red, green, blue;
};

template<class Channel>
bool same(Pixel<Channel> a, Pixel<Channel> b) {
    return a.alpha == b.alpha && a.red == b.red && a.green == b.green && a.blue == b.blue;
}

template<unsigned MaxChannel, class Channel>
bool checkRender() {
    constexpr int size = 38;
    constexpr int pitch = size + 3;
    constexpr int first = 3;
    constexpr int last = size - 4;
    constexpr int middle = size / 2;
    const Pixel<Channel> gray{MaxChannel, MaxChannel / 2, MaxChannel / 2, MaxChannel / 2};
    const Pixel<Channel> partial{MaxChannel / 2, MaxChannel / 4, MaxChannel / 4, MaxChannel / 4};
    const Pixel<Channel> marker{0, 1, 2, 3};
    std::array<Pixel<Channel>, pitch * size> source{}, output;
    output.fill(marker);
    for (int y = first; y <= last; ++y) {
        for (int x = first; x <= last; ++x) {
            source[y * pitch + x] = gray;
        }
    }
    source[middle * pitch + first - 1] = partial;

    const bevel::Frame<const Pixel<Channel>> input{
        source.data(), size, size, pitch * sizeof(gray)};
    const bevel::Frame<Pixel<Channel>> dest{
        output.data(), size, size, pitch * sizeof(gray)};
    const auto neverAbort = [] { return 0; };
    const auto pixel = [&](int x, int y) { return output[y * pitch + x]; };
    const auto run = [&](bevel::Settings settings, float spacing = 1.0f) {
        return bevel::render<MaxChannel>(input, dest, settings, spacing, spacing, neverAbort);
    };

    if (run({}) || pixel(first, middle - 3).red <= gray.red ||
        pixel(middle, first).red <= gray.red || pixel(last, middle).red >= gray.red ||
        pixel(middle, last).red >= gray.red || !same(pixel(middle, middle), gray)) {
        std::cerr << "Default lighting direction or interior failed\n";
        return false;
    }
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const auto original = source[y * pitch + x];
            if (pixel(x, y).alpha != original.alpha ||
                (original.alpha == 0 && !same(pixel(x, y), original))) {
                std::cerr << "Alpha preservation failed\n";
                return false;
            }
        }
        if (!same(pixel(size, y), marker)) {
            std::cerr << "Row padding changed\n";
            return false;
        }
    }

    bevel::Settings settings;
    settings.angle = 135;
    if (run(settings) || pixel(middle, first).red >= gray.red ||
        pixel(middle, last).red <= gray.red) {
        std::cerr << "Light angle did not reverse the bevel\n";
        return false;
    }

    settings = {};
    settings.green = 0.0f;
    settings.blue = 0.0f;
    if (run(settings) || pixel(middle, first).red <= gray.red ||
        pixel(middle, first).green >= gray.green) {
        std::cerr << "Highlight tint failed\n";
        return false;
    }

    settings = {};
    settings.highlight = 0;
    if (run(settings) || !same(pixel(middle, first), gray) ||
        pixel(middle, last).red >= gray.red) {
        std::cerr << "Zero highlight strength affected the shadow\n";
        return false;
    }
    settings.highlight = 1;
    settings.shadow = 0;
    if (run(settings) || pixel(middle, first).red <= gray.red ||
        !same(pixel(middle, last), gray)) {
        std::cerr << "Zero shadow strength affected the highlight\n";
        return false;
    }

    if (run({}, 2.0f)) return false;
    const auto downsampled = pixel(middle, first + 5).red;
    if (run({}) || pixel(middle, first + 5).red - gray.red <=
        2 * (downsampled - gray.red)) {
        std::cerr << "Downsample spacing did not adjust the width in pixels\n";
        return false;
    }

    for (const bevel::Settings bypass : {bevel::Settings{0.0f}, bevel::Settings{8.0f, -45.0f, 0.0f}}) {
        if (run(bypass)) return false;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (!same(pixel(x, y), source[y * pitch + x])) {
                    std::cerr << "Zero thickness or intensity changed a pixel\n";
                    return false;
                }
            }
        }
    }

    for (auto& sample : source) {
        if (sample.alpha > 0) sample = partial;
    }
    if (run({}) || pixel(middle, first + 4).red <= partial.red ||
        !same(pixel(middle, middle), partial)) {
        std::cerr << "Translucent interior did not get a full-width bevel\n";
        return false;
    }

    source.fill(gray);
    if (run({}) || pixel(0, middle).red <= gray.red ||
        pixel(size - 1, middle).red >= gray.red || !same(pixel(middle, middle), gray)) {
        std::cerr << "Opaque frame border failed\n";
        return false;
    }

    if (bevel::render<MaxChannel>(input, dest, {}, 1.0f, 1.0f, [] { return 17; }) != 17) {
        std::cerr << "Cancellation was not propagated\n";
        return false;
    }
    return true;
}

bool checkSparseRegion() {
    constexpr int width = 83, height = 64, pitch = width + 3;
    using Pixel8 = Pixel<std::uint8_t>;
    std::array<Pixel8, pitch * height> input, full{}, cropped{};
    input.fill({0, 73, 12, 9});
    for (int y = 0; y < 17; ++y) {
        for (int x = 24; x < 46; ++x) {
            input[y * pitch + x] = {static_cast<std::uint8_t>(x == 24 ? 128 : 255), 55, 20, 12};
        }
    }
    const bevel::Frame<const Pixel8> source{input.data(), width, height, pitch * sizeof(Pixel8)};
    const auto abort = [] { return 0; };
    bevel::renderSurface<255>(source, bevel::Frame<Pixel8>{full.data(), width, height,
        pitch * sizeof(Pixel8)}, {}, 1.0f, 2.0f, abort);
    bevel::render<255>(source, bevel::Frame<Pixel8>{cropped.data(), width, height,
        pitch * sizeof(Pixel8)}, {}, 1.0f, 2.0f, abort);
    for (std::size_t i = 0; i < full.size(); ++i) {
        if (!same(full[i], cropped[i])) {
            std::cerr << "Sparse render differs from the full frame\n";
            return false;
        }
    }
    return true;
}

bool checkFloatColor() {
    constexpr int size = 32;
    const Pixel<float> color{0.5f, 1.25f, -0.5f, 0.5003f};
    std::array<Pixel<float>, size * size> source, output{};
    source.fill(color);
    const auto error = bevel::render<1, false>(
        bevel::Frame<const Pixel<float>>{source.data(), size, size, size * sizeof(color)},
        bevel::Frame<Pixel<float>>{output.data(), size, size, size * sizeof(color)},
        {}, 1.0f, 1.0f, [] { return 0; });
    const auto highlight = output[size / 2];
    const auto center = output[(size / 2) * size + size / 2];
    if (error || highlight.alpha != color.alpha || highlight.blue <= color.blue ||
        highlight.red <= 1.0f || highlight.green >= 0.0f || !same(center, color)) {
        std::cerr << "Float rendering clipped headroom or changed the alpha\n";
        return false;
    }
    return true;
}

bool checkRoundedContour() {
    constexpr int size = 192;
    constexpr double center = (size - 1) / 2.0;
    constexpr double radius = 82.0;
    constexpr double radiansPerDegree = 0.017453292519943295;
    using Pixel8 = Pixel<std::uint8_t>;
    std::vector<Pixel8> input(size * size), output(size * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double distance = std::hypot(x - center, y - center);
            const double coverage = std::clamp(radius + 0.5 - distance, 0.0, 1.0);
            const auto alpha = static_cast<std::uint8_t>(std::lround(255 * coverage));
            input[y * size + x] = {alpha, static_cast<std::uint8_t>(alpha / 2), 0, 0};
        }
    }
    bevel::Settings settings;
    settings.intensity = 1.0f; // Make lighting ridges easy to measure.
    bevel::render<255>(bevel::Frame<const Pixel8>{input.data(), size, size, size * sizeof(Pixel8)},
        bevel::Frame<Pixel8>{output.data(), size, size, size * sizeof(Pixel8)},
        settings, 1.0f, 1.0f, [] { return 0; });

    const auto red = [&](int x, int y) { return output[y * size + x].red; };
    double previous = 0;
    for (int degree = 0; degree <= 360; ++degree) {
        const double angle = degree * radiansPerDegree;
        const double x = center + (radius - 3) * std::cos(angle);
        const double y = center + (radius - 3) * std::sin(angle);
        const int left = static_cast<int>(x);
        const int top = static_cast<int>(y);
        const double dx = x - left;
        const double dy = y - top;
        const double value = (1 - dy) * ((1 - dx) * red(left, top) + dx * red(left + 1, top)) +
            dy * ((1 - dx) * red(left, top + 1) + dx * red(left + 1, top + 1));
        if (degree > 0 && std::abs(value - previous) > 8.0) {
            std::cerr << "Rounded contour has a sharp lighting jump\n";
            return false;
        }
        previous = value;
    }
    return true;
}

} // namespace

int main() {
    if (!checkRender<255, std::uint8_t>() || !checkRender<32768, std::uint16_t>() ||
        !checkFloatColor() || !checkRoundedContour() || !checkSparseRegion()) {
        return 1;
    }
    std::cout << "Bevel renderer checks passed for 8-bit, 16-bit, and 32-bit float pixels.\n";
}
