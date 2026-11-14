// A .mogg is a version word (10 is unencrypted), the Ogg stream's offset,
// then the stream, served through content/vgs.h.

#include "content/mogg.h"

#include "content/vgs.h"
#include "content/vorbis.h"
#include "disc/ark.h"

#include <cstring>
#include <iostream>

namespace gh2::mogg
{
    void serveAsVgs(const std::string &vgs, size_t disc, const std::string &mogg)
    {
        vgs::serve(vgs, [disc, mogg]() -> std::unique_ptr<vgs::Pcm>
                   {
                       auto bytes = ark::readFile(disc, mogg);
                       uint32_t version = 0u, start = 0u;
                       if (bytes && bytes->size() >= 8u)
                       {
                           std::memcpy(&version, bytes->data(), 4u);
                           std::memcpy(&start, bytes->data() + 4u, 4u);
                       }
                       if (!bytes || version != 10u || start >= bytes->size())
                       {
                           std::cerr << "[mogg] " << mogg << ": not an unencrypted mogg" << std::endl;
                           return nullptr;
                       }
                       return vorbis::open(heldBytes(std::make_shared<const std::vector<uint8_t>>(std::move(*bytes)), start), mogg);
                   });
    }
}
