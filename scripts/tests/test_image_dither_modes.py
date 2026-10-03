"""Exercise the stateless and stateful image dither modes on the host."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def main():
    source = r'''
#include <cassert>
#include "lib/Epub/Epub/converters/DitherUtils.h"
int main() {
  assert(applyDither4Level(0, 0, 0, ImageDitherMode::None) == 0);
  assert(applyDither4Level(255, 1, 1, ImageDitherMode::Bayer4x4) == 3);
  assert(applyDither4Level(255, 2, 2, ImageDitherMode::Bayer8x8) == 3);
  assert(applyDither4Level(127, 3, 5, ImageDitherMode::Random) <= 3);
  ErrorDiffusionDither4Level fs(32);
  assert(fs.valid());
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 32; ++x)
      assert(fs.process(x, y, static_cast<unsigned char>((x * 7 + y * 13) & 255)) <= 3);
}
'''
    with tempfile.TemporaryDirectory(prefix="image-dither-") as temp:
        cpp = Path(temp) / "test.cpp"
        binary = Path(temp) / "test"
        cpp.write_text(source)
        subprocess.run(
            ["c++", "-std=c++20", "-I" + str(ROOT), "-I" + str(ROOT / "test/time_utils/stubs"), str(cpp), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
    print("image dither modes passed")


if __name__ == "__main__":
    main()
