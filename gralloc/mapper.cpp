/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cutils/log.h>
#include <cutils/atomic.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include "gralloc_priv.h"
#include "exynos_format.h"

#include <ion/ion.h>
#include <linux/ion.h>

/*****************************************************************************/

static int gralloc_map(gralloc_module_t const* module, buffer_handle_t handle)
{
    size_t chroma_vstride = 0;
    size_t chroma_size = 0;
    size_t ext_size = 256;

    private_handle_t *hnd = (private_handle_t*)handle;

    switch (hnd->format) {
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_TILED:
        chroma_vstride = ALIGN(hnd->height / 2, 32);
        chroma_size = chroma_vstride * hnd->stride + ext_size;
        break;
    case HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M:
    case HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL:
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M:
        chroma_size = hnd->stride * ALIGN(hnd->vstride / 2, 8) + ext_size;
        break;
    case HAL_PIXEL_FORMAT_EXYNOS_YV12_M:
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P_M:
        chroma_size = (hnd->vstride / 2) * ALIGN(hnd->stride / 2, 16) + ext_size;
        break;
    default:
        ALOGV("%s: unknown format: 0x%x", __func__, hnd->format);
        break;
    }

    void* mappedAddress = mmap(0, hnd->size, PROT_READ|PROT_WRITE, MAP_SHARED,
                               hnd->fd, 0);
    if (mappedAddress == MAP_FAILED) {
        ALOGE("%s: could not mmap %s", __func__, strerror(errno));
        return -errno;
    }
    ALOGV("%s: base %p %d %d %d %d\n", __func__, mappedAddress, hnd->size,
          hnd->width, hnd->height, hnd->stride);
    hnd->base = mappedAddress;

    if (hnd->fd1 >= 0) {
        void *mappedAddress1 = (void*)mmap(0, chroma_size, PROT_READ|PROT_WRITE,
                                            MAP_SHARED, hnd->fd1, 0);
        hnd->base1 = mappedAddress1;
    }
    if (hnd->fd2 >= 0) {
        void *mappedAddress2 = (void*)mmap(0, chroma_size, PROT_READ|PROT_WRITE,
                                            MAP_SHARED, hnd->fd2, 0);
        hnd->base2 = mappedAddress2;
    }

    return 0;
}

static int gralloc_unmap(gralloc_module_t const* module, buffer_handle_t handle)
{
    private_handle_t* hnd = (private_handle_t*)handle;
    size_t chroma_vstride = 0;
    size_t chroma_size = 0;
    size_t ext_size = 256;

    switch (hnd->format) {
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_TILED:
        chroma_vstride = ALIGN(hnd->height / 2, 32);
        chroma_size = chroma_vstride * hnd->stride + ext_size;
        break;
    case HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M:
    case HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL:
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M:
        chroma_size = hnd->stride * ALIGN(hnd->vstride / 2, 8) + ext_size;
        break;
    case HAL_PIXEL_FORMAT_EXYNOS_YV12_M:
    case HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P_M:
        chroma_size = (hnd->vstride / 2) * ALIGN(hnd->stride / 2, 16) + ext_size;
        break;
    default:
        ALOGV("%s: unknown format: 0x%x", __func__, hnd->format);
        break;
    }

    if (!hnd->base)
        return 0;

    if (munmap(hnd->base, hnd->size) < 0) {
        ALOGE("%s :could not unmap %s %p %d", __func__, strerror(errno),
              hnd->base, hnd->size);
    }
    ALOGV("%s: base %p %d %d %d %d\n", __func__, hnd->base, hnd->size,
          hnd->width, hnd->height, hnd->stride);
    hnd->base = 0;
    if (hnd->fd1 >= 0) {
        if (!hnd->base1)
            return 0;
        if (munmap(hnd->base1, chroma_size) < 0) {
            ALOGE("%s :could not unmap %s %p %d", __func__, strerror(errno),
                  hnd->base1, chroma_size);
        }
        hnd->base1 = 0;
    }
    if (hnd->fd2 >= 0) {
        if (!hnd->base2)
            return 0;
        if (munmap(hnd->base2, chroma_size) < 0) {
            ALOGE("%s :could not unmap %s %p %d", __func__, strerror(errno),
                  hnd->base2, chroma_size);
        }
        hnd->base2 = 0;
    }
    return 0;
}

/*****************************************************************************/

int getIonFd(gralloc_module_t const *module)
{
    private_module_t* m = const_cast<private_module_t*>(reinterpret_cast<const private_module_t*>(module));
    if (m->ionfd == -1)
        m->ionfd = ion_open();
    return m->ionfd;
}

static pthread_mutex_t sMapLock = PTHREAD_MUTEX_INITIALIZER;

/*****************************************************************************/

int gralloc_register_buffer(gralloc_module_t const* module,
                            buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t* hnd = (private_handle_t*)handle;
    ALOGV("%s: base %p %d %d %d %d\n", __func__, hnd->base, hnd->size,
          hnd->width, hnd->height, hnd->stride);

    int ret;
    ret = ion_import(getIonFd(module), hnd->fd, &hnd->handle);
    if (ret)
        ALOGE("error importing handle %d %x\n", hnd->fd, hnd->format);
    if (hnd->fd1 >= 0) {
        ret = ion_import(getIonFd(module), hnd->fd1, &hnd->handle1);
        if (ret)
            ALOGE("error importing handle1 %d %x\n", hnd->fd1, hnd->format);
    }
    if (hnd->fd2 >= 0) {
        ret = ion_import(getIonFd(module), hnd->fd2, &hnd->handle2);
        if (ret)
            ALOGE("error importing handle2 %d %x\n", hnd->fd2, hnd->format);
    }

    return ret;
}

int gralloc_unregister_buffer(gralloc_module_t const* module,
                              buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t* hnd = (private_handle_t*)handle;
    ALOGV("%s: base %p %d %d %d %d\n", __func__, hnd->base, hnd->size,
          hnd->width, hnd->height, hnd->stride);

    gralloc_unmap(module, handle);

    if (hnd->handle)
        ion_free(getIonFd(module), hnd->handle);
    if (hnd->handle1)
        ion_free(getIonFd(module), hnd->handle1);
    if (hnd->handle2)
        ion_free(getIonFd(module), hnd->handle2);

    return 0;
}

int gralloc_lock(gralloc_module_t const* module,
                 buffer_handle_t handle, int usage,
                 int l, int t, int w, int h,
                 void** vaddr)
{
    // this is called when a buffer is being locked for software
    // access. in thin implementation we have nothing to do since
    // not synchronization with the h/w is needed.
    // typically this is used to wait for the h/w to finish with
    // this buffer if relevant. the data cache may need to be
    // flushed or invalidated depending on the usage bits and the
    // hardware.

    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t* hnd = (private_handle_t*)handle;

    if (hnd->format == HAL_PIXEL_FORMAT_YCbCr_420_888)
    {
        ALOGE("%s: Buffers with format YCbCr_420_888 must be locked using"
            " (*lock_ycbcr)\n", __func__);
        return -EINVAL;
    }

    if (!hnd->base)
        gralloc_map(module, hnd);
    *vaddr = (void*)hnd->base;

    if (hnd->fd1 >= 0)
        vaddr[1] = (void*)hnd->base1;
    if (hnd->fd2 >= 0)
        vaddr[2] = (void*)hnd->base2;

    return 0;
}

int gralloc_lock_ycbcr(gralloc_module_t const* module,
                              buffer_handle_t handle, int usage,
                              int l, int t, int w, int h,
                              struct android_ycbcr *ycbcr)
{
    private_handle_t* hnd;

    if (private_handle_t::validate(handle) < 0) {
        ALOGE("%s: Locking invalid buffer %p, returning error", __func__, handle);
        return -EINVAL;
    }

    if (!ycbcr) {
        ALOGE("%s: YCbCr data is empty, bailing", __func__);
        return -EINVAL;
    }

    hnd = (private_handle_t*)handle;

    /* Make sure all struct members are zeroed */
    memset(ycbcr, 0, sizeof(struct android_ycbcr));

    /* NOTE: width = stride */
    //if (usage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK | GRALLOC_USAGE_HW_CAMERA_WRITE)) {
    if (hnd->flags & (GRALLOC_USAGE_HW_CAMERA_WRITE | GRALLOC_USAGE_SW_READ_MASK)) {
        char* base = (char*)hnd->base;
        int y_stride = hnd->width;
        /* Ensure height is aligned for subsampled chroma before calculating buffer parameters */
        int adjusted_height = ALIGN(hnd->height, 2);
        //int y_size =  y_stride * adjusted_height;
        int y_size = y_stride * hnd->height;

        int u_offset = 0;
        int v_offset = 0;
        int c_stride = 0;
        int step = 0;

        switch (hnd->format) {
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
            c_stride = y_stride;
            /* Y plane, UV plane */
            u_offset = y_size;
            v_offset = y_size + 1;
            step = 2;
            break;

        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            c_stride = y_stride;
            /* Y plane, UV plane */
            v_offset = y_size;
            u_offset = y_size + 1;
            step = 2;
            break;

        case HAL_PIXEL_FORMAT_YV12:
        {
            int c_size;

            /* Stride alignment set to 16 as the SW access flags were set */
            c_stride = ALIGN(hnd->width / 2, 16);
            c_size = c_stride * (adjusted_height / 2);
            /* Y plane, V plane, U plane */
            v_offset = y_size;
            u_offset = y_size + c_size;
            step = 1;
            break;
        }

        default:
            ALOGE("%s: Can't lock buffer %p: wrong format %d", __func__, hnd, hnd->format);
            return -EINVAL;
        }

        ycbcr->y = (void*)base;
        ycbcr->cb = (void*)(base + u_offset);
        ycbcr->cr = (void*)(base + v_offset);
        ycbcr->ystride = y_stride;
        ycbcr->cstride = c_stride;
        ycbcr->chroma_step = step;
    } else {
        ALOGE("%s: Can't lock buffer %p: wrong flags 0x%x", __func__, hnd, hnd->flags);
        return -EINVAL;
    }

    return 0;
}

int gralloc_unlock(gralloc_module_t const* module,
                   buffer_handle_t handle)
{
    // we're done with a software buffer. nothing to do in this
    // implementation. typically this is used to flush the data cache.
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t* hnd = (private_handle_t*)handle;

    if (!((hnd->flags & GRALLOC_USAGE_SW_READ_MASK) == GRALLOC_USAGE_SW_READ_OFTEN))
        return 0;

    ion_sync_fd(getIonFd(module), hnd->fd);
    if (hnd->fd1 >= 0)
        ion_sync_fd(getIonFd(module), hnd->fd1);
    if (hnd->fd2 >= 0)
        ion_sync_fd(getIonFd(module), hnd->fd2);

    return 0;
}
