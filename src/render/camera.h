#pragma once

#include "render/frame.h"

namespace gh2
{
    // View times projection for a camera, as a Milo row-vector matrix that
    // takes world space to Vulkan clip space within the camera's viewport.
    // yRatio is Rnd::YRatio: 0.75 for 4:3, 0.5625 for 16:9.
    Matrix viewProjection(const Camera &camera, float yRatio);

    // a * b for row-vector matrices: apply a, then b.
    Matrix multiply(const Matrix &a, const Matrix &b);

    Matrix identity();
}
