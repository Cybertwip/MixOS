#pragma once

#include <stdint.h>

#ifndef GPU_IOCTL_GET_INFO
#define GPU_IOCTL_GET_INFO       0x40
#endif
#ifndef GPU_IOCTL_SUBMIT_NOOP
#define GPU_IOCTL_SUBMIT_NOOP    0x41
#endif
#ifndef GPU_IOCTL_COMPUTE_RGBA8_TO_RGB565
#define GPU_IOCTL_COMPUTE_RGBA8_TO_RGB565 0x42
#endif
#ifndef GPU_IOCTL_COMPUTE_RGBA8_COPY
#define GPU_IOCTL_COMPUTE_RGBA8_COPY 0x43
#endif
#ifndef GPU_IOCTL_BLIT_RGBA8_SCALED
#define GPU_IOCTL_BLIT_RGBA8_SCALED 0x44
#endif
#ifndef GPU_IOCTL_DRAW_TRIANGLES
#define GPU_IOCTL_DRAW_TRIANGLES 0x45
#endif

#ifndef GPU_ACCELERATOR_ABI_VERSION
#define GPU_ACCELERATOR_ABI_VERSION 1
#endif

#ifndef GPU_CAP_PRESENT
#define GPU_CAP_PRESENT          0x00000001u
#define GPU_CAP_NVIDIA           0x00000002u
#define GPU_CAP_BAR0_MAPPED      0x00000004u
#define GPU_CAP_BUS_MASTER       0x00000008u
#define GPU_CAP_COMPUTE_ABI      0x00000010u
#define GPU_CAP_SUBMIT_NOOP      0x00000020u
#define GPU_CAP_INTEL            0x00000040u
#define GPU_CAP_COMPUTE_RGBA8_TO_RGB565 0x00000080u
#define GPU_CAP_CPU_VECTOR_FALLBACK 0x00000100u
#define GPU_CAP_COMPUTE_RGBA8_COPY 0x00000200u
#define GPU_CAP_MEDIATEK         0x00000400u
#define GPU_CAP_ZERO_COPY_RGBA8  0x00000800u
#define GPU_CAP_SHADER_PIPELINE   0x00001000u
#define GPU_CAP_SCALED_BLIT       0x00002000u
#define GPU_CAP_TRIANGLE_LIST     0x00004000u
#define GPU_CAP_TRIANGLE_PRESERVE 0x00008000u
#endif
#ifndef GPU_CAP_COMPUTE_RGBA8_TO_RGB565
#define GPU_CAP_COMPUTE_RGBA8_TO_RGB565 0x00000080u
#endif
#ifndef GPU_CAP_CPU_VECTOR_FALLBACK
#define GPU_CAP_CPU_VECTOR_FALLBACK 0x00000100u
#endif
#ifndef GPU_CAP_COMPUTE_RGBA8_COPY
#define GPU_CAP_COMPUTE_RGBA8_COPY 0x00000200u
#endif
#ifndef GPU_CAP_MEDIATEK
#define GPU_CAP_MEDIATEK         0x00000400u
#endif
#ifndef GPU_CAP_ZERO_COPY_RGBA8
#define GPU_CAP_ZERO_COPY_RGBA8  0x00000800u
#endif
#ifndef GPU_CAP_SHADER_PIPELINE
#define GPU_CAP_SHADER_PIPELINE   0x00001000u
#endif
#ifndef GPU_CAP_SCALED_BLIT
#define GPU_CAP_SCALED_BLIT       0x00002000u
#endif
/* An indexed triangle list of pre-transformed vertices can be drawn on the
 * accelerator -- the seam Dumpling's rasteriser sits at. */
#ifndef GPU_CAP_TRIANGLE_LIST
#define GPU_CAP_TRIANGLE_LIST     0x00004000u
#endif
/* ...and pixels of the target rectangle that no triangle covers come back
 * unchanged. Without this bit the caller must be redrawing the whole rectangle,
 * because the tile buffer is initialised by a clear before the batch runs. */
#ifndef GPU_CAP_TRIANGLE_PRESERVE
#define GPU_CAP_TRIANGLE_PRESERVE 0x00008000u
#endif

#ifndef MVII_GPU_DEVICE_INFO_DEFINED
#define MVII_GPU_DEVICE_INFO_DEFINED
struct gpu_device_info {
    uint32_t abi_version;
    uint32_t present;
    uint32_t vendor;
    uint32_t device;
    uint32_t bdf;
    uint32_t class_code;
    uint32_t subclass;
    uint32_t prog_if;
    uint32_t revision;
    uint32_t bar0;
    uint32_t caps;
    uint32_t queue_ready;
    char driver_name[32];
    char status[96];
};
#endif

#ifndef MVII_GPU_RGBA8_TO_RGB565_JOB_DEFINED
#define MVII_GPU_RGBA8_TO_RGB565_JOB_DEFINED
struct gpu_rgba8_to_rgb565_job {
    const uint8_t* src_rgba8;
    uint16_t* dst_rgb565;
    uint32_t pixel_count;
    uint32_t flags;
};
#endif

#ifndef MVII_GPU_RGBA8_COPY_JOB_DEFINED
#define MVII_GPU_RGBA8_COPY_JOB_DEFINED
struct gpu_rgba8_copy_job {
    const uint8_t* src_rgba8;
    uint8_t* dst_rgba8;
    uint32_t pixel_count;
    uint32_t flags;
};
#endif

#ifndef MVII_GPU_RGBA8_BLIT_JOB_DEFINED
#define MVII_GPU_RGBA8_BLIT_JOB_DEFINED
struct gpu_rgba8_blit_job {
    const uint8_t* src_rgba8;
    uint8_t* dst_rgba8;
    uint32_t src_width;
    uint32_t src_height;
    uint32_t src_pitch_bytes;
    uint32_t dst_width;
    uint32_t dst_height;
    uint32_t dst_pitch_bytes;
    uint32_t flags;
};
#endif

#ifndef MVII_GPU_TRIANGLE_JOB_DEFINED
#define MVII_GPU_TRIANGLE_JOB_DEFINED
/* Pre-transformed screen-space vertex. Positions are destination pixels and
 * texture coordinates are texels rather than the normalised range, which is
 * what a software rasteriser's varyings already hold and what saves the driver
 * a normalise/denormalise round trip. */
struct gpu_triangle_vertex {
    float x;
    float y;
    float u;
    float v;
};

/* Uncovered pixels of the destination rectangle may be lost. Set this when the
 * caller is redrawing the whole rectangle anyway; leave it clear to require
 * that they survive, which a backend without GPU_CAP_TRIANGLE_PRESERVE will
 * refuse rather than silently break. */
#define GPU_TRIANGLE_FLAG_OVERWRITE_TARGET 0x00000001u

struct gpu_triangle_job {
    const struct gpu_triangle_vertex* vertices;
    uint32_t                          vertex_count;
    const uint16_t*                   indices;
    uint32_t                          index_count;
    const uint8_t*                    texture_rgba8;
    uint32_t                          texture_width;
    uint32_t                          texture_height;
    uint32_t                          texture_pitch_bytes;
    void*                             dst;
    uint32_t                          dst_width;
    uint32_t                          dst_height;
    uint32_t                          dst_pitch_bytes;
    /* Zero selects RGB565 and three RGBA8, matching the writeback pixel-format
     * codes the rest of this API already passes around. */
    uint32_t dst_format;
    /* Scissor rectangle in destination pixels, half-open. */
    uint32_t scissor_min_x;
    uint32_t scissor_min_y;
    uint32_t scissor_max_x;
    uint32_t scissor_max_y;
    uint32_t flags;
};
#endif

#ifdef __cplusplus
extern "C" {
#endif

int mvii_gpu_offload_get_info(struct gpu_device_info* out);
int mvii_gpu_offload_submit_noop(void);
int mvii_gpu_offload_rgba8_to_rgb565(const uint8_t* src_rgba8, uint16_t* dst_rgb565, uint32_t pixel_count);
int mvii_gpu_offload_rgba8_copy(const uint8_t* src_rgba8, uint8_t* dst_rgba8, uint32_t pixel_count);
int mvii_gpu_offload_rgba8_to_rgb565_2d(const uint8_t* src_rgba8,
                                        uint16_t* dst_rgb565,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t src_pitch_bytes,
                                        uint32_t dst_pitch_bytes);
int mvii_gpu_offload_rgba8_copy_2d(const uint8_t* src_rgba8,
                                   uint8_t* dst_rgba8,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t src_pitch_bytes,
                                   uint32_t dst_pitch_bytes);
/* Nearest-neighbour rescale and RGBA8 -> RGB565 conversion in one pass, for the
 * compositor's scaled window blit. Both pointers must be 64-byte aligned, the
 * destination pitch a multiple of eight bytes, and the two ranges disjoint.
 * Offered only when GPU_CAP_SCALED_BLIT is set; returns -1 otherwise so the
 * caller keeps its CPU fallback. */
int mvii_gpu_offload_rgba8_to_rgb565_scaled(const uint8_t* src_rgba8,
                                            uint32_t src_width,
                                            uint32_t src_height,
                                            uint32_t src_pitch_bytes,
                                            uint16_t* dst_rgb565,
                                            uint32_t dst_width,
                                            uint32_t dst_height,
                                            uint32_t dst_pitch_bytes);
/* The same rescale keeping RGBA8, which is what a guest software rasteriser
 * needs: its render target is RGBA8 and the destination is usually a
 * sub-rectangle of it. Same contract as above -- alignment is the driver's to
 * enforce, and -1 means "run your own loop", not "the hardware is broken". */
int mvii_gpu_offload_rgba8_blit_scaled(const uint8_t* src_rgba8,
                                       uint32_t src_width,
                                       uint32_t src_height,
                                       uint32_t src_pitch_bytes,
                                       uint8_t* dst_rgba8,
                                       uint32_t dst_width,
                                       uint32_t dst_height,
                                       uint32_t dst_pitch_bytes);
/* Draw an indexed triangle list of pre-transformed, textured vertices. This is
 * the seam a software rasteriser hands its primitives over at, so the whole
 * vertex stage above it -- shaders, clipping, the vertex cache -- is unchanged
 * and stays on the CPU where it can run arbitrary code.
 *
 * Offered only when GPU_CAP_TRIANGLE_LIST is set, and only for jobs the caps
 * actually cover; returns -1 otherwise so the caller keeps rasterising in
 * software, which remains the definition of correct output. */
int mvii_gpu_offload_draw_triangles(const struct gpu_triangle_job* job);

#ifdef __cplusplus
}
#endif
