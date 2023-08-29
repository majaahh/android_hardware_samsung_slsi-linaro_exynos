/*
 * Copyright (C) 2016-2020 ARM Limited. All rights reserved.
 *
 * Copyright (C) 2008 The Android Open Source Project
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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>

#include <log/log.h>
#include <cutils/atomic.h>
#include <utils/Trace.h>

#include <linux/dma-buf.h>
#include <vector>
#include <string>
#include <sstream>
#include <sys/ioctl.h>

#include <hardware/hardware.h>
#include <hardware/gralloc1.h>

#include <BufferAllocator/BufferAllocator.h>
#include "mali_gralloc_buffer.h"
#include "gralloc_helper.h"
#include "mali_gralloc_formats.h"
#include "mali_gralloc_usages.h"
#include "core/format_info.h"
#include "core/mali_gralloc_bufferdescriptor.h"
#include "core/mali_gralloc_bufferallocation.h"

#include "mali_gralloc_ion.h"

#include <cassert>

static const char kDmabufSensorDirectHeapName[] = "sensor_direct_heap";
static const char kDmabufVscalerSecureHeapName[] = "vscaler-secure";

BufferAllocator& get_allocator() {
		static BufferAllocator allocator;
		return allocator;
}

std::string select_dmabuf_heap(uint64_t usage)
{
	const bool cached = (usage & GRALLOC_USAGE_SW_READ_MASK) == GRALLOC_USAGE_SW_READ_OFTEN;
	const char *system_heap = cached ? kDmabufSystemHeapName : kDmabufSystemUncachedHeapName;

	if (usage & GRALLOC_USAGE_PROTECTED)
	{
		if (usage & GRALLOC_USAGE_CAMERA_RESERVED)
		{
			return system_heap;
		}
		if (usage & GRALLOC_USAGE_SECURE_CAMERA_RESERVED)
		{
			return "secure_camera-secure";
		}
		if (usage & GRALLOC_USAGE_VIDEO_EXT)
		{
			return "system-secure-vstream-secure";
		}
		if ((usage & GRALLOC_USAGE_HW_COMPOSER) &&
		    !(usage & (GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER)))
		{
			return kDmabufVscalerSecureHeapName;
		}
		return "vframe-secure";
	}
	if (usage & GRALLOC_USAGE_CAMERA_RESERVED)
	{
		return cached ? "camera" : "camera-uncached";
	}
	if (usage & GRALLOC_USAGE_SECURE_CAMERA_RESERVED)
	{
		return "secure_camera-secure";
	}
	if (usage & GRALLOC_USAGE_SENSOR_DIRECT_DATA)
	{
		return kDmabufSensorDirectHeapName;
	}
	return system_heap;
}

int alloc_from_dmabuf_heap(uint64_t usage, size_t size)
{
	ATRACE_CALL();
	if (size == 0)
	{
		return -1;
	}

	const auto heap_name = select_dmabuf_heap(usage);
	std::stringstream tag;
	tag << "heap: " << heap_name << ", bytes: " << size;
	ATRACE_NAME(tag.str().c_str());
	int shared_fd = get_allocator().Alloc(heap_name, size, 0);
	if (shared_fd < 0)
	{
		ALOGE("Allocation failed for heap %s error: %d\n", heap_name.c_str(), shared_fd);
	}

	return shared_fd;
}

int mali_gralloc_ion_sync(const private_handle_t * const hnd,
                                       const bool /* read */,
                                       const bool /* write */,
                                       const bool /* start */)
{
	if (hnd == NULL)
	{
		return -EINVAL;
	}

	return 0;
}


int mali_gralloc_ion_sync_start(const private_handle_t * const hnd,
                                const bool read,
                                const bool write)
{
	return mali_gralloc_ion_sync(hnd, read, write, true);
}


int mali_gralloc_ion_sync_end(const private_handle_t * const hnd,
                              const bool read,
                              const bool write)
{
	return mali_gralloc_ion_sync(hnd, read, write, false);
}


void mali_gralloc_ion_free(private_handle_t * const hnd)
{
	for (int i = 0; i < hnd->fd_count; i++)
	{
		void* mapped_addr = reinterpret_cast<void*>(hnd->bases[i]);

		/* Buffer might be unregistered already so we need to assure we have a valid handle */
		if (mapped_addr != nullptr)
		{
			if (munmap(mapped_addr, hnd->alloc_sizes[i]) != 0)
			{
				/* TODO: more detailed error logs */
				MALI_GRALLOC_LOGE("Failed to munmap handle %p", hnd);
			}
		}
		close(hnd->fds[i]);
		hnd->fds[i] = -1;
		hnd->bases[i] = 0;
	}
}

void mali_gralloc_ion_free_internal(buffer_handle_t * const pHandle,
                                           const uint32_t num_hnds)
{
	for (uint32_t i = 0; i < num_hnds; i++)
	{
		if (pHandle[i] != NULL)
		{
			private_handle_t * const hnd = (private_handle_t * const)pHandle[i];
			mali_gralloc_ion_free(hnd);
			delete hnd;
			pHandle[i] = nullptr;
		}
	}
}

int mali_gralloc_ion_allocate_attr(private_handle_t *hnd)
{
	ATRACE_CALL();

	int idx = hnd->get_share_attr_fd_index();
	assert(idx >= 0);
	if(idx == -1)
	{
		/* return undefined share_attr index */
		MALI_GRALLOC_LOGE("failed to get share_attr_fd_index from private_handle");
		return -1;
	}
	uint64_t usage = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;

	hnd->fds[idx] = alloc_from_dmabuf_heap(usage, hnd->attr_size);
	if (hnd->fds[idx] < 0)
	{
		MALI_GRALLOC_LOGE("ion_alloc failed");
		return -1;
	}

	hnd->incr_numfds(1);

	return 0;
}

static inline int createBufferContainer(int *fd, int size, int *containerFd)
{
	int bufferContainerFd = -1;

	if (fd == NULL || size < 1 || containerFd == NULL)
	{
		MALI_GRALLOC_LOGE("invalid parameters. fd(%p), size(%d), containerFd(%p)",
				fd, size, containerFd);
		return -EINVAL;
	}

	struct dma_buf_merge
	{
		int *dma_bufs;
		int32_t count;
		int32_t dmabuf_container;
		uint32_t reserved[2];
	} data = {};

	int container_device = open("/dev/dmabuf_container", O_RDONLY | O_CLOEXEC);
	int ret;
	if (container_device >= 0)
	{
		data.dma_bufs = fd;
		data.count = size;
		ret = ioctl(container_device, _IOWR('D', 1, struct dma_buf_merge), &data);
		close(container_device);
	}
	else
	{
		data.dma_bufs = fd + 1;
		data.count = size - 1;
		ret = ioctl(fd[0], _IOWR('b', 13, struct dma_buf_merge), &data);
	}

	bufferContainerFd = ret < 0 ? ret : data.dmabuf_container;
	if (bufferContainerFd < 0)
	{
		MALI_GRALLOC_LOGE("fail to create Buffer Container. containerFd(%d), size(%d)",
				bufferContainerFd, size);
		return -EINVAL;
	}

	*containerFd = bufferContainerFd;

	return 0;
}

static int allocate_to_container(buffer_descriptor_t *bufDescriptor, int fd_arr[])
{
	/* TODO: make batch size as BoardConfig option */
#define GRALLOC_HFR_BATCH_SIZE 8
	const int layer_count = GRALLOC_HFR_BATCH_SIZE;
	int hfr_fds[3][layer_count];
	int err = 0;
	int64_t usage = bufDescriptor->consumer_usage | bufDescriptor->producer_usage;

	memset(hfr_fds, 0xff, sizeof(hfr_fds));

	for (int layer = 0; layer < layer_count; layer++)
	{
		for (int fidx = 0; fidx < bufDescriptor->fd_count; fidx++)
		{
			hfr_fds[fidx][layer] = alloc_from_dmabuf_heap(usage,
					bufDescriptor->alloc_sizes[fidx]);

			if (hfr_fds[fidx][layer] < 0)
			{
				MALI_GRALLOC_LOGE("ion_alloc failed");
				err = -1;
				goto finish;
			}
		}
	}


	for (int fidx = 0; fidx < bufDescriptor->fd_count; fidx++)
	{
		err = createBufferContainer(hfr_fds[fidx], layer_count, &fd_arr[fidx]);
		if (err)
		{
			for (int i = 0; i < fidx; i++)
			{
				if (fd_arr[i] >= 0)
				{
					close(fd_arr[i]);
					fd_arr[i] = -1;
				}
			}

			goto finish;
		}
	}

finish:
	/* free single fds to make ref count to 1 */
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < layer_count; j++)
			if (hfr_fds[i][j] >= 0) close(hfr_fds[i][j]);

	return err;
}

/*
 *  Allocates ION buffers
 *
 * @param descriptors     [in]    Buffer request descriptors
 * @param numDescriptors  [in]    Number of descriptors
 * @param pHandle         [out]   Handle for each allocated buffer
 * @param shared_backend  [out]   Shared buffers flag
 *
 * @return File handle which can be used for allocation, on success
 *         -1, otherwise.
 */
int mali_gralloc_ion_allocate(const gralloc_buffer_descriptor_t *descriptors,
                              uint32_t numDescriptors, buffer_handle_t *pHandle,
                              bool *shared_backend)
{
	ATRACE_CALL();
	GRALLOC_UNUSED(shared_backend);

	uint64_t usage;
	uint32_t i;

	for (i = 0; i < numDescriptors; i++)
	{
		buffer_descriptor_t *bufDescriptor = reinterpret_cast<buffer_descriptor_t *>(descriptors[i]);
		assert(bufDescriptor);
		assert(bufDescriptor->fd_count <= MAX_PLANES);

		usage = bufDescriptor->consumer_usage | bufDescriptor->producer_usage;
		const bool is_hfr = usage & GRALLOC_USAGE_HFR_MODE;
		const unsigned int priv_heap_flag = is_hfr ? private_handle_t::PRIV_FLAGS_USES_HFR_MODE : 0;

		auto hnd = new private_handle_t(
		    priv_heap_flag,
		    bufDescriptor->alloc_sizes,
		    bufDescriptor->consumer_usage, bufDescriptor->producer_usage,
		    nullptr, bufDescriptor->fd_count,
		    bufDescriptor->hal_format, bufDescriptor->alloc_format,
		    bufDescriptor->width, bufDescriptor->height, bufDescriptor->pixel_stride,
		    bufDescriptor->layer_count, bufDescriptor->plane_info);

		if (nullptr == hnd)
		{
			MALI_GRALLOC_LOGE("Private handle could not be created for descriptor:%d in non-shared usecase", i);
			mali_gralloc_ion_free_internal(pHandle, i);
			return -1;
		}

		/* Count only successfully allocated fds for partial-allocation cleanup. */
		hnd->set_numfds(0);
		pHandle[i] = hnd;

		if (is_hfr)
		{
			if (allocate_to_container(bufDescriptor, hnd->fds) < 0)
			{
				MALI_GRALLOC_LOGE("allocate to container failed");
				mali_gralloc_ion_free_internal(pHandle, i + 1);
				return -1;
			}
			hnd->incr_numfds(bufDescriptor->fd_count);
		}
		else
		{
			for (uint32_t fidx = 0; fidx < bufDescriptor->fd_count; fidx++)
			{
				int& fd = hnd->fds[fidx];
				fd = alloc_from_dmabuf_heap(usage, bufDescriptor->alloc_sizes[fidx]);
				if (fd < 0)
				{
					MALI_GRALLOC_LOGE("ion_alloc failed for fds[%u] = %d", fidx, fd);
					mali_gralloc_ion_free_internal(pHandle, i + 1);
					return -1;
				}
				hnd->incr_numfds(1);
			}
		}
	}

#if defined(GRALLOC_INIT_AFBC) && (GRALLOC_INIT_AFBC == 1)
	ATRACE_NAME("AFBC init block");
	unsigned char *cpu_ptr = NULL;
	for (i = 0; i < numDescriptors; i++)
	{
		buffer_descriptor_t *bufDescriptor = reinterpret_cast<buffer_descriptor_t *>(descriptors[i]);
		const private_handle_t *hnd = static_cast<const private_handle_t *>(pHandle[i]);

		usage = bufDescriptor->consumer_usage | bufDescriptor->producer_usage;

		if ((bufDescriptor->alloc_format & MALI_GRALLOC_INTFMT_AFBCENABLE_MASK)
			&& !(usage & GRALLOC_USAGE_PROTECTED))
		{
			{
				ATRACE_NAME("mmap");
				/* TODO: only map for AFBC buffers */
				cpu_ptr =
				    (unsigned char *)mmap(NULL, bufDescriptor->alloc_sizes[0], PROT_READ | PROT_WRITE, MAP_SHARED, hnd->fds[0], 0);

				if (MAP_FAILED == cpu_ptr)
				{
					MALI_GRALLOC_LOGE("mmap failed for fd ( %d )", hnd->fds[0]);
					mali_gralloc_ion_free_internal(pHandle, numDescriptors);
					return -1;
				}

				mali_gralloc_ion_sync_start(hnd, true, true);
			}

			{
				ATRACE_NAME("data init");
				/* For separated plane YUV, there is a header to initialise per plane. */
				const plane_info_t *plane_info = bufDescriptor->plane_info;
				assert(plane_info);
				const bool is_multi_plane = hnd->is_multi_plane();
				for (int i = 0; i < MAX_PLANES && (i == 0 || plane_info[i].byte_stride != 0); i++)
				{
					init_afbc(cpu_ptr + plane_info[i].offset,
					          bufDescriptor->alloc_format,
					          is_multi_plane,
					          plane_info[i].alloc_width,
					          plane_info[i].alloc_height);
				}
			}

			{
				ATRACE_NAME("munmap");
				mali_gralloc_ion_sync_end(hnd, true, true);
				munmap(cpu_ptr, bufDescriptor->alloc_sizes[0]);
			}
		}
	}
#endif

	return 0;
}


int mali_gralloc_ion_map(private_handle_t *hnd)
{
	uint64_t usage = hnd->producer_usage | hnd->consumer_usage;

	/* Do not allow cpu access to secure buffers */
	if (usage & (GRALLOC_USAGE_PROTECTED | GRALLOC_USAGE_NOZEROED | GRALLOC_USAGE_SECURE_CAMERA_RESERVED)
			&& !(usage & GRALLOC_USAGE_PRIVATE_NONSECURE))
	{
		return 0;
	}

	/* Do not allow cpu access to HFR buffers */
	if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_HFR_MODE)
	{
		return 0;
	}

#if defined(GRALLOC_SCALER_WFD) && GRALLOC_SCALER_WFD == 1
	// WFD for nacho cannot be mapped
	{
		if (!(usage & GRALLOC_USAGE_PROTECTED) &&
			usage & GRALLOC_USAGE_PRIVATE_NONSECURE &&
			usage & GRALLOC_USAGE_HW_COMPOSER)
		{
			return 0;
		}
	}
#endif

	for (int fidx = 0; fidx < hnd->fd_count; fidx++) {
		unsigned char *mappedAddress =
			(unsigned char *)mmap(NULL, hnd->alloc_sizes[fidx], PROT_READ | PROT_WRITE,
					MAP_SHARED, hnd->fds[fidx], 0);

		if (MAP_FAILED == mappedAddress)
		{
			int err = errno;
			MALI_GRALLOC_LOGE("mmap( fds[%d]:%d size:%zu ) failed with %s",
					fidx, hnd->fds[fidx], hnd->alloc_sizes[fidx], strerror(err));
			hnd->dump("map fail");

			for (int cidx = 0; cidx < fidx; cidx++)
			{
				munmap((void*)hnd->bases[cidx], hnd->alloc_sizes[cidx]);
			}

			return -err;
		}

		hnd->bases[fidx] = uintptr_t(mappedAddress);
	}

	return 0;
}

void mali_gralloc_ion_unmap(private_handle_t *hnd)
{
	for (int i = 0; i < hnd->fd_count; i++)
	{
		int err = 0;
		plane_info_t *plane = &hnd->plane_info[i];

		if (hnd->bases[i])
		{
			err = munmap((void*)hnd->bases[i], hnd->alloc_sizes[i]);
		}

		if (err)
		{
			MALI_GRALLOC_LOGE("Could not munmap base:%p size:%zu '%s'",
					(void*)hnd->bases[i], hnd->alloc_sizes[i], strerror(errno));
		}
		else
		{
			hnd->bases[i] = 0;
		}
	}

	hnd->cpu_read = 0;
	hnd->cpu_write = 0;
}

