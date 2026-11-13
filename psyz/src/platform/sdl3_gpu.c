#include <psyz.h>
#include <psyz/overlay.h>
#include <psyz/overlay_sdl3_gpu.h>
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "../internal.h"
#include <SDL3/SDL.h>
#include <webp/decode.h>
#include "sdl3_common.h"
#include "rasterizer.h"
static bool headless_renderer;
/* Psyz_VideoSetNativeRasterizer: requested, and currently drawing on the CPU
 * (only at 1x without GPU-only enhancements). */
static bool native_rasterizer_requested, native_rasterizer;
static bool hidden_gpu_renderer;
static bool UseSoftwareBackend(void);
#define PSYZ_SOFTWARE_ROUTING
static const void *interpolation_packet_current;
#define PSYZ_CAPTURE_INTERPOLATION(v,n,i,ni) InterpCapture(interpolation_packet_current,v,n,i,ni)
/* Drawing state changes between queued primitives (mask bits, drawing area,
 * offset) are recorded as marks instead of flushing (Draw_FlushBuffer). */
#define DRAW_STATE_MARKS 1
static void DrawStateChanging(void);
/* Why a batch is drawn (PSYZ_INTERP_STATS, PSYZ_FRAME_STATS): it is full
 * (vertices or indices, drawing-state marks, CLUT cache rows), a primitive
 * reads what the batch draws, the drawing state or VRAM changes, or a
 * present. */
enum { DRAW_FLUSH_STATE, DRAW_FLUSH_OVERFLOW, DRAW_FLUSH_MARKS, DRAW_FLUSH_CLUT,
       DRAW_FLUSH_FEEDBACK, DRAW_FLUSH_PRESENT, DRAW_FLUSH_REASONS };
static unsigned draw_flush_reason;
#define DRAW_FLUSH_FOR(reason) (draw_flush_reason = (reason), Draw_FlushBuffer())
static bool InterpBatchFull(unsigned vertices, unsigned indices);
#define DRAW_BATCH_FULL(vertices, indices) InterpBatchFull(vertices, indices)
/* A batch holds a race frame's geometry: a race, GP or Extra Trial replay
 * frame queues up to about 89000 vertices (PSYZ_INTERP_STATS), VS split
 * screen up to 115000, the attract sweep and Time Attack replays up to
 * 146000-165000 in a few frames, which then take two batches. The midpoint does not depend on
 * the batches: its geometry is kept for the whole frame (interp_mid_store)
 * and placed and drawn once the frame is complete (InterpMidpointSync). */
#define MAX_VERTEX_COUNT 131072
#include "sdl3_draw.h"

#if defined(_WIN32)
#include "shaders/psx_vert_dxil.h"
#include "shaders/psx_frag_dxil.h"
#include "shaders/clear_vert_dxil.h"
#include "shaders/clear_frag_dxil.h"
#define psx_vert psx_vert_dxil
#define psx_vert_len psx_vert_dxil_len
#define psx_frag psx_frag_dxil
#define psx_frag_len psx_frag_dxil_len
#define clear_vert clear_vert_dxil
#define clear_vert_len clear_vert_dxil_len
#define clear_frag clear_frag_dxil
#define clear_frag_len clear_frag_dxil_len
#ifdef psx_vert_dxil_revision
#define PSX_VERT_REVISION psx_vert_dxil_revision
#endif
#ifdef psx_frag_dxil_revision
#define PSX_FRAG_REVISION psx_frag_dxil_revision
#endif
#elif defined(__APPLE__)
#include "shaders/psx_vert_msl.h"
#include "shaders/psx_frag_msl.h"
#include "shaders/clear_vert_msl.h"
#include "shaders/clear_frag_msl.h"
#include "shaders/psx_frag_fetch_msl.h"
#include <objc/message.h>
#include <objc/runtime.h>
#define psx_vert psx_vert_msl
#define psx_vert_len psx_vert_msl_len
#define psx_frag psx_frag_msl
#define psx_frag_len psx_frag_msl_len
#define clear_vert clear_vert_msl
#define clear_vert_len clear_vert_msl_len
#define clear_frag clear_frag_msl
#define clear_frag_len clear_frag_msl_len
#ifdef psx_vert_msl_revision
#define PSX_VERT_REVISION psx_vert_msl_revision
#endif
#ifdef psx_frag_msl_revision
#define PSX_FRAG_REVISION psx_frag_msl_revision
#endif
#else
#include "shaders/psx_vert_spv.h"
#include "shaders/psx_frag_spv.h"
#include "shaders/clear_vert_spv.h"
#include "shaders/clear_frag_spv.h"
#define psx_vert psx_vert_spv
#define psx_vert_len psx_vert_spv_len
#define psx_frag psx_frag_spv
#define psx_frag_len psx_frag_spv_len
#define clear_vert clear_vert_spv
#define clear_vert_len clear_vert_spv_len
#define clear_frag clear_frag_spv
#define clear_frag_len clear_frag_spv_len
#ifdef psx_vert_spv_revision
#define PSX_VERT_REVISION psx_vert_spv_revision
#endif
#ifdef psx_frag_spv_revision
#define PSX_FRAG_REVISION psx_frag_spv_revision
#endif
#endif

/* The shader interface this file drives (PSYZ_SHADER_REVISION in
 * shaders/psx.*.glsl, copied into the headers by build_shaders.sh). Headers
 * built from older sources keep the previous behaviour: no exact polygons,
 * mask bit or CLUT cache, and the old upscaled sample positions. */
#define PSX_SHADER_REVISION 1
#if defined(PSX_VERT_REVISION) && defined(PSX_FRAG_REVISION) && \
    PSX_VERT_REVISION == PSX_SHADER_REVISION && \
    PSX_FRAG_REVISION == PSX_SHADER_REVISION
#define PSX_SHADERS_CURRENT 1
#else
#define PSX_SHADERS_CURRENT 0
#endif

typedef struct {
    int x, y;
} Posi;

#define VRAM_BYTES (VRAM_W * VRAM_H * 4)

static SDL_GPUDevice* device = NULL;
static bool swapchain_ok = false;
static SDL_GPUPresentMode swapchain_present_mode = SDL_GPU_PRESENTMODE_VSYNC;
static int swapchain_window_w, swapchain_window_h;
static SDL_WindowFlags swapchain_window_fullscreen;
static SDL_AtomicInt swapchain_resize_pending;
static bool SDLCALL WatchSwapchainResize(void *userdata, SDL_Event *event) {
    (void)userdata;
    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && sdl3_window &&
        event->window.windowID == SDL_GetWindowID(sdl3_window))
        SDL_SetAtomicInt(&swapchain_resize_pending, 1);
    return true;
}
/* Sticky until process restart. A failed SDL submission is consumed even when
 * Vulkan fails before queue admission, or after admission during presentation.
 * Never cancel/retry that pointer or destroy its dependency graph. */
static bool gpu_failed, gpu_retirement_uncertain;
static char gpu_first_error[512];
static SDL_GPUCommandBuffer *quarantined_cmd;
static SDL_GPUFence *quarantined_fence;
int Psyz_GpuFailed(void) { return gpu_failed; }
const char *Psyz_GpuFirstError(void) { return gpu_first_error; }
int Psyz_GpuRetirementUncertain(void) { return gpu_retirement_uncertain; }

static bool GpuFail(const char *stage, bool uncertain) {
    gpu_retirement_uncertain |= uncertain;
    if (!gpu_failed) {
        SDL_snprintf(gpu_first_error, sizeof(gpu_first_error), "%s: %s", stage, SDL_GetError());
        gpu_failed = true;
        ERRORF("GPU stopped; first error=%s; retirement_uncertain=%d", gpu_first_error, gpu_retirement_uncertain);
        /* Existing SDL event loop receives an explicit terminal request; the
         * public sticky API remains authoritative if event enqueue fails. */
        SDL_Event event = {0}; event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }
    return false;
}
static SDL_GPUTexture* vram_render = NULL;
static SDL_GPUTexture* rgb24_display = NULL;
static SDL_GPUTexture* presentation_display = NULL;
static Uint32 presentation_width, presentation_height;
static bool display_rgb24;
static bool display_double_height;
static SDL_GPUTexture* vram_sample = NULL;
static SDL_GPUTexture* blend_sample = NULL;
/* The GPU's CLUT cache (Draw_ClearCache): each load copies the palette from
 * the VRAM, in command order, into the next row of this ring; vertices name
 * the row their primitive reads (Vertex.exact[4][3]). It holds the loads
 * of a frame (a race frame at 8x loads up to about 700), which the
 * midpoint reads when the frame is complete (InterpMidpointSync). */
enum { CLUT_CACHE_ROWS = 2048 };
static SDL_GPUTexture* clut_cache = NULL;
static u32 clut_cache_reg = 0xFFFFFFFFu;
static bool clut_cache_8bit;
static unsigned clut_cache_row, clut_cache_loads; /* loads since a flush */
static SDL_GPUTexture* scaled_blend_sample = NULL;
static SDL_GPUSampler* vram_sampler = NULL;
static SDL_GPUTexture* scaled_vram_render = NULL;
/* Physical raster scale; public resolution excludes the SSAA factor. */
static unsigned internal_res = 1;
static bool antialiasing, set_antialiasing;
static bool model_filtering, set_model_filtering;
static bool sprite_filtering, set_sprite_filtering;
static SDL_GPUTexture* antialias_display;
static Uint32 antialias_width, antialias_height;
static SDL_GPUTexture* prescale_display;
static Uint32 prescale_width, prescale_height;
static unsigned set_internal_res = 1;
static bool geometry_correction = false;
static bool dynamic_lighting = false;
static bool effect_geometry_correction = false;
static bool set_effect_geometry_correction = false;
static bool set_geometry_correction = false;
static unsigned pgxp_trace_budget;
static unsigned long long pgxp_mixed_polygons, pgxp_none_polygons;
static unsigned long long pgxp_valid_polygons, pgxp_fallback_polygons, pgxp_nonfinite_polygons;
static SDL_GPUBuffer* vbuf = NULL;
static SDL_GPUBuffer* ibuf = NULL;
static SDL_GPUBuffer* clear_vbuf = NULL;
static SDL_GPUTransferBuffer* vtx_transfer = NULL;
/* Batches are uploaded one after another into vbuf/ibuf and vtx_transfer
 * (indices after MAX_VERTEX_COUNT vertices in the transfer buffer). Only a
 * batch that does not fit starts again at 0 and cycles the buffers, so SDL
 * allocates a new full-size buffer only when the previous one is still in use
 * at a wrap. Cycling on every batch made SDL keep one full-size buffer per
 * batch in flight; those pools never shrink (100+ GB with small batches). */
static Uint32 ring_vertex_pos, ring_index_pos;
static bool ring_restart = true;
static void RingWrapThrottle(void);
/* vbuf/ibuf still to cycle: a wrap reserved only transfer space (a fill's
 * quad goes to clear_vbuf), so the next batch upload cycles them */
static bool ring_cycle_buffers;
/* Space for a batch in the ring; true when the transfer buffer is to be
 * cycled (mapped with cycle). RingCycleBuffers then says whether the
 * batch's vbuf/ibuf uploads cycle. */
static bool RingReserve(Uint32 vertices, Uint32 indices, Uint32* vertex_at, Uint32* index_at) {
    const bool cycle = ring_restart || ring_vertex_pos + vertices > MAX_VERTEX_COUNT ||
                       ring_index_pos + indices > MAX_INDEX_COUNT;
    if (cycle) {
        RingWrapThrottle();
        ring_vertex_pos = ring_index_pos = 0;
        ring_restart = false;
        ring_cycle_buffers = true;
    }
    *vertex_at = ring_vertex_pos;
    *index_at = ring_index_pos;
    ring_vertex_pos += vertices;
    ring_index_pos += indices;
    return cycle;
}
static bool RingCycleBuffers(void) {
    const bool cycle = ring_cycle_buffers;
    ring_cycle_buffers = false;
    return cycle;
}
static SDL_GPUTransferBuffer* tex_upload_transfer = NULL;
/* VRAM uploads (LoadImage, a 24-bit display, a state restore) go one after
 * another into tex_upload_transfer, which is cycled only when one does not
 * fit: cycling it on every upload made SDL keep a VRAM-sized buffer per
 * upload in flight (a boot's or a movie's uploads ahead of the GPU took
 * gigabytes, and the pool never shrinks). */
static Uint32 upload_ring_pos;
static bool upload_ring_restart = true;
static u8* UploadMap(Uint32 bytes, Uint32* offset) {
    const Uint32 at = (upload_ring_pos + 511u) & ~511u;
    const bool cycle = upload_ring_restart || at + bytes > VRAM_BYTES;
    if (cycle)
        RingWrapThrottle();
    *offset = cycle ? 0 : at;
    upload_ring_pos = *offset + bytes;
    upload_ring_restart = false;
    u8* map = SDL_MapGPUTransferBuffer(device, tex_upload_transfer, cycle);
    return map ? map + *offset : NULL;
}
static SDL_GPUTransferBuffer* tex_download_transfer = NULL;
static SDL_GPUGraphicsPipeline* pipe_tri[BLEND_COUNT];
/* Semi-transparent primitives read their destination pixel in the fragment
 * shader from the colour attachment (Metal framebuffer fetch on Apple GPUs,
 * which orders the reads of overlapping fragments by primitive): no copy of
 * the target and no new render pass per primitive (Draw_FlushBuffer). The
 * shader computes the same blend from the same pixel as from the copy. */
static SDL_GPUGraphicsPipeline* pipe_direct_fetch;
static bool destination_fetch;
static SDL_GPUGraphicsPipeline* pipe_clear = NULL;
static SDL_GPUCommandBuffer* pending_cmd = NULL;
/* SDL D3D12 has 2048 sampler descriptors per command buffer. Keep half
 * available for presentation/overlays and never roll its heaps mid-draw. */
static unsigned command_descriptor_limit, command_descriptors;
static bool descriptor_rollover_reported;

static SDL_GPUCommandBuffer *AcquireCmd(void);
static SDL_GPUTexture *GetRenderTarget(void);
static bool SubmitCmdAndWait(void);
static bool InterpEnsureTarget(void);
/* Presentation-only geometry history. Packet pairs belong to the original
 * double buffers; UI packets are deliberately never registered. */
#define INTERP_PAIRS 16384u
#define INTERP_MAP 65536u
typedef struct { const void *address; unsigned pair, serial, frame, flags; } InterpAddress;
typedef struct {
    const void *a,*b;
    unsigned serial, frame, count;
    bool background;
    bool tagged; /* Psyz_GpuTagInterpolationPacket: a stable identity */
    /* the midpoint drawn for the identity this frame, for a second packet
     * of it in the same frame: 0 none, 1 at out */
    unsigned out_mode;
    float out[6][3];
    /* the pose at position was drawn into the midpoint only (a
     * PSYZ_INTERP_MIDPOINT_ONLY packet): not a pose the game drew */
    bool midpoint_pose;
    unsigned motion_frame; /* frame of motion: its last paired move */
    float motion[6][2];
    uint64_t signature;
    float position[6][3];
    float depth[6]; /* view depths of position (vertex_depth), 0 for none */
    unsigned long long id; /* the identity (PSYZ_INTERP_PROBE) */
    /* Psyz_GpuTagInterpolationPacket'ed for this frame */
    unsigned tagged_frame;
    /* The primitive the frame drew (interp_store), for a ghost in the next
     * midpoint when that frame leaves the identity out (InterpBuildGhosts):
     * its frame, vertices, indices and drawing offset. */
    unsigned store_frame, store_offset;
    unsigned char store_count, store_indices, store_index[6];
    /* the drawing offset within the drawing area, and the area's size */
    short store_offset_x, store_offset_y, store_width, store_height;
    /* ghosts drawn after this identity's primitive (pair index + 1) in the
     * frame ghost_frame, and the next ghost of a list */
    unsigned ghost_frame, ghost_head, ghost_next;
} InterpPair;
static InterpAddress interp_addresses[INTERP_MAP],interp_queue[INTERP_MAP];
/* Packets tagged with a stable identity (Psyz_GpuTagInterpolationPacket):
 * the packet address -> its identity's pair, for the frame it is drawn in. */
static InterpAddress interp_tags[INTERP_MAP];
typedef struct { unsigned long long id; unsigned pair, serial; } InterpId;
static InterpId interp_ids[INTERP_MAP];
/* The midpoint waits for a blank without a new frame (a frame that takes
 * more than one field): Psyz_GpuPrepareInterpolatedBlank. */
static bool interp_blank_mid;
static InterpPair interp_pairs[INTERP_PAIRS];
static unsigned interp_serial,interp_frame,interp_phase,interp_matches;
static unsigned interp_camera_frame;
static int interp_camera_x,interp_camera_y;
static float interp_camera_half_x,interp_camera_half_y;
static bool interp_camera_previous;
static int interp_next_y=-1;
static bool interpolation_requested;
static bool interpolation_enabled,interp_present_mid,interp_pair_ready;
static SDL_GPUTexture *interpolated_vram;
/* Failed submission/wait is not a completion witness. Retain until idle. */
static SDL_GPUTexture *interp_unwitnessed;
static unsigned interpolated_scale;
/* The midpoint's vertices of the frame: of the batches drawn into the
 * frame's targets since the last InterpMidpointSync (interp_mid_len of
 * them), then of the batch being queued, which interpolated_vertices (and
 * interp_mid_depth) index as vertex_buf does. Ghosts, new identities, lines
 * and T-junctions are placed from the neighbours of the whole frame, then
 * the midpoint is drawn (InterpMidpointSync); before that only what would
 * change what it draws makes it draw early (a split, PSYZ_INTERP_STATS). */
#define INTERP_MID_VERTICES 262144u
#define INTERP_MID_INDICES (INTERP_MID_VERTICES / 4u * 6u)
static Vertex interp_mid_store[INTERP_MID_VERTICES];
static float interp_mid_depth_store[INTERP_MID_VERTICES];
static u32 interp_mid_index[INTERP_MID_INDICES];
static unsigned interp_mid_len, interp_mid_index_len;
static Vertex *interpolated_vertices = interp_mid_store;
static float *interp_mid_depth = interp_mid_depth_store;
static void InterpMidpointSync(unsigned reason);
static void InterpMidpointComplete(unsigned reason);
static bool InterpMidpointDeferred(void);
/* Why the midpoint is drawn: the frame is complete, or (a split) its
 * store is full, a VRAM change reaches what it samples or draws over, a
 * present or capture shows it, or its target or settings change. */
enum { INTERP_SYNC_FRAME, INTERP_SYNC_FULL, INTERP_SYNC_TEXTURE, INTERP_SYNC_CLUT, INTERP_SYNC_TARGET,
       INTERP_SYNC_PRESENT, INTERP_SYNC_SETTINGS, INTERP_SYNC_N };
/* Entries the fixed interpolation tables drop or overwrite while live
 * (PSYZ_INTERP_STATS; a warning the first time for each). */
enum { IEVICT_ADDRESSES, IEVICT_TAGS, IEVICT_QUEUE, IEVICT_IDS, IEVICT_PAIRS, IEVICT_STORE, IEVICT_ORDER,
       IEVICT_FIELD, IEVICT_EDGES, IEVICT_EDGE_REFS, IEVICT_PENDING, IEVICT_JUNCTIONS, IEVICT_LINES, IEVICT_N };
static unsigned interp_evict[IEVICT_N];
static void InterpEvict(unsigned table);
/* Batches of the interpolation frame (PSYZ_INTERP_STATS) and of the
 * present (PSYZ_FRAME_STATS): vertices and indices queued, the largest
 * batch, flushes by reason, midpoint draws (syncs) by reason. */
typedef struct {
    unsigned vertices, indices, batch_vertices, batch_indices;
    unsigned flushes[DRAW_FLUSH_REASONS], syncs[INTERP_SYNC_N], split_pending, clut_loads;
} DrawFrameStats;
static DrawFrameStats draw_frame_stats;
/* primitives queued since the last flush (PSYZ_BATCH_PRIMITIVES) */
static unsigned batch_primitives;
/* 16x16-pixel cells of VRAM: what the midpoint kept so far samples
 * (texture pages) and draws over (drawing areas), and what changed in the
 * native VRAM since vram_sample was brought up to date (vram_dirty). */
#define VRAM_TILE 16
typedef struct { uint64_t row[VRAM_H / VRAM_TILE]; } VramTiles;
static VramTiles interp_mid_texture_tiles, interp_mid_target_tiles, vram_dirty_tiles;
static void VramTilesAdd(VramTiles *t, int x, int y, int w, int h) {
    int x0 = SDL_max(x, 0), y0 = SDL_max(y, 0), x1 = SDL_min(x + w, VRAM_W), y1 = SDL_min(y + h, VRAM_H);
    if (x1 <= x0 || y1 <= y0)
        return;
    x0 /= VRAM_TILE, x1 = (x1 - 1) / VRAM_TILE, y0 /= VRAM_TILE, y1 = (y1 - 1) / VRAM_TILE;
    const uint64_t bits = (x1 - x0 >= 63 ? ~0ull : ((1ull << (x1 - x0 + 1)) - 1)) << x0;
    for (int r = y0; r <= y1; ++r)
        t->row[r] |= bits;
}
static bool VramTilesHit(const VramTiles *a, const VramTiles *b) {
    for (unsigned r = 0; r < VRAM_H / VRAM_TILE; ++r)
        if (a->row[r] & b->row[r])
            return true;
    return false;
}
static bool VramTilesHitRect(const VramTiles *a, int x, int y, int w, int h) {
    VramTiles t = {{0}};
    VramTilesAdd(&t, x, y, w, h);
    return VramTilesHit(a, &t);
}
/* CLUT cache rows loaded since the first batch the midpoint keeps */
static unsigned interp_mid_clut_loads;
static bool InterpMidpointCovers(int x, int y, int w, int h);
static bool InterpMidpointSamplesRect(int x, int y, int w, int h);
/* A texture page (tpage bits of Vertex.t): 64, 128 or 256 VRAM pixels by
 * 256, wrapping at the VRAM's right edge. */
static void VramTilesAddPage(VramTiles *t, unsigned tpage) {
    const int x = (int)(tpage & 15) * 64, y = (int)((tpage >> 4) & 1) * 256;
    const int depth = (int)((tpage >> 7) & 3), w = depth == 0 ? 64 : depth == 1 ? 128 : 256;
    VramTilesAdd(t, x, y, w, 256);
    if (x + w > VRAM_W)
        VramTilesAdd(t, 0, y, x + w - VRAM_W, 256);
}
static unsigned long long interpolated_present_count;
/* The screen area (pixels) of the frame's tagged primitives by what the
 * midpoint does with them (InterpMidpointUnsound); PSYZ_INTERP_STATS=1 logs
 * them per frame, PSYZ_INTERP_PROBE (InterpProbe) the primitives at a pixel. */
enum { ISTAT_PAIRED, ISTAT_NEW, ISTAT_MISMATCH, ISTAT_FRAME_ONLY, ISTAT_MID_ONLY, ISTAT_VANISHED, ISTAT_GHOST, ISTAT_REPLACED, ISTAT_N };
static double interp_area[ISTAT_N];
static unsigned interp_count[ISTAT_N];
static int interp_stats_log=-1;
static float InterpArea(float (*pos)[3],unsigned count);
/* Ghosts: an identity the frame before drew and this frame leaves out (a
 * tile leaving the drawn range, a level of detail switching to another
 * representation, an object culled) would leave its place in the midpoint
 * to whatever is drawn behind it. Its primitive is kept (interp_store) and
 * drawn into the midpoint only, where it was half a frame later, right
 * after the primitive drawn before it in that frame (interp_order). A new
 * identity drawn where a ghost goes (the other representation) stays out
 * of the midpoint. PSYZ_INTERP_GHOSTS=0 turns them off. */
#define INTERP_STORE 32768u
/* the largest move (pixels a frame) a ghost continues */
#define INTERP_GHOST_MOTION 384.0f
#define INTERP_ORDER 16384u
static Vertex interp_store[2][INTERP_STORE];
static unsigned interp_store_len[2];
static unsigned interp_order[2][INTERP_ORDER],interp_order_len[2];
static unsigned interp_inject,interp_ghost_first;
static int interp_ghosts=-1;
/* the frame's ghosts leave out more than this share of its identities: a cut */
static bool interp_ghost_cut;
#define INTERP_COVER_CELL 8
static unsigned short interp_ghost_cover[VRAM_H/INTERP_COVER_CELL][VRAM_W/INTERP_COVER_CELL];
static const u32 *interp_capture_index;
static unsigned interp_capture_indices;
static bool InterpGhostCovers(float (*pos)[3],unsigned count);
static bool InterpGhostCoversLine(float (*pos)[3],int count);
static void InterpStore(InterpPair *p,unsigned pair,const Vertex *v,unsigned count);
static void InterpBuildGhosts(void);
/* The triangles (bits) of the polygon being queued that the GPU skips for
 * their size (1024+ columns or 512+ rows): see InterpCaptureDraw. */
static unsigned interp_size_culled;
/* The primitive being captured waits for InterpFinishPending. */
static bool interp_capture_pending;
/* The drawing offset in VRAM: tells the views of a frame (split screen,
 * mirror) apart in the motion fields. */
static int InterpOffsetX(void);
static int InterpOffsetY(void);
/* How the midpoint moved this frame's paired vertices (InterpCaptureDraw):
 * field 0 by their pose in the frame before, field 1 by their pose in this
 * frame. A ghost's vertex (a pose of the frame before only) moves as the
 * nearest vertex at a like depth in field 0, a new identity's (a pose of
 * this frame only) as the nearest in field 1, joined to it where it is the
 * same vertex (InterpFinishPending). A ghost's own continued motion does
 * not know how the view moved this frame, and a new identity's pose in
 * this frame is not where its neighbours are in the midpoint. Entries are
 * hashed by cells of their pose. */
#define INTERP_FIELD 65536u
#define INTERP_FIELD_CELL 16
#define INTERP_FIELD_REACH 24.0f
#define INTERP_FIELD_CELLS 4096u
typedef struct { float x, y, depth, dx, dy; short ox, oy; unsigned next; } InterpFieldEntry;
typedef struct {
    InterpFieldEntry entry[INTERP_FIELD];
    unsigned len, frame, head[INTERP_FIELD_CELLS], stamp[INTERP_FIELD_CELLS];
} InterpField;
static InterpField interp_field[2];
static unsigned InterpFieldCell(int cx,int cy,int ox,int oy) {
    uint32_t h=(uint32_t)cx*0x9E3779B1u^(uint32_t)cy*0x85EBCA77u^(uint32_t)(ox*977+oy)*0xC2B2AE3Du;
    return (h^h>>16)&(INTERP_FIELD_CELLS-1);
}
/* Returns the entry (1-based), 0 for none. */
static unsigned InterpFieldAdd(unsigned which,const float *from,float depth,int ox,int oy,const float *to) {
    InterpField *f=&interp_field[which];
    if(f->frame!=interp_frame){f->frame=interp_frame;f->len=0;}
    if(f->len>=INTERP_FIELD){InterpEvict(IEVICT_FIELD);return 0;}
    if(!(SDL_fabsf(from[0])<2048.0f&&SDL_fabsf(from[1])<2048.0f))return 0;
    const unsigned c=InterpFieldCell((int)SDL_floorf(from[0]/INTERP_FIELD_CELL),(int)SDL_floorf(from[1]/INTERP_FIELD_CELL),ox,oy);
    if(f->stamp[c]!=interp_frame){f->stamp[c]=interp_frame;f->head[c]=0;}
    f->entry[f->len]=(InterpFieldEntry){from[0],from[1],depth,to[0]-from[0],to[1]-from[1],(short)ox,(short)oy,f->head[c]};
    f->head[c]=++f->len;
    return f->len;
}
/* The motion of the nearest entry within reach at a like depth (depth 0:
 * any), its squared distance at *distance; false for none. */
static bool InterpFieldMotion(unsigned which,const float *at,float depth,int ox,int oy,float motion[2],float *distance) {
    const InterpField *f=&interp_field[which];
    if(f->frame!=interp_frame||!(SDL_fabsf(at[0])<2048.0f&&SDL_fabsf(at[1])<2048.0f))return false;
    const int cx=(int)SDL_floorf(at[0]/INTERP_FIELD_CELL),cy=(int)SDL_floorf(at[1]/INTERP_FIELD_CELL);
    float best=INTERP_FIELD_REACH*INTERP_FIELD_REACH;bool found=false;
    for(int y=cy-2;y<=cy+2;++y)for(int x=cx-2;x<=cx+2;++x){const unsigned c=InterpFieldCell(x,y,ox,oy);
        if(f->stamp[c]!=interp_frame)continue;
        for(unsigned k=f->head[c];k;k=f->entry[k-1].next){const InterpFieldEntry *e=&f->entry[k-1];
            if(e->ox!=ox||e->oy!=oy)continue;
            if(depth>0.0f&&e->depth>0.0f&&SDL_fabsf(e->depth-depth)>0.25f*SDL_max(e->depth,depth))continue;
            const float d=(e->x-at[0])*(e->x-at[0])+(e->y-at[1])*(e->y-at[1]);
            if(d<best){best=d;motion[0]=e->dx;motion[1]=e->dy;found=true;}}}
    if(found)*distance=best;
    return found;
}
/* Where a primitive drawn now lands in the frame's buffer: the drawing
 * offset, the widescreen squeeze and the drawing area. */
typedef struct { float ox, oy, squeeze, centre; int clip[4]; } InterpExposeView;
static void InterpExposeViewNow(InterpExposeView *view);
/* Ghosts (field 0) and new identities (field 1) queued since the last
 * flush: their midpoint pose is final once the primitives queued with them
 * are (Draw_FlushBuffer). from: the pose they move from. */
#define INTERP_PENDING 16384u
typedef struct {
    unsigned pair, base;
    unsigned char which, count, indices, index[6];
    short ox, oy;
    float from[6][2], depth[6];
    InterpExposeView view;
} InterpPending;
static InterpPending interp_pending[INTERP_PENDING];
static unsigned interp_pending_len;
static bool InterpPend(unsigned which,unsigned pair,unsigned base,unsigned count,const float (*from)[3],const float *depth,
                       const unsigned char *index,unsigned indices,int ox,int oy) {
    if(count>6||indices>6)return false;
    if(interp_pending_len>=INTERP_PENDING){InterpEvict(IEVICT_PENDING);return false;}
    InterpPending *q=&interp_pending[interp_pending_len++];
    /* base: in the frame's midpoint store */
    q->pair=pair;q->base=interp_mid_len+base;q->which=(unsigned char)which;q->count=(unsigned char)count;q->indices=(unsigned char)indices;
    q->ox=(short)ox;q->oy=(short)oy;
    for(unsigned i=0;i<count;++i){q->from[i][0]=from[i][0];q->from[i][1]=from[i][1];q->depth[i]=depth[i];}
    for(unsigned i=0;i<indices;++i)q->index[i]=index[i];
    InterpExposeViewNow(&q->view);
    return true;
}
static void InterpFinishPending(unsigned pending);
/* T-junctions: a vertex of one paired primitive that lies on an edge of
 * another (inside it, not at its ends) in both frames, a piece of a finer
 * subdivision meeting a coarser one. Each frame keeps it on that edge (up
 * to the GTE's rounding), but the view-space midpoints of the vertex and of
 * the edge's ends need not stay collinear (the depth weighting of a vertex
 * near the view, a midpoint-only pose nearer than the projection plane),
 * and the crack shows what lies behind. The edges of this frame's paired
 * primitives are kept by their pose in the frame before (cells of it); at
 * each flush the queued paired vertices that lie on one in both frames, to
 * the projection's precision (INTERP_JUNCTION_PRECISE with PGXP, else
 * _PIXELS; with a pose in the frame before only, plus _SLOPE of the
 * edge's length), go onto its midpoint
 * (InterpSnapJunctions), by at most INTERP_JUNCTION_MOVE pixels. */
#define INTERP_EDGES 65536u
#define INTERP_EDGE_REFS 262144u
#define INTERP_EDGE_CELL 16
#define INTERP_EDGE_CELLS 4096u
#define INTERP_JUNCTION_REACH 2.0f
#define INTERP_JUNCTION_PRECISE 0.25f
#define INTERP_JUNCTION_PIXELS 0.75f
#define INTERP_JUNCTION_SLOPE 0.004f
#define INTERP_JUNCTION_MOVE 6.0f
typedef struct { float a0[2], b0[2], a1[2], b1[2], am[2], bm[2], da, db; unsigned pair; short ox, oy; bool posed; } InterpEdge;
typedef struct { unsigned edge, next; } InterpEdgeRef;
static InterpEdge interp_edges[INTERP_EDGES];
static InterpEdgeRef interp_edge_refs[INTERP_EDGE_REFS];
static unsigned interp_edges_len,interp_edge_refs_len,interp_edges_frame;
static unsigned interp_edge_head[INTERP_EDGE_CELLS],interp_edge_stamp[INTERP_EDGE_CELLS];
/* the paired primitives queued since the last flush */
#define INTERP_JUNCTION_PRIMS 16384u
typedef struct { unsigned pair, base, field[2][6]; unsigned char count; short ox, oy; bool posed; float p0[6][2], p1[6][2], d0[6]; } InterpJunctionPrim;
static InterpJunctionPrim interp_junction_prims[INTERP_JUNCTION_PRIMS];
static unsigned interp_junction_prims_len;
static void InterpSnapJunctions(unsigned n);
static void InterpAddEdge(const float *a0,const float *b0,const float *a1,const float *b1,const float *am,const float *bm,
                          float da,float db,unsigned pair,int ox,int oy,bool posed);
/* Exposure: what the midpoint shows where both frames show something
 * nearer. Each frame's opaque primitives and the midpoint's are rasterized
 * at cell centres into depth grids (the nearest view depth, 0 for 2D),
 * relative to the frame's buffer (Psyz_GpuInterpolationBackdrop). A cell
 * both frames cover that the midpoint leaves uncovered, or covers with
 * something much farther, shows what neither frame shows (a hole, the
 * background through a gap): such a midpoint is not shown (see
 * InterpMidpointUnsound). PSYZ_INTERP_EXPOSE=<cells> sets the limit. */
#define INTERP_EXPOSE_CELL 4
#define INTERP_EXPOSE_W (VRAM_W/INTERP_EXPOSE_CELL)
#define INTERP_EXPOSE_H (VRAM_H/INTERP_EXPOSE_CELL)
typedef struct { float depth; unsigned frame; } InterpExposeCell;
/* [0], [1]: the frames by parity; [2]: the midpoint */
static InterpExposeCell interp_expose[3][INTERP_EXPOSE_H][INTERP_EXPOSE_W];
static struct { int x, y, w, h; unsigned frame; } interp_expose_rect[2];
static void InterpExposeTriangle(unsigned grid,const float *a,const float *b,const float *c,float da,float db,float dc,const int clip[4]);
static void InterpExposePrimitive(unsigned grid,const InterpExposeView *view,const Vertex *v,const float *depth,const u32 *index,unsigned indices,unsigned base,bool precise);
static unsigned InterpExposed(void);
static unsigned interp_exposed_holes;
static bool present_commit_pending,present_commit_mid;
static Uint64 present_window_start;
static unsigned present_window_frames,present_window_mid;
static float actual_present_fps;
float Psyz_GpuGetPresentedFps(void){return actual_present_fps;}
static void RecordPresentedCommit(void){
    if(!present_commit_pending)return;
    present_commit_pending=false;
    Uint64 now=SDL_GetTicks();
    if(!present_window_start){present_window_start=now;return;}
    ++present_window_frames;
    if(present_commit_mid)++present_window_mid;
    Uint64 elapsed=now-present_window_start;
    if(elapsed>=1000){
        actual_present_fps=1000.0f*present_window_frames/(float)elapsed;
        INFOF("[Presentation FPS] actual=%.1f midpoint=%.1f requested=%d effective=%d logic=original elapsed_ms=%llu",actual_present_fps,1000.0f*present_window_mid/(float)elapsed,interpolation_requested,interpolation_enabled,(unsigned long long)elapsed);
        present_window_start=now;present_window_frames=present_window_mid=0;
    }
}

static unsigned InterpHash(const void *p) {
    uintptr_t n=(uintptr_t)p;return (unsigned)(((n>>2)^(n>>16)^(n>>30))&(INTERP_MAP-1));
}
static InterpAddress *InterpFind(InterpAddress *map,const void *p) {
    unsigned h=InterpHash(p);
    for(unsigned i=0;i<4;++i){InterpAddress *e=&map[(h+i)&(INTERP_MAP-1)];if(e->address==p)return e;}
    return NULL;
}
static void InterpMap(InterpAddress *map,const void *p,unsigned pair,unsigned serial) {
    unsigned h=InterpHash(p);InterpAddress *e=InterpFind(map,p);
    if(!e){e=&map[h];bool free_slot=false;
        for(unsigned i=0;i<4;++i)if(!map[(h+i)&(INTERP_MAP-1)].address){e=&map[(h+i)&(INTERP_MAP-1)];free_slot=true;break;}
        /* another packet's entry goes: count it when its pair (for a tag,
         * its frame) is still current */
        if(!free_slot&&e->serial&&interp_pairs[e->pair].serial==e->serial&&(map!=interp_tags||e->frame>=interp_frame))
            InterpEvict(map==interp_tags?IEVICT_TAGS:map==interp_queue?IEVICT_QUEUE:IEVICT_ADDRESSES);}
    *e=(InterpAddress){p,pair,serial};
}
/* Until the first pair the midpoint target is not drawn: it is refreshed
 * from the render target at the next interpolation frame. */
static bool interp_target_stale;
void Psyz_GpuRegisterInterpolationPair(const void *a,const void *b) {
    if(!a||!b)return;
    if(!interp_serial)interp_target_stale=true;
    unsigned serial=++interp_serial;if(!serial)serial=++interp_serial;
    unsigned index=serial&(INTERP_PAIRS-1);
    if(interp_pairs[index].serial&&interp_pairs[index].frame&&interp_pairs[index].frame+1>=interp_frame)InterpEvict(IEVICT_PAIRS);
    interp_pairs[index]=(InterpPair){.a=a,.b=b,.serial=serial};
    InterpMap(interp_addresses,a,index,serial);InterpMap(interp_addresses,b,index,serial);
}
void Psyz_GpuRegisterInterpolationBackgroundPair(const void *a,const void *b) {
    Psyz_GpuRegisterInterpolationPair(a,b);
    InterpAddress *e=InterpFind(interp_addresses,a);if(e&&interp_pairs[e->pair].serial==e->serial)interp_pairs[e->pair].background=true;
}
void Psyz_GpuEnsureInterpolationPair(const void *a,const void *b) {
    InterpAddress *x=InterpFind(interp_addresses,a),*y=InterpFind(interp_addresses,b);
    if(x&&y&&x->pair==y->pair&&x->serial==y->serial&&x->serial&&interp_pairs[x->pair].serial==x->serial)return;
    Psyz_GpuRegisterInterpolationPair(a,b);
}
void Psyz_GpuEnsureInterpolationBackgroundPair(const void *a,const void *b) {
    Psyz_GpuEnsureInterpolationPair(a,b);
    InterpAddress *e=InterpFind(interp_addresses,a);if(e&&interp_pairs[e->pair].serial==e->serial)interp_pairs[e->pair].background=true;
}
/* A tagged packet of the frame being drawn, or NULL. */
static InterpAddress *InterpFindTagged(const void *packet) {
    InterpAddress *t=InterpFind(interp_tags,packet);
    return t&&t->serial&&t->frame==interp_frame&&interp_pairs[t->pair].serial==t->serial ? t : NULL;
}
void Psyz_GpuTagInterpolationPacket(const void *packet,unsigned long long id) {
    Psyz_GpuTagInterpolationPacketFlags(packet,id,0);
}
void Psyz_GpuTagInterpolationPacketFlags(const void *packet,unsigned long long id,unsigned flags) {
    if(!interpolation_enabled||!packet)return;
    uint64_t mix=(uint64_t)id*0x9E3779B97F4A7C15ULL;
    unsigned h=(unsigned)(mix>>40)&(INTERP_MAP-1);
    InterpId *slot=NULL,*free_slot=NULL;
    for(unsigned i=0;i<8&&!slot;++i){InterpId *e=&interp_ids[(h+i)&(INTERP_MAP-1)];
        if(e->serial&&e->id==id)slot=e;
        else if(!free_slot&&(!e->serial||interp_pairs[e->pair].serial!=e->serial))free_slot=e;}
    if(!slot||interp_pairs[slot->pair].serial!=slot->serial){
        if(!slot){slot=free_slot?free_slot:&interp_ids[h];if(!free_slot)InterpEvict(IEVICT_IDS);}
        if(!interp_serial)interp_target_stale=true;
        /* a new identity takes the next pair that no identity drew in the
         * last two frames, so the live ones keep their history */
        unsigned serial,index;
        for(unsigned tries=0;;++tries){
            serial=++interp_serial;if(!serial)serial=++interp_serial;
            index=serial&(INTERP_PAIRS-1);
            const InterpPair *old=&interp_pairs[index];
            if(!old->serial||!old->tagged||!old->frame||old->frame+1<interp_frame)break;
            if(tries>=64){InterpEvict(IEVICT_PAIRS);break;}
        }
        interp_pairs[index]=(InterpPair){.serial=serial,.tagged=true,.id=id};
        *slot=(InterpId){id,index,serial};
    }
    InterpMap(interp_tags,packet,slot->pair,slot->serial);
    InterpAddress *t=InterpFind(interp_tags,packet);
    /* drawn after the next BeginInterpolationFrame; the flags are the
     * packet's (one identity may be drawn twice, e.g. a tile listed twice) */
    if(t){t->frame=interp_frame+1;t->flags=flags;}
    interp_pairs[slot->pair].tagged_frame=interp_frame+1;
}
void Psyz_GpuCopyInterpolationPacket(const void *from,const void *to) {
    if(!interpolation_enabled)return;
    InterpAddress *t=InterpFindTagged(from);
    if(t){const unsigned flags=t->flags;InterpMap(interp_queue,to,t->pair,t->serial);
        InterpAddress *q=InterpFind(interp_queue,to);if(q)q->flags=flags;return;}
    InterpAddress *e=InterpFind(interp_addresses,from);
    if(e && interp_pairs[e->pair].serial==e->serial)
        InterpMap(interp_queue,to,e->pair,e->serial);
    else {e=InterpFind(interp_queue,to);if(e)e->serial=0;}
}
void Psyz_GpuResetInterpolation(void) {
    for(unsigned i=0;i<INTERP_PAIRS;++i)interp_pairs[i].frame=0;
    memset(interp_queue,0,sizeof(interp_queue));
    interp_pair_ready=false;interp_next_y=-1;interp_phase=0;interp_present_mid=false;interp_matches=0;
    interp_camera_frame=0;interp_camera_previous=false;
}
void Psyz_GpuClearInterpolationPackets(void) {
    Psyz_GpuResetInterpolation();
    memset(interp_addresses,0,sizeof(interp_addresses));
    memset(interp_pairs,0,sizeof(interp_pairs));
    memset(interp_tags,0,sizeof(interp_tags));
    memset(interp_ids,0,sizeof(interp_ids));
}
int Psyz_GpuSetFrameInterpolation(int enabled) {
    interpolation_requested=enabled!=0;
    static int logged_requested=-1,logged_effective=-1;
    if(interpolation_enabled!=(enabled!=0)){Draw_FlushBuffer();InterpMidpointSync(INTERP_SYNC_SETTINGS);interpolation_enabled=enabled!=0;Psyz_GpuResetInterpolation();}
    if(interpolation_enabled&&device&&!InterpEnsureTarget()){
        interpolation_enabled=false;Psyz_GpuResetInterpolation();
        WARNF("60 Hz interpolation admission failed; staying at original cadence: %s",SDL_GetError());
    }
    if(!interpolation_enabled&&device&&SubmitCmdAndWait()&&SDL_WaitForGPUIdle(device)){
        if(interp_unwitnessed){SDL_ReleaseGPUTexture(device,interp_unwitnessed);interp_unwitnessed=NULL;}
        if(interpolated_vram){SDL_ReleaseGPUTexture(device,interpolated_vram);interpolated_vram=NULL;}
        interpolated_scale=0;
    }
    Psyz_PgxpSetActive(!headless_renderer&&(set_geometry_correction||set_effect_geometry_correction||set_model_filtering||interpolation_enabled));
    Psyz_PgxpSetCulling(set_geometry_correction);
    Psyz_PgxpSetEffectEnabled(set_effect_geometry_correction);
    if(logged_requested!=(enabled!=0)||logged_effective!=interpolation_enabled){
        INFOF("60 Hz presentation requested=%d effective=%d scale=%u target=%s simulation=original rendered=unobserved",enabled!=0,interpolation_enabled,internal_res,interpolated_vram?"admitted":"none");
        logged_requested=enabled!=0;logged_effective=interpolation_enabled;
    }
    return 0;
}
int Psyz_GpuGetFrameInterpolationRequested(void){return interpolation_requested;}
int Psyz_GpuGetFrameInterpolation(void){return interpolation_enabled;}
unsigned long long Psyz_GpuGetInterpolatedPresentCount(void){return interpolated_present_count;}
/* Admission is transactional: failed allocation/copy leaves normal VRAM intact
 * and reports original cadence through the actual getter. No resolution bump. */
static bool InterpEnsureTarget(void) {
    if(!device||UseSoftwareBackend())return false;
    if(interpolated_vram&&interpolated_scale==internal_res)return true;
    InterpMidpointSync(INTERP_SYNC_SETTINGS);
    if(!SubmitCmdAndWait()||!SDL_WaitForGPUIdle(device))return false;
    if(interp_unwitnessed){SDL_ReleaseGPUTexture(device,interp_unwitnessed);interp_unwitnessed=NULL;}
    const SDL_GPUTextureCreateInfo info={.type=SDL_GPU_TEXTURETYPE_2D,
        .format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width=VRAM_W*internal_res,.height=VRAM_H*internal_res,
        .layer_count_or_depth=1,.num_levels=1};
    SDL_GPUTexture *candidate=SDL_CreateGPUTexture(device,&info);
    if(!candidate)return false;
    SDL_GPUCommandBuffer *cmd=AcquireCmd();
    SDL_GPUCopyPass *copy=cmd?SDL_BeginGPUCopyPass(cmd):NULL;
    if(!copy){SDL_ReleaseGPUTexture(device,candidate);return false;}
    const SDL_GPUTextureLocation from={.texture=GetRenderTarget()},to={.texture=candidate};
    SDL_CopyGPUTextureToTexture(copy,&from,&to,VRAM_W*internal_res,VRAM_H*internal_res,1,false);
    SDL_EndGPUCopyPass(copy);
    if(!SubmitCmdAndWait()){interp_unwitnessed=candidate;return false;}
    if(interpolated_vram)SDL_ReleaseGPUTexture(device,interpolated_vram);
    interpolated_vram=candidate;interpolated_scale=internal_res;
    Psyz_GpuResetInterpolation();return true;
}
/* Primitives of the frames drawn: all, tagged with an identity, paired. */
static unsigned interp_stat[3],interp_stat_last[3];
void Psyz_GpuGetInterpolationStats(unsigned *primitives,unsigned *tagged,unsigned *paired) {
    if(primitives)*primitives=interp_stat_last[0];
    if(tagged)*tagged=interp_stat_last[1];
    if(paired)*paired=interp_stat_last[2];
}
static const char *const interp_evict_names[IEVICT_N]={"addresses","tags","queue","ids","pairs","store","order",
    "field","edges","edge-refs","pending","junctions","lines"};
static void InterpEvict(unsigned table) {
    static bool warned[IEVICT_N];
    ++interp_evict[table];
    if(!warned[table]){warned[table]=true;
        WARNF("interpolation table %s full at frame %u: entries dropped (PSYZ_INTERP_STATS counts them per frame)",interp_evict_names[table],interp_frame);}
}
/* Batches, midpoint draws and table overflows of the frame (appended to
 * the [interp-stats] line): vertices and indices queued, the largest batch,
 * batches drawn because full (vertices, marks, CLUT rows), for feedback,
 * for a state or VRAM change and at a present or frame end, midpoint draws
 * and the splits among them that placed ghosts, new faces, T-junctions or
 * outlines before the frame was complete, and entries dropped by table. */
static void InterpStatsBatches(char *out,size_t size) {
    const DrawFrameStats *d=&draw_frame_stats;
    unsigned syncs=0,evicted=0;
    for(unsigned i=0;i<INTERP_SYNC_N;++i)syncs+=d->syncs[i];
    for(unsigned i=0;i<IEVICT_N;++i)evicted+=interp_evict[i];
    int n=SDL_snprintf(out,size," vertices=%u indices=%u batch=%u/%u overflow=%u marks=%u clut=%u feedback=%u state=%u present=%u syncs=%u split=%u/%u/%u/%u/%u/%u splitplaced=%u clutloads=%u evicted=%u",
        d->vertices,d->indices,d->batch_vertices,d->batch_indices,d->flushes[DRAW_FLUSH_OVERFLOW],d->flushes[DRAW_FLUSH_MARKS],
        d->flushes[DRAW_FLUSH_CLUT],d->flushes[DRAW_FLUSH_FEEDBACK],d->flushes[DRAW_FLUSH_STATE],d->flushes[DRAW_FLUSH_PRESENT],syncs,
        d->syncs[INTERP_SYNC_FULL],d->syncs[INTERP_SYNC_TEXTURE],d->syncs[INTERP_SYNC_CLUT],d->syncs[INTERP_SYNC_TARGET],
        d->syncs[INTERP_SYNC_PRESENT],d->syncs[INTERP_SYNC_SETTINGS],d->split_pending,d->clut_loads,evicted);
    for(unsigned i=0;i<IEVICT_N&&n>0&&(size_t)n<size;++i)
        if(interp_evict[i])n+=SDL_snprintf(out+n,size-(size_t)n," %s=%u",interp_evict_names[i],interp_evict[i]);
}
static void InterpStatsFrame(void) {
    if(interp_stats_log<0){const char *e=SDL_getenv("PSYZ_INTERP_STATS");interp_stats_log=e&&*e&&*e!='0';}
    if(interp_stats_log<=0||!interp_frame){
        memset(&draw_frame_stats,0,sizeof(draw_frame_stats));memset(interp_evict,0,sizeof(interp_evict));return;}
    /* identities drawn the frame before the last one and not in it */
    for(unsigned i=0;i<INTERP_PAIRS;++i){InterpPair *p=&interp_pairs[i];
        if(p->serial&&p->tagged&&p->frame+1==interp_frame&&!p->midpoint_pose){
            interp_area[ISTAT_VANISHED]+=InterpArea(p->position,p->count);++interp_count[ISTAT_VANISHED];}}
    const unsigned exposed=InterpExposed();
    char batches[512];InterpStatsBatches(batches,sizeof(batches));
    SDL_Log("[interp-stats] f=%u paired=%u/%.0f new=%u/%.0f mismatch=%u/%.0f frameonly=%u/%.0f midonly=%u/%.0f vanished=%u/%.0f ghost=%u/%.0f replaced=%u/%.0f cut=%d exposed=%u holes=%u%s",interp_frame,
        interp_count[0],interp_area[0],interp_count[1],interp_area[1],interp_count[2],interp_area[2],interp_count[3],interp_area[3],
        interp_count[4],interp_area[4],interp_count[5],interp_area[5],interp_count[6],interp_area[6],interp_count[7],interp_area[7],interp_ghost_cut,exposed,interp_exposed_holes,batches);
    memset(interp_area,0,sizeof(interp_area));memset(interp_count,0,sizeof(interp_count));
    memset(&draw_frame_stats,0,sizeof(draw_frame_stats));memset(interp_evict,0,sizeof(interp_evict));
}
void Psyz_GpuBeginInterpolationFrame(void) {
    InterpMidpointComplete(INTERP_SYNC_FRAME);
    InterpStatsFrame();
    interp_matches=0;
    memcpy(interp_stat_last,interp_stat,sizeof(interp_stat));memset(interp_stat,0,sizeof(interp_stat));
    ++interp_frame;if(!interp_frame){Psyz_GpuResetInterpolation();interp_frame=1;}
    interp_store_len[interp_frame&1]=interp_order_len[interp_frame&1]=0;
    InterpBuildGhosts();
    if(!interpolation_enabled||!device||UseSoftwareBackend())return;
    if(!InterpEnsureTarget()){
        WARNF("60 Hz interpolation target unavailable; original 30 Hz presentation retained: %s",SDL_GetError());
        interpolation_enabled=false;Psyz_GpuResetInterpolation();
        Psyz_PgxpSetActive(!headless_renderer&&(set_geometry_correction||set_effect_geometry_correction||set_model_filtering));
    }
    else if(interp_target_stale){
        Draw_FlushBuffer();
        SDL_GPUCommandBuffer *cmd=AcquireCmd();
        SDL_GPUCopyPass *copy=cmd?SDL_BeginGPUCopyPass(cmd):NULL;
        if(copy){
            const SDL_GPUTextureLocation from={.texture=GetRenderTarget()},to={.texture=interpolated_vram};
            SDL_CopyGPUTextureToTexture(copy,&from,&to,VRAM_W*internal_res,VRAM_H*internal_res,1,false);
            SDL_EndGPUCopyPass(copy);
            interp_target_stale=false;
        }
    }
}
/* Psyz_GpuInterpolationBackdrop of the frame being drawn: a background
 * fill of its rows (the game's DrawEnv clear) is followed by the copy. */
static struct { int x, src_y, dst_y, w, h; unsigned frame; } interp_backdrop;
static void InterpBackdropCopy(int x,int y,int w,int h) {
    if(InterpMidpointCovers(x,y,w,h))InterpMidpointSync(INTERP_SYNC_TARGET);
    SDL_GPUCommandBuffer *cmd=AcquireCmd();
    SDL_GPUCopyPass *copy=cmd?SDL_BeginGPUCopyPass(cmd):NULL;
    if(!copy)return;
    const unsigned r=internal_res;
    const int src_y=interp_backdrop.src_y+(y-interp_backdrop.dst_y);
    const SDL_GPUTextureLocation from={.texture=GetRenderTarget(),.x=(Uint32)x*r,.y=(Uint32)src_y*r},
                                 to={.texture=interpolated_vram,.x=(Uint32)x*r,.y=(Uint32)y*r};
    SDL_CopyGPUTextureToTexture(copy,&from,&to,(Uint32)w*r,(Uint32)h*r,1,false);
    SDL_EndGPUCopyPass(copy);
}
static void InterpBackdropAfterFill(int x,int y,int w,int h) {
    if(!interp_backdrop.frame||interp_backdrop.frame!=interp_frame||!interpolated_vram||
       interpolated_scale!=internal_res||!interpolation_enabled)return;
    const int x0=SDL_max(x,interp_backdrop.x),x1=SDL_min(x+w,interp_backdrop.x+interp_backdrop.w);
    const int y0=SDL_max(y,interp_backdrop.dst_y),y1=SDL_min(y+h,interp_backdrop.dst_y+interp_backdrop.h);
    if(x0<x1&&y0<y1)InterpBackdropCopy(x0,y0,x1-x0,y1-y0);
}
void Psyz_GpuInterpolationBackdrop(int x,int src_y,int dst_y,int w,int h) {
    interp_backdrop.frame=0;

    if(!interpolation_enabled||!device||UseSoftwareBackend()||!interpolated_vram||
       interpolated_scale!=internal_res||!interp_serial||interp_target_stale)return;
    if(x<0){w+=x;x=0;}
    if(x+w>VRAM_W)w=VRAM_W-x;
    if(w<=0||h<=0||src_y<0||dst_y<0||src_y+h>VRAM_H||dst_y+h>VRAM_H||src_y==dst_y)return;
    Draw_FlushBuffer();
    interp_backdrop.x=x;interp_backdrop.src_y=src_y;interp_backdrop.dst_y=dst_y;
    interp_backdrop.w=w;interp_backdrop.h=h;interp_backdrop.frame=interp_frame;
    interp_expose_rect[interp_frame&1].x=x;interp_expose_rect[interp_frame&1].y=dst_y;
    interp_expose_rect[interp_frame&1].w=w;interp_expose_rect[interp_frame&1].h=h;interp_expose_rect[interp_frame&1].frame=interp_frame;
    InterpBackdropCopy(x,dst_y,w,h);
}
void Psyz_GpuSetInterpolationCamera(int x,int y) {
    interp_camera_previous=interp_camera_frame&&interp_camera_frame+1==interp_frame;
    int dx=x-interp_camera_x,dy=y-interp_camera_y;
    if(interp_camera_previous&&(abs(dx)>32||abs(dy)>32)){Psyz_GpuResetInterpolation();interp_camera_previous=false;}
    interp_camera_half_x=interp_camera_previous?dx*0.5f:0;
    interp_camera_half_y=interp_camera_previous?dy*0.5f:0;
    interp_camera_x=x;interp_camera_y=y;interp_camera_frame=interp_frame;
}
/* The midpoint of the frame just drawn is no plausible in-between: a cut
 * (most of the frame's identities are new), much of the picture is
 * geometry without a pose in the frame before, which the midpoint can only
 * draw where this frame has it, or it shows what neither frame shows (a
 * hole or the background through a gap, InterpExposed). The frame before
 * stays on screen for that field instead. PSYZ_INTERP_HOLD=share of the
 * picture (default 0.15), PSYZ_INTERP_EXPOSE=cells (default 8, 0: off). */
static Posi display_size;
static bool InterpMidpointUnsound(void) {
    static float share=-1;
    if(share<0){const char *e=SDL_getenv("PSYZ_INTERP_HOLD");share=e&&*e?(float)SDL_atof(e):0.15f;}
    static int expose=-1;
    if(expose<0){const char *e=SDL_getenv("PSYZ_INTERP_EXPOSE");expose=e&&*e?SDL_atoi(e):8;}
    if(interp_ghost_cut)return true;
    if(expose>0&&InterpExposed()>=(unsigned)expose)return true;
    const double picture=(double)display_size.x*display_size.y;
    const double posed=interp_area[ISTAT_PAIRED]+interp_area[ISTAT_GHOST]+interp_area[ISTAT_REPLACED];
    const double unposed=interp_area[ISTAT_NEW]+interp_area[ISTAT_MISMATCH];
    return unposed>picture*share||(posed+unposed>0&&posed<unposed);
}
void Psyz_GpuPrepareInterpolatedPresent(int next_y) {
    /* the frame is complete: its midpoint is placed and drawn */
    InterpMidpointComplete(INTERP_SYNC_FRAME);
    interp_next_y=next_y;interp_phase=0;interp_blank_mid=false;
    interp_pair_ready=interpolation_enabled&&interpolated_vram&&interp_matches>0&&next_y>=0&&next_y<VRAM_H;
}
void Psyz_GpuPrepareInterpolatedBlank(int next_y) {
    Psyz_GpuPrepareInterpolatedPresent(next_y);
    interp_blank_mid=interp_pair_ready;
}
/* Drawing the runs of a line (Draw_PushPrim): see InterpCapture. */
static int interp_line_mode;
#define INTERP_LINE_POINTS 6
/* Outlines (lines along a tile's edges) are drawn at whole pixels next to
 * the projection of the polygons they outline, and behind them, so those
 * are queued after the line. A tagged line is drawn into the midpoint as a
 * quad per segment and placed at the flush (InterpFinishLines): its points
 * go where the polygon vertices they lie at (within INTERP_LINE_REACH
 * pixels) went, found by the frame before's pose for a paired line and by
 * this frame's for a new one (an outline's identity counts its place in
 * the tile, which shifts as outlines are left out). */
#define INTERP_LINE_REACH 2.0f
#define INTERP_LINE_PENDING 4096u
typedef struct {
    unsigned start, end;
    unsigned char which, count;
    short ox, oy;
    float from[INTERP_LINE_POINTS][2], want[INTERP_LINE_POINTS][2], at[INTERP_LINE_POINTS][2];
} InterpLinePending;
static InterpLinePending interp_line_pending[INTERP_LINE_PENDING];
static unsigned interp_line_pending_len;
/* the entry of the line being drawn, or -1 */
static int interp_line_pend = -1;
/* A line packet tagged for interpolation: records its points and, when the
 * previous frame drew the same identity with as many points, returns the
 * midpoint points (VRAM pixels relative to the drawing offset). A midpoint-
 * only line (*own false) is not drawn into the frame. */
static bool InterpLine(const u_long *words,int count,bool gouraud,int *mid_x,int *mid_y,bool *own,bool *own_only) {
    *own=true;*own_only=false;
    if(!interpolation_enabled||!interpolated_vram||interpolated_scale!=internal_res||
       count<2||count>INTERP_LINE_POINTS)return false;
    const void *packet=interpolation_packet_current;
    ++interp_stat[0];
    InterpAddress *e=InterpFind(interp_queue,packet);
    if(!e)e=InterpFindTagged(packet);
    if(!e)e=InterpFind(interp_addresses,packet);
    if(!e||!e->serial)return false;
    InterpPair *p=&interp_pairs[e->pair];if(p->serial!=e->serial||!p->tagged)return false;
    ++interp_stat[1];
    *own=!(e->flags&PSYZ_INTERP_MIDPOINT_ONLY);*own_only=(e->flags&PSYZ_INTERP_FRAME_ONLY)!=0;
    float position[INTERP_LINE_POINTS][3];
    const unsigned points=(unsigned)count|0x100u; /* never pairs with a polygon */
    bool valid=p->frame&&p->frame+1==interp_frame&&p->count==points;
    for(int i=0,at=0;i<count;i++){
        if(i&&gouraud)at++;
        position[i][0]=(float)s11(((const short*)&words[at])[0]);
        position[i][1]=(float)s11(((const short*)&words[at])[1]);
        position[i][2]=1.0f;
        at++;
        if(SDL_fabsf(position[i][0]-p->position[i][0])>160.0f||SDL_fabsf(position[i][1]-p->position[i][1])>160.0f)valid=false;
    }
    /* A midpoint-only line pairs as a polygon does (see InterpCapture);
     * one that cannot continues the last motion of a line the game drew in
     * the frame before. */
    if(!*own&&(p->midpoint_pose||(e->flags&PSYZ_INTERP_NO_POSE)))valid=false;
    const bool extrapolated=!valid&&!*own&&p->frame&&p->frame+1==interp_frame&&p->count==points&&
        !p->midpoint_pose&&p->motion_frame==p->frame;
    /* a new line where ghosts go: the outline of the other representation
     * of what the ghost draws (InterpGhostCovers) */
    if(!valid&&!extrapolated&&*own&&!(p->frame&&p->frame+1==interp_frame)&&InterpGhostCoversLine(position,count))*own_only=true;
    /* a new line (or one whose points changed) is placed as its neighbours
     * are, from this frame's pose (InterpFinishLines) */
    const bool newcomer=!valid&&!extrapolated&&*own&&!*own_only;
    interp_line_pend=-1;
    if((valid||newcomer)&&interp_line_pending_len>=INTERP_LINE_PENDING)InterpEvict(IEVICT_LINES);
    if((valid||newcomer)&&interp_line_pending_len<INTERP_LINE_PENDING){
        InterpLinePending *q=&interp_line_pending[interp_line_pending_len];
        q->which=valid?0:1;q->count=(unsigned char)count;q->ox=(short)InterpOffsetX();q->oy=(short)InterpOffsetY();q->start=q->end=0;
        for(int i=0;i<count;i++){
            q->from[i][0]=valid?p->position[i][0]:position[i][0];q->from[i][1]=valid?p->position[i][1]:position[i][1];
            q->want[i][0]=valid?(position[i][0]+p->position[i][0])*0.5f:position[i][0];
            q->want[i][1]=valid?(position[i][1]+p->position[i][1])*0.5f:position[i][1];}
        interp_line_pend=(int)interp_line_pending_len;
    }
    for(int i=0;i<count;i++){
        if(valid){mid_x[i]=(int)SDL_floorf((position[i][0]+p->position[i][0])*0.5f+0.5f);
                  mid_y[i]=(int)SDL_floorf((position[i][1]+p->position[i][1])*0.5f+0.5f);}
        else if(extrapolated){mid_x[i]=(int)SDL_floorf(p->position[i][0]+p->motion[i][0]*0.5f+0.5f);
                  mid_y[i]=(int)SDL_floorf(p->position[i][1]+p->motion[i][1]*0.5f+0.5f);}
        else{mid_x[i]=(int)position[i][0];mid_y[i]=(int)position[i][1];}
        if(interp_line_pend>=0){interp_line_pending[interp_line_pend].at[i][0]=(float)mid_x[i];interp_line_pending[interp_line_pend].at[i][1]=(float)mid_y[i];}
    }
    if(valid&&*own){for(int i=0;i<count;i++){p->motion[i][0]=position[i][0]-p->position[i][0];p->motion[i][1]=position[i][1]-p->position[i][1];}
        p->motion_frame=interp_frame;}
    else p->motion_frame=0;
    memcpy(p->position,position,(size_t)count*sizeof(position[0]));p->frame=interp_frame;p->count=points;p->signature=0;
    p->midpoint_pose=!*own;
    if(valid||extrapolated){++interp_stat[2];++interp_matches;}
    return (valid||extrapolated||newcomer)&&!*own_only;
}
static void InterpProbe(const void *packet,const Vertex *v,const Vertex *mid,unsigned count,const char *what);
static void InterpCaptureDraw(const void *packet,Vertex *v,unsigned count);
static const char *interp_probe_what;
static unsigned interp_probe_pair,interp_probe_flags;
static void InterpCapture(const void *packet,Vertex *v,unsigned count,const u32 *index,unsigned indices) {
    static int probe=-1;
    interp_capture_index=index;interp_capture_indices=indices;
    if(probe<0)probe=SDL_getenv("PSYZ_INTERP_PROBE")!=NULL;
    interp_probe_what="untracked";interp_probe_pair=0;interp_probe_flags=0;
    interp_capture_pending=false;
    InterpCaptureDraw(packet,v,count);
    interp_size_culled=0;
    if(interpolation_enabled&&!interp_line_mode&&count<=6&&v>=vertex_buf&&v+count<=vertex_buf+MAX_VERTEX_COUNT){
        const unsigned base=(unsigned)(v-vertex_buf);
        InterpExposeView view;InterpExposeViewNow(&view);
        InterpExposePrimitive(0,&view,v,&vertex_depth[base],index,indices,base,(geometry_correction||effect_geometry_correction)&&internal_res>1);
        if(!interp_capture_pending)InterpExposePrimitive(2,&view,interpolated_vertices+base,&interp_mid_depth[base],index,indices,base,true);
    }
    if(probe&&!interp_line_mode&&count<=6)InterpProbe(packet,v,interpolated_vertices+(v-vertex_buf),count,interp_probe_what);
}
/* A screen position the GTE did not clamp to its limit (a projection). */
static bool InterpUnclamped(const float *pos) {
    return pos[0]>-1023.0f&&pos[0]<1022.0f&&pos[1]>-1023.0f&&pos[1]<1022.0f;
}
static void InterpCaptureDraw(const void *packet,Vertex *v,unsigned count) {
    unsigned offset=(unsigned)(v-vertex_buf);
    memcpy(interpolated_vertices+offset,v,count*sizeof(*v));
    for(unsigned i=0;i<count&&offset+i<MAX_VERTEX_COUNT;++i)interp_mid_depth[offset+i]=vertex_depth[offset+i];
    for(unsigned i=0;i<count;++i)if(!((geometry_correction||effect_geometry_correction)&&internal_res>1&&(v[i].t&TPAGE_POLYGON)&&v[i].pgxp_pos[2]>0.0f))interpolated_vertices[offset+i].pgxp_pos[2]=-1;
    if(interp_line_mode){
        /* runs of a line: 1 its own pose only, 2 the midpoint only, 3 both */
        if(interp_line_mode!=3)for(unsigned i=0;i<count;++i){
            Vertex *hide=interp_line_mode==1?&interpolated_vertices[offset+i]:&v[i];
            hide->x=hide->y=-2048;hide->pgxp_pos[0]=hide->pgxp_pos[1]=-2048.0f;hide->pgxp_pos[2]=-1.0f;}
        return;
    }
    if(!interpolation_enabled||(!interpolated_vram&&interpolated_scale==internal_res)||count>6)return;
    ++interp_stat[0];
    /* PSYZ_INTERP_DEBUG=1 tints the midpoint's unpaired tagged primitives
     * magenta, =2 also the untagged ones green. */
    static int debug=-1;
    if(debug<0){const char *d=SDL_getenv("PSYZ_INTERP_DEBUG");debug=d?SDL_atoi(d):0;}
    InterpAddress *e=InterpFind(interp_queue,packet);
    if(!e)e=InterpFindTagged(packet);
    if(!e)e=InterpFind(interp_addresses,packet);
    if(debug>1&&(!e||!e->serial||interp_pairs[e->pair].serial!=e->serial))
        for(unsigned i=0;i<count;++i){Vertex *out=interpolated_vertices+offset+i;out->r=0;out->g=255;out->b=0;}
    if(!e||!e->serial)return;
    InterpPair *p=&interp_pairs[e->pair];if(p->serial!=e->serial)return;
    const unsigned flags=p->tagged?e->flags:0;
    const bool any_texture=(flags&PSYZ_INTERP_ANY_TEXTURE)!=0;
    const bool midpoint_only=(flags&PSYZ_INTERP_MIDPOINT_ONLY)!=0;
    bool frame_only=(flags&PSYZ_INTERP_FRAME_ONLY)!=0;
    uint64_t signature=14695981039346656037ULL;
    /* texture coordinates, CLUT and page of a textured primitive (an
     * untextured one leaves them as whatever was drawn before it) */
    for(unsigned i=0;i<count;++i){const unsigned char *s=(const void *)&v[i].u;
        if(!(v[i].t&TPAGE_NOTEXTURE))for(unsigned j=0;j<8;++j)signature=(signature^s[j])*1099511628211ULL;}
    float position[6][3],depth[6];
    for(unsigned i=0;i<count;++i){const float d=vertex_depth[offset+i];depth[i]=d>0.0f?d:0.0f;}
    for(unsigned i=0;i<count;++i){bool projected=(geometry_correction||effect_geometry_correction)&&internal_res>1&&(v[i].t&TPAGE_POLYGON)&&v[i].pgxp_pos[2]>0;
        float bias=(v[i].t&TPAGE_POLYGON)&&internal_res==1?0.5f:0.0f;
        position[i][0]=projected?v[i].pgxp_pos[0]:(float)v[i].x+bias;
        position[i][1]=projected?v[i].pgxp_pos[1]:(float)v[i].y+bias;
        position[i][2]=projected?v[i].pgxp_pos[2]:1;}
    /* A second packet of an identity in the same frame (the game drew the
     * same face twice): drawn as the first one was. */
    if(p->tagged&&p->frame==interp_frame&&p->count==count&&!memcmp(p->position,position,count*sizeof(position[0]))){
        for(unsigned i=0;i<count;++i){Vertex *out=interpolated_vertices+offset+i;
            if(p->out_mode==1&&!frame_only){out->pgxp_pos[0]=p->out[i][0];out->pgxp_pos[1]=p->out[i][1];out->pgxp_pos[2]=p->out[i][2];}
            else if(frame_only){out->x=out->y=-2048;out->pgxp_pos[0]=out->pgxp_pos[1]=-2048.0f;out->pgxp_pos[2]=-1.0f;}}
        if(midpoint_only)for(unsigned i=0;i<count;++i){v[i].x=v[i].y=-2048;v[i].pgxp_pos[0]=v[i].pgxp_pos[1]=-2048.0f;v[i].pgxp_pos[2]=-1.0f;}
        if(p->ghost_frame==interp_frame&&p->ghost_head)interp_inject=e->pair+1;
        return;
    }
    bool valid=p->frame&&p->frame+1==interp_frame&&p->count==count&&(any_texture||p->signature==signature);
    /* a new identity where a ghost goes: the other representation of it */
    const bool replaced=p->tagged&&!valid&&!midpoint_only&&!frame_only&&!(p->frame&&p->frame+1==interp_frame)&&
        InterpGhostCovers(position,count);
    if(replaced)frame_only=true;
    /* A midpoint-only packet is geometry the game leaves out of the frame
     * (nearer than the projection plane): it pairs only with a pose the
     * game drew in the frame before; two midpoint-only poses are no
     * projections the game would draw (often far off the view). With
     * PSYZ_INTERP_NO_POSE its pose this frame is none either: the midpoint
     * continues the last motion of the drawn pose instead (below). */
    if(midpoint_only&&(p->midpoint_pose||(flags&PSYZ_INTERP_NO_POSE)))valid=false;
    for(unsigned i=0;i<count;++i){
        /* a tagged identity is the same face: it may move as far as the
         * view reaches (vertices past the GTE's limit are handled below) */
        const float limit=p->tagged?1024.0f:32.0f;
        if(SDL_fabsf(position[i][0]-p->position[i][0])>limit||SDL_fabsf(position[i][1]-p->position[i][1])>limit)valid=false;
    }
    /* An authored animation may switch to another tile entry this frame.
     * Keep that image intact, but move it with the same interpolated camera
     * as its static neighbours so patch boundaries cannot separate. */
    bool camera_fallback=!valid&&p->background&&interp_camera_previous;
    /* A tagged primitive with a vertex at the GTE's screen limit crosses the
     * view plane: that pose is not a projection. When it was paired the
     * frame before, the midpoint continues its last motion from there. A
     * midpoint-only one that does not pair is left out. */
    bool clamped=midpoint_only&&!valid;
    if(!valid&&!camera_fallback&&p->tagged)
        for(unsigned i=0;i<count;++i)clamped|=v[i].x<=-1024||v[i].x>=1023||v[i].y<=-1024||v[i].y>=1023;
    const bool extrapolated=clamped&&!p->midpoint_pose&&p->frame&&p->frame+1==interp_frame&&
        (p->motion_frame==p->frame||midpoint_only)&&p->count==count&&(any_texture||p->signature==signature);
    /* without a last motion (a level of detail drawn in the frame before
     * only), midpoint-only geometry stays where that frame drew it rather
     * than leaving a hole */
    const float step=p->motion_frame==p->frame?0.5f:0.0f;
    if(p->tagged)++interp_stat[1];
    if(valid||extrapolated)++interp_stat[2];
    interp_probe_pair=e->pair;interp_probe_flags=flags;
    if(p->tagged){
        const int k=replaced?ISTAT_REPLACED:frame_only?ISTAT_FRAME_ONLY:(valid||extrapolated)?ISTAT_PAIRED:midpoint_only?ISTAT_MID_ONLY:
            (p->frame&&p->frame+1==interp_frame)?ISTAT_MISMATCH:ISTAT_NEW;
        interp_area[k]+=InterpArea(position,count);++interp_count[k];
    }
    interp_probe_what=!p->tagged?"untagged-pair":valid?"paired":extrapolated?"extrapolated":camera_fallback?"camera":clamped&&midpoint_only?"hidden":"unpaired";
    if(debug>0&&!valid&&!extrapolated&&p->tagged)
        for(unsigned i=0;i<count;++i){Vertex *out=interpolated_vertices+offset+i;out->r=255;out->g=0;out->b=255;}
    if((valid||camera_fallback||extrapolated)&&!frame_only)++interp_matches;
    p->out_mode=0;
    for(unsigned i=0;i<count;++i){Vertex *out=interpolated_vertices+offset+i;
        if(valid&&depth[i]>0.0f&&p->depth[i]>0.0f&&InterpUnclamped(position[i])&&InterpUnclamped(p->position[i])){
            /* the projection of the midpoint of the two view-space
             * positions: their screen positions weighted by depth (the
             * projection centre cancels out); points on a line stay on
             * it, so subdivided pieces meet their neighbours */
            const float w=depth[i]/(depth[i]+p->depth[i]);
            out->pgxp_pos[0]=position[i][0]*w+p->position[i][0]*(1.0f-w);
            out->pgxp_pos[1]=position[i][1]*w+p->position[i][1]*(1.0f-w);
            out->pgxp_pos[2]=position[i][2]>1.0f&&p->position[i][2]>1.0f?(depth[i]+p->depth[i])*0.5f:
                2.0f/(1.0f/position[i][2]+1.0f/p->position[i][2]);
        }else if(valid){
            out->pgxp_pos[0]=(position[i][0]+p->position[i][0])*0.5f;
            out->pgxp_pos[1]=(position[i][1]+p->position[i][1])*0.5f;
            out->pgxp_pos[2]=2.0f/(1.0f/position[i][2]+1.0f/p->position[i][2]);
        }else if(camera_fallback){
            out->pgxp_pos[0]=position[i][0]+interp_camera_half_x;
            out->pgxp_pos[1]=position[i][1]+interp_camera_half_y;
            out->pgxp_pos[2]=position[i][2];
        }else if(extrapolated){
            out->pgxp_pos[0]=p->position[i][0]+p->motion[i][0]*step;
            out->pgxp_pos[1]=p->position[i][1]+p->motion[i][1]*step;
            out->pgxp_pos[2]=p->position[i][2];
        }else if(clamped&&midpoint_only){
            out->x=out->y=-2048;out->pgxp_pos[0]=out->pgxp_pos[1]=-2048.0f;out->pgxp_pos[2]=-1.0f;
        }
        if(valid&&depth[i]>0.0f&&p->depth[i]>0.0f)interp_mid_depth[offset+i]=(depth[i]+p->depth[i])*0.5f;
        else if(extrapolated||(valid&&p->depth[i]>0.0f))interp_mid_depth[offset+i]=p->depth[i];
        if(valid||extrapolated){p->out[i][0]=out->pgxp_pos[0];p->out[i][1]=out->pgxp_pos[1];p->out[i][2]=out->pgxp_pos[2];p->out_mode=1;}
    }
    unsigned field[2][6]={{0}};
    if(valid&&!frame_only)for(unsigned i=0;i<count;++i){
        field[0][i]=InterpFieldAdd(0,p->position[i],p->depth[i],InterpOffsetX(),InterpOffsetY(),interpolated_vertices[offset+i].pgxp_pos);
        field[1][i]=InterpFieldAdd(1,position[i],depth[i],InterpOffsetX(),InterpOffsetY(),interpolated_vertices[offset+i].pgxp_pos);}
    /* T-junction edges and vertices (InterpSnapJunctions) */
    if(valid&&!frame_only&&(count==3||count==6)){
        const int ox=InterpOffsetX(),oy=InterpOffsetY();
        /* the polygon's triangles (t: vertices 3t..3t+2), also one the GPU
         * skips for its size (its indices are collapsed) */
        for(unsigned t=0;t<count;t+=3)for(unsigned k=0;k<3;++k){
            const unsigned a=t+k,b=t+(k+1)%3;
            InterpAddEdge(p->position[a],p->position[b],position[a],position[b],interpolated_vertices[offset+a].pgxp_pos,
                          interpolated_vertices[offset+b].pgxp_pos,p->depth[a],p->depth[b],e->pair,ox,oy,!midpoint_only);
        }
        if(interp_junction_prims_len>=INTERP_JUNCTION_PRIMS)InterpEvict(IEVICT_JUNCTIONS);
        else{InterpJunctionPrim *q=&interp_junction_prims[interp_junction_prims_len++];
            q->pair=e->pair;q->base=interp_mid_len+offset;q->count=(unsigned char)count;q->ox=(short)ox;q->oy=(short)oy;q->posed=!midpoint_only;
            for(unsigned i=0;i<count;++i){q->p0[i][0]=p->position[i][0];q->p0[i][1]=p->position[i][1];
                q->p1[i][0]=position[i][0];q->p1[i][1]=position[i][1];q->d0[i]=p->depth[i];
                q->field[0][i]=field[0][i];q->field[1][i]=field[1][i];}}
    }
    /* A new identity, and one whose pose this frame is none (it continues
     * its last motion): placed as their neighbours are in the midpoint
     * (InterpFinishPending), from this frame's pose or the last one. */
    const bool newcomer=p->tagged&&!valid&&!extrapolated&&!camera_fallback&&!frame_only&&!midpoint_only&&
       !(p->frame&&p->frame+1==interp_frame);
    if((newcomer||(extrapolated&&!frame_only))&&interp_capture_indices<=6){
        bool usable=true;unsigned char index[6];
        for(unsigned i=0;i<count;++i)usable&=InterpUnclamped(newcomer?position[i]:p->position[i]);
        for(unsigned k=0;k<interp_capture_indices;++k){const unsigned i=(unsigned)interp_capture_index[k]-offset;
            usable&=i<count;index[k]=(unsigned char)i;}
        if(usable)interp_capture_pending=newcomer?
            InterpPend(1,e->pair,offset,count,(const float (*)[3])position,depth,index,interp_capture_indices,InterpOffsetX(),InterpOffsetY()):
            InterpPend(0,e->pair,offset,count,(const float (*)[3])p->position,p->depth,index,interp_capture_indices,InterpOffsetX(),InterpOffsetY());
    }
    /* A triangle the GPU skips for its size in this frame (it passes close
     * to the view) that the frame before drew: the midpoint still draws
     * it, rather than show what it covered there. (Triangle t of a polygon
     * has vertices 3t..3t+2.) */
    unsigned mid_only_triangles=0;
    static int size_culled=-1;
    if(size_culled<0){const char *e=SDL_getenv("PSYZ_INTERP_SIZE_CULLED");size_culled=!(e&&*e=='0');}
    if(size_culled&&interp_size_culled&&(valid||extrapolated)&&!frame_only&&(count==3||count==6))
        for(unsigned t=0;t<count/3;++t){
            if(!(interp_size_culled>>t&1))continue;
            float x0=1e9f,x1=-1e9f,y0=1e9f,y1=-1e9f;
            for(unsigned k=0;k<3;++k){const float *q=p->position[3*t+k];
                x0=SDL_min(x0,q[0]);x1=SDL_max(x1,q[0]);y0=SDL_min(y0,q[1]);y1=SDL_max(y1,q[1]);}
            if(x1-x0<(float)VRAM_W&&y1-y0<(float)VRAM_H)mid_only_triangles|=1u<<t;
        }
    if(valid&&!midpoint_only){
        for(unsigned i=0;i<count;++i){p->motion[i][0]=position[i][0]-p->position[i][0];p->motion[i][1]=position[i][1]-p->position[i][1];}
        p->motion_frame=interp_frame;
    }else p->motion_frame=0;
    memcpy(p->position,position,count*sizeof(position[0]));p->frame=interp_frame;p->count=count;p->signature=signature;
    memcpy(p->depth,depth,count*sizeof(depth[0]));
    p->midpoint_pose=midpoint_only;
    if(p->tagged&&!midpoint_only)InterpStore(p,e->pair,v,count);
    if(p->tagged&&p->ghost_frame==interp_frame&&p->ghost_head)interp_inject=e->pair+1;
    if(frame_only)for(unsigned i=0;i<count;++i){Vertex *out=interpolated_vertices+offset+i;out->x=out->y=-2048;out->pgxp_pos[0]=out->pgxp_pos[1]=-2048;out->pgxp_pos[2]=-1;}
    if(midpoint_only)for(unsigned i=0;i<count;++i){
        v[i].x=v[i].y=-2048;v[i].pgxp_pos[0]=v[i].pgxp_pos[1]=-2048.0f;v[i].pgxp_pos[2]=-1.0f;}
    for(unsigned t=0;t<2;++t)if(mid_only_triangles>>t&1)
        for(unsigned k=0;k<3;++k){const unsigned i=3*t+k;
            index_cur[i]=(u32)(offset+i);
            v[i].x=v[i].y=-2048;v[i].pgxp_pos[0]=v[i].pgxp_pos[1]=-2048.0f;v[i].pgxp_pos[2]=-1.0f;}
}

/* Optional room-local atlas cache: bounded storage, no native VRAM changes. */
typedef struct SpriteAtlas {
    uint64_t key;
    unsigned columns, rows, first_cell;
} SpriteAtlas;
static SpriteAtlas sprite_atlases[512];
static unsigned sprite_atlas_count;
static size_t sprite_atlas_bytes;
static SpriteAtlas *next_sprite_atlas;
static SDL_GPUTexture *sprite_atlas_texture;
static unsigned sprite_atlas_cells;
#define HD_ATLAS_COLUMNS 240u
#define HD_ATLAS_ROWS 60u
#define HD_ATLAS_WIDTH (HD_ATLAS_COLUMNS*68u)
#define HD_ATLAS_HEIGHT (HD_ATLAS_ROWS*136u)
static unsigned next_sprite_cell;
static bool next_sprite_extension;
static bool sprite_atlases_enabled=true;
static SDL_GPUSampler *sprite_atlas_sampler;


static Posi display_area = {0, 0};
static Posi display_size = {256, 240};
static Posi cur_display_size = {-1, -1};
static Posi draw_offset = {0, 0};
static Posi draw_area_start = {0, 0};
static Posi draw_area_end = {0x10000, 0x10000};
static int InterpOffsetX(void){return draw_offset.x;}
static int InterpOffsetY(void){return draw_offset.y;}
/* The on-screen area of a primitive's pose (vertex space), clipped to the
 * drawing area's bounding box by clamping. */
static float InterpArea(float (*pos)[3],unsigned count) {
    static const unsigned tri[3][2][3]={{{0,1,2},{0,1,2}},{{0,1,2},{2,1,3}},{{0,1,2},{3,4,5}}};
    if(count!=3&&count!=4&&count!=6)return 0;
    const unsigned k=count==3?0:count==4?1:2;
    const float x0=(float)(draw_area_start.x-draw_offset.x),x1=(float)(draw_area_end.x+1-draw_offset.x);
    const float y0=(float)(draw_area_start.y-draw_offset.y),y1=(float)(draw_area_end.y+1-draw_offset.y);
    float area=0;
    for(unsigned t=0;t<(count==3?1u:2u);++t){float q[3][2];
        for(unsigned i=0;i<3;++i){q[i][0]=SDL_clamp(pos[tri[k][t][i]][0],x0,x1);q[i][1]=SDL_clamp(pos[tri[k][t][i]][1],y0,y1);}
        area+=SDL_fabsf((q[1][0]-q[0][0])*(q[2][1]-q[0][1])-(q[2][0]-q[0][0])*(q[1][1]-q[0][1]))*0.5f;}
    return area;
}
static bool InterpGhostEnabled(void) {
    if(interp_ghosts<0){const char *e=SDL_getenv("PSYZ_INTERP_GHOSTS");interp_ghosts=!(e&&*e=='0');}
    return interp_ghosts>0;
}
static unsigned short InterpCoverStamp(void){return (unsigned short)(interp_frame&0xFFFF)?(unsigned short)(interp_frame&0xFFFF):1;}
/* Marks the cells whose centre a triangle (drawing area coordinates) covers. */
static void InterpCoverTriangle(const float a[2],const float b[2],const float c[2]) {
    const float area=(b[0]-a[0])*(c[1]-a[1])-(c[0]-a[0])*(b[1]-a[1]);
    if(SDL_fabsf(area)<1.0f)return;
    const int x0=SDL_max(0,(int)SDL_floorf(SDL_min(a[0],SDL_min(b[0],c[0]))/INTERP_COVER_CELL));
    const int x1=SDL_min(VRAM_W/INTERP_COVER_CELL-1,(int)SDL_floorf(SDL_max(a[0],SDL_max(b[0],c[0]))/INTERP_COVER_CELL));
    const int y0=SDL_max(0,(int)SDL_floorf(SDL_min(a[1],SDL_min(b[1],c[1]))/INTERP_COVER_CELL));
    const int y1=SDL_min(VRAM_H/INTERP_COVER_CELL-1,(int)SDL_floorf(SDL_max(a[1],SDL_max(b[1],c[1]))/INTERP_COVER_CELL));
    const unsigned short stamp=InterpCoverStamp();
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
        const float px=(x+0.5f)*INTERP_COVER_CELL,py=(y+0.5f)*INTERP_COVER_CELL;
        const float d1=(b[0]-a[0])*(py-a[1])-(px-a[0])*(b[1]-a[1]);
        const float d2=(c[0]-b[0])*(py-b[1])-(px-b[0])*(c[1]-b[1]);
        const float d3=(a[0]-c[0])*(py-c[1])-(px-c[0])*(a[1]-c[1]);
        if(area>0?(d1>=0&&d2>=0&&d3>=0):(d1<=0&&d2<=0&&d3<=0))interp_ghost_cover[y][x]=stamp;
    }
}
static const unsigned interp_tri[3][2][3]={{{0,1,2},{0,1,2}},{{0,1,2},{2,1,3}},{{0,1,2},{3,4,5}}};
/* Most of a primitive's pose (vertex space) lies where ghosts go. */
static bool InterpGhostCovers(float (*pos)[3],unsigned count) {
    if(!InterpGhostEnabled()||interp_ghost_cut||(count!=3&&count!=4&&count!=6))return false;
    const unsigned k=count==3?0:count==4?1:2,tris=count==3?1u:2u;
    const unsigned short stamp=InterpCoverStamp();
    unsigned samples=0,covered=0;
    for(unsigned t=0;t<tris;++t){
        const float *a=pos[interp_tri[k][t][0]],*b=pos[interp_tri[k][t][1]],*c=pos[interp_tri[k][t][2]];
        static const float w[4][3]={{1/3.f,1/3.f,1/3.f},{.6f,.2f,.2f},{.2f,.6f,.2f},{.2f,.2f,.6f}};
        for(unsigned i=0;i<4;++i){
            const int x=(int)SDL_floorf((w[i][0]*a[0]+w[i][1]*b[0]+w[i][2]*c[0]+draw_offset.x-draw_area_start.x)/INTERP_COVER_CELL);
            const int y=(int)SDL_floorf((w[i][0]*a[1]+w[i][1]*b[1]+w[i][2]*c[1]+draw_offset.y-draw_area_start.y)/INTERP_COVER_CELL);
            ++samples;
            if(x>=0&&y>=0&&x<VRAM_W/INTERP_COVER_CELL&&y<VRAM_H/INTERP_COVER_CELL&&interp_ghost_cover[y][x]==stamp)++covered;
        }
    }
    return covered*4>=samples*3;
}
/* Most of a line's points and segment middles lie where ghosts go. */
static bool InterpGhostCoversLine(float (*pos)[3],int count) {
    if(!InterpGhostEnabled()||interp_ghost_cut||count<2)return false;
    const unsigned short stamp=InterpCoverStamp();
    unsigned samples=0,covered=0;
    for(int i=0;i<count;++i)for(unsigned h=0;h<(i+1<count?2u:1u);++h){
        const float x=h?(pos[i][0]+pos[i+1][0])*0.5f:pos[i][0],y=h?(pos[i][1]+pos[i+1][1])*0.5f:pos[i][1];
        const int cx=(int)SDL_floorf((x+draw_offset.x-draw_area_start.x)/INTERP_COVER_CELL);
        const int cy=(int)SDL_floorf((y+draw_offset.y-draw_area_start.y)/INTERP_COVER_CELL);
        ++samples;
        /* an outline lies on the ghost's edge: a cell next to it counts */
        bool near=false;
        for(int dy=-1;dy<=1&&!near;++dy)for(int dx=-1;dx<=1&&!near;++dx){const int u=cx+dx,w=cy+dy;
            near=u>=0&&w>=0&&u<VRAM_W/INTERP_COVER_CELL&&w<VRAM_H/INTERP_COVER_CELL&&interp_ghost_cover[w][u]==stamp;}
        covered+=near;
    }
    return covered*4>=samples*3;
}
/* Keeps the primitive an identity drew in this frame for a ghost. */
static void InterpStore(InterpPair *p,unsigned pair,const Vertex *v,unsigned count) {
    const unsigned b=interp_frame&1;
    if(p->store_frame==interp_frame||!InterpGhostEnabled())return;
    /* semi-transparent shapes (shadows, effects) blend: a ghost of one
     * shows as a patch where nothing was */
    if(count<3||count>6||interp_capture_indices<3||interp_capture_indices>6||v[0].exact[0][3]||v[0].a!=0xFF)return;
    if(interp_store_len[b]+count>INTERP_STORE){InterpEvict(IEVICT_STORE);return;}
    if(interp_order_len[b]>=INTERP_ORDER){InterpEvict(IEVICT_ORDER);return;}
    /* a pose crossing the view plane (a vertex at the GTE's screen limit)
     * or moving faster than a ghost can follow is no pose to continue */
    for(unsigned i=0;i<count;++i){
        if(v[i].x<=-1024||v[i].x>=1023||v[i].y<=-1024||v[i].y>=1023)return;
        if(p->motion_frame==interp_frame&&(SDL_fabsf(p->motion[i][0])>INTERP_GHOST_MOTION||SDL_fabsf(p->motion[i][1])>INTERP_GHOST_MOTION))return;
    }
    const unsigned base=(unsigned)(v-vertex_buf);
    for(unsigned k=0;k<interp_capture_indices;++k){
        const unsigned i=(unsigned)interp_capture_index[k]-base;
        if(i>=count)return;
        p->store_index[k]=(unsigned char)i;
    }
    memcpy(&interp_store[b][interp_store_len[b]],v,count*sizeof(*v));
    p->store_offset=interp_store_len[b];interp_store_len[b]+=count;
    p->store_count=(unsigned char)count;p->store_indices=(unsigned char)interp_capture_indices;
    p->store_offset_x=(short)(draw_offset.x-draw_area_start.x);p->store_offset_y=(short)(draw_offset.y-draw_area_start.y);
    p->store_width=(short)(draw_area_end.x-draw_area_start.x);p->store_height=(short)(draw_area_end.y-draw_area_start.y);
    p->store_frame=interp_frame;
    interp_order[b][interp_order_len[b]++]=pair;
}
/* The pose of a ghost after a whole frame (step 1) or half (0.5). */
static void InterpGhostPose(const InterpPair *g,unsigned i,float step,float out[2]) {
    const bool moving=g->motion_frame&&g->motion_frame==g->frame;
    out[0]=g->position[i][0]+(moving?g->motion[i][0]*step:0);
    out[1]=g->position[i][1]+(moving?g->motion[i][1]*step:0);
}
/* After the frame's tags (Psyz_GpuBeginInterpolationFrame): the identities
 * the frame before drew and this one leaves out become ghosts. */
static void InterpBuildGhosts(void) {
    const unsigned f=interp_frame-1,b=f&1,n=interp_order_len[b];
    unsigned vanished=0,last=0,tail=0;
    interp_inject=interp_ghost_first=0;interp_ghost_cut=false;
    if(!InterpGhostEnabled()||!n)return;
    for(unsigned k=0;k<n;++k){const InterpPair *p=&interp_pairs[interp_order[b][k]];
        if(p->store_frame==f&&p->tagged_frame!=interp_frame)++vanished;}
    if(interp_stats_log>0)SDL_Log("[ghost-build] f=%u order=%u store=%u vanished=%u",interp_frame,n,interp_store_len[b],vanished);
    /* most of the picture is new: a cut, which nothing pairs across */
    if(vanished*2>n){interp_ghost_cut=true;return;}
    for(unsigned k=0;k<n;++k){
        const unsigned q=interp_order[b][k];InterpPair *p=&interp_pairs[q];
        if(p->store_frame!=f)continue;
        if(p->tagged_frame==interp_frame){p->ghost_frame=interp_frame;p->ghost_head=0;last=q+1;tail=0;continue;}
        p->ghost_next=0;
        if(tail)interp_pairs[tail-1].ghost_next=q+1;
        else if(last)interp_pairs[last-1].ghost_head=q+1;
        else interp_ghost_first=q+1;
        tail=q+1;
        const unsigned c=p->store_count,kk=c==3?0:c==4?1:2;
        for(unsigned t=0;t<(c==3?1u:2u);++t){float a[3][2];
            for(unsigned i=0;i<3;++i){InterpGhostPose(p,interp_tri[kk][t][i],1.0f,a[i]);a[i][0]+=p->store_offset_x;a[i][1]+=p->store_offset_y;}
            InterpCoverTriangle(a[0],a[1],a[2]);}
    }
}
/* PSYZ_INTERP_PROBE=x,y[,first,last]: log every primitive whose frame or
 * midpoint pose covers draw-area pixel x,y (interpolation frames first..last). */
static bool InterpProbeCovers(const Vertex *v,unsigned count,bool precise,float px,float py) {
    static const unsigned tri[3][2][3]={{{0,1,2},{0,1,2}},{{0,1,2},{2,1,3}},{{0,1,2},{3,4,5}}};
    if(count!=3&&count!=4&&count!=6)return false;
    const unsigned k=count==3?0:count==4?1:2;
    for(unsigned t=0;t<2;++t){
        const Vertex *a=&v[tri[k][t][0]],*b=&v[tri[k][t][1]],*c=&v[tri[k][t][2]];
        float ax=precise&&a->pgxp_pos[2]>0?a->pgxp_pos[0]:a->x,ay=precise&&a->pgxp_pos[2]>0?a->pgxp_pos[1]:a->y;
        float bx=precise&&b->pgxp_pos[2]>0?b->pgxp_pos[0]:b->x,by=precise&&b->pgxp_pos[2]>0?b->pgxp_pos[1]:b->y;
        float cx=precise&&c->pgxp_pos[2]>0?c->pgxp_pos[0]:c->x,cy=precise&&c->pgxp_pos[2]>0?c->pgxp_pos[1]:c->y;
        float d1=(px-bx)*(ay-by)-(ax-bx)*(py-by),d2=(px-cx)*(by-cy)-(bx-cx)*(py-cy),d3=(px-ax)*(cy-ay)-(cx-ax)*(py-ay);
        bool neg=d1<0||d2<0||d3<0,pos=d1>0||d2>0||d3>0;
        if((ax-bx)*(cy-by)!=(ay-by)*(cx-bx)&&!(neg&&pos))return true;
    }
    return false;
}
static void InterpProbe(const void *packet,const Vertex *v,const Vertex *mid,unsigned count,const char *what) {
    static int px=-1,py,first=0,last=0x7FFFFFFF;
    static bool init,all;
    if(!init){init=true;if(SDL_sscanf(SDL_getenv("PSYZ_INTERP_PROBE"),"%d,%d,%d,%d",&px,&py,&first,&last)<2)px=-2;else if(px==-1)all=true,px=0;}
    if(px<0||(int)interp_frame<first||(int)interp_frame>last)return;
    const float x=(float)(px+draw_area_start.x-draw_offset.x)+0.5f,y=(float)(py+draw_area_start.y-draw_offset.y)+0.5f;
    const bool own=InterpProbeCovers(v,count,true,x,y),in_mid=InterpProbeCovers(mid,count,true,x,y);
    if(!own&&!in_mid&&!all)return;
    char line[768];int n=SDL_snprintf(line,sizeof(line),"[probe] f=%u %p id=%llx fl=%u %s%s%s n=%u rgb=%d,%d,%d t=%04x",interp_frame,packet,interp_pairs[interp_probe_pair].id,interp_probe_flags,what,own?" own":"",in_mid?" mid":"",count,v->r,v->g,v->b,(unsigned)v->t);
    for(unsigned i=0;i<count&&n<(int)sizeof(line)-40;++i)n+=SDL_snprintf(line+n,sizeof(line)-(size_t)n," (%.1f,%.1f|%.1f,%.1f)",
        v[i].pgxp_pos[2]>0?v[i].pgxp_pos[0]:v[i].x,v[i].pgxp_pos[2]>0?v[i].pgxp_pos[1]:v[i].y,mid[i].pgxp_pos[2]>0?mid[i].pgxp_pos[0]:mid[i].x,mid[i].pgxp_pos[2]>0?mid[i].pgxp_pos[1]:mid[i].y);
    if(v>=vertex_buf&&v+count<=vertex_buf+MAX_VERTEX_COUNT){n+=SDL_snprintf(line+n,sizeof(line)-(size_t)n," d=");
        for(unsigned i=0;i<count&&n<(int)sizeof(line)-12;++i)n+=SDL_snprintf(line+n,sizeof(line)-(size_t)n,"%.0f,",vertex_depth[v-vertex_buf+i]);}
    SDL_Log("%s",line);
}
static SDL_Rect scissor_rect = {0, 0, VRAM_W, VRAM_H};

/* Widescreen (Psyz_VideoSetWidescreen). The default squeeze, 3/4, keeps
 * every game pixel a whole number of render pixels wide at 4x and 8x (3 and
 * 6), so scrolling backgrounds do not shimmer. Psyz_VideoSetWidescreenSqueeze
 * sets another one, or 0 to fit the buffer to the 16:9 view of the display
 * mode. ws_picture holds the game's picture span per display buffer, keyed
 * by the buffer's VRAM row, the shift (game pixels) of the 16:9 view from
 * the screen's centre chosen for it and the squeeze it is drawn with. */
#define WS_SQUEEZE_DEFAULT 0.75f
#define WS_SQUEEZE_MIN 0.5f
#define WS_SQUEEZE_MAX 0.95f
static bool widescreen;
/* Psyz_VideoSetWidescreenSqueeze: 0 fits the display mode's 16:9 view */
static float ws_squeeze_setting = WS_SQUEEZE_DEFAULT;
/* Rendered-frame capture uses the same final widescreen crop and scaling as
 * presentation, into an offscreen texture even when the window is hidden. */
static SDL_GPUTexture* ws_capture;
static Uint32 ws_capture_width, ws_capture_height;
static bool ws_capture_active;
static struct {
    int y;
    int left, right;
    int shift;
    /* Psyz_VideoSetWidescreenPictureShifted: where the room belongs past
     * where it is drawn, in game pixels relative to the view (ex, ey), the
     * camera it is drawn for, and whether the picture's ends move with it */
    bool field, ends_move;
    float ex, ey;
    int camera_x;
    /* Psyz_VideoSetWidescreenNative: drawn and shown as without widescreen */
    bool native;
    /* the squeeze the buffer is drawn and presented with */
    float squeeze;
} ws_picture[2] = {{-1, 0, 0, 0, false, false, 0.0f, 0.0f, 0, false, WS_SQUEEZE_DEFAULT},
                   {-1, 0, 0, 0, false, false, 0.0f, 0.0f, 0, false, WS_SQUEEZE_DEFAULT}};
static float ws_mid_shift[2],ws_mid_qx[2],ws_mid_qy[2],ws_mid_ex[2],ws_mid_ey[2],ws_mid_left[2],ws_mid_right[2];
/* Psyz_VideoSetWidescreenNativeAspect: the aspect mode frames shown
 * without widescreen are presented at, or -1 to follow aspect_mode. */
static int ws_native_aspect = -1;
/* Psyz_VideoSetWidescreenCentred: drawing ignores the view's shift. */
static bool ws_centred;
/* Psyz_VideoSetWidescreenField: drawing is the room, it follows ex, ey. */
static bool ws_field;

static char aspect_trace_phase[32];
void Psyz_GpuTraceAspectOnce(const char *phase) {
    SDL_strlcpy(aspect_trace_phase, phase ? phase : "selection", sizeof(aspect_trace_phase));
}
/* The squeeze a buffer drawn now gets: the fixed one, or the one that makes
 * the buffer hold exactly the 16:9 view of the display mode's picture
 * aspect (45/64 for 320x256 at square pixels, 3/4 for 320x240). */
static float WidescreenCurrentSqueeze(void) {
    if (ws_squeeze_setting > 0.0f)
        return ws_squeeze_setting;
    const float game_aspect =
        GetCurrentGameAspectRatio(display_size.x, display_size.y);
    if (!(game_aspect > 0.0f))
        return WS_SQUEEZE_DEFAULT;
    return SDL_clamp(game_aspect * (9.0f / 16.0f), WS_SQUEEZE_MIN,
                     WS_SQUEEZE_MAX);
}

/* The squeeze of a picture (ws_picture index), or the current one. */
static float WidescreenPictureSqueeze(int picture) {
    return picture >= 0 && ws_picture[picture].y >= 0
               ? ws_picture[picture].squeeze
               : WidescreenCurrentSqueeze();
}

/* Half the 16:9 view's width in whole game pixels, for the screen's pixel
 * aspect (199 for 320x224), so its edges meet the picture's ends exactly;
 * never wider than the squeezed buffer. A buffer fitted to the view
 * (Psyz_VideoSetWidescreenSqueeze 0) shows all of it, to the fraction. */
static float WidescreenViewHalf(float game_aspect, float squeeze) {
    const float w = (float)display_size.x;
    const float view = w * (16.0f / 9.0f) / game_aspect;
    if (ws_squeeze_setting <= 0.0f)
        return SDL_min(view, w / squeeze) * 0.5f;
    return SDL_floorf(SDL_min(view, w / squeeze) * 0.5f);
}

static void TraceAspectViewport(SDL_Rect dst, WndSize win, bool rgb24, bool wide_view) {
    if (!*aspect_trace_phase) return;
    int picture=-1;
    for (int i=0;i<2;++i) if (ws_picture[i].y==display_area.y) picture=i;
    /* A changed wide selection has no room metadata in the paused preview.
     * Wait for a real field picture before labeling a resume viewport. */
    if (widescreen && wide_view && !rgb24 && picture<0 &&
        !SDL_strcmp(aspect_trace_phase,"menu_resume")) return;
    INFOF("GPU aspect prepared phase=%s runtime_aspect=%d runtime_wide=%d rgb24=%d wide_view=%d output=%dx%d viewport=%d,%d/%dx%d room_picture=%d room_bounds=%d..%d rendered=unobserved",
        aspect_trace_phase,aspect_mode,widescreen,rgb24,wide_view,win.w,win.h,
        dst.x,dst.y,dst.w,dst.h,picture,picture>=0?ws_picture[picture].left:0,
        picture>=0?ws_picture[picture].right:0);
    aspect_trace_phase[0]=0;
}

/* The drawing area is inside the display's VRAM columns and the first two
 * display heights of rows, where double buffers are stacked. */
static bool DisplayBufferDraw(void) {
    return !display_rgb24 && display_size.x > 0 && display_size.y > 0 &&
           draw_area_start.x >= display_area.x &&
           draw_area_end.x < display_area.x + display_size.x &&
           draw_area_start.y >= 0 && draw_area_end.y < 2 * display_size.y;
}


static int WidescreenDrawPicture(void);
/* The buffer at VRAM row y is drawn and shown as without widescreen. */
static bool WidescreenNativeBuffer(int y) {
    for (int i = 0; i < 2; ++i)
        if (ws_picture[i].y == y)
            return ws_picture[i].native;
    return false;
}
/* The aspect a frame shown from the buffer at VRAM row y (or 24-bit) is
 * presented at, before the widescreen view: a buffer shown without
 * widescreen while widescreen is on gets Psyz_VideoSetWidescreenNativeAspect. */
static float PresentGameAspect(int y, bool rgb24) {
    if (widescreen && ws_native_aspect >= 0 &&
        (rgb24 || WidescreenNativeBuffer(y)))
        return GameAspectRatioFor((PsyzAspectMode)ws_native_aspect,
                                  display_size.x, display_size.y);
    return GetCurrentGameAspectRatio(display_size.x, display_size.y);
}
static bool WidescreenDraw(void) {
    if (!widescreen || !DisplayBufferDraw())
        return false;
    const int picture = WidescreenDrawPicture();
    return picture < 0 || !ws_picture[picture].native;
}

/* The picture of the buffer the drawing area is in, or -1. */
static int WidescreenDrawPicture(void) {
    for (int i = 0; i < 2; ++i) {
        if (ws_picture[i].y >= 0 && draw_area_start.y >= ws_picture[i].y &&
            draw_area_start.y < ws_picture[i].y + display_size.y)
            return i;
    }
    return -1;
}

/* The view's shift for the buffer the drawing area is in. */
static int WidescreenDrawShift(void) {
    const int picture = WidescreenDrawPicture();
    return picture >= 0 ? ws_picture[picture].shift : 0;
}

/* The part of a picture's room shift drawn into the buffer at scale n: the
 * squeezed room moves by whole buffer pixels from frame to frame, so its
 * texels keep their shape, and the presenter shows the rest (under half a
 * buffer pixel). The room's grid is at whole game pixels from the camera,
 * and the view's shift moves it by the squeeze per pixel in the buffer. */
static void WidescreenFieldStep(int picture, float n, float* qx, float* qy) {
    *qx = *qy = 0.0f;
    if (picture < 0 || !ws_picture[picture].field)
        return;
    const float sx = WidescreenPictureSqueeze(picture) * n;
    const float base =
        (float)(ws_picture[picture].camera_x + ws_picture[picture].shift);
    *qx = SDL_roundf((ws_picture[picture].ex - base) * sx) / sx + base;
    *qy = SDL_roundf(ws_picture[picture].ey * n) / n;
}

/* The squeeze's fixed point. It moves s / (1 - s) pixels (3 at 3/4) against
 * each pixel of view shift, so the buffer holds the shifted view around its
 * own centre: game x centre + shift lands on the buffer's centre. */
static float WidescreenPivot(float shift, float squeeze) {
    return (float)display_size.x * 0.5f -
           (float)shift * squeeze / (1.0f - squeeze);
}

static float WidescreenCentre(void) {
    return (float)display_area.x +
           WidescreenPivot(ws_centred ? 0 : WidescreenDrawShift(),
                           WidescreenPictureSqueeze(WidescreenDrawPicture()));
}

/* Exposure grids (interp_expose): a triangle at cell centres, positions
 * relative to the frame's buffer, view depths (0 for none: 2D, in front). */
static void InterpExposeTriangle(unsigned grid,const float *a,const float *b,const float *c,float da,float db,float dc,const int clip[4]) {
    const float area=(b[0]-a[0])*(c[1]-a[1])-(c[0]-a[0])*(b[1]-a[1]);
    if(!(SDL_fabsf(area)>=0.25f))return;
    const float lx=SDL_max(SDL_min(a[0],SDL_min(b[0],c[0])),(float)clip[0]),hx=SDL_min(SDL_max(a[0],SDL_max(b[0],c[0])),(float)clip[2]);
    const float ly=SDL_max(SDL_min(a[1],SDL_min(b[1],c[1])),(float)clip[1]),hy=SDL_min(SDL_max(a[1],SDL_max(b[1],c[1])),(float)clip[3]);
    if(!(hx>=lx&&hy>=ly&&hx>=0.0f&&hy>=0.0f&&lx<(float)VRAM_W&&ly<(float)VRAM_H))return;
    /* the cells whose centre lies in the drawing area */
    const int x0=SDL_max(0,(int)SDL_ceilf(lx/INTERP_EXPOSE_CELL-0.5f)),x1=SDL_min(INTERP_EXPOSE_W-1,(int)SDL_floorf(hx/INTERP_EXPOSE_CELL-0.5f));
    const int y0=SDL_max(0,(int)SDL_ceilf(ly/INTERP_EXPOSE_CELL-0.5f)),y1=SDL_min(INTERP_EXPOSE_H-1,(int)SDL_floorf(hy/INTERP_EXPOSE_CELL-0.5f));
    const bool deep=da>0.0f&&db>0.0f&&dc>0.0f;
    const float ia=deep?1.0f/da:0,ib=deep?1.0f/db:0,ic=deep?1.0f/dc:0;
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
        const float px=(x+0.5f)*INTERP_EXPOSE_CELL,py=(y+0.5f)*INTERP_EXPOSE_CELL;
        const float wa=(c[0]-b[0])*(py-b[1])-(px-b[0])*(c[1]-b[1]);
        const float wb=(a[0]-c[0])*(py-c[1])-(px-c[0])*(a[1]-c[1]);
        const float wc=area-wa-wb;
        if(area>0?(wa<0||wb<0||wc<0):(wa>0||wb>0||wc>0))continue;
        const float depth=deep?area/(wa*ia+wb*ib+wc*ic):0.0f;
        InterpExposeCell *e=&interp_expose[grid][y][x];
        if(e->frame!=interp_frame||depth<e->depth){e->frame=interp_frame;e->depth=depth;}
    }
}
static void InterpExposeViewNow(InterpExposeView *view) {
    const unsigned slot=interp_frame&1;
    const bool wide=WidescreenDraw();
    view->ox=(float)draw_offset.x;view->oy=(float)draw_offset.y;
    view->squeeze=wide?WidescreenPictureSqueeze(WidescreenDrawPicture()):1.0f;
    view->centre=wide?WidescreenCentre():0.0f;
    /* the drawing area (inclusive), relative to the frame's buffer */
    view->clip[0]=draw_area_start.x-interp_expose_rect[slot].x;view->clip[1]=draw_area_start.y-interp_expose_rect[slot].y;
    view->clip[2]=draw_area_end.x+1-interp_expose_rect[slot].x;view->clip[3]=draw_area_end.y+1-interp_expose_rect[slot].y;
}
/* The opaque triangles of a queued primitive (vertices at base, their view
 * depths at depth) into the frame's grid (0) or the midpoint's (2). */
static void InterpExposePrimitive(unsigned grid,const InterpExposeView *view,const Vertex *v,const float *depth,const u32 *index,unsigned indices,unsigned base,bool precise) {
    const unsigned slot=interp_frame&1;
    if(interp_expose_rect[slot].frame!=interp_frame||v[0].a!=0xFF||indices<3)return;
    for(unsigned t=0;t+3<=indices;t+=3){float q[3][2],d[3];bool hidden=false;
        for(unsigned k=0;k<3;++k){const unsigned i=index[t+k];
            if(i<base||i>=base+6)return;
            const Vertex *u=&v[i-base];
            const bool p=precise&&u->pgxp_pos[2]>0.0f;
            const float vx=p?u->pgxp_pos[0]:(float)u->x,vy=p?u->pgxp_pos[1]:(float)u->y;
            hidden|=vx<=-2047.0f&&vy<=-2047.0f;
            float x=vx+view->ox;
            if(view->squeeze!=1.0f)x=view->centre+(x-view->centre)*view->squeeze;
            q[k][0]=x-(float)interp_expose_rect[slot].x;q[k][1]=vy+view->oy-(float)interp_expose_rect[slot].y;
            d[k]=depth[i-base]>0.0f?depth[i-base]:0.0f;}
        if(!hidden)InterpExposeTriangle(grid?2u:slot,q[0],q[1],q[2],d[0],d[1],d[2],view->clip);
    }
}
/* The cells of the midpoint of the last frame that show something much
 * farther (or nothing) where it and the frame before show something, and
 * whose four neighbours do too: an area at least two cells across, not the
 * sub-cell shift of an edge between near and far. */
static unsigned InterpExposed(void) {
    static unsigned char mask[INTERP_EXPOSE_H][INTERP_EXPOSE_W];
    const unsigned now=interp_frame&1,before=now^1;
    interp_exposed_holes=0;
    if(interp_expose_rect[now].frame!=interp_frame||interp_expose_rect[before].frame+1!=interp_frame||
       interp_expose_rect[now].w!=interp_expose_rect[before].w||interp_expose_rect[now].h!=interp_expose_rect[before].h)return 0;
    const int w=SDL_min(INTERP_EXPOSE_W,interp_expose_rect[now].w/INTERP_EXPOSE_CELL),h=SDL_min(INTERP_EXPOSE_H,interp_expose_rect[now].h/INTERP_EXPOSE_CELL);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){
        const InterpExposeCell *a=&interp_expose[before][y][x],*b=&interp_expose[now][y][x],*m=&interp_expose[2][y][x];
        const float nearest=SDL_max(a->depth,b->depth);
        mask[y][x]=a->frame+1==interp_frame&&b->frame==interp_frame&&
            (m->frame!=interp_frame||(nearest>0.0f&&m->depth>nearest*1.5f));
        interp_exposed_holes+=mask[y][x]&&m->frame!=interp_frame;
    }
    static int dump=-1;
    if(dump<0)dump=SDL_getenv("PSYZ_INTERP_EXPOSE_DUMP")?SDL_atoi(SDL_getenv("PSYZ_INTERP_EXPOSE_DUMP")):0;
    unsigned exposed=0;
    for(int y=1;y+1<h;++y)for(int x=1;x+1<w;++x)
        if(mask[y][x]&&mask[y-1][x]&&mask[y+1][x]&&mask[y][x-1]&&mask[y][x+1]){++exposed;
            if(dump&&(int)interp_frame==dump){const InterpExposeCell *a=&interp_expose[before][y][x],*b=&interp_expose[now][y][x],*m=&interp_expose[2][y][x];
                SDL_Log("[expose] %d,%d a=%.0f b=%.0f m=%.0f%s",x*INTERP_EXPOSE_CELL,y*INTERP_EXPOSE_CELL,a->depth,b->depth,m->depth,m->frame!=interp_frame?" none":"");}}
    return exposed;
}

/* Game pixels the squeezed buffer holds past each side of the screen,
 * rounded up (a sixth of the width at 3/4). */
static int WidescreenBufferMargin(float squeeze) {
    if (squeeze == WS_SQUEEZE_DEFAULT)
        return (display_size.x + 5) / 6;
    return (int)SDL_ceilf(
        (float)display_size.x * (1.0f / squeeze - 1.0f) * 0.5f - 0.001f);
}

static int WidescreenMargin(void) {
    return WidescreenBufferMargin(
        WidescreenPictureSqueeze(WidescreenDrawPicture()));
}

/* An untextured fill across the whole screen width (fades, flashes, dimmed
 * backdrops) is stretched by the margin on both sides, so it covers the
 * extra picture too. */
static void WidenFullWidthFill(Vertex* v, int n) {
    if (!WidescreenDraw()) {
        return;
    }
    int origin = draw_offset.x - display_area.x;
    int min_x = v[0].x, max_x = v[0].x;
    for (int i = 1; i < n; ++i) {
        min_x = SDL_min(min_x, v[i].x);
        max_x = SDL_max(max_x, v[i].x);
    }
    if (origin + min_x > 0 || origin + max_x < display_size.x) {
        return;
    }
    /* plus the room's step (WidescreenFieldStep) */
    int margin = WidescreenMargin() + SDL_abs(WidescreenDrawShift()) +
                 (ws_field ? 3 : 0);
    for (int i = 0; i < n; ++i) {
        int shift = v[i].x == min_x ? -margin : (v[i].x == max_x ? margin : 0);
        v[i].x = (short)(v[i].x + shift);
        v[i].pgxp_pos[0] += (float)shift;
    }
}

static PsyzOverlayInitCB_SDL3GPU overlay_init_cb;
PsyzOverlayInitCB_SDL3GPU Psyz_OverlayInit_SDL3GPU(
    PsyzOverlayInitCB_SDL3GPU cb) {
    const PsyzOverlayInitCB_SDL3GPU prev = overlay_init_cb;
    overlay_init_cb = cb;
    if (is_platform_init_successful && cb && sdl3_window && device) {
        cb(sdl3_window, device);
    }
    return prev;
}

static PsyzOverlayRenderCB_SDL3GPU overlay_render_cb;
PsyzOverlayRenderCB_SDL3GPU Psyz_OverlayRender_SDL3GPU(
    PsyzOverlayRenderCB_SDL3GPU cb) {
    const PsyzOverlayRenderCB_SDL3GPU prev = overlay_render_cb;
    overlay_render_cb = cb;
    return prev;
}

static SDL_GPUCommandBuffer* AcquireCmd(void) {
    if (gpu_failed || !device) return NULL;
    if (!pending_cmd) {
        command_descriptors = 0;
        pending_cmd = SDL_AcquireGPUCommandBuffer(device);
        if (!pending_cmd) {
            GpuFail("SDL_AcquireGPUCommandBuffer", false);
        }
    }
    return pending_cmd;
}

/* Fences waited for are released after the next submit. SDL's Metal
 * backend frees a submitted command buffer (and drops its references to the
 * buffers and textures it used) in a later submit, once its fence is not
 * busy; a fence released earlier went back to the pool and that submit took
 * it for its own command buffer, so the old one looked busy and was never
 * freed. Each fenced submit (frames, ring wraps) leaked one, and with it
 * every vertex buffer SDL had cycled in (gigabytes). */
#define FENCES_TO_RELEASE 16
static SDL_GPUFence* fences_to_release[FENCES_TO_RELEASE];
static unsigned n_fences_to_release;
static void ReleaseFenceAfterSubmit(SDL_GPUFence* fence) {
    if (n_fences_to_release == FENCES_TO_RELEASE)
        SDL_ReleaseGPUFence(device, fence);
    else
        fences_to_release[n_fences_to_release++] = fence;
}
/* After a submit (or at a retirement): the waited-for fences go. */
static void ReleaseWaitedFences(void) {
    for (unsigned i = 0; i < n_fences_to_release; ++i)
        SDL_ReleaseGPUFence(device, fences_to_release[i]);
    n_fences_to_release = 0;
}
static bool SubmitCmd(void) {
    if (gpu_failed) return false;
    if (pending_cmd) {
        SDL_GPUCommandBuffer *cmd = pending_cmd;
        pending_cmd = NULL; /* consumed by SDL on both results */
        if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            quarantined_cmd = cmd; /* evidence only; never dereference */
            present_commit_pending=false;return GpuFail("SDL_SubmitGPUCommandBuffer", true);
        }
        ReleaseWaitedFences();
        RecordPresentedCommit();
    }
    return true;
}
/* A ring wrap (RingReserve, UploadMap) submits the work recorded so far
 * with a fence and first waits for the fence of the wrap RING_WRAPS_IN_FLIGHT
 * wraps before. SDL cycles a ring's buffers at a wrap while the GPU still
 * reads them and keeps every buffer it cycled in; presents are fenced
 * (SubmitFrame), and this bounds those buffers also while a game draws or
 * loads without presenting. */
#define RING_WRAPS_IN_FLIGHT 3
static SDL_GPUFence* ring_wrap_fences[RING_WRAPS_IN_FLIGHT];
static unsigned ring_wrap_index;
static void RingWrapThrottle(void) {
    if (gpu_failed || !device)
        return;
    SDL_GPUFence** fence = &ring_wrap_fences[ring_wrap_index];
    if (*fence) {
        if (!SDL_WaitForGPUFences(device, true, fence, 1)) {
            GpuFail("ring SDL_WaitForGPUFences", true);
            return;
        }
        ReleaseFenceAfterSubmit(*fence);
        *fence = NULL;
    }
    if (pending_cmd) {
        SDL_GPUCommandBuffer* cmd = pending_cmd;
        pending_cmd = NULL;
        *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (!*fence) {
            quarantined_cmd = cmd;
            present_commit_pending = false;
            GpuFail("ring submit/fence", true);
            return;
        }
        ReleaseWaitedFences();
        RecordPresentedCommit();
    }
    ring_wrap_index = (ring_wrap_index + 1) % RING_WRAPS_IN_FLIGHT;
}
static void ReleaseRingFences(void) {
    for (int i = 0; i < RING_WRAPS_IN_FLIGHT; i++) {
        if (ring_wrap_fences[i]) {
            SDL_ReleaseGPUFence(device, ring_wrap_fences[i]);
            ring_wrap_fences[i] = NULL;
        }
    }
}
static bool RetireGpu(void) {
    if (gpu_retirement_uncertain) return false;
    if (!SubmitCmd()) {
        /* Acquisition failure can leave a previously recorded buffer. Do not
         * erase or reclaim that graph on a terminal backend failure. */
        gpu_retirement_uncertain = true;
        return false;
    }
    if (!SDL_WaitForGPUIdle(device)) return GpuFail("SDL_WaitForGPUIdle", true);
    return true;
}

int Psyz_GpuGetSpriteAtlasesEnabled(void) {return sprite_atlases_enabled;}

void Psyz_GpuSetSpriteAtlasesEnabled(int enabled) {
    if(sprite_atlases_enabled!=(enabled!=0)) {
        if(device){Draw_FlushBuffer();InterpMidpointSync(INTERP_SYNC_SETTINGS);}
        sprite_atlases_enabled=enabled!=0;
    }
}

void Psyz_GpuResetSpriteAtlases(void) {
    if (!device || UseSoftwareBackend()) return;
    /* A room transition may still have queued draws reading the old cells.
     * Retire those reads before another room uploads into the same storage. */
    Draw_FlushBuffer();
    InterpMidpointSync(INTERP_SYNC_SETTINGS);
    if (!RetireGpu()) return; /* preserve all atlas keys/cells on uncertainty */
    sprite_atlas_count=0;
    sprite_atlas_cells=0;
    sprite_atlas_bytes=0;
    next_sprite_atlas=NULL;
    next_sprite_cell=0;
}

/* The lossy material and lossless native-reference plane are deliberately
 * separate: changing reference colors would break palette fallback. */
static SDL_Surface *LoadHdImage(const char *filename) {
    const char *extension=SDL_strrchr(filename,'.');
    if (!extension || SDL_strcasecmp(extension,".webp"))
        return extension && !SDL_strcasecmp(extension,".png") ? SDL_LoadPNG(filename) : SDL_LoadBMP(filename);
    size_t bytes=0;void *data=SDL_LoadFile(filename,&bytes);
    if(!data)return NULL;
    int width,height;
    if(!WebPGetInfo(data,bytes,&width,&height) || width<=0 || height<=0 || width>4096 || height>16384 ||
       (uint64_t)width*height>32u*1024u*1024u){SDL_free(data);return NULL;}
    SDL_Surface *surface=SDL_CreateSurface(width,height,SDL_PIXELFORMAT_RGBA32);
    if(surface && !WebPDecodeRGBAInto(data,bytes,surface->pixels,(size_t)surface->pitch*height,surface->pitch)) {
        SDL_DestroySurface(surface);surface=NULL;
    }
    SDL_free(data);return surface;
}

int Psyz_GpuLoadSpriteAtlas(uint64_t key, const char *filename) {
    if (gpu_failed || !key || !filename || !device || UseSoftwareBackend()) return 0;
    for (unsigned i=0;i<sprite_atlas_count;++i)
        if (sprite_atlases[i].key==key) return 1;
    /* the midpoint kept so far samples the atlas as it is */
    InterpMidpointSync(INTERP_SYNC_SETTINGS);
    const char *extension=SDL_strrchr(filename,'.');
    SDL_Surface *source=LoadHdImage(filename);
    if(source && extension && !SDL_strcasecmp(extension,".webp")) {
        char reference_path[4096];size_t prefix=(size_t)(extension-filename);
        if(prefix+sizeof(".ref.png")>sizeof(reference_path)){SDL_DestroySurface(source);return -1;}
        memcpy(reference_path,filename,prefix);memcpy(reference_path+prefix,".ref.png",sizeof(".ref.png"));
        SDL_Surface *reference=SDL_LoadPNG(reference_path);
        if(!reference || source->w!=reference->w || source->h!=reference->h || source->w%68 || source->h%68 || source->h>8192) {
            if(reference)SDL_DestroySurface(reference);SDL_DestroySurface(source);return -1;
        }
        SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);SDL_SetSurfaceBlendMode(reference,SDL_BLENDMODE_NONE);
        SDL_Surface *combined=SDL_CreateSurface(source->w,source->h*2,SDL_PIXELFORMAT_RGBA32);
        SDL_Rect bottom={0,source->h,source->w,source->h};
        if(combined && (!SDL_BlitSurface(source,NULL,combined,NULL) || !SDL_BlitSurface(reference,NULL,combined,&bottom))) {
            SDL_DestroySurface(combined);combined=NULL;
        }
        SDL_DestroySurface(reference);SDL_DestroySurface(source);source=combined;
    }
    if (!source) return 0;
    int w=source->w,h=source->h;
    size_t bytes=(size_t)w*(size_t)h*4;
    if (w<=0 || h<=0 || w%68 || h%136 || w>16*68 || h>16384 ||
        sprite_atlas_count>=512 || (unsigned)(w/68)*(unsigned)(h/136)>HD_ATLAS_COLUMNS*HD_ATLAS_ROWS-sprite_atlas_cells) {
        SDL_DestroySurface(source); WARNF("Invalid or over-budget HD sprite atlas: %s",filename); return -1;
    }
    SDL_Surface *rgba=SDL_ConvertSurface(source,SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(source); if (!rgba) return -1;
    SDL_GPUTextureCreateInfo info={.type=SDL_GPU_TEXTURETYPE_2D,.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage=SDL_GPU_TEXTUREUSAGE_SAMPLER,.width=HD_ATLAS_WIDTH,.height=HD_ATLAS_HEIGHT,.layer_count_or_depth=1,.num_levels=1};
    if (!sprite_atlas_texture) sprite_atlas_texture=SDL_CreateGPUTexture(device,&info);
    if (!sprite_atlas_sampler) {
        SDL_GPUSamplerCreateInfo sampler={.min_filter=SDL_GPU_FILTER_LINEAR,.mag_filter=SDL_GPU_FILTER_LINEAR,
            .mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,.address_mode_u=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
        sprite_atlas_sampler=SDL_CreateGPUSampler(device,&sampler);
    }
    SDL_GPUTexture *texture=sprite_atlas_texture;
    SDL_GPUTransferBufferCreateInfo upload_info={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=(Uint32)bytes};
    SDL_GPUTransferBuffer *upload=(texture && sprite_atlas_sampler) ? SDL_CreateGPUTransferBuffer(device,&upload_info) : NULL;
    void *mapped=upload ? SDL_MapGPUTransferBuffer(device,upload,false) : NULL;
    if (!texture || !sprite_atlas_sampler || !mapped) {
        if (mapped) SDL_UnmapGPUTransferBuffer(device,upload);
        if (upload) SDL_ReleaseGPUTransferBuffer(device,upload);
        SDL_DestroySurface(rgba); return -1;
    }
    unsigned columns=(unsigned)w/68,rows=(unsigned)h/136,cells=columns*rows;
    /* This SDL revision's Metal uploader uses destination width for row
     * pitch. Pack every cell contiguously instead of relying on row strides. */
    for (unsigned cell=0;cell<cells;++cell) for (unsigned reference=0;reference<2;++reference) {
        unsigned sx=(cell%columns)*68,sy=(cell/columns)*68+reference*(unsigned)h/2;
        u8 *target=(u8 *)mapped+(size_t)(cell*2+reference)*68*68*4;
        for (unsigned row=0;row<68;++row)
            memcpy(target+(size_t)row*68*4,(u8 *)rgba->pixels+(size_t)(sy+row)*rgba->pitch+sx*4,68*4);
        if (reference==0) {
            u8 solid[68][68];
            for (unsigned row=0;row<68;++row) {
                u8 *line=target+(size_t)row*68*4;
                for (unsigned col=0;col<68;++col) solid[row][col]=line[col*4+3]>=128;
                for (unsigned col=1;col<68;++col) {
                    if (!solid[row][col] && solid[row][col-1]) {
                        memcpy(line+col*4,line+(col-1)*4,3);
                        solid[row][col]=1;
                    }
                }
                for (unsigned col=67;col>0;--col) {
                    if (!solid[row][col-1] && solid[row][col]) {
                        memcpy(line+(col-1)*4,line+col*4,3);
                        solid[row][col-1]=1;
                    }
                }
            }
            for (unsigned col=0;col<68;++col) {
                for (unsigned row=1;row<68;++row) {
                    if (!solid[row][col] && solid[row-1][col]) {
                        memcpy(target+((size_t)row*68+col)*4,target+((size_t)(row-1)*68+col)*4,3);
                        solid[row][col]=1;
                    }
                }
                for (unsigned row=67;row>0;--row) {
                    if (!solid[row-1][col] && solid[row][col]) {
                        memcpy(target+((size_t)(row-1)*68+col)*4,target+((size_t)row*68+col)*4,3);
                        solid[row-1][col]=1;
                    }
                }
            }
        }
    }
    SDL_UnmapGPUTransferBuffer(device,upload);SDL_DestroySurface(rgba);
    SDL_GPUCommandBuffer *cmd=AcquireCmd();
    if (!cmd) { SDL_ReleaseGPUTransferBuffer(device,upload);return -1; }
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    if (!copy) { SDL_ReleaseGPUTransferBuffer(device,upload); return -1; }
    for (unsigned cell=0;cell<cells;++cell) for (unsigned reference=0;reference<2;++reference) {
        unsigned target_cell=sprite_atlas_cells+cell;
        SDL_GPUTextureTransferInfo from={.transfer_buffer=upload,
            .offset=(cell*2+reference)*68*68*4,.pixels_per_row=68,.rows_per_layer=68};
        SDL_GPUTextureRegion to={.texture=texture,.x=(target_cell%HD_ATLAS_COLUMNS)*68,
            .y=(target_cell/HD_ATLAS_COLUMNS)*68+reference*HD_ATLAS_HEIGHT/2,.w=68,.h=68,.d=1};
        SDL_UploadToGPUTexture(copy,&from,&to,false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device,upload);
    sprite_atlases[sprite_atlas_count++]=(SpriteAtlas){key,columns,rows,sprite_atlas_cells};
    sprite_atlas_cells+=cells;
    sprite_atlas_bytes+=bytes;
    INFOF("HD sprite atlas %016llx: %dx%d (%zu MiB cached)",(unsigned long long)key,w,h,sprite_atlas_bytes/(1024*1024));
    return 1;
}

void Psyz_GpuSelectSpriteAtlas(uint64_t key, unsigned cell) {
    next_sprite_atlas=NULL;
    next_sprite_extension=(cell&0x800000u)!=0;
    cell&=0x7fffffu;
    for (unsigned i=0;i<sprite_atlas_count;++i) {
        SpriteAtlas *atlas=&sprite_atlases[i];
        if (atlas->key==key && cell<atlas->columns*atlas->rows) {
            next_sprite_atlas=atlas;next_sprite_cell=cell;return;
        }
    }
}


/* Call only between passes: submitted uploads remain ordered before subsequent
 * draws, and SDL tracks their resources until the submitted buffer completes.
 */
static SDL_GPUCommandBuffer* ReserveDrawDescriptors(unsigned count) {
    if (command_descriptor_limit &&
        (count > command_descriptor_limit ||
         command_descriptors > command_descriptor_limit - count)) {
        SubmitCmd();
        if (!descriptor_rollover_reported) {
            INFOF("GPU descriptor batch rollover (budget %u)",
                  command_descriptor_limit);
            descriptor_rollover_reported = true;
        }
    }
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    if (cmd)
        command_descriptors += count;
    return cmd;
}

// FIX: the fence mechanic solves a out-of-memory on Windows when running a
// game with SDL_VIDEODRIVER=offscreen and PSYZ_VSYNC_LIMITLESS when the CPU
// accumulates graphics buffers to submit than what the GPU can process.
// This is especially relevant when running game replays on GH Actions.
#define MAX_FRAMES_IN_FLIGHT 2
static SDL_GPUFence* frame_fences[MAX_FRAMES_IN_FLIGHT];
static unsigned frame_fence_idx = 0;
static void SubmitFrame(void) {
    if (gpu_failed) return;
    if (!pending_cmd) {
        return;
    }
    if (swapchain_ok && frame_presented) {
        SubmitCmd();
        return;
    }
    SDL_GPUFence** fence = &frame_fences[frame_fence_idx];
    if (*fence) {
        if (!SDL_WaitForGPUFences(device, true, fence, 1)) {
            GpuFail("frame SDL_WaitForGPUFences", true); return;
        }
        ReleaseFenceAfterSubmit(*fence);
        *fence = NULL;
    }
    SDL_GPUCommandBuffer *cmd = pending_cmd;
    pending_cmd = NULL;
    *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!*fence) { quarantined_cmd = cmd; GpuFail("frame submit/fence", true); return; }
    ReleaseWaitedFences();
    RecordPresentedCommit();
    frame_fence_idx = (frame_fence_idx + 1) % MAX_FRAMES_IN_FLIGHT;
}
static void ReleaseFrameFences(void) {
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (frame_fences[i]) {
            SDL_ReleaseGPUFence(device, frame_fences[i]);
            frame_fences[i] = NULL;
        }
    }
}

static bool SubmitCmdAndWait(void) {
    if (gpu_failed) return false;
    if (!pending_cmd) {
        return true;
    }
    SDL_GPUCommandBuffer *cmd = pending_cmd;
    pending_cmd = NULL;
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) { quarantined_cmd = cmd; return GpuFail("submit/fence", true); }
    ReleaseWaitedFences();
    if (!SDL_WaitForGPUFences(device, true, &fence, 1)) {
        quarantined_fence = fence;
        return GpuFail("SDL_WaitForGPUFences", true);
    }
    ReleaseFenceAfterSubmit(fence);
    RecordPresentedCommit();
    return true;
}

#if defined(__APPLE__)
/* Whether the system's Metal device is an Apple GPU (MTLGPUFamilyApple1 and
 * later read colour attachments in fragment shaders); PSYZ_GPU_FETCH=0 turns
 * framebuffer fetch off. */
void* MTLCreateSystemDefaultDevice(void);
static bool MetalFramebufferFetch(void) {
    const char* env = SDL_getenv("PSYZ_GPU_FETCH");
    if (env && !strcmp(env, "0"))
        return false;
    if (strcmp(SDL_GetGPUDeviceDriver(device), "metal"))
        return false;
    id mtl = (id)MTLCreateSystemDefaultDevice();
    if (!mtl)
        return false;
    SEL supports = sel_registerName("supportsFamily:");
    bool apple = class_respondsToSelector(object_getClass(mtl), supports) &&
                 ((BOOL(*)(id, SEL, long))objc_msgSend)(
                     mtl, supports, 1001 /* MTLGPUFamilyApple1 */);
    ((void (*)(id, SEL))objc_msgSend)(mtl, sel_registerName("release"));
    return apple;
}
#endif

static SDL_GPUShader* CreateShader(
    const unsigned char* code, unsigned int code_len, SDL_GPUShaderStage stage,
    Uint32 num_samplers, Uint32 num_uniform_buffers) {
    const SDL_GPUShaderCreateInfo info = {
        .code = code,
        .code_size = code_len,
#if defined(_WIN32)
        .format = SDL_GPU_SHADERFORMAT_DXIL,
        .entrypoint = "main",
#elif defined(__APPLE__)
        .format = SDL_GPU_SHADERFORMAT_MSL,
        .entrypoint = "main0",
#else
        .format = SDL_GPU_SHADERFORMAT_SPIRV,
        .entrypoint = "main",
#endif
        .stage = stage,
        .num_samplers = num_samplers,
        .num_uniform_buffers = num_uniform_buffers,
    };
    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (!shader) {
        GpuFail("resource initialization", false);
        ERRORF("SDL_CreateGPUShader: %s", SDL_GetError());
    }
    return shader;
}

static SDL_GPUGraphicsPipeline* CreatePsxPipeline(
    SDL_GPUShader* vs, SDL_GPUShader* fs, BlendMode mode) {
    const SDL_GPUVertexBufferDescription vb_desc = {
        .slot = 0,
        .pitch = sizeof(Vertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    };
    const SDL_GPUVertexAttribute attribs[] = {
        {.location = 0,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_SHORT2,
         .offset = offsetof(Vertex, x)},
        {.location = 1,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_USHORT4,
         .offset = offsetof(Vertex, u)},
        {.location = 2,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
         .offset = offsetof(Vertex, r)},
        {.location = 3,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4,
         .offset = offsetof(Vertex, twin)},
        {.location = 4,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
         .offset = offsetof(Vertex, polygon_uv)},
        {.location = 5,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
         .offset = offsetof(Vertex, polygon_rgb)},
        {.location = 6,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
         .offset = offsetof(Vertex, pgxp_pos)},
        {.location = 7,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
         .offset = offsetof(Vertex, uv_bounds)},
        {.location=8,.buffer_slot=0,.format=SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,.offset=offsetof(Vertex,hd_uv)},
        {.location = 9, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT4,
         .offset = offsetof(Vertex, exact[0])},
        {.location = 10, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT4,
         .offset = offsetof(Vertex, exact[1])},
        {.location = 11, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT4,
         .offset = offsetof(Vertex, exact[2])},
        {.location = 12, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT4,
         .offset = offsetof(Vertex, exact[3])},
        {.location = 13, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT4,
         .offset = offsetof(Vertex, exact[4])},
    };
    // the shader outputs alpha 0 on B-F texels and alpha 1 on opaque ones
    static const SDL_GPUColorTargetBlendState blend[BLEND_COUNT] = {
        [BLEND_ADD] =
            {
                .src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                .dst_color_blendfactor =
                    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .color_blend_op = SDL_GPU_BLENDOP_ADD,
                .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                .dst_alpha_blendfactor =
                    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
                .enable_blend = true,
            },
        [BLEND_SUB] =
            {
                .src_color_blendfactor =
                    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                .color_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT,
                .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO,
                .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
                .enable_blend = true,
            },
        /* "check mask": the destination's mask bit (alpha 0 or 1) selects
         * the destination, otherwise the source with its mask bit */
        [BLEND_CHECK] =
            {
                .src_color_blendfactor =
                    SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA,
                .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_ALPHA,
                .color_blend_op = SDL_GPU_BLENDOP_ADD,
                .src_alpha_blendfactor =
                    SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA,
                .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_DST_ALPHA,
                .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
                .enable_blend = true,
            },
        [BLEND_SUB_OPAQUE] =
            {
                .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                .dst_color_blendfactor =
                    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .color_blend_op = SDL_GPU_BLENDOP_ADD,
                .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                .dst_alpha_blendfactor =
                    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
                .enable_blend = true,
            },
    };
    SDL_GPUColorTargetBlendState blend_state = blend[mode];
    if (mode == BLEND_MASK) {
        blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_A;
        blend_state.enable_color_write_mask = true;
    }
    const SDL_GPUColorTargetDescription target = {
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .blend_state = blend_state,
    };
    const SDL_GPUGraphicsPipelineCreateInfo info = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .vertex_input_state =
            {
                .vertex_buffer_descriptions = &vb_desc,
                .num_vertex_buffers = 1,
                .vertex_attributes = attribs,
                .num_vertex_attributes = LENU(attribs),
            },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .target_info =
            {
                .color_target_descriptions = &target,
                .num_color_targets = 1,
            },
    };
    SDL_GPUGraphicsPipeline* pipe =
        SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!pipe) {
        ERRORF("SDL_CreateGPUGraphicsPipeline: %s", SDL_GetError());
    }
    return pipe;
}

// used only for a fast hardware ClearImage
static SDL_GPUGraphicsPipeline* CreateClearPipeline(
    SDL_GPUShader* vs, SDL_GPUShader* fs) {
    const SDL_GPUVertexBufferDescription vb_desc = {
        .slot = 0,
        .pitch = sizeof(Vertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    };
    const SDL_GPUVertexAttribute attribs[] = {
        {.location = 0,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_SHORT2,
         .offset = offsetof(Vertex, x)},
        {.location = 1,
         .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
         .offset = offsetof(Vertex, r)},
    };
    // blending disabled: the clear color (including alpha 0, the PS1 mask
    // bit) is written as-is, matching glClearColor(r, g, b, 0) + glClear
    const SDL_GPUColorTargetDescription target = {
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    };
    const SDL_GPUGraphicsPipelineCreateInfo info = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .vertex_input_state =
            {
                .vertex_buffer_descriptions = &vb_desc,
                .num_vertex_buffers = 1,
                .vertex_attributes = attribs,
                .num_vertex_attributes = LENU(attribs),
            },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP,
        .target_info =
            {
                .color_target_descriptions = &target,
                .num_color_targets = 1,
            },
    };
    SDL_GPUGraphicsPipeline* pipe =
        SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!pipe) {
        ERRORF("SDL_CreateGPUGraphicsPipeline: %s", SDL_GetError());
    }
    return pipe;
}

static bool CreateGpuResources(void) {
    const SDL_GPUTextureCreateInfo render_info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage =
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = VRAM_W,
        .height = VRAM_H,
        .layer_count_or_depth = 1,
        .num_levels = 1,
    };
    vram_render = SDL_CreateGPUTexture(device, &render_info);
    SDL_GPUTextureCreateInfo sample_info = render_info;
    sample_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    vram_sample = SDL_CreateGPUTexture(device, &sample_info);
    blend_sample = SDL_CreateGPUTexture(device, &sample_info);
    SDL_GPUTextureCreateInfo clut_info = sample_info;
    clut_info.width = 256;
    clut_info.height = CLUT_CACHE_ROWS;
    clut_cache = SDL_CreateGPUTexture(device, &clut_info);
    if (!vram_render || !vram_sample || !blend_sample || !clut_cache) {
        ERRORF("SDL_CreateGPUTexture: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUSamplerCreateInfo sampler_info = {
        .min_filter = SDL_GPU_FILTER_NEAREST,
        .mag_filter = SDL_GPU_FILTER_NEAREST,
        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    };
    vram_sampler = SDL_CreateGPUSampler(device, &sampler_info);
    if (!vram_sampler) {
        ERRORF("SDL_CreateGPUSampler: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUBufferCreateInfo vbuf_info = {
        .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
        .size = MAX_VERTEX_COUNT * sizeof(Vertex),
    };
    vbuf = SDL_CreateGPUBuffer(device, &vbuf_info);
    const SDL_GPUBufferCreateInfo ibuf_info = {
        .usage = SDL_GPU_BUFFERUSAGE_INDEX,
        .size = MAX_INDEX_COUNT * sizeof(*index_buf),
    };
    ibuf = SDL_CreateGPUBuffer(device, &ibuf_info);
    const SDL_GPUBufferCreateInfo clear_vbuf_info = {
        .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
        .size = 4 * sizeof(Vertex),
    };
    clear_vbuf = SDL_CreateGPUBuffer(device, &clear_vbuf_info);
    if (!vbuf || !ibuf || !clear_vbuf) {
        ERRORF("SDL_CreateGPUBuffer: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUTransferBufferCreateInfo vtx_transfer_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size =
            MAX_VERTEX_COUNT * sizeof(Vertex) + MAX_INDEX_COUNT * sizeof(*index_buf),
    };
    vtx_transfer = SDL_CreateGPUTransferBuffer(device, &vtx_transfer_info);
    ring_restart = true;
    const SDL_GPUTransferBufferCreateInfo upload_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = VRAM_BYTES,
    };
    tex_upload_transfer = SDL_CreateGPUTransferBuffer(device, &upload_info);
    upload_ring_restart = true;
    const SDL_GPUTransferBufferCreateInfo download_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = VRAM_BYTES,
    };
    tex_download_transfer = SDL_CreateGPUTransferBuffer(device, &download_info);
    if (!vtx_transfer || !tex_upload_transfer || !tex_download_transfer) {
        ERRORF("SDL_CreateGPUTransferBuffer: %s", SDL_GetError());
        return false;
    }

    SDL_GPUShader* psx_vs =
        CreateShader(psx_vert, psx_vert_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader* psx_fs = CreateShader(
        psx_frag, psx_frag_len, SDL_GPU_SHADERSTAGE_FRAGMENT,
        PSX_SHADERS_CURRENT ? 4 : 3, 1);
    SDL_GPUShader* clear_vs = CreateShader(
        clear_vert, clear_vert_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    destination_fetch = false;
#if defined(__APPLE__) && defined(psx_frag_fetch_msl_revision)
    if (PSX_SHADERS_CURRENT &&
        psx_frag_fetch_msl_revision == PSX_SHADER_REVISION &&
        MetalFramebufferFetch() && psx_vs) {
        SDL_GPUShader* fetch_fs = SDL_CreateGPUShader(
            device, &(SDL_GPUShaderCreateInfo){
                        .code = psx_frag_fetch_msl,
                        .code_size = psx_frag_fetch_msl_len,
                        .format = SDL_GPU_SHADERFORMAT_MSL,
                        .entrypoint = "main0",
                        .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                        .num_samplers = 4,
                        .num_uniform_buffers = 1});
        if (fetch_fs) {
            pipe_direct_fetch =
                CreatePsxPipeline(psx_vs, fetch_fs, BLEND_DIRECT);
            SDL_ReleaseGPUShader(device, fetch_fs);
        }
        destination_fetch = pipe_direct_fetch != NULL;
        INFOF("SDL_GPU semi-transparency: %s",
              destination_fetch ? "framebuffer fetch" : "target copies");
    }
#endif
    SDL_GPUShader* clear_fs = CreateShader(
        clear_frag, clear_frag_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    bool shaders_ok = psx_vs && psx_fs && clear_vs && clear_fs;
    if (shaders_ok) {
        for (int i = 0; i < BLEND_COUNT; i++) {
            pipe_tri[i] = CreatePsxPipeline(psx_vs, psx_fs, (BlendMode)i);
        }
        pipe_clear = CreateClearPipeline(clear_vs, clear_fs);
    }
    if (psx_vs) {
        SDL_ReleaseGPUShader(device, psx_vs);
    }
    if (psx_fs) {
        SDL_ReleaseGPUShader(device, psx_fs);
    }
    if (clear_vs) {
        SDL_ReleaseGPUShader(device, clear_vs);
    }
    if (clear_fs) {
        SDL_ReleaseGPUShader(device, clear_fs);
    }
    for (int i = 0; i < BLEND_COUNT; i++) {
        shaders_ok = shaders_ok && pipe_tri[i];
    }
    if(!shaders_ok||!pipe_clear)return false;
    return true;
}


static SDL_GPUDevice *CreatePsyzGpuDevice(void) {
    return SDL_CreateGPUDevice(
#if defined(_WIN32)
        SDL_GPU_SHADERFORMAT_DXIL,
#elif defined(__APPLE__)
        SDL_GPU_SHADERFORMAT_MSL,
#else
        SDL_GPU_SHADERFORMAT_SPIRV,
#endif
        false, NULL);
}

bool InitPlatform() {
    if (gpu_failed) return false;
    if (is_platform_initialized) {
        return is_platform_init_successful;
    }
    is_platform_initialized = true; // don't re-initialize on failures

    const char* headless = SDL_getenv("PSYZ_HEADLESS");
    headless_renderer = headless && strcmp(headless, "1") == 0;
    hidden_gpu_renderer = headless && strcmp(headless, "gpu") == 0;
    if (headless_renderer) {
        if (!SDL_Init(SDL_INIT_EVENTS)) {
            return false;
        }
        SoftwareRasterizer_Reset();
        vsync_mode = PSYZ_VSYNC_LIMITLESS;
        Sdl3Common_TimingInit();
        is_platform_init_successful = true;
        return true;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        ERRORF("SDL_Init: %s", SDL_GetError());
        return false;
    }

    device = CreatePsyzGpuDevice();
    if (!device) {
        ERRORF("SDL_CreateGPUDevice: %s", SDL_GetError());
        return false;
    }
    INFOF("SDL_GPU with driver %s", SDL_GetGPUDeviceDriver(device));
    /* Every backend submits the frame's command buffer every 256
     * descriptors: D3D12 needs a limit for its descriptor heaps, and on Metal
     * one command buffer holds the memory of each of its passes until it
     * completes (a 2x race frame of 1500 forced 16-primitive batches peaked
     * at 1.5 GB in one command buffer a frame, 0.9 GB at 256). */
    command_descriptor_limit = 256u;
    /* Exercise the same command boundaries with other backends in headless
     * tests. */
    const char* descriptor_limit =
        SDL_getenv("PSYZ_GPU_COMMAND_DESCRIPTOR_LIMIT");
    if (descriptor_limit) {
        int value = SDL_atoi(descriptor_limit);
        if (value >= 4 && value <= 1024)
            command_descriptor_limit = (unsigned)value;
    }

#ifdef PLATFORM_IOS
    const SDL_WindowFlags window_flags =
        SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    const SDL_WindowFlags window_flags =
        SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    sdl3_window = SDL_CreateWindow(
        window_title, DEFAULT_FRONT_W, DEFAULT_FRONT_H, window_flags);
    if (!sdl3_window) {
        ERRORF("SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }

    // useful for offscreen rendering with SDL_VIDEODRIVER=offscreen
    swapchain_ok = !hidden_gpu_renderer &&
                   SDL_ClaimWindowForGPUDevice(device, sdl3_window);
    if (swapchain_ok) {
        swapchain_present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    } else if (!hidden_gpu_renderer) {
        WARNF("no swapchain, presentation disabled: %s", SDL_GetError());
    }

    SDL_SetAtomicInt(&swapchain_resize_pending, 0);
    SDL_AddEventWatch(WatchSwapchainResize, NULL);
    SDL_GetWindowSizeInPixels(sdl3_window, &swapchain_window_w, &swapchain_window_h);
    swapchain_window_fullscreen = SDL_GetWindowFlags(sdl3_window) & SDL_WINDOW_FULLSCREEN;

    if (!CreateGpuResources()) {
        return false;
    }

    cur_tpage = 0;
    /* A hidden renderer has nothing to pace against; PSYZ_HEADLESS_PACED=1
     * keeps the frame limiter to measure the interactive timing path. */
    if (hidden_gpu_renderer)
        vsync_mode = SDL_getenv("PSYZ_HEADLESS_PACED") ? PSYZ_VSYNC_OFF
                                                       : PSYZ_VSYNC_LIMITLESS;
    Sdl3Common_TimingInit();

    is_platform_init_successful = true;
    if (overlay_init_cb) {
        overlay_init_cb(sdl3_window, device);
    }
    return true;
}

static bool UseSoftwareBackend(void) {
    if (!is_platform_initialized)
        InitPlatform();
    return headless_renderer || native_rasterizer;
}

/* Software drawing without a GPU device to present it (PSYZ_HEADLESS=1).
 * The native rasterizer mode draws on the CPU but keeps the display state
 * and the GPU presentation. */
static bool SoftwareOnly(void) {
    return UseSoftwareBackend() && !native_rasterizer;
}

static bool PlatformBackend_SetDriverVsync(bool enable) {
    if (!device || !swapchain_ok) {
        return false;
    }
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!enable) {
        if (SDL_WindowSupportsGPUPresentMode(
                device, sdl3_window, SDL_GPU_PRESENTMODE_IMMEDIATE)) {
            mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
        } else if (SDL_WindowSupportsGPUPresentMode(
                       device, sdl3_window, SDL_GPU_PRESENTMODE_MAILBOX)) {
            mode = SDL_GPU_PRESENTMODE_MAILBOX;
        }
    }
    if (mode != swapchain_present_mode) {
        if (!SDL_SetGPUSwapchainParameters(
                device, sdl3_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
            ERRORF("could not set swapchain present mode: %s", SDL_GetError());
            SDL_ReleaseWindowFromGPUDevice(device, sdl3_window);
            swapchain_ok = false;
            return false;
        }
        swapchain_present_mode = mode;
    }
    return enable && mode == SDL_GPU_PRESENTMODE_VSYNC;
}

/* VSYNC presents never queue up while the limiter paces them below the
 * display's rate. */
static bool PlatformBackend_SetPresentSync(bool enable) {
    return PlatformBackend_SetDriverVsync(enable);
}
static bool PlatformBackend_BlankPresentWanted(void) {
    return interp_blank_mid && interp_pair_ready && interpolation_enabled;
}

/* The frame's drawing runs during the blank wait; the present's command
 * buffer then holds only the display copy (the frame fences of SubmitFrame
 * still bound the frames in flight). */
static void PlatformBackend_SubmitDrawing(void) {
    if (gpu_failed || !device)
        return;
    DRAW_FLUSH_FOR(DRAW_FLUSH_PRESENT);
    if (interp_pair_ready)
        InterpMidpointSync(INTERP_SYNC_PRESENT);
    SubmitCmd();
}

static void UpdateScissor(void);

static SDL_GPUTexture* GetRenderTarget(void) {
    return scaled_vram_render ? scaled_vram_render : vram_render;
}

static SDL_Rect vram_dirty = {0, 0, 0, 0};
static void MarkVramDirty(SDL_Rect r) {
    if (r.w <= 0 || r.h <= 0) {
        return;
    }
    int x0 = CLAMP(r.x, 0, VRAM_W);
    int y0 = CLAMP(r.y, 0, VRAM_H);
    int x1 = CLAMP(r.x + r.w, 0, VRAM_W);
    int y1 = CLAMP(r.y + r.h, 0, VRAM_H);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    VramTilesAdd(&vram_dirty_tiles, x0, y0, x1 - x0, y1 - y0);
    if (vram_dirty.w <= 0 || vram_dirty.h <= 0) {
        vram_dirty = (SDL_Rect){x0, y0, x1 - x0, y1 - y0};
        return;
    }
    int ux0 = x0 < vram_dirty.x ? x0 : vram_dirty.x;
    int uy0 = y0 < vram_dirty.y ? y0 : vram_dirty.y;
    int ux1 =
        x1 > vram_dirty.x + vram_dirty.w ? x1 : vram_dirty.x + vram_dirty.w;
    int uy1 =
        y1 > vram_dirty.y + vram_dirty.h ? y1 : vram_dirty.y + vram_dirty.h;
    vram_dirty = (SDL_Rect){ux0, uy0, ux1 - ux0, uy1 - uy0};
}
static void ResetVramDirty(void) {
    vram_dirty = (SDL_Rect){0, 0, 0, 0};
    vram_dirty_tiles = (VramTiles){{0}};
}

// mirror a native VRAM region into the scaled render target
static void InterpBackdropAfterFill(int x, int y, int w, int h);
static void SyncNativeVramToScaled(int x, int y, int w, int h) {
    if ((!scaled_vram_render && !interpolated_vram) || w <= 0 || h <= 0) {
        return;
    }
    /* the midpoint kept so far draws over it first */
    if (interpolated_vram && interpolated_scale == internal_res &&
        InterpMidpointCovers(x, y, w, h))
        InterpMidpointSync(INTERP_SYNC_TARGET);
    SDL_GPUCommandBuffer* cmd = ReserveDrawDescriptors(2);
    if (!cmd) {
        return;
    }
    SDL_GPUTexture *targets[]={scaled_vram_render,interpolated_scale==internal_res?interpolated_vram:NULL};
    for(unsigned i=0;i<2;++i){if(!targets[i])continue;
    const SDL_GPUBlitInfo blit = {
        .source = {.texture = vram_render,
                   .x = (Uint32)x,
                   .y = (Uint32)y,
                   .w = (Uint32)w,
                   .h = (Uint32)h},
        .destination = {.texture = targets[i],
                        .x = (Uint32)(x * internal_res),
                        .y = (Uint32)(y * internal_res),
                        .w = (Uint32)(w * internal_res),
                        .h = (Uint32)(h * internal_res)},
        .load_op = SDL_GPU_LOADOP_LOAD,
        .filter = SDL_GPU_FILTER_NEAREST,
    };
    SDL_BlitGPUTexture(cmd, &blit);
    }
    InterpBackdropAfterFill(x, y, w, h);
}

static void ReleaseTransientDisplayTextures(void) {
    if (!device) return;
    InterpMidpointSync(INTERP_SYNC_SETTINGS);
    if(interpolated_vram){
        if(!SubmitCmdAndWait()||!SDL_WaitForGPUIdle(device))return;
        SDL_ReleaseGPUTexture(device,interpolated_vram);interpolated_vram=NULL;
    }
    interpolated_scale=0;
    Psyz_GpuResetInterpolation();
    if (presentation_display) {
        SDL_ReleaseGPUTexture(device, presentation_display);
        presentation_display = NULL;
        presentation_width = presentation_height = 0;
    }
    if (antialias_display) {
        SDL_ReleaseGPUTexture(device, antialias_display);
        antialias_display = NULL;
        antialias_width = antialias_height = 0;
    }
    if (prescale_display) {
        SDL_ReleaseGPUTexture(device, prescale_display);
        prescale_display = NULL;
        prescale_width = prescale_height = 0;
    }
}

static bool CreateScaledVramTarget(unsigned n) {
    /* Settings changes can otherwise leave several generations resident on
     * a small/shared GPU. Finish the old work before allocating a new pair. */
    ReleaseTransientDisplayTextures();
    if (!SubmitCmdAndWait() || !SDL_WaitForGPUIdle(device))
        return false;
    const SDL_GPUTextureCreateInfo info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage =
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = VRAM_W * n,
        .height = VRAM_H * n,
        .layer_count_or_depth = 1,
        .num_levels = 1,
    };
    SDL_GPUTexture *render = SDL_CreateGPUTexture(device, &info);
    SDL_GPUTextureCreateInfo sample_info = info;
    sample_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture *sample = render ? SDL_CreateGPUTexture(device, &sample_info) : NULL;
    if (!render || !sample) {
        ERRORF("scaled VRAM texture creation failed (%dx): %s", n, SDL_GetError());
        if (render) SDL_ReleaseGPUTexture(device, render);
        if (sample) SDL_ReleaseGPUTexture(device, sample);
        /* Release partial allocations before the next draw needs uniforms. */
        SDL_WaitForGPUIdle(device);
        return false;
    }
    if (scaled_vram_render) SDL_ReleaseGPUTexture(device, scaled_vram_render);
    if (scaled_blend_sample) SDL_ReleaseGPUTexture(device, scaled_blend_sample);
    scaled_vram_render = render;
    scaled_blend_sample = sample;
    SDL_WaitForGPUIdle(device);
    return true;
}

// ensure scaled VRAM does not exceed max allowed texture size
static unsigned AdjustInternalRes(unsigned scale) {
    unsigned n = scale;
    const unsigned max_dim = 16384; // max theoretical texture size
    unsigned vram_dim = VRAM_W > VRAM_H ? VRAM_W : VRAM_H;
    while (n > 1 && vram_dim * n > max_dim) {
        n--;
    }
    if (n != scale) {
        WARNF("internal resolution %dx exceeds max texture size, "
              "fallback to internal resolution %dx",
              scale, n);
    }
    return n;
}

static void ApplyPendingInternalRes(void) {
    unsigned n =
        AdjustInternalRes(set_internal_res * (set_antialiasing ? 2u : 1u));
    bool want_scaled = n > 1 || set_geometry_correction ||
                       set_effect_geometry_correction || set_model_filtering || set_sprite_filtering;
    bool have_scaled = scaled_vram_render != NULL;
    if (n == internal_res && set_geometry_correction == geometry_correction &&
        set_effect_geometry_correction == effect_geometry_correction &&
        set_antialiasing == antialiasing &&
        set_model_filtering == model_filtering && set_sprite_filtering == sprite_filtering && want_scaled == have_scaled) {
        return;
    }
    if (!sdl3_window || !is_platform_init_successful) {
        return;
    }
    Draw_FlushBuffer(); // render pending prims with the old multiplier
    InterpMidpointSync(INTERP_SYNC_SETTINGS);
    Psyz_GpuResetInterpolation();
    if (want_scaled && (n != internal_res || !have_scaled)) {
        bool created = CreateScaledVramTarget(n);
        if (!created) {
            /* Keep the working target and AA mode, rather than losing the
             * framebuffer and retrying an impossible size on each setting. */
            unsigned requested_n = n;
            n = internal_res;
            set_antialiasing = antialiasing;
            set_internal_res = n / (antialiasing ? 2u : 1u);
            if (!have_scaled && requested_n > 1 &&
                (set_geometry_correction || set_effect_geometry_correction ||
                 set_model_filtering || set_sprite_filtering) &&
                CreateScaledVramTarget(1)) {
                have_scaled = true;
                created = true;
            }
            if (!have_scaled) {
                set_geometry_correction = set_effect_geometry_correction =
                    set_model_filtering = set_sprite_filtering = false;
            }
            want_scaled = have_scaled;
        }
        if (created) {
            // upscale the whole native VRAM into the freshly created scaled
            // target
            SDL_GPUCommandBuffer* cmd = ReserveDrawDescriptors(1);
            if (cmd) {
                const SDL_GPUBlitInfo blit = {
                    .source = {.texture = vram_render,
                               .w = VRAM_W,
                               .h = VRAM_H},
                    .destination = {.texture = scaled_vram_render,
                                    .w = VRAM_W * n,
                                    .h = VRAM_H * n},
                    .load_op = SDL_GPU_LOADOP_DONT_CARE,
                    .filter = SDL_GPU_FILTER_NEAREST,
                };
                SDL_BlitGPUTexture(cmd, &blit);
            }
        }
    } else if (!want_scaled && scaled_vram_render) {
        ReleaseTransientDisplayTextures();
        SDL_ReleaseGPUTexture(device, scaled_vram_render);
        scaled_vram_render = NULL;
        if (scaled_blend_sample) {
            SDL_ReleaseGPUTexture(device, scaled_blend_sample);
            scaled_blend_sample = NULL;
        }
    }
    if (!set_antialiasing && antialias_display) {
        SDL_ReleaseGPUTexture(device, antialias_display);
        antialias_display = NULL;
        antialias_width = antialias_height = 0;
    }
    internal_res = n;
    geometry_correction = set_geometry_correction;
    if (geometry_correction || set_effect_geometry_correction) {
        pgxp_trace_budget=4;
        pgxp_mixed_polygons=pgxp_none_polygons=0;
        Psyz_PgxpTraceReasonsEnable(1);
        pgxp_valid_polygons=pgxp_fallback_polygons=pgxp_nonfinite_polygons=0;
    }
    effect_geometry_correction = set_effect_geometry_correction;
    antialiasing = set_antialiasing;
    model_filtering = set_model_filtering;
    sprite_filtering = set_sprite_filtering;
    if (!geometry_correction && !effect_geometry_correction) Psyz_PgxpTraceReasonsEnable(0);
    Psyz_PgxpSetActive(
        !UseSoftwareBackend() &&
        (geometry_correction || effect_geometry_correction ||
         model_filtering || interpolation_enabled));
    Psyz_PgxpSetCulling(geometry_correction);
    Psyz_PgxpSetEffectEnabled(effect_geometry_correction);
    UpdateScissor();
    INFOF("internal resolution set to %dx (%dx%d), geometry_correction=%d", n,
          VRAM_W * n, VRAM_H * n, (int)geometry_correction);
}

/* Display-only title material. Native RGB24 pixels remain the UI authority. */
static SDL_Surface *title_image, *title_base, *title_output;
static u8 *title_reference;
static int title_width, title_height;
static char title_path[4096];
static uint64_t title_hash;
static unsigned title_generation, title_uploaded;
static SDL_GPUTexture *title_texture;
static SDL_GPUTransferBuffer *title_transfer;
static unsigned title_texture_width, title_texture_height;
static bool title_present_active;

int Psyz_VideoSetRgb24Backdrop(const char *filename, const unsigned char *reference,
                             int width, int height, int top, int bottom) {
    if (filename && reference && (width<=0 || width>1024 || height<=0 || top<0 || bottom<0 ||
        height>512 || top>512-height || bottom>512-height-top || strlen(filename)>=sizeof(title_path))) return -1;
    if (filename && title_image && !strcmp(filename,title_path) && reference &&
        width==title_width && top+height+bottom==title_height && top>=0 && height>0 &&
        top+height<=title_height && !memcmp(title_reference+(size_t)top*width*3,reference,(size_t)width*height*3)) return 1;
    if (title_image) SDL_DestroySurface(title_image);
    if (title_base) SDL_DestroySurface(title_base);
    if (title_output) SDL_DestroySurface(title_output);
    title_image=title_base=title_output=NULL;
    free(title_reference);title_reference=NULL;title_path[0]=0;title_hash=0;
    if (!filename || !reference || UseSoftwareBackend()) return 0;
    if (width<=0 || width>1024 || height<=0 || top<0 || bottom<0 ||
        height>512 || top>512-height || bottom>512-height-top || strlen(filename)>=sizeof(title_path)) return -1;
    SDL_Surface *source=LoadHdImage(filename);
    if (!source) return 0;
    if (source->w<=0 || source->w>4096 || source->h<=0 || source->h>4096) {SDL_DestroySurface(source);return -1;}
    title_image=SDL_ConvertSurface(source,SDL_PIXELFORMAT_RGBA32);SDL_DestroySurface(source);
    title_width=width;title_height=top+height+bottom;
    title_reference=calloc((size_t)width*title_height,3);
    if (!title_image || !title_reference) {
        if (title_image) SDL_DestroySurface(title_image);
        title_image=NULL;free(title_reference);title_reference=NULL;return -1;
    }
    memcpy(title_reference+(size_t)top*width*3,reference,(size_t)width*height*3);
    SDL_strlcpy(title_path,filename,sizeof(title_path));return 1;
}

static SDL_Surface *BuildTitleDisplay(const u8 *rgb) {
    if (!sprite_atlases_enabled || !title_image || display_size.x!=title_width || display_size.y!=title_height) return NULL;
    unsigned n=SDL_clamp((unsigned)(title_image->h/title_height),1u,4u);
    const bool title_wide=widescreen;
    unsigned h=title_height*n,w=title_wide?(h*16u+8u)/9u:title_width*n;
    if (!title_output || title_output->w!=(int)w || title_output->h!=(int)h) {
        if (title_base) SDL_DestroySurface(title_base);
        if (title_output) SDL_DestroySurface(title_output);
        title_base=SDL_CreateSurface(w,h,SDL_PIXELFORMAT_RGBA32);
        title_output=SDL_CreateSurface(w,h,SDL_PIXELFORMAT_RGBA32);
        if (!title_base || !title_output) {
            if (title_base) SDL_DestroySurface(title_base);
            if (title_output) SDL_DestroySurface(title_output);
            title_base=title_output=NULL;return NULL;
        }
        SDL_Rect source={0,0,title_image->w,title_image->h};
        if (!title_wide) {
            source.w=SDL_min(source.w,source.h*4/3);source.x=(title_image->w-source.w)/2;
        }
        if (!SDL_BlitSurfaceScaled(title_image,&source,title_base,NULL,SDL_SCALEMODE_LINEAR)) return NULL;
        title_hash=0;
    }
    uint64_t hash=UINT64_C(14695981039346656037);
    for (size_t i=0;i<(size_t)title_width*title_height*3;++i) hash=(hash^rgb[i])*UINT64_C(1099511628211);
    if (title_hash==hash) return title_output;
    if (!SDL_BlitSurface(title_base,NULL,title_output,NULL)) return NULL;
    int margin=((int)w-title_width*(int)n)/2;
    for (int y=0;y<title_height;++y) for (int x=0;x<title_width;++x) {
        size_t at=((size_t)y*title_width+x)*3;
        if (!memcmp(rgb+at,title_reference+at,3)) continue;
        u8 color[4]={rgb[at],rgb[at+1],rgb[at+2],255};u32 value;memcpy(&value,color,4);
        for (unsigned dy=0;dy<n;++dy) {
            u32 *row=(u32 *)((u8 *)title_output->pixels+(y*n+dy)*title_output->pitch)+margin+x*n;
            for (unsigned dx=0;dx<n;++dx) row[dx]=value;
        }
    }
    title_hash=hash;++title_generation;return title_output;
}

static SDL_GPUTexture *UploadTitleDisplay(SDL_Surface *surface) {
    if (!title_texture || !title_transfer || title_texture_width!=(unsigned)surface->w || title_texture_height!=(unsigned)surface->h) {
        if (title_texture) SDL_ReleaseGPUTexture(device,title_texture);
        if (title_transfer) SDL_ReleaseGPUTransferBuffer(device,title_transfer);
        title_texture=NULL;title_transfer=NULL;title_uploaded=0;
        SDL_GPUTextureCreateInfo info={.type=SDL_GPU_TEXTURETYPE_2D,.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage=SDL_GPU_TEXTUREUSAGE_SAMPLER,.width=surface->w,.height=surface->h,.layer_count_or_depth=1,.num_levels=1};
        title_texture=SDL_CreateGPUTexture(device,&info);
        SDL_GPUTransferBufferCreateInfo transfer={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=surface->w*surface->h*4};
        title_transfer=title_texture?SDL_CreateGPUTransferBuffer(device,&transfer):NULL;
        if (!title_texture || !title_transfer) {
            if (title_texture) SDL_ReleaseGPUTexture(device,title_texture);
            if (title_transfer) SDL_ReleaseGPUTransferBuffer(device,title_transfer);
            title_texture=NULL;title_transfer=NULL;
            title_texture_width=title_texture_height=0;
            return NULL;
        }
        title_texture_width=surface->w;title_texture_height=surface->h;
    }
    if (!title_texture || !title_transfer) return NULL;
    if (title_uploaded==title_generation) return title_texture;
    u8 *mapped=SDL_MapGPUTransferBuffer(device,title_transfer,true);if (!mapped) return NULL;
    for (int y=0;y<surface->h;++y) memcpy(mapped+(size_t)y*surface->w*4,(u8 *)surface->pixels+(size_t)y*surface->pitch,(size_t)surface->w*4);
    SDL_UnmapGPUTransferBuffer(device,title_transfer);
    SDL_GPUCommandBuffer *cmd=AcquireCmd();if (!cmd) return NULL;
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);if (!copy) return NULL;
    SDL_GPUTextureTransferInfo src={.transfer_buffer=title_transfer,.pixels_per_row=surface->w,.rows_per_layer=surface->h};
    SDL_GPUTextureRegion dst={.texture=title_texture,.w=surface->w,.h=surface->h,.d=1};
    SDL_UploadToGPUTexture(copy,&src,&dst,false);SDL_EndGPUCopyPass(copy);title_uploaded=title_generation;
    return title_texture;
}

static SDL_GPUTexture* PrepareRgb24Display(void) {
    u8* rgb = Sdl3Common_ReadRgb24(
        display_area.x, display_area.y, display_size.x, display_size.y);
    title_present_active=false;
    if (!rgb)
        return NULL;
    SDL_Surface *enhanced=BuildTitleDisplay(rgb);
    if (enhanced) {
        SDL_GPUTexture *texture=UploadTitleDisplay(enhanced);
        if (texture) {free(rgb);title_present_active=true;return texture;}
    }
    if (!rgb24_display) {
        SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                     SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            .width = VRAM_W,
            .height = VRAM_H,
            .layer_count_or_depth = 1,
            .num_levels = 1};
        rgb24_display = SDL_CreateGPUTexture(device, &info);
    }
    Uint32 upload_at = 0;
    u8* mapped =
        rgb24_display
            ? UploadMap((Uint32)(display_size.x * display_size.y * 4), &upload_at)
            : NULL;
    if (!mapped) {
        free(rgb);
        return NULL;
    }
    for (int i = 0; i < display_size.x * display_size.y; ++i) {
        memcpy(mapped + i * 4, rgb + i * 3, 3);
        mapped[i * 4 + 3] = 255;
    }
    free(rgb);
    SDL_UnmapGPUTransferBuffer(device, tex_upload_transfer);
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    if (!cmd)
        return NULL;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {.transfer_buffer = tex_upload_transfer,
                                      .offset = upload_at,
                                      .pixels_per_row = display_size.x,
                                      .rows_per_layer = display_size.y};
    SDL_GPUTextureRegion dst = {.texture = rgb24_display,
                                .w = display_size.x,
                                .h = display_size.y,
                                .d = 1};
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    return rgb24_display;
}

/* Normalize both framebuffer origins before scaling. SSAA is resolved here,
 * at the requested render resolution, so integer-aligned 2D texels stay sharp.
 */
static SDL_GPUBlitRegion PrepareDisplayTexture(
    SDL_GPUCommandBuffer* cmd, SDL_GPUBlitRegion src) {
    if (!presentation_display || presentation_width != src.w ||
        presentation_height != src.h) {
        if (presentation_display)
            SDL_ReleaseGPUTexture(device, presentation_display);
        const SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = src.w,
            .height = src.h,
            .layer_count_or_depth = 1,
            .num_levels = 1,
        };
        presentation_display = SDL_CreateGPUTexture(device, &info);
        presentation_width = presentation_display ? src.w : 0;
        presentation_height = presentation_display ? src.h : 0;
    }
    if (!presentation_display)
        return src;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTextureLocation from = {
        .texture = src.texture, .x = src.x, .y = src.y};
    const SDL_GPUTextureLocation to = {.texture = presentation_display};
    SDL_CopyGPUTextureToTexture(copy, &from, &to, src.w, src.h, 1, false);
    SDL_EndGPUCopyPass(copy);
    src.texture = presentation_display;
    src.x = src.y = 0;
    if (antialiasing) {
        Uint32 width = src.w / 2, height = src.h / 2;
        if (!antialias_display || antialias_width != width ||
            antialias_height != height) {
            if (antialias_display)
                SDL_ReleaseGPUTexture(device, antialias_display);
            const SDL_GPUTextureCreateInfo info = {
                .type = SDL_GPU_TEXTURETYPE_2D,
                .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                         SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                .width = width,
                .height = height,
                .layer_count_or_depth = 1,
                .num_levels = 1,
            };
            antialias_display = SDL_CreateGPUTexture(device, &info);
            antialias_width = antialias_display ? width : 0;
            antialias_height = antialias_display ? height : 0;
        }
        if (antialias_display) {
            const SDL_GPUBlitInfo resolve = {
                .source = src,
                .destination = {.texture = antialias_display,
                                .w = width,
                                .h = height},
                .load_op = SDL_GPU_LOADOP_DONT_CARE,
                .filter = SDL_GPU_FILTER_LINEAR};
            SDL_BlitGPUTexture(cmd, &resolve);
            src = (SDL_GPUBlitRegion){
                .texture = antialias_display, .w = width, .h = height};
        }
    }
    return src;
}

/* Sharp bilinear for the widescreen stretch: a whole-number nearest prescale
 * (kx, ky) of the buffer, so the final linear blit only shrinks and pixel
 * edges stay crisp. x0..x1, y0..y1 is the part to show, in buffer pixels
 * from the buffer's corner: the whole pixels around it are prescaled and
 * the returned region picks it out to 1/k of a pixel, so the room can be
 * shown less than a buffer pixel from where it is drawn. */
static SDL_GPUBlitRegion PrescaleWidescreen(
    SDL_GPUCommandBuffer* cmd, SDL_GPUBlitRegion src, float x0, float y0,
    float x1, float y1, Uint32 kx, Uint32 ky) {
    x0 = SDL_clamp(x0, 0.0f, (float)src.w);
    x1 = SDL_clamp(x1, x0, (float)src.w);
    y0 = SDL_clamp(y0, 0.0f, (float)src.h);
    y1 = SDL_clamp(y1, y0, (float)src.h);
    const Uint32 ix0 = (Uint32)SDL_floorf(x0), ix1 = (Uint32)SDL_ceilf(x1);
    const Uint32 iy0 = (Uint32)SDL_floorf(y0), iy1 = (Uint32)SDL_ceilf(y1);
    if (ix1 <= ix0 || iy1 <= iy0)
        return src;
    kx = SDL_max(kx, 1u);
    ky = SDL_max(ky, 1u);
    const SDL_GPUBlitRegion whole = {
        .texture = src.texture,
        .x = src.x + (Uint32)SDL_lroundf(x0),
        .y = src.y + (Uint32)SDL_lroundf(y0),
        .w = SDL_max((Uint32)SDL_lroundf(x1 - x0), 1u),
        .h = SDL_max((Uint32)SDL_lroundf(y1 - y0), 1u)};
    if (kx <= 1 && ky <= 1)
        return whole;
    const Uint32 width = src.w * kx, height = src.h * ky;
    if (!prescale_display || prescale_width != width ||
        prescale_height != height) {
        if (prescale_display)
            SDL_ReleaseGPUTexture(device, prescale_display);
        const SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                     SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            .width = width,
            .height = height,
            .layer_count_or_depth = 1,
            .num_levels = 1,
        };
        prescale_display = SDL_CreateGPUTexture(device, &info);
        prescale_width = prescale_display ? width : 0;
        prescale_height = prescale_display ? height : 0;
    }
    if (!prescale_display)
        return whole;
    const Uint32 pw = (ix1 - ix0) * kx, ph = (iy1 - iy0) * ky;
    const SDL_GPUBlitInfo prescale = {
        .source = {.texture = src.texture,
                   .x = src.x + ix0,
                   .y = src.y + iy0,
                   .w = ix1 - ix0,
                   .h = iy1 - iy0},
        .destination = {.texture = prescale_display, .w = pw, .h = ph},
        .load_op = SDL_GPU_LOADOP_DONT_CARE,
        .filter = SDL_GPU_FILTER_NEAREST};
    SDL_BlitGPUTexture(cmd, &prescale);
    const Uint32 px =
        SDL_min((Uint32)SDL_lroundf((x0 - (float)ix0) * (float)kx), pw - 1);
    const Uint32 py =
        SDL_min((Uint32)SDL_lroundf((y0 - (float)iy0) * (float)ky), ph - 1);
    return (SDL_GPUBlitRegion){
        .texture = prescale_display,
        .x = px,
        .y = py,
        .w = SDL_clamp((Uint32)SDL_lroundf((x1 - x0) * (float)kx), 1u, pw - px),
        .h =
            SDL_clamp((Uint32)SDL_lroundf((y1 - y0) * (float)ky), 1u, ph - py)};
}


/* SDL's D3D12 ResizeBuffers failure path frees the old views before it knows
 * whether resizing succeeded. Recreate the claimed swapchain at a frame
 * boundary instead, while its old views are still valid. Keep VRAM intact. */
static bool RefreshWindowSwapchain(void) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(sdl3_window, &w, &h);
    SDL_WindowFlags fullscreen = SDL_GetWindowFlags(sdl3_window) & SDL_WINDOW_FULLSCREEN;
    if (w <= 0 || h <= 0) return false;
    if (swapchain_ok && !SDL_GetAtomicInt(&swapchain_resize_pending) &&
        w == swapchain_window_w && h == swapchain_window_h &&
        fullscreen == swapchain_window_fullscreen) return true;
    Draw_FlushBuffer();
    if (!SubmitCmdAndWait() || !SDL_WaitForGPUIdle(device)) return false;
    if (swapchain_ok) SDL_ReleaseWindowFromGPUDevice(device, sdl3_window);
    SDL_SetAtomicInt(&swapchain_resize_pending, 0);
    swapchain_ok = SDL_ClaimWindowForGPUDevice(device, sdl3_window);
    if (!swapchain_ok) {
        ERRORF("could not recreate window swapchain: %s", SDL_GetError());
        return false;
    }
    swapchain_present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    swapchain_window_w = w;
    swapchain_window_h = h;
    swapchain_window_fullscreen = fullscreen;
    PlatformBackend_SetDriverVsync(use_driver_vsync || present_sync);
    return swapchain_ok;
}

static void UpdateNativeRasterizer(void);
static void PresentNativeVram(void);
void Psyz_GpuResendDrawState(void); /* libgpu: E1h..E6h to the new backend */

static void PlatformBackend_Present(void) {
    frame_presented=false;present_commit_pending=false;
    if(host_clock_pause_depth||SDL_GetAtomicInt(&app_in_background)){present_window_start=0;present_window_frames=present_window_mid=0;actual_present_fps=0;Psyz_GpuResetInterpolation();}
    if (gpu_failed) return;
    UpdateNativeRasterizer();
    if (SoftwareOnly()) {
        finish_time = SDL_GetPerformanceCounter();
        return;
    }
    if (!sdl3_window && !InitPlatform()) {
        finish_time = SDL_GetPerformanceCounter();
        return;
    }
    if (!ws_capture_active && Sdl3Common_IsInBackground()) {
        finish_time = SDL_GetPerformanceCounter();
        return;
    }

    DRAW_FLUSH_FOR(DRAW_FLUSH_PRESENT);
    /* the midpoint may be shown: it is complete */
    if (interp_pair_ready)
        InterpMidpointSync(INTERP_SYNC_PRESENT);
    ApplyPendingInternalRes();
    if (native_rasterizer)
        PresentNativeVram();
    bool can_present = swapchain_ok && !present_skip;
    if (!ws_capture_active && !hidden_gpu_renderer &&
        !SDL_strcmp(SDL_GetGPUDeviceDriver(device), "direct3d12") &&
        !RefreshWindowSwapchain()) {
        can_present = false;
    }
    /* a capture shows what the last present showed */
    if(!ws_capture_active){
        const bool mid_ready=interp_pair_ready&&interpolation_enabled&&interpolated_vram&&
            interpolated_scale==internal_res&&interp_next_y>=0&&interp_next_y<=VRAM_H-display_size.y&&
            !display_rgb24&&
            !debug_show_vram&&game_speed==1&&
            /* decided when it is shown: a paced present draws the frame
             * during the wait before */
            !InterpMidpointUnsound();
        interp_present_mid=mid_ready&&(interp_blank_mid?present_blank_poll:interp_phase++==1);
        /* Unpaced (headless, limitless) runs poll no blank: they count the
         * midpoint a paced present would have shown (captures, tests). */
        if(interp_blank_mid&&mid_ready&&!present_blank_poll&&!PacedPresents())++interpolated_present_count;
        /* a blank midpoint is shown once, or not after a new frame */
        if(interp_blank_mid){interp_blank_mid=false;interp_pair_ready=false;}
        if(interp_present_mid)++interpolated_present_count;
        present_midpoint=interp_present_mid;
    }

    if(display_rgb24||
       game_speed!=1)Psyz_GpuResetInterpolation();
    SDL_GPUTexture* rgb24 =
        disp_on && display_rgb24 && !debug_show_vram && (ws_capture_active || can_present)
            ? PrepareRgb24Display()
            : NULL;
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    if (!cmd) {
        finish_time = SDL_GetPerformanceCounter();
        return;
    }
    // a skipped frame still submits its drawing, without a swapchain image
    if (ws_capture_active || can_present) {
        SDL_GPUTexture* swapchain = NULL;
        Uint32 sc_w = 0, sc_h = 0;
        if (ws_capture_active) {
            swapchain = ws_capture;
            sc_w = ws_capture_width;
            sc_h = ws_capture_height;
        } else if (!SDL_WaitAndAcquireGPUSwapchainTexture(
                       cmd, sdl3_window, &swapchain, &sc_w, &sc_h)) {
            WARNF("SDL_WaitAndAcquireGPUSwapchainTexture: %s", SDL_GetError());
        }
        if (swapchain) {
            if (!ws_capture_active){frame_presented=true;present_commit_pending=!host_clock_pause_depth;present_commit_mid=interp_present_mid;}
            if (!disp_on) {
                const SDL_GPUColorTargetInfo blank_target = {
                    .texture = swapchain,
                    .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
                    .load_op = SDL_GPU_LOADOP_CLEAR,
                    .store_op = SDL_GPU_STOREOP_STORE,
                };
                SDL_GPURenderPass* blank_pass =
                    SDL_BeginGPURenderPass(cmd, &blank_target, 1, NULL);
                SDL_EndGPURenderPass(blank_pass);
            } else {
                const Uint32 n = internal_res;
                SDL_GPUBlitRegion src = {
                    .texture = interp_present_mid ? interpolated_vram : GetRenderTarget(),
                    .x = (Uint32)display_area.x * n,
                    .y = (Uint32)(interp_present_mid?interp_next_y:display_area.y) * n,
                    .w = (Uint32)display_size.x * n,
                    .h = (Uint32)display_size.y * n,
                };
                if (rgb24)
                    src = (SDL_GPUBlitRegion){.texture = rgb24,
                                              .w = title_present_active ? title_texture_width : display_size.x,
                                              .h = title_present_active ? title_texture_height : display_size.y};
                float game_aspect = PresentGameAspect(
                    interp_present_mid ? interp_next_y : display_area.y, rgb24 != NULL);
                if (rgb24 && title_present_active && widescreen) game_aspect=16.0f/9.0f;
                if (debug_show_vram) {
                    src.x = 0;
                    src.y = 0;
                    src.w = VRAM_W * n;
                    src.h = VRAM_H * n;
                    game_aspect = (float)VRAM_W / (float)VRAM_H;
                }

                if (!rgb24 && !debug_show_vram)
                    src = PrepareDisplayTexture(cmd, src);

                WndSize win = {(int)sc_w, (int)sc_h};
                SDL_Rect dst = FitGameToWindow(game_aspect, win);
                bool ws_view = false;

                /* Widescreen: the buffer holds 1 / squeeze of the screen's width,
                 * around the screen's centre plus the view's shift (see
                 * Psyz_VideoSetWidescreenPicture). A 16:9 window of it (for
                 * the screen's pixel aspect) is shown around that point. Past
                 * the picture the swapchain clear leaves black; a room
                 * narrower than the window shows 4:3, with even bars. */
                if (widescreen && !rgb24 && !debug_show_vram &&
                    !WidescreenNativeBuffer(interp_present_mid ? interp_next_y : display_area.y) &&
                    game_aspect > 0.0f) {
                    const float w = (float)display_size.x, centre = w * 0.5f;
                    int picture = -1;
                    for (int i = 0; i < 2; ++i) {
                        if (ws_picture[i].y == (interp_present_mid?interp_next_y:display_area.y))
                            picture = i;
                    }
                    const float squeeze = WidescreenPictureSqueeze(picture);
                    const float buffer_half = centre / squeeze;
                    const float half = WidescreenViewHalf(game_aspect, squeeze);
                    const float shift=picture>=0 ? (interp_present_mid?ws_mid_shift[picture]:(float)ws_picture[picture].shift) : 0;
                    const float pivot = WidescreenPivot(shift, squeeze);
                    /* the room is shown where it belongs: the rest of its
                     * shift past the step drawn (WidescreenFieldStep) */
                    float rest_x = 0.0f, rest_y = 0.0f, ends_x = 0.0f;
                    if (picture >= 0 && ws_picture[picture].field) {
                        float qx, qy;
                        WidescreenFieldStep(picture, (float)n, &qx, &qy);
                        if(interp_present_mid){qx=ws_mid_qx[picture];qy=ws_mid_qy[picture];}
                        float ex=interp_present_mid?ws_mid_ex[picture]:ws_picture[picture].ex;
                        float ey=interp_present_mid?ws_mid_ey[picture]:ws_picture[picture].ey;
                        rest_x = ex - qx;
                        rest_y = ey - qy;
                        if (ws_picture[picture].ends_move)
                            ends_x = ex;
                    }
                    float left = centre + (float)shift - buffer_half;
                    float right = centre + (float)shift + buffer_half;
                    if (picture >= 0 &&
                        ws_picture[picture].left < ws_picture[picture].right) {
                        left = SDL_max(
                            left, (interp_present_mid?ws_mid_left[picture]:(float)ws_picture[picture].left) + ends_x);
                        right = SDL_min(
                            right, (interp_present_mid?ws_mid_right[picture]:(float)ws_picture[picture].right) + ends_x);
                    }
                    const float view_l = centre + (float)shift - half;
                    const float view_r = centre + (float)shift + half;
                    const float shown_l = SDL_max(view_l, left);
                    const float shown_r =
                        SDL_max(shown_l, SDL_min(view_r, right));
                    /* game x -> fraction of the squeezed buffer */
                    const float f0 =
                        (pivot + (shown_l - rest_x - pivot) * squeeze) / w;
                    const float f1 =
                        (pivot + (shown_r - rest_x - pivot) * squeeze) / w;
                    dst = FitGameToWindow(16.0f / 9.0f, win);
                    /* whole prescale factors from the view's size, so they
                     * hold while a picture end moves through the view */
                    const float view_w =
                        2.0f * half * squeeze / w * (float)src.w;
                    const Uint32 kx = (Uint32)SDL_max(
                        1.0f, SDL_ceilf((float)dst.w / view_w - 0.001f));
                    const Uint32 ky =
                        src.h > 2 ? ((Uint32)dst.h + src.h - 3) / (src.h - 2)
                                  : 1;
                    const int dx0 =
                        dst.x + (int)SDL_lroundf((shown_l - view_l) /
                                                 (2.0f * half) * (float)dst.w);
                    const int dx1 =
                        dst.x + (int)SDL_lroundf((shown_r - view_l) /
                                                 (2.0f * half) * (float)dst.w);
                    float sx0 = f0 * (float)src.w, sx1 = f1 * (float)src.w;
                    if (sx1 > sx0 && dx1 > dx0) {
                        dst.x = dx0;
                        dst.w = dx1 - dx0;
                    } else {
                        sx0 = 0.0f;
                        sx1 = (float)src.w;
                    }
                    /* a buffer pixel off the top and bottom leaves room to
                     * show the room up to half a pixel up or down */
                    const float sy = rest_y * (float)n;
                    ws_view = true;
                    src = PrescaleWidescreen(cmd, src, sx0, 1.0f - sy, sx1,
                                             (float)src.h - 1.0f - sy,
                                             kx, ky
                                             );
                }

                TraceAspectViewport(dst, win, rgb24 != NULL, ws_view);

                const SDL_GPUBlitInfo blit = {
                    .source = src,
                    .destination =
                        {
                            .texture = swapchain,
                            .x = (Uint32)dst.x,
                            .y = (Uint32)dst.y,
                            .w = (Uint32)dst.w,
                            .h = (Uint32)dst.h,
                        },
                    // clear the swapchain to black first so the horizontal or
                    // vertical bars around the game output are black
                    .load_op =
                        SDL_GPU_LOADOP_CLEAR,
                    .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
                    // the widescreen stretch is 1 / squeeze per game pixel: nearest
                    // would make every third column wider, so the view is
                    // prescaled and only this last step is linear
                    .filter = ws_view ? SDL_GPU_FILTER_LINEAR
                                      : SDL_GPU_FILTER_NEAREST,
                };
                SDL_BlitGPUTexture(cmd, &blit);
            }
            if (!ws_capture_active && overlay_frame_cb) {
                overlay_frame_cb();
            }
            if (!ws_capture_active && overlay_render_cb) {
                SDL_GPUColorTargetInfo target = {
                    .texture = swapchain,
                    .load_op = SDL_GPU_LOADOP_LOAD,
                    .store_op = SDL_GPU_STOREOP_STORE,
                };
                SDL_GPURenderPass* pass =
                    SDL_BeginGPURenderPass(cmd, &target, 1, NULL);
                overlay_render_cb(cmd, pass);
                SDL_EndGPURenderPass(pass);
            }
        } else if (!ws_capture_active && overlay_frame_cb) {
            overlay_frame_cb();
        }
    } else if (overlay_frame_cb) {
        overlay_frame_cb();
    }
    finish_time = SDL_GetPerformanceCounter();
    if(pgxp_trace_budget && (pgxp_valid_polygons || pgxp_fallback_polygons)) {
        INFOF("GPU precision sample requested_scale=%u physical_scale=%u AA=%d geometry_requested=%d geometry_effective=%d effect_requested=%d effect_effective=%d filtering=%d polygons_precision=%llu polygons_fallback=%llu polygons_nonfinite=%llu (fallback also includes ordinary2D polygons; model jitter cause unconfirmed)",
            set_internal_res,internal_res,antialiasing,set_geometry_correction,geometry_correction,set_effect_geometry_correction,effect_geometry_correction,model_filtering,
            pgxp_valid_polygons,pgxp_fallback_polygons,pgxp_nonfinite_polygons);
        PsyzPgxpReasons reasons;
        Psyz_PgxpTraceReasonsTake(&reasons);
        INFOF("GPU precision reasons matrix_high=%llu matrix_integer=%llu xy_clip=%llu xy_disagree=%llu lookup_hit=%llu lookup_miss=%llu copy_hit=%llu copy_miss=%llu polygon_mixed=%llu polygon_none=%llu (lookups include ordinary2D; model identity unobserved)",
            reasons.matrix_high,reasons.matrix_integer,reasons.xy_clip,reasons.xy_disagree,
            reasons.lookup_hit,reasons.lookup_miss,reasons.copy_hit,reasons.copy_miss,
            pgxp_mixed_polygons,pgxp_none_polygons);
        for(unsigned i=0;i<reasons.samples;++i)
            INFOF("GPU precision rejection sample=%u reason_bits=%u projected=%.5f,%.5f integer=%.5f,%.5f depth=%.5f (clip=1 disagreement=2)",i,reasons.sample[i].reason,
                reasons.sample[i].expected_x,reasons.sample[i].expected_y,
                reasons.sample[i].actual_x,reasons.sample[i].actual_y,reasons.sample[i].depth);
        pgxp_mixed_polygons=pgxp_none_polygons=0;
        --pgxp_trace_budget;
        if(!pgxp_trace_budget)Psyz_PgxpTraceReasonsEnable(0);
        pgxp_valid_polygons=pgxp_fallback_polygons=pgxp_nonfinite_polygons=0;
    }
    SubmitFrame();
}

static void QuitPlatform(void) {
    SDL_RemoveEventWatch(WatchSwapchainResize, NULL);
    if (device) {
        /* Retire before overlay destroy, window release, SDL_Quit or any GPU
         * dependency destruction. Failure preserves the entire graph. */
        if (!RetireGpu()) {
            is_platform_init_successful = false;
            ERRORF("GPU cleanup quarantined; first=%s; no device/window/overlay destruction", gpu_first_error);
            return;
        }
        Draw_ResetBuffer();
    }
    Sdl3Common_Shutdown();
    if (overlay_destroy_cb) {
        overlay_destroy_cb();
    }
    if (device) {
        ReleaseFrameFences(); /* global idle already established */
        ReleaseRingFences();
        ReleaseWaitedFences();
        if (sprite_atlas_texture) SDL_ReleaseGPUTexture(device,sprite_atlas_texture);
        sprite_atlas_texture=NULL;sprite_atlas_cells=0;
        sprite_atlas_count=0;sprite_atlas_bytes=0;next_sprite_atlas=NULL;
        if (sprite_atlas_sampler) SDL_ReleaseGPUSampler(device,sprite_atlas_sampler);
        sprite_atlas_sampler=NULL;
        if (ws_capture)
            SDL_ReleaseGPUTexture(device, ws_capture);
        ws_capture = NULL;
        ws_capture_width = ws_capture_height = 0;
        ws_capture_active = false;
        for (int i = 0; i < BLEND_COUNT; i++) {
            if (pipe_tri[i]) {
                SDL_ReleaseGPUGraphicsPipeline(device, pipe_tri[i]);
                pipe_tri[i] = NULL;
            }
        }
        if (pipe_direct_fetch)
            SDL_ReleaseGPUGraphicsPipeline(device, pipe_direct_fetch);
        pipe_direct_fetch = NULL;
        destination_fetch = false;
        if (pipe_clear) {
            SDL_ReleaseGPUGraphicsPipeline(device, pipe_clear);
            pipe_clear = NULL;
        }
        Psyz_VideoSetRgb24Backdrop(NULL,NULL,0,0,0,0);
        if (title_texture) SDL_ReleaseGPUTexture(device,title_texture);
        if (title_transfer) SDL_ReleaseGPUTransferBuffer(device,title_transfer);
        title_texture=NULL;title_transfer=NULL;title_uploaded=title_generation=0;
        title_texture_width=title_texture_height=0;
        ReleaseTransientDisplayTextures();
        Psyz_GpuResetInterpolation();
        if (rgb24_display) {
            SDL_ReleaseGPUTexture(device, rgb24_display);
            rgb24_display = NULL;
        }
        if (vram_render) {
            SDL_ReleaseGPUTexture(device, vram_render);
            vram_render = NULL;
        }
        if (vram_sample) {
            SDL_ReleaseGPUTexture(device, vram_sample);
            vram_sample = NULL;
        }
        if (blend_sample) {
            SDL_ReleaseGPUTexture(device, blend_sample);
            blend_sample = NULL;
        }
        if (clut_cache) {
            SDL_ReleaseGPUTexture(device, clut_cache);
            clut_cache = NULL;
        }
        if (scaled_blend_sample) {
            SDL_ReleaseGPUTexture(device, scaled_blend_sample);
            scaled_blend_sample = NULL;
        }
        if (scaled_vram_render) {
            SDL_ReleaseGPUTexture(device, scaled_vram_render);
            scaled_vram_render = NULL;
        }
        if (vram_sampler) {
            SDL_ReleaseGPUSampler(device, vram_sampler);
            vram_sampler = NULL;
        }
        if (vbuf) {
            SDL_ReleaseGPUBuffer(device, vbuf);
            vbuf = NULL;
        }
        if (ibuf) {
            SDL_ReleaseGPUBuffer(device, ibuf);
            ibuf = NULL;
        }
        if (clear_vbuf) {
            SDL_ReleaseGPUBuffer(device, clear_vbuf);
            clear_vbuf = NULL;
        }
        if (vtx_transfer) {
            SDL_ReleaseGPUTransferBuffer(device, vtx_transfer);
            vtx_transfer = NULL;
            ring_restart = true;
        }
        if (tex_upload_transfer) {
            SDL_ReleaseGPUTransferBuffer(device, tex_upload_transfer);
            tex_upload_transfer = NULL;
        }
        if (tex_download_transfer) {
            SDL_ReleaseGPUTransferBuffer(device, tex_download_transfer);
            tex_download_transfer = NULL;
        }
        if (swapchain_ok) {
            SDL_ReleaseWindowFromGPUDevice(device, sdl3_window);
            swapchain_ok = false;
        }
        SDL_DestroyGPUDevice(device);
        device = NULL;
        command_descriptors = 0;
        descriptor_rollover_reported = false;
    }
    if (sdl3_window) {
        SDL_DestroyWindow(sdl3_window);
        sdl3_window = NULL;
        is_window_visible = false;
    }
    SDL_Quit();
    is_platform_initialized = false;
    is_platform_init_successful = false;
}


void ResetPlatform(void) {
    cur_tpage = 0;
    internal_res = 1;
    geometry_correction = false;
    set_geometry_correction = false;
    sprite_filtering = set_sprite_filtering = false;
    antialiasing = set_antialiasing = model_filtering = set_model_filtering =
        false;
    Psyz_PgxpSetActive(0);
    QuitPlatform();
}

static bool DownloadVramRegionAsRGBA8888(int x, int y, int w, int h, u8* out) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x >= VRAM_W || y >= VRAM_H ||
        w > VRAM_W - x || h > VRAM_H - y)
        return false;
    if (UseSoftwareBackend()) {
        u16* words = malloc((size_t)w * h * sizeof(*words));
        if (!words)
            return false;
        PS1_RECT region = {x, y, w, h};
        software_rasterizer.store_image(&region, (u_long*)words);
        ConvertRgb5551ToRgba8888(words, out, (size_t)w * h);
        free(words);
        return true;
    }
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    if (!cmd) {
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTextureRegion region = {
        .texture = vram_render,
        .x = (Uint32)x,
        .y = (Uint32)y,
        .w = (Uint32)w,
        .h = (Uint32)h,
        .d = 1,
    };
    const SDL_GPUTextureTransferInfo transfer = {
        .transfer_buffer = tex_download_transfer,
    };
    SDL_DownloadFromGPUTexture(copy, &region, &transfer);
    SDL_EndGPUCopyPass(copy);
    if (!SubmitCmdAndWait())
        return false;

    const u8* map =
        SDL_MapGPUTransferBuffer(device, tex_download_transfer, false);
    if (!map) {
        ERRORF("SDL_MapGPUTransferBuffer: %s", SDL_GetError());
        return false;
    }
    memcpy(out, map, (size_t)w * h * 4);
    SDL_UnmapGPUTransferBuffer(device, tex_download_transfer);
    return true;
}

static unsigned char* AllocRgb888Region(int x, int y, int w, int h) {
    if (!UseSoftwareBackend() && (!device || !vram_render)) {
        ERRORF("GPU device not initialized");
        return NULL;
    }

    unsigned char* pixels = malloc((size_t)w * h * 3);
    u8* rgba = malloc((size_t)w * h * 4);
    if (!pixels || !rgba) {
        free(pixels);
        free(rgba);
        return NULL;
    }

    if (!DownloadVramRegionAsRGBA8888(x, y, w, h, rgba)) {
        free(pixels);
        free(rgba);
        return NULL;
    }
    const u8* src = rgba;
    unsigned char* dst = pixels;
    for (int i = 0; i < w * h; i++) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst += 3;
        src += 4;
    }
    free(rgba);
    return pixels;
}

unsigned char* Psyz_VideoAllocCapturedFrame(int* w, int* h) {
    *w = display_size.x;
    *h = display_size.y;
    if (display_rgb24)
        return Sdl3Common_ReadRgb24(display_area.x, display_area.y, *w, *h);
    return AllocRgb888Region(display_area.x, display_area.y, *w, *h);
}

unsigned char* Psyz_VideoAllocVramDump(int* w, int* h) {
    *w = VRAM_W;
    *h = VRAM_H;
    return AllocRgb888Region(0, 0, VRAM_W, VRAM_H);
}

unsigned char* Psyz_VideoAllocScaledVramDump(int* w, int* h) {
    *w = *h = 0;
    if (!InitPlatform())
        return NULL;
    Draw_FlushBuffer();
    if (UseSoftwareBackend() || !scaled_vram_render) {
        u8* rgba = malloc((size_t)VRAM_W * VRAM_H * 4);
        if (rgba && !DownloadVramRegionAsRGBA8888(0, 0, VRAM_W, VRAM_H, rgba)) {
            free(rgba);
            return NULL;
        }
        if (rgba)
            *w = VRAM_W, *h = VRAM_H;
        return rgba;
    }
    const unsigned width = VRAM_W * internal_res, height = VRAM_H * internal_res;
    const size_t bytes = (size_t)width * height * 4;
    u8* rgba = malloc(bytes);
    SDL_GPUTransferBufferCreateInfo info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = (Uint32)bytes};
    SDL_GPUTransferBuffer* transfer =
        rgba ? SDL_CreateGPUTransferBuffer(device, &info) : NULL;
    SDL_GPUCommandBuffer* cmd = transfer ? AcquireCmd() : NULL;
    SDL_GPUCopyPass* copy = cmd ? SDL_BeginGPUCopyPass(cmd) : NULL;
    if (!copy) {
        free(rgba);
        if (transfer)
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        return NULL;
    }
    const SDL_GPUTextureRegion region = {
        .texture = scaled_vram_render, .w = width, .h = height, .d = 1};
    const SDL_GPUTextureTransferInfo destination = {.transfer_buffer = transfer};
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);
    const u8* map = SubmitCmdAndWait()
                        ? SDL_MapGPUTransferBuffer(device, transfer, false)
                        : NULL;
    if (map) {
        memcpy(rgba, map, bytes);
        SDL_UnmapGPUTransferBuffer(device, transfer);
        *w = (int)width;
        *h = (int)height;
    } else {
        free(rgba);
        rgba = NULL;
    }
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return rgba;
}

unsigned char* Psyz_VideoAllocRenderedFrame(int* w, int* h);
unsigned char* Psyz_VideoAllocMidpointFrame(int* w, int* h) {
    InterpMidpointComplete(INTERP_SYNC_PRESENT);
    if (w)
        *w = 0;
    if (h)
        *h = 0;
    if (!interpolation_enabled || !interpolated_vram || interpolated_scale != internal_res ||
        !interp_serial || display_area.y < 0 || display_area.y > VRAM_H - display_size.y)
        return NULL;
    const bool mid = interp_present_mid;
    const int next = interp_next_y;
    interp_present_mid = true;
    interp_next_y = display_area.y;
    unsigned char* pixels = Psyz_VideoAllocRenderedFrame(w, h);
    interp_present_mid = mid;
    interp_next_y = next;
    return pixels;
}
unsigned char* Psyz_VideoAllocRenderedFrame(int* w, int* h) {
    if (w)
        *w = 0;
    if (h)
        *h = 0;
    if (!w || !h)
        return NULL;
    if (!InitPlatform())
        return NULL;
    Draw_FlushBuffer();
    if (!UseSoftwareBackend())
        ApplyPendingInternalRes();
    if (!UseSoftwareBackend() && display_rgb24 && title_image) {
        u8 *native=Sdl3Common_ReadRgb24(display_area.x,display_area.y,display_size.x,display_size.y);
        SDL_Surface *enhanced=native?BuildTitleDisplay(native):NULL;free(native);
        if (enhanced) {
            u8 *pixels=malloc((size_t)enhanced->w*enhanced->h*3);if (!pixels) return NULL;
            for (int y=0;y<enhanced->h;++y) for (int x=0;x<enhanced->w;++x)
                memcpy(pixels+((size_t)y*enhanced->w+x)*3,(u8 *)enhanced->pixels+(size_t)y*enhanced->pitch+x*4,3);
            *w=enhanced->w;*h=enhanced->h;return pixels;
        }
    }
    const bool shown_wide =
        widescreen && !WidescreenNativeBuffer(interp_present_mid ? interp_next_y : display_area.y);
    if (UseSoftwareBackend() || display_rgb24 ||
        (!scaled_vram_render && !shown_wide && !interp_present_mid)) {
        unsigned char* pixels = Psyz_VideoAllocCapturedFrame(w, h);
        if (!pixels)
            *w = *h = 0;
        return pixels;
    }
    if ((!scaled_vram_render && !shown_wide && !interp_present_mid) || display_area.x < 0 ||
        display_area.y < 0 || display_size.x <= 0 || display_size.y <= 0 ||
        display_area.x > VRAM_W - display_size.x ||
        display_area.y > VRAM_H - display_size.y)
        return NULL;
    unsigned output_scale = Psyz_VideoGetInternalResolution();
    unsigned width = (unsigned)display_size.x * output_scale;
    unsigned height = (unsigned)display_size.y * output_scale;
    bool capture_widescreen = shown_wide && !debug_show_vram;
    if (capture_widescreen) {
        width = (height * 16u + 4u) / 9u;
        if (!ws_capture || ws_capture_width != width ||
            ws_capture_height != height) {
            if (ws_capture)
                SDL_ReleaseGPUTexture(device, ws_capture);
            ws_capture_width = width;
            ws_capture_height = height;
            const SDL_GPUTextureCreateInfo capture_info = {
                .type = SDL_GPU_TEXTURETYPE_2D,
                .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                         SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                .width = width,
                .height = height,
                .layer_count_or_depth = 1,
                .num_levels = 1};
            ws_capture = SDL_CreateGPUTexture(device, &capture_info);
        }
        if (!ws_capture)
            return NULL;
        ws_capture_active = true;
        PlatformBackend_Present();
        ws_capture_active = false;
    }
    size_t count = (size_t)width * height;
    unsigned char* pixels = malloc(count * 3);
    SDL_GPUTransferBufferCreateInfo info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = (Uint32)(count * 4)};
    SDL_GPUTransferBuffer* transfer =
        SDL_CreateGPUTransferBuffer(device, &info);
    if (!pixels || !transfer) {
        free(pixels);
        if (transfer)
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        return NULL;
    }
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    if (!cmd) {
        free(pixels);
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return NULL;
    }
    SDL_GPUBlitRegion visible = {
        .texture = interp_present_mid ? interpolated_vram : GetRenderTarget(),
        .x = (Uint32)display_area.x * internal_res,
        .y = (Uint32)(interp_present_mid?interp_next_y:display_area.y) * internal_res,
        .w = (Uint32)display_size.x * internal_res,
        .h = (Uint32)display_size.y * internal_res};
    if (capture_widescreen)
        visible =
            (SDL_GPUBlitRegion){.texture = ws_capture, .w = width, .h = height};
    else if (antialiasing) {
        visible = PrepareDisplayTexture(cmd, visible);
        if (visible.w != width || visible.h != height) {
            SubmitCmdAndWait();
            free(pixels);
            SDL_ReleaseGPUTransferBuffer(device, transfer);
            return NULL;
        }
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) {
        free(pixels);
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return NULL;
    }
    SDL_GPUTextureRegion region = {
        .texture = visible.texture,
        .x = visible.x,
        .y = visible.y,
        .w = width,
        .h = height,
        .d = 1};
    SDL_GPUTextureTransferInfo destination = {.transfer_buffer = transfer};
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);
    if (!SubmitCmdAndWait()) {
        free(pixels);
        /* transfer remains referenced by an uncertain submitted readback. */
        if (!gpu_retirement_uncertain) SDL_ReleaseGPUTransferBuffer(device, transfer);
        return NULL;
    }
    const u8* rgba = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (rgba) {
        for (size_t i = 0; i < count; ++i)
            memcpy(pixels + i * 3, rgba + i * 4, 3);
        SDL_UnmapGPUTransferBuffer(device, transfer);
        *w = (int)width;
        *h = (int)height;
    } else {
        free(pixels);
        pixels = NULL;
    }
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return pixels;
}

/* The drawing area clips every primitive. One whose end lies before its
 * start (a menu row's clip collapsed to zero width) draws nothing on the
 * PS1: the scissor becomes empty and Draw_FlushBuffer drops such batches
 * instead of keeping the previous area. */
/* The drawing area's rectangle; primitives queued before keep theirs. */
static void SetScissorFromArea(void) {
    int width = draw_area_end.x - draw_area_start.x + 1;
    int height = draw_area_end.y - draw_area_start.y + 1;

    if (!sdl3_window || !InitPlatform()) {
        return;
    }
    DrawStateChanging();
    scissor_rect.x = draw_area_start.x;
    scissor_rect.y = draw_area_start.y;
    scissor_rect.w = width > 0 ? width : 0;
    scissor_rect.h = height > 0 ? height : 0;
}

static void UpdateScissor() {
    int width = draw_area_end.x - draw_area_start.x + 1;
    int height = draw_area_end.y - draw_area_start.y + 1;

    if (!sdl3_window || !InitPlatform()) {
        return;
    }
    Draw_FlushBuffer();

    scissor_rect.x = draw_area_start.x;
    scissor_rect.y = draw_area_start.y;
    scissor_rect.w = width > 0 ? width : 0;
    scissor_rect.h = height > 0 ? height : 0;
}

unsigned Psyz_VideoGetInternalResolution(void) {
    return internal_res / (antialiasing ? 2u : 1u);
}

int Psyz_VideoSetInternalResolution(unsigned multiplier) {
    if (gpu_failed) return -1;
    if (multiplier < 1) {
        return -1;
    }
    if (multiplier > PSYZ_INTERNAL_RES_MAX) {
        WARNF("internal resolution %dx exceeds maximum value of %dx",
              multiplier, PSYZ_INTERNAL_RES_MAX);
        return -1;
    }
    set_internal_res = multiplier;
    if (sdl3_window && is_platform_init_successful) {
        ApplyPendingInternalRes();
    }
    return 0;
}

int Psyz_VideoGetGeometryCorrection(void) {
    return set_geometry_correction ? 1 : 0;
}

int Psyz_VideoSetGeometryCorrection(int enabled) {
    if (gpu_failed) return -1;
    set_geometry_correction = enabled != 0;
    if (sdl3_window && is_platform_init_successful) {
        ApplyPendingInternalRes();
    } else {
        geometry_correction = set_geometry_correction;
        Psyz_PgxpSetActive(!headless_renderer &&
                           (set_geometry_correction || set_effect_geometry_correction ||
                            set_model_filtering || interpolation_enabled));
        Psyz_PgxpSetCulling(set_geometry_correction);
    }
    return 0;
}

int Psyz_VideoSetAntialiasing(int enabled) {
    if (gpu_failed) return -1;
    set_antialiasing = enabled != 0;
    if (sdl3_window && is_platform_init_successful)
        ApplyPendingInternalRes();
    return 0;
}
int Psyz_VideoGetAntialiasing(void) { return set_antialiasing; }
int Psyz_VideoSetModelFiltering(int enabled) {
    if (gpu_failed) return -1;
    set_model_filtering = enabled != 0;
    if (sdl3_window && is_platform_init_successful)
        ApplyPendingInternalRes();
    else {
        model_filtering = set_model_filtering;
        Psyz_PgxpSetActive(!headless_renderer &&
                           (set_geometry_correction || set_effect_geometry_correction ||
                            set_model_filtering || interpolation_enabled));
    }
    return 0;
}
int Psyz_VideoGetModelFiltering(void) { return set_model_filtering; }
int Psyz_VideoSetSpriteFiltering(int enabled){
    if(gpu_failed)return -1;set_sprite_filtering=enabled!=0;
    if(sdl3_window&&is_platform_init_successful)ApplyPendingInternalRes();else sprite_filtering=set_sprite_filtering;
    return set_sprite_filtering==(enabled!=0)?0:-1;
}
int Psyz_VideoGetSpriteFiltering(void){return set_sprite_filtering;}

int Psyz_VideoSetDynamicLighting(int enabled) {
    if (gpu_failed) return -1;
    dynamic_lighting = enabled != 0;
    return 0;
}
int Psyz_VideoGetDynamicLighting(void) { return dynamic_lighting ? 1 : 0; }
int Psyz_VideoSetEffectGeometryCorrection(int enabled) {
    if (gpu_failed) return -1;
    set_effect_geometry_correction = enabled != 0;
    Psyz_PgxpSetEffectEnabled(set_effect_geometry_correction);
    if (sdl3_window && is_platform_init_successful) {
        ApplyPendingInternalRes();
    } else {
        effect_geometry_correction = set_effect_geometry_correction;
        Psyz_PgxpSetActive(!headless_renderer &&
                           (set_geometry_correction || set_effect_geometry_correction ||
                            set_model_filtering || interpolation_enabled));
    }
    return 0;
}
int Psyz_VideoGetEffectGeometryCorrection(void) {
    return set_effect_geometry_correction ? 1 : 0;
}

int Psyz_VideoSetWidescreen(int enabled) {
    if ((enabled != 0) != widescreen) {
        Draw_FlushBuffer();
        widescreen = enabled != 0;
        ws_picture[0].y = ws_picture[1].y = -1;
        ws_centred = false;
        if (!widescreen && device) {
            if (prescale_display) {
                SDL_ReleaseGPUTexture(device, prescale_display);
                prescale_display = NULL;
                prescale_width = prescale_height = 0;
            }
            if (ws_capture) {
                SDL_ReleaseGPUTexture(device, ws_capture);
                ws_capture = NULL;
                ws_capture_width = ws_capture_height = 0;
            }
        }
    }
    return 0;
}
int Psyz_VideoGetWidescreen(void) { return widescreen; }
int Psyz_VideoSetWidescreenSqueeze(float squeeze) {
    if (squeeze != 0.0f &&
        !(squeeze >= WS_SQUEEZE_MIN && squeeze <= WS_SQUEEZE_MAX))
        return -1;
    if (squeeze != ws_squeeze_setting) {
        Draw_FlushBuffer();
        ws_squeeze_setting = squeeze;
    }
    return 0;
}
float Psyz_VideoGetWidescreenSqueeze(void) {
    if (display_size.x <= 0 || display_size.y <= 0)
        return ws_squeeze_setting > 0.0f ? ws_squeeze_setting
                                         : WS_SQUEEZE_DEFAULT;
    return WidescreenCurrentSqueeze();
}
int Psyz_VideoGetWidescreenBufferMargin(void) {
    if (!widescreen || display_size.x <= 0 || display_size.y <= 0)
        return 0;
    return WidescreenBufferMargin(WidescreenCurrentSqueeze());
}
int Psyz_VideoGetWidescreenViewMargin(void) {
    if (!widescreen || display_size.x <= 0 || display_size.y <= 0)
        return 0;
    const float half = WidescreenViewHalf(
        GetCurrentGameAspectRatio(display_size.x, display_size.y),
        WidescreenCurrentSqueeze());
    return SDL_max(0, (int)half - display_size.x / 2);
}
void Psyz_VideoSetWidescreenCentred(int enabled) {
    if ((enabled != 0) != ws_centred) {
        Draw_FlushBuffer();
        ws_centred = enabled != 0;
    }
}
void Psyz_VideoSetWidescreenField(int enabled) {
    if ((enabled != 0) != ws_field) {
        Draw_FlushBuffer();
        ws_field = enabled != 0;
    }
}
static void SetWidescreenPicture(int buffer_y, int left, int right, bool field,
                                 float dx, float dy, int camera_x);
void Psyz_VideoSetWidescreenPicture(int buffer_y, int left, int right) {
    SetWidescreenPicture(buffer_y, left, right, false, 0.0f, 0.0f, 0);
}
int Psyz_VideoSetWidescreenNativeAspect(int mode) {
    if (mode != -1 && mode != PSYZ_ASPECT_DISPLAY && mode != PSYZ_ASPECT_SQUARE)
        return -1;
    ws_native_aspect = mode;
    return 0;
}
int Psyz_VideoGetWidescreenNativeAspect(void) { return ws_native_aspect; }
float Psyz_VideoGetPresentedAspect(void) {
    if (display_size.x <= 0 || display_size.y <= 0)
        return 0.0f;
    if (widescreen && !display_rgb24 && !WidescreenNativeBuffer(display_area.y))
        return 16.0f / 9.0f;
    return PresentGameAspect(display_area.y, display_rgb24);
}
void Psyz_VideoSetWidescreenNative(int buffer_y) {
    SetWidescreenPicture(buffer_y, 0, 0, false, 0.0f, 0.0f, 0);
    for (int i = 0; i < 2; ++i)
        if (ws_picture[i].y == buffer_y)
            ws_picture[i].native = true;
}
void Psyz_VideoSetWidescreenPictureShifted(
    int buffer_y, int left, int right, float dx, float dy, int camera_x) {
    SetWidescreenPicture(buffer_y, left, right, true, dx, dy, camera_x);
}
static void SetWidescreenPicture(int buffer_y, int left, int right, bool field,
                                 float dx, float dy, int camera_x) {
    int slot = ws_picture[0].y == buffer_y
                   ? 0
                   : (ws_picture[1].y == buffer_y ? 1 : -1);
    if (slot < 0) {
        slot = ws_picture[0].y < 0 ? 0 : 1;
    }
    if (ws_picture[slot].y != buffer_y && ws_picture[0].y >= 0 &&
        ws_picture[1].y >= 0) {
        /* a third buffer: forget both, the presenter falls back to all */
        ws_picture[1].y = -1;
        slot = 0;
    }
    /* The view slides towards the side that has picture at a room's end, so
     * the end of the picture lines up with the view's edge, but never so far
     * that the game's own screen leaves the view. A picture narrower than
     * the view is shown within its available span, unshifted, with uncovered bars.
     * A partially extended picture retains its real bounds without stretching.
     * Drawing into the buffer squeezes around the shifted point from here
     * on, so the buffer holds the shifted view. */
    Draw_FlushBuffer();
    ws_centred = false;
    ws_field = false;
    const float centre = (float)display_size.x * 0.5f;
    const float squeeze = WidescreenCurrentSqueeze();
    const float half = WidescreenViewHalf(
        GetCurrentGameAspectRatio(display_size.x, display_size.y), squeeze);
    const float max_shift = SDL_max(0.0f, half - centre);
    float shift = 0.0f;
    /* the view as it would be for the room where it belongs */
    float exact_shift = 0.0f;
    bool ends_move = false;
    dx = SDL_clamp(dx, -2.0f, 2.0f);
    dy = SDL_clamp(dy, -2.0f, 2.0f);
    /* A room can have real side art without covering an entire16:9 view.
     * Keep that span: the presenter already intersects it with the view and
     * clears uncovered pixels. Collapsing384px art to320 loses valid geometry
     * and also makes metadata depend on stale title/display dimensions. */
    if (left < right && (float)(right - left) >= 2.0f * half) {
        /* whole pixels, rounded towards the picture so no sliver of black shows
         */
        if (centre - half < (float)left) {
            shift = SDL_ceilf((float)left - (centre - half));
        } else if (centre + half > (float)right) {
            shift = SDL_floorf((float)right - (centre + half));
        }
        if (centre - half < (float)left + dx) {
            exact_shift = (float)left + dx - (centre - half);
        } else if (centre + half > (float)right + dx) {
            exact_shift = (float)right + dx - (centre + half);
        }
        ends_move = true;
    }
    shift = SDL_clamp(shift, -max_shift, max_shift);
    exact_shift = SDL_clamp(exact_shift, -max_shift, max_shift);
    ws_picture[slot].y = buffer_y;
    ws_picture[slot].left = left;
    ws_picture[slot].right = right;
    ws_picture[slot].shift = (int)shift;
    /* A view that follows the picture's end to the fraction keeps the room
     * still on screen: only the room's shift past that is left to show. */
    ws_picture[slot].native = false;
    ws_picture[slot].squeeze = squeeze;
    ws_picture[slot].field = field;
    ws_picture[slot].ends_move = ends_move;
    ws_picture[slot].ex = field ? dx - (exact_shift - shift) : 0.0f;
    ws_picture[slot].ey = field ? dy : 0.0f;
    ws_picture[slot].camera_x = camera_x;
    int previous=1-slot;
    float qx,qy,px,py;WidescreenFieldStep(slot,(float)internal_res,&qx,&qy);
    bool pair=ws_picture[previous].y>=0&&ws_picture[previous].field==field;
    if(!pair)previous=slot;
    WidescreenFieldStep(previous,(float)internal_res,&px,&py);
    ws_mid_qx[slot]=(qx+px)*0.5f;ws_mid_qy[slot]=(qy+py)*0.5f;
    ws_mid_shift[slot]=(ws_picture[slot].shift+ws_picture[previous].shift)*0.5f;
    ws_mid_ex[slot]=(ws_picture[slot].ex+ws_picture[previous].ex)*0.5f;
    ws_mid_ey[slot]=(ws_picture[slot].ey+ws_picture[previous].ey)*0.5f;
    ws_mid_left[slot]=(ws_picture[slot].left+ws_picture[previous].left)*0.5f;
    ws_mid_right[slot]=(ws_picture[slot].right+ws_picture[previous].right)*0.5f;
}

static void ApplyDisplayPendingChanges() {
    if (!sdl3_window && !InitPlatform()) {
        return;
    }
    ApplyPendingInternalRes();
    if (cur_display_size.x != display_size.x ||
        cur_display_size.y != display_size.y ||
        (!is_window_visible && !hidden_gpu_renderer)) {
        if (!is_window_visible) {
#ifdef PLATFORM_IOS
            SDL_GetWindowSizeInPixels(
                sdl3_window, &wnd_size_in_pixels.w, &wnd_size_in_pixels.h);
#else
            SetWindowSizeInPixels(DEFAULT_FRONT_W, DEFAULT_FRONT_H);
#endif
        }
        cur_display_size = display_size;
    }
    if (!is_window_visible && !hidden_gpu_renderer) {
        SDL_ShowWindow(sdl3_window);
        is_window_visible = true;
#ifdef PLATFORM_IOS
        SDL_GetWindowSizeInPixels(
            sdl3_window, &wnd_size_in_pixels.w, &wnd_size_in_pixels.h);
        Psyz_IosAttachWindow(sdl3_window);
#endif
    }
    if (cur_disp_horiz != set_disp_horiz || cur_disp_vert != set_disp_vert) {
        cur_disp_horiz = set_disp_horiz;
        cur_disp_vert = set_disp_vert;
    }
}

void Draw_Reset() {
    if (UseSoftwareBackend()) {
        software_rasterizer.reset();
        return;
    }
    /* GP1 00h resets the drawing state (GP0 E1h..E6h), as the CPU
     * rasterizer does: page 0 without dithering, no texture window, offset
     * 0, the whole VRAM as drawing area and no mask setting. */
    Draw_FlushBuffer();
    cur_tpage = 0;
    SetDither(0);
    cur_twin = TWIN_PACK(0xFF, 0xFF, 0x00, 0x00);
    mask_set_bit = mask_check_bit = false;
    draw_offset.x = draw_offset.y = 0;
    draw_area_start.x = draw_area_start.y = 0;
    draw_area_end.x = VRAM_W - 1;
    draw_area_end.y = VRAM_H - 1;
    UpdateScissor();
}

void Draw_DisplayEnable(unsigned int on) {
    if (SoftwareOnly()) {
        return;
    }
    disp_on = on;
    if (on) {
        ApplyDisplayPendingChanges();
    }
    PresentDue(); /* the new display, at the blank */
}

void Draw_DisplayArea(unsigned int x, unsigned int y) {
    /* A buffer's picture span lasts until the display leaves it: the next
     * frame drawn there sets a new one, or draws without one (all shown). */
    if ((int)y != display_area.y) {
        for (int i = 0; i < 2; ++i) {
            if (ws_picture[i].y == display_area.y)
                ws_picture[i].y = -1;
        }
    }
    display_area.x = (int)x;
    display_area.y = (int)y;
    if (SoftwareOnly())
        return;

    Draw_FlushBuffer();
    ApplyDisplayPendingChanges();
    PresentDue(); /* the new display, at the blank */
}

void Draw_DisplayHorizontalRange(unsigned int start, unsigned int end) {
    if (SoftwareOnly())
        return;
    set_disp_horiz = (int)(end - start) / 10;
    ApplyDisplayPendingChanges();
}

void Draw_DisplayVerticalRange(unsigned int start, int unsigned end) {
    set_disp_vert = (int)(end - start);
    display_size.y = set_disp_vert * (display_double_height ? 2 : 1);
    if (SoftwareOnly())
        return;
    ApplyDisplayPendingChanges();
}

void Draw_SetDisplayMode(DisplayMode* mode) {
    display_rgb24 = mode->rgb24;
    // TODO the interlace flag is ignored
    if (mode->reversed) {
        WARNF("reverse mode not supported");
    }
    is_pal = mode->pal;
    if (mode->horizontal_resolution_368) {
        display_size.x = 368;
    } else {
        switch (mode->horizontal_resolution) {
        case 0:
            display_size.x = 256;
            break;
        case 1:
            display_size.x = 320;
            break;
        case 2:
            display_size.x = 512;
            break;
        case 3:
            display_size.x = 640;
            break;
        default:
            break;
        }
    }
    display_double_height = mode->vertical_resolution;
    display_size.y = set_disp_vert * (display_double_height ? 2 : 1);
    if (SoftwareOnly()) {
        // Games rewrite the display mode every frame (PutDispEnv); only a
        // change of video standard may reset the frame limiter.
        double fps = mode->pal ? VSYNC_PAL : VSYNC_NTSC;
        if (fps != target_frame_rate) {
            UpdateTargetFramerate(fps);
        }
        return;
    }
    ApplyDisplayPendingChanges();

    double new_target_fps = mode->pal ? VSYNC_PAL : VSYNC_NTSC;
    if (new_target_fps != target_frame_rate) {
        UpdateTargetFramerate(new_target_fps);
    }
}

int Draw_ExequeSync() { return 0; }

// optimization to avoid sampling the VRAM on an untextured batch draw
static bool batch_has_texture = false;
static void ClutCacheUpdate(u16 tpage, u16 clut);
/* Draw_EnqueueBuffer: the ghosts that follow the primitive just queued. */
static void InterpInjectGhosts(unsigned h) {
    const unsigned f=interp_frame-1,b=f&1;
    while(h){
        InterpPair *g=&interp_pairs[h-1];h=g->ghost_next;
        /* another view (the drawing area of a buffer moves with it) */
        if(g->store_frame!=f||g->tagged_frame==interp_frame||g->store_offset_x!=draw_offset.x-draw_area_start.x||
           g->store_offset_y!=draw_offset.y-draw_area_start.y||g->store_width!=draw_area_end.x-draw_area_start.x||
           g->store_height!=draw_area_end.y-draw_area_start.y)continue;
        const unsigned n=g->store_count,ni=g->store_indices;
        const Vertex *src=&interp_store[b][g->store_offset];
        /* the CLUT cache row of the frame before is gone: load it again */
        const bool textured=!(src[0].t&TPAGE_NOTEXTURE);
        if(textured)ClutCacheUpdate(src[0].t,src[0].c);
        Draw_EnsureBufferWillNotOverflow((int)n,(int)ni);
        const unsigned base=n_vertices;
        InterpPend(0,(unsigned)(g-interp_pairs),base,n,g->position,g->depth,g->store_index,ni,InterpOffsetX(),InterpOffsetY());
        for(unsigned i=0;i<n;++i){
            Vertex *out=&interpolated_vertices[base+i];float pose[2];
            *out=src[i];InterpGhostPose(g,i,0.5f,pose);
            out->pgxp_pos[0]=pose[0];out->pgxp_pos[1]=pose[1];out->pgxp_pos[2]=g->position[i][2]>0?g->position[i][2]:1.0f;
            if(textured)out->exact[4][3]=clut_cache_row;
            vertex_cur[i]=src[i];vertex_cur[i].x=vertex_cur[i].y=-2048;
            vertex_cur[i].pgxp_pos[0]=vertex_cur[i].pgxp_pos[1]=-2048.0f;vertex_cur[i].pgxp_pos[2]=-1.0f;
        }
        for(unsigned k=0;k<ni;++k)index_cur[k]=(u32)(base+g->store_index[k]);
        if(textured)batch_has_texture=true;
        vertex_cur+=n;index_cur+=ni;n_vertices+=n;n_indices+=(int)ni;
        interp_area[ISTAT_GHOST]+=InterpArea(g->position,n);++interp_count[ISTAT_GHOST];
        static int probe=-1;
        if(probe<0)probe=SDL_getenv("PSYZ_INTERP_PROBE")!=NULL;
        if(probe){interp_probe_pair=(unsigned)(g-interp_pairs);interp_probe_flags=0;InterpProbe(NULL,&vertex_buf[base],&interpolated_vertices[base],n,"ghost");}
    }
}
static unsigned InterpEdgeCell(int cx,int cy,int ox,int oy) {
    uint32_t h=(uint32_t)cx*0x9E3779B1u^(uint32_t)cy*0x85EBCA77u^(uint32_t)(ox*977+oy)*0xC2B2AE3Du;
    return (h^h>>16)&(INTERP_EDGE_CELLS-1);
}
static void InterpAddEdge(const float *a0,const float *b0,const float *a1,const float *b1,const float *am,const float *bm,
                          float da,float db,unsigned pair,int ox,int oy,bool posed) {
    if(interp_edges_frame!=interp_frame){interp_edges_frame=interp_frame;interp_edges_len=interp_edge_refs_len=0;}
    const float lx=SDL_min(a0[0],b0[0]),hx=SDL_max(a0[0],b0[0]),ly=SDL_min(a0[1],b0[1]),hy=SDL_max(a0[1],b0[1]);
    if(!(lx>-2048.0f&&hx<2048.0f&&ly>-2048.0f&&hy<2048.0f))return;
    if(interp_edges_len>=INTERP_EDGES){InterpEvict(IEVICT_EDGES);return;}
    const int x0=(int)SDL_floorf((lx-INTERP_JUNCTION_REACH)/INTERP_EDGE_CELL),x1=(int)SDL_floorf((hx+INTERP_JUNCTION_REACH)/INTERP_EDGE_CELL);
    const int y0=(int)SDL_floorf((ly-INTERP_JUNCTION_REACH)/INTERP_EDGE_CELL),y1=(int)SDL_floorf((hy+INTERP_JUNCTION_REACH)/INTERP_EDGE_CELL);
    if((x1-x0+1)*(y1-y0+1)>96)return;
    if(interp_edge_refs_len+(unsigned)((x1-x0+1)*(y1-y0+1))>INTERP_EDGE_REFS){InterpEvict(IEVICT_EDGE_REFS);return;}
    const unsigned e=interp_edges_len++;
    InterpEdge *g=&interp_edges[e];
    memcpy(g->a0,a0,sizeof(g->a0));memcpy(g->b0,b0,sizeof(g->b0));memcpy(g->a1,a1,sizeof(g->a1));memcpy(g->b1,b1,sizeof(g->b1));
    memcpy(g->am,am,sizeof(g->am));memcpy(g->bm,bm,sizeof(g->bm));g->da=da;g->db=db;g->pair=pair;g->ox=(short)ox;g->oy=(short)oy;g->posed=posed;
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){const unsigned c=InterpEdgeCell(x,y,ox,oy);
        if(interp_edge_stamp[c]!=interp_frame){interp_edge_stamp[c]=interp_frame;interp_edge_head[c]=0;}
        interp_edge_refs[interp_edge_refs_len]=(InterpEdgeRef){e,interp_edge_head[c]};interp_edge_head[c]=++interp_edge_refs_len;}
}
/* Where p lies along segment a-b (0..1) and how far from its line. */
static float InterpAlong(const float *p,const float *a,const float *b,float *distance) {
    const float dx=b[0]-a[0],dy=b[1]-a[1],l2=dx*dx+dy*dy;
    if(!(l2>1.0f)){*distance=1e9f;return -1.0f;}
    const float t=((p[0]-a[0])*dx+(p[1]-a[1])*dy)/l2;
    *distance=SDL_fabsf((p[0]-a[0])*dy-(p[1]-a[1])*dx)/SDL_sqrtf(l2);
    return t;
}
/* InterpMidpointSync: the first n queued paired primitives' vertices that
 * lie on another paired primitive's edge in both frames go onto the edge's
 * midpoint. */
static void InterpSnapJunctions(unsigned n) {
    static int snap=-1;
    if(snap<0){const char *e=SDL_getenv("PSYZ_INTERP_JUNCTIONS");snap=!(e&&*e=='0');}
    if(!snap||interp_edges_frame!=interp_frame)return;
    /* how far off the edge a vertex on it lies: the projection's precision
     * (PGXP, or whole pixels); a vertex farther off belongs to another
     * surface next to the edge (the faces of a thin beam) */
    const float reach=(geometry_correction||effect_geometry_correction)&&internal_res>1?INTERP_JUNCTION_PRECISE:INTERP_JUNCTION_PIXELS;
    for(unsigned k=0;k<n;++k){const InterpJunctionPrim *q=&interp_junction_prims[k];
        for(unsigned i=0;i<q->count;++i){
            const float *v0=q->p0[i],*v1=q->p1[i];
            const int cx=(int)SDL_floorf(v0[0]/INTERP_EDGE_CELL),cy=(int)SDL_floorf(v0[1]/INTERP_EDGE_CELL);
            const unsigned c=InterpEdgeCell(cx,cy,q->ox,q->oy);
            if(interp_edge_stamp[c]!=interp_frame)continue;
            float best=1e9f;const InterpEdge *on=NULL;
            for(unsigned r=interp_edge_head[c];r;r=interp_edge_refs[r-1].next){const InterpEdge *g=&interp_edges[interp_edge_refs[r-1].edge];
                if(g->pair==q->pair||g->ox!=q->ox||g->oy!=q->oy)continue;
                float d0,d1;const float t0=InterpAlong(v0,g->a0,g->b0,&d0),t1=InterpAlong(v1,g->a1,g->b1,&d1);
                /* a midpoint-only pose in this frame (nearer than the
                 * projection plane) is no projection to test */
                const bool both=q->posed&&g->posed;
                /* with one pose to test (geometry passing the view, often a
                 * long edge) the direction of a long edge counts less */
                const float l0=SDL_sqrtf((g->b0[0]-g->a0[0])*(g->b0[0]-g->a0[0])+(g->b0[1]-g->a0[1])*(g->b0[1]-g->a0[1]));
                if(!(t0>0.0f&&t0<1.0f&&d0<reach+(both?0.0f:l0*INTERP_JUNCTION_SLOPE)&&(!both||(t1>0.0f&&t1<1.0f&&d1<reach))))continue;
                /* not one of its ends (a shared vertex is joined already) */
                if(t0*l0<0.5f||(1.0f-t0)*l0<0.5f)continue;
                /* at the edge's depth there (not something in front or behind) */
                if(q->d0[i]>0.0f&&g->da>0.0f&&g->db>0.0f){
                    const float z=1.0f/((1.0f-t0)/g->da+t0/g->db);
                    if(SDL_fabsf(z-q->d0[i])>0.1f*SDL_max(z,q->d0[i]))continue;}
                if(d0+(both?d1:d0)<best){best=d0+(both?d1:d0);on=g;}
            }
            if(!on)continue;
            Vertex *out=&interp_mid_store[q->base+i];
            if(!(out->pgxp_pos[2]>0.0f))continue;
            float d;const float t=InterpAlong(out->pgxp_pos,on->am,on->bm,&d);
            if(!(t>0.0f&&t<1.0f)||d>INTERP_JUNCTION_MOVE)continue;
            out->pgxp_pos[0]=on->am[0]+(on->bm[0]-on->am[0])*t;out->pgxp_pos[1]=on->am[1]+(on->bm[1]-on->am[1])*t;
            /* what moves with the vertex (a ghost or a new identity sharing
             * it, InterpFinishPending) goes there too */
            for(unsigned w=0;w<2;++w)if(q->field[w][i]&&interp_field[w].frame==interp_frame&&q->field[w][i]<=interp_field[w].len){
                InterpFieldEntry *f=&interp_field[w].entry[q->field[w][i]-1];
                f->dx=out->pgxp_pos[0]-f->x;f->dy=out->pgxp_pos[1]-f->y;}
        }
    }
}
/* InterpMidpointSync: the first pending ghosts and new identities move as
 * the primitives next to them did (InterpFieldMotion), in the order queued,
 * so one next to another follows it; vertices with none in reach move as
 * the primitive's others, a primitive with none keeps its pose (a ghost its
 * continued motion, a new identity this frame's pose). */
static void InterpFinishPending(unsigned pending) {
    static int field=-1;
    if(field<0){const char *e=SDL_getenv("PSYZ_INTERP_FIELD");field=!(e&&*e=='0');}
    static int probe=-1;
    if(probe<0)probe=SDL_getenv("PSYZ_INTERP_PROBE")!=NULL;
    for(unsigned k=0;k<pending;++k){
        const InterpPending *q=&interp_pending[k];
        const unsigned base=q->base,n=q->count;
        float motion[6][2],shift[2]={0,0},weight=0;unsigned found=0;
        for(unsigned i=0;i<n;++i){float d;
            if(!field||!InterpFieldMotion(q->which,q->from[i],q->depth[i],q->ox,q->oy,motion[i],&d))continue;
            const float w=1.0f/(1.0f+d);
            shift[0]+=motion[i][0]*w;shift[1]+=motion[i][1]*w;weight+=w;found|=1u<<i;}
        for(unsigned i=0;i<n;++i){Vertex *out=&interp_mid_store[base+i];
            /* a ghost's vertex with no neighbour keeps its continued
             * motion; a new identity's moves as its neighbours do */
            if((found>>i&1)||(found&&q->which==1)){const float *m=(found>>i&1)?motion[i]:(const float[2]){shift[0]/weight,shift[1]/weight};
                out->pgxp_pos[0]=q->from[i][0]+m[0];out->pgxp_pos[1]=q->from[i][1]+m[1];
                if(!(out->pgxp_pos[2]>0.0f))out->pgxp_pos[2]=1.0f;}
            InterpFieldAdd(q->which,q->from[i],q->depth[i],q->ox,q->oy,out->pgxp_pos);
            interp_mid_depth_store[base+i]=q->depth[i];}
        u32 index[6];for(unsigned i=0;i<q->indices;++i)index[i]=base+q->index[i];
        InterpExposePrimitive(2,&q->view,&interp_mid_store[base],&interp_mid_depth_store[base],index,q->indices,base,true);
        /* (the frame's vertices are drawn: "own" is the midpoint pose too) */
        if(probe){interp_probe_pair=q->pair;interp_probe_flags=found;
            InterpProbe(NULL,&interp_mid_store[base],&interp_mid_store[base],n,q->which?"new-final":"ghost-final");}
    }
}
static void InterpEnqueued(void) {
    ++batch_primitives;
    if(!interp_inject&&!interp_ghost_first)return;
    if(!interpolation_enabled||!interpolated_vram||interpolated_scale!=internal_res||interp_line_mode){interp_inject=0;return;}
    if(interp_ghost_first){const unsigned h=interp_ghost_first;interp_ghost_first=0;InterpInjectGhosts(h);}
    if(interp_inject){InterpPair *p=&interp_pairs[interp_inject-1];const unsigned h=p->ghost_head;
        p->ghost_head=0;interp_inject=0;InterpInjectGhosts(h);}
}
/* Retain the PS1's signed 12-fractional-bit attribute gradients while the
 * GPU rasterizes the geometry, including at higher internal resolutions. */
static void SetPolygonAttributes(Vertex* vertices) {
    Vertex* a = &vertices[0];
    Vertex* b = &vertices[1];
    Vertex* c = &vertices[2];
    Vertex* anchor =
        b->x <= a->x ? (c->x <= b->x ? c : b) : (c->x < a->x ? c : a);
    int64_t area = (int64_t)(b->x - a->x) * (c->y - a->y) -
                   (int64_t)(b->y - a->y) * (c->x - a->x);
    for (int i = 0; i < 3; ++i) {
        vertices[i].uv_bounds[0] = SDL_min(a->u, SDL_min(b->u, c->u));
        vertices[i].uv_bounds[1] = SDL_min(a->v, SDL_min(b->v, c->v));
        vertices[i].uv_bounds[2] = SDL_max(a->u, SDL_max(b->u, c->u));
        vertices[i].uv_bounds[3] = SDL_max(a->v, SDL_max(b->v, c->v));
    }
    const int av[5] = {a->u, a->v, a->r, a->g, a->b};
    const int bv[5] = {b->u, b->v, b->r, b->g, b->b};
    const int cv[5] = {c->u, c->v, c->r, c->g, c->b};
    const int origin[5] = {
        anchor->u, anchor->v, anchor->r, anchor->g, anchor->b};
    for (int attribute = 0; attribute < 5; ++attribute) {
        int64_t dx = 0, dy = 0;
        if (area) {
            dx = ((int64_t)(bv[attribute] - av[attribute]) * (c->y - a->y) -
                  (int64_t)(cv[attribute] - av[attribute]) * (b->y - a->y)) *
                 4096 / area;
            dy = ((int64_t)(cv[attribute] - av[attribute]) * (b->x - a->x) -
                  (int64_t)(bv[attribute] - av[attribute]) * (c->x - a->x)) *
                 4096 / area;
        }
        for (int vertex = 0; vertex < 3; ++vertex) {
            float value = (float)(origin[attribute] * 4096 + 2048 +
                                  (vertices[vertex].x - anchor->x) * dx +
                                  (vertices[vertex].y - anchor->y) * dy) /
                          4096.0f;
            if (attribute < 2)
                vertices[vertex].polygon_uv[attribute] = value;
            else
                vertices[vertex].polygon_rgb[attribute - 2] = value;
        }
    }
}
/* Exact PS1 polygons: the GPU's own fill rule, clipping and floating-point
 * interpolation decide neither coverage nor attributes. A triangle is drawn
 * as its bounding box (clipped to the drawing area, so nothing is clipped by
 * the GPU) carrying the integer triangle and its 12.12 attribute planes; the
 * fragment shader keeps the samples the PS1 covers (top and left edges
 * inside, bottom and right outside) and steps the attributes as the GPU does
 * from the leftmost vertex with a half-unit bias. At Nx the target pixel
 * (i, j) is the PS1 sample (i / N, j / N): the first target pixel of every
 * VRAM pixel is the 1x result and the others sample between PS1 pixels.
 * Not used with enhancements that move vertices or replace texels
 * (geometry correction, frame interpolation, widescreen, draw grid, HD). */
static bool ExactPolygonsActive(void) {
    return PSX_SHADERS_CURRENT && !geometry_correction && !effect_geometry_correction &&
           !interpolation_enabled && !WidescreenDraw() &&
           draw_grid_source_width == draw_grid_target_width;
}

static int VertexAttribute(const Vertex* v, int i) {
    switch (i) {
    case 0: return v->u;
    case 1: return v->v;
    case 2: return v->r;
    case 3: return v->g;
    default: return v->b;
    }
}

/* Emits the bounding box of triangle (a, b, c) as four vertices per
 * visible piece at out (up to 16) and returns their count, 0 for a
 * degenerate triangle or one outside the drawing area. The GPU wraps a
 * line's X start and the line number to signed 11 bits: a span starting
 * left of X -1024 or a line above Y -1024 lands 2048 pixels further, so the
 * box is also drawn shifted by 2048 in X, Y or both where that is visible
 * (exact[0][3] bits 1 and 2 tell the shader). */
static int EmitExactTriangle(const Vertex* a, const Vertex* b, const Vertex* c,
                             Vertex* out) {
    const Vertex* v[3] = {a, b, c};
    int x[3], y[3];
    for (int i = 0; i < 3; i++) {
        x[i] = v[i]->x + draw_offset.x;
        y[i] = v[i]->y + draw_offset.y;
    }
    const int64_t area = (int64_t)(x[1] - x[0]) * (y[2] - y[0]) -
                         (int64_t)(y[1] - y[0]) * (x[2] - x[0]);
    if (!area)
        return 0;
    /* covered pixels lie in [bx0, bx1) x [by0, by1) */
    const int bx0 = SDL_min(x[0], SDL_min(x[1], x[2]));
    const int bx1 = SDL_max(x[0], SDL_max(x[1], x[2]));
    const int by0 = SDL_min(y[0], SDL_min(y[1], y[2]));
    const int by1 = SDL_max(y[0], SDL_max(y[1], y[2]));
    /* leftmost vertex, the earlier one on ties */
    const int left = x[1] <= x[0] ? (x[2] <= x[1] ? 2 : 1)
                                  : (x[2] < x[0] ? 2 : 0);
    uint32_t base[5], per_x[5], per_y[5];
    for (int i = 0; i < 5; i++) {
        const int a0 = VertexAttribute(a, i), a1 = VertexAttribute(b, i),
                  a2 = VertexAttribute(c, i);
        const int64_t dx = (int64_t)(a1 - a0) * (y[2] - y[0]) -
                           (int64_t)(a2 - a0) * (y[1] - y[0]);
        const int64_t dy = (int64_t)(a2 - a0) * (x[1] - x[0]) -
                           (int64_t)(a1 - a0) * (x[2] - x[0]);
        per_x[i] = (uint32_t)(int32_t)(dx * 4096 / area);
        per_y[i] = (uint32_t)(int32_t)(dy * 4096 / area);
        base[i] = ((uint32_t)VertexAttribute(v[left], i) << 12) + 2048u -
                  per_x[i] * (uint32_t)x[left] - per_y[i] * (uint32_t)y[left];
    }
    unsigned int exact[5][4] = {
        {(unsigned)(x[0] & 0xFFFF) | (unsigned)y[0] << 16,
         (unsigned)(x[1] & 0xFFFF) | (unsigned)y[1] << 16,
         (unsigned)(x[2] & 0xFFFF) | (unsigned)y[2] << 16, 0},
        {base[0], base[1], base[2], base[3]},
        {per_x[0], per_x[1], per_x[2], per_x[3]},
        {per_y[0], per_y[1], per_y[2], per_y[3]},
        {base[4], per_x[4], per_y[4], clut_cache_row}};
    int emitted = 0;
    for (int shift = 0; shift < 4; shift++) {
    const int sx = (shift & 1) ? 2048 : 0, sy = (shift & 2) ? 2048 : 0;
    const int x0 = SDL_max(bx0 + sx, SDL_max(scissor_rect.x, 0));
    const int y0 = SDL_max(by0 + sy, SDL_max(scissor_rect.y, 0));
    const int x1 =
        SDL_min(bx1 + sx, SDL_min(scissor_rect.x + scissor_rect.w, VRAM_W));
    const int y1 =
        SDL_min(by1 + sy, SDL_min(scissor_rect.y + scissor_rect.h, VRAM_H));
    if (x0 >= x1 || y0 >= y1)
        continue;
    exact[0][3] = 1u | (unsigned)shift << 1;
    for (int k = 0; k < 4; k++) {
        Vertex* q = &out[emitted++];
        *q = *a;
        const int cx = (k & 1) ? x1 : x0, cy = (k & 2) ? y1 : y0;
        q->x = (short)(cx - draw_offset.x);
        q->y = (short)(cy - draw_offset.y);
        /* floating attributes for the high colour and filtering paths */
        for (int i = 0; i < 5; i++) {
            const double value =
                ((double)(int32_t)base[i] +
                 (double)(int32_t)per_x[i] * (cx - sx) +
                 (double)(int32_t)per_y[i] * (cy - sy)) / 4096.0;
            if (i < 2)
                q->polygon_uv[i] = (float)value;
            else
                q->polygon_rgb[i - 2] = (float)value;
        }
        q->uv_bounds[0] = SDL_min(a->u, SDL_min(b->u, c->u));
        q->uv_bounds[1] = SDL_min(a->v, SDL_min(b->v, c->v));
        q->uv_bounds[2] = SDL_max(a->u, SDL_max(b->u, c->u));
        q->uv_bounds[3] = SDL_max(a->v, SDL_max(b->v, c->v));
        memcpy(q->exact, exact, sizeof(exact));
    }
    }
    return emitted;
}

/* Texture feedback within a batch: the batch samples a copy of the VRAM
 * made before it is drawn (vram_sample), while the PS1 reads texels and
 * CLUTs as they are when each primitive is drawn. batch_drawn bounds what
 * the queued primitives draw; a textured primitive reading from it first
 * flushes the batch. (A primitive reading what it draws itself still reads
 * the VRAM before it.) */
static SDL_Rect batch_drawn;

static bool RectsIntersect(SDL_Rect a, SDL_Rect b) {
    return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.x + b.w &&
           b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void UniteRect(SDL_Rect* into, SDL_Rect r) {
    if (r.w <= 0 || r.h <= 0)
        return;
    if (into->w <= 0 || into->h <= 0) {
        *into = r;
        return;
    }
    const int x0 = SDL_min(into->x, r.x), y0 = SDL_min(into->y, r.y);
    const int x1 = SDL_max(into->x + into->w, r.x + r.w);
    const int y1 = SDL_max(into->y + into->h, r.y + r.h);
    *into = (SDL_Rect){x0, y0, x1 - x0, y1 - y0};
}

/* The texels a textured primitive reads: its UV range (the whole page with
 * a texture window or when the range may wrap), as up to two rectangles
 * split at the VRAM's right edge. Its CLUT comes from the CLUT cache. */
static int TextureFootprint(u16 tpage, int u0, int u1, int v0, int v1,
                            SDL_Rect out[2]) {
    const int depth = (tpage >> 7) & 3;
    if (cur_twin != TWIN_PACK(0xFF, 0xFF, 0x00, 0x00) || u0 < 1 || u1 > 254) {
        u0 = 0;
        u1 = 255;
    }
    if (cur_twin != TWIN_PACK(0xFF, 0xFF, 0x00, 0x00) || v0 < 1 || v1 > 254) {
        v0 = 0;
        v1 = 255;
    } else {
        v0--, v1++;
    }
    if (u0 > 0)
        u0--, u1++;
    const int shift = depth == 0 ? 2 : depth == 1 ? 1 : 0;
    const int x = (tpage & 15) * 64 + (u0 >> shift);
    const int w = (u1 >> shift) - (u0 >> shift) + 1;
    const int y = ((tpage >> 4) & 1) * 256 + v0;
    int n = 0;
    out[n++] = (SDL_Rect){x, y, SDL_min(w, VRAM_W - x), v1 - v0 + 1};
    if (x + w > VRAM_W)
        out[n++] = (SDL_Rect){0, y, x + w - VRAM_W, v1 - v0 + 1};
    return n;
}

void Draw_ClearCache(void) {
    if (UseSoftwareBackend()) {
        software_rasterizer.clear_cache();
        return;
    }
    clut_cache_reg = 0xFFFFFFFFu;
    clut_cache_8bit = false;
}

/* Loads the CLUT cache for a textured primitive of page tpage and CLUT
 * clut when the GPU would: the copy sees the VRAM as it is at this point of
 * the command stream, so a batch drawing over the CLUT is flushed first. */
static void ClutCacheUpdate(u16 tpage, u16 clut) {
    const int depth = (tpage >> 7) & 3;
    if (depth >= 2 || (clut == clut_cache_reg && (depth == 0 || clut_cache_8bit)))
        return;
    const int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
    const int cw = depth ? 256 : 16;
    const SDL_Rect row = {cx, cy, cw, 1}, wrapped = {0, cy, cx + cw - VRAM_W, 1};
    if (RectsIntersect(row, batch_drawn) || RectsIntersect(wrapped, batch_drawn))
        DRAW_FLUSH_FOR(DRAW_FLUSH_FEEDBACK);
    else if (clut_cache_loads + 1 >= CLUT_CACHE_ROWS)
        DRAW_FLUSH_FOR(DRAW_FLUSH_CLUT);
    /* the rows the midpoint kept so far reads stay until it is drawn */
    if (InterpMidpointDeferred() && interp_mid_clut_loads + 1 >= CLUT_CACHE_ROWS)
        InterpMidpointSync(INTERP_SYNC_CLUT);
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    SDL_GPUCopyPass* copy = cmd ? SDL_BeginGPUCopyPass(cmd) : NULL;
    if (!copy)
        return;
    clut_cache_reg = clut;
    clut_cache_8bit = depth == 1;
    clut_cache_row = (clut_cache_row + 1) % CLUT_CACHE_ROWS;
    clut_cache_loads++;
    interp_mid_clut_loads++;
    draw_frame_stats.clut_loads++;
    const int first = SDL_min(cw, VRAM_W - cx);
    const SDL_GPUTextureLocation src = {
        .texture = vram_render, .x = (Uint32)cx, .y = (Uint32)cy};
    const SDL_GPUTextureLocation dst = {
        .texture = clut_cache, .y = clut_cache_row};
    SDL_CopyGPUTextureToTexture(copy, &src, &dst, (Uint32)first, 1, 1, false);
    if (first < cw) {
        const SDL_GPUTextureLocation src2 = {
            .texture = vram_render, .y = (Uint32)cy};
        const SDL_GPUTextureLocation dst2 = {
            .texture = clut_cache, .x = (Uint32)first, .y = clut_cache_row};
        SDL_CopyGPUTextureToTexture(copy, &src2, &dst2, (Uint32)(cw - first), 1,
                                    1, false);
    }
    SDL_EndGPUCopyPass(copy);
}

/* Flushes the batch before a textured primitive whose texels or CLUT the
 * batch draws, and accounts for what this primitive draws. Reads only the
 * command words (GP0 20h..7Fh). */
static void BatchTextureFeedback(const u_long* words, int max_len) {
    const int code = (int)(words[0] >> 24) & 0xFF;
    int x0 = 0x7FFF, y0 = 0x7FFF, x1 = -0x7FFF, y1 = -0x7FFF;
    int u0 = 255, u1 = 0, v0 = 255, v1 = 0;
    u16 tpage = cur_tpage, clut = 0;
    const bool textured = (code & TEXTURED) != 0 && (code & 0x60) != 0x40;
    if (!(code & 0x40)) {
        const int count = (code & EXTRA_VERTEX) ? 4 : 3;
        const bool gouraud = (code & GOURAUD) != 0;
        if (max_len < 1 + count * (1 + textured) + (count - 1) * gouraud)
            return;
        for (int i = 0, at = 1; i < count; i++) {
            if (gouraud && i)
                at++;
            const u32 xy = (u32)words[at++];
            const int px = s11((short)xy), py = s11((short)(xy >> 16));
            x0 = SDL_min(x0, px), x1 = SDL_max(x1, px);
            y0 = SDL_min(y0, py), y1 = SDL_max(y1, py);
            if (textured) {
                const u32 uv = (u32)words[at++];
                u0 = SDL_min(u0, (int)(uv & 255));
                u1 = SDL_max(u1, (int)(uv & 255));
                v0 = SDL_min(v0, (int)((uv >> 8) & 255));
                v1 = SDL_max(v1, (int)((uv >> 8) & 255));
                if (i == 0)
                    clut = (u16)(uv >> 16);
                if (i == 1)
                    tpage = (u16)(uv >> 16);
            }
        }
    } else if ((code & 0x60) == 0x40) {
        /* lines: the drawing area bounds them */
        x0 = scissor_rect.x - draw_offset.x;
        y0 = scissor_rect.y - draw_offset.y;
        x1 = x0 + scissor_rect.w;
        y1 = y0 + scissor_rect.h;
    } else {
        if (max_len < 2 + textured)
            return;
        const u32 xy = (u32)words[1];
        int w = 1, h = 1;
        switch (code & 0x18) {
        case 0x00:
            if (max_len < 3 + textured)
                return;
            w = (int)((u32)words[2 + textured] & (VRAM_W - 1));
            h = (int)(((u32)words[2 + textured] >> 16) & (VRAM_H - 1));
            break;
        case 0x08: w = h = 1; break;
        case 0x10: w = h = 8; break;
        default: w = h = 16; break;
        }
        x0 = s11((short)(s11((short)xy) + draw_offset.x)) - draw_offset.x;
        y0 = s11((short)(s11((short)(xy >> 16)) + draw_offset.y)) - draw_offset.y;
        x1 = x0 + w;
        y1 = y0 + h;
        if (textured) {
            const u32 uv = (u32)words[2];
            u0 = (int)(uv & 255), u1 = u0 + w - 1;
            v0 = (int)((uv >> 8) & 255), v1 = v0 + h - 1;
            clut = (u16)(uv >> 16);
        }
    }
    if (textured)
        ClutCacheUpdate(tpage, clut);
    if (textured && batch_drawn.w > 0) {
        SDL_Rect read[2];
        const int n = TextureFootprint(tpage, u0, u1, v0, v1, read);
        for (int i = 0; i < n; i++) {
            if (RectsIntersect(read[i], batch_drawn)) {
                DRAW_FLUSH_FOR(DRAW_FLUSH_FEEDBACK);
                break;
            }
        }
    }
    /* what it draws, within the drawing area (which bounds it otherwise) */
    SDL_Rect area = scissor_rect;
    x0 += draw_offset.x, x1 += draw_offset.x;
    y0 += draw_offset.y, y1 += draw_offset.y;
    if (x0 >= -1024 && x1 < 1024 && y0 >= -1024 && y1 < 1024) {
        const int ax0 = SDL_max(x0, area.x), ay0 = SDL_max(y0, area.y);
        const int ax1 = SDL_min(x1 + 1, area.x + area.w);
        const int ay1 = SDL_min(y1 + 1, area.y + area.h);
        area = (SDL_Rect){ax0, ay0, ax1 - ax0, ay1 - ay0};
    }
    UniteRect(&batch_drawn, area);
}

static void DrawLineQuad(int x0, int y0, int r0, int g0, int b0, int x1, int y1,
                         int r1, int g1, int b1, bool semi, u16 tpage);
/* A run of pixels of one line on one row, [x, x + n) at row y in VRAM
 * coordinates, drawn as an untextured rectangle of one colour. */
static void DrawLineRun(int x, int y, int n, int r, int g, int b, bool semi,
                        u16 tpage) {
    Draw_EnsureBufferWillNotOverflow(4, 6);
    Vertex* q = vertex_cur;
    memset(q, 0, 4 * sizeof(*q));
    for (int k = 0; k < 4; k++) {
        q[k].x = (short)(x - draw_offset.x + ((k & 1) ? n : 0));
        q[k].y = (short)(y - draw_offset.y + ((k & 2) ? 1 : 0));
        q[k].r = (unsigned char)r;
        q[k].g = (unsigned char)g;
        q[k].b = (unsigned char)b;
        q[k].a = semi ? 0x80 : 0xFF;
        q[k].pgxp_pos[0] = (float)q[k].x;
        q[k].pgxp_pos[1] = (float)q[k].y;
        q[k].pgxp_pos[2] = -1.0f;
        SET_TC(&q[k], tpage, -1);
    }
    static const unsigned short order[6] = {0, 1, 2, 1, 3, 2};
    for (int k = 0; k < 6; k++)
        index_cur[k] = (u32)(n_vertices + order[k]);
    Draw_EnqueueBuffer(4, 6);
}

/* GP0 40h..5Fh: the GPU steps a line along its major axis with 32.32
 * positions and 12.12 colours (the CPU rasterizer's RasterLineNative), one
 * pixel per step, both ends included; lines spanning 1024+ columns or 512+
 * rows are not drawn. Pixels are emitted in VRAM coordinates as runs on one
 * row, so at higher internal resolutions each PS1 pixel covers its NxN block
 * and a dithered line keeps the PS1's per-pixel offsets. */
static void DrawLinePixels(int x0, int y0, int r0, int g0, int b0, int x1,
                           int y1, int r1, int g1, int b1, bool semi,
                           u16 tpage) {
    x0 += draw_offset.x;
    y0 += draw_offset.y;
    x1 += draw_offset.x;
    y1 += draw_offset.y;
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int k = dx > dy ? dx : dy;
    if (dx >= VRAM_W || dy >= VRAM_H)
        return;
    if (x0 >= x1 && k > 0) {
        int t;
        t = x0, x0 = x1, x1 = t;
        t = y0, y0 = y1, y1 = t;
        t = r0, r0 = r1, r1 = t;
        t = g0, g0 = g1, g1 = t;
        t = b0, b0 = b1, b1 = t;
    }
    int64_t step_x = 0, step_y = 0;
    int32_t step_c[3] = {0, 0, 0};
    const int c0[3] = {r0, g0, b0}, c1[3] = {r1, g1, b1};
    if (k) {
        const int64_t delta[2] = {x1 - x0, y1 - y0};
        int64_t step[2];
        for (int i = 0; i < 2; i++)
            step[i] = (delta[i] * 4294967296LL - (delta[i] < 0 ? k - 1 : 0) +
                       (delta[i] > 0 ? k - 1 : 0)) / k;
        step_x = step[0];
        step_y = step[1];
        for (int i = 0; i < 3; i++)
            step_c[i] = (int32_t)((uint32_t)(c1[i] - c0[i]) << 12) / k;
    }
    int64_t x = (int64_t)x0 * 4294967296LL + (1LL << 31) - 1024;
    int64_t y = (int64_t)y0 * 4294967296LL + (1LL << 31) -
                (step_y < 0 ? 1024 : 0);
    int32_t c[3];
    for (int i = 0; i < 3; i++)
        c[i] = (c0[i] << 12) | (1 << 11);
    int run_x = 0, run_y = 0, run_n = 0, run_c[3] = {0, 0, 0};
    for (int i = 0; i <= k; i++) {
        /* outside the drawing area (the scissor) unless within the VRAM */
        int px = s11((short)(x >> 32)), py = s11((short)(y >> 32));
        int rgb[3] = {(u8)(c[0] >> 12), (u8)(c[1] >> 12), (u8)(c[2] >> 12)};
        if (run_n && py == run_y && px == run_x + run_n && rgb[0] == run_c[0] &&
            rgb[1] == run_c[1] && rgb[2] == run_c[2]) {
            run_n++;
        } else {
            if (run_n)
                DrawLineRun(run_x, run_y, run_n, run_c[0], run_c[1], run_c[2],
                            semi, tpage);
            run_x = px, run_y = py, run_n = 1;
            run_c[0] = rgb[0], run_c[1] = rgb[1], run_c[2] = rgb[2];
        }
        x += step_x;
        y += step_y;
        for (int j = 0; j < 3; j++)
            c[j] += step_c[j];
    }
    if (run_n)
        DrawLineRun(run_x, run_y, run_n, run_c[0], run_c[1], run_c[2], semi,
                    tpage);
}

int Draw_PushPrim(u_long* packets, int max_len) {
    PresentDue(); /* a paced present due at the blank goes first */
    if (UseSoftwareBackend()) {
        next_sprite_extension=false;
        return software_rasterizer.push_prim(packets,max_len);
    }
    interpolation_packet_current=packets;
    int len = max_len;
    int code = (int)(*packets >> 24) & 0xFF;
    BatchTextureFeedback(packets, max_len);
    bool isPoly = !(code & 0x40);
    bool isLine = (code & 0x40) && !(code & 0x20);
    bool isTile = (code & 0x40) && (code & 0x20);
    bool isTextured = (code & TEXTURED) != 0;
    bool isGouraud = (code & GOURAUD) != 0;
    SpriteAtlas *atlas=(isTile && isTextured && (code&~3)==0x7C) ? next_sprite_atlas : NULL;
    unsigned atlas_cell=next_sprite_cell;
    bool atlas_extension=next_sprite_extension;
    next_sprite_atlas=NULL;next_sprite_extension=false;
    /* One shared atlas: selecting a replacement must not split PS1 batches. */

    bool isShadeTex = !((code & 1) && isTextured && !isLine);
    u16 tpage = -1, clut = -1, pad2, pad3;
    Vertex* v;

    // A quad has two independent attribute planes, hence six vertices, or
    // two exact triangles of up to four bounding boxes each (32 vertices,
    // 48 indices). This also covers Gouraud's fifth-slot colour read-ahead.
    Draw_EnsureBufferWillNotOverflow(32, 48);
    v = vertex_cur;
    for (int i=0;i<8;++i) {
        v[i].exact[0][3]=0;
        v[i].hd_uv[0]=v[i].hd_uv[1]=0;
        v[i].hd_uv[2]=(atlas_extension&&!atlas&&isTile&&isTextured&&(code&~3)==0x7c)?3:0;
        v[i].pgxp_pos[0] = 0.0f;
        v[i].pgxp_pos[1] = 0.0f;
        v[i].pgxp_pos[2] = -1.0f;
        v[i].t = 0;
    }
    if (isShadeTex) {
        v->r = (unsigned char)(*packets >> 0);
        v->g = (unsigned char)(*packets >> 8);
        v->b = (unsigned char)(*packets >> 16);
    } else {
        v->r = v->g = v->b = 0x80;
    }
    v->a = code & SEMITRANSP ? 0x80 : 0xFF;
    packets++;
    len--;
    if (isPoly) {
        if (code & TRIANGLE) {
            int wr, nVertices, nIndices;
            wr = writePacket(v++, code, len, packets, &clut);
            packets += wr;
            len -= wr;
            wr = writePacket(v++, code, len, packets, &tpage);
            packets += wr;
            len -= wr;
            wr = writePacket(v++, code, len, packets, &pad2);
            packets += wr;
            len -= wr;

            index_cur[0] = n_vertices + 0;
            index_cur[1] = n_vertices + 1;
            index_cur[2] = n_vertices + 2;
            if (code & EXTRA_VERTEX) {
                index_cur[3] = n_vertices + 1;
                index_cur[4] = n_vertices + 3;
                index_cur[5] = n_vertices + 2;
                nVertices = 4;
                nIndices = 6;
                wr = writePacket(v, code, len, packets, &pad3);
                packets += wr;
                len -= wr;
            } else {
                nVertices = 3;
                nIndices = 3;
            }
            if (isGouraud && wr == 2 + isTextured) {
                packets--;
                len++;
            }

            if (!isTextured) {
                clut = -1;
                tpage = cur_tpage | TPAGE_NOTEXTURE;
            } else {
                /* the polygon's page becomes the drawing mode (GPUSTAT
                 * bits 0-8), as for an E1h command: later sprites use it */
                cur_tpage = (u_short)((cur_tpage & ~0x1FF) | (tpage & 0x1FF));
            }
            if (CanPolyDither(isGouraud, isTextured, isShadeTex)) {
                tpage |= TPAGE_DITHER;
            }
            /* the GPU skips a triangle spanning 1024+ columns or 512+ rows */
            interp_size_culled = 0;
            for (int t = 0; t < nIndices / 3; ++t) {
                static const int corner[2][3] = {{0, 1, 2}, {2, 1, 3}};
                int x0 = vertex_cur[corner[t][0]].x, x1 = x0;
                int y0 = vertex_cur[corner[t][0]].y, y1 = y0;
                for (int k = 1; k < 3; ++k) {
                    const Vertex* q = &vertex_cur[corner[t][k]];
                    x0 = q->x < x0 ? q->x : x0;
                    x1 = q->x > x1 ? q->x : x1;
                    y0 = q->y < y0 ? q->y : y0;
                    y1 = q->y > y1 ? q->y : y1;
                }
                if (x1 - x0 >= VRAM_W || y1 - y0 >= VRAM_H) {
                    index_cur[t * 3] = index_cur[t * 3 + 1] =
                        index_cur[t * 3 + 2] = n_vertices + corner[t][0];
                    interp_size_culled |= 1u << t;
                }
            }
            if (!isGouraud || !isShadeTex) {
                VRGBA(vertex_cur[1]) = VRGBA(vertex_cur[2]) =
                    VRGBA(vertex_cur[3]) = VRGBA(vertex_cur[0]);
            }
            if (!isTextured) {
                WidenFullWidthFill(vertex_cur, nVertices);
            }

            {
                bool pgxp_valid = true;
                float min_w = 1e30f;
                float max_w = 0.0f;
                for (int i = 0; i < nVertices; ++i) {
                    float w = vertex_cur[i].pgxp_pos[2];
                    if (w <= 0.0f) {
                        pgxp_valid = false;
                        break;
                    }
                    if (w < min_w) {
                        min_w = w;
                    }
                    if (w > max_w) {
                        max_w = w;
                    }
                }
                if ((geometry_correction || effect_geometry_correction) && pgxp_trace_budget) {
                    bool finite=true;
                    for (int i=0;i<nVertices;++i) for(int k=0;k<3;++k)
                        if(SDL_isnanf(vertex_cur[i].pgxp_pos[k]) || SDL_isinff(vertex_cur[i].pgxp_pos[k]))finite=false;
                    if(pgxp_valid)++pgxp_valid_polygons;else ++pgxp_fallback_polygons;
                    unsigned positive=0;
                    for(int i=0;i<nVertices;++i) if(vertex_cur[i].pgxp_pos[2]>0.0f)++positive;
                    if(!positive)++pgxp_none_polygons;
                    else if(positive<(unsigned)nVertices)++pgxp_mixed_polygons;
                    if(!finite)++pgxp_nonfinite_polygons;
                }
                if (!pgxp_valid) {
                    for (int i = 0; i < nVertices; ++i) {
                        vertex_cur[i].pgxp_pos[2] = -1.0f;
                    }
                } else if (max_w > min_w * 8.0f) {
                    for (int i = 0; i < nVertices; ++i) {
                        vertex_cur[i].pgxp_pos[2] = 1.0f;
                    }
                }
            }

            {
                bool is_model_poly = true;
                for (int i = 0; i < nVertices; ++i) {
                    if (!(vertex_cur[i].t & TPAGE_MODEL) ||
                        vertex_cur[i].pgxp_pos[2] <= 0.0f) {
                        is_model_poly = false;
                        break;
                    }
                }
                if (is_model_poly) {
                    tpage |= TPAGE_MODEL;
                    if (!geometry_correction) {
                        for (int i = 0; i < nVertices; ++i) {
                            vertex_cur[i].pgxp_pos[2] = -1.0f;
                        }
                    }
                }
            }
            bool exact_polygon = ExactPolygonsActive();
            for (int i = 0; i < nVertices; ++i)
                exact_polygon &= vertex_cur[i].hd_uv[2] == 0.0f;
            if (exact_polygon) {
                Vertex source[4];
                memcpy(source, vertex_cur, sizeof(Vertex) * nVertices);
                for (int i = 0; i < nVertices; ++i)
                    SET_TC(&source[i], tpage | TPAGE_POLYGON, clut);
                static const int corner[2][3] = {{0, 1, 2}, {2, 1, 3}};
                const int triangles = nIndices / 3;
                bool culled[2] = {false, false};
                for (int t = 0; t < triangles; ++t)
                    /* culled above: the indices collapsed to one vertex */
                    culled[t] = index_cur[t * 3] == index_cur[t * 3 + 1];
                /* The second triangle of a quad reads texels the first one
                 * drew: draw them in separate batches. */
                bool split = false;
                if (triangles == 2 && isTextured && !culled[0]) {
                    SDL_Rect read[2], drawn = {0, 0, 0, 0};
                    int u0 = 255, u1 = 0, v0 = 255, v1 = 0;
                    for (int i = 0; i < 4; ++i) {
                        u0 = SDL_min(u0, source[i].u), u1 = SDL_max(u1, source[i].u);
                        v0 = SDL_min(v0, source[i].v), v1 = SDL_max(v1, source[i].v);
                    }
                    for (int i = 0; i < 3; ++i)
                        UniteRect(&drawn, (SDL_Rect){source[i].x + draw_offset.x,
                                                     source[i].y + draw_offset.y, 1, 1});
                    const int n = TextureFootprint(tpage, u0, u1, v0, v1, read);
                    for (int i = 0; i < n; ++i)
                        split |= RectsIntersect(read[i], drawn);
                }
                for (int t = 0; t < triangles; ++t) {
                    if (culled[t])
                        continue;
                    if (t == 1 && split)
                        DRAW_FLUSH_FOR(DRAW_FLUSH_FEEDBACK);
                    const int emitted = EmitExactTriangle(
                        &source[corner[t][0]], &source[corner[t][1]],
                        &source[corner[t][2]], vertex_cur);
                    for (int q = 0; q < emitted / 4; ++q) {
                        static const unsigned short order[6] = {0, 1, 2, 1, 3, 2};
                        for (int k = 0; k < 6; ++k)
                            index_cur[q * 6 + k] =
                                (u32)(n_vertices + q * 4 + order[k]);
                    }
                    Draw_EnqueueBuffer(emitted, emitted / 4 * 6);
                    batch_has_texture |= isTextured;
                }
                return max_len - len;
            }
            if (nVertices == 4) {
                /* The GPU draws a quad as (v0, v1, v2) and (v2, v1, v3); the
                 * order picks the leftmost vertex on ties, the origin of
                 * the attribute gradients. */
                Vertex second[3] = {vertex_cur[2], vertex_cur[1],
                                    vertex_cur[3]};
                memcpy(vertex_cur + 3, second, sizeof(second));
                {
                    float *depth = &vertex_depth[vertex_cur - vertex_buf];
                    const float second_depth[3] = {depth[2], depth[1], depth[3]};
                    memcpy(depth + 3, second_depth, sizeof(second_depth));
                }
                const bool culled = index_cur[3] == index_cur[4];
                index_cur[3] = n_vertices + 3;
                index_cur[4] = n_vertices + (culled ? 3 : 4);
                index_cur[5] = n_vertices + (culled ? 3 : 5);
                nVertices = 6;
                SetPolygonAttributes(vertex_cur + 3);
            }
            SetPolygonAttributes(vertex_cur);
            for (int i = 0; i < nVertices; ++i) {
                SET_TC(&vertex_cur[i], tpage | TPAGE_POLYGON, clut);
                vertex_cur[i].exact[4][3] = clut_cache_row;
            }
            Draw_EnqueueBuffer(nVertices, nIndices);
        } else {
            // shouldn't happen on a normal PSX application
            WARNF("code %02X not supported", code);
        }
    } else if (isLine) {
        int length, count = Draw_LineVertices(packets - 1, len + 1, &length);
        if (length > len + 1) { /* truncated command */
            length = len + 1;
            count = 0;
        }
        u16 lt = cur_tpage | TPAGE_NOTEXTURE;
        if (CanLineDither()) {
            lt |= TPAGE_DITHER;
        }
        const bool semi = (code & SEMITRANSP) != 0;
        /* A line paired for the midpoint is drawn twice: its own pose only
         * into the frame, the midpoint pose only into the midpoint target. */
        int mid_x[INTERP_LINE_POINTS], mid_y[INTERP_LINE_POINTS];
        bool own_line, own_only;
        const bool mid_line = InterpLine(packets, count, isGouraud, mid_x, mid_y, &own_line, &own_only);
        for (int pass = own_line ? 0 : 1; pass < (mid_line ? 2 : own_line ? 1 : 0); pass++) {
        interp_line_mode = mid_line || own_only ? 1 + pass : 3;
        const bool placed = pass == 1 && interp_line_pend >= 0;
        if (placed) {
            /* its quads stay in one batch (InterpFinishLines); start and
             * end are in the frame's midpoint store */
            Draw_EnsureBufferWillNotOverflow(4 * (count - 1), 6 * (count - 1));
            interp_line_pending[interp_line_pend].start = interp_mid_len + n_vertices;
        }
        int x0 = 0, y0 = 0, r0 = v->r, g0 = v->g, b0 = v->b;
        int at = 0;
        for (int i = 0; i < count; i++) {
            int r1 = r0, g1 = g0, b1 = b0;
            if (i && isGouraud) {
                r1 = ((u8*)&packets[at])[0];
                g1 = ((u8*)&packets[at])[1];
                b1 = ((u8*)&packets[at])[2];
                at++;
            }
            const int x1 = pass ? mid_x[i] : s11(((short*)&packets[at])[0]);
            const int y1 = pass ? mid_y[i] : s11(((short*)&packets[at])[1]);
            at++;
            if (i && placed)
                DrawLineQuad(x0, y0, r0, g0, b0, x1, y1, r1, g1, b1, semi, lt);
            else if (i)
                DrawLinePixels(x0, y0, r0, g0, b0, x1, y1, r1, g1, b1, semi, lt);
            x0 = x1, y0 = y1, r0 = r1, g0 = g1, b0 = b1;
        }
        if (placed) {
            InterpLinePending* q = &interp_line_pending[interp_line_pend];
            if (q->start <= interp_mid_len + n_vertices) {
                q->end = interp_mid_len + n_vertices;
                ++interp_line_pending_len;
            }
        }
        }
        interp_line_mode = 0;
        interp_line_pend = -1;
        packets += length - 1;
        len -= length - 1;
    } else if (isTile) {
        int x, y, w, h, tu, tv;
        x = s11(((short*)packets)[0]);
        y = s11(((short*)packets)[1]);
        packets++;
        len--;
        if (isTextured) {
            tu = ((u8*)packets)[0];
            tv = ((u8*)packets)[1];
            clut = ((s16*)packets)[1];
            tpage = cur_tpage;
            packets++;
            len--;
        } else {
            clut = -1;
            tpage = cur_tpage | TPAGE_NOTEXTURE;
        }
        switch (code & ~3) {
        case 0x60: // TILE
        case 0x64: // SPRT
            w = ((u16*)packets)[0] & (VRAM_W - 1);
            h = ((u16*)packets)[1] & (VRAM_H - 1);
            packets++;
            len--;
            break;
        case 0x68: // TILE_1
        case 0x6C:
            w = 1;
            h = 1;
            break;
        case 0x70: // TILE_8
        case 0x74: // SPRT_8
            w = 8;
            h = 8;
            break;
        case 0x78: // TILE_16
        case 0x7C: // SPRT_16
            w = 16;
            h = 16;
            break;
        default:
            // TODO warn about unrecognized code
            break;
        }
        /* rectangle positions wrap to 11 bits after the drawing offset */
        x = s11((short)(x + draw_offset.x)) - draw_offset.x;
        y = s11((short)(y + draw_offset.y)) - draw_offset.y;
        vertex_cur[0].x = (short)(x);
        vertex_cur[0].y = (short)(y);
        vertex_cur[1].x = (short)(x + w);
        vertex_cur[1].y = (short)(y);
        vertex_cur[2].x = (short)(x);
        vertex_cur[2].y = (short)(y + h);
        vertex_cur[3].x = (short)(x + w);
        vertex_cur[3].y = (short)(y + h);
        if (isTextured) {
            vertex_cur[0].u = tu;
            vertex_cur[0].v = tv;
            vertex_cur[1].u = tu + w;
            vertex_cur[1].v = tv;
            vertex_cur[2].u = tu;
            vertex_cur[2].v = tv + h;
            vertex_cur[3].u = tu + w;
            vertex_cur[3].v = tv + h;
            /* Widescreen: a squeezed sprite reads only its own texels
             * (psx.frag.glsl); otherwise the bounds hold anything. */
            const bool own_texels = WidescreenDraw();
            for (int i = 0; i < 4; ++i) {
                vertex_cur[i].uv_bounds[0] = own_texels ? (float)tu : -32768.0f;
                vertex_cur[i].uv_bounds[1] = own_texels ? (float)tv : -32768.0f;
                vertex_cur[i].uv_bounds[2] =
                    own_texels ? (float)(tu + w - 1) : 32767.0f;
                vertex_cur[i].uv_bounds[3] =
                    own_texels ? (float)(tv + h - 1) : 32767.0f;
            }
        }
        VRGBA(vertex_cur[1]) = VRGBA(vertex_cur[2]) = VRGBA(vertex_cur[3]) =
            VRGBA(vertex_cur[0]);
        if (!isTextured) {
            WidenFullWidthFill(vertex_cur, 4);
        }
        index_cur[0] = n_vertices + 0;
        index_cur[1] = n_vertices + 1;
        index_cur[2] = n_vertices + 2;
        index_cur[3] = n_vertices + 1;
        index_cur[4] = n_vertices + 3;
        index_cur[5] = n_vertices + 2;
        if (atlas) {
            float width=(float)HD_ATLAS_WIDTH,height=(float)HD_ATLAS_HEIGHT;
            atlas_cell+=atlas->first_cell;
            float left=(float)((atlas_cell%HD_ATLAS_COLUMNS)*68+2),top=(float)((atlas_cell/HD_ATLAS_COLUMNS)*68+2);
            for (int i=0;i<4;++i) {
                vertex_cur[i].hd_uv[0]=(left+((i&1)?64.0f:0.0f))/width;
                vertex_cur[i].hd_uv[1]=(top+((i&2)?64.0f:0.0f))/height;
                vertex_cur[i].hd_uv[2]=atlas_extension?(widescreen&&sprite_atlases_enabled?2:3):(sprite_atlases_enabled?1:0);
            }
        }
        for (int i = 0; i < 4; ++i) {
            vertex_cur[i].pgxp_pos[0] = (float)vertex_cur[i].x;
            vertex_cur[i].pgxp_pos[1] = (float)vertex_cur[i].y;
            vertex_cur[i].pgxp_pos[2] = -1.0f;
        }
        SET_TC_ALL(vertex_cur, tpage, clut);
        for (int i = 0; i < 4; ++i)
            vertex_cur[i].exact[4][3] = clut_cache_row;
        Draw_EnqueueBuffer(4, 6);
    }
    batch_has_texture |= isTextured;
    return max_len - len;
}

void Draw_SetAreaStart(int x, int y) {
    if (UseSoftwareBackend()) {
        software_rasterizer.area_start(x, y);
        draw_area_start.x=x;draw_area_start.y=y;
        return;
    }
    if (draw_area_start.x == x && draw_area_start.y == y)
        return;
    DrawStateChanging();
    draw_area_start.x = x;
    draw_area_start.y = y;
    /* GP0 E3 and E4 each take effect on their own */
    SetScissorFromArea();
}
void Draw_SetAreaEnd(int x, int y) {
    if (UseSoftwareBackend()) {
        software_rasterizer.area_end(x, y);
        draw_area_end.x=x;draw_area_end.y=y;
        return;
    }
    if (draw_area_end.x == x && draw_area_end.y == y)
        return;
    DrawStateChanging();
    draw_area_end.x = x;
    draw_area_end.y = y;
    SetScissorFromArea();
}
void Draw_SetOffset(int x, int y) {
    if (UseSoftwareBackend()) {
        software_rasterizer.offset(x, y);
        return;
    }
    /* GP0 E5h: signed 11-bit offsets */
    if (draw_offset.x == s11((short)x) && draw_offset.y == s11((short)y))
        return;
    DrawStateChanging();
    draw_offset.x = s11((short)x);
    draw_offset.y = s11((short)y);
}

void Draw_ClearImage(PS1_RECT* rect, u_char r, u_char g, u_char b) {
    PresentDue(); /* a paced present due at the blank goes first */
    if (UseSoftwareBackend()) {
        software_rasterizer.clear_image(rect, r, g, b);
        return;
    }
    if (rect->w == 0 || rect->h == 0) {
        return;
    }
    if (!sdl3_window && !InitPlatform()) {
        return;
    }
    /* the fill comes after the primitives queued before it */
    Draw_FlushBuffer();
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    if (!cmd) {
        return;
    }

    Vertex quad[4] = {0};
    quad[0].x = rect->x;
    quad[0].y = rect->y;
    quad[1].x = (short)(rect->x + rect->w);
    quad[1].y = rect->y;
    quad[2].x = rect->x;
    quad[2].y = (short)(rect->y + rect->h);
    quad[3].x = (short)(rect->x + rect->w);
    quad[3].y = (short)(rect->y + rect->h);
    /* The fill writes RGB555 before any later polygon blends with VRAM. */
    for (int i = 0; i < 4; i++) {
        quad[i].r = color_5to8(r >> 3);
        quad[i].g = color_5to8(g >> 3);
        quad[i].b = color_5to8(b >> 3);
        quad[i].a = 0;
    }

    Uint32 clear_at, clear_index_at;
    const bool clear_cycle = RingReserve(4, 0, &clear_at, &clear_index_at);
    Vertex* map = SDL_MapGPUTransferBuffer(device, vtx_transfer, clear_cycle);
    if (!map) {
        ERRORF("SDL_MapGPUTransferBuffer: %s", SDL_GetError());
        return;
    }
    memcpy(map + clear_at, quad, sizeof(quad));
    SDL_UnmapGPUTransferBuffer(device, vtx_transfer);

    cmd = ReserveDrawDescriptors(1);
    if (!cmd)
        return;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation src = {
        .transfer_buffer = vtx_transfer, .offset = clear_at * (Uint32)sizeof(Vertex)};
    const SDL_GPUBufferRegion dst = {
        .buffer = clear_vbuf, .size = sizeof(quad)};
    SDL_UploadToGPUBuffer(copy, &src, &dst, true);
    SDL_EndGPUCopyPass(copy);

    const SDL_GPUColorTargetInfo target = {
        .texture = vram_render,
        .load_op = SDL_GPU_LOADOP_LOAD,
        .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, NULL);
    SDL_BindGPUGraphicsPipeline(pass, pipe_clear);
    const SDL_GPUBufferBinding vb = {.buffer = clear_vbuf};
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    MarkVramDirty((SDL_Rect){rect->x, rect->y, rect->w, rect->h});
    SyncNativeVramToScaled(rect->x, rect->y, rect->w, rect->h);
}

static void UploadVram(PS1_RECT* rect, const u16* p);
static bool DownloadVramRegionAsRGBA8888(int x, int y, int w, int h, u8* out);

/* Uploads and copies under a mask setting (GP0 E6h): a pixel whose bit 15
 * is set is kept while "check mask" is on, and "set mask" forces bit 15 of
 * each written pixel. The destination is read back and merged on the CPU,
 * a copy row by row from the top like the GPU (each row read before it is
 * written). src is the uploaded image, or NULL to copy from (sx, sy).
 * Returns false when no mask setting applies. */
static bool MaskedVramWrite(PS1_RECT* rect, const u16* src, int sx, int sy) {
    if (!mask_set_bit && !mask_check_bit)
        return false;
    Draw_FlushBuffer();
    const size_t pixels = (size_t)VRAM_W * VRAM_H;
    u8* rgba = malloc(pixels * 4);
    u16* vram = malloc(pixels * sizeof(u16));
    u16* out = malloc((size_t)rect->w * rect->h * sizeof(u16));
    if (rgba && vram && out &&
        DownloadVramRegionAsRGBA8888(0, 0, VRAM_W, VRAM_H, rgba)) {
        ConvertRgba8888ToRgb5551(rgba, vram, pixels);
        for (int row = 0; row < rect->h; row++) {
            u16* dst = &vram[(size_t)(rect->y + row) * VRAM_W + rect->x];
            const u16* from =
                src ? &src[(size_t)row * rect->w]
                    : &vram[(size_t)(sy + row) * VRAM_W + sx];
            u16 line[VRAM_W];
            memcpy(line, from, (size_t)rect->w * sizeof(u16));
            for (int i = 0; i < rect->w; i++)
                if (!mask_check_bit || !(dst[i] & 0x8000))
                    dst[i] = (u16)(line[i] | (mask_set_bit ? 0x8000 : 0));
            memcpy(&out[(size_t)row * rect->w], dst,
                   (size_t)rect->w * sizeof(u16));
        }
        UploadVram(rect, out);
    }
    free(rgba);
    free(vram);
    free(out);
    return true;
}

void Draw_LoadImage(PS1_RECT* rect, u_long* p) {
    PresentDue(); /* a paced present due at the blank goes first */
    if (UseSoftwareBackend()) {
        software_rasterizer.load_image(rect, p);
        return;
    }
    if (rect->w == 0 || rect->h == 0) {
        return;
    }
    if (rect->x < 0 || rect->y < 0 || rect->w < 0 || rect->h < 0 ||
        rect->x + rect->w > VRAM_W || rect->y + rect->h > VRAM_H) {
        WARNF("rect (%d,%d %dx%d) is out of bound %dx%d, skipping upload",
              rect->x, rect->y, rect->w, rect->h, VRAM_W, VRAM_H);
        return;
    }
    if (!sdl3_window && !InitPlatform()) {
        return;
    }
    if (MaskedVramWrite(rect, (const u16*)p, 0, 0))
        return;
    UploadVram(rect, (const u16*)p);
}

/* Writes native RGB555 pixels into the GPU VRAM (and the scaled target). */
static void UploadVram(PS1_RECT* rect, const u16* p) {
    /* (a wrap submits the command buffer: mapped before acquiring one) */
    Uint32 upload_at;
    u8* map = UploadMap((Uint32)(rect->w * rect->h * 4), &upload_at);
    if (!map) {
        ERRORF("SDL_MapGPUTransferBuffer: %s", SDL_GetError());
        return;
    }
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    if (!cmd) {
        SDL_UnmapGPUTransferBuffer(device, tex_upload_transfer);
        return;
    }
    ConvertRgb5551ToRgba8888(p, map, (size_t)rect->w * rect->h);
    SDL_UnmapGPUTransferBuffer(device, tex_upload_transfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTextureTransferInfo src = {
        .transfer_buffer = tex_upload_transfer,
        .offset = upload_at,
        .pixels_per_row = rect->w,
        .rows_per_layer = rect->h,
    };
    const SDL_GPUTextureRegion dst = {
        .texture = vram_render,
        .x = (Uint32)rect->x,
        .y = (Uint32)rect->y,
        .w = rect->w,
        .h = rect->h,
        .d = 1,
    };
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    MarkVramDirty((SDL_Rect){rect->x, rect->y, rect->w, rect->h});
    SyncNativeVramToScaled(rect->x, rect->y, rect->w, rect->h);
}

void Draw_StoreImage(PS1_RECT* rect, u_long* p) {
    PresentDue(); /* a paced present due at the blank goes first */
    if (UseSoftwareBackend()) {
        software_rasterizer.store_image(rect, p);
        return;
    }
    if (rect->w == 0 || rect->h == 0) {
        return;
    }
    if (rect->x < 0 || rect->y < 0 || rect->w < 0 || rect->h < 0 ||
        rect->x + rect->w > VRAM_W || rect->y + rect->h > VRAM_H) {
        WARNF("rect (%d,%d %dx%d) is out of bound %dx%d, skipping download",
              rect->x, rect->y, rect->w, rect->h, VRAM_W, VRAM_H);
        return;
    }
    if (!device || !vram_render) {
        return;
    }
    Draw_FlushBuffer(); // flush primitives before operating with the VRAM

    const size_t pixels = (size_t)rect->w * rect->h;
    u8* rgba = malloc(pixels * 4);
    if (!rgba) {
        return;
    }
    if (DownloadVramRegionAsRGBA8888(
            rect->x, rect->y, rect->w, rect->h, rgba)) {
        ConvertRgba8888ToRgb5551(rgba, (u16*)p, pixels);
    }
    free(rgba);
}

void Draw_MoveImage(PS1_RECT* rect, unsigned int x, unsigned int y) {
    PresentDue(); /* a paced present due at the blank goes first */
    if (UseSoftwareBackend()) {
        software_rasterizer.move_image(rect, x, y);
        return;
    }
    if (rect->x >= 0 && rect->y >= 0 && rect->w > 0 && rect->h > 0 &&
        rect->x + rect->w <= VRAM_W && rect->y + rect->h <= VRAM_H &&
        (int)x + rect->w <= VRAM_W && (int)y + rect->h <= VRAM_H) {
        PS1_RECT dst = {(short)x, (short)y, rect->w, rect->h};
        if (MaskedVramWrite(&dst, NULL, rect->x, rect->y))
            return;
    }
    if (rect->x == x && rect->y == y) {
        return;
    }
    Draw_FlushBuffer(); // flush primitives before operating with the VRAM

    int src_x = CLAMP(rect->x, 0, VRAM_W - 1);
    int src_y = CLAMP(rect->y, 0, VRAM_H - 1);
    int src_w = CLAMP(rect->w, 0, VRAM_W - src_x);
    int src_h = CLAMP(rect->h, 0, VRAM_H - src_y);
    int dst_x = CLAMP((int)x, 0, VRAM_W - 1);
    int dst_y = CLAMP((int)y, 0, VRAM_H - 1);
    int copy_w = CLAMP(src_w, 0, VRAM_W - dst_x);
    int copy_h = CLAMP(src_h, 0, VRAM_H - dst_y);

    if (src_x != rect->x || src_y != rect->y || src_w != rect->w ||
        src_h != rect->h || dst_x != (int)x || dst_y != (int)y ||
        copy_w != src_w || copy_h != src_h) {
        WARNF("params out of bounds: src=(%d,%d,%d,%d)->(%d,%d,%d,%d) "
              "dst=(%d,%d)->(%d,%d)",
              rect->x, rect->y, rect->w, rect->h, src_x, src_y, src_w, src_h,
              (int)x, (int)y, dst_x, dst_y);
    }

    if (copy_w <= 0 || copy_h <= 0) {
        WARNF("nothing to copy");
        return;
    }

    /* the copy goes through vram_sample at the source (as its native
     * pixels are now): the midpoint kept so far samples it first */
    if (InterpMidpointSamplesRect(src_x, src_y, copy_w, copy_h))
        InterpMidpointSync(INTERP_SYNC_TEXTURE);
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    if (!cmd) {
        return;
    }
    /* The GPU copies row by row from the top, each row read before it is
     * written: an overlapping copy goes one VRAM row at a time through a
     * scratch texture. The upscaled VRAM copies its own pixels, keeping
     * their detail (its first pixel of every NxN block is the native copy). */
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const bool overlap = RectsOverlap(src_x, src_y, dst_x, dst_y, copy_w, copy_h);
    const int rows = overlap ? copy_h : 1, step = overlap ? 1 : copy_h;
    for (unsigned target = 0; target < (scaled_vram_render ? 2u : 1u); ++target) {
        SDL_GPUTexture* vram = target ? scaled_vram_render : vram_render;
        SDL_GPUTexture* bounce = target ? scaled_blend_sample : vram_sample;
        const Uint32 n = target ? internal_res : 1;
        for (int row = 0; row < rows; row++) {
            const SDL_GPUTextureLocation src = {
                .texture = vram, .x = (Uint32)src_x * n,
                .y = (Uint32)(src_y + row) * n};
            const SDL_GPUTextureLocation scratch = {
                .texture = bounce, .x = src.x, .y = src.y};
            const SDL_GPUTextureLocation dst = {
                .texture = vram, .x = (Uint32)dst_x * n,
                .y = (Uint32)(dst_y + row) * n};
            SDL_CopyGPUTextureToTexture(copy, &src, &scratch,
                                        (Uint32)copy_w * n, (Uint32)step * n,
                                        1, false);
            SDL_CopyGPUTextureToTexture(copy, &scratch, &dst,
                                        (Uint32)copy_w * n, (Uint32)step * n,
                                        1, false);
        }
    }
    SDL_EndGPUCopyPass(copy);
    MarkVramDirty((SDL_Rect){dst_x, dst_y, copy_w, copy_h});
    MarkVramDirty((SDL_Rect){src_x, src_y, copy_w, copy_h});
    if (interpolated_vram && interpolated_scale == internal_res)
        SyncNativeVramToScaled(dst_x, dst_y, copy_w, copy_h);
}

/* The drawing state of queued primitives: mark k holds the state of the
 * indices before draw_marks[k].end (from the previous mark's end). The
 * primitives after the last mark have the current state. */
typedef struct DrawState {
    Posi offset, area_start, area_end;
    SDL_Rect scissor;
    bool mask_set, mask_check;
} DrawState;
#define DRAW_MARKS_MAX 1024
static struct {
    int end;
    DrawState state;
} draw_marks[DRAW_MARKS_MAX + 1];
static int n_draw_marks;

static DrawState CurrentDrawState(void) {
    return (DrawState){draw_offset, draw_area_start, draw_area_end,
                       scissor_rect, mask_set_bit, mask_check_bit};
}
static void ApplyDrawState(const DrawState* st) {
    draw_offset = st->offset;
    draw_area_start = st->area_start;
    draw_area_end = st->area_end;
    scissor_rect = st->scissor;
    mask_set_bit = st->mask_set;
    mask_check_bit = st->mask_check;
}
/* Called before the drawing state changes. */
static void DrawStateChanging(void) {
    if (!n_indices)
        return;
    if (n_draw_marks && draw_marks[n_draw_marks - 1].end == n_indices)
        return; /* no primitive has the state the last mark ended with */
    if (n_draw_marks == DRAW_MARKS_MAX) {
        DRAW_FLUSH_FOR(DRAW_FLUSH_MARKS);
        return;
    }
    draw_marks[n_draw_marks].end = n_indices;
    draw_marks[n_draw_marks++].state = CurrentDrawState();
}

static void InterpMidpointDropBatch(void);
void Draw_ResetBuffer(void) {
    n_draw_marks = 0;
    if (UseSoftwareBackend()) {
        software_rasterizer.reset_buffer();
        return;
    }
    /* a batch the midpoint did not keep (InterpMidpointRecord) takes its
     * pending placements with it */
    InterpMidpointDropBatch();
    n_vertices = 0;
    n_indices = 0;
    batch_primitives = 0;
    vertex_cur = vertex_buf;
    index_cur = index_buf;
    batch_has_texture = false;
    batch_drawn = (SDL_Rect){0, 0, 0, 0};
    clut_cache_loads = 0;
}

/* The target pixels the primitives at indices [start, end) can touch,
 * clipped to the drawing area; false when they touch none. */
typedef struct SemiBox {
    int x0, y0, x1, y1;
} SemiBox;
/* Primitives of one draw within a render pass (Draw_FlushBuffer). */
typedef struct DrawSegment {
    int start, end;
    bool semi;
    int mark; /* draw_marks index of its drawing state */
} DrawSegment;
#define DRAW_SEGMENTS_MAX 32
/* Runs in one pass when the shader fetches the destination (no copies). */
#define DRAW_SEGMENTS_FETCH_MAX 512
static bool PrimitiveBox(const Vertex* vertices, const u32* indices,
                         int start, int end, bool midpoint,
                         bool scaled_target, unsigned target_scale,
                         Posi offset, float sub_x, float sub_y, float target_centre,
                         float ws_squeeze, SDL_Rect clip, SemiBox* box) {
    float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
    /* Sprites and rectangles on whole target pixels cover exactly
     * [min, max); anything else may round to one more pixel each side. */
    bool rectangle = true;
    for (int i = start; i < end; ++i) {
        const Vertex* v = &vertices[indices[i]];
        float bias =
            (v->t & TPAGE_POLYGON) ? 0.5f / (float)target_scale : 0.0f;
        bool use_pgxp =
            (midpoint ||
             (scaled_target &&
              (geometry_correction || effect_geometry_correction) &&
              (v->t & TPAGE_POLYGON))) &&
            v->pgxp_pos[2] > 0.0f;
        float vx = (use_pgxp ? v->pgxp_pos[0] : (float)v->x) + sub_x;
        float vy = (use_pgxp ? v->pgxp_pos[1] : (float)v->y) + sub_y;
        float x = offset.x + (vx + bias) * GetDrawGridXScale();
        x = (target_centre + (x - target_centre) * ws_squeeze) * target_scale;
        float y = (offset.y + vy + bias) * target_scale;
        rectangle &= !(v->t & TPAGE_POLYGON) && x == SDL_floorf(x) &&
                     y == SDL_floorf(y);
        min_x = SDL_min(min_x, x);
        max_x = SDL_max(max_x, x);
        min_y = SDL_min(min_y, y);
        max_y = SDL_max(max_y, y);
    }
    int widen = rectangle ? 0 : 1;
    int x0 = (int)SDL_floorf(min_x) - widen;
    int x1 = (int)SDL_ceilf(max_x) + widen;
    int y0 = (int)SDL_floorf(min_y) - widen;
    int y1 = (int)SDL_ceilf(max_y) + widen;
    x0 = SDL_max(x0, SDL_max(0, clip.x) * (int)target_scale);
    y0 = SDL_max(y0, SDL_max(0, clip.y) * (int)target_scale);
    x1 = SDL_min(x1, SDL_min(VRAM_W, clip.x + clip.w) * (int)target_scale);
    y1 = SDL_min(y1, SDL_min(VRAM_H, clip.y + clip.h) * (int)target_scale);
    *box = (SemiBox){x0, y0, x1, y1};
    return x1 > x0 && y1 > y0;
}

static int SharedVertices(const u32* indices, int a, int b) {
    int shared = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            shared += indices[a + i] == indices[b + j];
    return shared;
}

/* What a target's draws need from one drawing state (ApplyDrawState). */
typedef struct DrawView {
    bool visible; /* the drawing area is not empty */
    SDL_Rect clip; /* drawing area, squeezed in widescreen (VRAM pixels) */
    Posi offset;
    float ws_squeeze, target_centre, sub_x, sub_y;
    SDL_GPUViewport viewport;
    SDL_Rect scissor; /* target pixels */
    float offset_ubo[8];
    u32 pass_flags;
    bool mask_check;
} DrawView;

static void ComputeDrawView(DrawView* view, unsigned target_index,
                            bool midpoint, unsigned target_scale) {
    view->visible = scissor_rect.w > 0 && scissor_rect.h > 0;
    view->offset = draw_offset;
    /* Widescreen squeezes X around the display buffer's centre. A clip
     * narrower than the screen (a menu list) is squeezed with its contents;
     * the full-width one is kept, so what lies past the screen's edges stays.
     */
    const float ws_squeeze =
        WidescreenDraw() ? WidescreenPictureSqueeze(WidescreenDrawPicture())
                         : 1.0f;
    const float ws_centre = WidescreenCentre();
    SDL_Rect clip = scissor_rect;
    if (ws_squeeze < 1.0f && clip.w < display_size.x) {
        float x0 = ws_centre + ((float)clip.x - ws_centre) * ws_squeeze;
        float x1 =
            ws_centre + ((float)(clip.x + clip.w) - ws_centre) * ws_squeeze;
        clip.x = (int)SDL_floorf(x0);
        clip.w = (int)SDL_ceilf(x1) - clip.x;
    }
    /* Widescreen: the room moves by its whole-pixel step (WidescreenFieldStep)
     * in the shown picture; native VRAM under a high-resolution one keeps
     * the game's own picture for read-backs. */
    const int field_picture =
        ws_field && WidescreenDraw() ? WidescreenDrawPicture() : -1;
    const float target_centre=midpoint&&!ws_centred&&WidescreenDrawPicture()>=0 ?
        (float)display_area.x+WidescreenPivot(ws_mid_shift[WidescreenDrawPicture()],WidescreenPictureSqueeze(WidescreenDrawPicture())) : ws_centre;
    float sub_x = 0.0f, sub_y = 0.0f;
    if (target_index || !scaled_vram_render)
        WidescreenFieldStep(
            field_picture, (float)target_scale, &sub_x, &sub_y);
    if(midpoint&&field_picture>=0){sub_x=ws_mid_qx[field_picture];sub_y=ws_mid_qy[field_picture];}
    view->clip = clip;
    view->ws_squeeze = ws_squeeze;
    view->target_centre = target_centre;
    view->sub_x = sub_x;
    view->sub_y = sub_y;
    const float grid_x = GetDrawGridXScale();
    const float render_scale = (float)target_scale;
    view->viewport = (SDL_GPUViewport){
        .x = (float)draw_offset.x * render_scale * (1.0f - grid_x),
        .y = 0.0f,
        .w = (float)VRAM_W * render_scale * grid_x,
        .h = (float)VRAM_H * render_scale,
        .min_depth = 0.0f,
        .max_depth = 1.0f,
    };
    view->scissor = (SDL_Rect){
        clip.x * (int)target_scale, clip.y * (int)target_scale,
        clip.w * (int)target_scale, clip.h * (int)target_scale};
    /* The viewport maps shader x to offset * (1 - grid) + x * grid;
     * the shader squeezes around the centre in its own space, so
     * VRAM x = centre + (offset + x * grid - centre) * squeeze. */
    const float offset_ubo[8] = {
        (float)draw_offset.x,
        (float)draw_offset.y,
        ((target_index != 0 &&
          (geometry_correction || effect_geometry_correction))
             ? 1.0f
             : 0.0f) +
            (midpoint ? 2.0f : 0.0f),
        PSX_SHADERS_CURRENT ? (float)target_scale
                            : (target_scale > 1 ? 1.0f : 0.0f),
        ws_squeeze,
        (target_centre - (float)draw_offset.x * (1.0f - grid_x)) / grid_x,
        sub_x,
        sub_y};
    memcpy(view->offset_ubo, offset_ubo, sizeof(offset_ubo));
    view->pass_flags =
        (target_index && model_filtering ? 1u : 0u) |
        (target_index && sprite_filtering ? 4u : 0u) |
        (dither_mode == PSYZ_DITHER_OFF ? 2u : 0u) |
        (PSX_SHADERS_CURRENT && mask_set_bit ? 8u : 0u) |
        (PSX_SHADERS_CURRENT && mask_check_bit ? 16u : 0u);
    view->mask_check = mask_check_bit;
}

/* InterpMidpointSync: the points of the first n queued lines (in the
 * store's first vertices) go where the polygon vertices they lie at went (within INTERP_LINE_REACH
 * pixels): a paired line's by at most a pixel and a half from the midpoint
 * of their own poses (not to another surface at the same place), a new
 * line's only when every point lies at one. The others keep the line's own
 * placement (a paired line the midpoint of its poses, a new one this
 * frame's pose). Each segment is a quad (DrawLineQuad), its first two
 * vertices at the segment's first point. */
static void InterpFinishLines(unsigned n,unsigned vertices) {
    for(unsigned k=0;k<n;++k){const InterpLinePending *q=&interp_line_pending[k];
        if(q->end<=q->start||q->end>vertices||q->end-q->start!=4u*(q->count-1u))continue;
        float to[INTERP_LINE_POINTS][2];unsigned found=0;
        for(unsigned i=0;i<q->count;++i){float m[2],d;
            to[i][0]=q->want[i][0];to[i][1]=q->want[i][1];
            if(!InterpFieldMotion(q->which,q->from[i],0.0f,q->ox,q->oy,m,&d)||d>INTERP_LINE_REACH*INTERP_LINE_REACH)continue;
            const float x=q->from[i][0]+m[0],y=q->from[i][1]+m[1];
            if(q->which==0&&(SDL_fabsf(x-q->want[i][0])>1.5f||SDL_fabsf(y-q->want[i][1])>1.5f))continue;
            to[i][0]=x;to[i][1]=y;++found;}
        if(q->which==1&&found<q->count)for(unsigned i=0;i<q->count;++i){to[i][0]=q->want[i][0];to[i][1]=q->want[i][1];}
        /* at 1x at whole pixels, as the line is drawn */
        if(internal_res<=1)for(unsigned i=0;i<q->count;++i){to[i][0]=SDL_floorf(to[i][0]+0.5f);to[i][1]=SDL_floorf(to[i][1]+0.5f);}
        for(unsigned v=q->start;v<q->end;++v){Vertex *out=&interp_mid_store[v];
            const unsigned i=(v-q->start)/4u+((v-q->start)%4u>=2u);
            out->pgxp_pos[0]=(float)out->x-q->at[i][0]+to[i][0];out->pgxp_pos[1]=(float)out->y-q->at[i][1]+to[i][1];out->pgxp_pos[2]=1.0f;}
    }
}
/* A line segment drawn into the midpoint as one quad over the pixels the
 * GPU's stepping covers (both ends included), moved at the flush. */
static void DrawLineQuad(int x0, int y0, int r0, int g0, int b0, int x1, int y1,
                         int r1, int g1, int b1, bool semi, u16 tpage) {
    Draw_EnsureBufferWillNotOverflow(4, 6);
    Vertex* q = vertex_cur;
    memset(q, 0, 4 * sizeof(*q));
    const bool across = abs(x1 - x0) >= abs(y1 - y0);
    const int e0 = across ? (x1 >= x0 ? 0 : 1) : (y1 >= y0 ? 0 : 1);
    const int x[4] = {x0 + (across ? e0 : 0), x0 + (across ? e0 : 1), x1 + (across ? 1 - e0 : 0), x1 + (across ? 1 - e0 : 1)};
    const int y[4] = {y0 + (across ? 0 : e0), y0 + (across ? 1 : e0), y1 + (across ? 0 : 1 - e0), y1 + (across ? 1 : 1 - e0)};
    for (int k = 0; k < 4; k++) {
        q[k].x = (short)x[k];
        q[k].y = (short)y[k];
        q[k].r = (unsigned char)(k < 2 ? r0 : r1);
        q[k].g = (unsigned char)(k < 2 ? g0 : g1);
        q[k].b = (unsigned char)(k < 2 ? b0 : b1);
        q[k].a = semi ? 0x80 : 0xFF;
        q[k].pgxp_pos[0] = (float)q[k].x;
        q[k].pgxp_pos[1] = (float)q[k].y;
        q[k].pgxp_pos[2] = -1.0f;
        SET_TC(&q[k], tpage, -1);
    }
    static const unsigned short order[6] = {0, 1, 2, 1, 3, 2};
    for (int k = 0; k < 6; k++)
        index_cur[k] = (u32)(n_vertices + order[k]);
    Draw_EnqueueBuffer(4, 6);
}
/* What one target draws of a batch (DrawTargetRuns): vertices and indices,
 * the end index and view of each drawing state, the texture bound third
 * (sprite atlas or VRAM) and whether the draw grid is 1:1. */
typedef struct DrawTargetBatch {
    SDL_GPUTexture* target;
    unsigned target_index, target_scale;
    bool midpoint;
    const Vertex* vertices;
    const u32* indices;
    int n_indices;
    Uint32 vertex_offset, index_offset; /* bytes into vbuf, ibuf */
    const int* mark_end;
    const DrawView* views;
    SDL_GPUTexture* extra_texture;
    SDL_GPUSampler* extra_sampler;
    bool grid_same;
} DrawTargetBatch;

/* Draws a batch into one target; false when the command buffer failed. */
static bool DrawTargetRuns(const DrawTargetBatch* b) {
    const Vertex* vb = b->vertices;
    const u32* ix = b->indices;
    const int n_indices = b->n_indices;
    SDL_GPUTexture* render_target = b->target;
    const unsigned target_index = b->target_index;
    const unsigned target_scale = b->target_scale;
    const bool midpoint = b->midpoint;
    SDL_GPUTexture* destination_sample =
        target_index && scaled_blend_sample ? scaled_blend_sample : blend_sample;
    int start = 0, mark = 0;
    while (start < n_indices) {
        /* A semi-transparent primitive blends with a copy of the VRAM
         * made before it: one triangle, or one bounding box (two
         * triangles that do not overlap) of an exact polygon. Each render
         * pass loads and stores the whole target, so one pass draws a
         * run of segments (consecutive opaque primitives, or one
         * semi-transparent primitive) from one copy: a semi-transparent
         * primitive joins it while its pixels miss those of every
         * earlier segment of the run, so the copy still holds its
         * destination. With destination_fetch the shader reads the
         * destination from the target itself: a run of semi-transparent
         * primitives is one draw and nothing breaks the pass. A segment
         * keeps to one drawing state (mark); the pass sets each one's
         * viewport, scissor and uniforms. */
        DrawSegment segments[DRAW_SEGMENTS_FETCH_MAX];
        SemiBox boxes[DRAW_SEGMENTS_FETCH_MAX];
        const int max_segments = destination_fetch ? DRAW_SEGMENTS_FETCH_MAX
                                                   : DRAW_SEGMENTS_MAX;
        int n_segments = 0, n_draws = 0;
        int end = start;
        while (end < n_indices && n_segments < max_segments) {
            while (b->mark_end[mark] <= end)
                ++mark;
            const int mark_end = b->mark_end[mark];
            const DrawView* view = &b->views[mark];
            if (!view->visible) {
                end = mark_end; // an empty drawing area draws nothing
                continue;
            }
            int seg_start = end;
            bool semi = vb[ix[seg_start]].a != 0xFF;
            int seg_end =
                seg_start + (vb[ix[seg_start]].exact[0][3] ? 6 : 3);
            /* The two triangles of one quad (two shared vertices) cover
             * disjoint pixels: they read the same copy. */
            if (semi && seg_end + 3 <= mark_end &&
                !vb[ix[seg_start]].exact[0][3] &&
                vb[ix[seg_end]].a != 0xFF &&
                SharedVertices(ix, seg_start, seg_end) >= 2)
                seg_end += 3;
            if (!semi || destination_fetch) {
                while (seg_end < mark_end &&
                       (vb[ix[seg_end]].a != 0xFF) == semi)
                    seg_end += 3;
            }
            if (seg_end > mark_end)
                seg_end = mark_end;
            SemiBox box;
            bool visible = PrimitiveBox(
                vb, ix, seg_start, seg_end, midpoint, target_index != 0,
                target_scale, view->offset, view->sub_x, view->sub_y,
                view->target_centre, view->ws_squeeze, view->clip, &box);
            if (semi && n_segments && !destination_fetch) {
                bool overlaps = false;
                for (int i = 0; visible && i < n_segments; ++i)
                    overlaps |= boxes[i].x1 > boxes[i].x0 &&
                                box.x0 < boxes[i].x1 &&
                                boxes[i].x0 < box.x1 &&
                                box.y0 < boxes[i].y1 && boxes[i].y0 < box.y1;
                if (overlaps)
                    break;
            }
            end = seg_end;
            if (semi && !visible)
                continue; // outside the drawing area: draws nothing
            if (!visible)
                box.x0 = box.x1 = 0;
            boxes[n_segments] = box;
            segments[n_segments++] =
                (DrawSegment){seg_start, seg_end, semi, mark};
            n_draws += semi ? 1 : 2;
        }
        if (!n_segments) {
            start = end;
            continue;
        }
        /* Up to four sampled textures for each draw. Reserve both heaps'
         * worst-case sampler use before any destination copy. */
        SDL_GPUCommandBuffer* cmd = ReserveDrawDescriptors(4u * (unsigned)n_draws);
        if (!cmd)
            return false;
        SDL_GPUCopyPass* blend_copy = NULL;
        for (int i = 0; i < n_segments && !destination_fetch; ++i) {
            if (!segments[i].semi)
                continue;
            if (!blend_copy)
                blend_copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureLocation from = {
                .texture = render_target,
                .x = (Uint32)boxes[i].x0,
                .y = (Uint32)boxes[i].y0};
            SDL_GPUTextureLocation to = from;
            to.texture = destination_sample;
            SDL_CopyGPUTextureToTexture(
                blend_copy, &from, &to,
                (Uint32)(boxes[i].x1 - boxes[i].x0),
                (Uint32)(boxes[i].y1 - boxes[i].y0), 1, false);
        }
        if (blend_copy)
            SDL_EndGPUCopyPass(blend_copy);
        const SDL_GPUColorTargetInfo target = {
            .texture = render_target,
            .load_op = SDL_GPU_LOADOP_LOAD,
            .store_op = SDL_GPU_STOREOP_STORE,
        };
        ++frame_stats_batches;
        SDL_GPURenderPass* pass =
            SDL_BeginGPURenderPass(cmd, &target, 1, NULL);
        const SDL_GPUBufferBinding vbind = {.buffer = vbuf, .offset = b->vertex_offset};
        SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);
        const SDL_GPUBufferBinding ibind = {.buffer = ibuf, .offset = b->index_offset};
        SDL_BindGPUIndexBuffer(pass, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        const SDL_GPUTextureSamplerBinding sampler_binding[4] = {
            {.texture=vram_sample,.sampler=vram_sampler},
            {.texture=destination_sample,.sampler=vram_sampler},
            {.texture=b->extra_texture,.sampler=b->extra_sampler},
            {.texture=clut_cache,.sampler=vram_sampler}};
        SDL_BindGPUFragmentSamplers(pass,0,sampler_binding,PSX_SHADERS_CURRENT ? 4 : 3);
        int pass_mark = -1;
        for (int s = 0; s < n_segments; ++s) {
            const DrawView* view = &b->views[segments[s].mark];
            if (segments[s].mark != pass_mark) {
                pass_mark = segments[s].mark;
                SDL_SetGPUViewport(pass, &view->viewport);
                SDL_SetGPUScissor(pass, &view->scissor);
                SDL_PushGPUVertexUniformData(
                    cmd, 0, view->offset_ubo, sizeof(view->offset_ubo));
            }
            const u32 pass_flags = view->pass_flags;
            const bool semi = segments[s].semi;
            const Uint32 first = (Uint32)segments[s].start;
            const Uint32 count = (Uint32)(segments[s].end - segments[s].start);
            /* Opaque primitives under "check mask" overlap each other in
             * one draw: pass 3 writes colour and mask bit together,
             * blended by the destination's mask bit (alpha), so a pixel
             * masked by an earlier primitive of the batch is kept. */
            const bool checked = PSX_SHADERS_CURRENT && !semi && view->mask_check;
            const u32 color_pass[4] = {
                semi ? 2u : checked ? 3u : 0u, target_scale,
                b->grid_same, pass_flags};
            SDL_PushGPUFragmentUniformData(
                cmd, 0, color_pass, sizeof(color_pass));

            SDL_BindGPUGraphicsPipeline(
                pass, semi && destination_fetch ? pipe_direct_fetch :
                      pipe_tri[semi      ? BLEND_DIRECT
                               : checked ? BLEND_CHECK
                                         : BLEND_ADD]);
            SDL_DrawGPUIndexedPrimitives(pass, count, 1, first, 0, 0);
            /* Blend factors use source alpha as an operation selector.
             * Write the actual PS1 mask bit separately, without changing
             * the color result. */
            if (!semi && !checked) {
                const u32 mask_pass[4] = {
                    1, target_scale, b->grid_same, pass_flags};
                SDL_PushGPUFragmentUniformData(
                    cmd, 0, mask_pass, sizeof(mask_pass));
                SDL_BindGPUGraphicsPipeline(pass, pipe_tri[BLEND_MASK]);
                SDL_DrawGPUIndexedPrimitives(pass, count, 1, first, 0, 0);
            }
        }
        SDL_EndGPURenderPass(pass);
        start = end;
    }
    return true;
}

/* The batches the midpoint keeps until it is drawn (InterpMidpointSync):
 * their vertices (interp_mid_store) and indices (interp_mid_index), and the
 * view (for the midpoint target, as Draw_FlushBuffer computed it then) and
 * end index of each drawing state. */
/* (a frame of forced 16-primitive batches, PSYZ_BATCH_PRIMITIVES=16, is up
 * to about 2300 batches in the attract sweep) */
#define INTERP_MID_BATCHES 8192u
#define INTERP_MID_MARKS 16384u
typedef struct {
    unsigned vertex, vertices, index, indices, mark;
    SDL_GPUTexture* extra_texture;
    SDL_GPUSampler* extra_sampler;
    bool grid_same;
} InterpMidBatch;
static InterpMidBatch interp_mid_batches[INTERP_MID_BATCHES];
static unsigned interp_mid_batches_len, interp_mid_marks_len;
static int interp_mid_mark_end[INTERP_MID_MARKS];
static DrawView interp_mid_views[INTERP_MID_MARKS];
static bool InterpMidpointDeferred(void) { return interp_mid_batches_len != 0; }
static bool InterpMidpointCovers(int x, int y, int w, int h) {
    return interp_mid_batches_len && VramTilesHitRect(&interp_mid_target_tiles, x, y, w, h);
}
static bool InterpMidpointSamplesRect(int x, int y, int w, int h) {
    return interp_mid_batches_len && VramTilesHitRect(&interp_mid_texture_tiles, x, y, w, h);
}
/* The midpoint target draws this frame's batches. */
static bool InterpMidpointDrawn(void) {
    return interpolation_enabled && interpolated_vram &&
           interpolated_scale == internal_res && interp_serial;
}
/* Draw_ResetBuffer: the placements queued with a batch the midpoint did
 * not keep (they follow those of the kept batches). */
static void InterpMidpointDropBatch(void) {
    const unsigned kept = interp_mid_len;
    while (interp_pending_len && interp_pending[interp_pending_len - 1].base >= kept)
        --interp_pending_len;
    while (interp_junction_prims_len && interp_junction_prims[interp_junction_prims_len - 1].base >= kept)
        --interp_junction_prims_len;
    while (interp_line_pending_len && interp_line_pending[interp_line_pending_len - 1].start >= kept)
        --interp_line_pending_len;
}
/* Draw_FlushBuffer, after the frame's targets: the midpoint keeps the
 * batch (n_marks drawing states in draw_marks). */
static void InterpMidpointRecord(int n_marks, SDL_GPUTexture* extra_texture,
                                 SDL_GPUSampler* extra_sampler, bool grid_same) {
    if (interp_mid_batches_len >= INTERP_MID_BATCHES ||
        interp_mid_marks_len + (unsigned)n_marks > INTERP_MID_MARKS)
        InterpMidpointSync(INTERP_SYNC_FULL);
    if (!interp_mid_batches_len)
        interp_mid_clut_loads = clut_cache_loads;
    InterpMidBatch* b = &interp_mid_batches[interp_mid_batches_len++];
    *b = (InterpMidBatch){interp_mid_len, n_vertices, interp_mid_index_len, (unsigned)n_indices,
                          interp_mid_marks_len, extra_texture, extra_sampler, grid_same};
    memcpy(&interp_mid_index[interp_mid_index_len], index_buf, sizeof(*index_buf) * (size_t)n_indices);
    const unsigned target_index = scaled_vram_render ? 2u : 1u;
    for (int k = 0; k < n_marks; ++k) {
        ApplyDrawState(&draw_marks[k].state);
        ComputeDrawView(&interp_mid_views[interp_mid_marks_len + k], target_index, true, internal_res);
        interp_mid_mark_end[interp_mid_marks_len + k] = draw_marks[k].end;
        const SDL_Rect area = draw_marks[k].state.scissor;
        VramTilesAdd(&interp_mid_target_tiles, area.x, area.y, area.w, area.h);
    }
    unsigned last = 0xFFFFFFFFu;
    for (unsigned i = 0; i < n_vertices; ++i) {
        const unsigned t = vertex_buf[i].t;
        if (t == last)
            continue;
        last = t;
        if (!(t & TPAGE_NOTEXTURE))
            VramTilesAddPage(&interp_mid_texture_tiles, t);
    }
    interp_mid_len += n_vertices;
    interp_mid_index_len += (unsigned)n_indices;
    interp_mid_marks_len += (unsigned)n_marks;
    interpolated_vertices = interp_mid_store + interp_mid_len;
    interp_mid_depth = interp_mid_depth_store + interp_mid_len;
}
/* Draws the kept batches into the midpoint target, each as the frame's
 * batch was drawn (through the same vertex and index buffers). */
static bool InterpMidpointDraw(void) {
    if (!interpolation_enabled || !interpolated_vram || interpolated_scale != internal_res)
        return true; /* no target to draw into */
    const unsigned target_index = scaled_vram_render ? 2u : 1u;
    for (unsigned k = 0; k < interp_mid_batches_len; ++k) {
        const InterpMidBatch* b = &interp_mid_batches[k];
        if (!b->indices)
            continue;
        Uint32 vertex_at, index_at;
        const bool cycle = RingReserve(b->vertices, b->indices, &vertex_at, &index_at);
        SDL_GPUCommandBuffer* cmd = AcquireCmd();
        if (!cmd)
            return false;
        u8* map = SDL_MapGPUTransferBuffer(device, vtx_transfer, cycle);
        if (!map) {
            GpuFail("interpolation transfer map", false);
            return false;
        }
        const Uint32 vtx_size = (Uint32)(sizeof(Vertex) * b->vertices);
        const Uint32 idx_size = (Uint32)(sizeof(*interp_mid_index) * b->indices);
        const Uint32 vtx_offset = vertex_at * (Uint32)sizeof(Vertex);
        const Uint32 idx_offset = MAX_VERTEX_COUNT * (Uint32)sizeof(Vertex) +
                                  index_at * (Uint32)sizeof(*interp_mid_index);
        memcpy(map + vtx_offset, &interp_mid_store[b->vertex], vtx_size);
        memcpy(map + idx_offset, &interp_mid_index[b->index], idx_size);
        SDL_UnmapGPUTransferBuffer(device, vtx_transfer);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        if (!copy) {
            GpuFail("interpolation copy pass", false);
            return false;
        }
        const SDL_GPUTransferBufferLocation vtx_src = {.transfer_buffer = vtx_transfer, .offset = vtx_offset};
        const SDL_GPUBufferRegion vtx_dst = {.buffer = vbuf, .offset = vtx_offset, .size = vtx_size};
        const bool buffers_cycle = RingCycleBuffers();
        SDL_UploadToGPUBuffer(copy, &vtx_src, &vtx_dst, buffers_cycle);
        const SDL_GPUTransferBufferLocation idx_src = {.transfer_buffer = vtx_transfer, .offset = idx_offset};
        const SDL_GPUBufferRegion idx_dst = {.buffer = ibuf, .offset = index_at * (Uint32)sizeof(*interp_mid_index), .size = idx_size};
        SDL_UploadToGPUBuffer(copy, &idx_src, &idx_dst, buffers_cycle);
        SDL_EndGPUCopyPass(copy);
        const DrawTargetBatch target = {
            interpolated_vram, target_index, internal_res, true,
            &interp_mid_store[b->vertex], &interp_mid_index[b->index], (int)b->indices,
            vtx_offset, index_at * (Uint32)sizeof(*interp_mid_index),
            &interp_mid_mark_end[b->mark], &interp_mid_views[b->mark],
            b->extra_texture, b->extra_sampler, b->grid_same};
        if (!DrawTargetRuns(&target))
            return false;
    }
    return true;
}
static const char* const interp_sync_names[INTERP_SYNC_N] = {
    "frame", "full", "texture", "clut", "target", "present", "settings"};
/* Places the kept batches' ghosts, new identities, T-junctions and
 * outlines from the frame's neighbours so far (all of them at the end of
 * the frame, INTERP_SYNC_FRAME), then draws them into the midpoint. The
 * batch being queued, if any, stays: its vertices move to the store's
 * start and its placements wait for the next sync. */
static void InterpMidpointSync(unsigned reason) {
    if (!interp_mid_batches_len)
        return;
    const unsigned drawn = interp_mid_len;
    unsigned np = 0, nj = 0, nl = 0;
    while (np < interp_pending_len && interp_pending[np].base < drawn)
        ++np;
    while (nj < interp_junction_prims_len && interp_junction_prims[nj].base < drawn)
        ++nj;
    while (nl < interp_line_pending_len && interp_line_pending[nl].start < drawn)
        ++nl;
    ++draw_frame_stats.syncs[reason];
    if (reason != INTERP_SYNC_FRAME && np + nj + nl) {
        ++draw_frame_stats.split_pending;
        static bool warned[INTERP_SYNC_N];
        if (!warned[reason]) {
            warned[reason] = true;
            WARNF("midpoint of frame %u placed before the frame was complete (%s): %u ghosts and new faces, %u T-junction primitives and %u outlines see only the neighbours queued so far",
                  interp_frame, interp_sync_names[reason], np, nj, nl);
        }
    }
    InterpSnapJunctions(nj);
    InterpFinishPending(np);
    InterpFinishLines(nl, drawn);
    if (!InterpMidpointDraw()) {
        interp_pair_ready = false;
        interp_matches = 0;
    }
    /* what the batch being queued placed: relative to the store's start */
    memmove(interp_pending, interp_pending + np, sizeof(*interp_pending) * (interp_pending_len - np));
    interp_pending_len -= np;
    for (unsigned k = 0; k < interp_pending_len; ++k)
        interp_pending[k].base -= drawn;
    memmove(interp_junction_prims, interp_junction_prims + nj,
            sizeof(*interp_junction_prims) * (interp_junction_prims_len - nj));
    interp_junction_prims_len -= nj;
    for (unsigned k = 0; k < interp_junction_prims_len; ++k)
        interp_junction_prims[k].base -= drawn;
    const unsigned lines = interp_line_pending_len;
    memmove(interp_line_pending, interp_line_pending + nl, sizeof(*interp_line_pending) * (lines - nl));
    interp_line_pending_len -= nl;
    for (unsigned k = 0; k < interp_line_pending_len; ++k)
        interp_line_pending[k].start -= drawn, interp_line_pending[k].end -= drawn;
    /* a line being drawn (InterpLine) has its entry after the others; its
     * quads already drawn (before its start) leave it unplaced */
    if (interp_line_pend >= 0 && (unsigned)interp_line_pend == lines) {
        InterpLinePending* q = &interp_line_pending[lines];
        q->start = q->start >= drawn ? q->start - drawn : 0xFFFFFFFFu;
        interp_line_pending[interp_line_pending_len] = *q;
        interp_line_pend = (int)interp_line_pending_len;
    }
    if (n_vertices) {
        memmove(interp_mid_store, interp_mid_store + drawn, sizeof(Vertex) * n_vertices);
        memmove(interp_mid_depth_store, interp_mid_depth_store + drawn, sizeof(float) * n_vertices);
    }
    interp_mid_len = interp_mid_index_len = 0;
    interp_mid_batches_len = interp_mid_marks_len = 0;
    interpolated_vertices = interp_mid_store;
    interp_mid_depth = interp_mid_depth_store;
    interp_mid_texture_tiles = interp_mid_target_tiles = (VramTiles){{0}};
    interp_mid_clut_loads = clut_cache_loads;
}
/* The midpoint is to be complete: the batch being queued is drawn, then
 * the midpoint. */
static void InterpMidpointComplete(unsigned reason) {
    if (n_vertices && InterpMidpointDrawn())
        DRAW_FLUSH_FOR(DRAW_FLUSH_PRESENT);
    InterpMidpointSync(reason);
}
/* Draw_EnsureBufferWillNotOverflow: the batch is full, or (forced for
 * tests: PSYZ_BATCH_VERTICES=<vertices>, PSYZ_BATCH_PRIMITIVES=<primitives>)
 * smaller. A midpoint store too full for the primitive draws the batch and
 * the midpoint first. */
static bool InterpBatchFull(unsigned vertices, unsigned indices) {
    static int limit_vertices = -1, limit_primitives;
    if (limit_vertices < 0) {
        const char* e = SDL_getenv("PSYZ_BATCH_VERTICES");
        limit_vertices = e ? SDL_max(SDL_atoi(e), 0) : 0;
        e = SDL_getenv("PSYZ_BATCH_PRIMITIVES");
        limit_primitives = e ? SDL_max(SDL_atoi(e), 0) : 0;
    }
    const unsigned most = limit_vertices > 0 && (unsigned)limit_vertices < MAX_VERTEX_COUNT
                              ? (unsigned)limit_vertices : MAX_VERTEX_COUNT;
    if (n_vertices + vertices > most || n_indices + (int)indices > MAX_INDEX_COUNT ||
        (limit_primitives > 0 && batch_primitives >= (unsigned)limit_primitives))
        return true;
    if (!interpolation_enabled ||
        (interp_mid_len + n_vertices + vertices <= INTERP_MID_VERTICES &&
         interp_mid_index_len + (unsigned)n_indices + indices <= INTERP_MID_INDICES))
        return false;
    DRAW_FLUSH_FOR(DRAW_FLUSH_OVERFLOW);
    InterpMidpointSync(INTERP_SYNC_FULL);
    return false;
}
/* A batch is drawn (Draw_FlushBuffer): PSYZ_INTERP_STATS, PSYZ_FRAME_STATS. */
static void DrawCountFlush(unsigned reason) {
    DrawFrameStats* s = &draw_frame_stats;
    s->vertices += n_vertices;
    s->indices += (unsigned)n_indices;
    s->batch_vertices = SDL_max(s->batch_vertices, n_vertices);
    s->batch_indices = SDL_max(s->batch_indices, (unsigned)n_indices);
    ++s->flushes[reason < DRAW_FLUSH_REASONS ? reason : DRAW_FLUSH_STATE];
    frame_stats_vertices += n_vertices;
    ++frame_stats_flushes;
    if (reason == DRAW_FLUSH_OVERFLOW || reason == DRAW_FLUSH_MARKS || reason == DRAW_FLUSH_CLUT)
        ++frame_stats_overflows;
}

void Draw_FlushBuffer(void) {
    const unsigned reason = draw_flush_reason;
    draw_flush_reason = DRAW_FLUSH_STATE;
    if (UseSoftwareBackend()) {
        software_rasterizer.flush_buffer();
        return;
    }
    if (n_vertices == 0) {
        return;
    }
    DrawCountFlush(reason);
    if (!n_draw_marks && (scissor_rect.w <= 0 || scissor_rect.h <= 0)) {
        /* an empty drawing area (UpdateScissor) draws nothing */
        Draw_ResetBuffer();
        return;
    }
    /* the texture copy below changes what the midpoint kept so far samples:
     * it is drawn first */
    if (batch_has_texture && vram_dirty.w > 0 && vram_dirty.h > 0 &&
        InterpMidpointDeferred() &&
        VramTilesHit(&interp_mid_texture_tiles, &vram_dirty_tiles))
        InterpMidpointSync(INTERP_SYNC_TEXTURE);
    /* (a wrap submits the command buffer: reserved before acquiring one) */
    Uint32 vertex_at, index_at;
    const bool cycle = RingReserve((Uint32)n_vertices, (Uint32)n_indices, &vertex_at, &index_at);
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    if (!cmd) {
        Draw_ResetBuffer();
        return;
    }

    u8* map = SDL_MapGPUTransferBuffer(device, vtx_transfer, cycle);
    if (!map) {
        ERRORF("SDL_MapGPUTransferBuffer: %s", SDL_GetError());
        Draw_ResetBuffer();
        return;
    }
    const Uint32 vtx_size = (Uint32)(sizeof(Vertex) * n_vertices);
    const Uint32 idx_size = (Uint32)(sizeof(*index_buf) * n_indices);
    const Uint32 vtx_offset = vertex_at * (Uint32)sizeof(Vertex);
    const Uint32 idx_offset = MAX_VERTEX_COUNT * (Uint32)sizeof(Vertex) +
                              index_at * (Uint32)sizeof(*index_buf);
    memcpy(map + vtx_offset, vertex_buf, vtx_size);
    memcpy(map + idx_offset, index_buf, idx_size);
    SDL_UnmapGPUTransferBuffer(device, vtx_transfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation vtx_src = {
        .transfer_buffer = vtx_transfer, .offset = vtx_offset};
    const SDL_GPUBufferRegion vtx_dst = {.buffer = vbuf, .offset = vtx_offset, .size = vtx_size};
    const bool buffers_cycle = RingCycleBuffers();
    SDL_UploadToGPUBuffer(copy, &vtx_src, &vtx_dst, buffers_cycle);
    const SDL_GPUTransferBufferLocation idx_src = {
        .transfer_buffer = vtx_transfer, .offset = idx_offset};
    const SDL_GPUBufferRegion idx_dst = {
        .buffer = ibuf, .offset = index_at * (Uint32)sizeof(*index_buf), .size = idx_size};
    SDL_UploadToGPUBuffer(copy, &idx_src, &idx_dst, buffers_cycle);
    if (batch_has_texture && vram_dirty.w > 0 && vram_dirty.h > 0) {
        const SDL_GPUTextureLocation vram_src = {.texture = vram_render,
                                                 .x = (Uint32)vram_dirty.x,
                                                 .y = (Uint32)vram_dirty.y};
        const SDL_GPUTextureLocation vram_dst = {.texture = vram_sample,
                                                 .x = (Uint32)vram_dirty.x,
                                                 .y = (Uint32)vram_dirty.y};
        SDL_CopyGPUTextureToTexture(
            copy, &vram_src, &vram_dst, (Uint32)vram_dirty.w,
            (Uint32)vram_dirty.h, 1, false);
        ResetVramDirty();
    }
    SDL_EndGPUCopyPass(copy);

    /* The drawing state of each range of queued primitives (draw_marks):
     * the primitives after the last mark have the current state. */
    const DrawState current_state = CurrentDrawState();
    draw_marks[n_draw_marks].end = n_indices;
    draw_marks[n_draw_marks].state = current_state;
    const int n_marks = n_draw_marks + 1;
    static DrawView views[DRAW_MARKS_MAX + 1];
    static int mark_end[DRAW_MARKS_MAX + 1];
    for (int k = 0; k < n_marks; ++k)
        mark_end[k] = draw_marks[k].end;
    SDL_GPUTexture* extra_texture =
        (sprite_atlas_texture && sprite_atlas_sampler) ? sprite_atlas_texture
                                                         : vram_sample;
    SDL_GPUSampler* extra_sampler =
        (sprite_atlas_texture && sprite_atlas_sampler) ? sprite_atlas_sampler
                                                         : vram_sampler;
    const bool grid_same = draw_grid_source_width == draw_grid_target_width;

    /* Native VRAM is independently rasterized: gameplay readback and texture
     * feedback must not depend on high-resolution sample positions. The
     * midpoint target draws the batch once the frame is complete
     * (InterpMidpointRecord, InterpMidpointSync). */
    for (unsigned target_index = 0;
         target_index < (scaled_vram_render ? 2u : 1u); ++target_index) {
        SDL_GPUTexture* render_target =
            target_index ? scaled_vram_render : vram_render;
        unsigned target_scale = target_index ? internal_res : 1;
        for (int k = 0; k < n_marks; ++k) {
            ApplyDrawState(&draw_marks[k].state);
            ComputeDrawView(&views[k], target_index, false, target_scale);
        }
        const DrawTargetBatch target = {
            render_target, target_index, target_scale, false,
            vertex_buf, index_buf, n_indices,
            vtx_offset, index_at * (Uint32)sizeof(*index_buf), mark_end, views,
            extra_texture, extra_sampler, grid_same};
        if (!DrawTargetRuns(&target))
            goto abort_flush;
    }
    if (InterpMidpointDrawn())
        InterpMidpointRecord(n_marks, extra_texture, extra_sampler, grid_same);
    ApplyDrawState(&current_state);
    for (int k = 0; k < n_marks; ++k) {
        const SDL_Rect area = draw_marks[k].state.scissor;
        if (area.w > 0 && area.h > 0)
            MarkVramDirty(area);
    }
    Draw_ResetBuffer();
    return;
abort_flush:
    ApplyDrawState(&current_state);
    Draw_ResetBuffer();
}








/* ===== Native rasterizer mode =====
 * At 1x without GPU-only enhancements, primitives are drawn into native VRAM
 * by the CPU rasterizer (sdl3_software.c), which follows the PS1 GPU's
 * rasterization bit for bit. The GPU device keeps presenting: each frame the
 * CPU VRAM is uploaded to vram_render and goes through the usual display
 * pipeline. Higher internal resolutions and the other enhancements use the
 * GPU rasterizer; switching hands over the VRAM and the drawing state at a
 * frame boundary. */
static bool NativeRasterizerWanted(void) {
    return native_rasterizer_requested && !headless_renderer && device &&
           vram_render && is_platform_init_successful && set_internal_res == 1 &&
           !set_antialiasing && !set_geometry_correction &&
           !set_effect_geometry_correction && !set_model_filtering &&
           !set_sprite_filtering && !widescreen && !interpolation_enabled &&
           dither_mode != PSYZ_DITHER_OFF &&
           !(sprite_atlases_enabled && sprite_atlas_count);
}

static void UpdateNativeRasterizer(void) {
    bool want = NativeRasterizerWanted();
    if (want == native_rasterizer)
        return;
    size_t pixels = (size_t)VRAM_W * VRAM_H;
    u16* vram = malloc(pixels * sizeof(u16));
    u8* rgba = want ? malloc(pixels * 4) : NULL;
    if (!vram || (want && !rgba)) {
        free(vram);
        free(rgba);
        return;
    }
    PS1_RECT all = {0, 0, VRAM_W, VRAM_H};
    if (want) {
        Draw_FlushBuffer();
        if (!DownloadVramRegionAsRGBA8888(0, 0, VRAM_W, VRAM_H, rgba)) {
            free(vram);
            free(rgba);
            return;
        }
        ConvertRgba8888ToRgb5551(rgba, vram, pixels);
        native_rasterizer = true;
        SoftwareRasterizer_SetVram(vram);
    } else {
        memcpy(vram, SoftwareRasterizer_GetVram(), pixels * sizeof(u16));
        native_rasterizer = false;
        UploadVram(&all, vram);
        /* the CPU rasterizer's CLUT cache is not carried over */
        clut_cache_reg = 0xFFFFFFFFu;
        clut_cache_8bit = false;
    }
    free(vram);
    free(rgba);
    Psyz_GpuResendDrawState();
}

static void PresentNativeVram(void) {
    PS1_RECT all = {0, 0, VRAM_W, VRAM_H};
    UploadVram(&all, SoftwareRasterizer_GetVram());
}

int Psyz_VideoSetNativeRasterizer(int enabled) {
    native_rasterizer_requested = enabled != 0;
    return 0;
}

int Psyz_VideoGetNativeRasterizer(void) {
    return native_rasterizer_requested ? 1 : 0;
}

int Psyz_VideoNativeRasterizerActive(void) {
    return (native_rasterizer || headless_renderer) ? 1 : 0;
}

