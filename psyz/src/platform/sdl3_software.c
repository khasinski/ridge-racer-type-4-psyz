#include <psyz.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "../internal.h"
#include "rasterizer.h"
/* RGB555 to RGB888 as every presented and captured frame: the PS1 video DAC
 * spans the full range, so channel c shows as round(c * 255 / 31) (the
 * GPU's UNORM expansion and color_5to8 in sdl3_common.h). */
static unsigned char Expand5(unsigned c) { return (unsigned char)((c * 255 + 15) / 31); }

typedef struct {
    int x, y;
} Point;

typedef struct {
    int x, y;
    int u, v;
    int r, g, b;
} SwVertex;

#define SEMITRANSP 0x02
#define TEXTURED 0x04
#define EXTRA_VERTEX 0x08
#define GOURAUD 0x10
#define TRIANGLE 0x20

static u16 vram[VRAM_W * VRAM_H];
static bool dither_enabled;

static int SoftwareDither(void) {
    return dither_enabled && Psyz_VideoGetDitheringMode() != PSYZ_DITHER_OFF;
}
static Point draw_offset;
static Point draw_area_start;
static Point draw_area_end = {VRAM_W - 1, VRAM_H - 1};
static u16 cur_tpage;
static u8 twin_and_x = 0xff;
static u8 twin_and_y = 0xff;
static u8 twin_or_x;
static u8 twin_or_y;
static bool set_mask_bit;
static bool preserve_masked;
static unsigned grid_source = 1;
static unsigned grid_target = 1;

static int Sign11(int value) {
    value &= 0x7ff;
    return (value ^ 0x400) - 0x400;
}

static int Min3(int a, int b, int c) {
    int m = a < b ? a : b;
    return m < c ? m : c;
}

static int Max3(int a, int b, int c) {
    int m = a > b ? a : b;
    return m > c ? m : c;
}

static int Clamp5(int value) { return CLAMP(value, 0, 31); }

static u16 PackColor(int r, int g, int b, bool mask) {
    return (u16)(Clamp5(r) | (Clamp5(g) << 5) | (Clamp5(b) << 10) |
                 (mask ? 0x8000 : 0));
}

/* Shared native RGB555 destination semantics; caller owns addressing. */
static bool NativeDestinationWrite(u16 *out,u16 src,bool semi,int abr,
    bool set_mask_bit,bool preserve_masked) {
    if (preserve_masked && (*out & 0x8000)) {
        return false;
    }
    if (semi) {
        u16 dst = *out;
        int sr = src & 31;
        int sg = (src >> 5) & 31;
        int sb = (src >> 10) & 31;
        int dr = dst & 31;
        int dg = (dst >> 5) & 31;
        int db = (dst >> 10) & 31;
        switch (abr & 3) {
        case 0:
            sr = (sr + dr) >> 1;
            sg = (sg + dg) >> 1;
            sb = (sb + db) >> 1;
            break;
        case 1:
            sr += dr;
            sg += dg;
            sb += db;
            break;
        case 2:
            sr = dr - sr;
            sg = dg - sg;
            sb = db - sb;
            break;
        case 3:
            sr = dr + sr / 4;
            sg = dg + sg / 4;
            sb = db + sb / 4;
            break;
        }
        src = PackColor(sr, sg, sb, (src & 0x8000) != 0);
    }
    *out =
        (u16)((src & 0x7fff) | ((src | (set_mask_bit ? 0x8000 : 0)) & 0x8000));
    return true;
}

static bool WritePixel(int x, int y, u16 src, bool semi, int abr) {
    if (x < draw_area_start.x || x > draw_area_end.x || y < draw_area_start.y ||
        y > draw_area_end.y || x < 0 || x >= VRAM_W || y < 0 || y >= VRAM_H) {
        return false;
    }
    u16* out = &vram[y * VRAM_W + x];
    return NativeDestinationWrite(out, src, semi, abr, set_mask_bit,
                                  preserve_masked);
}

/* The GPU's CLUT cache (Draw_ClearCache): the palette of the textured
 * primitive that last loaded it. */
static u16 clut_cache[256];
static u32 clut_cache_reg = 0xFFFFFFFFu;
static bool clut_cache_8bit;

static void ClutCacheUpdate(u16 tpage, u16 clut) {
    const int depth = (tpage >> 7) & 3;
    if (depth >= 2 || (clut == clut_cache_reg && (depth == 0 || clut_cache_8bit)))
        return;
    clut_cache_reg = clut;
    clut_cache_8bit = depth == 1;
    const int cx = (clut & 0x3f) * 16, cy = (clut >> 6) & 511;
    for (int i = 0; i < (depth ? 256 : 16); i++)
        clut_cache[i] = vram[cy * VRAM_W + ((cx + i) & 1023)];
}

static void Software_ClearCache(void) {
    clut_cache_reg = 0xFFFFFFFFu;
    clut_cache_8bit = false;
}

static u16 SampleTexture(int u, int v, u16 tpage, u16 clut) {
    (void)clut;
    u = ((u & twin_and_x) | twin_or_x) & 0xff;
    v = ((v & twin_and_y) | twin_or_y) & 0xff;
    int tx = (tpage & 0xf) * 64;
    int ty = ((tpage >> 4) & 1) * 256 + v;
    int depth = (tpage >> 7) & 3;
    if (depth == 0) {
        u16 packed = vram[ty * VRAM_W + ((tx + u / 4) & 1023)];
        return clut_cache[(packed >> ((u & 3) * 4)) & 15];
    }
    if (depth == 1) {
        u16 packed = vram[ty * VRAM_W + ((tx + u / 2) & 1023)];
        return clut_cache[(packed >> ((u & 1) * 8)) & 255];
    }
    return vram[ty * VRAM_W + ((tx + u) & 1023)];
}

static const int dither[4][4] = {
    {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static u16 ShadeTexel(
    u16 texel, int r, int g, int b, bool raw, int x, int y, bool do_dither) {
    int tr = texel & 31;
    int tg = (texel >> 5) & 31;
    int tb = (texel >> 10) & 31;
    if (raw) {
        return texel;
    }
    if (do_dither) {
        int d = dither[y & 3][x & 3];
        tr = Clamp5((CLAMP((tr * r) >> 4, 0, 255) + d) >> 3);
        tg = Clamp5((CLAMP((tg * g) >> 4, 0, 255) + d) >> 3);
        tb = Clamp5((CLAMP((tb * b) >> 4, 0, 255) + d) >> 3);
    } else {
        tr = CLAMP((tr * r) >> 7, 0, 31);
        tg = CLAMP((tg * g) >> 7, 0, 31);
        tb = CLAMP((tb * b) >> 7, 0, 31);
    }
    return PackColor(tr, tg, tb, (texel & 0x8000) != 0);
}


/* ===== Native PS1 polygon and line rasterization =====
 *
 * The GPU scans a triangle top to bottom in two parts split at the middle
 * vertex. Both edges are 32.32 fixed-point X positions stepped once per
 * line from a start just below the vertex's integer X, and a line covers the
 * pixels [left, right) of the two truncated positions. This gives the
 * hardware's fill convention: the top and left edges are inside, the bottom
 * and right edges outside, and pixel centres are not sampled. Colours and
 * texture coordinates step by per-pixel and per-line gradients (12 integer +
 * 12 fractional bits, kept in 32-bit wrapping registers) from a half-unit
 * bias at the leftmost vertex. Lines step a 32.32 position along the major
 * axis. The arithmetic follows the Mednafen/DuckStation software GPU, which
 * reproduces hardware test results; RGB555 output, dithering, blending and
 * the mask bit are shared with sprites through ShadeTexel/WritePixel. */

typedef struct {
    bool textured, raw, semi, gouraud, dither;
    u16 tpage, clut;
    int abr;
    int r, g, b;           /* flat colour */
    u32 dx[5], dy[5];      /* R, G, B, U, V gradients */
} NativePoly;

static void NativeStep(u32* attr, const u32* step, int count) {
    for (int i = 0; i < 5; i++)
        attr[i] += step[i] * (u32)count;
}

static void NativePixel(const NativePoly* p, int x, int y, const u32* attr) {
    int r = p->gouraud ? (int)(attr[0] >> 24) : p->r;
    int g = p->gouraud ? (int)(attr[1] >> 24) : p->g;
    int b = p->gouraud ? (int)(attr[2] >> 24) : p->b;
    u16 color;
    bool blend = p->semi;
    if (p->textured) {
        u16 texel = SampleTexture((int)(attr[3] >> 24), (int)(attr[4] >> 24),
                                  p->tpage, p->clut);
        if (!texel)
            return;
        blend = p->semi && (texel & 0x8000);
        color = ShadeTexel(texel, r, g, b, p->raw, x, y, p->dither);
    } else {
        int d = p->dither ? dither[y & 3][x & 3] : 0;
        color = PackColor((r + d) >> 3, (g + d) >> 3, (b + d) >> 3, false);
    }
    WritePixel(x, y, color, blend, p->abr);
}

static void NativeSpan(const NativePoly* p, int y, int x_start, int x_bound,
                       const u32* line_attr) {
    int width = x_bound - x_start;
    int x = Sign11(x_start);
    if (x < draw_area_start.x) {
        int delta = draw_area_start.x - x;
        x_start += delta;
        x += delta;
        width -= delta;
    }
    if (x + width > draw_area_end.x + 1)
        width = draw_area_end.x + 1 - x;
    if (width <= 0)
        return;
    u32 attr[5];
    memcpy(attr, line_attr, sizeof(attr));
    NativeStep(attr, p->dx, x_start);
    do {
        NativePixel(p, x, y, attr);
        x++;
        NativeStep(attr, p->dx, 1);
    } while (--width > 0);
}

typedef struct {
    uint64_t x[2], step[2]; /* left and right edge, 32.32 */
    int y, end_y;
    bool upwards;      /* drawn from y - 1 up to end_y */
} NativePart;

static int NativeEdgeX(uint64_t fp) { return (int)(s32)(u32)(fp >> 32); }

static void NativeDrawPart(const NativePoly* p, const NativePart* part,
                           const u32* origin) {
    uint64_t left = part->x[0], right = part->x[1];
    int y = part->y;
    u32 attr[5];
    memcpy(attr, origin, sizeof(attr));
    if (part->upwards) {
        if (y <= part->end_y)
            return;
        NativeStep(attr, p->dy, y);
        do {
            y--;
            left -= part->step[0];
            right -= part->step[1];
            int line = Sign11(y);
            if (line < draw_area_start.y)
                break;
            NativeStep(attr, p->dy, -1);
            if (line > draw_area_end.y)
                continue;
            NativeSpan(p, line & (VRAM_H - 1), NativeEdgeX(left),
                       NativeEdgeX(right), attr);
        } while (y > part->end_y);
    } else {
        if (y >= part->end_y)
            return;
        NativeStep(attr, p->dy, y);
        do {
            int line = Sign11(y);
            if (line > draw_area_end.y)
                break;
            if (line >= draw_area_start.y)
                NativeSpan(p, line & (VRAM_H - 1), NativeEdgeX(left),
                           NativeEdgeX(right), attr);
            y++;
            left += part->step[0];
            right += part->step[1];
            NativeStep(attr, p->dy, 1);
        } while (y < part->end_y);
    }
}

static int64_t NativeEdgeStart(int x) {
    return (int64_t)x * 4294967296LL + (4294967296LL - 2048);
}

static int64_t NativeEdgeStep(int dx, int dy) {
    int64_t bias = dx < 0 ? -(dy - 1) : (dx > 0 ? dy - 1 : 0);
    return ((int64_t)dx * 4294967296LL + bias) / dy;
}

static int NativeAttr(const SwVertex* v, int i) {
    switch (i) {
    case 0: return v->r;
    case 1: return v->g;
    case 2: return v->b;
    case 3: return v->u;
    default: return v->v;
    }
}

static void RasterTriangleNative(
    const SwVertex* v0, const SwVertex* v1, const SwVertex* v2, bool textured,
    bool raw_texture, bool semi, bool gouraud, u16 tpage, u16 clut) {
    const SwVertex* swap;
    /* tl: one bit per position, tracking the leftmost vertex (ties keep the
     * earlier vertex) through the sort by Y */
    unsigned tl;
    if (v1->x <= v0->x)
        tl = v2->x <= v1->x ? 4 : 2;
    else
        tl = v2->x < v0->x ? 4 : 1;
    if (v2->y < v1->y) {
        swap = v2; v2 = v1; v1 = swap;
        tl = ((tl >> 1) & 2) | ((tl << 1) & 4) | (tl & 1);
    }
    if (v1->y < v0->y) {
        swap = v1; v1 = v0; v0 = swap;
        tl = ((tl >> 1) & 1) | ((tl << 1) & 2) | (tl & 4);
    }
    if (v2->y < v1->y) {
        swap = v2; v2 = v1; v1 = swap;
        tl = ((tl >> 1) & 2) | ((tl << 1) & 4) | (tl & 1);
    }
    const SwVertex* vertex[3] = {v0, v1, v2};
    tl >>= 1;
    if (v0->y == v2->y)
        return;

    int64_t base = NativeEdgeStart(v0->x);
    int64_t base_step = NativeEdgeStep(v2->x - v0->x, v2->y - v0->y);
    int64_t upper_step =
        v1->y == v0->y ? 0 : NativeEdgeStep(v1->x - v0->x, v1->y - v0->y);
    int64_t lower_step =
        v2->y == v1->y ? 0 : NativeEdgeStep(v2->x - v1->x, v2->y - v1->y);
    int vo = tl != 0 ? 1 : 0;
    int vp = tl == 2 ? 3 : 0;
    bool right_facing =
        v1->y == v0->y ? v1->x > v0->x : upper_step > base_step;
    int rfi = right_facing ? 1 : 0, ofi = !rfi;

    NativePart parts[2];
    NativePart* po = &parts[vo];
    NativePart* pp = &parts[vo ^ 1];
    po->y = vertex[0 ^ vo]->y;
    po->end_y = vertex[1 ^ vo]->y;
    pp->y = vertex[1 ^ vp]->y;
    pp->end_y = vertex[2 ^ vp]->y;
    po->x[rfi] = (uint64_t)NativeEdgeStart(vertex[0 ^ vo]->x);
    po->step[rfi] = (uint64_t)upper_step;
    po->x[ofi] = (uint64_t)(base + (int64_t)(vertex[vo]->y - vertex[0]->y) * base_step);
    po->step[ofi] = (uint64_t)base_step;
    po->upwards = vo != 0;
    pp->x[rfi] = (uint64_t)NativeEdgeStart(vertex[1 ^ vp]->x);
    pp->step[rfi] = (uint64_t)lower_step;
    pp->x[ofi] =
        (uint64_t)(base + (int64_t)(vertex[1 ^ vp]->y - vertex[0]->y) * base_step);
    pp->step[ofi] = (uint64_t)base_step;
    pp->upwards = vp != 0;

    s32 det = (v1->x - v0->x) * (v2->y - v1->y) - (v2->x - v1->x) * (v1->y - v0->y);
    if (!det)
        return;
    NativePoly p = {
        .textured = textured,
        .raw = raw_texture,
        .semi = semi,
        .gouraud = gouraud,
        .dither = SoftwareDither() && (gouraud || (textured && !raw_texture)),
        .tpage = tpage,
        .clut = clut,
        .abr = (tpage >> 5) & 3,
    };
    const SwVertex* left = vertex[tl];
    p.r = left->r;
    p.g = left->g;
    p.b = left->b;
    for (int i = 0; i < 5; i++) {
        if (i < 3 ? !gouraud : !textured)
            continue;
        s32 a0 = NativeAttr(v0, i), a1 = NativeAttr(v1, i), a2 = NativeAttr(v2, i);
        s32 per_x = (a1 - a0) * (v2->y - v1->y) - (a2 - a1) * (v1->y - v0->y);
        s32 per_y = (v1->x - v0->x) * (a2 - a1) - (v2->x - v1->x) * (a1 - a0);
        p.dx[i] = (u32)(s32)((int64_t)per_x * 4096 / det) << 12;
        p.dy[i] = (u32)(s32)((int64_t)per_y * 4096 / det) << 12;
    }
    u32 origin[5];
    for (int i = 0; i < 5; i++)
        origin[i] = (((u32)NativeAttr(left, i) << 12) + (1u << 11)) << 12;
    NativeStep(origin, p.dx, -left->x);
    NativeStep(origin, p.dy, -left->y);
    for (int i = 0; i < 2; i++)
        NativeDrawPart(&p, &parts[i], origin);
}

/* Polygons whose bounding box spans 1024 or more columns or 512 or more rows
 * are not drawn. */
static bool NativeTriangleCulled(const SwVertex* a, const SwVertex* b,
                                 const SwVertex* c) {
    return Max3(a->x, b->x, c->x) - Min3(a->x, b->x, c->x) >= VRAM_W ||
           Max3(a->y, b->y, c->y) - Min3(a->y, b->y, c->y) >= VRAM_H;
}

static void RasterLineNative(const SwVertex* p0, const SwVertex* p1, bool semi,
                             bool gouraud, u16 tpage) {
    int dx = abs(p1->x - p0->x), dy = abs(p1->y - p0->y);
    int k = dx > dy ? dx : dy;
    if (dx >= VRAM_W || dy >= VRAM_H)
        return;
    if (p0->x >= p1->x && k > 0) {
        const SwVertex* swap = p0;
        p0 = p1;
        p1 = swap;
    }
    int64_t step_x = 0, step_y = 0;
    s32 step_c[3] = {0, 0, 0};
    const int c0[3] = {p0->r, p0->g, p0->b}, c1[3] = {p1->r, p1->g, p1->b};
    if (k) {
        int64_t delta[2] = {p1->x - p0->x, p1->y - p0->y};
        int64_t step[2];
        for (int i = 0; i < 2; i++)
            step[i] = (delta[i] * 4294967296LL - (delta[i] < 0 ? k - 1 : 0) +
                       (delta[i] > 0 ? k - 1 : 0)) / k;
        step_x = step[0];
        step_y = step[1];
        if (gouraud)
            for (int i = 0; i < 3; i++)
                step_c[i] = (s32)((u32)(c1[i] - c0[i]) << 12) / k;
    }
    int64_t x = (int64_t)p0->x * 4294967296LL + (1LL << 31) - 1024;
    int64_t y = (int64_t)p0->y * 4294967296LL + (1LL << 31) - (step_y < 0 ? 1024 : 0);
    s32 c[3];
    for (int i = 0; i < 3; i++)
        c[i] = (c0[i] << 12) | (1 << 11);
    bool dithered = SoftwareDither() != 0;
    int abr = (tpage >> 5) & 3;
    for (int i = 0; i <= k; i++) {
        int px = Sign11((int)(x >> 32)), py = Sign11((int)(y >> 32));
        if (px >= draw_area_start.x && px <= draw_area_end.x &&
            py >= draw_area_start.y && py <= draw_area_end.y) {
            int r = gouraud ? (u8)(c[0] >> 12) : c0[0];
            int g = gouraud ? (u8)(c[1] >> 12) : c0[1];
            int b = gouraud ? (u8)(c[2] >> 12) : c0[2];
            int d = dithered ? dither[py & 3][px & 3] : 0;
            WritePixel(px, py & (VRAM_H - 1),
                       PackColor((r + d) >> 3, (g + d) >> 3, (b + d) >> 3, false),
                       semi, abr);
        }
        x += step_x;
        y += step_y;
        for (int j = 0; j < 3; j++)
            c[j] += step_c[j];
    }
}


static SwVertex VertexXY(u32 word, u32 color) {
    SwVertex v = {0};
    v.x = Sign11(word) + draw_offset.x;
    v.y = Sign11(word >> 16) + draw_offset.y;
    if (grid_source != grid_target) {
        v.x = draw_offset.x +
              (v.x - draw_offset.x) * (int)grid_target / (int)grid_source;
    }
    v.r = color & 0xff;
    v.g = (color >> 8) & 0xff;
    v.b = (color >> 16) & 0xff;
    return v;
}

static void DrawLine(SwVertex a, SwVertex b, bool semi, u16 tpage) {
    RasterLineNative(&a, &b, semi, true, tpage);
}

static void RasterSprite(
    const SwVertex* origin, int w, int source_w, int h, int u, int v,
    bool textured, bool raw_texture, bool semi, u16 clut) {
    int abr = (cur_tpage >> 5) & 3;
    for (int py = 0; py < h; py++) {
        int y = origin->y + py;
        if (y < draw_area_start.y || y > draw_area_end.y) {
            continue;
        }
        y &= VRAM_H - 1;
        for (int px = 0; px < w; px++) {
            int x = origin->x + px;
            if (x < draw_area_start.x || x > draw_area_end.x) {
                continue;
            }
            u16 color;
            bool blend = semi;
            if (textured) {
                int sample_x = w ? px * source_w / w : 0;
                u16 texel =
                    SampleTexture(u + sample_x, v + py, cur_tpage, clut);
                if (texel == 0) {
                    continue;
                }
                blend = semi && (texel & 0x8000);
                color = ShadeTexel(texel, origin->r, origin->g, origin->b,
                                   raw_texture, x, y, false);
            } else {
                color = PackColor(
                    origin->r >> 3, origin->g >> 3, origin->b >> 3, false);
            }
            WritePixel(x, y, color, blend, abr);
        }
    }
}

static void Software_Reset(void) {
    cur_tpage = 0;
    draw_offset = (Point){0, 0};
    draw_area_start = (Point){0, 0};
    draw_area_end = (Point){VRAM_W - 1, VRAM_H - 1};
    set_mask_bit = preserve_masked = false;
    Software_ClearCache();
}

static void Software_SetTexpageMode(ParamDrawTexpageMode* p) {
    u16 mode = *(u16*)p;
    cur_tpage = mode & 0x1ff;
    dither_enabled = (mode & 0x200) != 0;
}

static void Software_SetTextureWindow(unsigned int mask_x, unsigned int mask_y,
                                      unsigned int off_x, unsigned int off_y) {
    mask_x &= 31;
    mask_y &= 31;
    twin_and_x = (u8) ~(mask_x * 8);
    twin_and_y = (u8) ~(mask_y * 8);
    twin_or_x = (u8)((off_x & mask_x) * 8);
    twin_or_y = (u8)((off_y & mask_y) * 8);
}

static void Software_SetAreaStart(int x, int y) {
    draw_area_start = (Point){x, y};
}

static void Software_SetAreaEnd(int x, int y) { draw_area_end = (Point){x, y}; }

static void Software_SetOffset(int x, int y) {
    draw_offset = (Point){Sign11(x), Sign11(y)};
}

static int Software_SetHorizontalGrid(
    unsigned int source_width, unsigned int target_width) {
    if (!source_width || !target_width) {
        return -1;
    }
    grid_source = source_width;
    grid_target = target_width;
    return 0;
}

static void Software_SetMask(int bit0, int bit1) {
    set_mask_bit = bit0 != 0;
    preserve_masked = bit1 != 0;
}

static void Software_ClearImage(PS1_RECT* rect, u_char r, u_char g, u_char b) {
    /* GP0 02h writes bit 15 clear and ignores both mask settings; libgpu
     * passes rectangles already rounded and cut at the VRAM edges. */
    u16 color = PackColor(r >> 3, g >> 3, b >> 3, false);
    int x0 = CLAMP(rect->x, 0, VRAM_W);
    int y0 = CLAMP(rect->y, 0, VRAM_H);
    int x1 = CLAMP(rect->x + rect->w, 0, VRAM_W);
    int y1 = CLAMP(rect->y + rect->h, 0, VRAM_H);
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            vram[y * VRAM_W + x] = color;
        }
    }
}

/* Writes one VRAM row of an upload or copy with the GPU's mask rules: a
 * pixel with bit 15 set is kept while "check mask" is on, and "set mask"
 * forces bit 15 of each written pixel. Rows wrap at the VRAM width. */
static void TransferRow(int x, int y, int w, const u16* src) {
    u16* row = &vram[(y & (VRAM_H - 1)) * VRAM_W];
    if (!set_mask_bit && !preserve_masked && x >= 0 && x + w <= VRAM_W) {
        memcpy(&row[x], src, (size_t)w * sizeof(u16));
    } else {
        for (int i = 0; i < w; i++) {
            u16* p = &row[(x + i) & (VRAM_W - 1)];
            if (!preserve_masked || !(*p & 0x8000))
                *p = (u16)(src[i] | (set_mask_bit ? 0x8000 : 0));
        }
    }
}

static void Software_LoadImage(PS1_RECT* rect, u_long* data) {
    if (rect->w <= 0 || rect->h <= 0) {
        return;
    }
    const u16* src = (const u16*)data;
    for (int y = 0; y < rect->h; y++)
        TransferRow(rect->x & (VRAM_W - 1), rect->y + y, rect->w,
                    &src[(size_t)y * rect->w]);
}

static void Software_StoreImage(PS1_RECT* rect, u_long* data) {
    if (rect->w <= 0 || rect->h <= 0) {
        return;
    }
    u16* dst = (u16*)data;
    if (rect->x >= 0 && rect->y >= 0 && rect->x + rect->w <= VRAM_W &&
        rect->y + rect->h <= VRAM_H) {
        for (int y = 0; y < rect->h; y++) {
            memcpy(&dst[y * rect->w], &vram[(rect->y + y) * VRAM_W + rect->x],
                   (size_t)rect->w * sizeof(u16));
        }
        return;
    }
    for (int y = 0; y < rect->h; y++) {
        for (int x = 0; x < rect->w; x++) {
            int vx = rect->x + x;
            int vy = rect->y + y;
            dst[y * rect->w + x] =
                ((unsigned)vx < VRAM_W && (unsigned)vy < VRAM_H)
                    ? vram[vy * VRAM_W + vx]
                    : 0;
        }
    }
}

static void Software_MoveImage(PS1_RECT* rect, unsigned int x, unsigned int y) {
    int w = CLAMP(rect->w, 0, VRAM_W);
    u16 line[VRAM_W];
    /* row by row from the top, each row read before it is written */
    for (int row = 0; row < rect->h; row++) {
        const u16* src = &vram[((rect->y + row) & (VRAM_H - 1)) * VRAM_W];
        for (int i = 0; i < w; i++)
            line[i] = src[(rect->x + i) & (VRAM_W - 1)];
        TransferRow((int)x, (int)y + row, w, line);
    }
}

static int Software_PushPrim(u_long* words, int max_len) {
    if (max_len <= 0) {
        return 0;
    }
    int code = (int)(words[0] >> 24);
    int line_length = 0, line_points = 0;
    if ((code & 0xe0) == 0x40)
        line_points = Draw_LineVertices(words, max_len, &line_length);
    bool textured = (code & TEXTURED) != 0;
    bool gouraud = (code & GOURAUD) != 0;
    bool semi = (code & SEMITRANSP) != 0;
    bool raw_texture = textured && (code & 1);
    bool polygon = !(code & 0x40);
    bool line = (code & 0x40) && !(code & 0x20);
    int at = 1;
    if (polygon && (code & TRIANGLE)) {
        int count = (code & EXTRA_VERTEX) ? 4 : 3;
        int required = 1 + count * (1 + textured) + (count - 1) * gouraud;
        if (max_len < required) {
            return max_len;
        }
        SwVertex v[4] = {0};
        u16 clut = 0;
        u16 tpage = cur_tpage;
        u32 color = words[0];
        for (int i = 0; i < count && at < max_len; i++) {
            v[i] = VertexXY(words[at++], color);
            if (textured && at < max_len) {
                u32 uv = words[at++];
                v[i].u = uv & 0xff;
                v[i].v = (uv >> 8) & 0xff;
                if (i == 0) {
                    clut = uv >> 16;
                } else if (i == 1) {
                    tpage = uv >> 16;
                }
            }
            if (gouraud && i + 1 < count && at < max_len) {
                color = words[at++];
            }
        }
        if (textured) {
            /* A textured polygon's page (bits 0-8) becomes the GPU's drawing
             * mode, as an E1h command would set it: later sprites use it. */
            cur_tpage = (u16)((cur_tpage & ~0x1ff) | (tpage & 0x1ff));
            ClutCacheUpdate(tpage, clut);
        }
        if (!NativeTriangleCulled(&v[0], &v[1], &v[2]))
            RasterTriangleNative(&v[0], &v[1], &v[2], textured, raw_texture,
                                 semi, gouraud, tpage, clut);
        if (count == 4 && !NativeTriangleCulled(&v[2], &v[1], &v[3])) {
            RasterTriangleNative(&v[2], &v[1], &v[3], textured, raw_texture,
                                 semi, gouraud, tpage, clut);
        }
        return at;
    }
    if (line) {
        /* two vertices, or a polyline up to its terminator */
        if (line_length > max_len) {
            return max_len;
        }
        SwVertex prev = {0};
        u32 color = words[0];
        for (int i = 0; i < line_points; i++) {
            if (gouraud && i)
                color = words[at++];
            SwVertex cur = VertexXY(words[at++], color);
            if (i) {
                DrawLine(prev, cur, semi, cur_tpage);
            }
            prev = cur;
        }
        return line_length;
    }
    if ((code & 0x60) == 0x60) {
        int required = 2 + textured + ((code & 0x18) == 0);
        if (max_len < required) {
            return max_len;
        }
        SwVertex origin = VertexXY(words[at++], words[0]);
        /* rectangle positions wrap to 11 bits after the drawing offset */
        origin.x = Sign11(origin.x);
        origin.y = Sign11(origin.y);
        u16 clut = 0;
        int u = 0, v = 0;
        if (textured && at < max_len) {
            u32 uv = words[at++];
            u = uv & 0xff;
            v = (uv >> 8) & 0xff;
            clut = uv >> 16;
            ClutCacheUpdate(cur_tpage, clut);
        }
        int w, h;
        switch (code & ~3) {
        case 0x60:
        case 0x64:
            w = (int)(words[at] & (VRAM_W - 1));
            h = (int)((words[at++] >> 16) & (VRAM_H - 1));
            break;
        case 0x68:
        case 0x6c:
            w = h = 1;
            break;
        case 0x70:
        case 0x74:
            w = h = 8;
            break;
        default:
            w = h = 16;
            break;
        }
        int source_w = w;
        if (grid_source != grid_target) {
            w = w * (int)grid_target / (int)grid_source;
        }
        RasterSprite(
            &origin, w, source_w, h, u, v, textured, raw_texture, semi, clut);
        return at;
    }
    WARNF("unsupported GP0 primitive %02x", code);
    return 1;
}

void SoftwareRasterizer_Reset(void) {
    memset(vram, 0, sizeof(vram));
    Software_Reset();
    grid_source = grid_target = 1;
    twin_and_x = twin_and_y = 0xff;
    twin_or_x = twin_or_y = 0;
    dither_enabled = false;
}

const u16* SoftwareRasterizer_GetVram(void) { return vram; }

void SoftwareRasterizer_SetVram(const u16* pixels) {
    Software_ClearCache();
    memcpy(vram, pixels, sizeof(vram));
}

static void Software_FlushBuffer(void) {}
static void Software_ResetBuffer(void) {}
static int Software_ExequeSync(void) { return 0; }

const DrawBackend software_rasterizer = {
    .reset = Software_Reset,
    .texpage_mode = Software_SetTexpageMode,
    .texture_window = Software_SetTextureWindow,
    .area_start = Software_SetAreaStart,
    .area_end = Software_SetAreaEnd,
    .offset = Software_SetOffset,
    .horizontal_grid = Software_SetHorizontalGrid,
    .mask = Software_SetMask,
    .clear_cache = Software_ClearCache,
    .clear_image = Software_ClearImage,
    .load_image = Software_LoadImage,
    .store_image = Software_StoreImage,
    .move_image = Software_MoveImage,
    .reset_buffer = Software_ResetBuffer,
    .flush_buffer = Software_FlushBuffer,
    .push_prim = Software_PushPrim,
    .exeque_sync = Software_ExequeSync,
};
