// Milo's camera maths, from GH2 retail, producing Vulkan clip space.
//
// Camera space is x right, y forward, z up. RndCam::UpdateLocal (0x1b1f50)
// builds the projection from yfov and the aspect; PsCam::Select (0x19c460)
// adds depth for the GS, where larger Z is nearer and z_range picks a slice.
// Vulkan depth here equals the GS's Z / 65535, so the engine's GEQUAL test
// against a 0 clear carries over as GREATER_OR_EQUAL against 0.

#include "render/camera.h"

#include <cmath>

namespace gh2
{
    Matrix multiply(const Matrix &a, const Matrix &b)
    {
        Matrix out{};
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += a[row * 4 + k] * b[k * 4 + col];
                out[row * 4 + col] = sum;
            }
        return out;
    }

    Matrix identity()
    {
        Matrix m{};
        m[0] = m[5] = m[10] = m[15] = 1.0f;
        return m;
    }

    Matrix viewProjection(const Camera &camera, float yRatio)
    {
        const float n = camera.nearPlane;
        const float f = camera.farPlane;
        const float zr0 = camera.zRange[0];
        const float zr1 = camera.zRange[1];
        // aspect = (rect h / rect w) * YRatio (UpdateLocal 0x1b1f70), or the
        // target texture's height over width in place of YRatio.
        if (camera.target != 0u && camera.targetWidth != 0u)
            yRatio = static_cast<float>(camera.targetHeight) / static_cast<float>(camera.targetWidth);
        const float aspect = camera.rect[2] != 0.0f ? camera.rect[3] / camera.rect[2] * yRatio : yRatio;
        const float k = (zr1 - zr0) * 0.5f;
        const float cz = 1.0f - zr0 - k;

        // Rows are the coefficients of camera-space x, y, z and 1.
        Matrix projection{};
        if (camera.yFov != 0.0f)
        {
            const float t = std::tan(camera.yFov * 0.5f);
            const float a = (f + n) / (f - n);
            const float b = -2.0f * f * n / (f - n);
            projection[0 * 4 + 0] = aspect / t; // x
            projection[2 * 4 + 1] = -1.0f / t;  // y: screen down is -z
            projection[1 * 4 + 2] = cz - k * a; // z
            projection[3 * 4 + 2] = -k * b;
            projection[1 * 4 + 3] = 1.0f;       // w = forward distance
        }
        else
        {
            projection[0 * 4 + 0] = 1.0f;
            projection[2 * 4 + 1] = -1.0f / aspect;
            projection[1 * 4 + 2] = -2.0f * k / (f - n);
            projection[3 * 4 + 2] = cz + k * (f + n) / (f - n);
            projection[3 * 4 + 3] = 1.0f;
        }
        return multiply(camera.view, projection);
    }
}
