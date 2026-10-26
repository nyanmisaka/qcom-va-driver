// SPDX-License-Identifier: MIT
// Exercise the cache-mode contract without a DRM device.
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <xf86drm.h>
#include <msm_drm.h>

#ifndef MSM_BO_CACHED_COHERENT
#define MSM_BO_CACHED_COHERENT 0x080000
#endif

int irisva_msm_allocate(int fd, uint64_t size, int *coherent);
int irisva_msm_allocate_coherent(int fd, uint64_t size);
static int coherent_error, export_error, wc_requests, closed_handles;

int drmIoctl(int fd, unsigned long request, void *arg) {
    (void)fd;
    if (request == DRM_IOCTL_MSM_GEM_NEW) {
        struct drm_msm_gem_new *allocation = arg;
        assert(allocation->size == 4096);
        if (allocation->flags == MSM_BO_CACHED_COHERENT && coherent_error) {
            errno = coherent_error;
            return -1;
        }
        if (allocation->flags == MSM_BO_WC)
            ++wc_requests;
        else
            assert(allocation->flags == MSM_BO_CACHED_COHERENT);
        allocation->handle = 42;
    } else {
        assert(request == DRM_IOCTL_GEM_CLOSE);
        assert(((struct drm_gem_close *)arg)->handle == 42);
        ++closed_handles;
        errno = 0; // Closing a handle must not erase an export failure's errno.
    }
    return 0;
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) {
    (void)fd;
    assert(handle == 42);
    assert(flags == (DRM_CLOEXEC | DRM_RDWR));
    if (export_error) {
        errno = export_error;
        return -1;
    }
    *prime_fd = 123; // No real descriptors are created by these mocks.
    return 0;
}

int main(void) {
    assert(irisva_msm_allocate_coherent(0, 4096) == 123);
    assert(closed_handles == 1 && wc_requests == 0);
    const int failures[] = {EINVAL, EOPNOTSUPP, EACCES, ENOMEM};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        coherent_error = failures[i];
        assert(irisva_msm_allocate_coherent(0, 4096) == -1);
        assert(errno == coherent_error);
        assert(wc_requests == 0 && closed_handles == 1);
    }
    coherent_error = 0;
    export_error = EMFILE;
    assert(irisva_msm_allocate_coherent(0, 4096) == -1);
    assert(errno == EMFILE && closed_handles == 2 && wc_requests == 0);
    export_error = 0;
    int coherent = -1;
    coherent_error = EINVAL;
    assert(irisva_msm_allocate(0, 4096, &coherent) == 123);
    assert(!coherent && wc_requests == 1 && closed_handles == 3);
    coherent_error = EACCES;
    assert(irisva_msm_allocate(0, 4096, &coherent) == -1);
    assert(errno == EACCES && wc_requests == 1 && closed_handles == 3);
    coherent_error = 0;
    assert(irisva_msm_allocate(0, 4096, &coherent) == 123);
    assert(coherent && closed_handles == 4);
    return 0;
}
