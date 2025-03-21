/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cros_gralloc_buffer.h"

#include <assert.h>
#include <sys/mman.h>

cros_gralloc_buffer::cros_gralloc_buffer(uint32_t id, struct bo *acquirebo_,
					 struct cros_gralloc_handle *acquire_handle)
    : id_(id), bo_(acquirebo_), hnd_(acquire_handle), refcount_(1), lockcount_(0)
{
	assert(bo_);
	num_planes_ = drv_bo_get_num_planes(bo_);
	for (uint32_t plane = 0; plane < num_planes_; plane++)
		lock_data_[plane] = nullptr;
}

cros_gralloc_buffer::~cros_gralloc_buffer()
{
	drv_bo_destroy(bo_);
	if (hnd_) {
		native_handle_close(&hnd_->base);
		delete hnd_;
	}
}

uint32_t cros_gralloc_buffer::get_id() const
{
	return id_;
}

int32_t cros_gralloc_buffer::increase_refcount()
{
	return ++refcount_;
}

int32_t cros_gralloc_buffer::decrease_refcount()
{
	assert(refcount_ > 0);
	return --refcount_;
}

int32_t cros_gralloc_buffer::lock(uint64_t flags, uint8_t *addr[DRV_MAX_PLANES])
{
	/*
	 * Gralloc consumers don't support more than one kernel buffer per buffer object yet, so
	 * just use the first kernel buffer.
	 */
	if (drv_num_buffers_per_bo(bo_) != 1) {
		cros_gralloc_error("Can only support one buffer per bo.");
		return CROS_GRALLOC_ERROR_NO_RESOURCES;
	}

	if (flags) {
		void *vaddr;
		if (lock_data_[0]) {
			vaddr = lock_data_[0]->addr;
		} else {
			vaddr = drv_bo_map(bo_, 0, 0, drv_bo_get_width(bo_), drv_bo_get_height(bo_),
					   0, &lock_data_[0], 0);
		}

		if (vaddr == MAP_FAILED) {
			cros_gralloc_error("Mapping failed.");
			return CROS_GRALLOC_ERROR_UNSUPPORTED;
		}

		addr[0] = static_cast<uint8_t *>(vaddr);
	}

	for (uint32_t plane = 0; plane < num_planes_; plane++)
		addr[plane] = addr[0] + drv_bo_get_plane_offset(bo_, plane);

	lockcount_++;
	return CROS_GRALLOC_ERROR_NONE;
}

int32_t cros_gralloc_buffer::unlock()
{
	if (lockcount_ <= 0) {
		cros_gralloc_error("Buffer was not locked.");
		return CROS_GRALLOC_ERROR_UNSUPPORTED;
	}

	if (!--lockcount_) {
		if (lock_data_[0]) {
			drv_bo_unmap(bo_, lock_data_[0]);
			lock_data_[0] = nullptr;
		}
	}

	return CROS_GRALLOC_ERROR_NONE;
}
