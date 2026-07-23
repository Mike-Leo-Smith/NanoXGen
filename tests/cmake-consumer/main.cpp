#include <nanoxgen/context.h>
#include <nanoxgen/xgen.h>

int main() {
    nanoxgen::NanoXGenContext context{1u};
    return nanoxgen::kXGenFileMagic == 0x8099ceadull &&
                   context.worker_count() == 1u &&
                   context.owns_executor()
        ? 0
        : 1;
}
