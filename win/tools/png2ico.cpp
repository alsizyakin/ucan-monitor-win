// png2ico: wraps a PNG image, as it is, into an .ico file (Windows Vista and
// later read PNG icons), so the program icon comes from upstream's PNG.
//
//   png2ico in.png out.ico

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: png2ico in.png out.ico\n";
        return 2;
    }

    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<unsigned char> png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    static const unsigned char signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (png.size() < 24 || !std::equal(std::begin(signature), std::end(signature), png.begin())) {
        std::cerr << argv[1] << ": not a PNG image\n";
        return 1;
    }

    // width and height of the IHDR chunk, big-endian
    const auto be32 = [&](size_t at) {
        return uint32_t{png[at]} << 24 | uint32_t{png[at + 1]} << 16 | uint32_t{png[at + 2]} << 8 | png[at + 3];
    };
    const uint32_t width = be32(16);
    const uint32_t height = be32(20);
    if (width == 0 || width > 256 || height == 0 || height > 256) {
        std::cerr << argv[1] << ": an icon is at most 256x256, this is " << width << 'x' << height << '\n';
        return 1;
    }

    std::vector<unsigned char> ico;
    const auto le16 = [&](uint32_t value) {
        ico.push_back(value & 0xff);
        ico.push_back(value >> 8 & 0xff);
    };
    const auto le32 = [&](uint32_t value) {
        le16(value & 0xffff);
        le16(value >> 16);
    };

    // ICONDIR: reserved, type 1 (icon), one image
    le16(0);
    le16(1);
    le16(1);
    // ICONDIRENTRY: 256 is written as 0; no palette; 1 plane, 32 bits per pixel
    ico.push_back(width == 256 ? 0 : width);
    ico.push_back(height == 256 ? 0 : height);
    ico.push_back(0);
    ico.push_back(0);
    le16(1);
    le16(32);
    le32(static_cast<uint32_t>(png.size()));
    le32(6 + 16); // the image follows the directory
    ico.insert(ico.end(), png.begin(), png.end());

    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(ico.data()), static_cast<std::streamsize>(ico.size()));
    if (!out) {
        std::cerr << argv[2] << ": cannot write\n";
        return 1;
    }
    return 0;
}
