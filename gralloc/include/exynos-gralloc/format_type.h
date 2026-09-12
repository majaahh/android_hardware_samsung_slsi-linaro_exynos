#pragma once

#include <cstdint>

namespace exynos::graphics {

// FormatType alludes to the type of components that are in the buffer of a particular format. This
// is same as querying for PlaneLayouts and then checking the stored components
// (PlaneLayoutComponentType) in the buffer.
enum class FormatType : uint8_t {
    // Format component type is RAW or unknown
    UNKNOWN,

    // Format components are strictly a subset of R, G, B, and A
    RGB,

    // Format components are strictly a subset of Y, CB, and CR
    YUV,
};

} // namespace exynos::graphics
