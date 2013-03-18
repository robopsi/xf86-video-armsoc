/*
 * Copyright © 2012 ARM Limited
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */
#include <stdlib.h>
#include <sys/mman.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <xf86.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "armsoc_dumb.h"
#include "drmmode_driver.h"

struct armsoc_device {
	int fd;
	uint32_t dumb_scanout_flags;
	uint32_t dumb_no_scanout_flags;
};

struct armsoc_bo {
	struct armsoc_device *dev;
	uint32_t handle;
	uint32_t size;
	void *map_addr;
	uint32_t fb_id;
	uint32_t width;
	uint32_t height;
	uint8_t depth;
	uint8_t bpp;
	uint32_t pitch;
	int refcnt;
	int dmabuf;
	/* initial size of backing memory. Used on resize to check if the new size will fit */
	uint32_t original_size;
};

/* device related functions:
 */

struct armsoc_device *armsoc_device_new(int fd, uint32_t dumb_scanout_flags, uint32_t dumb_no_scanout_flags)
{
	struct armsoc_device *new_dev = malloc(sizeof(*new_dev));
	if (!new_dev)
		return NULL;

	new_dev->fd = fd;
	new_dev->dumb_scanout_flags = dumb_scanout_flags;
	new_dev->dumb_no_scanout_flags = dumb_no_scanout_flags;
	return new_dev;
}

void armsoc_device_del(struct armsoc_device *dev)
{
	free(dev);
}

/* buffer-object related functions:
 */

int armsoc_bo_set_dmabuf(struct armsoc_bo *bo)
{
	int res;
	struct drm_prime_handle prime_handle;

	assert(bo->refcnt > 0);
	assert(!armsoc_bo_has_dmabuf(bo));

	/* Try to get dma_buf fd */
	prime_handle.handle = bo->handle;
	prime_handle.flags  = 0;
	res  = drmIoctl(bo->dev->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle);
	if (res)
	{
		res = errno;
	}
	else
	{
		bo->dmabuf = prime_handle.fd;
	}
	return res;
}

void armsoc_bo_clear_dmabuf(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	assert(armsoc_bo_has_dmabuf(bo));

	close(bo->dmabuf);
	bo->dmabuf = -1;
}

int armsoc_bo_has_dmabuf(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->dmabuf >= 0;
}

struct armsoc_bo *armsoc_bo_new_with_dim(struct armsoc_device *dev,
			uint32_t width, uint32_t height, uint8_t depth,
			uint8_t bpp, enum armsoc_buf_type buf_type)
{
	struct drm_mode_create_dumb create_dumb;
	struct armsoc_bo *new_buf;
	int res;

	new_buf = malloc(sizeof(*new_buf));
	if (!new_buf)
		return NULL;

	create_dumb.height = height;
	create_dumb.width = width;
	create_dumb.bpp = bpp;
	assert((ARMSOC_BO_SCANOUT == buf_type) || (ARMSOC_BO_NON_SCANOUT == buf_type));
	if (ARMSOC_BO_SCANOUT == buf_type)
	{
		create_dumb.flags = dev->dumb_scanout_flags;
	}
	else
	{
		create_dumb.flags = dev->dumb_no_scanout_flags;
	}

	res = drmIoctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
	if (res)
	{
		free(new_buf);
		xf86DrvMsg(-1, X_ERROR, "_CREATE_DUMB("
				"{height: 0x%X, width: 0x%X, bpp: 0x%X, flags: 0x%X}) "
				"failed. errno:0x%X\n",height,width,bpp,create_dumb.flags,errno);
		return NULL;
	}

	new_buf->dev = dev;
	new_buf->handle = create_dumb.handle;
	new_buf->size = create_dumb.size;
	new_buf->map_addr = NULL;
	new_buf->fb_id = 0;
	new_buf->pitch = create_dumb.pitch;
	new_buf->width = create_dumb.width;
	new_buf->height = create_dumb.height;
	new_buf->original_size = create_dumb.size;
	new_buf->depth = depth;
	new_buf->bpp = create_dumb.bpp;
	new_buf->refcnt = 1;
	new_buf->dmabuf = -1;

	return new_buf;
}

static void armsoc_bo_del(struct armsoc_bo *bo)
{
	int res;
	struct drm_mode_destroy_dumb destroy_dumb;

	if (!bo)
		return;
	assert(bo->refcnt == 0);
	assert(bo->dmabuf < 0);

	if (bo->map_addr)
	{
		/* always map/unmap the full buffer for consistency */
		munmap(bo->map_addr, bo->original_size);
	}

	if (bo->fb_id)
	{
		res = drmModeRmFB(bo->dev->fd, bo->fb_id);
		if(res) {
			xf86DrvMsg(-1, X_ERROR, "drmModeRmFb failed %d : %s\n", res, strerror(errno));
		}
	}
	destroy_dumb.handle = bo->handle;
	res = drmIoctl(bo->dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
	if(res) {
		xf86DrvMsg(-1, X_ERROR, "destroy dumb failed %d : %s\n", res, strerror(errno));
	}
	free(bo);
}

void armsoc_bo_unreference(struct armsoc_bo *bo)
{
	if (!bo)
		return;

	assert(bo->refcnt > 0);
	if (--bo->refcnt == 0)
		armsoc_bo_del(bo);
}

void armsoc_bo_reference(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	bo->refcnt++;
}

int armsoc_bo_get_name(struct armsoc_bo *bo, uint32_t *name)
{
	int ret;
	struct drm_gem_flink flink;

	assert(bo->refcnt > 0);
	flink.handle = bo->handle;

	ret = drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_FLINK, &flink);
	if (ret)
		return ret;

	*name = flink.name;
	return 0;
}

uint32_t armsoc_bo_handle(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->handle;
}

uint32_t armsoc_bo_size(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->size;
}

uint32_t armsoc_bo_width(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->width;
}

uint32_t armsoc_bo_height(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->height;
}

uint32_t armsoc_bo_bpp(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->bpp;
}

/* Bytes per pixel */
uint32_t armsoc_bo_Bpp(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	return (bo->bpp + 7) / 8;
}

uint32_t armsoc_bo_pitch(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->pitch;
}

void *armsoc_bo_map(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	if (!bo->map_addr)
	{
		struct drm_mode_map_dumb map_dumb;
		int res;

		map_dumb.handle = bo->handle;

		res = drmIoctl(bo->dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
		if (res)
			return NULL;

		/* always map/unmap the full buffer for consistency */
		bo->map_addr = mmap(NULL, bo->original_size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->dev->fd, map_dumb.offset);

		if (bo->map_addr == MAP_FAILED)
		{
			bo->map_addr = NULL;
		}
	}

	return bo->map_addr;
}

int armsoc_bo_cpu_prep(struct armsoc_bo *bo, enum armsoc_gem_op op)
{
	int ret = 0;

	assert(bo->refcnt > 0);
	if(armsoc_bo_has_dmabuf(bo))
	{
		fd_set fds;
		const struct timeval timeout = {10,0}; /* 10s before printing a msg */
		struct timeval t;
		FD_ZERO(&fds);
		FD_SET(bo->dmabuf, &fds);

		do
		{
			t = timeout;
			ret = select(bo->dmabuf+1, &fds, NULL, NULL, &t);
			if (ret == 0)
			{
				xf86DrvMsg(-1, X_ERROR, "select() on dma_buf fd has timed-out\n");
			}
		}while ( (ret == -1 && errno == EINTR) || ret == 0 );

		if (ret > 0)
			ret = 0;
	}
	return ret;
}

int armsoc_bo_cpu_fini(struct armsoc_bo *bo, enum armsoc_gem_op op)
{
	assert(bo->refcnt > 0);
	return msync(bo->map_addr, bo->size, MS_SYNC | MS_INVALIDATE);
}

int armsoc_bo_add_fb(struct armsoc_bo *bo)
{
	int ret;

	assert(bo->refcnt > 0);
	assert(bo->fb_id == 0);

	ret = drmModeAddFB(bo->dev->fd, bo->width, bo->height, bo->depth,
			bo->bpp, bo->pitch, bo->handle, &bo->fb_id);
	if (ret < 0) {
		xf86DrvMsg(-1, X_ERROR, "Could not add fb to bo %d\n", ret);
		bo->fb_id = 0;
		return ret;
	}
	return 0;
}

int armsoc_bo_rm_fb(struct armsoc_bo *bo)
{
	int ret;

	assert(bo->refcnt > 0);
	assert(bo->fb_id != 0);
	ret = drmModeRmFB( bo->dev->fd, bo->fb_id );
	if( ret < 0 ) {
		xf86DrvMsg(-1, X_ERROR, "Could not remove fb from bo %d\n", ret);
		return ret;
	}
	bo->fb_id = 0;
	return 0;
}

uint32_t armsoc_bo_get_fb(struct armsoc_bo *bo)
{
	assert(bo->refcnt > 0);
	return bo->fb_id;
}

int armsoc_bo_clear(struct armsoc_bo *bo)
{
	unsigned char *dst;

	assert(bo->refcnt > 0);
	if (!(dst = armsoc_bo_map(bo))) {
		xf86DrvMsg(-1, X_ERROR,
				"Couldn't map scanout bo\n");
		return -1;
	}
	if( armsoc_bo_cpu_prep(bo, ARMSOC_GEM_WRITE)) {
		xf86DrvMsg(-1, X_ERROR," %s: armsoc_bo_cpu_prep failed - "
					"unable to synchronise access.\n", __FUNCTION__);
		return -1;
	}
	memset(dst, 0x0, bo->size);
	(void)armsoc_bo_cpu_fini(bo, ARMSOC_GEM_WRITE);
	return 0;
}

int armsoc_bo_resize(struct armsoc_bo *bo, uint32_t new_width, uint32_t new_height)
{
	uint32_t new_size;
	uint32_t new_pitch;

	assert( bo != NULL );
	assert( new_width > 0 );
	assert( new_height > 0 );
	assert( bo->fb_id == 0 ); /* The caller must remove the fb object before attempting to resize. */
	assert(bo->refcnt > 0);

	xf86DrvMsg(-1, X_INFO, "Resizing bo from %dx%d to %dx%d\n",
		   bo->width, bo->height, new_width, new_height);

	/* TODO: MIDEGL-1563: Get pitch from DRM as only DRM knows the ideal pitch and alignment requirements */
	new_pitch  = new_width * armsoc_bo_Bpp(bo);
	/* Align pitch to 64 byte */
	new_pitch  = ((new_pitch + 63) & ~(63));
	new_size   = (((new_height-1) * new_pitch) + (new_width * armsoc_bo_Bpp(bo) ));

	if( new_size <= bo->original_size )
	{
	    bo->width  = new_width;
	    bo->height = new_height;
	    bo->pitch  = new_pitch;
	    bo->size   = new_size;
	    return 0;
	}
	xf86DrvMsg(-1, X_ERROR, "Failed to resize buffer\n");
	return -1;
}
