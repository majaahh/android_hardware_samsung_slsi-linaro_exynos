/*
 * Copyright (C) 2020 Arm Limited. All rights reserved.
 *
 * Copyright 2016 The Android Open Source Project
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GrallocMapper.h"

#include <cutils/native_handle.h>

#include "allocator/mali_gralloc_ion.h"
#include "core/format_info.h"
#include "drmutils.h"
#include "hidl_common/BufferDescriptor.h"
#include "hidl_common/MapperMetadata.h"
#include "hidl_common/SharedMetadata.h"
#include "hidl_common/hidl_common.h"

namespace arm {
namespace mapper {

using namespace android::hardware::graphics::mapper;

AIMapper_Error GrallocMapper::importBuffer(const native_handle_t* _Nonnull handle,
                                           buffer_handle_t _Nullable* _Nonnull outBufferHandle) {
    if (handle == nullptr) return AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;
    AIMapper_Error err = static_cast<AIMapper_Error>(common::importBuffer(handle, outBufferHandle));
    return err;
}

AIMapper_Error GrallocMapper::freeBuffer(buffer_handle_t _Nonnull buffer) {
    if (buffer == nullptr) return AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;
    native_handle_t* bufferHandle = const_cast<native_handle_t*>(buffer);
    return static_cast<AIMapper_Error>(common::freeBuffer(bufferHandle));
}

AIMapper_Error GrallocMapper::lock(buffer_handle_t _Nonnull buffer, uint64_t cpuUsage,
                                   ARect accessRegion, int acquireFence,
                                   void* _Nullable* _Nonnull outData) {
    if (buffer == nullptr) return AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;
    AIMapper_Error err = static_cast<AIMapper_Error>(common::lock(buffer, cpuUsage,
                                                                  common::GrallocRect(accessRegion),
                                                                  acquireFence, outData));
    return err;
}

AIMapper_Error GrallocMapper::unlock(buffer_handle_t _Nonnull buffer, int* _Nonnull releaseFence) {
    if (buffer == nullptr) return AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;

    AIMapper_Error err = static_cast<AIMapper_Error>(common::unlock(buffer, releaseFence));
    return err;
}

AIMapper_Error GrallocMapper::flushLockedBuffer(buffer_handle_t _Nonnull buffer) {
    if (buffer == nullptr) return AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;

    AIMapper_Error err = static_cast<AIMapper_Error>(common::flushLockedBuffer(buffer));
    return err;
}

AIMapper_Error GrallocMapper::rereadLockedBuffer(buffer_handle_t _Nonnull buffer) {
    if (buffer == nullptr) return AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;
    return static_cast<AIMapper_Error>(common::rereadLockedBuffer(buffer));
}


int32_t GrallocMapper::getStandardMetadata(buffer_handle_t _Nonnull buffer,
                                           int64_t standardMetadataType, void* _Nonnull outData,
                                           size_t outDataSize) {
    if (buffer == nullptr) return -AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;
    return common::getStandardMetadata(static_cast<const private_handle_t*>(buffer),
                                      static_cast<StandardMetadataType>(standardMetadataType),
                                      outData, outDataSize);
}

int32_t GrallocMapper::getMetadata(buffer_handle_t _Nonnull buffer,
                                   AIMapper_MetadataType metadataType, void* _Nonnull outData,
                                   size_t outDataSize) {
    if (buffer == nullptr) return -AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;
    if (isStandardMetadataType(common::MetadataType(metadataType))) {
        return getStandardMetadata(buffer, metadataType.value, outData, outDataSize);
    } else
        return -AIMapper_Error::AIMAPPER_ERROR_UNSUPPORTED;
}

template <StandardMetadataType TYPE>
AIMapper_Error setStandardMetadataHelper(const private_handle_t* hnd,
                                         typename StandardMetadata<TYPE>::value_type&& value) {
    if constexpr (TYPE == StandardMetadataType::DATASPACE) {
        common::set_dataspace(hnd, value);
    }
    if constexpr (TYPE == StandardMetadataType::CROP) {
        common::set_crop_rect(hnd, value[0]);
    }
    if constexpr (TYPE == StandardMetadataType::BLEND_MODE) {
        common::set_blend_mode(hnd, value);
    }
    if constexpr (TYPE == StandardMetadataType::SMPTE2086) {
        common::set_smpte2086(hnd, value);
    }
    if constexpr (TYPE == StandardMetadataType::CTA861_3) {
        common::set_cta861_3(hnd, value);
    }
    if constexpr (TYPE == StandardMetadataType::SMPTE2094_40) {
        common::set_smpte2094_40(hnd, value);
    }
    return AIMapper_Error::AIMAPPER_ERROR_NONE;
}

AIMapper_Error GrallocMapper::setStandardMetadata(buffer_handle_t _Nonnull bufferHandle,
                                                  int64_t standardTypeRaw,
                                                  const void* _Nonnull metadata,
                                                  size_t metadataSize) {
    if (bufferHandle == nullptr) return AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;
    auto standardMeta = static_cast<StandardMetadataType>(standardTypeRaw);
    switch (standardMeta) {
        // Read-only values
        case StandardMetadataType::BUFFER_ID:
        case StandardMetadataType::NAME:
        case StandardMetadataType::WIDTH:
        case StandardMetadataType::HEIGHT:
        case StandardMetadataType::LAYER_COUNT:
        case StandardMetadataType::PIXEL_FORMAT_REQUESTED:
        case StandardMetadataType::USAGE:
            return AIMAPPER_ERROR_BAD_VALUE;

        // Supported to set
        case StandardMetadataType::BLEND_MODE:
        case StandardMetadataType::CTA861_3:
        case StandardMetadataType::DATASPACE:
        case StandardMetadataType::SMPTE2086:
        case StandardMetadataType::SMPTE2094_40:
        case StandardMetadataType::CROP:
            break;

        // Everything else unsupported
        default:
            return AIMAPPER_ERROR_UNSUPPORTED;
    }

    auto applier = [&]<StandardMetadataType meta>(auto&& value) -> AIMapper_Error {
        return setStandardMetadataHelper<meta>(static_cast<const private_handle_t*>(bufferHandle),
                                               std::forward<decltype(value)>(value));
    };

    return applyStandardMetadata(standardMeta, metadata, metadataSize, applier);
}

AIMapper_Error GrallocMapper::setMetadata(buffer_handle_t _Nonnull buffer,
                                          AIMapper_MetadataType metadataType,
                                          const void* _Nonnull metadata, size_t metadataSize) {
    if (buffer == nullptr) return AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;
    if (isStandardMetadataType(common::MetadataType(metadataType))) {
        return setStandardMetadata(buffer, metadataType.value, metadata, metadataSize);
    } else
        return AIMapper_Error::AIMAPPER_ERROR_NONE;
}

AIMapper_Error GrallocMapper::getTransportSize(buffer_handle_t _Nonnull buffer,
                                               uint32_t* _Nonnull outNumFds,
                                               uint32_t* _Nonnull outNumInts) {
    if (buffer == nullptr) return AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;
    AIMapper_Error err =
            static_cast<AIMapper_Error>(common::getTransportSize(buffer, outNumFds, outNumInts));
    return err;
}

AIMapper_Error GrallocMapper::listSupportedMetadataTypes(
        const AIMapper_MetadataTypeDescription* _Nullable* _Nonnull outDescriptionList,
        size_t* _Nonnull outNumberOfDescriptions) {
    std::vector<common::MetadataTypeDescription> desc = common::listSupportedMetadataTypes();
    std::vector<AIMapper_MetadataTypeDescription> outDesc;
    for (auto x : desc) {
        outDesc.push_back(static_cast<AIMapper_MetadataTypeDescription>(x));
    }

    *outDescriptionList = outDesc.data();
    *outNumberOfDescriptions = outDesc.size();
    return AIMapper_Error::AIMAPPER_ERROR_NONE;
}

AIMapper_Error GrallocMapper::dumpBuffer(buffer_handle_t _Nonnull bufferHandle,
                                         AIMapper_DumpBufferCallback _Nonnull dumpBufferCallback,
                                         void* _Null_unspecified context) {
    if (bufferHandle == nullptr) return AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;
    common::BufferDump out;
    AIMapper_Error err = static_cast<AIMapper_Error>(common::dumpBuffer(bufferHandle, out));
    if (err != AIMapper_Error::AIMAPPER_ERROR_NONE) return err;
    for (auto metadump : out.metadataDump) {
        dumpBufferCallback(context, static_cast<AIMapper_MetadataType>(metadump.metadataType),
                           static_cast<void*>((metadump.metadata).data()),
                           metadump.metadata.size());
    }
    return AIMapper_Error::AIMAPPER_ERROR_NONE;
}

AIMapper_Error GrallocMapper::dumpAllBuffers(
        AIMapper_BeginDumpBufferCallback _Nonnull beginDumpBufferCallback,
        AIMapper_DumpBufferCallback _Nonnull dumpBufferCallback, void* _Null_unspecified context) {
    std::vector<common::BufferDump> bufferDump = common::dumpBuffers();
    for (auto dump : bufferDump) {
        beginDumpBufferCallback(context);
        for (auto metadump : dump.metadataDump) {
            dumpBufferCallback(context, static_cast<AIMapper_MetadataType>(metadump.metadataType),
                               static_cast<void*>((metadump.metadata).data()),
                               metadump.metadata.size());
        }
    }
    return AIMapper_Error::AIMAPPER_ERROR_NONE;
}

AIMapper_Error GrallocMapper::getReservedRegion(buffer_handle_t _Nonnull buffer,
                                                void* _Nullable* _Nonnull outReservedRegion,
                                                uint64_t* _Nonnull outReservedSize) {
    if (buffer == nullptr) return AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;
    uint64_t reservedSize = 0;
    AIMapper_Error err = static_cast<AIMapper_Error>(
            common::getReservedRegion(buffer, outReservedRegion, reservedSize));
    *outReservedSize = reservedSize;
    return err;
}

} // namespace mapper
} // namespace arm

extern "C" uint32_t ANDROID_HAL_MAPPER_VERSION = AIMAPPER_VERSION_5;

extern "C" AIMapper_Error AIMapper_loadIMapper(AIMapper* _Nullable* _Nonnull outImplementation) {
    static vendor::mapper::IMapperProvider<arm::mapper::GrallocMapper> provider;
    return provider.load(outImplementation);
}
