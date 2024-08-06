/*
 * Copyright (C) 2020 ARM Limited. All rights reserved.
 *
 * Copyright 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>
#include <sync/sync.h>
#include <hardware/gralloc1.h>
#include "RegisteredHandlePool.h"
#include "Mapper.h"
#include "BufferDescriptor.h"
#include "mali_gralloc_log.h"
#include "core/mali_gralloc_bufferallocation.h"
#include "core/mali_gralloc_bufferdescriptor.h"
#include "core/mali_gralloc_bufferaccess.h"
#include "core/mali_gralloc_reference.h"
#include "core/format_info.h"
#include "allocator/mali_gralloc_ion.h"
#include "mali_gralloc_buffer.h"
#include "mali_gralloc_log.h"

#include "MapperMetadata.h"
#include "SharedMetadata.h"

#include "drmutils.h"

#include <array>
#include <cstdio>

/* GraphicBufferMapper is expected to be valid (and leaked) during process
 * termination. IMapper, and in turn, gRegisteredHandles must be valid as
 * well. Create the registered handle pool on the heap, and let
 * it leak for simplicity.
 *
 * However, there is no way to make sure gralloc0/gralloc1 are valid. Any use
 * of static/global object in gralloc0/gralloc1 that may have been destructed
 * is potentially broken.
 */
RegisteredHandlePool* gRegisteredHandles = new RegisteredHandlePool;

namespace arm {
namespace mapper {
namespace common {

buffer_handle_t getBuffer(void *buffer) {
	return gRegisteredHandles->get(buffer);
}

#ifdef GRALLOC_MAPPER_5

template <typename F, StandardMetadataType metadataType>
int32_t getStandardMetadataHelper(const private_handle_t* hnd, F&& provide,
                                  StandardMetadata<metadataType>) {
    if constexpr (metadataType == StandardMetadataType::BUFFER_ID) {
        return provide(hnd->backing_store_id);
    }
    if constexpr (metadataType == StandardMetadataType::WIDTH) {
        return provide(hnd->width);
    }
    if constexpr (metadataType == StandardMetadataType::HEIGHT) {
        return provide(hnd->height);
    }
    if constexpr (metadataType == StandardMetadataType::LAYER_COUNT) {
        return provide(hnd->layer_count);
    }
    if constexpr (metadataType == StandardMetadataType::PIXEL_FORMAT_REQUESTED) {
        return provide(static_cast<PixelFormat>(hnd->req_format));
    }
    if constexpr (metadataType == StandardMetadataType::PIXEL_FORMAT_FOURCC) {
        return provide(drm_fourcc_from_handle(hnd));
    }
    if constexpr (metadataType == StandardMetadataType::PIXEL_FORMAT_MODIFIER) {
        return provide(drm_modifier_from_handle(hnd));
    }
    if constexpr (metadataType == StandardMetadataType::USAGE) {
        return provide(static_cast<BufferUsage>(hnd->consumer_usage | hnd->producer_usage));
    }
    if constexpr (metadataType == StandardMetadataType::ALLOCATION_SIZE) {
        uint64_t total_size = 0;
        for (int fidx = 0; fidx < hnd->fd_count; fidx++) {
            total_size += hnd->alloc_sizes[fidx];
        }
        return provide(total_size);
    }
    if constexpr (metadataType == StandardMetadataType::PROTECTED_CONTENT) {
        return provide((((hnd->consumer_usage | hnd->producer_usage) &
                         static_cast<uint64_t>(BufferUsage::PROTECTED)) == 0)
                               ? 0
                               : 1);
    }
    if constexpr (metadataType == StandardMetadataType::COMPRESSION) {
        ExtendableType compression = android::gralloc4::Compression_None;
        if (hnd->alloc_format & MALI_GRALLOC_INTFMT_AFBC_BASIC)
            compression = common::Compression_AFBC;
        return provide(compression);
    }
    if constexpr (metadataType == StandardMetadataType::INTERLACED) {
        return provide(android::gralloc4::Interlaced_None);
    }
    if constexpr (metadataType == StandardMetadataType::CHROMA_SITING) {
        ExtendableType siting = android::gralloc4::ChromaSiting_None;
        int format_index = get_format_index(hnd->alloc_format & MALI_GRALLOC_INTFMT_FMT_MASK);
        if (formats[format_index].is_yuv) siting = android::gralloc4::ChromaSiting_Unknown;
        return provide(siting);
    }
    if constexpr (metadataType == StandardMetadataType::PLANE_LAYOUTS) {
        std::vector<PlaneLayout> layouts;
        Error err = static_cast<Error>(common::get_plane_layouts(hnd, &layouts));
        return provide(layouts);
    }
    if constexpr (metadataType == StandardMetadataType::NAME) {
        std::string name;
        common::get_name(hnd, &name);
        return provide(name);
    }
    if constexpr (metadataType == StandardMetadataType::CROP) {
        const int num_planes = common::get_num_planes(hnd);
        std::vector<Rect> crops(num_planes);
        for (size_t plane_index = 0; plane_index < num_planes; ++plane_index) {
            Rect rect = {.top = 0,
                         .left = 0,
                         .right = static_cast<int32_t>(hnd->plane_info[plane_index].alloc_width),
                         .bottom = static_cast<int32_t>(hnd->plane_info[plane_index].alloc_height)};
            if (plane_index == 0) {
                std::optional<Rect> crop_rect;
                common::get_crop_rect(hnd, &crop_rect);
                if (crop_rect.has_value()) {
                    rect = crop_rect.value();
                } else {
                    rect = {.top = 0, .left = 0, .right = hnd->width, .bottom = hnd->height};
                }
            }
            crops[plane_index] = rect;
        }
        return provide(crops);
    }
    if constexpr (metadataType == StandardMetadataType::DATASPACE) {
        std::optional<Dataspace> dataspace;
        common::get_dataspace(hnd, &dataspace);
        return provide(dataspace.value_or(Dataspace::UNKNOWN));
    }
    if constexpr (metadataType == StandardMetadataType::BLEND_MODE) {
        std::optional<BlendMode> blendmode;
        common::get_blend_mode(hnd, &blendmode);
        return provide(blendmode.value_or(BlendMode::INVALID));
    }
    if constexpr (metadataType == StandardMetadataType::SMPTE2086) {
        std::optional<Smpte2086> smpte2086;
        common::get_smpte2086(hnd, &smpte2086);
        return provide(smpte2086);
    }
    if constexpr (metadataType == StandardMetadataType::CTA861_3) {
        std::optional<Cta861_3> cta861_3;
        common::get_cta861_3(hnd, &cta861_3);
        return provide(cta861_3);
    }
    if constexpr (metadataType == StandardMetadataType::SMPTE2094_40) {
        std::optional<std::vector<uint8_t>> smpte2094_40;
        common::get_smpte2094_40(hnd, &smpte2094_40);
        return provide(smpte2094_40);
    }
    if constexpr (metadataType == StandardMetadataType::STRIDE) {
        std::vector<PlaneLayout> layouts;
        Error err = static_cast<Error>(common::get_plane_layouts(hnd, &layouts));
        uint64_t stride = 0;
        switch (hnd->get_alloc_format())
        {
            case HAL_PIXEL_FORMAT_RAW10:
            case HAL_PIXEL_FORMAT_RAW12:
                  stride = layouts[0].strideInBytes;
                  break;
            default:
                  stride = hnd->plane_info[0].alloc_width;
                  break;
        }
        return provide(stride);
    }
    return -AIMapper_Error::AIMAPPER_ERROR_UNSUPPORTED;
}

int32_t getStandardMetadata(const private_handle_t *handle, StandardMetadataType metadata_type,
                            void *outData, size_t outDataSize) {
    if (handle == nullptr) return -AIMapper_Error::AIMAPPER_ERROR_BAD_BUFFER;

    auto provider = [&]<StandardMetadataType meta>(auto &&provide) -> int32_t {
        return getStandardMetadataHelper(handle, provide, StandardMetadata<meta>{});
    };
    return android::hardware::graphics::mapper::provideStandardMetadata(metadata_type, outData,
                                                                      outDataSize, provider);
}

#endif

/*
 * Translates the register buffer API into existing gralloc implementation
 *
 * @param bufferHandle [in] Private handle for the buffer to be imported
 *
 * @return Error::BAD_BUFFER for an invalid buffer
 *         Error::NO_RESOURCES when unable to import the given buffer
 *         Error::NONE on successful import
 */
static Error registerBuffer(buffer_handle_t bufferHandle)
{
	if (private_handle_t::validate(bufferHandle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer: %p is corrupted", bufferHandle);
		return Error::BAD_BUFFER;
	}

	if (mali_gralloc_reference_retain(bufferHandle) < 0)
	{
		return Error::NO_RESOURCES;
	}

	return Error::NONE;
}

/*
 * Translates the unregister buffer API into existing gralloc implementation
 *
 * @param bufferHandle [in] Private handle for the buffer to be released
 *
 * @return Error::BAD_BUFFER for an invalid buffer / buffers which can't be released
 *         Error::NONE on successful release of the buffer
 */
static Error unregisterBuffer(buffer_handle_t bufferHandle)
{
	if (private_handle_t::validate(bufferHandle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer: %p is corrupted", bufferHandle);
		return Error::BAD_BUFFER;
	}

	const int status = mali_gralloc_reference_release(bufferHandle);
	if (status != 0)
	{
		MALI_GRALLOC_LOGE("Unable to release buffer:%p", bufferHandle);
		return Error::BAD_BUFFER;
	}

	return Error::NONE;
}

/*
 * Converts a gralloc error code to a mapper error code
 *
 * @param grallocError  [in] Gralloc error as integer.
 *
 * @return Corresponding Mapper error code
 *
 * @note There is no full 1:1 correspondence, several gralloc errors may map to Error::UNSUPPORTED.
 * @note -EINVAL is mapped to Error::BAD_VALUE.
 */
static Error grallocErrorToMapperError(int grallocError)
{
	switch(grallocError)
	{
		case GRALLOC1_ERROR_NONE:
			return Error::NONE;
		case GRALLOC1_ERROR_BAD_DESCRIPTOR:
			return Error::BAD_DESCRIPTOR;
		case GRALLOC1_ERROR_BAD_HANDLE:
			return Error::BAD_BUFFER;
		case GRALLOC1_ERROR_BAD_VALUE:
		case -EINVAL:
			return Error::BAD_VALUE;
		case GRALLOC1_ERROR_NO_RESOURCES:
			return Error::NO_RESOURCES;
		default:
			/* Covers NOT_SHARED, UNDEFINED, UNSUPPORTED */
			return Error::UNSUPPORTED;
	}
}

/*
 * Locks the given buffer for the specified CPU usage.
 *
 * @param bufferHandle [in]  Buffer to lock.
 * @param cpuUsage     [in]  Specifies one or more CPU usage flags to request
 * @param accessRegion [in]  Portion of the buffer that the client intends to
 * access.
 * @param fenceFd      [in]  Fence file descriptor
 * @param outData      [out] CPU accessible buffer address
 *
 * @return Error::BAD_BUFFER for an invalid buffer
 *         Error::NO_RESOURCES when unable to duplicate fence
 *         Error::BAD_VALUE when locking fails
 *         Error::UNSUPPORTED when locking fails on unsupported image formats
 *         Error::NONE on successful buffer lock
 */
static Error lockBuffer(buffer_handle_t bufferHandle,
                        uint64_t cpuUsage,
                        const GrallocRect& accessRegion, int fenceFd,
                        void** outData)
{
	/* dup fenceFd as it is going to be owned by gralloc. Note that it is
	 * gralloc's responsibility to close it, even on locking errors.
	 */
	if (fenceFd >= 0)
	{
		fenceFd = dup(fenceFd);
		if (fenceFd < 0)
		{
			MALI_GRALLOC_LOGE("Error encountered while duplicating fence file descriptor");
			return Error::NO_RESOURCES;
		}
	}

	if (private_handle_t::validate(bufferHandle) < 0)
	{
		if (fenceFd >= 0)
		{
			close(fenceFd);
		}
		MALI_GRALLOC_LOGE("Buffer: %p is corrupted", bufferHandle);
		return Error::BAD_BUFFER;
	}

	auto private_handle = private_handle_t::dynamicCast(bufferHandle);
	if (private_handle->cpu_write != 0 && (cpuUsage & static_cast<uint64_t>(BufferUsage::CPU_WRITE_MASK)))
	{
		if (fenceFd >= 0)
		{
			close(fenceFd);
		}
#if 0
		MALI_GRALLOC_LOGW("Attempt to call lock*() for writing on an already locked buffer (%p)", bufferHandle);
#endif

		/* TODO: handle simulatneous locks differently. May be keep a global lock count per buffer? */
	}
	else if (fenceFd >= 0)
	{
		sync_wait(fenceFd, -1);
		close(fenceFd);
	}

	void* data = nullptr;
	const int gralloc_err =
		mali_gralloc_lock(bufferHandle, cpuUsage, accessRegion.left, accessRegion.top,
				  accessRegion.right - accessRegion.left,
				  accessRegion.bottom - accessRegion.top, &data);
	const Error lock_err = grallocErrorToMapperError(gralloc_err);

	if(Error::NONE == lock_err)
	{
		*outData = data;
	}
	else
	{
		MALI_GRALLOC_LOGE("Locking failed with error: %d", gralloc_err);
	}

	return lock_err;
}

/*
 * Unlocks a buffer to indicate all CPU accesses to the buffer have completed
 *
 * @param bufferHandle [in]  Buffer to lock.
 * @param outFenceFd   [out] Fence file descriptor
 *
 * @return Error::BAD_BUFFER for an invalid buffer
 *         Error::BAD_VALUE when unlocking failed
 *         Error::NONE on successful buffer unlock
 */
static Error unlockBuffer(buffer_handle_t bufferHandle,
                                  int* outFenceFd)
{
	if (private_handle_t::validate(bufferHandle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer: %p is corrupted", bufferHandle);
		return Error::BAD_BUFFER;
	}

	const int gralloc_err = mali_gralloc_unlock(bufferHandle);
	const Error unlock_err = grallocErrorToMapperError(gralloc_err);

	if (Error::NONE == unlock_err)
	{
		*outFenceFd = -1;
	}
	else
	{
		MALI_GRALLOC_LOGE("Unlocking failed with error: %d",
				  gralloc_err);
	}

	return unlock_err;
}

Error importBuffer(const native_handle_t *inBuffer, buffer_handle_t *outBuffer)
{
	*outBuffer = nullptr;
	if (inBuffer == nullptr) return Error::BAD_BUFFER;
	native_handle_t *bufferHandle = native_handle_clone(inBuffer);
	if (bufferHandle == nullptr) return Error::NO_RESOURCES;
	const Error error = registerBuffer(bufferHandle);
	if (error != Error::NONE)
	{
		native_handle_close(bufferHandle);
		native_handle_delete(bufferHandle);
		return error;
	}

	auto *private_handle = static_cast<private_handle_t *>(bufferHandle);
	private_handle->attr_base = mmap(nullptr, private_handle->attr_size, PROT_READ | PROT_WRITE,
	                                 MAP_SHARED, private_handle->get_share_attr_fd(), 0);
	if (private_handle->attr_base == MAP_FAILED)
	{
		private_handle->attr_base = nullptr;
		unregisterBuffer(bufferHandle);
		native_handle_close(bufferHandle);
		native_handle_delete(bufferHandle);
		return Error::NO_RESOURCES;
	}

	if (gRegisteredHandles->add(bufferHandle) == false)
	{
		/* The newly cloned handle is already registered. This can only happen
		 * when a handle previously registered was native_handle_delete'd instead
		 * of freeBuffer'd.
		 */
		MALI_GRALLOC_LOGE("Handle %p has already been imported; potential fd leaking",
		       bufferHandle);
		unregisterBuffer(bufferHandle);
		native_handle_close(bufferHandle);
		native_handle_delete(bufferHandle);
		return Error::NO_RESOURCES;
	}

	*outBuffer = bufferHandle;
	return Error::NONE;
}

Error freeBuffer(buffer_handle_t bufferHandle)
{
	native_handle_t *handle = gRegisteredHandles->remove(bufferHandle);
	if (handle == nullptr)
	{
		MALI_GRALLOC_LOGE("Invalid buffer handle %p to freeBuffer", bufferHandle);
		return Error::BAD_BUFFER;
	}

	const Error status = unregisterBuffer(handle);
	if (status != Error::NONE)
	{
		return status;
	}

	native_handle_close(handle);
	native_handle_delete(handle);

	return Error::NONE;
}

Error lock(buffer_handle_t bufferHandle, uint64_t cpuUsage, const GrallocRect &accessRegion, int acquireFence, void **outData)
{
	*outData = nullptr;
	if (!bufferHandle || private_handle_t::validate(bufferHandle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer to lock: %p is not valid",
				  bufferHandle);
		return Error::BAD_BUFFER;
	}

	const Error error = lockBuffer(bufferHandle, cpuUsage, accessRegion,
				       acquireFence, outData);
	return error;
}

Error unlock(buffer_handle_t bufferHandle, int *releaseFence) {
	if(bufferHandle == nullptr) return Error::BAD_BUFFER;
	if(!gRegisteredHandles->isRegistered(bufferHandle)) {
		MALI_GRALLOC_LOGE("Buffer to unlock: %p has not been registered with Gralloc",
		    bufferHandle);
		return Error::BAD_BUFFER;
	}

	const Error error = unlockBuffer(bufferHandle, releaseFence);
	return error;
}
#ifdef GRALLOC_MAPPER_4
Error validateBufferSize(void* buffer,
                         const IMapper::BufferDescriptorInfo& descriptorInfo,
                         uint32_t in_stride)
{
	/* The buffer must have been allocated by Gralloc */
	buffer_handle_t bufferHandle = gRegisteredHandles->get(buffer);
	if (!bufferHandle)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has not been registered with Gralloc", buffer);
		return Error::BAD_BUFFER;
	}

	if (private_handle_t::validate(bufferHandle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer: %p is corrupted", bufferHandle);
		return Error::BAD_BUFFER;
	}

	buffer_descriptor_t grallocDescriptor;
	grallocDescriptor.width = descriptorInfo.width;
	grallocDescriptor.height = descriptorInfo.height;
	grallocDescriptor.layer_count = descriptorInfo.layerCount;
	grallocDescriptor.hal_format = static_cast<uint64_t>(descriptorInfo.format);
	grallocDescriptor.producer_usage = static_cast<uint64_t>(descriptorInfo.usage);
	grallocDescriptor.consumer_usage = grallocDescriptor.producer_usage;
	grallocDescriptor.format_type = MALI_GRALLOC_FORMAT_TYPE_USAGE;

	/* Derive the buffer size for the given descriptor */
	const int result = mali_gralloc_derive_format_and_size(&grallocDescriptor);
	if (result)
	{
		MALI_GRALLOC_LOGV("Unable to derive format and size for the given descriptor information. error: %d", result);
		return Error::BAD_VALUE;
	}

	/* Validate the buffer parameters against descriptor info */
	private_handle_t *gralloc_buffer = (private_handle_t *)bufferHandle;

	/* The buffer size must be greater than (or equal to) what would have been allocated with descriptor */
	for (int i = 0; i < gralloc_buffer->fd_count; i++)
	{
		if (gralloc_buffer->alloc_sizes[i] < grallocDescriptor.alloc_sizes[i])
		{
			MALI_GRALLOC_LOGW("Buf size mismatch. fd_idx(%d) Buffer size = %" PRIu64 ", Descriptor (derived) size = %" PRIu64,
			       i, gralloc_buffer->alloc_sizes[i], grallocDescriptor.alloc_sizes[i]);
			return Error::BAD_VALUE;
		}
	}

	if (in_stride != 0 && (uint32_t)gralloc_buffer->stride != in_stride)
	{
		MALI_GRALLOC_LOGE("Stride mismatch. Expected stride = %d, Buffer stride = %" PRIu64,
		                       in_stride, gralloc_buffer->stride);
		return Error::BAD_VALUE;
	}

	if (gralloc_buffer->alloc_format != grallocDescriptor.alloc_format)
	{
		MALI_GRALLOC_LOGE("Buffer alloc format: (%s, 0x%" PRIx64") does not match descriptor (derived) alloc format: (%s 0x%"
			PRIx64 ")", format_name(gralloc_buffer->alloc_format), gralloc_buffer->alloc_format,
			format_name(grallocDescriptor.alloc_format), grallocDescriptor.alloc_format);
		return Error::BAD_VALUE;
	}

	const int format_idx = get_format_index(gralloc_buffer->alloc_format & MALI_GRALLOC_INTFMT_FMT_MASK);
	if (format_idx == -1)
	{
		MALI_GRALLOC_LOGE("Invalid format to validate buffer descriptor");
		return Error::BAD_VALUE;
	}
	else
	{
		for (int i = 0; i < formats[format_idx].npln; i++)
		{
			if (gralloc_buffer->plane_info[i].byte_stride != grallocDescriptor.plane_info[i].byte_stride)
			{
				MALI_GRALLOC_LOGE("Buffer byte stride %" PRIu64 " mismatch with desc byte stride %" PRIu64 " in plane %d ",
				      gralloc_buffer->plane_info[i].byte_stride, grallocDescriptor.plane_info[i].byte_stride, i);
				return Error::BAD_VALUE;
			}

			if (gralloc_buffer->plane_info[i].alloc_width != grallocDescriptor.plane_info[i].alloc_width)
			{
				MALI_GRALLOC_LOGE("Buffer alloc width %" PRIu64 " mismatch with desc alloc width %" PRIu64 " in plane %d ",
				      gralloc_buffer->plane_info[i].alloc_width, grallocDescriptor.plane_info[i].alloc_width, i);
				return Error::BAD_VALUE;
			}

			if (gralloc_buffer->plane_info[i].alloc_height != grallocDescriptor.plane_info[i].alloc_height)
			{
				MALI_GRALLOC_LOGE("Buffer alloc height %" PRIu64 " mismatch with desc alloc height %" PRIu64 " in plane %d ",
				      gralloc_buffer->plane_info[i].alloc_height, grallocDescriptor.plane_info[i].alloc_height, i);
				return Error::BAD_VALUE;
			}
		}
	}

	if ((uint32_t)gralloc_buffer->width != grallocDescriptor.width)
	{
		MALI_GRALLOC_LOGE("Width mismatch. Buffer width = %u, Descriptor width = %u",
		      gralloc_buffer->width, grallocDescriptor.width);
		return Error::BAD_VALUE;
	}

	if ((uint32_t)gralloc_buffer->height != grallocDescriptor.height)
	{
		MALI_GRALLOC_LOGE("Height mismatch. Buffer height = %u, Descriptor height = %u",
		      gralloc_buffer->height, grallocDescriptor.height);
		return Error::BAD_VALUE;
	}

	if (gralloc_buffer->layer_count != grallocDescriptor.layer_count)
	{
		MALI_GRALLOC_LOGE("Layer Count mismatch. Buffer layer_count = %u, Descriptor layer_count width = %u",
		      gralloc_buffer->layer_count, grallocDescriptor.layer_count);
		return Error::BAD_VALUE;
	}

	return Error::NONE;
}
#endif

Error getTransportSize(buffer_handle_t bufferHandle, uint32_t *outNumFds, uint32_t *outNumInts)
{
	*outNumFds = 0;
	*outNumInts = 0;
	/* The buffer must have been allocated by Gralloc */
	if (!bufferHandle)
	{
		MALI_GRALLOC_LOGE("Buffer %p is not registered with Gralloc",
				  bufferHandle);
		return Error::BAD_BUFFER;
	}

	if (private_handle_t::validate(bufferHandle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer %p is corrupted", bufferHandle);
		return Error::BAD_BUFFER;
	}
	*outNumFds = bufferHandle->numFds;
	*outNumInts = bufferHandle->numInts;
	return Error::NONE;
}

#ifdef GRALLOC_MAPPER_4
bool isSupported(const IMapper::BufferDescriptorInfo &description)
{
	buffer_descriptor_t grallocDescriptor;
	grallocDescriptor.width = description.width;
	grallocDescriptor.height = description.height;
	grallocDescriptor.layer_count = description.layerCount;
	grallocDescriptor.hal_format = static_cast<uint64_t>(description.format);
	grallocDescriptor.producer_usage = static_cast<uint64_t>(description.usage);
	grallocDescriptor.consumer_usage = grallocDescriptor.producer_usage;
	grallocDescriptor.format_type = MALI_GRALLOC_FORMAT_TYPE_USAGE;

	/* Check if it is possible to allocate a buffer for the given description */
	const int result = mali_gralloc_derive_format_and_size(&grallocDescriptor);
	if (result != 0)
	{
		MALI_GRALLOC_LOGV("Allocation for the given description will not succeed. error: %d", result);
		return false;
	}
	else
	{
		return true;
	}
}

#endif
Error flushLockedBuffer(buffer_handle_t handle)
{
	if (private_handle_t::validate(handle) < 0)
	{
		MALI_GRALLOC_LOGE("Handle: %p is corrupted", handle);
		return Error::BAD_BUFFER;
	}

	auto private_handle = static_cast<const private_handle_t *>(handle);
	if (!private_handle->cpu_write && !private_handle->cpu_read)
	{
		MALI_GRALLOC_LOGE("Attempt to call flushLockedBuffer() on an unlocked buffer (%p)", handle);
		return Error::BAD_BUFFER;
	}

	mali_gralloc_ion_sync_end(private_handle, false, true);
	return Error::NONE;
}

Error rereadLockedBuffer(buffer_handle_t handle)
{
	if (private_handle_t::validate(handle) < 0)
	{
		MALI_GRALLOC_LOGE("Buffer: %p is corrupted", handle);
		return Error::BAD_BUFFER;
	}

	auto private_handle = static_cast<const private_handle_t *>(handle);
	if (!private_handle->cpu_write && !private_handle->cpu_read)
	{
		MALI_GRALLOC_LOGE("Attempt to call rereadLockedBuffer() on an unlocked buffer (%p)", handle);
		return Error::BAD_BUFFER;
	}

	mali_gralloc_ion_sync_start(private_handle, true, false);
	return Error::NONE;
}

Error get(buffer_handle_t buffer, const MetadataType &metadataType, std::vector<uint8_t> &vec)
{
	/* The buffer must have been allocated by Gralloc */
	const private_handle_t *handle = static_cast<const private_handle_t *>(buffer);
	if (handle == nullptr)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has not been registered with Gralloc", buffer);
		return Error::BAD_BUFFER;
	}
	return get_metadata(handle, metadataType, vec);
}

Error set(buffer_handle_t buffer, const MetadataType &metadataType, const frameworks_vec<uint8_t> &metadata)
{
	/* The buffer must have been allocated by Gralloc */
	const private_handle_t *handle = static_cast<const private_handle_t *>(buffer);
	if (handle == nullptr)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has not been registered with Gralloc", buffer);
		return Error::BAD_BUFFER;
	}
	return set_metadata(handle, metadataType, metadata);
}

MetadataTypeDescription describeStandard(StandardMetadataType meta, bool isGettable, bool isSettable)
{
	return MetadataTypeDescription(MetadataType(GRALLOC4_STANDARD_METADATA_TYPE,
						    static_cast<uint64_t>(meta)), "", isGettable, isSettable);
}

std::vector<MetadataTypeDescription> listSupportedMetadataTypes()
{
	/* Returns a vector of {metadata type, description, isGettable, isSettable}
	*  Only non-standardMetadataTypes require a description.
	*/
	std::array<MetadataTypeDescription, 22> descriptions = {
		describeStandard(StandardMetadataType::BUFFER_ID, true, false ),
		describeStandard(StandardMetadataType::NAME, true, false ),
		describeStandard(StandardMetadataType::WIDTH, true, false ),
		describeStandard(StandardMetadataType::HEIGHT, true, false ),
		describeStandard(StandardMetadataType::LAYER_COUNT, true, false ),
		describeStandard(StandardMetadataType::PIXEL_FORMAT_REQUESTED, true, false ),
		describeStandard(StandardMetadataType::PIXEL_FORMAT_FOURCC, true, false ),
		describeStandard(StandardMetadataType::PIXEL_FORMAT_MODIFIER, true, false ),
		describeStandard(StandardMetadataType::USAGE, true, false ),
		describeStandard(StandardMetadataType::ALLOCATION_SIZE, true, false ),
		describeStandard(StandardMetadataType::PROTECTED_CONTENT, true, false ),
		describeStandard(StandardMetadataType::COMPRESSION, true, false ),
		describeStandard(StandardMetadataType::INTERLACED, true, false ),
		describeStandard(StandardMetadataType::CHROMA_SITING, true, false ),
		describeStandard(StandardMetadataType::PLANE_LAYOUTS, true, false ),
		describeStandard(StandardMetadataType::DATASPACE, true, true ),
		describeStandard(StandardMetadataType::BLEND_MODE, true, true ),
		describeStandard(StandardMetadataType::SMPTE2086, true, true ),
		describeStandard(StandardMetadataType::CTA861_3, true, true ),
		describeStandard(StandardMetadataType::SMPTE2094_40, true, true ),
		describeStandard(StandardMetadataType::CROP, true, true ),
		/* Arm vendor metadata */
		{ ArmMetadataType_PLANE_FDS,
			"Vector of file descriptors of each plane", true, false},
	};
	return std::vector<MetadataTypeDescription>(descriptions.begin(), descriptions.end());
}


static BufferDump dumpBufferHelper(const private_handle_t *handle)
{
	static std::array<MetadataType, 21> standardMetadataTypes = {
		MetadataType(StandardMetadataType::BUFFER_ID),
		MetadataType(StandardMetadataType::NAME),
		MetadataType(StandardMetadataType::WIDTH),
		MetadataType(StandardMetadataType::HEIGHT),
		MetadataType(StandardMetadataType::LAYER_COUNT),
		MetadataType(StandardMetadataType::PIXEL_FORMAT_REQUESTED),
		MetadataType(StandardMetadataType::PIXEL_FORMAT_FOURCC),
		MetadataType(StandardMetadataType::PIXEL_FORMAT_MODIFIER),
		MetadataType(StandardMetadataType::USAGE),
		MetadataType(StandardMetadataType::ALLOCATION_SIZE),
		MetadataType(StandardMetadataType::PROTECTED_CONTENT),
		MetadataType(StandardMetadataType::COMPRESSION),
		MetadataType(StandardMetadataType::INTERLACED),
		MetadataType(StandardMetadataType::CHROMA_SITING),
		MetadataType(StandardMetadataType::PLANE_LAYOUTS),
		MetadataType(StandardMetadataType::DATASPACE),
		MetadataType(StandardMetadataType::BLEND_MODE),
		MetadataType(StandardMetadataType::SMPTE2086),
		MetadataType(StandardMetadataType::CTA861_3),
		MetadataType(StandardMetadataType::SMPTE2094_40),
		MetadataType(StandardMetadataType::CROP),
	};

	std::vector<MetadataDump> metadataDumps;
	for (const auto& metadataType: standardMetadataTypes)
	{
		std::vector<uint8_t> metadata;
#ifdef GRALLOC_MAPPER_5
		const auto type = static_cast<StandardMetadataType>(metadataType.value);
		int32_t result = getStandardMetadata(handle, type, nullptr, 0);
		if (result >= 0)
		{
			metadata.resize(result);
			result = getStandardMetadata(handle, type, metadata.data(), metadata.size());
		}
		Error error = Error::NONE;
		if (result < 0)
		{
			error = static_cast<Error>(-result);
		}
		else if (static_cast<size_t>(result) > metadata.size())
		{
			error = Error::NO_RESOURCES;
		}
		else
		{
			metadata.resize(result);
		}
#else
		Error error = get_metadata(handle, metadataType, metadata);
#endif
		if (error == Error::NONE)
		{
			metadataDumps.push_back(MetadataDump(MetadataType(metadataType), metadata));
		}
		else
		{
			return BufferDump();
		}
	}
	return BufferDump(metadataDumps);
}

Error dumpBuffer(buffer_handle_t buffer, BufferDump &bufferDump)
{
	auto handle = static_cast<const private_handle_t *>(buffer);
	if (handle == nullptr)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has not been registered with Gralloc", buffer);
		return Error::BAD_BUFFER;
	}

	bufferDump = dumpBufferHelper(handle);
	return Error::NONE;
}

std::vector<BufferDump> dumpBuffers()
{
	std::vector<BufferDump> bufferDumps;
	gRegisteredHandles->for_each([&bufferDumps](buffer_handle_t buffer) {
	    BufferDump bufferDump;
	    auto err = dumpBuffer(buffer, bufferDump);
	    bufferDumps.push_back(bufferDump);
	});
	return bufferDumps;
}

Error getReservedRegion(buffer_handle_t buffer, void **outReservedRegion, uint64_t &outReservedSize)
{
	auto handle = static_cast<const private_handle_t *>(buffer);
	if (handle == nullptr)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has not been registered with Gralloc", buffer);
		return Error::BAD_BUFFER;
	}
	else if (handle->reserved_region_size == 0)
	{
		MALI_GRALLOC_LOGE("Buffer: %p has no reserved region", buffer);
		return Error::BAD_BUFFER;
	}
	*outReservedRegion = static_cast<std::byte *>(handle->attr_base)
	    + mapper::common::shared_metadata_size();
	outReservedSize = handle->reserved_region_size;
	return Error::NONE;
}

} // namespace common
} // namespace mapper
} // namespace arm
