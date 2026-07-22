#include <nanoxgen/xgen.h>

int main() {
    return nanoxgen::kXGenFileMagic == 0x8099ceadull ? 0 : 1;
}
