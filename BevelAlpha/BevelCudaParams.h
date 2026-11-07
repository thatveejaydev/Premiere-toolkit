#pragma once

struct BevelCudaBounds {
    int left, top, right, bottom;
};

struct BevelCudaParams {
    int width;
    int height;
    int sourcePitch;
    int outputPitch;
    int halfPixels;
    int enabled;
    float thickness;
    float stepX;
    float stepY;
    float lightX;
    float lightY;
    float intensity;
    float red;
    float green;
    float blue;
    float highlight;
    float shadow;
};

struct BevelCudaWeights {
    int radius;
    float values[27]; // 2 * ceil(2.5 * maximum sigma 5) + 1
};
