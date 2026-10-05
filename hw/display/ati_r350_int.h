/*
 * QEMU ATI Radeon 9800 Pro (R350) emulation
 *
 * Models the ATI Radeon 9800 Pro Mac Edition (PCI vendor 0x1002, device
 * 0x4E48) as an AGP add-in graphics card for the mac99 (PowerMac3,4)
 * machine. Forked from our ati_rage128 device: the Radeon keeps the Rage
 * 128 register lineage for the display controller (CRTC/DAC/palette/
 * cursor/I2C/GPIO), the 2D GUI engine (0x14xx), the scratch registers
 * and the PM4 command-stream format, so all of that is inherited; what
 * changes is the identity, the aperture layout (128MB frame buffer
 * BAR, 64KB register BAR), the memory-controller address windows
 * (MC_FB_LOCATION / MC_AGP_LOCATION -- engine and CP addresses are
 * card addresses, not frame-buffer offsets), the CP ring (CP_RB_*,
 * read-pointer and scratch write-back), RBBM_STATUS, the SURFACE_CNTL
 * byte swappers that replace CONFIG_CNTL's aperture endian modes, and
 * the R300 3D engine (not modeled: packets are parsed and skipped).
 *
 * Ground truth: the retail 9800 Pro Mac SE / OEM 9800 XT FCode ROMs
 * (detokenized 2026-08-24), Mac OS X 10.4.11's ATIRadeon9700.kext, the
 * Linux radeon DRM and X.org radeon drivers, and AMD's public R3xx
 * register references.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef ATI_R350_INT_H
#define ATI_R350_INT_H

#include "hw/pci/pci_device.h"
#include "hw/display/edid.h"
#include "qemu/bitmap.h"
#include "hw/i2c/bitbang_i2c.h"
#include "qemu/timer.h"
#include "qemu/thread.h"
#include "qapi/qapi-types-common.h"
#include "qom/object.h"
#include "ati_r350_pvs.h"
#include "ati_r350_us.h"
#include "ati_r350_cap.h"
#include "ati_r350_gl.h"

#define PCI_VENDOR_ID_ATI              0x1002
/*
 * 0x4E48 = R350 "Radeon 9800 Pro". The PCIR header of every Mac Edition
 * 9800 ROM we have (retail 9800 Pro SE 1.03/1.26 and Apple's OEM 9800
 * XT 1.18/1.23) declares this ID -- even the XT, whose silicon is an
 * R360 (0x4E4A): Mac OS X's ATIRadeon9700.kext matches 0x4E48 but not
 * 0x4E4A, so Apple's cards are strapped to the Pro's identity. Open
 * Firmware binds the ROM's FCode only on an exact vendor:device match.
 */
#define PCI_DEVICE_ID_ATI_R350         0x4e48

#define TYPE_ATI_R350 "ati-radeon9800"
OBJECT_DECLARE_SIMPLE_TYPE(ATIR350State, ATI_R350)

/*
 * Real Rage 128 BAR layout (confirmed by both the FCode's own mapping
 * words and real-hardware lspci dumps of Rage 128 cards):
 *   BAR0: 64MB memory, framebuffer aperture (prefetchable on real hw)
 *   BAR1: 256 bytes I/O, register access
 *   BAR2: 16KB memory, register aperture
 *   Expansion ROM: 128KB
 */
#define ATI_R350_APER_SIZE   (128 * 1024 * 1024)
/*
 * 128MB: the retail 9800 Pro Mac Edition's memory (the "SE" and the
 * OEM XT carry 256MB). The whole BAR0 aperture is the frame buffer --
 * unlike the Rage 128 there is no second aperture image; the Radeon's
 * byte swappers are SURFACE_CNTL's non-surface/surface swap bits.
 * Quartz Extreme needs >= 16MB, Quartz 2D Extreme >= 64MB.
 */
#define ATI_R350_VRAM_SIZE   (128 * 1024 * 1024)
#define ATI_R350_MMIO_SIZE   (64 * 1024)
#define ATI_R350_IO_SIZE     256
#define ATI_R350_NUM_REGS    (ATI_R350_MMIO_SIZE / 4)
#define ATI_R350_NUM_PLLS    64
#define ATI_R350_FB_SCAN_BLOCK (64 * 1024)

/*
 * Kinds of "the hardware does this, we do not" gap the engines report
 * through ati_r350_note_gap(). Each kind indexes its counters by the
 * field value itself (an opcode, a format code), so the tally names
 * exactly what a guest asked for and never got.
 */
typedef enum ATIR350GapKind {
    R350_GAP_P3_OPCODE,      /* packet3 opcode the parser discards */
    R350_GAP_PRIM,           /* VAP_VF_CNTL primitive type */
    R350_GAP_VTX_WALK,       /* VAP_VF_CNTL vertex walk mode */
    R350_GAP_TEX_FORMAT,     /* TX_FORMAT1 texel format code */
    R350_GAP_BLEND_FACTOR,   /* RB3D_BLENDCNTL src/dst factor code */
    R350_GAP_VTX_PROGRAM,    /* draw needs a vertex program we don't run */
    R350_GAP_DEST_OFF_VRAM,  /* work discarded: destination outside VRAM */
    R350_GAP_VS_VECTOR_OP,   /* PVS vector-engine opcode not implemented */
    R350_GAP_VS_MATH_OP,     /* PVS math-engine opcode not implemented */
    R350_GAP_VS_DST_FILE,    /* PVS destination register file not modelled */
    R350_GAP_TEX_SWIZZLE,    /* TX_FORMAT1 component select not modelled */
    R350_GAP_AOS_ARRAYS,     /* more vertex arrays bound than we fetch */
    /*
     * VAP_PROG_STREAM_CNTL element format not unpacked. Indexed by the
     * DATA_TYPE code itself for a type this model cannot fetch, and by
     * 0x10 | code for a VAP_PROG_STREAM_CNTL_EXT swizzle select it does
     * not know. Either way the element falls back to the raw-float read
     * that predates the format decoder, so the tally is the only place
     * the guest's request is visible.
     */
    R350_GAP_VTX_DATA_TYPE,
    R350_GAP_FS_RGB_OP,      /* US colour-side opcode not implemented */
    R350_GAP_FS_ALPHA_OP,    /* US alpha-side opcode not implemented */
    R350_GAP_FS_TEX_OP,      /* US texture instruction not implemented */
    R350_GAP_FS_INDIRECT,    /* US_CONFIG names an indirection level */
    R350_GAP_FS_RS_ROUTE,    /* rasterizer routes something we do not emit */
    R350_GAP_FS_OUT_FMT,     /* US_OUT_FMT_0 pixel format not modelled */
    R350_GAP_ZB_FORMAT,      /* ZB_FORMAT depth format not modelled */
    R350_GAP_CB_FORMAT,      /* COLORFORMAT the rasteriser cannot store */
    R350_GAP_VS_ADDR_MODE,   /* PVS operand addressing mode not modelled */
    R350_GAP_UCP,            /* VAP_CLIP_CNTL user clip planes enabled */
    R350_GAP_ZPASS,          /* occlusion counter use not modelled */
    R350_GAP_MAX
} ATIR350GapKind;

#define R350_GAP_SLOTS 256

/* dumps of the occlusion counter waiting for the GPU's count */
#define R350_ZQ_PEND 16

/*
 * How the "gl" property is set: whether the host-GPU backend renders a
 * draw, and whether the software rasterizer runs alongside it as a
 * check. `off` is the default and is a provable no-op -- the draw path
 * tests one NULL pointer and calls exactly what it called before.
 */
typedef enum ATIR350GlMode {
    R350_GL_OFF = 0,
    R350_GL_ON,             /* GL renders what it can; the rest falls back */
    R350_GL_VERIFY          /* both run, they are diffed, software wins */
} ATIR350GlMode;

/*
 * Why a draw the GL backend was offered went to the software rasterizer
 * instead. Counted per primitive type, the same shape as the gap
 * tracker, so `qom-get <device> gl` says how much of a session the
 * offload actually covered rather than leaving it to be inferred.
 */
typedef enum ATIR350GlFallback {
    R350_GLF_PRIM,          /* point sprites, lines: not assembled here */
    R350_GLF_RESOLVE,       /* AA resolve: the colour buffer is the source */
    R350_GLF_RECT,          /* no drawable destination rectangle */
    R350_GLF_ALIGN,         /* destination offset or pitch not dword-aligned */
    R350_GLF_VRAMEND,       /* rectangle runs off the end of VRAM */
    R350_GLF_XOR,           /* aperture swapper not uniform over the rect */
    R350_GLF_CLIPRULE,      /* a genuine 4-rect cliprect truth table */
    R350_GLF_TEXTURE,       /* texture too large to decode per draw */
    R350_GLF_SELFBLEND,     /* self-overlapping blend needing too many passes */
    R350_GLF_SURFACE,       /* target too large, or it would not resize */
    R350_GLF_BACKEND,       /* the backend itself declined the request */
    R350_GLF_FSPROG,        /* fragment program the translator refused */
    R350_GLF_CBFMT,         /* 16bpp or GART colour buffer */
    R350_GLF_ZTEST,         /* depth or stencil test */
    R350_GLF_ZPASS,         /* counted by an occlusion query, gl-zpass=off */
    R350_GLF_PROGWAIT,      /* the program worker still has the program */
    R350_GLF_MAX
} ATIR350GlFallback;

/*
 * Which coherency hook gave the resident render target back. The rules
 * are stated at "GL-OWNED RENDER TARGET" in ati_r350_3d.c; this counts
 * how often each of them actually fires and how many pixels it moved,
 * because "the target is not staying resident" is a symptom whose cure
 * depends entirely on WHICH rule is ending it. Read the tally from
 * `gl-stats`.
 */
typedef enum ATIR350GlRel {
    R350_GLR_SCANOUT,       /* the display refresh, and the cursor's timer */
    R350_GLR_RING,          /* the end of a command-processor ring run */
    R350_GLR_IB,            /* the end of an indirect buffer */
    R350_GLR_FIFO,          /* a CP FIFO push through the register aperture */
    R350_GLR_READ,          /* a named-range reader, ati_r350_gl_sync() */
    R350_GLR_2D,            /* the 2D scaler, and host-data pushes */
    R350_GLR_TARGET,        /* a draw bound a different colour buffer */
    R350_GLR_FALLBACK,      /* a draw the offload handed back */
    R350_GLR_BACKEND,       /* the backend declined, mid-draw */
    R350_GLR_RESET,         /* reset, unrealize */
    R350_GLR_FENCE,         /* a scratch or read pointer write, threaded CP */
    R350_GLR_MAX
} ATIR350GlRel;

/* why the GPU's copy of the depth buffer was given up */
typedef enum ATIR350GlZDrop {
    R350_GLZD_PARK,         /* could not be kept across a release */
    R350_GLZD_STALE,        /* kept, then VRAM under it changed */
    R350_GLZD_OTHER,        /* another depth buffer, or drawn into */
    R350_GLZD_SOFT,         /* the software path wrote Z */
    R350_GLZD_WRITER,       /* 2D engine, CP or MM_DATA wrote its range */
    R350_GLZD_GUARD,        /* the guest CPU touched it */
    R350_GLZD_RESET,        /* reset, backend failure, target grown */
    R350_GLZD_VERIFY,       /* gl=verify */
    R350_GLZD_MAX
} ATIR350GlZDrop;

/*
 * WHICH 2D path ended the residency. R350_GLR_2D above lumps three
 * unrelated operations together, and they call for opposite fixes: a
 * blit's ranges usually miss the render target entirely, a host-data
 * push never can (its destination is an 8bpp glyph atlas), and the
 * scaler is a video path 10.5 does not use at all. The cause enum
 * itself cannot carry the split -- the offline gl-replay harness in
 * doc/radeon9800 compiles ati_r350_3d.c against its own copy of this
 * header -- so the three paths keep their own tally beside it, filled
 * by ati_r350_2d_gl_note() and reported by `gl-stats`.
 *
 * A path is credited whatever cause the release was booked under, so
 * the tally stays honest when a range-gated path releases through
 * ati_r350_gl_sync() as R350_GLR_READ rather than as R350_GLR_2D.
 */
typedef enum ATIR350Gl2dPath {
    R350_GL2D_BLIT,         /* ati_r350_2d_blt(), via its named ranges */
    R350_GL2D_HOST,         /* ati_r350_host_data_flush(), CPU-pushed */
    R350_GL2D_SCALE,        /* ati_r350_2d_scale_run() */
    R350_GL2D_MAX
} ATIR350Gl2dPath;

/*
 * How long a DECODED TEXTURE may live. The default states the rule the
 * decode actually depends on -- see r300_gl_tex_current() -- and the
 * other two exist to measure it:
 *
 *   burst   the M3 lifetime: every release drops the whole cache. Safe,
 *           and on Flurry it left a 0.6% hit rate. Kept as the A/B arm.
 *   dirty   the default: an entry lives while the VRAM it was decoded
 *           from is unwritten, which is what correctness requires.
 *   never   deliberately WRONG -- entries are never invalidated at all.
 *           It exists so the guard can be shown to bite: a gl=verify
 *           run in this mode must FAIL. Never a shipping configuration.
 */
typedef enum ATIR350GlTexLife {
    R350_TEXLIFE_DIRTY,
    R350_TEXLIFE_BURST,
    R350_TEXLIFE_NEVER,
} ATIR350GlTexLife;

/*
 * A blended draw whose own primitives overlap is rendered in several
 * ordered passes rather than handed back to the software rasterizer
 * (milestone M3; see r300_gl_passes()). These bound the partition: a
 * draw needing more triangles or more passes than this falls back and
 * is counted, so the limits are visible in `gl-stats` rather than
 * silently shaping what the offload covers.
 */
#define R300_GL_TRI_MAX     512
#define R300_GL_PASS_MAX    128

/*
 * The decoded-texture cache: at most R300_GL_TEXCACHE entries (the
 * gl-texcache-slots property picks how many are used), each at most
 * R300_GL_TEXCACHE_MAX texels per face. With more than the classic 32
 * slots, lookups go through a hash on the decode key and the decoded
 * bytes are bounded by R300_GL_TEXCACHE_BYTES, least recently used
 * first, so a guest cannot make the device allocate without limit.
 */
#define R300_GL_TEXCACHE       R350_GL_TEXSLOTS
#define R300_GL_TEXCACHE_OLD   32
#define R300_GL_TEXCACHE_MAX   (256 * 1024)
#define R300_GL_TEXCACHE_BYTES (128u << 20)
#define R300_GL_TEXHASH        512
#define R300_GL_TEXNIL         0xffff

/*
 * The guard on a cached entry (see r300_gl_tex_current()) walks the
 * host pages its VRAM range covers. A texture spanning more pages than
 * this is decoded into the scratch buffer and not cached, so that the
 * walk stays a bounded cost per draw -- 4 MB at a 4 KB page, where the
 * cache's own texel limit is a quarter of that.
 */
#define R300_GL_DIRTY_PAGES  1024

typedef struct ATIR350PM4Parser {
    uint32_t remaining;      /* data dwords still expected */
    uint32_t type;           /* packet type of the in-flight packet */
    uint32_t reg;            /* running register offset, packet0 */
    bool one_reg;
    uint32_t p1_reg1;        /* packet1's two register offsets */
    uint32_t p1_reg2;
    uint32_t p3_opcode;      /* packet3 2D-draw sub-state */
    uint32_t p3_params[8];
    uint32_t p3_scale[16];      /* R350_SCALE_PKT_DWORDS */
    uint32_t p3_param_idx;
    uint32_t p3_total;       /* payload dwords the packet3 declared */
} ATIR350PM4Parser;

/*
 * Memo for ati_r350_vram_xor(). Resolving the swapper means walking
 * eight surface descriptors, three registers each, and the software
 * rasterizer asks two or three times for every pixel it touches -- which
 * is why the surface walk sat second and third in a profile of a stalled
 * guest. `lo`..`hi` is the offset range over which the walk provably
 * cannot give a different answer than `val`, so a hit costs two
 * comparisons. A memo belongs to one thread.
 */
typedef struct ATIR350SwapMemo {
    uint32_t lo, hi;
    uint32_t gen;               /* the device's swap_gen it was resolved at */
    unsigned val;
    bool valid;
} ATIR350SwapMemo;

typedef struct ATIR350Mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint32_t bpp;        /* bytes per pixel */
    uint32_t fb_offset;  /* byte offset into VRAM */
    uint32_t pix_width;  /* raw CRTC_PIX_WIDTH field, for draw dispatch */
} ATIR350Mode;

/*
 * One frame for the display worker to convert, filled on the main loop
 * while the worker is idle; see "ASYNC DISPLAY" in ati_r350.c.
 */
typedef struct ATIR350DispJob {
    ATIR350Mode mode;
    struct DisplaySurface *ds;
    const uint8_t *vram;
    unsigned xr;
    const uint8_t (*pal)[3];    /* palette, or the device's own */
    uint8_t palette[256][3];
} ATIR350DispJob;

enum {
    ATI_R350_DISP_IDLE,         /* job is the main loop's */
    ATI_R350_DISP_QUEUED,       /* job is the worker's from here ... */
    ATI_R350_DISP_RUNNING,
    ATI_R350_DISP_DONE,         /* ... until the hand-over takes it back */
};

struct ATIR350State {
    PCIDevice parent_obj;

    MemoryRegion aper;        /* BAR0: 64MB aperture container */
    MemoryRegion vram;        /* 16MB of real VRAM at aperture offset 0 */
    MemoryRegion mmio;        /* BAR2: 16KB register file */
    MemoryRegion io;          /* BAR1: 256-byte I/O register window */
    AddressSpace agp_as;      /* AGP transactions, via the host's GART */
    bool agp_as_valid;
    QemuConsole *con;

    uint32_t regs[ATI_R350_NUM_REGS];
    uint32_t plls[ATI_R350_NUM_PLLS];
    /*
     * Memos for ati_r350_vram_xor(): the display's, and the command
     * processor thread's. Raster workers keep their own. Any write to a
     * surface register bumps `swap_gen`, which stales all of them.
     */
    ATIR350SwapMemo swap;
    ATIR350SwapMemo eswap;
    uint32_t swap_gen;
    /*
     * Command processor thread. A CPU write of CP_RB_WPTR records the
     * pointer and wakes it; see "COMMAND PROCESSOR THREAD" in ati_r350.c.
     */
    OnOffAuto engine_async;
    bool engine_on;
    QemuThread engine_thread;
    QemuMutex engine_lock;
    QemuCond engine_cond;
    QemuEvent engine_idle;
    VMChangeStateEntry *engine_vmse;
    bool engine_busy;           /* atomic */
    bool engine_kick, engine_quit;
    uint64_t engine_kicks, engine_waits, engine_wait_us, engine_bql_writes;
    uint64_t engine_ibs, engine_scratch;
    unsigned engine_rptr_wb;
    /*
     * Register accesses without the BQL; see "LOCKLESS REGISTER ACCESS"
     * in ati_r350.c. engine_claimed and the claim counters are under
     * engine_lock.
     */
    bool lockless_mmio;         /* property */
    bool lockless;              /* in effect: needs the engine thread */
    bool engine_claimed;
    QemuCond engine_claim_cond;
    ATIR350SwapMemo cswap;      /* the claimant's ati_r350_vram_xor() memo */
    uint64_t engine_claims, engine_claim_waits, engine_claim_wait_us;
    uint64_t engine_claim_busy;
    /* the decoded-texture cache, held by the draw path */
    QemuRecMutex gl_tex_lock;
    /*
     * What the display's refresh hands the draw path: the pages its
     * dirty-bitmap clear found written, not yet applied to the texture
     * cache, and the framebuffer range it shows (offset << 32 | length).
     * scan_lock covers only the hand-over, so the refresh never waits
     * for a draw; scan_seq is odd while a clear is under way.
     */
    QemuMutex scan_lock;
    unsigned long *scan_dirty;
    unsigned long *scan_work;
    bool scan_pending;          /* atomic */
    unsigned scan_seq;          /* atomic */
    uint64_t scan_fb;           /* atomic */
    /*
     * Display worker; see "ASYNC DISPLAY" in ati_r350.c. disp_state and
     * disp_quit are under disp_lock; disp_job belongs to whoever
     * disp_state says; disp_tick_missed and the counters to the BQL.
     */
    OnOffAuto async_display;    /* property */
    bool disp_on;
    QemuThread disp_thread;
    QemuMutex disp_lock;
    QemuCond disp_cond;
    QEMUBH *disp_bh;
    unsigned disp_state;
    bool disp_quit;
    bool disp_tick_missed;
    ATIR350DispJob disp_job;
    uint64_t disp_frames, disp_deferred;
    /* R300 memory-controller indirect register file (MC_IND_INDEX/DATA) */
    uint32_t mc_ind[256];
    /*
     * Bit-banged DDC on the Radeon's dedicated GPIO_VGA_DDC / GPIO_DVI_DDC
     * pads (same A/Y/EN lane layout as GPIO_MONID), each serving the EDID.
     */
    bitbang_i2c_interface vga_ddc_i2c;
    bitbang_i2c_interface dvi_ddc_i2c;
    int vga_ddc_sda;
    int dvi_ddc_sda;

    /* DAC palette state (PALETTE_INDEX/PALETTE_DATA) */
    uint8_t dac_wr_index;
    uint8_t dac_rd_index;
    uint8_t palette[256][3];

    ATIR350Mode mode;      /* what the last refresh actually drew */
    /*
     * The last mode CRTC1 itself described while valid. Kept apart from
     * `mode`: when the auto-detected framebuffer overrides the CRTC, `mode`
     * holds the guess, and using it as the "remembered CRTC mode" fallback
     * made the override compare the guess against itself and stick for
     * good (seen live: OS X blanked the display for sleep, the heuristic
     * swapped in a stale 8bpp 800x600 buffer, and it never let go).
     */
    ATIR350Mode crtc_mode;
    bool mode_dirty;
    bool have_valid_mode; /* has `crtc_mode` ever held a real, valid mode? */

    /*
     * Auto-detected framebuffer, tracked via VRAM write activity
     * rather than any register: neither real guest OS this device has
     * been tested against (Mac OS X 10.2, Mac OS 9.2) ever programs
     * CRTC1's "Extended" mode-set registers at all -- confirmed live,
     * see the comment on ati_r350_scan_vram_activity() -- so the
     * *actual* live framebuffer, whenever it isn't the one CRTC1's
     * last-known-good mode already correctly describes, has to be
     * found some other way. `fb_scan_activity` is a decayed
     * hit-counter per ATI_R350_FB_SCAN_BLOCK-sized block of VRAM;
     * a sustained run of "recently written every scan" blocks is
     * treated as the real, currently-live framebuffer.
     */
    uint8_t fb_scan_activity[ATI_R350_VRAM_SIZE / ATI_R350_FB_SCAN_BLOCK];
    uint32_t fb_scan_counter;
    /*
     * Dirty VRAM blocks seen by the per-refresh dirty snapshot since the
     * activity scan last consumed them (the snapshot clears the bitmap, so
     * there is exactly one consumer of it -- ati_r350_update_display --
     * and the slower activity scan reads this instead).
     */
    bool fb_block_pending[ATI_R350_VRAM_SIZE / ATI_R350_FB_SCAN_BLOCK];
    /* redraw the whole surface next pass regardless of dirty state */
    bool force_redraw;
    bool auto_fb_valid;
    ATIR350Mode auto_fb_mode;
    /*
     * Adoption is gated on a candidate staying identical across two
     * consecutive scans: while a slow canvas paint is still in
     * progress the partial region matches a different (wrong)
     * resolution each scan, and after painting stops the region's
     * activity credit drains slice-by-slice in write order, shrinking
     * the run through more wrong intermediate shapes -- both churn
     * states never repeat the same candidate twice in a row, so the
     * stability gate suppresses them and only the settled, fully
     * painted region is ever adopted (verified in the qtest harness:
     * without this gate a 6-slice gradual paint visibly cycles the
     * display through 4+ wrong modes and can END on one).
     */
    bool auto_fb_pending_valid;

    /*
     * Apple Monitor Sense: report a connected MultiScan display on
     * the MONID sense pins (default on); off = nothing plugged in
     * (all sense lines float high).
     */
    bool monitor_connected;
    /*
     * Dual-head card (DVI-I + ADC). The DVI head always has the display;
     * this says whether the second head (ADC/VGA connector: its DDC pad,
     * the primary DAC's comparator and the Apple-sense lines) reports a
     * connected display too -- off by default so Mac OS sees one screen.
     */
    bool second_display;
    ATIR350Mode auto_fb_pending;

    /*
     * Same VBL model as the mach64 device: a gated pulse -- raise the
     * VBLANK status bit (and the PCI interrupt, when enabled) at each
     * frame's blank phase, drop the line a blank-length later. See
     * ati_mach64.c for why the free-running and held-until-ack
     * variants both wedged real ROM boots on that device.
     */
    QEMUTimer *vblank_timer;
    QEMUTimer *vblank_end_timer;
    /* coalesces a burst of cursor-register writes into one host update */
    QEMUTimer *cursor_timer;

    /*
     * Hardware I2C engine (I2C_CNTL_0/1 + I2C_DATA) serving a
     * generated EDID at DDC address 0xA0/0xA1 -- the Rage 128 has a
     * real I2C controller (unlike the mach64's bit-banged GP_IO pins)
     * and ATI's Mac FCode/NDRV use it for monitor detection.
     */
    qemu_edid_info edid_info;
    uint8_t edid[128];
    uint8_t i2c_offset;      /* current EDID read offset */
    uint8_t i2c_data_fifo[16];
    int i2c_data_len;
    int i2c_data_pos;

    /*
     * Second DDC path: the ATI Mac drivers (Mac OS X's ndrv on mac99,
     * and -- observed live on g3beige -- the classic Mac OS 9 driver
     * and the card FCode too) bit-bang I2C on the GPIO_MONID pads
     * (SDA = pad 0, SCL = pad 1), marking them with MASK nibble 0xf --
     * distinct from the Apple-sense probes (MASK 0x7) and the FCode's
     * EN-only convention (MASK 0). Serves the same 128-byte EDID as
     * the hardware engine above.
     */
    bool host_cursor_published;   /* synthetic arrow handed to the UI */
    bitbang_i2c_interface monid_i2c;
    int monid_sda;           /* live SDA level fed back into MONID_Y */
    /*
     * A bit-banged I2C session on pads 1 (SDA) / 2 (SCL) under the
     * Apple-sense MASK nibble 0x7 -- what Mac OS 9's "ATI Resource
     * Manager" does. Told apart from a sense probe by its clock: a
     * START (SDA falling while SCL is released) followed by SCL being
     * driven low opens the session; SDA rising while SCL is released
     * (STOP) closes it. A sense probe pulses one pad and never clocks.
     */
    bool monid7_i2c;         /* session open: pads answer as an I2C bus */
    bool monid7_start;       /* START seen, waiting for the first clock */
    bool monid7_sda_low;     /* pad 1 currently driven low */
    bool monid7_scl_low;     /* pad 2 currently driven low */
    uint32_t ddc1_pos;       /* DDC1 EDID bitstream position (in bits) */
    uint32_t ddc1_half;      /* half-bit phase: 2 manual VSYNC pulses/bit */

    /*
     * PM4/CCE GUI command-FIFO ring buffer -- the mechanism the real
     * Mac driver actually uses for register/2D submission on this
     * card (distinct from, and used after, the older one-shot
     * BM_GUI_TABLE descriptor engine). Register offsets and the ring
     * living in VRAM (this PCI, non-AGP variant has no GART/system-
     * memory command access) are cross-verified between a live trace
     * of the real driver and independent reference source; see the
     * comment on R350_PM4_BUFFER_OFFSET in ati_r350_regs.h.
     * Consumed synchronously on each PM4_BUFFER_DL_WPTR write, same
     * style as the BM engine.
     */
    uint32_t pm4_rptr;         /* ring read pointer, in dwords */
    uint32_t pm4_wptr;         /* ring write pointer, in dwords */
    /* ring base: VRAM offset, or GART offset when AGP_OFFSET_FLAG set */
    uint32_t pm4_buffer_addr;
    uint32_t pm4_buffer_cntl;
    uint32_t pm4_ring_dwords;  /* ring size decoded from CNTL, 0 = no ring */
    bool pm4_in_ib;            /* fetching from the indirect buffer */
    bool pm4_in_ring;          /* fetching from the ring */

    /*
     * CCE microcode store (PM4_MICROCODE_ADDR/RADDR/DATAH/DATAL):
     * 256 x 64-bit words, streamed as high/low pairs with the address
     * auto-incrementing after each DATAL access -- kept so a driver
     * that reads its upload back to verify sees exactly what it wrote.
     */
    uint32_t pm4_microcode[256][2];
    uint16_t pm4_ucode_waddr;
    uint16_t pm4_ucode_raddr;

    /*
     * PIO alternative to the ring: the real Mac driver actually
     * submits its command stream by writing raw dwords straight to
     * PM4_FIFO_DATA_EVEN/ODD (0x1000/0x1004, undocumented in the OEM
     * manual but confirmed live: consecutive writes there decode as
     * an ordinary PM4 packet stream, ending in a packet0 write of the
     * exact 64-bit fence value the driver then polls for at
     * GUI_SCRATCH_REG0/1). Both addresses behave identically -- they
     * just feed the next dword into this same parser state machine.
     */
    /*
     * One packet-parser state per independent command stream: the PIO
     * FIFO stream gets a persistent one (writes arrive one dword at a
     * time across many MMIO accesses), while each indirect-buffer
     * dispatch runs a fresh private instance -- an IB dispatch is
     * triggered from *inside* a FIFO packet (packet0 hitting
     * IW_INDOFF/INDSIZE), so sharing the parser state would corrupt
     * the framing of both streams (a real bug: it desynced the FIFO
     * stream after the first IB and wedged the guest driver).
     */
    ATIR350PM4Parser pm4_fifo;
    ATIR350PM4Parser pm4_ring;

    /* unimplemented-command tally -- see ati_r350_note_gap() */
    uint32_t gap_count[R350_GAP_MAX][R350_GAP_SLOTS];

    /*
     * Silent-register tally -- see ati_r350_audit_reg_write(). One
     * counter per register in the audited window, indexed by offset>>2:
     * a first-seen slot list saturated on Mac OS X 10.5, which writes
     * over 200 distinct silent registers in one session (measured
     * 2026-08-30: all 160 slots used, 75960 writes dropped). The word
     * count mirrors R350_REG_AUDIT_LIMIT in the generated
     * ati_r350_audit.h; ati_r350_dbg.c build-checks the pairing.
     */
#define R350_SILENT_REG_WORDS 6144
    uint32_t silent_reg_count[R350_SILENT_REG_WORDS];

    /*
     * 2D GUI (destination datapath) engine state -- ported from the
     * real upstream `ati-vga` device (hw/display/ati.c/ati_2d.c), not
     * generic/free-standing; register semantics are documented on the
     * offsets themselves in ati_r350_regs.h. These mirror upstream's
     * ATIVGARegs 2D fields one-to-one so the ported blt logic needs no
     * renaming.
     */
    uint32_t dst_offset, dst_pitch, dst_tile, dst_width, dst_height;
    uint32_t dst_x, dst_y;
    uint32_t src_offset, src_pitch, src_tile, src_x, src_y;
    uint32_t dp_gui_master_cntl;
    uint32_t dp_brush_bkgd_clr, dp_brush_frgd_clr;
    uint32_t dp_src_frgd_clr, dp_src_bkgd_clr;
    /*
     * Scissors are SIGNED 14-bit fields -- the register guide gives the
     * range as -8192..8191 ("Destination left scissor", RAGE 128 VR/GL
     * Register Reference Manual 7.6). Holding them unsigned turned a
     * legitimately negative left/top edge -- what the driver programs
     * for a window hanging off the left or top of the screen -- into a
     * huge positive one, which clips the whole drawing away.
     */
    int32_t sc_top, sc_left, sc_bottom, sc_right;
    int32_t src_sc_bottom, src_sc_right;
    uint32_t dp_cntl, dp_datatype, dp_mix, dp_write_mask;
    uint32_t default_offset, default_pitch;
    int32_t default_sc_bottom, default_sc_right;

    /*
     * The pitch/offset and scissor fields above are the EFFECTIVE values
     * the drawing code uses; these are the register values they are
     * derived from. DP_GUI_MASTER_CNTL's SRC/DST_PITCH_OFFSET_CNTL and
     * SRC/DST_CLIPPING bits select, per operation, whether the effective
     * value comes from the source/destination registers or from the
     * DEFAULT_* ones -- a selection, not a transfer. Folding it straight
     * into the effective field on every GMC write (which is what this
     * used to do) destroys the register behind it, so the result depends
     * on the order the driver happens to write things in. Mac OS X hits
     * exactly that: it programs the separate SRC_PITCH/SRC_OFFSET
     * registers rather than the packed SRC_PITCH_OFFSET, and every
     * PAINT/BITBLT packet carries its own GMC dword, so a blit could run
     * with a pitch left over from an earlier, differently sized surface.
     * One 8-pixel pitch unit of staleness shears the copy by 8 pixels per
     * row -- the diagonally streaked window contents on the Rage.
     */
    uint32_t src_offset_reg, src_pitch_reg, src_tile_reg;
    uint32_t dst_offset_reg, dst_pitch_reg, dst_tile_reg;
    /*
     * Radeon's standalone SRC_PITCH/DST_PITCH registers count BYTES;
     * the packed *_PITCH_OFFSET registers keep the Rage 128 8-pixel
     * unit. Track which flavour each side's live value came from so
     * the engine applies the right stride (OS X pages textures in
     * through byte-pitch blits; treating those as 8-pixel units
     * shredded every window into 32x-spaced slices).
     */
    bool src_pitch_bytes_reg, dst_pitch_bytes_reg;
    bool src_pitch_bytes, dst_pitch_bytes;
    int32_t sc_top_reg, sc_left_reg, sc_bottom_reg, sc_right_reg;
    int32_t src_sc_bottom_reg, src_sc_right_reg;

    /*
     * Diagnostic: CPU stores into the frame buffer go through aperture 0,
     * which is a plain RAM alias, so nothing in this device can see them
     * -- and that is exactly the path that fills the driver's offscreen
     * staging surface. Setting the "fillwatch"/"fillwatch-size" properties
     * lays an instrumented IO window over that range of aperture 0 so the
     * fills become visible. Writes are coalesced into contiguous runs and
     * traced one line per run, so a full surface fill costs a line per row
     * rather than one per store.
     */
    MemoryRegion vram_watch;
    uint32_t fillwatch_off, fillwatch_size;
    uint32_t fw_run_start, fw_run_end;
    bool fw_active;

    /*
     * Diagnostic: 3D draw capture. When the "draw-capture" property names
     * a file, every draw the rasterizer runs is also written there as a
     * self-contained record -- the resolved draw state, the transformed
     * vertices, the texture, and the destination rectangle before and
     * after the draw. `cap_fp` is NULL unless the property was given, and
     * that pointer is the only thing the draw path tests, so an unarmed
     * capture costs one predictable branch per draw and changes nothing.
     * See ati_r350_cap.h for the record format.
     */
    char *cap_path;
    FILE *cap_fp;
    bool cap_arm;               /* record at all: a settable QOM property */
    int draw_xr;                /* swapper xor the LAST redraw used */
    uint32_t cap_max;           /* records to take before closing the file */
    uint32_t cap_max_px;        /* skip a draw whose rectangle exceeds this */
    uint32_t cap_index;         /* records written so far */
    uint32_t cap_skipped;
    struct {
        uint32_t off, len, xr, hash, rec;
    } cap_tex[R350_CAP_TEX_CACHE];
    unsigned cap_tex_n;

    /*
     * Host-GPU offload (phase 2, milestone M2). `gl_path` is the "gl"
     * property as given; `gl_mode` is it parsed, and `gl_ctx` is NULL
     * unless a backend was actually opened -- that pointer is the only
     * thing the draw path tests, so a device left at the default is
     * untouched by any of this. See ati_r350_gl.h for the backend
     * interface and ati_r350_gl.c for the GL implementation of it.
     */
    char *gl_path;
    char *gl_api;               /* "gl-api": gl (default) or metal */
    ATIR350GlMode gl_mode;
    struct R350GlCtx *gl_ctx;
    uint64_t gl_drawn;          /* draws the backend rendered */
    uint64_t gl_fb[R350_GLF_MAX][R350_GAP_SLOTS];   /* fallbacks, by prim */
    /*
     * Of those fallbacks, the ones that were PROVED to paint no pixels
     * and therefore did not give the resident render target back. They
     * are counted in gl_fb[] as well -- this is a sub-count of it, not
     * a family beside it, and it exists so that `gl-stats` shows the
     * skipped release rather than leaving it to be inferred from a
     * flush count that went down.
     */
    uint64_t gl_nowork;
    /* view-volume clipping: draws cut, triangles cut, primitives dropped */
    uint64_t clip_draws, clip_tris, clip_drop, clip_ucp;
    /* gl=verify: the per-pixel agreement between the two paths */
    uint64_t gl_v_px, gl_v_hist[4];     /* delta 0, 1, 2-4, above 4 */
    uint64_t gl_v_draws, gl_v_bad;      /* draws compared / with delta > 1 */
    uint64_t gl_v_cover_px, gl_v_cover;  /* coverage-class pixels / differing */
    uint64_t gl_v_mesh;         /* ... of those, from a multi-triangle draw */
    unsigned gl_v_max, gl_v_vmax;
    /*
     * Scratch for one request, grown on demand and never shrunk: a draw
     * needs the destination rectangle twice over in RGBA, the decoded
     * texture, and the expanded triangle list. Milestone M3 hands these
     * to a queue instead, which is why they are sized per request rather
     * than allocated inside the backend.
     */
    uint8_t *gl_before, *gl_out, *gl_sw, *gl_texbuf;
    size_t gl_rect_sz, gl_texbuf_sz;
    float *gl_verts;
    size_t gl_verts_sz;
    float *gl_tcx;              /* the general form's coordinate block */
    size_t gl_tcx_sz;
    /*
     * The self-overlap partition's working set, sized once rather than
     * allocated per draw: which pass each triangle landed in, the
     * triangles in pass order, where each pass begins in the vertex
     * array, and the bounding boxes the pairwise test screens with.
     */
    uint8_t gl_pass[R300_GL_TRI_MAX];
    unsigned gl_order[R300_GL_TRI_MAX];
    unsigned gl_pass_first[R300_GL_PASS_MAX + 1];
    float gl_bbox[R300_GL_TRI_MAX][4];
    bool gl_fast;               /* gl=fast: allow the additive one-pass blend */
    uint64_t gl_addblend;       /* draws GL's own blender ordered */
    uint64_t gl_multipass;      /* draws that needed more than one pass */
    uint64_t gl_passes;         /* ... and how many passes in total */
    /*
     * The GL-owned render target (milestone M3). The rules that keep it
     * coherent with emulated VRAM are stated in full at "GL-OWNED RENDER
     * TARGET" in ati_r350_3d.c; these are the four facts they need.
     * `gl_res` says a target is resident at all and is the ONLY thing
     * the hot hooks test.
     */
    bool gl_res;
    uint32_t gl_res_off, gl_res_pitch;
    unsigned gl_res_xr;
    int gl_tex_w, gl_tex_h;             /* the backend texture's extent */
    int gl_vx0, gl_vy0, gl_vx1, gl_vy1; /* seeded: the GPU copy is good */
    int gl_dx0, gl_dy0, gl_dx1, gl_dy1; /* drawn: the GPU copy is NEWER */
    uint64_t gl_flushes;                /* fetches back into VRAM */
    uint64_t gl_flush_px, gl_seed_px;   /* ... and pixels moved each way */
    uint64_t gl_rel[R350_GLR_MAX];      /* which hook ended a residency */
    uint64_t gl_rel_px[R350_GLR_MAX];   /* ... and what it cost to */
    /* the same question one level finer for the 2D engine's three paths */
    uint64_t gl_rel_2d[R350_GL2D_MAX];
    uint64_t gl_rel_2d_px[R350_GL2D_MAX];
    /*
     * The GL-owned DEPTH buffer. The rules are at "GL-OWNED DEPTH BUFFER"
     * in ati_r350_3d.c. `gl_zres` says the GPU holds a copy of the Z
     * buffer described by gl_z_*, good inside the seeded rectangle and
     * newer than VRAM inside the drawn one; `gl_zpark` that the copy
     * outlived a release and must be checked against the dirty bitmap
     * before it is used again; `gl_ztaint` that VRAM under it was written
     * while it was resident, so it may not be kept.
     */
    bool gl_depth;              /* "gl-depth": depth-tested draws on the GPU */
    bool gl_zres, gl_zpark, gl_ztaint;
    uint64_t gl_zepoch;
    uint32_t gl_z_off, gl_z_pitch;
    bool gl_z_macro, gl_z_micro, gl_z_aa, gl_z_z16;
    int gl_zvx0, gl_zvy0, gl_zvx1, gl_zvy1;     /* seeded */
    int gl_zdx0, gl_zdy0, gl_zdx1, gl_zdy1;     /* drawn: GPU is NEWER */
    uint32_t *gl_zstage, *gl_zbefore, *gl_zgpu;
    size_t gl_zstage_n, gl_zv_n;
    uint64_t gl_zdrawn, gl_zflushes, gl_zflush_px, gl_zseed_px;
    uint64_t gl_zclears, gl_zkept, gl_zstale, gl_zdropped;
    uint64_t gl_zdrop_why[R350_GLZD_MAX];
    /*
     * "gl-depth-resident": the copy stays on the GPU across releases and
     * goes back to VRAM only when something reads it. The guest CPU's
     * accesses are trapped by the DEPTH GUARD, a window over the
     * aperture at [zg_lo, zg_hi) laid over the Z buffer's pages; zg_zlo
     * and zg_zhi are the Z bytes inside it. Changed with the BQL and the
     * engine both held. See "GL-OWNED DEPTH BUFFER".
     */
    bool gl_zlazy;
    bool zg_ready, zg_on;
    MemoryRegion zg_io, zg_win;
    uint32_t zg_lo, zg_hi, zg_zlo, zg_zhi;
    uint64_t gl_zlazy_rel, gl_zg_arms, gl_zg_traps, gl_zg_back;
    uint64_t gl_zclr_px;        /* cleared on the GPU only */
    /* gl=verify over the depth buffer, classed as the colour is */
    uint64_t gl_vz_draws, gl_vz_px, gl_vz_diff, gl_vz_cover_px, gl_vz_cover;
    /*
     * The occlusion counter (ZB_ZPASS_DATA, ZB_ZPASS_ADDR); the rules
     * are at "ZPASS COUNTER" in ati_r350_3d.c. Engine state, except
     * zq_draw, which the raster threads add to.
     */
    bool gl_zpass;              /* "gl-zpass": count queried draws on the GPU */
    bool gl_async;              /* "gl-async-compile": worker threads */
    uint32_t gl_workers;        /* "gl-compile-workers": 1..8 */
    bool zq_on;                 /* reset since the last dump: counting */
    bool zq_late;               /* drawn uncounted since the last dump */
    bool zq_gl;                 /* GL counted a draw since the reset */
    /* a 3D_CLEAR_ZMASK held for the first draw on its buffer */
    bool zclr_pend;
    uint32_t zclr_first, zclr_n, zclr_zoff, zclr_zp, zclr_clr;
    uint32_t zclr_zfmt, zclr_bw, zclr_smp;
    uint32_t zq_base;           /* the value ZB_ZPASS_DATA was given */
    uint64_t zq_sw;             /* software samples since the reset */
    uint64_t zq_draw;           /* the current draw's software samples */
    uint64_t zq_t0;             /* backend ticket at the reset */
    bool zq_cv;                 /* zq_cp is the backend's sum at zq_ct */
    uint64_t zq_ct, zq_cp;
    struct {
        uint32_t addr, part;    /* where, and base + software samples */
        unsigned swap;          /* ZB_DEPTHPITCH DEPTHENDIAN */
        uint64_t t0, t;         /* backend tickets: reset, dump */
    } zq_pend[R350_ZQ_PEND];
    unsigned zq_npend;
    uint64_t zq_resets, zq_dumps, zq_deferred, zq_gldraws, zq_swdraws;
    uint64_t zq_offdraws, zq_reads;
    /* gl=verify: each draw counted both ways */
    uint64_t zq_v_draws, zq_v_bad, zq_v_sw, zq_v_gl;
    /*
     * Decoded textures, keyed on everything the decode depends on.
     *
     * An entry is valid while the VRAM range it was decoded from is
     * UNWRITTEN -- that, and not the render target's residency, is what
     * the decode depends on. `epoch` is the dirty-bitmap generation the
     * entry was last known clean in; see r300_gl_tex_current() for the
     * rule and its two enforcement points.
     */
    struct {
        uint32_t off, len, pitch;
        unsigned bpp, code, xr;
        unsigned yuv;
        unsigned sel[4];
        int w, h;
        unsigned nlev;              /* mip levels decoded, one after another */
        bool cube;                  /* each level six faces tall */
        uint32_t lay;               /* offset of the last of them */
        uint8_t *rgba;
        size_t sz;
        uint64_t used;
        uint64_t epoch;             /* the bitmap generation it is clean in */
        unsigned npg;               /* host pages the range spans */
        bool live;                  /* the decoded bytes are current */
        bool up;                    /* ... and the backend has them too */
        uint16_t hb;                /* hash bucket, or R300_GL_TEXNIL */
        uint16_t hnext;             /* next entry in that bucket */
    } gl_tex[R300_GL_TEXCACHE];
    uint32_t gl_tex_slots;      /* "gl-texcache-slots" */
    unsigned gl_tex_n;          /* entries in use */
    bool gl_tex_hashed;         /* hash lookup and byte bound */
    uint16_t gl_tex_bucket[R300_GL_TEXHASH];
    size_t gl_tex_bytes;        /* decoded bytes held */
    uint64_t gl_tex_pin;        /* entries used after this are this draw's */
    uint64_t gl_tex_trim;       /* live entries dropped for the byte bound */
    uint64_t gl_tex_seq, gl_tex_hit, gl_tex_miss;
    uint64_t gl_tex_stale;      /* entries the dirty guard killed */
    /*
     * Why the other lookups missed. A hit rate is only actionable with
     * these beside it: an entry refused admission, one a writer killed,
     * one a draw rendered over and one the LRU evicted are four
     * different problems.
     */
    uint64_t gl_tex_noadmit;    /* range was dirty: cannot be guarded */
    uint64_t gl_tex_wrote;      /* a writer hook killed it */
    uint64_t gl_tex_over;       /* a draw rendered into its range */
    uint64_t gl_tex_evict;      /* the LRU gave its slot away */
    uint64_t gl_epoch;          /* bumped whenever the VGA bitmap is cleared */
    unsigned gl_pgbits;         /* qemu_target_page_bits(), resolved once */
    ATIR350GlTexLife gl_texlife;
    char *gl_texlife_path;
    bool gl_tex_any;            /* any entry live: the hot hooks test this */

    /*
     * Hardware cursor (CUR_* registers). hw_cursor_on tracks whether the
     * guest's own cursor is live, so the host-driven fallback pointer
     * stands aside rather than fighting it for the console cursor.
     * hw_cursor_sum is a checksum of the published image, so a shape
     * change made by writing VRAM alone is still picked up without
     * re-uploading an unchanged cursor on every frame.
     */
    bool hw_cursor_on;
    uint32_t hw_cursor_sum;
    /*
     * CUR_LOCK is a single bit that merely appears in bit 31 of all three
     * of CUR_OFFSET / CUR_HORZ_VERT_POSN / CUR_HORZ_VERT_OFF (RRG 3-80):
     * the most recent write to any of them sets or clears it. Kept here
     * rather than in the regs[] copies, which are stored without it.
     */
    bool cur_lock;
    /* last position published to the console, so an unchanged cursor is
     * not re-published (and re-traced) on every refresh tick */
    int hw_cursor_x, hw_cursor_y;
    /* the auto-detected framebuffer is currently overriding the CRTC mode
     * (tracked so the transition can be traced, not every frame) */
    bool auto_fb_overriding;
    /*
     * When the mach64's host-side pointer tracking is driving this display
     * (its host-cursor-tracking property, which the Mac OS 9 launcher turns
     * on), it owns the console cursor for good and the guest's own hardware
     * cursor must stand aside -- under Mac OS 9 the guest barely updates the
     * CUR_* registers on this card, so publishing them leaves a pointer
     * frozen at a stale position. Under Mac OS X, where tracking is off and
     * the guest drives the registers properly, the hardware cursor wins.
     *
     * Ownership is a latch, NOT a timeout. Expiring it after a second of no
     * host updates meant that whenever the pointer sat still on the other
     * display the hardware cursor took the console back and redisplayed its
     * stale position -- a ghost pointer left behind on this screen, plus an
     * artefact as the handover happened on the way across.
     */
    bool host_cursor_active;

    /* HOST_DATA0-7/LAST accumulator, same protocol as upstream */
    bool host_data_active;
    uint32_t host_data_row, host_data_col, host_data_next;
    uint32_t host_data_acc[4];
    /*
     * The drawing context the transfer STARTED with. A host-data transfer
     * spans many register writes, and its final partial accumulator is
     * only flushed when the NEXT blit arrives -- by which time the packet
     * that carries it has already installed its own destination rectangle,
     * datatype, pitch and scissors. Reading the live registers there wrote
     * one glyph's tail into the next glyph's cell, and where the stale
     * column index ran past the new width the unsigned `dst_width - col`
     * underflowed and painted sixteen pixels across its neighbours.
     * A transfer owns its context until it ends.
     */
    struct {
        uint32_t dst_x, dst_y, dst_width, dst_height;
        uint32_t dst_offset, dst_pitch;
        bool dst_pitch_bytes;
        uint32_t datatype;
        uint32_t src_frgd_clr, src_bkgd_clr;
        int sc_left, sc_top, sc_right, sc_bottom;
        unsigned host_swap_xor;   /* RBBM_GUICNTL.HOST_DATA_SWAP as a XOR */
    } hd;

    /*
     * Vertex-program RAM as uploaded through VAP_PVS_UPLOAD_ADDRESS/DATA.
     * It is one flat vector-indexed address space: vectors below
     * R300_PVS_CONST_START are program instructions, four dwords each,
     * and from there up they are the constant file. Neither is ever
     * cleared, so `pvs_code_slot_valid` remembers which instruction slots
     * the guest has actually written -- the control registers alone would
     * happily point at whatever a previous program left behind.
     */
    uint32_t pvs_upload_addr;
    uint32_t pvs_upload_cnt;
    uint32_t pvs_const[R300_PVS_CONST_SLOTS * 4];
    uint32_t pvs_const_dwords;
    /* user clip planes and point sprite state, R300_PVS_UCP_START on */
    uint32_t pvs_clip[R300_PVS_CLIP_VECS * 4];
    uint32_t pvs_code[R300_PVS_CODE_SLOTS * 4];
    uint32_t pvs_code_slot_valid[R300_PVS_CODE_SLOTS / 32];
    /* dwords ever uploaded to the code region: is there a program at all */
    uint32_t pvs_code_dwords;

    /*
     * The Z buffer of the draw in hand, decoded from the ZB_* registers
     * by r300_setup_draw(). Not in R300DrawState: that structure's size
     * is the draw-capture format's version number.
     */
    struct {
        bool z_en;          /* ZB_CNTL Z_ENABLE or STENCIL_ENABLE */
        bool z_test;        /* ZB_CNTL Z_ENABLE */
        bool z_wr;          /* ZB_CNTL ZWRITEENABLE */
        bool s_en;          /* ZB_CNTL STENCIL_ENABLE */
        bool s_fb;          /* ZB_CNTL STENCIL_FRONT_BACK */
        uint32_t zsc;       /* ZB_ZSTENCILCNTL */
        uint8_t s_ref, s_mask, s_wmask;     /* ZB_STENCILREFMASK */
        bool vte_zs, vte_zo;    /* VAP_VTE_CNTL z scale/offset enables */
        uint32_t vte_fmt;   /* VAP_VTE_CNTL VTX_XY/Z/W0_FMT bits */
        unsigned zfunc;     /* ZB_ZSTENCILCNTL ZFUNC */
        uint32_t off;       /* VRAM byte offset, 0x20-aligned */
        uint32_t pitch;     /* pixels */
        bool macro, micro;  /* ZB_DEPTHPITCH tiling */
        bool aa;            /* GB_AA_CONFIG: two samples per pixel */
        bool z16;           /* ZB_FORMAT 16-bit Z, no stencil */
    } zb;

    /*
     * Parallel rasterisation, ati_r350_3d.c. `raster_threads` counts the
     * threads that draw, the submitting one included: 0 for half the
     * host's cores, 1 to draw serially. NULL `raster` draws serially.
     */
    struct R300Raster *raster;
    uint32_t raster_threads;
    uint64_t raster_tri_split;      /* triangles drawn across threads */
    uint64_t raster_tri_serial;     /* ... and on the submitting thread */

    /*
     * Phase 2, milestone M4: how much of what real guests upload the
     * GLSL translation in ati_r350_pvs_glsl.c can express. Armed by the
     * "pvs-glsl" property and otherwise never entered, because the
     * translation is not on any pixel's path -- the offline three-way
     * harness measures what it COMPUTES, and this measures what it
     * COVERS, which no corpus of seven programs can answer.
     *
     * A program is translated once, when the control registers first
     * name it: `pvs_tr_sig` is the range and constant base the last
     * attempt was made for, so a thousand draws of one program cost one
     * translation.
     */
    bool pvs_glsl;
    uint64_t pvs_tr_sig;
    uint64_t pvs_tr_ok, pvs_tr_refused;
    uint64_t pvs_tr_by_reason[4];   /* vector op, math op, dst file, addr */
    uint32_t pvs_tr_last_bytes, pvs_tr_last_nconst;
    uint32_t pvs_tr_last_in, pvs_tr_last_out;

    /*
     * Phase 2, milestone M5: the fragment program in force, decoded once
     * per draw out of the US register banks. It lives here rather than in
     * the per-draw state because it is three kilobytes of decoded
     * instruction and because the GL backend keys its shader cache on it.
     * `us_sig` is the signature of the control words and instruction
     * dwords the decode was made from, so a thousand draws of one program
     * cost one decode.
     */
    R300UsProgram us_prog;
    uint64_t us_sig;
    uint32_t us_out_fmt;        /* US_OUT_FMT_0 the decode was made with */
    uint32_t us_vtx_fmt1;       /* VAP_OUTPUT_VTX_FMT_1 likewise */
    uint64_t us_draws, us_refused;
    /*
     * The same program as GLSL, for the host-GPU backend, translated
     * whenever the decode is. `us_glsl_key` is what the backend caches
     * the linked shader under and is never zero for a usable program.
     * The constants are flattened per draw because they change without
     * the program changing.
     */
    char us_glsl[64 * 1024];
    bool us_glsl_ok;
    uint64_t us_glsl_key;
    uint64_t us_glsl_ok_n, us_glsl_refused_n;
    /*
     * A `gl_simple` program in the general form, for a draw that needs
     * its fetch inside the shader (a cube map); made on demand for the
     * program `us_glsl_gen_for` names, keyed by `us_glsl_gen_key`.
     */
    char us_glsl_gen[64 * 1024];
    uint64_t us_glsl_gen_for, us_glsl_gen_key;
    float us_konst_flat[R300_US_CONSTS * 4];

    /*
     * Staging buffer for an in-flight R300 3D_DRAW_IMMD_2 or
     * 3D_DRAW_INDX_2 payload (VAP_VF_CNTL + inline vertices or
     * indices). Scratch state only: a packet split across a migration
     * is lost, like the 2D host-data accumulator above.
     */
    uint32_t r300_immd[16384];
};


/* ati_r350_dbg.c */
const char *ati_r350_reg_name(uint32_t base);

/*
 * Report a command the hardware understands and this model does not.
 * Warns on the first sight of each distinct value and counts every
 * one, so a silently dropped packet type shows up in the log the
 * first time a guest uses it instead of years later as a rendering
 * mystery. Packet3 opcode 0x1b -- the rectangle-only blit that moves
 * a window's body during a drag -- went unnoticed exactly that way:
 * its trace event existed but was never armed in any capture.
 * Read the running tally with `qom-get <device> gaps`.
 */
void ati_r350_note_gap(ATIR350State *s, ATIR350GapKind kind, unsigned idx);

/*
 * The register-level half of the same coverage question: tally a write
 * to a register no model code reads (the RBBM_GUICNTL class -- stored
 * into s->regs[] and consulted by no logic, so no gap ever fires).
 * Called from the ati_r350_reg_write32() funnel, which both the MMIO
 * path and PM4 type-0/1 packets go through. Read the tally with
 * `qom-get <device> silent-regs`.
 */
void ati_r350_audit_reg_write(ATIR350State *s, uint32_t base);

/* sizes of the private draw-capture payload structs (ati_r350_cap.h) */
uint32_t ati_r350_cap_state_bytes(void);
uint32_t ati_r350_cap_vtx_bytes(void);

/* why a draw went to the software rasterizer instead of the GL backend */
const char *ati_r350_gl_fb_name(ATIR350GlFallback why);

/*
 * GL-OWNED RENDER TARGET -- the coherency hooks. The rules are stated
 * in full where they are implemented, at "GL-OWNED RENDER TARGET" in
 * ati_r350_3d.c; what follows is how the rest of the device obeys them.
 *
 * ati_r350_gl_release() gives the target back: anything the host GPU
 * holds is fetched into VRAM first, and the GPU copy stops being
 * trusted. Call it before ANY code that reads or writes VRAM outside
 * the 3D draw path, and at every boundary where the guest CPU might run
 * -- which in this device means the end of a command-processor run,
 * because a whole ring or indirect buffer is drained inside one guest
 * store and nothing else can interleave with it.
 *
 * ati_r350_gl_touch() is the same thing for code that names a range and
 * is hot enough to care: it costs one predictable branch when no target
 * is resident, which is always the case with the default gl=off.
 */
void ati_r350_gl_release(ATIR350State *s, ATIR350GlRel why);
/* reset: give the target back and drop the decoded textures */
void ati_r350_gl_reset(ATIR350State *s);
void ati_r350_gl_sync(ATIR350State *s, uint32_t off, uint32_t len);
void ati_r350_gl_wrote(ATIR350State *s, uint32_t off, uint32_t len);
const char *ati_r350_gl_rel_name(ATIR350GlRel why);
/* the 2D sub-tally's row label -- see ATIR350Gl2dPath */
const char *ati_r350_gl_2d_path_name(ATIR350Gl2dPath path);

/*
 * The occlusion counter; see "ZPASS COUNTER" in ati_r350_3d.c. Called
 * by the thread that has the engine: the ZB_ZPASS_DATA and ZB_ZPASS_ADDR
 * writes, a ZB_ZPASS_DATA read, the guest-visible points where pending
 * dumps must have landed (settle), and reset.
 */
void ati_r350_zpass_reset(ATIR350State *s, uint32_t val);
void ati_r350_zpass_dump(ATIR350State *s, uint32_t addr);
uint32_t ati_r350_zpass_read(ATIR350State *s);
void ati_r350_zpass_settle(ATIR350State *s);
void ati_r350_zpass_drop(ATIR350State *s);

/*
 * The display refresh has just snapshotted and CLEARED the VGA dirty
 * bitmap, which is the one thing the decoded-texture cache reads to
 * know whether the guest CPU wrote over a texture. Hand the pages it
 * marks over before the snapshot is discarded (with scan_lock held,
 * the same hold as the clear); ati_r350_gl_epoch_apply() then kills
 * the entries over them and carries the rest into the new generation,
 * on the draw path, with gl_tex_lock held.
 */
void ati_r350_gl_epoch(ATIR350State *s, DirtyBitmapSnapshot *snap);
void ati_r350_gl_epoch_apply(ATIR350State *s);

/*
 * Take responsibility for the VGA dirty bits over a VRAM range the
 * decoded-texture cache wants to guard, feeding the scanout's own
 * accumulator first so nothing it uses is lost. False when the range
 * may not be claimed. Implemented in ati_r350.c, beside the scanout
 * code whose bits these are.
 */
bool ati_r350_gl_admit(ATIR350State *s, uint32_t off, uint32_t len);

/*
 * The depth guard (see "GL-OWNED DEPTH BUFFER" in ati_r350_3d.c).
 * ati_r350_zguard_arm() lays it over [lo, hi), page aligned, taking the
 * BQL if the caller does not hold it; the engine must be this thread's.
 * ati_r350_gl_zguard() is the trap's half in the draw code: the guest
 * CPU is about to access [off, off+len), the engine is held, and it
 * returns true when the guard has nothing left to watch.
 */
bool ati_r350_zguard_arm(ATIR350State *s, uint32_t lo, uint32_t hi,
                         uint32_t zlo, uint32_t zhi);
bool ati_r350_gl_zguard(ATIR350State *s, uint32_t off, unsigned len);

bool ati_r350_on_engine(void);
/* ati_r350_vram_xor() on this thread uses `memo` from now on */
void ati_r350_swap_memo_bind(ATIR350SwapMemo *memo);
void ati_r350_engine_wait(ATIR350State *s);

/*
 * May this thread use the GL target and the command processor's state
 * right now: always on the engine thread or the thread that claimed the
 * engine, elsewhere only while it is idle. Without lockless register
 * access it cannot become busy under the BQL, which every other caller
 * holds; with it, a caller here takes the engine if it is idle, and
 * ati_r350_gl_leave() gives it back when *claimed says so.
 */
bool ati_r350_gl_enter(ATIR350State *s, bool *claimed);
void ati_r350_gl_leave(ATIR350State *s, bool claimed);

/* something is about to READ this range of VRAM */
static inline void ati_r350_gl_touch(ATIR350State *s, uint32_t off,
                                     uint32_t len)
{
    if (unlikely(s->gl_res || s->gl_zres)) {
        ati_r350_gl_sync(s, off, len);
    }
}

/* ... and about to WRITE it, which also stales anything decoded from it */
static inline void ati_r350_gl_dirty(ATIR350State *s, uint32_t off,
                                     uint32_t len)
{
    if (unlikely(s->gl_res || s->gl_tex_any || s->gl_zres)) {
        ati_r350_gl_wrote(s, off, len);
    }
}

/* ati_r350_3d.c */
void ati_r350_r300_draw_immd(ATIR350State *s, const uint32_t *dw, unsigned n);
void ati_r350_r300_draw_vbuf(ATIR350State *s, uint32_t vf);
void ati_r350_r300_draw_indx(ATIR350State *s, const uint32_t *dw, unsigned n);
void ati_r350_r300_clear_zmask(ATIR350State *s, uint32_t first, uint32_t n,
                               uint32_t val);
void ati_r350_raster_init(ATIR350State *s);
void ati_r350_raster_fini(ATIR350State *s);

/* ati_r350.c MC-window translation, shared with the engines */
bool ati_r350_mc_to_vram(ATIR350State *s, uint32_t addr, uint32_t *off);
uint32_t ati_r350_mc_read32(ATIR350State *s, uint32_t addr);
void ati_r350_mc_write32(ATIR350State *s, uint32_t addr, uint32_t val);
void ati_r350_mc_read_block(ATIR350State *s, uint32_t addr, uint32_t *dst,
                            unsigned n);
#define R350_MC_MAP_REGS 7
void ati_r350_mc_map_save(ATIR350State *s, uint32_t *map);
bool ati_r350_mc_map_same(ATIR350State *s, const uint32_t *map);
/*
 * Trace helper: name the window a card address resolves through and
 * return the address it resolves to ("vram" -> a VRAM byte offset,
 * "agp" -> a bus address after AGP_BASE, "bus" -> passed through
 * untranslated). Read-only; it exists so a trace can record where a
 * fetch really went instead of leaving it to be inferred from
 * post-hoc register reads.
 */
const char *ati_r350_mc_describe(ATIR350State *s, uint32_t addr,
                                 uint64_t *target);

/* ati_r350_2d.c */
void ati_r350_2d_blt(ATIR350State *s);
void ati_r350_2d_scale(ATIR350State *s, const uint32_t *pkt);
void ati_r350_2d_scale_regs(ATIR350State *s);
bool ati_r350_host_data_flush(ATIR350State *s);

/*
 * Show/position a host-driven pointer on this card's console. Used by
 * the mach64's host-cursor-tracking workaround once the pointer crosses
 * onto this display: this device has no hardware-cursor emulation and
 * the guest never drives one here, so without it the pointer would
 * simply vanish on the second screen.
 */
void ati_r350_host_cursor(int x, int y, bool on);
/*
 * Byte-lane XOR that turns a raw VRAM byte offset into the byte a
 * little-endian consumer (CRTC, 2D engine, CP) sees there, given the
 * SURFACE_CNTL / SURFACEn swappers in force at that address: 0 = no
 * swap, 1 = 16-bit swap, 3 = 32-bit swap. See ati_r350_vram_xor().
 */
unsigned ati_r350_vram_xor(ATIR350State *s, uint32_t off);
uint32_t ati_r350_vram_ld32(ATIR350State *s, uint32_t off);

#endif /* ATI_R350_INT_H */
