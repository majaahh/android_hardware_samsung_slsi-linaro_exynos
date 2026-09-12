#pragma once

#include <aidl/android/hardware/graphics/common/StandardMetadataType.h>
#include <gralloctypes/Gralloc4.h>

#include <cstdint>
#include <limits>

#include "format.h"
#include "format_type.h"

namespace exynos::graphics {

constexpr const char* kGralloc4StandardMetadataTypeName = GRALLOC4_STANDARD_METADATA_TYPE;
constexpr const char* kPixelMetadataTypeName = "android.hardware.graphics.common.PixelMetadataType";

using StandardMetadataType = aidl::android::hardware::graphics::common::StandardMetadataType;

#define MapMetadataType(f) f = static_cast<uint64_t>(StandardMetadataType::f)

// This seemingly clashes with MetadataType in Mapper, but this enum represents just the "value"
// member of that struct. MetadataType comprises of a metadata name and value. Name is just there to
// identify what kind of metadata it is. So, for all StandardMetadataType, clients need to use
// kGralloc4StandardMetadataType and for pixel specific metadata, clients should use
// kPixelMetadataType.
enum class MetadataType : int64_t {
    MapMetadataType(INVALID),
    MapMetadataType(BUFFER_ID),
    MapMetadataType(NAME),
    MapMetadataType(WIDTH),
    MapMetadataType(HEIGHT),
    MapMetadataType(LAYER_COUNT),
    MapMetadataType(PIXEL_FORMAT_REQUESTED),
    MapMetadataType(PIXEL_FORMAT_FOURCC),
    MapMetadataType(PIXEL_FORMAT_MODIFIER),
    MapMetadataType(USAGE),
    MapMetadataType(ALLOCATION_SIZE),
    MapMetadataType(PROTECTED_CONTENT),
    MapMetadataType(COMPRESSION),
    MapMetadataType(INTERLACED),
    MapMetadataType(CHROMA_SITING),
    MapMetadataType(PLANE_LAYOUTS),
    MapMetadataType(CROP),
    MapMetadataType(DATASPACE),
    MapMetadataType(BLEND_MODE),
    MapMetadataType(SMPTE2086),
    MapMetadataType(CTA861_3),
    MapMetadataType(SMPTE2094_40),
    MapMetadataType(SMPTE2094_10),
    MapMetadataType(STRIDE),

    // Pixel specific metadata
    // Make sure to use kPixelMetadataType as the name when using these metadata.

    // TODO: These metadata queries returns a pointer inside metadata for now. Need to change that
    // so we are returning proper data only.
    // Returns: void*
    VIDEO_HDR = std::numeric_limits<int64_t>::max() - (1 << 16),

    // TODO(b/289448426#comment2): ROIINFO is probably not being used. Remove this after
    // confirmation.
    // Returns: void*
    VIDEO_ROI,

    // This metadata just refers to the same fd contained in buffer handle and not a clone.
    // So the client should not attempt to close these fds.
    // Returns: std::vector<int>
    PLANE_DMA_BUFS,

    // PLANE_LAYOUTS from gralloc reply with the actual offset of the plane from the start of the
    // header if any. But some IPs require the offset starting from the body of a plane.
    // Returns: std::vector<CompressedPlaneLayout>
    COMPRESSED_PLANE_LAYOUTS,

    // Ideally drivers should be using fourcc to identify an allocation, but some of the drivers
    // depend upon the format too much that updating them will require longer time.
    // Returns: ::exynos::graphics::Format
    PIXEL_FORMAT_ALLOCATED,

    // Returns: ::exynos::graphics::FormatType
    FORMAT_TYPE,

    // This is a experimental feature
    VIDEO_GMV,
};

struct VideoGMV {
    int x;
    int y;
};

#undef MapMetadataType

// There is no backward compatibility guarantees, all dependencies must be built together.
struct CompressedPlaneLayout {
    uint64_t header_offset_in_bytes;
    uint64_t header_size_in_bytes;
    uint64_t body_offset_in_bytes;
    uint64_t body_size_in_bytes;

    bool operator==(const CompressedPlaneLayout& other) const {
        return header_offset_in_bytes == other.header_offset_in_bytes &&
                header_size_in_bytes == other.header_size_in_bytes &&
                body_offset_in_bytes == other.body_offset_in_bytes &&
                body_size_in_bytes == other.body_size_in_bytes;
    }

    bool operator!=(const CompressedPlaneLayout& other) const { return !(*this == other); }
};

template <MetadataType T>
struct always_false : std::false_type {};

namespace metadata {

template <MetadataType T>
struct ReturnType {
    static_assert(always_false<T>::value, "Unspecialized ReturnType is not supported");
    using type = void;
};

#define DEFINE_TYPE(meta_name, return_type)      \
    template <>                                  \
    struct ReturnType<MetadataType::meta_name> { \
        using type = return_type;                \
    };

DEFINE_TYPE(PLANE_DMA_BUFS, std::vector<int>);
DEFINE_TYPE(VIDEO_HDR, void*);
DEFINE_TYPE(VIDEO_ROI, void*);
DEFINE_TYPE(VIDEO_GMV, VideoGMV);

DEFINE_TYPE(COMPRESSED_PLANE_LAYOUTS, std::vector<CompressedPlaneLayout>);
DEFINE_TYPE(PIXEL_FORMAT_ALLOCATED, Format);
DEFINE_TYPE(FORMAT_TYPE, FormatType);

#undef DEFINE_TYPE

} // namespace metadata

} // namespace exynos::graphics
