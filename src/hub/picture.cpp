#include "hub/picture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace gh2::picture
{
    namespace
    {
        float bicubic(float x)
        {
            constexpr float a = -0.5f;
            x = std::fabs(x);
            if (x < 1.0f)
                return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f;
            if (x < 2.0f)
                return ((a * x - 5.0f * a) * x + 8.0f * a) * x - 4.0f * a;
            return 0.0f;
        }

        float sinc(float x)
        {
            if (x == 0.0f)
                return 1.0f;
            x *= std::numbers::pi_v<float>;
            return std::sin(x) / x;
        }

        float lanczos(float x)
        {
            return std::fabs(x) < 3.0f ? sinc(x) * sinc(x / 3.0f) : 0.0f;
        }

        // Each output pixel's first input pixel and weights along one axis,
        // as ImagingResample spreads them: the filter widened by the
        // shrink, centred on the output pixel's middle.
        struct Taps
        {
            std::vector<int> first;
            std::vector<std::vector<float>> weights;
        };

        Taps taps(int in, int out, Filter filter)
        {
            const float support0 = filter == Filter::Lanczos ? 3.0f : 2.0f;
            const float scale = float(in) / float(out);
            const float filterScale = std::max(scale, 1.0f);
            const float support = support0 * filterScale;
            Taps t;
            t.first.resize(size_t(out));
            t.weights.resize(size_t(out));
            for (int i = 0; i < out; ++i)
            {
                const float centre = (float(i) + 0.5f) * scale;
                const int x0 = std::max(int(centre - support + 0.5f), 0);
                const int x1 = std::min(int(centre + support + 0.5f), in);
                std::vector<float> w;
                float sum = 0.0f;
                for (int x = x0; x < x1; ++x)
                {
                    const float d = (float(x) - centre + 0.5f) / filterScale;
                    const float k = filter == Filter::Lanczos ? lanczos(d) : bicubic(d);
                    w.push_back(k);
                    sum += k;
                }
                if (sum != 0.0f)
                    for (float &k : w)
                        k /= sum;
                t.first[size_t(i)] = x0;
                t.weights[size_t(i)] = std::move(w);
            }
            return t;
        }

        // A separable pass over `channels` floats a pixel, rows then columns.
        std::vector<float> resample(const std::vector<float> &in, int w, int h, int channels, int nw, int nh,
                                    Filter filter)
        {
            const Taps across = taps(w, nw, filter), down = taps(h, nh, filter);
            std::vector<float> mid(size_t(nw) * h * channels, 0.0f);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < nw; ++x)
                {
                    const auto &wt = across.weights[size_t(x)];
                    float *o = mid.data() + (size_t(y) * nw + x) * channels;
                    for (size_t k = 0; k < wt.size(); ++k)
                    {
                        const float *p = in.data() + (size_t(y) * w + across.first[size_t(x)] + k) * channels;
                        for (int c = 0; c < channels; ++c)
                            o[c] += p[c] * wt[k];
                    }
                }
            std::vector<float> out(size_t(nw) * nh * channels, 0.0f);
            for (int y = 0; y < nh; ++y)
            {
                const auto &wt = down.weights[size_t(y)];
                for (size_t k = 0; k < wt.size(); ++k)
                {
                    const float *row = mid.data() + size_t(down.first[size_t(y)] + int(k)) * nw * channels;
                    float *o = out.data() + size_t(y) * nw * channels;
                    for (int i = 0; i < nw * channels; ++i)
                        o[i] += row[i] * wt[k];
                }
            }
            return out;
        }

        std::vector<float> premultiplied(const Image &image)
        {
            std::vector<float> p = image.px;
            for (size_t i = 0; i < p.size(); i += 4)
                for (int c = 0; c < 3; ++c)
                    p[i + c] *= p[i + 3] / 255.0f;
            return p;
        }

        Image unpremultiplied(std::vector<float> p, int w, int h)
        {
            Image out;
            out.w = w;
            out.h = h;
            for (size_t i = 0; i < p.size(); i += 4)
            {
                p[i + 3] = std::clamp(p[i + 3], 0.0f, 255.0f);
                const float a = p[i + 3];
                for (int c = 0; c < 3; ++c)
                    p[i + c] = a > 0.0f ? std::clamp(p[i + c] * 255.0f / a, 0.0f, 255.0f) : 0.0f;
            }
            out.px = std::move(p);
            return out;
        }

        // One channel blurred along rows then columns, clamped at the edges.
        void blurChannel(float *data, int w, int h, int stride, float sigma)
        {
            if (sigma <= 0.0f)
                return;
            const int r = int(std::ceil(sigma * 3.0f));
            std::vector<float> k(size_t(2 * r + 1));
            float sum = 0.0f;
            for (int i = -r; i <= r; ++i)
                sum += k[size_t(i + r)] = std::exp(-float(i * i) / (2.0f * sigma * sigma));
            for (float &v : k)
                v /= sum;
            std::vector<float> line(size_t(std::max(w, h)));
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    float acc = 0.0f;
                    for (int i = -r; i <= r; ++i)
                        acc += data[(size_t(y) * w + std::clamp(x + i, 0, w - 1)) * stride] * k[size_t(i + r)];
                    line[size_t(x)] = acc;
                }
                for (int x = 0; x < w; ++x)
                    data[(size_t(y) * w + x) * stride] = line[size_t(x)];
            }
            for (int x = 0; x < w; ++x)
            {
                for (int y = 0; y < h; ++y)
                {
                    float acc = 0.0f;
                    for (int i = -r; i <= r; ++i)
                        acc += data[(size_t(std::clamp(y + i, 0, h - 1)) * w + x) * stride] * k[size_t(i + r)];
                    line[size_t(y)] = acc;
                }
                for (int y = 0; y < h; ++y)
                    data[(size_t(y) * w + x) * stride] = line[size_t(y)];
            }
        }
    }

    Image::Image(int width, int height) : w(width), h(height), px(size_t(width) * height * 4u, 0.0f) {}

    Plane::Plane(int width, int height, float fill) : w(width), h(height), v(size_t(width) * height, fill) {}

    Image fromTexture(const TextureData &texture)
    {
        Image out(int(texture.width), int(texture.height));
        for (size_t i = 0; i < out.px.size() && i < texture.rgba.size(); ++i)
            out.px[i] = float(texture.rgba[i]);
        return out;
    }

    std::vector<uint8_t> toRgba(const Image &image)
    {
        std::vector<uint8_t> out(image.px.size());
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = uint8_t(std::clamp(std::lround(image.px[i]), 0l, 255l));
        return out;
    }

    Image crop(const Image &image, int x0, int y0, int x1, int y1)
    {
        Image out(std::max(x1 - x0, 0), std::max(y1 - y0, 0));
        for (int y = 0; y < out.h; ++y)
            for (int x = 0; x < out.w; ++x)
            {
                const int sx = x0 + x, sy = y0 + y;
                if (sx >= 0 && sy >= 0 && sx < image.w && sy < image.h)
                    std::copy_n(image.at(sx, sy), 4, out.at(x, y));
            }
        return out;
    }

    Image resize(const Image &image, int w, int h, Filter filter)
    {
        if (image.w == 0 || image.h == 0 || w <= 0 || h <= 0)
            return Image(std::max(w, 0), std::max(h, 0));
        return unpremultiplied(resample(premultiplied(image), image.w, image.h, 4, w, h, filter), w, h);
    }

    Plane resize(const Plane &plane, int w, int h, Filter filter)
    {
        Plane out;
        out.w = w;
        out.h = h;
        out.v = resample(plane.v, plane.w, plane.h, 1, w, h, filter);
        return out;
    }

    Image flipLeftRight(const Image &image)
    {
        Image out(image.w, image.h);
        for (int y = 0; y < image.h; ++y)
            for (int x = 0; x < image.w; ++x)
                std::copy_n(image.at(image.w - 1 - x, y), 4, out.at(x, y));
        return out;
    }

    Image flipTopBottom(const Image &image)
    {
        Image out(image.w, image.h);
        for (int y = 0; y < image.h; ++y)
            std::copy_n(image.at(0, image.h - 1 - y), size_t(image.w) * 4u, out.at(0, y));
        return out;
    }

    void paste(Image &dst, const Image &src, int x, int y)
    {
        for (int j = std::max(0, -y); j < src.h && y + j < dst.h; ++j)
            for (int i = std::max(0, -x); i < src.w && x + i < dst.w; ++i)
                std::copy_n(src.at(i, j), 4, dst.at(x + i, y + j));
    }

    void paste(Image &dst, const Image &src, int x, int y, const Plane &mask)
    {
        for (int j = std::max(0, -y); j < src.h && y + j < dst.h; ++j)
            for (int i = std::max(0, -x); i < src.w && x + i < dst.w; ++i)
            {
                const float m = mask.at(i, j);
                const float *s = src.at(i, j);
                float *d = dst.at(x + i, y + j);
                for (int c = 0; c < 4; ++c)
                    d[c] = s[c] * m + d[c] * (1.0f - m);
            }
    }

    void over(Image &dst, const Image &src, int x, int y)
    {
        for (int j = std::max(0, -y); j < src.h && y + j < dst.h; ++j)
            for (int i = std::max(0, -x); i < src.w && x + i < dst.w; ++i)
            {
                const float *s = src.at(i, j);
                float *d = dst.at(x + i, y + j);
                const float sa = s[3] / 255.0f, da = d[3] / 255.0f;
                const float oa = sa + da * (1.0f - sa);
                if (oa <= 0.0f)
                {
                    std::fill_n(d, 4, 0.0f);
                    continue;
                }
                for (int c = 0; c < 3; ++c)
                    d[c] = (s[c] * sa + d[c] * da * (1.0f - sa)) / oa;
                d[3] = oa * 255.0f;
            }
    }

    Box bbox(const Image &image)
    {
        Box b{image.w, image.h, 0, 0};
        for (int y = 0; y < image.h; ++y)
            for (int x = 0; x < image.w; ++x)
                if (image.at(x, y)[3] >= 0.5f)
                {
                    b.x0 = std::min(b.x0, x);
                    b.y0 = std::min(b.y0, y);
                    b.x1 = std::max(b.x1, x + 1);
                    b.y1 = std::max(b.y1, y + 1);
                }
        return b.x1 > b.x0 ? b : Box{};
    }

    Image rotate(const Image &image, float degrees)
    {
        const float t = degrees * std::numbers::pi_v<float> / 180.0f, c = std::cos(t), s = std::sin(t);
        // Where its corners go, about its middle: the canvas round them.
        const float hw = image.w / 2.0f, hh = image.h / 2.0f;
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
        for (const auto &[px, py] : std::array<std::array<float, 2>, 4>{{{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}}})
        {
            const float x = px * c + py * s, y = -px * s + py * c;
            lo[0] = std::min(lo[0], x), hi[0] = std::max(hi[0], x);
            lo[1] = std::min(lo[1], y), hi[1] = std::max(hi[1], y);
        }
        const int nw = int(std::ceil(hi[0]) - std::floor(lo[0])), nh = int(std::ceil(hi[1]) - std::floor(lo[1]));
        const std::vector<float> pre = premultiplied(image);
        std::vector<float> out(size_t(nw) * nh * 4u, 0.0f);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
            {
                const float dx = x + 0.5f - nw / 2.0f, dy = y + 0.5f - nh / 2.0f;
                const float sx = dx * c - dy * s + hw - 0.5f, sy = dx * s + dy * c + hh - 0.5f;
                const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                float acc[4] = {};
                for (int j = -1; j <= 2; ++j)
                    for (int i = -1; i <= 2; ++i)
                    {
                        const int qx = ix + i, qy = iy + j;
                        if (qx < 0 || qy < 0 || qx >= image.w || qy >= image.h)
                            continue;
                        const float k = bicubic(sx - qx) * bicubic(sy - qy);
                        const float *p = pre.data() + (size_t(qy) * image.w + qx) * 4u;
                        for (int ch = 0; ch < 4; ++ch)
                            acc[ch] += p[ch] * k;
                    }
                std::copy_n(acc, 4, out.data() + (size_t(y) * nw + x) * 4u);
            }
        return unpremultiplied(std::move(out), nw, nh);
    }

    void blur(Image &image, float sigma)
    {
        for (int c = 0; c < 4; ++c)
            blurChannel(image.px.data() + c, image.w, image.h, 4, sigma);
    }

    void blur(Plane &plane, float sigma)
    {
        blurChannel(plane.v.data(), plane.w, plane.h, 1, sigma);
    }

    Plane maxFilter(const Plane &plane, int size)
    {
        const int r = size / 2;
        Plane rows(plane.w, plane.h), out(plane.w, plane.h);
        for (int y = 0; y < plane.h; ++y)
            for (int x = 0; x < plane.w; ++x)
            {
                float m = -1e9f;
                for (int i = -r; i <= r; ++i)
                    m = std::max(m, plane.at(std::clamp(x + i, 0, plane.w - 1), y));
                rows.at(x, y) = m;
            }
        for (int y = 0; y < plane.h; ++y)
            for (int x = 0; x < plane.w; ++x)
            {
                float m = -1e9f;
                for (int i = -r; i <= r; ++i)
                    m = std::max(m, rows.at(x, std::clamp(y + i, 0, plane.h - 1)));
                out.at(x, y) = m;
            }
        return out;
    }

    Image medianFilter(const Image &image, int size)
    {
        // Per channel, a histogram of the square slid along each row.
        const int r = size / 2, n = size * size;
        Image out = image;
        for (int c = 0; c < 3; ++c)
            for (int y = 0; y < image.h; ++y)
            {
                std::array<int, 256> hist{};
                const auto level = [&](int x, int yy)
                {
                    const float v = image.at(std::clamp(x, 0, image.w - 1), std::clamp(yy, 0, image.h - 1))[c];
                    return std::clamp(int(std::lround(v)), 0, 255);
                };
                for (int j = -r; j <= r; ++j)
                    for (int i = -r; i <= r; ++i)
                        ++hist[size_t(level(i, y + j))];
                for (int x = 0; x < image.w; ++x)
                {
                    int seen = 0, v = 0;
                    while ((seen += hist[size_t(v)]) <= n / 2)
                        ++v;
                    out.at(x, y)[c] = float(v);
                    for (int j = -r; j <= r; ++j)
                    {
                        --hist[size_t(level(x - r, y + j))];
                        ++hist[size_t(level(x + r + 1, y + j))];
                    }
                }
            }
        return out;
    }

    Plane luma(const Image &image)
    {
        Plane out(image.w, image.h);
        for (size_t i = 0; i < out.v.size(); ++i)
        {
            const float *p = image.px.data() + i * 4u;
            out.v[i] = (p[0] * 299.0f + p[1] * 587.0f + p[2] * 114.0f) / 1000.0f;
        }
        return out;
    }

    Plane mean(const Image &image)
    {
        Plane out(image.w, image.h);
        for (size_t i = 0; i < out.v.size(); ++i)
        {
            const float *p = image.px.data() + i * 4u;
            out.v[i] = (p[0] + p[1] + p[2]) / 3.0f;
        }
        return out;
    }

    void sample(const Image &image, float x, float y, float out[4])
    {
        x = std::clamp(x, 0.0f, float(image.w - 1));
        y = std::clamp(y, 0.0f, float(image.h - 1));
        const int x0 = std::min(int(x), image.w - 2), y0 = std::min(int(y), image.h - 2);
        const float fx = x - x0, fy = y - y0;
        for (int c = 0; c < 4; ++c)
        {
            const float top = image.at(x0, y0)[c] * (1 - fx) + image.at(x0 + 1, y0)[c] * fx;
            const float bottom = image.at(x0, y0 + 1)[c] * (1 - fx) + image.at(x0 + 1, y0 + 1)[c] * fx;
            out[c] = top * (1 - fy) + bottom * fy;
        }
    }
}
