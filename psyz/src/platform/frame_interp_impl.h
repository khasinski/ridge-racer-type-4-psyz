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
static const char* const interp_sync_names[INTERP_SYNC_N] = {
    "frame", "full", "texture", "clut", "target", "present", "settings"};

static unsigned InterpFieldCell(int cx, int cy, int ox, int oy) {
    uint32_t h = (uint32_t)cx * 0x9E3779B1u ^ (uint32_t)cy * 0x85EBCA77u ^
                 (uint32_t)(ox * 977 + oy) * 0xC2B2AE3Du;
    return (h ^ h >> 16) & (INTERP_FIELD_CELLS - 1);
}

/* Returns the entry (1-based), 0 for none. */
static unsigned InterpFieldAdd(unsigned which, const float* from, float depth,
                               int ox, int oy, const float* to) {
    InterpField* f = &interp_field[which];
    if (f->frame != interp_frame) {
        f->frame = interp_frame;
        f->len = 0;
    }
    if (f->len >= INTERP_FIELD) {
        InterpEvict(IEVICT_FIELD);
        return 0;
    }
    if (!(SDL_fabsf(from[0]) < 2048.0f && SDL_fabsf(from[1]) < 2048.0f))
        return 0;
    const unsigned c =
        InterpFieldCell((int)SDL_floorf(from[0] / INTERP_FIELD_CELL),
                        (int)SDL_floorf(from[1] / INTERP_FIELD_CELL), ox, oy);
    if (f->stamp[c] != interp_frame) {
        f->stamp[c] = interp_frame;
        f->head[c] = 0;
    }
    f->entry[f->len] = (InterpFieldEntry){
        from[0],         from[1],   depth,     to[0] - from[0],
        to[1] - from[1], (short)ox, (short)oy, f->head[c]};
    f->head[c] = ++f->len;
    return f->len;
}

/* The motion of the nearest entry within reach at a like depth (depth 0:
 * any), its squared distance at *distance; false for none. */
static bool InterpFieldMotion(
    unsigned which, const float* at, float depth, int ox, int oy,
    float motion[2], float* distance) {
    const InterpField* f = &interp_field[which];
    if (f->frame != interp_frame ||
        !(SDL_fabsf(at[0]) < 2048.0f && SDL_fabsf(at[1]) < 2048.0f))
        return false;
    const int cx = (int)SDL_floorf(at[0] / INTERP_FIELD_CELL),
              cy = (int)SDL_floorf(at[1] / INTERP_FIELD_CELL);
    float best = INTERP_FIELD_REACH * INTERP_FIELD_REACH;
    bool found = false;
    for (int y = cy - 2; y <= cy + 2; ++y)
        for (int x = cx - 2; x <= cx + 2; ++x) {
            const unsigned c = InterpFieldCell(x, y, ox, oy);
            if (f->stamp[c] != interp_frame)
                continue;
            for (unsigned k = f->head[c]; k; k = f->entry[k - 1].next) {
                const InterpFieldEntry* e = &f->entry[k - 1];
                if (e->ox != ox || e->oy != oy)
                    continue;
                if (depth > 0.0f && e->depth > 0.0f &&
                    SDL_fabsf(e->depth - depth) >
                        INTERP_FIELD_DEPTH * SDL_max(e->depth, depth))
                    continue;
                const float d = (e->x - at[0]) * (e->x - at[0]) +
                                (e->y - at[1]) * (e->y - at[1]);
                if (d < best) {
                    best = d;
                    motion[0] = e->dx;
                    motion[1] = e->dy;
                    found = true;
                }
            }
        }
    if (found)
        *distance = best;
    return found;
}

static bool InterpPend(
    unsigned which, unsigned pair, unsigned base, unsigned count,
    const float (*from)[3], const float* depth, const unsigned char* index,
    unsigned indices, int ox, int oy) {
    if (count > 6 || indices > 6)
        return false;
    if (interp_pending_len >= INTERP_PENDING) {
        InterpEvict(IEVICT_PENDING);
        return false;
    }
    InterpPending* q = &interp_pending[interp_pending_len++];
    /* base: in the frame's midpoint store */
    q->pair = pair;
    q->base = interp_mid_len + base;
    q->which = (unsigned char)which;
    q->count = (unsigned char)count;
    q->indices = (unsigned char)indices;
    q->ox = (short)ox;
    q->oy = (short)oy;
    for (unsigned i = 0; i < count; ++i) {
        q->from[i][0] = from[i][0];
        q->from[i][1] = from[i][1];
        q->depth[i] = depth[i];
    }
    for (unsigned i = 0; i < indices; ++i)
        q->index[i] = index[i];
    InterpExposeViewNow(&q->view);
    return true;
}

/* Packet addresses are hashed by their offset from a table of this
 * executable: the game's packets live in its static data, which ASLR moves
 * with the tables, while slots by the absolute address would make which
 * entries collide, and so which ones a full probe sequence drops, differ
 * from run to run. */
static unsigned InterpHash(const void* p) {
    uintptr_t n = (uintptr_t)p - (uintptr_t)interp_addresses;
    return (unsigned)(((n >> 2) ^ (n >> 16) ^ (n >> 30)) & (INTERP_MAP - 1));
}

static InterpAddress* InterpFind(InterpAddress* map, const void* p) {
    unsigned h = InterpHash(p);
    for (unsigned i = 0; i < 4; ++i) {
        InterpAddress* e = &map[(h + i) & (INTERP_MAP - 1)];
        if (e->address == p)
            return e;
    }
    return NULL;
}

static void InterpMap(
    InterpAddress* map, const void* p, unsigned pair, unsigned serial) {
    unsigned h = InterpHash(p);
    InterpAddress* e = InterpFind(map, p);
    if (!e) {
        e = &map[h];
        bool free_slot = false;
        for (unsigned i = 0; i < 4; ++i)
            if (!map[(h + i) & (INTERP_MAP - 1)].address) {
                e = &map[(h + i) & (INTERP_MAP - 1)];
                free_slot = true;
                break;
            }
        /* another packet's entry goes: count it when its pair (for a tag,
         * its frame) is still current */
        if (!free_slot && e->serial &&
            interp_pairs[e->pair].serial == e->serial &&
            (map != interp_tags || e->frame >= interp_frame))
            InterpEvict(map == interp_tags    ? IEVICT_TAGS
                        : map == interp_queue ? IEVICT_QUEUE
                                              : IEVICT_ADDRESSES);
    }
    *e = (InterpAddress){p, pair, serial};
}

void Psyz_GpuRegisterInterpolationPair(const void* a, const void* b) {
    if (!a || !b)
        return;
    if (!interp_serial)
        interp_target_stale = true;
    unsigned serial = ++interp_serial;
    if (!serial)
        serial = ++interp_serial;
    unsigned index = serial & (INTERP_PAIRS - 1);
    if (interp_pairs[index].serial && interp_pairs[index].frame &&
        interp_pairs[index].frame + 1 >= interp_frame)
        InterpEvict(IEVICT_PAIRS);
    interp_pairs[index] = (InterpPair){.a = a, .b = b, .serial = serial};
    InterpMap(interp_addresses, a, index, serial);
    InterpMap(interp_addresses, b, index, serial);
}

void Psyz_GpuRegisterInterpolationBackgroundPair(const void* a, const void* b) {
    Psyz_GpuRegisterInterpolationPair(a, b);
    InterpAddress* e = InterpFind(interp_addresses, a);
    if (e && interp_pairs[e->pair].serial == e->serial)
        interp_pairs[e->pair].background = true;
}

void Psyz_GpuEnsureInterpolationPair(const void* a, const void* b) {
    InterpAddress *x = InterpFind(interp_addresses, a),
                  *y = InterpFind(interp_addresses, b);
    if (x && y && x->pair == y->pair && x->serial == y->serial && x->serial &&
        interp_pairs[x->pair].serial == x->serial)
        return;
    Psyz_GpuRegisterInterpolationPair(a, b);
}

void Psyz_GpuEnsureInterpolationBackgroundPair(const void* a, const void* b) {
    Psyz_GpuEnsureInterpolationPair(a, b);
    InterpAddress* e = InterpFind(interp_addresses, a);
    if (e && interp_pairs[e->pair].serial == e->serial)
        interp_pairs[e->pair].background = true;
}

/* A tagged packet of the frame being drawn, or NULL. */
static InterpAddress* InterpFindTagged(const void* packet) {
    InterpAddress* t = InterpFind(interp_tags, packet);
    return t && t->serial && t->frame == interp_frame &&
                   interp_pairs[t->pair].serial == t->serial
               ? t
               : NULL;
}

void Psyz_GpuTagInterpolationPacket(const void* packet, unsigned long long id) {
    Psyz_GpuTagInterpolationPacketFlags(packet, id, 0);
}

void Psyz_GpuTagInterpolationPacketFlags(
    const void* packet, unsigned long long id, unsigned flags) {
    if (!interpolation_enabled || !packet)
        return;
    uint64_t mix = (uint64_t)id * 0x9E3779B97F4A7C15ULL;
    unsigned h = (unsigned)(mix >> 40) & (INTERP_MAP - 1);
    InterpId *slot = NULL, *free_slot = NULL;
    for (unsigned i = 0; i < INTERP_ID_PROBES && !slot; ++i) {
        InterpId* e = &interp_ids[(h + i) & (INTERP_MAP - 1)];
        if (e->serial && e->id == id)
            slot = e;
        else if (!free_slot &&
                 (!e->serial || interp_pairs[e->pair].serial != e->serial))
            free_slot = e;
    }
    if (!slot || interp_pairs[slot->pair].serial != slot->serial) {
        if (!slot) {
            slot = free_slot ? free_slot : &interp_ids[h];
            if (!free_slot)
                InterpEvict(IEVICT_IDS);
        }
        if (!interp_serial)
            interp_target_stale = true;
        /* a new identity takes the next pair that no identity drew in the
         * last two frames, so the live ones keep their history */
        unsigned serial, index;
        for (unsigned tries = 0;; ++tries) {
            serial = ++interp_serial;
            if (!serial)
                serial = ++interp_serial;
            index = serial & (INTERP_PAIRS - 1);
            const InterpPair* old = &interp_pairs[index];
            if (!old->serial || !old->tagged || !old->frame ||
                old->frame + 1 < interp_frame)
                break;
            if (tries >= 64) {
                InterpEvict(IEVICT_PAIRS);
                break;
            }
        }
        interp_pairs[index] =
            (InterpPair){.serial = serial, .tagged = true, .id = id};
        *slot = (InterpId){id, index, serial};
    }
    InterpMap(interp_tags, packet, slot->pair, slot->serial);
    InterpAddress* t = InterpFind(interp_tags, packet);
    /* drawn after the next BeginInterpolationFrame; the flags are the
     * packet's (one identity may be drawn twice, e.g. a tile listed twice) */
    if (t) {
        t->frame = interp_frame + 1;
        t->flags = flags;
    }
    interp_pairs[slot->pair].tagged_frame = interp_frame + 1;
}

void Psyz_GpuCopyInterpolationPacket(const void* from, const void* to) {
    if (!interpolation_enabled)
        return;
    InterpAddress* t = InterpFindTagged(from);
    if (t) {
        const unsigned flags = t->flags;
        InterpMap(interp_queue, to, t->pair, t->serial);
        InterpAddress* q = InterpFind(interp_queue, to);
        if (q)
            q->flags = flags;
        return;
    }
    InterpAddress* e = InterpFind(interp_addresses, from);
    if (e && interp_pairs[e->pair].serial == e->serial)
        InterpMap(interp_queue, to, e->pair, e->serial);
    else {
        e = InterpFind(interp_queue, to);
        if (e)
            e->serial = 0;
    }
}

void Psyz_GpuResetInterpolation(void) {
    for (unsigned i = 0; i < INTERP_PAIRS; ++i)
        interp_pairs[i].frame = 0;
    memset(interp_queue, 0, sizeof(interp_queue));
    interp_pair_ready = false;
    interp_next_y = -1;
    interp_phase = 0;
    interp_present_mid = false;
    interp_matches = 0;
    interp_camera_frame = 0;
    interp_camera_previous = false;
}

void Psyz_GpuClearInterpolationPackets(void) {
    Psyz_GpuResetInterpolation();
    memset(interp_addresses, 0, sizeof(interp_addresses));
    memset(interp_pairs, 0, sizeof(interp_pairs));
    memset(interp_tags, 0, sizeof(interp_tags));
    memset(interp_ids, 0, sizeof(interp_ids));
}

int Psyz_GpuSetFrameInterpolation(int enabled) {
    interpolation_requested = enabled != 0;
    if (fallback_presenter)
        enabled = 0; /* no midpoints from the CPU rasterizer */
    static int logged_requested = -1, logged_effective = -1;
    if (interpolation_enabled != (enabled != 0)) {
        Draw_FlushBuffer();
        InterpMidpointSync(INTERP_SYNC_SETTINGS);
        interpolation_enabled = enabled != 0;
        Psyz_GpuResetInterpolation();
    }
    if (interpolation_enabled && device && !InterpEnsureTarget()) {
        interpolation_enabled = false;
        Psyz_GpuResetInterpolation();
        WARNF("60 Hz interpolation admission failed; staying at original "
              "cadence: %s",
              SDL_GetError());
    }
    if (!interpolation_enabled && device && SubmitCmdAndWait() &&
        SDL_WaitForGPUIdle(device)) {
        if (interp_unwitnessed) {
            SDL_ReleaseGPUTexture(device, interp_unwitnessed);
            interp_unwitnessed = NULL;
        }
        if (interpolated_vram) {
            SDL_ReleaseGPUTexture(device, interpolated_vram);
            interpolated_vram = NULL;
        }
        interpolated_scale = 0;
    }
    Psyz_PgxpSetActive(
        !headless_renderer &&
        (set_geometry_correction || set_effect_geometry_correction ||
         set_model_filtering || interpolation_enabled));
    Psyz_PgxpSetCulling(set_geometry_correction);
    Psyz_PgxpSetEffectEnabled(set_effect_geometry_correction);
    if (logged_requested != (enabled != 0) ||
        logged_effective != interpolation_enabled) {
        INFOF("60 Hz presentation requested=%d effective=%d scale=%u target=%s "
              "simulation=original rendered=unobserved",
              enabled != 0, interpolation_enabled, internal_res,
              interpolated_vram ? "admitted" : "none");
        logged_requested = enabled != 0;
        logged_effective = interpolation_enabled;
    }
    return 0;
}

int Psyz_GpuGetFrameInterpolationRequested(void) {
    return interpolation_requested;
}

int Psyz_GpuGetFrameInterpolation(void) { return interpolation_enabled; }

unsigned long long Psyz_GpuGetInterpolatedPresentCount(void) {
    return interpolated_present_count;
}

/* Admission is transactional: failed allocation/copy leaves normal VRAM intact
 * and reports original cadence through the actual getter. No resolution bump.
 */
static bool InterpEnsureTarget(void) {
    if (!device || UseSoftwareBackend())
        return false;
    if (interpolated_vram && interpolated_scale == internal_res)
        return true;
    InterpMidpointSync(INTERP_SYNC_SETTINGS);
    if (!SubmitCmdAndWait() || !SDL_WaitForGPUIdle(device))
        return false;
    if (interp_unwitnessed) {
        SDL_ReleaseGPUTexture(device, interp_unwitnessed);
        interp_unwitnessed = NULL;
    }
    const SDL_GPUTextureCreateInfo info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage =
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = VRAM_W * internal_res,
        .height = VRAM_H * internal_res,
        .layer_count_or_depth = 1,
        .num_levels = 1};
    SDL_GPUTexture* candidate = SDL_CreateGPUTexture(device, &info);
    if (!candidate)
        return false;
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    SDL_GPUCopyPass* copy = cmd ? SDL_BeginGPUCopyPass(cmd) : NULL;
    if (!copy) {
        SDL_ReleaseGPUTexture(device, candidate);
        return false;
    }
    const SDL_GPUTextureLocation from = {.texture = GetRenderTarget()},
                                 to = {.texture = candidate};
    SDL_CopyGPUTextureToTexture(copy, &from, &to, VRAM_W * internal_res,
                                VRAM_H * internal_res, 1, false);
    SDL_EndGPUCopyPass(copy);
    if (!SubmitCmdAndWait()) {
        interp_unwitnessed = candidate;
        return false;
    }
    if (interpolated_vram)
        SDL_ReleaseGPUTexture(device, interpolated_vram);
    interpolated_vram = candidate;
    interpolated_scale = internal_res;
    Psyz_GpuResetInterpolation();
    return true;
}

void Psyz_GpuGetInterpolationStats(
    unsigned* primitives, unsigned* tagged, unsigned* paired) {
    if (primitives)
        *primitives = interp_stat_last[0];
    if (tagged)
        *tagged = interp_stat_last[1];
    if (paired)
        *paired = interp_stat_last[2];
}

static void InterpEvict(unsigned table) {
    static bool warned[IEVICT_N];
    ++interp_evict[table];
    if (!warned[table]) {
        warned[table] = true;
        WARNF("interpolation table %s full at frame %u: entries dropped "
              "(PSYZ_INTERP_STATS counts them per frame)",
              interp_evict_names[table], interp_frame);
    }
}

/* Batches, midpoint draws and table overflows of the frame (appended to
 * the [interp-stats] line): vertices and indices queued, the largest batch,
 * batches drawn because full (vertices, marks, CLUT rows), for feedback,
 * for a state or VRAM change and at a present or frame end, midpoint draws
 * and the splits among them that placed ghosts, new faces, T-junctions or
 * outlines before the frame was complete, and entries dropped by table. */
static void InterpStatsBatches(char* out, size_t size) {
    const DrawFrameStats* d = &draw_frame_stats;
    unsigned syncs = 0, evicted = 0;
    for (unsigned i = 0; i < INTERP_SYNC_N; ++i)
        syncs += d->syncs[i];
    for (unsigned i = 0; i < IEVICT_N; ++i)
        evicted += interp_evict[i];
    int n = SDL_snprintf(
        out, size,
        " vertices=%u indices=%u batch=%u/%u overflow=%u marks=%u clut=%u "
        "feedback=%u state=%u present=%u syncs=%u split=%u/%u/%u/%u/%u/%u "
        "splitplaced=%u clutloads=%u evicted=%u",
        d->vertices, d->indices, d->batch_vertices, d->batch_indices,
        d->flushes[DRAW_FLUSH_OVERFLOW], d->flushes[DRAW_FLUSH_MARKS],
        d->flushes[DRAW_FLUSH_CLUT], d->flushes[DRAW_FLUSH_FEEDBACK],
        d->flushes[DRAW_FLUSH_STATE], d->flushes[DRAW_FLUSH_PRESENT], syncs,
        d->syncs[INTERP_SYNC_FULL], d->syncs[INTERP_SYNC_TEXTURE],
        d->syncs[INTERP_SYNC_CLUT], d->syncs[INTERP_SYNC_TARGET],
        d->syncs[INTERP_SYNC_PRESENT], d->syncs[INTERP_SYNC_SETTINGS],
        d->split_pending, d->clut_loads, evicted);
    for (unsigned i = 0; i < IEVICT_N && n > 0 && (size_t)n < size; ++i)
        if (interp_evict[i])
            n += SDL_snprintf(out + n, size - (size_t)n, " %s=%u",
                              interp_evict_names[i], interp_evict[i]);
}

static void InterpStatsFrame(void) {
    if (interp_stats_log < 0) {
        const char* e = SDL_getenv("PSYZ_INTERP_STATS");
        interp_stats_log = e && *e && *e != '0';
    }
    if (interp_stats_log <= 0 || !interp_frame) {
        memset(&draw_frame_stats, 0, sizeof(draw_frame_stats));
        memset(interp_evict, 0, sizeof(interp_evict));
        return;
    }
    /* identities drawn the frame before the last one and not in it */
    for (unsigned i = 0; i < INTERP_PAIRS; ++i) {
        InterpPair* p = &interp_pairs[i];
        if (p->serial && p->tagged && p->frame + 1 == interp_frame &&
            !p->midpoint_pose) {
            interp_area[ISTAT_VANISHED] += InterpArea(p->position, p->count);
            ++interp_count[ISTAT_VANISHED];
        }
    }
    const unsigned exposed = InterpExposed();
    char batches[512];
    InterpStatsBatches(batches, sizeof(batches));
    SDL_Log("[interp-stats] f=%u paired=%u/%.0f new=%u/%.0f mismatch=%u/%.0f "
            "frameonly=%u/%.0f midonly=%u/%.0f vanished=%u/%.0f ghost=%u/%.0f "
            "replaced=%u/%.0f cut=%d exposed=%u holes=%u%s",
            interp_frame, interp_count[0], interp_area[0], interp_count[1],
            interp_area[1], interp_count[2], interp_area[2], interp_count[3],
            interp_area[3], interp_count[4], interp_area[4], interp_count[5],
            interp_area[5], interp_count[6], interp_area[6], interp_count[7],
            interp_area[7], interp_ghost_cut, exposed, interp_exposed_holes,
            batches);
    memset(interp_area, 0, sizeof(interp_area));
    memset(interp_count, 0, sizeof(interp_count));
    memset(&draw_frame_stats, 0, sizeof(draw_frame_stats));
    memset(interp_evict, 0, sizeof(interp_evict));
}

void Psyz_GpuBeginInterpolationFrame(void) {
    InterpMidpointComplete(INTERP_SYNC_FRAME);
    InterpStatsFrame();
    interp_matches = 0;
    memcpy(interp_stat_last, interp_stat, sizeof(interp_stat));
    memset(interp_stat, 0, sizeof(interp_stat));
    ++interp_frame;
    if (!interp_frame) {
        Psyz_GpuResetInterpolation();
        interp_frame = 1;
    }
    interp_store_len[interp_frame & 1] = interp_order_len[interp_frame & 1] = 0;
    InterpBuildGhosts();
    if (!interpolation_enabled || !device || UseSoftwareBackend())
        return;
    if (!InterpEnsureTarget()) {
        WARNF("60 Hz interpolation target unavailable; original 30 Hz "
              "presentation retained: %s",
              SDL_GetError());
        interpolation_enabled = false;
        Psyz_GpuResetInterpolation();
        Psyz_PgxpSetActive(
            !headless_renderer &&
            (set_geometry_correction || set_effect_geometry_correction ||
             set_model_filtering));
    } else if (interp_target_stale) {
        Draw_FlushBuffer();
        SDL_GPUCommandBuffer* cmd = AcquireCmd();
        SDL_GPUCopyPass* copy = cmd ? SDL_BeginGPUCopyPass(cmd) : NULL;
        if (copy) {
            const SDL_GPUTextureLocation from = {.texture = GetRenderTarget()},
                                         to = {.texture = interpolated_vram};
            SDL_CopyGPUTextureToTexture(copy, &from, &to, VRAM_W * internal_res,
                                        VRAM_H * internal_res, 1, false);
            SDL_EndGPUCopyPass(copy);
            interp_target_stale = false;
        }
    }
}

static void InterpBackdropCopy(int x, int y, int w, int h) {
    if (InterpMidpointCovers(x, y, w, h))
        InterpMidpointSync(INTERP_SYNC_TARGET);
    SDL_GPUCommandBuffer* cmd = AcquireCmd();
    SDL_GPUCopyPass* copy = cmd ? SDL_BeginGPUCopyPass(cmd) : NULL;
    if (!copy)
        return;
    const unsigned r = internal_res;
    const int src_y = interp_backdrop.src_y + (y - interp_backdrop.dst_y);
    const SDL_GPUTextureLocation from = {.texture = GetRenderTarget(),
                                         .x = (Uint32)x * r,
                                         .y = (Uint32)src_y * r},
                                 to = {.texture = interpolated_vram,
                                       .x = (Uint32)x * r,
                                       .y = (Uint32)y * r};
    SDL_CopyGPUTextureToTexture(
        copy, &from, &to, (Uint32)w * r, (Uint32)h * r, 1, false);
    SDL_EndGPUCopyPass(copy);
}

static void InterpBackdropAfterFill(int x, int y, int w, int h) {
    if (!interp_backdrop.frame || interp_backdrop.frame != interp_frame ||
        !interpolated_vram || interpolated_scale != internal_res ||
        !interpolation_enabled)
        return;
    const int x0 = SDL_max(x, interp_backdrop.x),
              x1 = SDL_min(x + w, interp_backdrop.x + interp_backdrop.w);
    const int y0 = SDL_max(y, interp_backdrop.dst_y),
              y1 = SDL_min(y + h, interp_backdrop.dst_y + interp_backdrop.h);
    if (x0 < x1 && y0 < y1)
        InterpBackdropCopy(x0, y0, x1 - x0, y1 - y0);
}

void Psyz_GpuInterpolationBackdrop(int x, int src_y, int dst_y, int w, int h) {
    interp_backdrop.frame = 0;

    if (!interpolation_enabled || !device || UseSoftwareBackend() ||
        !interpolated_vram || interpolated_scale != internal_res ||
        !interp_serial || interp_target_stale)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (x + w > VRAM_W)
        w = VRAM_W - x;
    if (w <= 0 || h <= 0 || src_y < 0 || dst_y < 0 || src_y + h > VRAM_H ||
        dst_y + h > VRAM_H || src_y == dst_y)
        return;
    Draw_FlushBuffer();
    interp_backdrop.x = x;
    interp_backdrop.src_y = src_y;
    interp_backdrop.dst_y = dst_y;
    interp_backdrop.w = w;
    interp_backdrop.h = h;
    interp_backdrop.frame = interp_frame;
    interp_expose_rect[interp_frame & 1].x = x;
    interp_expose_rect[interp_frame & 1].y = dst_y;
    interp_expose_rect[interp_frame & 1].w = w;
    interp_expose_rect[interp_frame & 1].h = h;
    interp_expose_rect[interp_frame & 1].frame = interp_frame;
    InterpBackdropCopy(x, dst_y, w, h);
}

void Psyz_GpuSetInterpolationCamera(int x, int y) {
    interp_camera_previous =
        interp_camera_frame && interp_camera_frame + 1 == interp_frame;
    int dx = x - interp_camera_x, dy = y - interp_camera_y;
    if (interp_camera_previous && (abs(dx) > 32 || abs(dy) > 32)) {
        Psyz_GpuResetInterpolation();
        interp_camera_previous = false;
    }
    interp_camera_half_x = interp_camera_previous ? dx * 0.5f : 0;
    interp_camera_half_y = interp_camera_previous ? dy * 0.5f : 0;
    interp_camera_x = x;
    interp_camera_y = y;
    interp_camera_frame = interp_frame;
}

/* The midpoint of the frame just drawn is no plausible in-between: a cut
 * (most of the frame's identities are new), much of the picture is
 * geometry without a pose in the frame before, which the midpoint can only
 * draw where this frame has it, or it shows what neither frame shows (a
 * hole or the background through a gap, InterpExposed). The frame before
 * stays on screen for that field instead. PSYZ_INTERP_HOLD=share of the
 * picture (INTERP_HOLD_SHARE), PSYZ_INTERP_EXPOSE=cells
 * (INTERP_EXPOSE_LIMIT, 0: off). */
static bool InterpMidpointUnsound(void) {
    static float share = -1;
    if (share < 0) {
        const char* e = SDL_getenv("PSYZ_INTERP_HOLD");
        share = e && *e ? (float)SDL_atof(e) : INTERP_HOLD_SHARE;
    }
    static int expose = -1;
    if (expose < 0) {
        const char* e = SDL_getenv("PSYZ_INTERP_EXPOSE");
        expose = e && *e ? SDL_atoi(e) : INTERP_EXPOSE_LIMIT;
    }
    if (interp_ghost_cut)
        return true;
    if (expose > 0 && InterpExposed() >= (unsigned)expose)
        return true;
    const double picture = (double)display_size.x * display_size.y;
    const double posed = interp_area[ISTAT_PAIRED] + interp_area[ISTAT_GHOST] +
                         interp_area[ISTAT_REPLACED];
    const double unposed = interp_area[ISTAT_NEW] + interp_area[ISTAT_MISMATCH];
    return unposed > picture * share ||
           (posed + unposed > 0 && posed < unposed);
}

void Psyz_GpuPrepareInterpolatedPresent(int next_y) {
    /* the frame is complete: its midpoint is placed and drawn */
    InterpMidpointComplete(INTERP_SYNC_FRAME);
    interp_next_y = next_y;
    interp_phase = 0;
    interp_blank_mid = false;
    interp_pair_ready = interpolation_enabled && interpolated_vram &&
                        interp_matches > 0 && next_y >= 0 && next_y < VRAM_H;
}

void Psyz_GpuPrepareInterpolatedBlank(int next_y) {
    Psyz_GpuPrepareInterpolatedPresent(next_y);
    interp_blank_mid = interp_pair_ready;
}

/* A line packet tagged for interpolation: records its points and, when the
 * previous frame drew the same identity with as many points, returns the
 * midpoint points (VRAM pixels relative to the drawing offset). A midpoint-
 * only line (*own false) is not drawn into the frame. */
static bool InterpLine(const u_long* words, int count, bool gouraud, int* mid_x,
                       int* mid_y, bool* own, bool* own_only) {
    *own = true;
    *own_only = false;
    if (!interpolation_enabled || !interpolated_vram ||
        interpolated_scale != internal_res || count < 2 ||
        count > INTERP_LINE_POINTS)
        return false;
    const void* packet = interpolation_packet_current;
    ++interp_stat[0];
    InterpAddress* e = InterpFind(interp_queue, packet);
    if (!e)
        e = InterpFindTagged(packet);
    if (!e)
        e = InterpFind(interp_addresses, packet);
    if (!e || !e->serial)
        return false;
    InterpPair* p = &interp_pairs[e->pair];
    if (p->serial != e->serial || !p->tagged)
        return false;
    ++interp_stat[1];
    *own = !(e->flags & PSYZ_INTERP_MIDPOINT_ONLY);
    *own_only = (e->flags & PSYZ_INTERP_FRAME_ONLY) != 0;
    float position[INTERP_LINE_POINTS][3];
    const unsigned points =
        (unsigned)count | 0x100u; /* never pairs with a polygon */
    bool valid = p->frame && p->frame + 1 == interp_frame && p->count == points;
    for (int i = 0, at = 0; i < count; i++) {
        if (i && gouraud)
            at++;
        position[i][0] = (float)s11(((const short*)&words[at])[0]);
        position[i][1] = (float)s11(((const short*)&words[at])[1]);
        position[i][2] = 1.0f;
        at++;
        if (SDL_fabsf(position[i][0] - p->position[i][0]) >
                INTERP_LINE_PAIR_REACH ||
            SDL_fabsf(position[i][1] - p->position[i][1]) >
                INTERP_LINE_PAIR_REACH)
            valid = false;
    }
    /* A midpoint-only line pairs as a polygon does (see InterpCapture);
     * one that cannot continues the last motion of a line the game drew in
     * the frame before. */
    if (!*own && (p->midpoint_pose || (e->flags & PSYZ_INTERP_NO_POSE)))
        valid = false;
    const bool extrapolated =
        !valid && !*own && p->frame && p->frame + 1 == interp_frame &&
        p->count == points && !p->midpoint_pose && p->motion_frame == p->frame;
    /* a new line where ghosts go: the outline of the other representation
     * of what the ghost draws (InterpGhostCovers) */
    if (!valid && !extrapolated && *own &&
        !(p->frame && p->frame + 1 == interp_frame) &&
        InterpGhostCoversLine(position, count))
        *own_only = true;
    /* a new line (or one whose points changed) is placed as its neighbours
     * are, from this frame's pose (InterpFinishLines) */
    const bool newcomer = !valid && !extrapolated && *own && !*own_only;
    interp_line_pend = -1;
    if ((valid || newcomer) && interp_line_pending_len >= INTERP_LINE_PENDING)
        InterpEvict(IEVICT_LINES);
    if ((valid || newcomer) && interp_line_pending_len < INTERP_LINE_PENDING) {
        InterpLinePending* q = &interp_line_pending[interp_line_pending_len];
        q->which = valid ? 0 : 1;
        q->count = (unsigned char)count;
        q->ox = (short)InterpOffsetX();
        q->oy = (short)InterpOffsetY();
        q->start = q->end = 0;
        for (int i = 0; i < count; i++) {
            q->from[i][0] = valid ? p->position[i][0] : position[i][0];
            q->from[i][1] = valid ? p->position[i][1] : position[i][1];
            q->want[i][0] = valid ? (position[i][0] + p->position[i][0]) * 0.5f
                                  : position[i][0];
            q->want[i][1] = valid ? (position[i][1] + p->position[i][1]) * 0.5f
                                  : position[i][1];
        }
        interp_line_pend = (int)interp_line_pending_len;
    }
    for (int i = 0; i < count; i++) {
        if (valid) {
            mid_x[i] = (int)SDL_floorf(
                (position[i][0] + p->position[i][0]) * 0.5f + 0.5f);
            mid_y[i] = (int)SDL_floorf(
                (position[i][1] + p->position[i][1]) * 0.5f + 0.5f);
        } else if (extrapolated) {
            mid_x[i] = (int)SDL_floorf(
                p->position[i][0] + p->motion[i][0] * 0.5f + 0.5f);
            mid_y[i] = (int)SDL_floorf(
                p->position[i][1] + p->motion[i][1] * 0.5f + 0.5f);
        } else {
            mid_x[i] = (int)position[i][0];
            mid_y[i] = (int)position[i][1];
        }
        if (interp_line_pend >= 0) {
            interp_line_pending[interp_line_pend].at[i][0] = (float)mid_x[i];
            interp_line_pending[interp_line_pend].at[i][1] = (float)mid_y[i];
        }
    }
    if (valid && *own) {
        for (int i = 0; i < count; i++) {
            p->motion[i][0] = position[i][0] - p->position[i][0];
            p->motion[i][1] = position[i][1] - p->position[i][1];
        }
        p->motion_frame = interp_frame;
    } else
        p->motion_frame = 0;
    memcpy(p->position, position, (size_t)count * sizeof(position[0]));
    p->frame = interp_frame;
    p->count = points;
    p->signature = 0;
    p->midpoint_pose = !*own;
    if (valid || extrapolated) {
        ++interp_stat[2];
        ++interp_matches;
    }
    return (valid || extrapolated || newcomer) && !*own_only;
}

static void InterpCapture(const void* packet, Vertex* v, unsigned count,
                          const u32* index, unsigned indices) {
    static int probe = -1;
    interp_capture_index = index;
    interp_capture_indices = indices;
    if (probe < 0)
        probe = SDL_getenv("PSYZ_INTERP_PROBE") != NULL;
    interp_probe_what = "untracked";
    interp_probe_pair = 0;
    interp_probe_flags = 0;
    interp_capture_pending = false;
    InterpCaptureDraw(packet, v, count);
    interp_size_culled = 0;
    if (interpolation_enabled && !interp_line_mode && count <= 6 &&
        v >= vertex_buf && v + count <= vertex_buf + MAX_VERTEX_COUNT) {
        const unsigned base = (unsigned)(v - vertex_buf);
        InterpExposeView view;
        InterpExposeViewNow(&view);
        InterpExposePrimitive(
            0, &view, v, &vertex_depth[base], index, indices, base,
            (geometry_correction || effect_geometry_correction) &&
                internal_res > 1);
        if (!interp_capture_pending)
            InterpExposePrimitive(
                2, &view, interpolated_vertices + base, &interp_mid_depth[base],
                index, indices, base, true);
    }
    if (probe && !interp_line_mode && count <= 6)
        InterpProbe(packet, v, interpolated_vertices + (v - vertex_buf), count,
                    interp_probe_what);
}

/* A screen position the GTE did not clamp to its limit (a projection). */
static bool InterpUnclamped(const float* pos) {
    return pos[0] > -1023.0f && pos[0] < 1022.0f && pos[1] > -1023.0f &&
           pos[1] < 1022.0f;
}

static void InterpCaptureDraw(const void* packet, Vertex* v, unsigned count) {
    unsigned offset = (unsigned)(v - vertex_buf);
    memcpy(interpolated_vertices + offset, v, count * sizeof(*v));
    for (unsigned i = 0; i < count && offset + i < MAX_VERTEX_COUNT; ++i)
        interp_mid_depth[offset + i] = vertex_depth[offset + i];
    for (unsigned i = 0; i < count; ++i)
        if (!((geometry_correction || effect_geometry_correction) &&
              internal_res > 1 && (v[i].t & TPAGE_POLYGON) &&
              v[i].pgxp_pos[2] > 0.0f))
            interpolated_vertices[offset + i].pgxp_pos[2] = -1;
    if (interp_line_mode) {
        /* runs of a line: 1 its own pose only, 2 the midpoint only, 3 both */
        if (interp_line_mode != 3)
            for (unsigned i = 0; i < count; ++i) {
                Vertex* hide = interp_line_mode == 1
                                   ? &interpolated_vertices[offset + i]
                                   : &v[i];
                hide->x = hide->y = -2048;
                hide->pgxp_pos[0] = hide->pgxp_pos[1] = -2048.0f;
                hide->pgxp_pos[2] = -1.0f;
            }
        return;
    }
    if (!interpolation_enabled ||
        (!interpolated_vram && interpolated_scale == internal_res) || count > 6)
        return;
    ++interp_stat[0];
    /* PSYZ_INTERP_DEBUG=1 tints the midpoint's unpaired tagged primitives
     * magenta, =2 also the untagged ones green. */
    static int debug = -1;
    if (debug < 0) {
        const char* d = SDL_getenv("PSYZ_INTERP_DEBUG");
        debug = d ? SDL_atoi(d) : 0;
    }
    InterpAddress* e = InterpFind(interp_queue, packet);
    if (!e)
        e = InterpFindTagged(packet);
    if (!e)
        e = InterpFind(interp_addresses, packet);
    if (debug > 1 &&
        (!e || !e->serial || interp_pairs[e->pair].serial != e->serial))
        for (unsigned i = 0; i < count; ++i) {
            Vertex* out = interpolated_vertices + offset + i;
            out->r = 0;
            out->g = 255;
            out->b = 0;
        }
    if (!e || !e->serial)
        return;
    InterpPair* p = &interp_pairs[e->pair];
    if (p->serial != e->serial)
        return;
    const unsigned flags = p->tagged ? e->flags : 0;
    const bool any_texture = (flags & PSYZ_INTERP_ANY_TEXTURE) != 0;
    const bool midpoint_only = (flags & PSYZ_INTERP_MIDPOINT_ONLY) != 0;
    bool frame_only = (flags & PSYZ_INTERP_FRAME_ONLY) != 0;
    uint64_t signature = 14695981039346656037ULL;
    /* texture coordinates, CLUT and page of a textured primitive (an
     * untextured one leaves them as whatever was drawn before it) */
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char* s = (const void*)&v[i].u;
        if (!(v[i].t & TPAGE_NOTEXTURE))
            for (unsigned j = 0; j < 8; ++j)
                signature = (signature ^ s[j]) * 1099511628211ULL;
    }
    float position[6][3], depth[6];
    for (unsigned i = 0; i < count; ++i) {
        const float d = vertex_depth[offset + i];
        depth[i] = d > 0.0f ? d : 0.0f;
    }
    for (unsigned i = 0; i < count; ++i) {
        bool projected = (geometry_correction || effect_geometry_correction) &&
                         internal_res > 1 && (v[i].t & TPAGE_POLYGON) &&
                         v[i].pgxp_pos[2] > 0;
        float bias =
            (v[i].t & TPAGE_POLYGON) && internal_res == 1 ? 0.5f : 0.0f;
        position[i][0] = projected ? v[i].pgxp_pos[0] : (float)v[i].x + bias;
        position[i][1] = projected ? v[i].pgxp_pos[1] : (float)v[i].y + bias;
        position[i][2] = projected ? v[i].pgxp_pos[2] : 1;
    }
    /* A second packet of an identity in the same frame (the game drew the
     * same face twice): drawn as the first one was. */
    if (p->tagged && p->frame == interp_frame && p->count == count &&
        !memcmp(p->position, position, count * sizeof(position[0]))) {
        for (unsigned i = 0; i < count; ++i) {
            Vertex* out = interpolated_vertices + offset + i;
            if (p->out_mode == 1 && !frame_only) {
                out->pgxp_pos[0] = p->out[i][0];
                out->pgxp_pos[1] = p->out[i][1];
                out->pgxp_pos[2] = p->out[i][2];
            } else if (frame_only) {
                out->x = out->y = -2048;
                out->pgxp_pos[0] = out->pgxp_pos[1] = -2048.0f;
                out->pgxp_pos[2] = -1.0f;
            }
        }
        if (midpoint_only)
            for (unsigned i = 0; i < count; ++i) {
                v[i].x = v[i].y = -2048;
                v[i].pgxp_pos[0] = v[i].pgxp_pos[1] = -2048.0f;
                v[i].pgxp_pos[2] = -1.0f;
            }
        if (p->ghost_frame == interp_frame && p->ghost_head)
            interp_inject = e->pair + 1;
        return;
    }
    bool valid =
        p->frame && p->frame + 1 == interp_frame && p->count == count &&
        (any_texture || p->signature == signature);
    /* a new identity where a ghost goes: the other representation of it */
    const bool replaced =
        p->tagged && !valid && !midpoint_only && !frame_only &&
        !(p->frame && p->frame + 1 == interp_frame) &&
        InterpGhostCovers(position, count);
    if (replaced)
        frame_only = true;
    /* A midpoint-only packet is geometry the game leaves out of the frame
     * (nearer than the projection plane): it pairs only with a pose the
     * game drew in the frame before; two midpoint-only poses are no
     * projections the game would draw (often far off the view). With
     * PSYZ_INTERP_NO_POSE its pose this frame is none either: the midpoint
     * continues the last motion of the drawn pose instead (below). */
    if (midpoint_only && (p->midpoint_pose || (flags & PSYZ_INTERP_NO_POSE)))
        valid = false;
    for (unsigned i = 0; i < count; ++i) {
        /* a tagged identity is the same face: it may move as far as the
         * view reaches (vertices past the GTE's limit are handled below) */
        const float limit =
            p->tagged ? INTERP_PAIR_REACH_TAGGED : INTERP_PAIR_REACH;
        if (SDL_fabsf(position[i][0] - p->position[i][0]) > limit ||
            SDL_fabsf(position[i][1] - p->position[i][1]) > limit)
            valid = false;
    }
    /* An authored animation may switch to another tile entry this frame.
     * Keep that image intact, but move it with the same interpolated camera
     * as its static neighbours so patch boundaries cannot separate. */
    bool camera_fallback = !valid && p->background && interp_camera_previous;
    /* A tagged primitive with a vertex at the GTE's screen limit crosses the
     * view plane: that pose is not a projection. When it was paired the
     * frame before, the midpoint continues its last motion from there. A
     * midpoint-only one that does not pair is left out. */
    bool clamped = midpoint_only && !valid;
    if (!valid && !camera_fallback && p->tagged)
        for (unsigned i = 0; i < count; ++i)
            clamped |= v[i].x <= -1024 || v[i].x >= 1023 || v[i].y <= -1024 ||
                       v[i].y >= 1023;
    const bool extrapolated =
        clamped && !p->midpoint_pose && p->frame &&
        p->frame + 1 == interp_frame &&
        (p->motion_frame == p->frame || midpoint_only) && p->count == count &&
        (any_texture || p->signature == signature);
    /* without a last motion (a level of detail drawn in the frame before
     * only), midpoint-only geometry stays where that frame drew it rather
     * than leaving a hole */
    const float step = p->motion_frame == p->frame ? 0.5f : 0.0f;
    if (p->tagged)
        ++interp_stat[1];
    if (valid || extrapolated)
        ++interp_stat[2];
    interp_probe_pair = e->pair;
    interp_probe_flags = flags;
    if (p->tagged) {
        const int k =
            replaced                  ? ISTAT_REPLACED
            : frame_only              ? ISTAT_FRAME_ONLY
            : (valid || extrapolated) ? ISTAT_PAIRED
            : midpoint_only           ? ISTAT_MID_ONLY
            : (p->frame && p->frame + 1 == interp_frame)
                ? ISTAT_MISMATCH
                : ISTAT_NEW;
        interp_area[k] += InterpArea(position, count);
        ++interp_count[k];
    }
    interp_probe_what =
        !p->tagged                 ? "untagged-pair"
        : valid                    ? "paired"
        : extrapolated             ? "extrapolated"
        : camera_fallback          ? "camera"
        : clamped && midpoint_only ? "hidden"
                                   : "unpaired";
    if (debug > 0 && !valid && !extrapolated && p->tagged)
        for (unsigned i = 0; i < count; ++i) {
            Vertex* out = interpolated_vertices + offset + i;
            out->r = 255;
            out->g = 0;
            out->b = 255;
        }
    if ((valid || camera_fallback || extrapolated) && !frame_only)
        ++interp_matches;
    p->out_mode = 0;
    for (unsigned i = 0; i < count; ++i) {
        Vertex* out = interpolated_vertices + offset + i;
        if (valid && depth[i] > 0.0f && p->depth[i] > 0.0f &&
            InterpUnclamped(position[i]) && InterpUnclamped(p->position[i])) {
            /* the projection of the midpoint of the two view-space
             * positions: their screen positions weighted by depth (the
             * projection centre cancels out); points on a line stay on
             * it, so subdivided pieces meet their neighbours */
            const float w = depth[i] / (depth[i] + p->depth[i]);
            out->pgxp_pos[0] =
                position[i][0] * w + p->position[i][0] * (1.0f - w);
            out->pgxp_pos[1] =
                position[i][1] * w + p->position[i][1] * (1.0f - w);
            out->pgxp_pos[2] =
                position[i][2] > 1.0f && p->position[i][2] > 1.0f
                    ? (depth[i] + p->depth[i]) * 0.5f
                    : 2.0f / (1.0f / position[i][2] + 1.0f / p->position[i][2]);
        } else if (valid) {
            out->pgxp_pos[0] = (position[i][0] + p->position[i][0]) * 0.5f;
            out->pgxp_pos[1] = (position[i][1] + p->position[i][1]) * 0.5f;
            out->pgxp_pos[2] =
                2.0f / (1.0f / position[i][2] + 1.0f / p->position[i][2]);
        } else if (camera_fallback) {
            out->pgxp_pos[0] = position[i][0] + interp_camera_half_x;
            out->pgxp_pos[1] = position[i][1] + interp_camera_half_y;
            out->pgxp_pos[2] = position[i][2];
        } else if (extrapolated) {
            out->pgxp_pos[0] = p->position[i][0] + p->motion[i][0] * step;
            out->pgxp_pos[1] = p->position[i][1] + p->motion[i][1] * step;
            out->pgxp_pos[2] = p->position[i][2];
        } else if (clamped && midpoint_only) {
            out->x = out->y = -2048;
            out->pgxp_pos[0] = out->pgxp_pos[1] = -2048.0f;
            out->pgxp_pos[2] = -1.0f;
        }
        if (valid && depth[i] > 0.0f && p->depth[i] > 0.0f)
            interp_mid_depth[offset + i] = (depth[i] + p->depth[i]) * 0.5f;
        else if (extrapolated || (valid && p->depth[i] > 0.0f))
            interp_mid_depth[offset + i] = p->depth[i];
        if (valid || extrapolated) {
            p->out[i][0] = out->pgxp_pos[0];
            p->out[i][1] = out->pgxp_pos[1];
            p->out[i][2] = out->pgxp_pos[2];
            p->out_mode = 1;
        }
    }
    unsigned field[2][6] = {{0}};
    if (valid && !frame_only)
        for (unsigned i = 0; i < count; ++i) {
            field[0][i] = InterpFieldAdd(
                0, p->position[i], p->depth[i], InterpOffsetX(),
                InterpOffsetY(), interpolated_vertices[offset + i].pgxp_pos);
            field[1][i] = InterpFieldAdd(
                1, position[i], depth[i], InterpOffsetX(), InterpOffsetY(),
                interpolated_vertices[offset + i].pgxp_pos);
        }
    /* T-junction edges and vertices (InterpSnapJunctions) */
    if (valid && !frame_only && (count == 3 || count == 6)) {
        const int ox = InterpOffsetX(), oy = InterpOffsetY();
        /* the polygon's triangles (t: vertices 3t..3t+2), also one the GPU
         * skips for its size (its indices are collapsed) */
        for (unsigned t = 0; t < count; t += 3)
            for (unsigned k = 0; k < 3; ++k) {
                const unsigned a = t + k, b = t + (k + 1) % 3;
                InterpAddEdge(
                    p->position[a], p->position[b], position[a], position[b],
                    interpolated_vertices[offset + a].pgxp_pos,
                    interpolated_vertices[offset + b].pgxp_pos, p->depth[a],
                    p->depth[b], e->pair, ox, oy, !midpoint_only);
            }
        if (interp_junction_prims_len >= INTERP_JUNCTION_PRIMS)
            InterpEvict(IEVICT_JUNCTIONS);
        else {
            InterpJunctionPrim* q =
                &interp_junction_prims[interp_junction_prims_len++];
            q->pair = e->pair;
            q->base = interp_mid_len + offset;
            q->count = (unsigned char)count;
            q->ox = (short)ox;
            q->oy = (short)oy;
            q->posed = !midpoint_only;
            for (unsigned i = 0; i < count; ++i) {
                q->p0[i][0] = p->position[i][0];
                q->p0[i][1] = p->position[i][1];
                q->p1[i][0] = position[i][0];
                q->p1[i][1] = position[i][1];
                q->d0[i] = p->depth[i];
                q->field[0][i] = field[0][i];
                q->field[1][i] = field[1][i];
            }
        }
    }
    /* A new identity, and one whose pose this frame is none (it continues
     * its last motion): placed as their neighbours are in the midpoint
     * (InterpFinishPending), from this frame's pose or the last one. */
    const bool newcomer = p->tagged && !valid && !extrapolated &&
                          !camera_fallback && !frame_only && !midpoint_only &&
                          !(p->frame && p->frame + 1 == interp_frame);
    if ((newcomer || (extrapolated && !frame_only)) &&
        interp_capture_indices <= 6) {
        bool usable = true;
        unsigned char index[6];
        for (unsigned i = 0; i < count; ++i)
            usable &= InterpUnclamped(newcomer ? position[i] : p->position[i]);
        for (unsigned k = 0; k < interp_capture_indices; ++k) {
            const unsigned i = (unsigned)interp_capture_index[k] - offset;
            usable &= i < count;
            index[k] = (unsigned char)i;
        }
        if (usable)
            interp_capture_pending =
                newcomer
                    ? InterpPend(1, e->pair, offset, count,
                                 (const float (*)[3])position, depth, index,
                                 interp_capture_indices, InterpOffsetX(),
                                 InterpOffsetY())
                    : InterpPend(0, e->pair, offset, count,
                                 (const float (*)[3])p->position, p->depth,
                                 index, interp_capture_indices, InterpOffsetX(),
                                 InterpOffsetY());
    }
    /* A triangle the GPU skips for its size in this frame (it passes close
     * to the view) that the frame before drew: the midpoint still draws
     * it, rather than show what it covered there. (Triangle t of a polygon
     * has vertices 3t..3t+2.) */
    unsigned mid_only_triangles = 0;
    static int size_culled = -1;
    if (size_culled < 0) {
        const char* e = SDL_getenv("PSYZ_INTERP_SIZE_CULLED");
        size_culled = !(e && *e == '0');
    }
    if (size_culled && interp_size_culled && (valid || extrapolated) &&
        !frame_only && (count == 3 || count == 6))
        for (unsigned t = 0; t < count / 3; ++t) {
            if (!(interp_size_culled >> t & 1))
                continue;
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (unsigned k = 0; k < 3; ++k) {
                const float* q = p->position[3 * t + k];
                x0 = SDL_min(x0, q[0]);
                x1 = SDL_max(x1, q[0]);
                y0 = SDL_min(y0, q[1]);
                y1 = SDL_max(y1, q[1]);
            }
            if (x1 - x0 < (float)VRAM_W && y1 - y0 < (float)VRAM_H)
                mid_only_triangles |= 1u << t;
        }
    if (valid && !midpoint_only) {
        for (unsigned i = 0; i < count; ++i) {
            p->motion[i][0] = position[i][0] - p->position[i][0];
            p->motion[i][1] = position[i][1] - p->position[i][1];
        }
        p->motion_frame = interp_frame;
    } else
        p->motion_frame = 0;
    memcpy(p->position, position, count * sizeof(position[0]));
    p->frame = interp_frame;
    p->count = count;
    p->signature = signature;
    memcpy(p->depth, depth, count * sizeof(depth[0]));
    p->midpoint_pose = midpoint_only;
    if (p->tagged && !midpoint_only)
        InterpStore(p, e->pair, v, count);
    if (p->tagged && p->ghost_frame == interp_frame && p->ghost_head)
        interp_inject = e->pair + 1;
    if (frame_only)
        for (unsigned i = 0; i < count; ++i) {
            Vertex* out = interpolated_vertices + offset + i;
            out->x = out->y = -2048;
            out->pgxp_pos[0] = out->pgxp_pos[1] = -2048;
            out->pgxp_pos[2] = -1;
        }
    if (midpoint_only)
        for (unsigned i = 0; i < count; ++i) {
            v[i].x = v[i].y = -2048;
            v[i].pgxp_pos[0] = v[i].pgxp_pos[1] = -2048.0f;
            v[i].pgxp_pos[2] = -1.0f;
        }
    for (unsigned t = 0; t < 2; ++t)
        if (mid_only_triangles >> t & 1)
            for (unsigned k = 0; k < 3; ++k) {
                const unsigned i = 3 * t + k;
                index_cur[i] = (u32)(offset + i);
                v[i].x = v[i].y = -2048;
                v[i].pgxp_pos[0] = v[i].pgxp_pos[1] = -2048.0f;
                v[i].pgxp_pos[2] = -1.0f;
            }
}

static int InterpOffsetX(void) { return draw_offset.x; }

static int InterpOffsetY(void) { return draw_offset.y; }

/* The on-screen area of a primitive's pose (vertex space), clipped to the
 * drawing area's bounding box by clamping. */
static float InterpArea(float (*pos)[3], unsigned count) {
    static const unsigned tri[3][2][3] = {
        {{0, 1, 2}, {0, 1, 2}}, {{0, 1, 2}, {2, 1, 3}}, {{0, 1, 2}, {3, 4, 5}}};
    if (count != 3 && count != 4 && count != 6)
        return 0;
    const unsigned k = count == 3 ? 0 : count == 4 ? 1 : 2;
    const float x0 = (float)(draw_area_start.x - draw_offset.x),
                x1 = (float)(draw_area_end.x + 1 - draw_offset.x);
    const float y0 = (float)(draw_area_start.y - draw_offset.y),
                y1 = (float)(draw_area_end.y + 1 - draw_offset.y);
    float area = 0;
    for (unsigned t = 0; t < (count == 3 ? 1u : 2u); ++t) {
        float q[3][2];
        for (unsigned i = 0; i < 3; ++i) {
            q[i][0] = SDL_clamp(pos[tri[k][t][i]][0], x0, x1);
            q[i][1] = SDL_clamp(pos[tri[k][t][i]][1], y0, y1);
        }
        area += SDL_fabsf((q[1][0] - q[0][0]) * (q[2][1] - q[0][1]) -
                          (q[2][0] - q[0][0]) * (q[1][1] - q[0][1])) *
                0.5f;
    }
    return area;
}

static bool InterpGhostEnabled(void) {
    if (interp_ghosts < 0) {
        const char* e = SDL_getenv("PSYZ_INTERP_GHOSTS");
        interp_ghosts = !(e && *e == '0');
    }
    return interp_ghosts > 0;
}

static unsigned short InterpCoverStamp(void) {
    return (unsigned short)(interp_frame & 0xFFFF)
               ? (unsigned short)(interp_frame & 0xFFFF)
               : 1;
}

/* Marks the cells whose centre a triangle (drawing area coordinates) covers. */
static void InterpCoverTriangle(
    const float a[2], const float b[2], const float c[2]) {
    const float area =
        (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    if (SDL_fabsf(area) < 1.0f)
        return;
    const int x0 =
        SDL_max(0, (int)SDL_floorf(
                       SDL_min(a[0], SDL_min(b[0], c[0])) / INTERP_COVER_CELL));
    const int x1 =
        SDL_min(VRAM_W / INTERP_COVER_CELL - 1,
                (int)SDL_floorf(
                    SDL_max(a[0], SDL_max(b[0], c[0])) / INTERP_COVER_CELL));
    const int y0 =
        SDL_max(0, (int)SDL_floorf(
                       SDL_min(a[1], SDL_min(b[1], c[1])) / INTERP_COVER_CELL));
    const int y1 =
        SDL_min(VRAM_H / INTERP_COVER_CELL - 1,
                (int)SDL_floorf(
                    SDL_max(a[1], SDL_max(b[1], c[1])) / INTERP_COVER_CELL));
    const unsigned short stamp = InterpCoverStamp();
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float px = (x + 0.5f) * INTERP_COVER_CELL,
                        py = (y + 0.5f) * INTERP_COVER_CELL;
            const float d1 =
                (b[0] - a[0]) * (py - a[1]) - (px - a[0]) * (b[1] - a[1]);
            const float d2 =
                (c[0] - b[0]) * (py - b[1]) - (px - b[0]) * (c[1] - b[1]);
            const float d3 =
                (a[0] - c[0]) * (py - c[1]) - (px - c[0]) * (a[1] - c[1]);
            if (area > 0 ? (d1 >= 0 && d2 >= 0 && d3 >= 0)
                         : (d1 <= 0 && d2 <= 0 && d3 <= 0))
                interp_ghost_cover[y][x] = stamp;
        }
}

/* Most of a primitive's pose (vertex space) lies where ghosts go. */
static bool InterpGhostCovers(float (*pos)[3], unsigned count) {
    if (!InterpGhostEnabled() || interp_ghost_cut ||
        (count != 3 && count != 4 && count != 6))
        return false;
    const unsigned k = count == 3   ? 0
                       : count == 4 ? 1
                                    : 2,
                   tris = count == 3 ? 1u : 2u;
    const unsigned short stamp = InterpCoverStamp();
    unsigned samples = 0, covered = 0;
    for (unsigned t = 0; t < tris; ++t) {
        const float *a = pos[interp_tri[k][t][0]],
                    *b = pos[interp_tri[k][t][1]],
                    *c = pos[interp_tri[k][t][2]];
        static const float w[4][3] = {{1 / 3.f, 1 / 3.f, 1 / 3.f},
                                      {.6f, .2f, .2f},
                                      {.2f, .6f, .2f},
                                      {.2f, .2f, .6f}};
        for (unsigned i = 0; i < 4; ++i) {
            const int x = (int)SDL_floorf(
                (w[i][0] * a[0] + w[i][1] * b[0] + w[i][2] * c[0] +
                 draw_offset.x - draw_area_start.x) /
                INTERP_COVER_CELL);
            const int y = (int)SDL_floorf(
                (w[i][0] * a[1] + w[i][1] * b[1] + w[i][2] * c[1] +
                 draw_offset.y - draw_area_start.y) /
                INTERP_COVER_CELL);
            ++samples;
            if (x >= 0 && y >= 0 && x < VRAM_W / INTERP_COVER_CELL &&
                y < VRAM_H / INTERP_COVER_CELL &&
                interp_ghost_cover[y][x] == stamp)
                ++covered;
        }
    }
    return covered * 4 >= samples * 3;
}

/* Most of a line's points and segment middles lie where ghosts go. */
static bool InterpGhostCoversLine(float (*pos)[3], int count) {
    if (!InterpGhostEnabled() || interp_ghost_cut || count < 2)
        return false;
    const unsigned short stamp = InterpCoverStamp();
    unsigned samples = 0, covered = 0;
    for (int i = 0; i < count; ++i)
        for (unsigned h = 0; h < (i + 1 < count ? 2u : 1u); ++h) {
            const float x = h ? (pos[i][0] + pos[i + 1][0]) * 0.5f : pos[i][0],
                        y = h ? (pos[i][1] + pos[i + 1][1]) * 0.5f : pos[i][1];
            const int cx = (int)SDL_floorf(
                (x + draw_offset.x - draw_area_start.x) / INTERP_COVER_CELL);
            const int cy = (int)SDL_floorf(
                (y + draw_offset.y - draw_area_start.y) / INTERP_COVER_CELL);
            ++samples;
            /* an outline lies on the ghost's edge: a cell next to it counts */
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; ++dy)
                for (int dx = -1; dx <= 1 && !near; ++dx) {
                    const int u = cx + dx, w = cy + dy;
                    near = u >= 0 && w >= 0 && u < VRAM_W / INTERP_COVER_CELL &&
                           w < VRAM_H / INTERP_COVER_CELL &&
                           interp_ghost_cover[w][u] == stamp;
                }
            covered += near;
        }
    return covered * 4 >= samples * 3;
}

/* Keeps the primitive an identity drew in this frame for a ghost. */
static void InterpStore(
    InterpPair* p, unsigned pair, const Vertex* v, unsigned count) {
    const unsigned b = interp_frame & 1;
    if (p->store_frame == interp_frame || !InterpGhostEnabled())
        return;
    /* semi-transparent shapes (shadows, effects) blend: a ghost of one
     * shows as a patch where nothing was */
    if (count < 3 || count > 6 || interp_capture_indices < 3 ||
        interp_capture_indices > 6 || v[0].exact[0][3] || v[0].a != 0xFF)
        return;
    if (interp_store_len[b] + count > INTERP_STORE) {
        InterpEvict(IEVICT_STORE);
        return;
    }
    if (interp_order_len[b] >= INTERP_ORDER) {
        InterpEvict(IEVICT_ORDER);
        return;
    }
    /* a pose crossing the view plane (a vertex at the GTE's screen limit)
     * or moving faster than a ghost can follow is no pose to continue */
    for (unsigned i = 0; i < count; ++i) {
        if (v[i].x <= -1024 || v[i].x >= 1023 || v[i].y <= -1024 ||
            v[i].y >= 1023)
            return;
        if (p->motion_frame == interp_frame &&
            (SDL_fabsf(p->motion[i][0]) > INTERP_GHOST_MOTION ||
             SDL_fabsf(p->motion[i][1]) > INTERP_GHOST_MOTION))
            return;
    }
    const unsigned base = (unsigned)(v - vertex_buf);
    for (unsigned k = 0; k < interp_capture_indices; ++k) {
        const unsigned i = (unsigned)interp_capture_index[k] - base;
        if (i >= count)
            return;
        p->store_index[k] = (unsigned char)i;
    }
    memcpy(&interp_store[b][interp_store_len[b]], v, count * sizeof(*v));
    p->store_offset = interp_store_len[b];
    interp_store_len[b] += count;
    p->store_count = (unsigned char)count;
    p->store_indices = (unsigned char)interp_capture_indices;
    p->store_offset_x = (short)(draw_offset.x - draw_area_start.x);
    p->store_offset_y = (short)(draw_offset.y - draw_area_start.y);
    p->store_width = (short)(draw_area_end.x - draw_area_start.x);
    p->store_height = (short)(draw_area_end.y - draw_area_start.y);
    p->store_frame = interp_frame;
    interp_order[b][interp_order_len[b]++] = pair;
}

/* The pose of a ghost after a whole frame (step 1) or half (0.5). */
static void InterpGhostPose(
    const InterpPair* g, unsigned i, float step, float out[2]) {
    const bool moving = g->motion_frame && g->motion_frame == g->frame;
    out[0] = g->position[i][0] + (moving ? g->motion[i][0] * step : 0);
    out[1] = g->position[i][1] + (moving ? g->motion[i][1] * step : 0);
}

/* After the frame's tags (Psyz_GpuBeginInterpolationFrame): the identities
 * the frame before drew and this one leaves out become ghosts. */
static void InterpBuildGhosts(void) {
    const unsigned f = interp_frame - 1, b = f & 1, n = interp_order_len[b];
    unsigned vanished = 0, last = 0, tail = 0;
    interp_inject = interp_ghost_first = 0;
    interp_ghost_cut = false;
    if (!InterpGhostEnabled() || !n)
        return;
    for (unsigned k = 0; k < n; ++k) {
        const InterpPair* p = &interp_pairs[interp_order[b][k]];
        if (p->store_frame == f && p->tagged_frame != interp_frame)
            ++vanished;
    }
    if (interp_stats_log > 0)
        SDL_Log("[ghost-build] f=%u order=%u store=%u vanished=%u",
                interp_frame, n, interp_store_len[b], vanished);
    /* most of the picture is new: a cut, which nothing pairs across */
    if (vanished * 2 > n) {
        interp_ghost_cut = true;
        return;
    }
    for (unsigned k = 0; k < n; ++k) {
        const unsigned q = interp_order[b][k];
        InterpPair* p = &interp_pairs[q];
        if (p->store_frame != f)
            continue;
        if (p->tagged_frame == interp_frame) {
            p->ghost_frame = interp_frame;
            p->ghost_head = 0;
            last = q + 1;
            tail = 0;
            continue;
        }
        p->ghost_next = 0;
        if (tail)
            interp_pairs[tail - 1].ghost_next = q + 1;
        else if (last)
            interp_pairs[last - 1].ghost_head = q + 1;
        else
            interp_ghost_first = q + 1;
        tail = q + 1;
        const unsigned c = p->store_count, kk = c == 3 ? 0 : c == 4 ? 1 : 2;
        for (unsigned t = 0; t < (c == 3 ? 1u : 2u); ++t) {
            float a[3][2];
            for (unsigned i = 0; i < 3; ++i) {
                InterpGhostPose(p, interp_tri[kk][t][i], 1.0f, a[i]);
                a[i][0] += p->store_offset_x;
                a[i][1] += p->store_offset_y;
            }
            InterpCoverTriangle(a[0], a[1], a[2]);
        }
    }
}

/* PSYZ_INTERP_PROBE=x,y[,first,last]: log every primitive whose frame or
 * midpoint pose covers draw-area pixel x,y (interpolation frames first..last).
 */
static bool InterpProbeCovers(
    const Vertex* v, unsigned count, bool precise, float px, float py) {
    static const unsigned tri[3][2][3] = {
        {{0, 1, 2}, {0, 1, 2}}, {{0, 1, 2}, {2, 1, 3}}, {{0, 1, 2}, {3, 4, 5}}};
    if (count != 3 && count != 4 && count != 6)
        return false;
    const unsigned k = count == 3 ? 0 : count == 4 ? 1 : 2;
    for (unsigned t = 0; t < 2; ++t) {
        const Vertex *a = &v[tri[k][t][0]], *b = &v[tri[k][t][1]],
                     *c = &v[tri[k][t][2]];
        float ax = precise && a->pgxp_pos[2] > 0 ? a->pgxp_pos[0] : a->x,
              ay = precise && a->pgxp_pos[2] > 0 ? a->pgxp_pos[1] : a->y;
        float bx = precise && b->pgxp_pos[2] > 0 ? b->pgxp_pos[0] : b->x,
              by = precise && b->pgxp_pos[2] > 0 ? b->pgxp_pos[1] : b->y;
        float cx = precise && c->pgxp_pos[2] > 0 ? c->pgxp_pos[0] : c->x,
              cy = precise && c->pgxp_pos[2] > 0 ? c->pgxp_pos[1] : c->y;
        float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by),
              d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy),
              d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
        bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
        if ((ax - bx) * (cy - by) != (ay - by) * (cx - bx) && !(neg && pos))
            return true;
    }
    return false;
}

static void InterpProbe(const void* packet, const Vertex* v, const Vertex* mid,
                        unsigned count, const char* what) {
    static int px = -1, py, first = 0, last = 0x7FFFFFFF;
    static bool init, all;
    if (!init) {
        init = true;
        if (SDL_sscanf(SDL_getenv("PSYZ_INTERP_PROBE"), "%d,%d,%d,%d", &px, &py,
                       &first, &last) < 2)
            px = -2;
        else if (px == -1)
            all = true, px = 0;
    }
    if (px < 0 || (int)interp_frame < first || (int)interp_frame > last)
        return;
    const float x = (float)(px + draw_area_start.x - draw_offset.x) + 0.5f,
                y = (float)(py + draw_area_start.y - draw_offset.y) + 0.5f;
    const bool own = InterpProbeCovers(v, count, true, x, y),
               in_mid = InterpProbeCovers(mid, count, true, x, y);
    if (!own && !in_mid && !all)
        return;
    char line[768];
    int n = SDL_snprintf(
        line, sizeof(line),
        "[probe] f=%u %p id=%llx fl=%u %s%s%s n=%u rgb=%d,%d,%d t=%04x",
        interp_frame, packet, interp_pairs[interp_probe_pair].id,
        interp_probe_flags, what, own ? " own" : "", in_mid ? " mid" : "",
        count, v->r, v->g, v->b, (unsigned)v->t);
    for (unsigned i = 0; i < count && n < (int)sizeof(line) - 40; ++i)
        n += SDL_snprintf(
            line + n, sizeof(line) - (size_t)n, " (%.1f,%.1f|%.1f,%.1f)",
            v[i].pgxp_pos[2] > 0 ? v[i].pgxp_pos[0] : v[i].x,
            v[i].pgxp_pos[2] > 0 ? v[i].pgxp_pos[1] : v[i].y,
            mid[i].pgxp_pos[2] > 0 ? mid[i].pgxp_pos[0] : mid[i].x,
            mid[i].pgxp_pos[2] > 0 ? mid[i].pgxp_pos[1] : mid[i].y);
    if (v >= vertex_buf && v + count <= vertex_buf + MAX_VERTEX_COUNT) {
        n += SDL_snprintf(line + n, sizeof(line) - (size_t)n, " d=");
        for (unsigned i = 0; i < count && n < (int)sizeof(line) - 12; ++i)
            n += SDL_snprintf(line + n, sizeof(line) - (size_t)n, "%.0f,",
                              vertex_depth[v - vertex_buf + i]);
    }
    SDL_Log("%s", line);
}

/* Exposure grids (interp_expose): a triangle at cell centres, positions
 * relative to the frame's buffer, view depths (0 for none: 2D, in front). */
static void InterpExposeTriangle(
    unsigned grid, const float* a, const float* b, const float* c, float da,
    float db, float dc, const int clip[4]) {
    const float area =
        (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    if (!(SDL_fabsf(area) >= 0.25f))
        return;
    const float lx =
                    SDL_max(SDL_min(a[0], SDL_min(b[0], c[0])), (float)clip[0]),
                hx =
                    SDL_min(SDL_max(a[0], SDL_max(b[0], c[0])), (float)clip[2]);
    const float ly =
                    SDL_max(SDL_min(a[1], SDL_min(b[1], c[1])), (float)clip[1]),
                hy =
                    SDL_min(SDL_max(a[1], SDL_max(b[1], c[1])), (float)clip[3]);
    if (!(hx >= lx && hy >= ly && hx >= 0.0f && hy >= 0.0f &&
          lx < (float)VRAM_W && ly < (float)VRAM_H))
        return;
    /* the cells whose centre lies in the drawing area */
    const int x0 = SDL_max(0, (int)SDL_ceilf(lx / INTERP_EXPOSE_CELL - 0.5f)),
              x1 = SDL_min(INTERP_EXPOSE_W - 1,
                           (int)SDL_floorf(hx / INTERP_EXPOSE_CELL - 0.5f));
    const int y0 = SDL_max(0, (int)SDL_ceilf(ly / INTERP_EXPOSE_CELL - 0.5f)),
              y1 = SDL_min(INTERP_EXPOSE_H - 1,
                           (int)SDL_floorf(hy / INTERP_EXPOSE_CELL - 0.5f));
    const bool deep = da > 0.0f && db > 0.0f && dc > 0.0f;
    const float ia = deep ? 1.0f / da : 0, ib = deep ? 1.0f / db : 0,
                ic = deep ? 1.0f / dc : 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float px = (x + 0.5f) * INTERP_EXPOSE_CELL,
                        py = (y + 0.5f) * INTERP_EXPOSE_CELL;
            const float wa =
                (c[0] - b[0]) * (py - b[1]) - (px - b[0]) * (c[1] - b[1]);
            const float wb =
                (a[0] - c[0]) * (py - c[1]) - (px - c[0]) * (a[1] - c[1]);
            const float wc = area - wa - wb;
            if (area > 0 ? (wa < 0 || wb < 0 || wc < 0)
                         : (wa > 0 || wb > 0 || wc > 0))
                continue;
            const float depth =
                deep ? area / (wa * ia + wb * ib + wc * ic) : 0.0f;
            InterpExposeCell* e = &interp_expose[grid][y][x];
            if (e->frame != interp_frame || depth < e->depth) {
                e->frame = interp_frame;
                e->depth = depth;
            }
        }
}

static void InterpExposeViewNow(InterpExposeView* view) {
    const unsigned slot = interp_frame & 1;
    const bool wide = WidescreenDraw();
    view->ox = (float)draw_offset.x;
    view->oy = (float)draw_offset.y;
    view->squeeze =
        wide ? WidescreenPictureSqueeze(WidescreenDrawPicture()) : 1.0f;
    view->centre = wide ? WidescreenCentre() : 0.0f;
    /* the drawing area (inclusive), relative to the frame's buffer */
    view->clip[0] = draw_area_start.x - interp_expose_rect[slot].x;
    view->clip[1] = draw_area_start.y - interp_expose_rect[slot].y;
    view->clip[2] = draw_area_end.x + 1 - interp_expose_rect[slot].x;
    view->clip[3] = draw_area_end.y + 1 - interp_expose_rect[slot].y;
}

/* The opaque triangles of a queued primitive (vertices at base, their view
 * depths at depth) into the frame's grid (0) or the midpoint's (2). */
static void InterpExposePrimitive(
    unsigned grid, const InterpExposeView* view, const Vertex* v,
    const float* depth, const u32* index, unsigned indices, unsigned base,
    bool precise) {
    const unsigned slot = interp_frame & 1;
    if (interp_expose_rect[slot].frame != interp_frame || v[0].a != 0xFF ||
        indices < 3)
        return;
    for (unsigned t = 0; t + 3 <= indices; t += 3) {
        float q[3][2], d[3];
        bool hidden = false;
        for (unsigned k = 0; k < 3; ++k) {
            const unsigned i = index[t + k];
            if (i < base || i >= base + 6)
                return;
            const Vertex* u = &v[i - base];
            const bool p = precise && u->pgxp_pos[2] > 0.0f;
            const float vx = p ? u->pgxp_pos[0] : (float)u->x,
                        vy = p ? u->pgxp_pos[1] : (float)u->y;
            hidden |= vx <= -2047.0f && vy <= -2047.0f;
            float x = vx + view->ox;
            if (view->squeeze != 1.0f)
                x = view->centre + (x - view->centre) * view->squeeze;
            q[k][0] = x - (float)interp_expose_rect[slot].x;
            q[k][1] = vy + view->oy - (float)interp_expose_rect[slot].y;
            d[k] = depth[i - base] > 0.0f ? depth[i - base] : 0.0f;
        }
        if (!hidden)
            InterpExposeTriangle(grid ? 2u : slot, q[0], q[1], q[2], d[0], d[1],
                                 d[2], view->clip);
    }
}

/* The cells of the midpoint of the last frame that show something much
 * farther (or nothing) where it and the frame before show something, and
 * whose four neighbours do too: an area at least two cells across, not the
 * sub-cell shift of an edge between near and far. */
static unsigned InterpExposed(void) {
    static unsigned char mask[INTERP_EXPOSE_H][INTERP_EXPOSE_W];
    const unsigned now = interp_frame & 1, before = now ^ 1;
    interp_exposed_holes = 0;
    if (interp_expose_rect[now].frame != interp_frame ||
        interp_expose_rect[before].frame + 1 != interp_frame ||
        interp_expose_rect[now].w != interp_expose_rect[before].w ||
        interp_expose_rect[now].h != interp_expose_rect[before].h)
        return 0;
    const int w = SDL_min(INTERP_EXPOSE_W,
                          interp_expose_rect[now].w / INTERP_EXPOSE_CELL),
              h = SDL_min(INTERP_EXPOSE_H,
                          interp_expose_rect[now].h / INTERP_EXPOSE_CELL);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const InterpExposeCell *a = &interp_expose[before][y][x],
                                   *b = &interp_expose[now][y][x],
                                   *m = &interp_expose[2][y][x];
            const float nearest = SDL_max(a->depth, b->depth);
            mask[y][x] =
                a->frame + 1 == interp_frame && b->frame == interp_frame &&
                (m->frame != interp_frame ||
                 (nearest > 0.0f && m->depth > nearest * INTERP_EXPOSE_DEPTH));
            interp_exposed_holes += mask[y][x] && m->frame != interp_frame;
        }
    static int dump = -1;
    if (dump < 0)
        dump = SDL_getenv("PSYZ_INTERP_EXPOSE_DUMP")
                   ? SDL_atoi(SDL_getenv("PSYZ_INTERP_EXPOSE_DUMP"))
                   : 0;
    unsigned exposed = 0;
    for (int y = 1; y + 1 < h; ++y)
        for (int x = 1; x + 1 < w; ++x)
            if (mask[y][x] && mask[y - 1][x] && mask[y + 1][x] &&
                mask[y][x - 1] && mask[y][x + 1]) {
                ++exposed;
                if (dump && (int)interp_frame == dump) {
                    const InterpExposeCell *a = &interp_expose[before][y][x],
                                           *b = &interp_expose[now][y][x],
                                           *m = &interp_expose[2][y][x];
                    SDL_Log("[expose] %d,%d a=%.0f b=%.0f m=%.0f%s",
                            x * INTERP_EXPOSE_CELL, y * INTERP_EXPOSE_CELL,
                            a->depth, b->depth, m->depth,
                            m->frame != interp_frame ? " none" : "");
                }
            }
    return exposed;
}

/* Draw_EnqueueBuffer: the ghosts that follow the primitive just queued. */
static void InterpInjectGhosts(unsigned h) {
    const unsigned f = interp_frame - 1, b = f & 1;
    while (h) {
        InterpPair* g = &interp_pairs[h - 1];
        h = g->ghost_next;
        /* another view (the drawing area of a buffer moves with it) */
        if (g->store_frame != f || g->tagged_frame == interp_frame ||
            g->store_offset_x != draw_offset.x - draw_area_start.x ||
            g->store_offset_y != draw_offset.y - draw_area_start.y ||
            g->store_width != draw_area_end.x - draw_area_start.x ||
            g->store_height != draw_area_end.y - draw_area_start.y)
            continue;
        const unsigned n = g->store_count, ni = g->store_indices;
        const Vertex* src = &interp_store[b][g->store_offset];
        /* the CLUT cache row of the frame before is gone: load it again */
        const bool textured = !(src[0].t & TPAGE_NOTEXTURE);
        if (textured)
            ClutCacheUpdate(src[0].t, src[0].c);
        Draw_EnsureBufferWillNotOverflow((int)n, (int)ni);
        const unsigned base = n_vertices;
        InterpPend(0, (unsigned)(g - interp_pairs), base, n, g->position,
                   g->depth, g->store_index, ni, InterpOffsetX(),
                   InterpOffsetY());
        for (unsigned i = 0; i < n; ++i) {
            Vertex* out = &interpolated_vertices[base + i];
            float pose[2];
            *out = src[i];
            InterpGhostPose(g, i, 0.5f, pose);
            out->pgxp_pos[0] = pose[0];
            out->pgxp_pos[1] = pose[1];
            out->pgxp_pos[2] = g->position[i][2] > 0 ? g->position[i][2] : 1.0f;
            if (textured)
                out->exact[4][3] = clut_cache_row;
            vertex_cur[i] = src[i];
            vertex_cur[i].x = vertex_cur[i].y = -2048;
            vertex_cur[i].pgxp_pos[0] = vertex_cur[i].pgxp_pos[1] = -2048.0f;
            vertex_cur[i].pgxp_pos[2] = -1.0f;
        }
        for (unsigned k = 0; k < ni; ++k)
            index_cur[k] = (u32)(base + g->store_index[k]);
        if (textured)
            batch_has_texture = true;
        vertex_cur += n;
        index_cur += ni;
        n_vertices += n;
        n_indices += (int)ni;
        interp_area[ISTAT_GHOST] += InterpArea(g->position, n);
        ++interp_count[ISTAT_GHOST];
        static int probe = -1;
        if (probe < 0)
            probe = SDL_getenv("PSYZ_INTERP_PROBE") != NULL;
        if (probe) {
            interp_probe_pair = (unsigned)(g - interp_pairs);
            interp_probe_flags = 0;
            InterpProbe(NULL, &vertex_buf[base], &interpolated_vertices[base],
                        n, "ghost");
        }
    }
}

static unsigned InterpEdgeCell(int cx, int cy, int ox, int oy) {
    uint32_t h = (uint32_t)cx * 0x9E3779B1u ^ (uint32_t)cy * 0x85EBCA77u ^
                 (uint32_t)(ox * 977 + oy) * 0xC2B2AE3Du;
    return (h ^ h >> 16) & (INTERP_EDGE_CELLS - 1);
}

static void InterpAddEdge(
    const float* a0, const float* b0, const float* a1, const float* b1,
    const float* am, const float* bm, float da, float db, unsigned pair, int ox,
    int oy, bool posed) {
    if (interp_edges_frame != interp_frame) {
        interp_edges_frame = interp_frame;
        interp_edges_len = interp_edge_refs_len = 0;
    }
    const float lx = SDL_min(a0[0], b0[0]), hx = SDL_max(a0[0], b0[0]),
                ly = SDL_min(a0[1], b0[1]), hy = SDL_max(a0[1], b0[1]);
    if (!(lx > -2048.0f && hx < 2048.0f && ly > -2048.0f && hy < 2048.0f))
        return;
    if (interp_edges_len >= INTERP_EDGES) {
        InterpEvict(IEVICT_EDGES);
        return;
    }
    const int x0 = (int)SDL_floorf(
                  (lx - INTERP_JUNCTION_REACH) / INTERP_EDGE_CELL),
              x1 = (int)SDL_floorf(
                  (hx + INTERP_JUNCTION_REACH) / INTERP_EDGE_CELL);
    const int y0 = (int)SDL_floorf(
                  (ly - INTERP_JUNCTION_REACH) / INTERP_EDGE_CELL),
              y1 = (int)SDL_floorf(
                  (hy + INTERP_JUNCTION_REACH) / INTERP_EDGE_CELL);
    if ((x1 - x0 + 1) * (y1 - y0 + 1) > 96)
        return;
    if (interp_edge_refs_len + (unsigned)((x1 - x0 + 1) * (y1 - y0 + 1)) >
        INTERP_EDGE_REFS) {
        InterpEvict(IEVICT_EDGE_REFS);
        return;
    }
    const unsigned e = interp_edges_len++;
    InterpEdge* g = &interp_edges[e];
    memcpy(g->a0, a0, sizeof(g->a0));
    memcpy(g->b0, b0, sizeof(g->b0));
    memcpy(g->a1, a1, sizeof(g->a1));
    memcpy(g->b1, b1, sizeof(g->b1));
    memcpy(g->am, am, sizeof(g->am));
    memcpy(g->bm, bm, sizeof(g->bm));
    g->da = da;
    g->db = db;
    g->pair = pair;
    g->ox = (short)ox;
    g->oy = (short)oy;
    g->posed = posed;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const unsigned c = InterpEdgeCell(x, y, ox, oy);
            if (interp_edge_stamp[c] != interp_frame) {
                interp_edge_stamp[c] = interp_frame;
                interp_edge_head[c] = 0;
            }
            interp_edge_refs[interp_edge_refs_len] =
                (InterpEdgeRef){e, interp_edge_head[c]};
            interp_edge_head[c] = ++interp_edge_refs_len;
        }
}

/* Where p lies along segment a-b (0..1) and how far from its line. */
static float InterpAlong(
    const float* p, const float* a, const float* b, float* distance) {
    const float dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
    if (!(l2 > 1.0f)) {
        *distance = 1e9f;
        return -1.0f;
    }
    const float t = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2;
    *distance =
        SDL_fabsf((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / SDL_sqrtf(l2);
    return t;
}

/* InterpMidpointSync: the first n queued paired primitives' vertices that
 * lie on another paired primitive's edge in both frames go onto the edge's
 * midpoint. */
static void InterpSnapJunctions(unsigned n) {
    static int snap = -1;
    if (snap < 0) {
        const char* e = SDL_getenv("PSYZ_INTERP_JUNCTIONS");
        snap = !(e && *e == '0');
    }
    if (!snap || interp_edges_frame != interp_frame)
        return;
    /* how far off the edge a vertex on it lies: the projection's precision
     * (PGXP, or whole pixels); a vertex farther off belongs to another
     * surface next to the edge (the faces of a thin beam) */
    const float reach =
        (geometry_correction || effect_geometry_correction) && internal_res > 1
            ? INTERP_JUNCTION_PRECISE
            : INTERP_JUNCTION_PIXELS;
    for (unsigned k = 0; k < n; ++k) {
        const InterpJunctionPrim* q = &interp_junction_prims[k];
        for (unsigned i = 0; i < q->count; ++i) {
            const float *v0 = q->p0[i], *v1 = q->p1[i];
            const int cx = (int)SDL_floorf(v0[0] / INTERP_EDGE_CELL),
                      cy = (int)SDL_floorf(v0[1] / INTERP_EDGE_CELL);
            const unsigned c = InterpEdgeCell(cx, cy, q->ox, q->oy);
            if (interp_edge_stamp[c] != interp_frame)
                continue;
            float best = 1e9f;
            const InterpEdge* on = NULL;
            for (unsigned r = interp_edge_head[c]; r;
                 r = interp_edge_refs[r - 1].next) {
                const InterpEdge* g =
                    &interp_edges[interp_edge_refs[r - 1].edge];
                if (g->pair == q->pair || g->ox != q->ox || g->oy != q->oy)
                    continue;
                float d0, d1;
                const float t0 = InterpAlong(v0, g->a0, g->b0, &d0),
                            t1 = InterpAlong(v1, g->a1, g->b1, &d1);
                /* a midpoint-only pose in this frame (nearer than the
                 * projection plane) is no projection to test */
                const bool both = q->posed && g->posed;
                /* with one pose to test (geometry passing the view, often a
                 * long edge) the direction of a long edge counts less */
                const float l0 =
                    SDL_sqrtf((g->b0[0] - g->a0[0]) * (g->b0[0] - g->a0[0]) +
                              (g->b0[1] - g->a0[1]) * (g->b0[1] - g->a0[1]));
                if (!(t0 > 0.0f && t0 < 1.0f &&
                      d0 < reach + (both ? 0.0f : l0 * INTERP_JUNCTION_SLOPE) &&
                      (!both || (t1 > 0.0f && t1 < 1.0f && d1 < reach))))
                    continue;
                /* not one of its ends (a shared vertex is joined already) */
                if (t0 * l0 < 0.5f || (1.0f - t0) * l0 < 0.5f)
                    continue;
                /* at the edge's depth there (not something in front or behind)
                 */
                if (q->d0[i] > 0.0f && g->da > 0.0f && g->db > 0.0f) {
                    const float z = 1.0f / ((1.0f - t0) / g->da + t0 / g->db);
                    if (SDL_fabsf(z - q->d0[i]) > 0.1f * SDL_max(z, q->d0[i]))
                        continue;
                }
                if (d0 + (both ? d1 : d0) < best) {
                    best = d0 + (both ? d1 : d0);
                    on = g;
                }
            }
            if (!on)
                continue;
            Vertex* out = &interp_mid_store[q->base + i];
            if (!(out->pgxp_pos[2] > 0.0f))
                continue;
            float d;
            const float t = InterpAlong(out->pgxp_pos, on->am, on->bm, &d);
            if (!(t > 0.0f && t < 1.0f) || d > INTERP_JUNCTION_MOVE)
                continue;
            out->pgxp_pos[0] = on->am[0] + (on->bm[0] - on->am[0]) * t;
            out->pgxp_pos[1] = on->am[1] + (on->bm[1] - on->am[1]) * t;
            /* what moves with the vertex (a ghost or a new identity sharing
             * it, InterpFinishPending) goes there too */
            for (unsigned w = 0; w < 2; ++w)
                if (q->field[w][i] && interp_field[w].frame == interp_frame &&
                    q->field[w][i] <= interp_field[w].len) {
                    InterpFieldEntry* f =
                        &interp_field[w].entry[q->field[w][i] - 1];
                    f->dx = out->pgxp_pos[0] - f->x;
                    f->dy = out->pgxp_pos[1] - f->y;
                }
        }
    }
}

/* InterpMidpointSync: the first pending ghosts and new identities move as
 * the primitives next to them did (InterpFieldMotion), in the order queued,
 * so one next to another follows it; vertices with none in reach move as
 * the primitive's others, a primitive with none keeps its pose (a ghost its
 * continued motion, a new identity this frame's pose). */
static void InterpFinishPending(unsigned pending) {
    static int field = -1;
    if (field < 0) {
        const char* e = SDL_getenv("PSYZ_INTERP_FIELD");
        field = !(e && *e == '0');
    }
    static int probe = -1;
    if (probe < 0)
        probe = SDL_getenv("PSYZ_INTERP_PROBE") != NULL;
    for (unsigned k = 0; k < pending; ++k) {
        const InterpPending* q = &interp_pending[k];
        const unsigned base = q->base, n = q->count;
        float motion[6][2], shift[2] = {0, 0}, weight = 0;
        unsigned found = 0;
        for (unsigned i = 0; i < n; ++i) {
            float d;
            if (!field || !InterpFieldMotion(q->which, q->from[i], q->depth[i],
                                             q->ox, q->oy, motion[i], &d))
                continue;
            const float w = 1.0f / (1.0f + d);
            shift[0] += motion[i][0] * w;
            shift[1] += motion[i][1] * w;
            weight += w;
            found |= 1u << i;
        }
        for (unsigned i = 0; i < n; ++i) {
            Vertex* out = &interp_mid_store[base + i];
            /* a ghost's vertex with no neighbour keeps its continued
             * motion; a new identity's moves as its neighbours do */
            if ((found >> i & 1) || (found && q->which == 1)) {
                const float* m =
                    (found >> i & 1) ? motion[i]
                                     : (const float[2]){shift[0] / weight,
                                                        shift[1] / weight};
                out->pgxp_pos[0] = q->from[i][0] + m[0];
                out->pgxp_pos[1] = q->from[i][1] + m[1];
                if (!(out->pgxp_pos[2] > 0.0f))
                    out->pgxp_pos[2] = 1.0f;
            }
            InterpFieldAdd(
                q->which, q->from[i], q->depth[i], q->ox, q->oy, out->pgxp_pos);
            interp_mid_depth_store[base + i] = q->depth[i];
        }
        u32 index[6];
        for (unsigned i = 0; i < q->indices; ++i)
            index[i] = base + q->index[i];
        InterpExposePrimitive(
            2, &q->view, &interp_mid_store[base], &interp_mid_depth_store[base],
            index, q->indices, base, true);
        /* (the frame's vertices are drawn: "own" is the midpoint pose too) */
        if (probe) {
            interp_probe_pair = q->pair;
            interp_probe_flags = found;
            InterpProbe(NULL, &interp_mid_store[base], &interp_mid_store[base],
                        n, q->which ? "new-final" : "ghost-final");
        }
    }
}

static void InterpEnqueued(void) {
    ++batch_primitives;
    if (!interp_inject && !interp_ghost_first)
        return;
    if (!interpolation_enabled || !interpolated_vram ||
        interpolated_scale != internal_res || interp_line_mode) {
        interp_inject = 0;
        return;
    }
    if (interp_ghost_first) {
        const unsigned h = interp_ghost_first;
        interp_ghost_first = 0;
        InterpInjectGhosts(h);
    }
    if (interp_inject) {
        InterpPair* p = &interp_pairs[interp_inject - 1];
        const unsigned h = p->ghost_head;
        p->ghost_head = 0;
        interp_inject = 0;
        InterpInjectGhosts(h);
    }
}

/* InterpMidpointSync: the points of the first n queued lines (in the
 * store's first vertices) go where the polygon vertices they lie at went
 * (within INTERP_LINE_REACH pixels): a paired line's by at most a pixel and a
 * half from the midpoint of their own poses (not to another surface at the same
 * place), a new line's only when every point lies at one. The others keep the
 * line's own placement (a paired line the midpoint of its poses, a new one this
 * frame's pose). Each segment is a quad (DrawLineQuad), its first two
 * vertices at the segment's first point. */
static void InterpFinishLines(unsigned n, unsigned vertices) {
    for (unsigned k = 0; k < n; ++k) {
        const InterpLinePending* q = &interp_line_pending[k];
        if (q->end <= q->start || q->end > vertices ||
            q->end - q->start != 4u * (q->count - 1u))
            continue;
        float to[INTERP_LINE_POINTS][2];
        unsigned found = 0;
        for (unsigned i = 0; i < q->count; ++i) {
            float m[2], d;
            to[i][0] = q->want[i][0];
            to[i][1] = q->want[i][1];
            if (!InterpFieldMotion(
                    q->which, q->from[i], 0.0f, q->ox, q->oy, m, &d) ||
                d > INTERP_LINE_REACH * INTERP_LINE_REACH)
                continue;
            const float x = q->from[i][0] + m[0], y = q->from[i][1] + m[1];
            if (q->which == 0 && (SDL_fabsf(x - q->want[i][0]) > 1.5f ||
                                  SDL_fabsf(y - q->want[i][1]) > 1.5f))
                continue;
            to[i][0] = x;
            to[i][1] = y;
            ++found;
        }
        if (q->which == 1 && found < q->count)
            for (unsigned i = 0; i < q->count; ++i) {
                to[i][0] = q->want[i][0];
                to[i][1] = q->want[i][1];
            }
        /* at 1x at whole pixels, as the line is drawn */
        if (internal_res <= 1)
            for (unsigned i = 0; i < q->count; ++i) {
                to[i][0] = SDL_floorf(to[i][0] + 0.5f);
                to[i][1] = SDL_floorf(to[i][1] + 0.5f);
            }
        for (unsigned v = q->start; v < q->end; ++v) {
            Vertex* out = &interp_mid_store[v];
            const unsigned i =
                (v - q->start) / 4u + ((v - q->start) % 4u >= 2u);
            out->pgxp_pos[0] = (float)out->x - q->at[i][0] + to[i][0];
            out->pgxp_pos[1] = (float)out->y - q->at[i][1] + to[i][1];
            out->pgxp_pos[2] = 1.0f;
        }
    }
}

static bool InterpMidpointDeferred(void) { return interp_mid_batches_len != 0; }

static bool InterpMidpointCovers(int x, int y, int w, int h) {
    return interp_mid_batches_len &&
           VramTilesHitRect(&interp_mid_target_tiles, x, y, w, h);
}

static bool InterpMidpointSamplesRect(int x, int y, int w, int h) {
    return interp_mid_batches_len &&
           VramTilesHitRect(&interp_mid_texture_tiles, x, y, w, h);
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
    while (interp_pending_len &&
           interp_pending[interp_pending_len - 1].base >= kept)
        --interp_pending_len;
    while (interp_junction_prims_len &&
           interp_junction_prims[interp_junction_prims_len - 1].base >= kept)
        --interp_junction_prims_len;
    while (interp_line_pending_len &&
           interp_line_pending[interp_line_pending_len - 1].start >= kept)
        --interp_line_pending_len;
}

/* Draw_FlushBuffer, after the frame's targets: the midpoint keeps the
 * batch (n_marks drawing states in draw_marks). */
static void InterpMidpointRecord(
    int n_marks, SDL_GPUTexture* extra_texture, SDL_GPUSampler* extra_sampler,
    bool grid_same) {
    if (interp_mid_batches_len >= INTERP_MID_BATCHES ||
        interp_mid_marks_len + (unsigned)n_marks > INTERP_MID_MARKS)
        InterpMidpointSync(INTERP_SYNC_FULL);
    if (!interp_mid_batches_len)
        interp_mid_clut_loads = clut_cache_loads;
    InterpMidBatch* b = &interp_mid_batches[interp_mid_batches_len++];
    *b = (InterpMidBatch){interp_mid_len,       n_vertices,
                          interp_mid_index_len, (unsigned)n_indices,
                          interp_mid_marks_len, extra_texture,
                          extra_sampler,        grid_same};
    memcpy(&interp_mid_index[interp_mid_index_len], index_buf,
           sizeof(*index_buf) * (size_t)n_indices);
    const unsigned target_index = scaled_vram_render ? 2u : 1u;
    for (int k = 0; k < n_marks; ++k) {
        ApplyDrawState(&draw_marks[k].state);
        ComputeDrawView(&interp_mid_views[interp_mid_marks_len + k],
                        target_index, true, internal_res);
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
    if (!interpolation_enabled || !interpolated_vram ||
        interpolated_scale != internal_res)
        return true; /* no target to draw into */
    const unsigned target_index = scaled_vram_render ? 2u : 1u;
    for (unsigned k = 0; k < interp_mid_batches_len; ++k) {
        const InterpMidBatch* b = &interp_mid_batches[k];
        if (!b->indices)
            continue;
        Uint32 vertex_at, index_at;
        const bool cycle =
            RingReserve(b->vertices, b->indices, &vertex_at, &index_at);
        SDL_GPUCommandBuffer* cmd = AcquireCmd();
        if (!cmd)
            return false;
        u8* map = SDL_MapGPUTransferBuffer(device, vtx_transfer, cycle);
        if (!map) {
            GpuFail("interpolation transfer map", false);
            return false;
        }
        const Uint32 vtx_size = (Uint32)(sizeof(Vertex) * b->vertices);
        const Uint32 idx_size =
            (Uint32)(sizeof(*interp_mid_index) * b->indices);
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
        const SDL_GPUTransferBufferLocation vtx_src = {
            .transfer_buffer = vtx_transfer, .offset = vtx_offset};
        const SDL_GPUBufferRegion vtx_dst = {
            .buffer = vbuf, .offset = vtx_offset, .size = vtx_size};
        const bool buffers_cycle = RingCycleBuffers();
        SDL_UploadToGPUBuffer(copy, &vtx_src, &vtx_dst, buffers_cycle);
        const SDL_GPUTransferBufferLocation idx_src = {
            .transfer_buffer = vtx_transfer, .offset = idx_offset};
        const SDL_GPUBufferRegion idx_dst = {
            .buffer = ibuf,
            .offset = index_at * (Uint32)sizeof(*interp_mid_index),
            .size = idx_size};
        SDL_UploadToGPUBuffer(copy, &idx_src, &idx_dst, buffers_cycle);
        SDL_EndGPUCopyPass(copy);
        const DrawTargetBatch target = {
            interpolated_vram,
            target_index,
            internal_res,
            true,
            &interp_mid_store[b->vertex],
            &interp_mid_index[b->index],
            (int)b->indices,
            vtx_offset,
            index_at * (Uint32)sizeof(*interp_mid_index),
            &interp_mid_mark_end[b->mark],
            &interp_mid_views[b->mark],
            b->extra_texture,
            b->extra_sampler,
            b->grid_same};
        if (!DrawTargetRuns(&target))
            return false;
    }
    return true;
}

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
    while (nj < interp_junction_prims_len &&
           interp_junction_prims[nj].base < drawn)
        ++nj;
    while (
        nl < interp_line_pending_len && interp_line_pending[nl].start < drawn)
        ++nl;
    ++draw_frame_stats.syncs[reason];
    if (reason != INTERP_SYNC_FRAME && np + nj + nl) {
        ++draw_frame_stats.split_pending;
        static bool warned[INTERP_SYNC_N];
        if (!warned[reason]) {
            warned[reason] = true;
            WARNF("midpoint of frame %u placed before the frame was complete "
                  "(%s): %u ghosts and new faces, %u T-junction primitives and "
                  "%u outlines see only the neighbours queued so far",
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
    memmove(interp_pending, interp_pending + np,
            sizeof(*interp_pending) * (interp_pending_len - np));
    interp_pending_len -= np;
    for (unsigned k = 0; k < interp_pending_len; ++k)
        interp_pending[k].base -= drawn;
    memmove(interp_junction_prims, interp_junction_prims + nj,
            sizeof(*interp_junction_prims) * (interp_junction_prims_len - nj));
    interp_junction_prims_len -= nj;
    for (unsigned k = 0; k < interp_junction_prims_len; ++k)
        interp_junction_prims[k].base -= drawn;
    const unsigned lines = interp_line_pending_len;
    memmove(interp_line_pending, interp_line_pending + nl,
            sizeof(*interp_line_pending) * (lines - nl));
    interp_line_pending_len -= nl;
    for (unsigned k = 0; k < interp_line_pending_len; ++k)
        interp_line_pending[k].start -= drawn,
            interp_line_pending[k].end -= drawn;
    /* a line being drawn (InterpLine) has its entry after the others; its
     * quads already drawn (before its start) leave it unplaced */
    if (interp_line_pend >= 0 && (unsigned)interp_line_pend == lines) {
        InterpLinePending* q = &interp_line_pending[lines];
        q->start = q->start >= drawn ? q->start - drawn : 0xFFFFFFFFu;
        interp_line_pending[interp_line_pending_len] = *q;
        interp_line_pend = (int)interp_line_pending_len;
    }
    if (n_vertices) {
        memmove(interp_mid_store, interp_mid_store + drawn,
                sizeof(Vertex) * n_vertices);
        memmove(interp_mid_depth_store, interp_mid_depth_store + drawn,
                sizeof(float) * n_vertices);
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
    const unsigned most =
        limit_vertices > 0 && (unsigned)limit_vertices < MAX_VERTEX_COUNT
            ? (unsigned)limit_vertices
            : MAX_VERTEX_COUNT;
    if (n_vertices + vertices > most ||
        n_indices + (int)indices > MAX_INDEX_COUNT ||
        (limit_primitives > 0 &&
         batch_primitives >= (unsigned)limit_primitives))
        return true;
    if (!interpolation_enabled ||
        (interp_mid_len + n_vertices + vertices <= INTERP_MID_VERTICES &&
         interp_mid_index_len + (unsigned)n_indices + indices <=
             INTERP_MID_INDICES))
        return false;
    DRAW_FLUSH_FOR(DRAW_FLUSH_OVERFLOW);
    InterpMidpointSync(INTERP_SYNC_FULL);
    return false;
}
