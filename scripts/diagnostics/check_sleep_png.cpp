// Read-only host probe using the same PNGdec dependency and buffer limit as firmware.
// This validates file decoding, not ESP heap availability or display behavior.
#include <PNGdec.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
static unsigned rows = 0;
static int draw(PNGDRAW*) {
  ++rows;
  return 1;
}
int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: check_sleep_png <downloaded-image.png>\n";
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  if (!input) return 2;
  std::vector<uint8_t> data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  PNG png;
  const int opened = png.openRAM(data.data(), static_cast<int>(data.size()), draw);
  std::cout << "open=" << opened << " width=" << png.getWidth() << " height=" << png.getHeight()
            << " type=" << png.getPixelType() << " depth=" << png.getBpp() << " workspace=" << sizeof(PNG) << '\n';
  if (opened != PNG_SUCCESS) return 1;
  const int result = png.decode(nullptr, 0);
  std::cout << "decode=" << result << " rows=" << rows << '\n';
  const bool success = result == PNG_SUCCESS && rows == static_cast<unsigned>(png.getHeight());
  png.close();
  return success ? 0 : 1;
}
