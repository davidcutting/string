#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Minimal, dependency-free PNG writer (brief 06 capture polish). Encodes 8-bit RGB(A) into a valid
// PNG using DEFLATE *stored* (uncompressed) blocks — no zlib, no stb needed, fully deterministic
// (run-twice byte-identical). Files are larger than a compressed PNG but load in every viewer and
// are trivially diffable; captures are a tooling artefact, not shipped assets, so size is fine.
//
// This keeps the engine free of a new third-party dependency while giving the capture path the
// lossless .png output the brief asks for (BMP crushes to 8-bit BGR with tonemap already; PNG here
// takes the same tonemapped 8-bit pixels but preserves them without BGR/row-flip surprises).
namespace string::core::png
{

namespace detail
{
inline uint32_t crc32(const uint8_t* data, std::size_t len, uint32_t crc = 0xFFFFFFFFu)
{
    static uint32_t table[256];
    static bool built = [] {
        for (uint32_t n = 0; n < 256; ++n)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        return true;
    }();
    (void)built;
    for (std::size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

inline uint32_t adler32(const uint8_t* data, std::size_t len)
{
    uint32_t a = 1, b = 0;
    for (std::size_t i = 0; i < len; ++i)
    {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

inline void put_be32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

inline void write_chunk(std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& data)
{
    put_be32(out, uint32_t(data.size()));
    const std::size_t crc_start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t crc = crc32(out.data() + crc_start, out.size() - crc_start) ^ 0xFFFFFFFFu;
    put_be32(out, crc);
}
}  // namespace detail

// Encode `pixels` (row-major, top-down, `channels` bytes/pixel, 3=RGB or 4=RGBA) to a PNG byte
// stream. width*height*channels bytes expected.
inline std::vector<uint8_t> encode(const uint8_t* pixels, uint32_t width, uint32_t height,
                                   uint32_t channels)
{
    using namespace detail;
    std::vector<uint8_t> out;
    const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    out.insert(out.end(), sig, sig + 8);

    // IHDR
    std::vector<uint8_t> ihdr;
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.push_back(8);                               // bit depth
    ihdr.push_back(channels == 4 ? 6 : 2);           // colour type: 2=RGB, 6=RGBA
    ihdr.push_back(0);                               // compression
    ihdr.push_back(0);                               // filter
    ihdr.push_back(0);                               // interlace
    write_chunk(out, "IHDR", ihdr);

    // Raw scanlines with a leading filter byte (0 = none) per row.
    std::vector<uint8_t> raw;
    raw.reserve(std::size_t(height) * (1 + std::size_t(width) * channels));
    for (uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0);
        const uint8_t* row = pixels + std::size_t(y) * width * channels;
        raw.insert(raw.end(), row, row + std::size_t(width) * channels);
    }

    // zlib stream: 2-byte header, DEFLATE stored blocks (<=65535 each), adler32 trailer.
    std::vector<uint8_t> zlib;
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    std::size_t off = 0;
    while (off < raw.size())
    {
        const std::size_t block = std::min<std::size_t>(65535, raw.size() - off);
        const bool last = (off + block) >= raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(uint8_t(block & 0xFF));
        zlib.push_back(uint8_t((block >> 8) & 0xFF));
        const uint16_t nlen = uint16_t(~block);
        zlib.push_back(uint8_t(nlen & 0xFF));
        zlib.push_back(uint8_t((nlen >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.begin() + off, raw.begin() + off + block);
        off += block;
    }
    put_be32(zlib, adler32(raw.data(), raw.size()));
    write_chunk(out, "IDAT", zlib);

    write_chunk(out, "IEND", {});
    return out;
}

}  // namespace string::core::png
