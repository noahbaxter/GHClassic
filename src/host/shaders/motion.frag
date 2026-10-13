#version 450

// Where this point of a moving mesh was in the last frame's picture, for
// post.frag's joining of frames: the step in uv from here to there, the
// depth it is drawn at, by which the joining tells that the scene shows this
// mesh here, and 1, where a pixel no mesh moved over is left 0.

layout(location = 0) in vec4 vNow;
layout(location = 1) in vec4 vWas;

layout(location = 0) out vec4 outMotion;

void main()
{
    // Behind the camera then: nowhere in that picture.
    if (vWas.w <= 0.0)
        discard;
    outMotion = vec4((vWas.xy / vWas.w - vNow.xy / vNow.w) * 0.5, gl_FragCoord.z, 1.0);
}
