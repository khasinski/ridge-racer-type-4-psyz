/* Frame interpolation of the SDL GPU renderer (included by sdl3_gpu.c).
 *
 * A game frame that takes two fields (25 or 30 fps) gets a midpoint drawn
 * between it and the frame before, presented at the blank in between
 * (Psyz_GpuPrepareInterpolatedBlank). The midpoint is drawn from the same
 * packets as the frame, into a separate target (interpolated_vram) that
 * starts as a copy of the frame before, with every paired vertex moved
 * halfway:
 *
 *   identity  a packet pairs with the packet of the frame before that has
 *             the same identity (Psyz_GpuTagInterpolationPacket, flags
 *             PSYZ_INTERP_* in psyz/gpu.h) or the same place in the double
 *             buffers (Psyz_GpuRegisterInterpolationPair). Untagged packets
 *             (2D, HUD) are drawn into the midpoint as they are.
 *   capture   InterpCaptureDraw stores each tagged primitive's pose and
 *             writes its midpoint vertices next to the frame's own
 *             (interp_mid_store): the projection of the midpoint of the two
 *             view-space positions, from the screen positions and their
 *             PGXP depths.
 *   ghosts    an identity the frame before drew and this frame leaves out
 *             is drawn into the midpoint only (InterpBuildGhosts,
 *             InterpInjectGhosts), after the primitive drawn before it.
 *   pending   geometry with one pose only (ghosts, new faces, tiles passing
 *             under the camera) moves as the paired vertices next to it
 *             moved (motion fields, InterpFinishPending).
 *   junctions a vertex on an edge of a coarser neighbour in both frames (a
 *             T-junction) is put on that edge's midpoint
 *             (InterpSnapJunctions); outlines go where the polygon vertices
 *             they lie at went (InterpFinishLines).
 *   sync      the midpoint keeps its geometry for the whole frame and is
 *             placed and drawn once the frame is complete
 *             (InterpMidpointSync), or earlier when something would change
 *             what it samples or draws over (a split, counted by
 *             PSYZ_INTERP_STATS).
 *   hold      a midpoint that is no plausible in-between
 *             (InterpMidpointUnsound) is not shown; the frame before stays
 *             for that field.
 *
 * Tuned thresholds (screen distances are in native PS1 pixels, at any
 * internal resolution):
 *
 *   INTERP_PAIR_REACH          32 px: an untagged (address) pair whose
 *                              vertex moved farther is no pair
 *   INTERP_PAIR_REACH_TAGGED   1024 px: the same for a tagged identity (the
 *                              same face, so up to the view's size)
 *   INTERP_LINE_PAIR_REACH     160 px: the same for a line's points
 *   INTERP_GHOST_MOTION        384 px a frame: the largest move a ghost
 *                              continues
 *   (cut)                      1/2: a frame whose ghosts would leave out
 *                              more than this share of the identities the
 *                              frame before drew is a cut (held)
 *   INTERP_HOLD_SHARE          0.15: held when geometry without a pose in
 *                              the frame before covers more than this share
 *                              of the picture (PSYZ_INTERP_HOLD), or more
 *                              than the posed geometry
 *   INTERP_EXPOSE_LIMIT        8 cells of 4x4 px: held when this many cells
 *                              (each with its four neighbours) show
 *                              something farther in the midpoint than both
 *                              frames show there (PSYZ_INTERP_EXPOSE)
 *   INTERP_EXPOSE_DEPTH        1.5: "farther" for that test, as a ratio of
 *                              the nearer of the two frames' depths
 *   INTERP_FIELD_REACH         24 px: how far a one-pose vertex looks for a
 *                              paired neighbour to move with
 *   INTERP_FIELD_DEPTH         1/4: a neighbour counts when its depth is
 *                              within this share of the vertex's
 *   INTERP_JUNCTION_*          the T-junction test: edges are found in
 *                              reach of 2 px; a vertex lies on one within
 *                              0.25 px (precise projection) or 0.75 px
 *                              (whole pixels), plus 0.004 of the edge's
 *                              length when only the frame before has a
 *                              pose to test, at the edge's depth within a
 *                              tenth, and moves onto it by at most 6 px
 *   INTERP_LINE_REACH          2 px: an outline point lies at a polygon
 *                              vertex
 *
 * Diagnostics and switches (read once, at the first use):
 *
 *   PSYZ_INTERP_STATS=1        one [interp-stats] line a frame: areas by
 *                              what the midpoint did, batches, midpoint
 *                              draws and splits, table overflows
 *   PSYZ_INTERP_HOLD=<share>   INTERP_HOLD_SHARE
 *   PSYZ_INTERP_EXPOSE=<cells> INTERP_EXPOSE_LIMIT (0: no exposure test)
 *   PSYZ_INTERP_EXPOSE_DUMP=<frame>  log the exposed cells of that frame
 *   PSYZ_INTERP_PROBE=x,y[,first,last]  log every primitive of the frames
 *                              first..last that covers the pixel x,y
 *                              (x=-1: all primitives)
 *   PSYZ_INTERP_DEBUG=1|2      tint the midpoint's unpaired tagged
 *                              primitives magenta (2: untagged ones green)
 *   PSYZ_INTERP_GHOSTS=0       no ghosts
 *   PSYZ_INTERP_FIELD=0        one-pose geometry keeps its own pose
 *   PSYZ_INTERP_JUNCTIONS=0    no T-junction snapping
 *   PSYZ_INTERP_SIZE_CULLED=0  triangles the GPU skips for their size this
 *                              frame are not drawn into the midpoint
 *   PSYZ_BATCH_VERTICES=<n>, PSYZ_BATCH_PRIMITIVES=<n>  draw batches of at
 *                              most n vertices or primitives (tests: the
 *                              midpoint must not depend on the batches)
 */

/* Pairing, holding and exposure thresholds (see above). */
#define INTERP_PAIR_REACH 32.0f
#define INTERP_PAIR_REACH_TAGGED 1024.0f
#define INTERP_LINE_PAIR_REACH 160.0f
#define INTERP_HOLD_SHARE 0.15f
#define INTERP_EXPOSE_LIMIT 8
#define INTERP_EXPOSE_DEPTH 1.5f
#define INTERP_FIELD_DEPTH 0.25f

static bool InterpEnsureTarget(void);
/* Presentation-only geometry history. Packet pairs belong to the original
 * double buffers; UI packets are deliberately never registered. */
#define INTERP_PAIRS 16384u
#define INTERP_MAP 65536u

typedef struct {
    const void* address;
    unsigned pair, serial, frame, flags;
} InterpAddress;

typedef struct {
    const void *a, *b;
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

static InterpAddress interp_addresses[INTERP_MAP], interp_queue[INTERP_MAP];
/* Packets tagged with a stable identity (Psyz_GpuTagInterpolationPacket):
 * the packet address -> its identity's pair, for the frame it is drawn in. */
static InterpAddress interp_tags[INTERP_MAP];

typedef struct {
    unsigned long long id;
    unsigned pair, serial;
} InterpId;
/* Slots an identity is looked for in (linear probing). The game's
 * identities are mixed from host addresses of its model data, so their
 * slots move with ASLR from run to run: the table must not drop any (at
 * most INTERP_PAIRS of its INTERP_MAP slots are live), or the midpoint
 * would differ between runs of the same input. */
#define INTERP_ID_PROBES 64u

static InterpId interp_ids[INTERP_MAP];
/* The midpoint waits for a blank without a new frame (a frame that takes
 * more than one field): Psyz_GpuPrepareInterpolatedBlank. */
static bool interp_blank_mid;
static InterpPair interp_pairs[INTERP_PAIRS];
static unsigned interp_serial, interp_frame, interp_phase, interp_matches;
static unsigned interp_camera_frame;
static int interp_camera_x, interp_camera_y;
static float interp_camera_half_x, interp_camera_half_y;
static bool interp_camera_previous;
static int interp_next_y = -1;
static bool interpolation_requested;
static bool interpolation_enabled, interp_present_mid, interp_pair_ready;
static SDL_GPUTexture* interpolated_vram;
/* Failed submission/wait is not a completion witness. Retain until idle. */
static SDL_GPUTexture* interp_unwitnessed;
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
static Vertex* interpolated_vertices = interp_mid_store;
static float* interp_mid_depth = interp_mid_depth_store;
static void InterpMidpointSync(unsigned reason);
static void InterpMidpointComplete(unsigned reason);
static bool InterpMidpointDeferred(void);

/* Why the midpoint is drawn: the frame is complete, or (a split) its
 * store is full, a VRAM change reaches what it samples or draws over, a
 * present or capture shows it, or its target or settings change. */
enum {
    INTERP_SYNC_FRAME,
    INTERP_SYNC_FULL,
    INTERP_SYNC_TEXTURE,
    INTERP_SYNC_CLUT,
    INTERP_SYNC_TARGET,
    INTERP_SYNC_PRESENT,
    INTERP_SYNC_SETTINGS,
    INTERP_SYNC_N
};

/* Entries the fixed interpolation tables drop or overwrite while live
 * (PSYZ_INTERP_STATS; a warning the first time for each). */
enum {
    IEVICT_ADDRESSES,
    IEVICT_TAGS,
    IEVICT_QUEUE,
    IEVICT_IDS,
    IEVICT_PAIRS,
    IEVICT_STORE,
    IEVICT_ORDER,
    IEVICT_FIELD,
    IEVICT_EDGES,
    IEVICT_EDGE_REFS,
    IEVICT_PENDING,
    IEVICT_JUNCTIONS,
    IEVICT_LINES,
    IEVICT_N
};

static unsigned interp_evict[IEVICT_N];
static void InterpEvict(unsigned table);
/* CLUT cache rows loaded since the first batch the midpoint keeps */
static unsigned interp_mid_clut_loads;
static bool InterpMidpointCovers(int x, int y, int w, int h);
static bool InterpMidpointSamplesRect(int x, int y, int w, int h);
static unsigned long long interpolated_present_count;

/* The screen area (pixels) of the frame's tagged primitives by what the
 * midpoint does with them (InterpMidpointUnsound); PSYZ_INTERP_STATS=1 logs
 * them per frame, PSYZ_INTERP_PROBE (InterpProbe) the primitives at a pixel. */
enum {
    ISTAT_PAIRED,
    ISTAT_NEW,
    ISTAT_MISMATCH,
    ISTAT_FRAME_ONLY,
    ISTAT_MID_ONLY,
    ISTAT_VANISHED,
    ISTAT_GHOST,
    ISTAT_REPLACED,
    ISTAT_N
};

static double interp_area[ISTAT_N];
static unsigned interp_count[ISTAT_N];
static int interp_stats_log = -1;
static float InterpArea(float (*pos)[3], unsigned count);
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
static unsigned interp_order[2][INTERP_ORDER], interp_order_len[2];
static unsigned interp_inject, interp_ghost_first;
static int interp_ghosts = -1;
/* the frame's ghosts leave out more than this share of its identities: a cut */
static bool interp_ghost_cut;
#define INTERP_COVER_CELL 8
static unsigned short
    interp_ghost_cover[VRAM_H / INTERP_COVER_CELL][VRAM_W / INTERP_COVER_CELL];
static const u32* interp_capture_index;
static unsigned interp_capture_indices;
static bool InterpGhostCovers(float (*pos)[3], unsigned count);
static bool InterpGhostCoversLine(float (*pos)[3], int count);
static void InterpStore(
    InterpPair* p, unsigned pair, const Vertex* v, unsigned count);
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

typedef struct {
    float x, y, depth, dx, dy;
    short ox, oy;
    unsigned next;
} InterpFieldEntry;

typedef struct {
    InterpFieldEntry entry[INTERP_FIELD];
    unsigned len, frame, head[INTERP_FIELD_CELLS], stamp[INTERP_FIELD_CELLS];
} InterpField;

static InterpField interp_field[2];

/* Where a primitive drawn now lands in the frame's buffer: the drawing
 * offset, the widescreen squeeze and the drawing area. */
typedef struct {
    float ox, oy, squeeze, centre;
    int clip[4];
} InterpExposeView;

static void InterpExposeViewNow(InterpExposeView* view);
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

typedef struct {
    float a0[2], b0[2], a1[2], b1[2], am[2], bm[2], da, db;
    unsigned pair;
    short ox, oy;
    bool posed;
} InterpEdge;

typedef struct {
    unsigned edge, next;
} InterpEdgeRef;

static InterpEdge interp_edges[INTERP_EDGES];
static InterpEdgeRef interp_edge_refs[INTERP_EDGE_REFS];
static unsigned interp_edges_len, interp_edge_refs_len, interp_edges_frame;
static unsigned interp_edge_head[INTERP_EDGE_CELLS],
    interp_edge_stamp[INTERP_EDGE_CELLS];
/* the paired primitives queued since the last flush */
#define INTERP_JUNCTION_PRIMS 16384u

typedef struct {
    unsigned pair, base, field[2][6];
    unsigned char count;
    short ox, oy;
    bool posed;
    float p0[6][2], p1[6][2], d0[6];
} InterpJunctionPrim;

static InterpJunctionPrim interp_junction_prims[INTERP_JUNCTION_PRIMS];
static unsigned interp_junction_prims_len;
static void InterpSnapJunctions(unsigned n);
static void InterpAddEdge(
    const float* a0, const float* b0, const float* a1, const float* b1,
    const float* am, const float* bm, float da, float db, unsigned pair, int ox,
    int oy, bool posed);
/* Exposure: what the midpoint shows where both frames show something
 * nearer. Each frame's opaque primitives and the midpoint's are rasterized
 * at cell centres into depth grids (the nearest view depth, 0 for 2D),
 * relative to the frame's buffer (Psyz_GpuInterpolationBackdrop). A cell
 * both frames cover that the midpoint leaves uncovered, or covers with
 * something much farther, shows what neither frame shows (a hole, the
 * background through a gap): such a midpoint is not shown (see
 * InterpMidpointUnsound). PSYZ_INTERP_EXPOSE=<cells> sets the limit. */
#define INTERP_EXPOSE_CELL 4
#define INTERP_EXPOSE_W (VRAM_W / INTERP_EXPOSE_CELL)
#define INTERP_EXPOSE_H (VRAM_H / INTERP_EXPOSE_CELL)

typedef struct {
    float depth;
    unsigned frame;
} InterpExposeCell;

/* [0], [1]: the frames by parity; [2]: the midpoint */
static InterpExposeCell interp_expose[3][INTERP_EXPOSE_H][INTERP_EXPOSE_W];

static struct {
    int x, y, w, h;
    unsigned frame;
} interp_expose_rect[2];

static void InterpExposeTriangle(
    unsigned grid, const float* a, const float* b, const float* c, float da,
    float db, float dc, const int clip[4]);
static void InterpExposePrimitive(
    unsigned grid, const InterpExposeView* view, const Vertex* v,
    const float* depth, const u32* index, unsigned indices, unsigned base,
    bool precise);
static unsigned InterpExposed(void);
static unsigned interp_exposed_holes;
/* Until the first pair the midpoint target is not drawn: it is refreshed
 * from the render target at the next interpolation frame. */
static bool interp_target_stale;
/* Primitives of the frames drawn: all, tagged with an identity, paired. */
static unsigned interp_stat[3], interp_stat_last[3];
static const char* const interp_evict_names[IEVICT_N] = {
    "addresses", "tags",  "queue",     "ids",     "pairs",     "store", "order",
    "field",     "edges", "edge-refs", "pending", "junctions", "lines"};

/* Psyz_GpuInterpolationBackdrop of the frame being drawn: a background
 * fill of its rows (the game's DrawEnv clear) is followed by the copy. */
static struct {
    int x, src_y, dst_y, w, h;
    unsigned frame;
} interp_backdrop;

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
    float from[INTERP_LINE_POINTS][2], want[INTERP_LINE_POINTS][2],
        at[INTERP_LINE_POINTS][2];
} InterpLinePending;

static InterpLinePending interp_line_pending[INTERP_LINE_PENDING];
static unsigned interp_line_pending_len;
/* the entry of the line being drawn, or -1 */
static int interp_line_pend = -1;
static void InterpProbe(const void* packet, const Vertex* v, const Vertex* mid,
                        unsigned count, const char* what);
static void InterpCaptureDraw(const void* packet, Vertex* v, unsigned count);
static const char* interp_probe_what;
static unsigned interp_probe_pair, interp_probe_flags;
static const unsigned interp_tri[3][2][3] = {
    {{0, 1, 2}, {0, 1, 2}}, {{0, 1, 2}, {2, 1, 3}}, {{0, 1, 2}, {3, 4, 5}}};
// mirror a native VRAM region into the scaled render target
static void InterpBackdropAfterFill(int x, int y, int w, int h);
static void InterpMidpointDropBatch(void);
static unsigned InterpFieldCell(int cx, int cy, int ox, int oy);
static unsigned InterpFieldAdd(unsigned which, const float* from, float depth,
                               int ox, int oy, const float* to);
static bool InterpFieldMotion(unsigned which, const float* at, float depth,
                              int ox, int oy, float motion[2], float* distance);
static bool InterpPend(
    unsigned which, unsigned pair, unsigned base, unsigned count,
    const float (*from)[3], const float* depth, const unsigned char* index,
    unsigned indices, int ox, int oy);
static unsigned InterpHash(const void* p);
static InterpAddress* InterpFind(InterpAddress* map, const void* p);
static void InterpMap(
    InterpAddress* map, const void* p, unsigned pair, unsigned serial);
static InterpAddress* InterpFindTagged(const void* packet);
static bool InterpEnsureTarget(void);
static void InterpEvict(unsigned table);
static void InterpStatsBatches(char* out, size_t size);
static void InterpStatsFrame(void);
static void InterpBackdropCopy(int x, int y, int w, int h);
static void InterpBackdropAfterFill(int x, int y, int w, int h);
static bool InterpMidpointUnsound(void);
static bool InterpLine(const u_long* words, int count, bool gouraud, int* mid_x,
                       int* mid_y, bool* own, bool* own_only);
static void InterpCapture(const void* packet, Vertex* v, unsigned count,
                          const u32* index, unsigned indices);
static bool InterpUnclamped(const float* pos);
static void InterpCaptureDraw(const void* packet, Vertex* v, unsigned count);
static int InterpOffsetX(void);
static int InterpOffsetY(void);
static float InterpArea(float (*pos)[3], unsigned count);
static bool InterpGhostEnabled(void);
static unsigned short InterpCoverStamp(void);
static void InterpCoverTriangle(
    const float a[2], const float b[2], const float c[2]);
static bool InterpGhostCovers(float (*pos)[3], unsigned count);
static bool InterpGhostCoversLine(float (*pos)[3], int count);
static void InterpStore(
    InterpPair* p, unsigned pair, const Vertex* v, unsigned count);
static void InterpGhostPose(
    const InterpPair* g, unsigned i, float step, float out[2]);
static void InterpBuildGhosts(void);
static bool InterpProbeCovers(
    const Vertex* v, unsigned count, bool precise, float px, float py);
static void InterpProbe(const void* packet, const Vertex* v, const Vertex* mid,
                        unsigned count, const char* what);
static void InterpExposeTriangle(
    unsigned grid, const float* a, const float* b, const float* c, float da,
    float db, float dc, const int clip[4]);
static void InterpExposeViewNow(InterpExposeView* view);
static void InterpExposePrimitive(
    unsigned grid, const InterpExposeView* view, const Vertex* v,
    const float* depth, const u32* index, unsigned indices, unsigned base,
    bool precise);
static unsigned InterpExposed(void);
static void InterpInjectGhosts(unsigned h);
static unsigned InterpEdgeCell(int cx, int cy, int ox, int oy);
static void InterpAddEdge(
    const float* a0, const float* b0, const float* a1, const float* b1,
    const float* am, const float* bm, float da, float db, unsigned pair, int ox,
    int oy, bool posed);
static float InterpAlong(
    const float* p, const float* a, const float* b, float* distance);
static void InterpSnapJunctions(unsigned n);
static void InterpFinishPending(unsigned pending);
static void InterpEnqueued(void);
static void InterpFinishLines(unsigned n, unsigned vertices);
static bool InterpMidpointDeferred(void);
static bool InterpMidpointCovers(int x, int y, int w, int h);
static bool InterpMidpointSamplesRect(int x, int y, int w, int h);
static bool InterpMidpointDrawn(void);
static void InterpMidpointDropBatch(void);
static void InterpMidpointRecord(int n_marks, SDL_GPUTexture* extra_texture,
                                 SDL_GPUSampler* extra_sampler, bool grid_same);
static bool InterpMidpointDraw(void);
static void InterpMidpointSync(unsigned reason);
static void InterpMidpointComplete(unsigned reason);
static bool InterpBatchFull(unsigned vertices, unsigned indices);
