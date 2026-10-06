#include "dev/draw_dump.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

namespace gh2
{
    namespace
    {
        std::mutex s_mutex;
        std::filesystem::path s_dir;
        std::string s_request;
        uint64_t s_fromSerial = 0;
        bool s_writing = false;

        struct V3
        {
            float x, y, z;
        };

        float dot(V3 a, V3 b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        // mat3(c0, c1, c2) * x, the columns three vec4s of the frame data.
        V3 through(const Vec4 *c, V3 x)
        {
            return {c[0].v[0] * x.x + c[1].v[0] * x.y + c[2].v[0] * x.z,
                    c[0].v[1] * x.x + c[1].v[1] * x.y + c[2].v[1] * x.z,
                    c[0].v[2] * x.x + c[1].v[2] * x.y + c[2].v[2] * x.z};
        }

        struct Out
        {
            float pos[4];
            float color[4];
            float uv[2];
        };

        // mesh.vert's main, line for line.
        Out shade(const Vertex &in, const PushConstants &pc, const Vec4 *data)
        {
            float pos[4] = {in.pos[0], in.pos[1], in.pos[2], 1.0f};
            float vertexColor[4] = {in.color[0], in.color[1], in.color[2], in.color[3]};
            V3 litPos{in.pos[0], in.pos[1], in.pos[2]};
            V3 litNormal{in.normal[0], in.normal[1], in.normal[2]};
            const V3 inNormal = litNormal;
            if (pc.boneBase >= 0)
            {
                const Vec4 *bones = data + pc.boneBase;
                float skinned[4] = {};
                for (int b = 0; b < 4; ++b)
                    for (int c = 0; c < 4; ++c)
                        skinned[c] += in.color[b] * (bones[b * 4 + 0].v[c] * pos[0] + bones[b * 4 + 1].v[c] * pos[1] +
                                                     bones[b * 4 + 2].v[c] * pos[2] + bones[b * 4 + 3].v[c] * pos[3]);
                std::copy(skinned, skinned + 4, pos);
                std::fill(vertexColor, vertexColor + 4, 1.0f);
                const uint32_t count = ((pc.flags >> kSkinBonesShift) & 3u) + 1u;
                if (count > 1u)
                {
                    litPos = {pos[0], pos[1], pos[2]};
                    const float *w = in.color;
                    if (count == 4u)
                    {
                        V3 sum{};
                        for (int b = 0; b < 4; ++b)
                        {
                            const V3 n = through(bones + b * 4, inNormal);
                            sum = {sum.x + w[b] * n.x, sum.y + w[b] * n.y, sum.z + w[b] * n.z};
                        }
                        const float length = std::sqrt(dot(sum, sum));
                        litNormal = {sum.x / length, sum.y / length, sum.z / length};
                    }
                    else if (count == 3u)
                    {
                        int b = w[1] >= w[0] ? 1 : 0;
                        if (w[2] > w[b])
                            b = 2;
                        litNormal = through(bones + b * 4, inNormal);
                    }
                    else if (w[1] >= w[0])
                        litNormal = through(bones + 4, inNormal);
                    else
                    {
                        const Vec4 mixed[3] = {bones[0], bones[1], bones[6]};
                        litNormal = through(mixed, inNormal);
                    }
                }
            }
            Out out{};
            for (int c = 0; c < 4; ++c)
                out.pos[c] = pc.mvp[0 + c] * pos[0] + pc.mvp[4 + c] * pos[1] + pc.mvp[8 + c] * pos[2] +
                             pc.mvp[12 + c] * pos[3];

            const uint32_t mode = pc.flags & 7u;
            const float *base = (pc.flags & kFlagPrelit) ? vertexColor : pc.matColor;
            float color[4] = {vertexColor[0], vertexColor[1], vertexColor[2], vertexColor[3]};
            const Vec4 *light = data + pc.lightBase;
            if (mode == kColorAmbient)
            {
                for (int c = 0; c < 4; ++c)
                    color[c] = std::min(base[c] * light[0].v[c], 1.0f);
            }
            else if (mode == kColorMaterial)
            {
                for (int c = 0; c < 4; ++c)
                    color[c] = std::min(pc.matColor[c], 1.0f);
            }
            else if (mode == kColorDirectional)
            {
                float d[3];
                for (int i = 0; i < 3; ++i)
                    d[i] = std::max(dot(litNormal, {light[4 + i].v[0], light[4 + i].v[1], light[4 + i].v[2]}), 0.0f);
                for (int c = 0; c < 4; ++c)
                {
                    const float lit = d[0] * (light[1].v[c] * pc.matColor[c]) + d[1] * (light[2].v[c] * pc.matColor[c]) +
                                      d[2] * (light[3].v[c] * pc.matColor[c]);
                    color[c] = std::min(lit + base[c] * light[0].v[c], 1.0f);
                }
            }
            else if (mode == kColorPoint)
            {
                const Vec4 &local = light[4];
                const V3 to{local.v[0] - litPos.x, local.v[1] - litPos.y, local.v[2] - litPos.z};
                const float d2 = dot(to, to);
                const float facing = dot(to, litNormal);
                for (int c = 0; c < 4; ++c)
                    color[c] = base[c] * light[0].v[c];
                if (d2 <= light[5].v[3] && d2 > 0.0f && facing >= 0.0f)
                    for (int c = 0; c < 4; ++c)
                        color[c] += light[1].v[c] * pc.matColor[c] * (facing * (local.v[3] + 1.0f / std::sqrt(d2)));
                for (int c = 0; c < 4; ++c)
                    color[c] = std::clamp(color[c], 0.0f, 1.0f);
            }
            if (pc.flags & kFlagIntensify)
                for (int c = 0; c < 3; ++c)
                    color[c] *= 255.0f / 128.0f;
            if (pc.flags & kFlagHighlight)
                color[3] = 0.035f;
            std::copy(color, color + 4, out.color);

            if (pc.envBase >= 0 && (pc.flags & kFlagSphere))
            {
                const Vec4 *e = data + pc.envBase;
                const V3 n = through(e, litNormal);
                out.uv[0] = n.x + e[3].v[0];
                out.uv[1] = n.y + e[3].v[1];
            }
            else if (pc.envBase >= 0 && (pc.flags & kFlagProjected))
            {
                const Vec4 *e = data + pc.envBase;
                const V3 p = through(e, litPos);
                out.uv[0] = p.x + e[3].v[0];
                out.uv[1] = p.y + e[3].v[1];
            }
            else if (pc.envBase >= 0)
            {
                const Vec4 *e = data + pc.envBase;
                const V3 p = through(e, litPos);
                const V3 v{e[3].v[0] - p.x, e[3].v[1] - p.y, e[3].v[2] - p.z};
                const V3 n = through(e, litNormal);
                const float nv = dot(n, v);
                const float scale = 0.5f / std::sqrt(dot(v, v));
                out.uv[0] = (2.0f * n.x * nv - v.x) * scale + 0.5f;
                out.uv[1] = (2.0f * n.y * nv - v.y) * scale + 0.5f;
            }
            else
            {
                out.uv[0] = in.uv[0] * pc.uvRows[0] + in.uv[1] * pc.uvRows[2] + pc.uvOffset[0];
                out.uv[1] = in.uv[0] * pc.uvRows[1] + in.uv[1] * pc.uvRows[3] + pc.uvOffset[1];
            }
            return out;
        }

        // A texture's pixels, to tell two of a size apart.
        uint32_t hash(const TextureData &texture)
        {
            uint32_t h = 2166136261u;
            for (uint8_t byte : texture.rgba)
                h = (h ^ byte) * 16777619u;
            return h;
        }
    }

    void setDrawDumpDir(const std::filesystem::path &dir)
    {
        std::lock_guard lock(s_mutex);
        s_dir = dir;
    }

    void requestDrawDump(const std::string &name, uint64_t fromSerial)
    {
        std::lock_guard lock(s_mutex);
        s_fromSerial = fromSerial;
        s_request = name;
    }

    bool drawDumpPending()
    {
        std::lock_guard lock(s_mutex);
        return !s_request.empty() || s_writing;
    }

    void writeDrawDump(const Frame &frame, const PushConstants *pushes, const Vec4 *data)
    {
        std::filesystem::path path;
        {
            std::lock_guard lock(s_mutex);
            if (s_request.empty() || frame.serial < s_fromSerial)
                return;
            path = s_dir / ("draws_" + s_request + ".json");
            s_request.clear();
            s_writing = true;
        }
        if (FILE *file = std::fopen(path.string().c_str(), "wb"))
        {
            std::fprintf(file, "{\"width\": %u, \"height\": %u, \"clear\": [%.9g, %.9g, %.9g, %.9g], \"draws\": [\n",
                         frame.width, frame.height, frame.clear[0], frame.clear[1], frame.clear[2], frame.clear[3]);
            bool first = true;
            // Each texture's pixels once, in draws_<name>.rgba, by its hash.
            std::map<uint32_t, const TextureData *> textures;
            for (size_t i = 0; i < frame.draws.size(); ++i)
            {
                const DrawCall &draw = frame.draws[i];
                if (!draw.mesh || draw.camera >= frame.cameras.size())
                    continue;
                const Camera &camera = frame.cameras[draw.camera];
                const Material &m = draw.material;
                const PushConstants &pc = pushes[i];
                std::fprintf(file,
                             "%s{\"index\": %zu, \"screen\": %s, \"target\": %u, \"targetSize\": [%u, %u], "
                             "\"rect\": [%.9g, %.9g, %.9g, %.9g], "
                             "\"blend\": %u, \"zMode\": %u, \"mode\": %u, \"alphaCut\": %s, \"alphaWrite\": %s, "
                             "\"destAlphaTest\": %s, \"intensify\": %s, \"texWrap\": %s, \"skinned\": %s, "
                             "\"renderTarget\": %u, \"highlight\": %s, ",
                             first ? "" : ",\n", i, draw.screen ? "true" : "false", camera.target, camera.targetWidth,
                             camera.targetHeight, camera.rect[0],
                             camera.rect[1], camera.rect[2], camera.rect[3], m.blend, m.zMode, pc.flags & 7u,
                             (pc.flags & kFlagAlphaCut) ? "true" : "false", m.alphaWrite ? "true" : "false",
                             m.destAlphaTest ? "true" : "false", (pc.flags & kFlagIntensify) ? "true" : "false",
                             m.texWrap ? "true" : "false", draw.skinned ? "true" : "false", m.renderTarget,
                             (pc.flags & kFlagHighlight) ? "true" : "false");
                first = false;
                if (m.renderTarget != 0u)
                {
                    // A rendered texture's size is its camera's, or its screen copy's.
                    uint32_t width = 0, height = 0;
                    for (const Camera &other : frame.cameras)
                        if (other.target == m.renderTarget)
                        {
                            width = other.targetWidth;
                            height = other.targetHeight;
                        }
                    for (const Frame::ScreenCopy &copy : frame.copies)
                        if (copy.tex == m.renderTarget)
                        {
                            width = copy.width;
                            height = copy.height;
                        }
                    std::fprintf(file, "\"texture\": {\"width\": %u, \"height\": %u, \"hash\": 0}, ", width, height);
                }
                else if (m.texture)
                {
                    const uint32_t id = hash(*m.texture);
                    textures[id] = m.texture.get();
                    std::fprintf(file, "\"texture\": {\"width\": %u, \"height\": %u, \"hash\": %u}, ", m.texture->width,
                                 m.texture->height, id);
                }
                else
                    std::fprintf(file, "\"texture\": null, ");
                // What the lighting program is given: the material colour
                // (qw690) and the draw's block of writeLighting.
                std::fprintf(file, "\"flags\": %u, \"matColor\": [%.9g, %.9g, %.9g, %.9g], \"light\": [", pc.flags,
                             pc.matColor[0], pc.matColor[1], pc.matColor[2], pc.matColor[3]);
                for (int l = 0; pc.lightBase >= 0 && l < 7; ++l)
                    std::fprintf(file, "%s[%.9g, %.9g, %.9g, %.9g]", l ? ", " : "", data[pc.lightBase + l].v[0],
                                 data[pc.lightBase + l].v[1], data[pc.lightBase + l].v[2], data[pc.lightBase + l].v[3]);
                // The matrices VU1 is given: the mesh's world (qw676..679 when rigid) and a
                // skinned mesh's bones (qw660..675).
                std::fprintf(file, "], \"world\": [");
                for (size_t m = 0; m < 16u; ++m)
                    std::fprintf(file, "%s%.9g", m ? ", " : "", draw.world[m]);
                std::fprintf(file, "], \"bones\": [");
                for (size_t m = 0; draw.skinned && m < 64u; ++m)
                    std::fprintf(file, "%s%.9g", m ? ", " : "", draw.bones[m / 16u][m % 16u]);
                std::fprintf(file, "], \"triangles\": %zu, \"verts\": [", draw.mesh->indices.size() / 3u);
                // Clip x, y, z, w, then colour (1 is the GS's 0x80) and uv.
                for (size_t v = 0; v < draw.mesh->verts.size(); ++v)
                {
                    const Out out = shade(draw.mesh->verts[v], pc, data);
                    std::fprintf(file, "%s[%.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g]", v ? ", " : "",
                                 out.pos[0], out.pos[1], out.pos[2], out.pos[3], out.color[0], out.color[1],
                                 out.color[2], out.color[3], out.uv[0], out.uv[1]);
                }
                std::fprintf(file, "], \"indices\": [");
                for (size_t n = 0; n < draw.mesh->indices.size(); ++n)
                    std::fprintf(file, "%s%u", n ? ", " : "", draw.mesh->indices[n]);
                std::fprintf(file, "]}");
            }
            std::fprintf(file, "\n], \"textures\": {");
            FILE *pixels = std::fopen(std::filesystem::path(path).replace_extension(".rgba").string().c_str(), "wb");
            long at = 0;
            first = true;
            for (const auto &[id, texture] : textures)
            {
                if (!pixels)
                    break;
                std::fwrite(texture->rgba.data(), 1, texture->rgba.size(), pixels);
                std::fprintf(file, "%s\"%u\": [%ld, %u, %u]", first ? "" : ", ", id, at, texture->width, texture->height);
                at += static_cast<long>(texture->rgba.size());
                first = false;
            }
            if (pixels)
                std::fclose(pixels);
            std::fprintf(file, "}}\n");
            std::fclose(file);
        }
        std::lock_guard lock(s_mutex);
        s_writing = false;
    }
}
