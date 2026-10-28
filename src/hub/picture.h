#pragma once

// Pictures cut, scaled, laid over one another and filtered on the host, as a
// paint program would: what the pre-show's venue posters are made with
// (hub/venue_posters.h). Each pixel r g b a, 0 to 255 as floats, its
// colour not multiplied by its alpha. Each call follows the Python Imaging
// Library's of the same name, which the posters were first made in.

#include "render/scene.h"

#include <cstdint>
#include <vector>

namespace gh2::picture
{
    struct Image
    {
        int w = 0, h = 0;
        std::vector<float> px; // w * h * 4

        Image() = default;
        Image(int width, int height); // clear

        float *at(int x, int y) { return px.data() + (size_t(y) * w + x) * 4u; }
        const float *at(int x, int y) const { return px.data() + (size_t(y) * w + x) * 4u; }
    };

    // One value a pixel: a mask, a brightness.
    struct Plane
    {
        int w = 0, h = 0;
        std::vector<float> v;

        Plane() = default;
        Plane(int width, int height, float fill = 0.0f);

        float &at(int x, int y) { return v[size_t(y) * w + x]; }
        float at(int x, int y) const { return v[size_t(y) * w + x]; }
    };

    Image fromTexture(const TextureData &texture);
    // 8 bits a channel, rounded and clamped.
    std::vector<uint8_t> toRgba(const Image &image);

    // The box from (x0, y0) to (x1, y1), clear past `image`'s edges.
    Image crop(const Image &image, int x0, int y0, int x1, int y1);

    enum class Filter
    {
        Bicubic,
        Lanczos,
    };
    // Scaled to w by h, each way on its own, its colour weighed by its alpha.
    Image resize(const Image &image, int w, int h, Filter filter = Filter::Bicubic);
    Plane resize(const Plane &plane, int w, int h, Filter filter = Filter::Bicubic);

    Image flipLeftRight(const Image &image);
    Image flipTopBottom(const Image &image);

    // `src` put at (x, y): its pixels in place of `dst`'s, alpha and all.
    void paste(Image &dst, const Image &src, int x, int y);
    // Only as far as `mask` (src's size, 0 to 1): each channel between them.
    void paste(Image &dst, const Image &src, int x, int y, const Plane &mask);
    // `src` over `dst` at (x, y), as alpha_composite lays it.
    void over(Image &dst, const Image &src, int x = 0, int y = 0);

    // The box round the pixels whose alpha is above 0, as (x0, y0, x1, y1),
    // x1 and y1 one past; all 0 when there are none.
    struct Box
    {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int w() const { return x1 - x0; }
        int h() const { return y1 - y0; }
    };
    Box bbox(const Image &image);

    // Turned `degrees` anticlockwise about its middle, bicubic, its canvas
    // grown to hold it all.
    Image rotate(const Image &image, float degrees);

    // Blurred by a Gaussian of that standard deviation, each channel alike.
    void blur(Image &image, float sigma);
    void blur(Plane &plane, float sigma);
    // Each pixel the largest, or the middle, of the size by size square
    // round it (size odd), clamped at the edges.
    Plane maxFilter(const Plane &plane, int size);
    // r, g and b each the middle of its square; alpha kept.
    Image medianFilter(const Image &image, int size);

    // Brightness as ITU-R 601-2 weighs it (convert "L"), and as the mean of
    // the three.
    Plane luma(const Image &image);
    Plane mean(const Image &image);

    // A pixel's r g b a at a point between pixels, bilinear, clamped.
    void sample(const Image &image, float x, float y, float out[4]);
}
