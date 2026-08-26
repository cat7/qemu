/*
 * ATI R300/R350 3D draw capture -- the on-disk record format.
 *
 * A capture is taken at the point where a draw is already a fully
 * resolved, device-state-free description: the R300DrawState the setup
 * produced and the R300Vtx array whose positions have been through the
 * vertex program and the viewport. Everything the rasterizer reads is
 * either in that pair or in the few registers the point-sprite path
 * consults, so a record replayed on a host reaches exactly the pixels
 * the device reached -- without redoing GART translation, VC_SWAP, the
 * AOS layout, the TX_FORMAT1 component select or GUI_HOST_SWAP_CNTL,
 * every one of which this model has had wrong at least once.
 *
 * Each record carries its own inputs AND the bytes the software
 * rasterizer left behind, so it is a self-contained test case: the
 * destination rectangle before the draw, the same rectangle after it,
 * the texture the draw sampled, and the aperture swapper's byte-lane
 * xor over each. `doc/radeon9800/gl-replay/` consumes it.
 *
 * Records are written in host byte order and host struct layout, and
 * the file header repeats every size the reader has to agree on: this
 * is a diagnostic between one build of this device and one build of the
 * harness beside it, not an interchange format.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ATI_R350_CAP_H
#define ATI_R350_CAP_H

#define R350_CAP_MAGIC      "R350CAP2"
#define R350_CAP_REC_MAGIC  0x52334452u         /* "R3DR" */

/* how many recently written textures are remembered for deduplication */
#define R350_CAP_TEX_CACHE  64

typedef struct R350CapFileHdr {
    char magic[8];
    uint32_t hdr_bytes;         /* sizeof(R350CapFileHdr) */
    uint32_t rec_hdr_bytes;     /* sizeof(R350CapRecHdr) */
    uint32_t state_bytes;       /* sizeof(R300DrawState) */
    uint32_t vtx_bytes;         /* sizeof(R300Vtx) */
    uint32_t vram_bytes;        /* ATI_R350_VRAM_SIZE */
    uint32_t us_bytes;          /* sizeof(R300UsProgram) */
} R350CapFileHdr;

/*
 * Payload, in this order, immediately after the record header:
 *
 *   R300DrawState  state    (vram pointer, vertex program, fs pointer cleared)
 *   R300UsProgram  fs            the FRAGMENT program this draw ran
 *   R300Vtx        vb[nvtx]
 *   uint8_t        tex[tex_bytes]        raw VRAM bytes, swapper NOT applied
 *   uint8_t        before[rect_bytes]    raw VRAM bytes of the destination
 *   uint8_t        after[rect_bytes]     the same bytes after the draw
 *
 * The fragment program is carried by VALUE because the state holds only
 * a pointer to it: from milestone M5 the shading a draw performs is the
 * guest's own program, so a record that did not carry it would not be a
 * self-contained test case at all.
 *
 * `rect_bytes` is (x1 - x0) * 4 * (y1 - y0): the rows are packed, not
 * at the destination pitch. A record whose texture repeats one already
 * written sets tex_bytes to 0 and names the earlier record in tex_ref.
 */
typedef struct R350CapRecHdr {
    uint32_t magic;
    uint32_t index;             /* draw ordinal within the capture */
    uint32_t prim;              /* VAP_VF_CNTL primitive type */
    uint32_t nvtx;
    int32_t x0, y0, x1, y1;     /* destination rect, top-left inclusive */
    uint32_t rect_bytes;
    uint32_t dst_xor;           /* aperture swapper xor over the rect */
    uint32_t tex_xor;           /* ... and over the texture */
    uint32_t tex_vram_off;      /* where the texture resolved to in VRAM */
    uint32_t tex_bytes;
    uint32_t tex_ref;           /* record whose texture this one repeats */
    uint32_t txfmt1;            /* TX_FORMAT1_0, for family bookkeeping */
    uint32_t flags;
    /* the registers the point-sprite path reads at raster time */
    uint32_t pointsize;
    uint32_t point_s0, point_s1, point_t0, point_t1;
    uint32_t tx_enable;
} R350CapRecHdr;

/*
 * The two things the state cannot carry once the vertex program is
 * dropped: whether one ran for this draw and whether it was the plain
 * 4x4 matrix. A family filter needs both -- "plain-matrix program" is
 * half the definition of the compositor's blit quad.
 */
#define R350_CAP_F_TEXDEDUP (1u << 0)   /* texture is in record tex_ref */
#define R350_CAP_F_VS_RUN   (1u << 1)   /* a vertex program was executed */
#define R350_CAP_F_PLAIN_MAT (1u << 2)  /* ... and it was the plain matrix */

#endif /* ATI_R350_CAP_H */
