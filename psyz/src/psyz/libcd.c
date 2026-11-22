#include <psyz.h>
#include <libetc.h>
#include <libcd.h>
#include <psyz/log.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <kernel.h>
#include "../internal.h"
#include "os.h"
#include "../../../decomp/src/libcd/libcd_stream.h"
#include "../../decomp/src/libspu/libspu_private.h"

#define SECTOR_SIZE 2352
#define MAX_TRACKS 99
#define MAX_PATH_LEN 512

// Track types
typedef enum {
    TRACK_AUDIO,
    TRACK_MODE1_2352,
    TRACK_MODE2_2352,
    TRACK_UNKNOWN
} TrackType;

// Track entry from CUE file
typedef struct {
    char file_path[MAX_PATH_LEN];
    int start_sector; // MSF sector offset within the file
    int abs_sector;   // Absolute sector position on the "virtual CD"
    int track_num;
    TrackType type;
    int is_valid;
} TrackEntry;

// https://www.problemkaputt.de/psx-spx.htm#cdromcontrollerioports
typedef struct {
    u_char cd_src_l_dst_l;  // volume for CD(L) -> SPU (L)
    u_char cd_src_l_dst_r;  // volume for CD(L) -> SPU (R)
    u_char cd_src_r_dst_l;  // volume for CD(R) -> SPU (L)
    u_char cd_src_r_dst_r;  // volume for CD(R) -> SPU (R)
    u_char adpcm_mute;      // 0=Normal, 1=Mute
    u_char is_stereo;       // 0=Mono, 1=Stereo
    u_char sample_rate;     // 0=37800Hz, 1=18900Hz
    u_char bits_per_sample; // 0=4bit, 1=8bit
    u_char emphasis;        // 0=Off, 1=Emphasis
} CdContext;

// Global CUE data storage
static TrackEntry g_tracks[MAX_TRACKS];
static int g_track_count = 0;
static char g_cue_base_path[MAX_PATH_LEN];

char* CD_comstr[] = {
    "CdlSync",    "CdlNop",
    "CdlSetloc",  "CdlPlay",
    "CdlForward", "CdlBackward",
    "CdlReadN",   "CdlStandby",
    "CdlStop",    "CdlPause",
    "CdlReset",   "CdlMute",
    "CdlDemute",  "CdlSetfilter",
    "CdlSetmode", "CdlGetparam",
    "CdlGetlocL", "CdlGetlocP",
    "?",          "CdlGetTN",
    "CdlGetTD",   "CdlSeekL",
    "CdlSeekP",   "?",
    "?",          "?",
    "?",          "CdlReadS",
    "?",          "?",
    "?",          "?",
};
char* CD_intstr[] = {
    "NoIntr",  "DataReady", "Complete", "Acknowledge",
    "DataEnd", "DiskError", "?",        "?",
};

CdlCB CD_cbsync = NULL;
CdlCB CD_cbready = NULL;
int CD_cbread = 0;
int CD_debug = 0;
u_char CD_status = 0;
int CD_status1 = 0;
int CD_nopen = 0;
CdlLOC CD_pos = {2, 0, 0, 0};
u_char CD_mode = 0;
u_char CD_com = 0;
int DS_active = 0;
static CdContext ctx;

int CD_cw(u8 com, u8* param, u_char* result, s32 arg3);
int CD_sync(int mode, u_char* result);

static void ADPMUTE(int mute) {
    // 0x1F801800 = 3
    ctx.adpcm_mute = mute != 0; // 0x1F801803 = mute & 1
}

static int enable_passthrough = 1;
static void CHNGATV() {
    // 0x1F801800 = 3
    // 0x1F801803 = 0x20

    // enabling passthrough avoid the transformation of the CD pulled samples
    // to be re-mixed with different volume values, leading an increase of
    // performance on weak/embedded targets.
    //
    // PS1 defaults CD_vol as 127,0,127,0 - if detected, enable passthrough.
    //
    // Volume mix algo on PsyZ is (sample * volume) / 128, meaning 128,0,128,0
    // would also return 1:1 samples - enable passthrough here too.
    //
    // volume at 127 and 128 would theoretically generate sound at different
    // levels of volume, but no normal human would ever notice such difference.
    enable_passthrough =
        (ctx.cd_src_l_dst_l == 127 || ctx.cd_src_l_dst_l == 128) &&
        (ctx.cd_src_r_dst_r == 127 || ctx.cd_src_r_dst_r == 128) &&
        ctx.cd_src_l_dst_r == 0 && ctx.cd_src_r_dst_l == 0;
}

static short clamp16(int v) {
    if (v < -32768)
        return -32768;
    if (v > 32767)
        return 32767;
    return (short)v;
}

// Trim whitespace from both ends of a string
static char* str_trim(char* str) {
    char* end;
    while (isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == 0) {
        return str;
    }
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }
    end[1] = '\0';
    return str;
}

// Extract quoted string or unquoted token
static char* extract_token(char* str, char* dest, int max_len) {
    char* p = str;
    int len = 0;

    // Skip leading whitespace
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '"') {
        // Quoted string
        p++;
        while (*p && *p != '"' && len < max_len - 1) {
            dest[len++] = *p++;
        }
        if (*p == '"') {
            p++;
        }
    } else {
        // Unquoted token (up to space)
        while (*p && !isspace((unsigned char)*p) && len < max_len - 1) {
            dest[len++] = *p++;
        }
    }
    dest[len] = '\0';
    return p;
}

// Get file size in sectors
static int get_file_size_in_sectors(const char* file_path) {
    FILE* fp = Psyz_FileOpen(file_path, "rb");
    if (!fp) {
        WARNF("Failed to open file for size calculation: %s", file_path);
        return 0;
    }

    int64_t file_size =
        Psyz_FileSeek(fp, 0, SEEK_END) == 0 ? Psyz_FileTell(fp) : -1;
    fclose(fp);

    if (file_size < 0) {
        WARNF("Failed to get file size: %s", file_path);
        return 0;
    }

    // Convert bytes to sectors (rounded up)
    return (int)((file_size + SECTOR_SIZE - 1) / SECTOR_SIZE);
}

// Parse a CUE file and populate the global track table
static int parse_cue_file(const char* cue_path) {
    // Binary mode: lines end in LF or CRLF on every host (str_trim drops the
    // CR), and the paths are UTF-8 (Psyz_FileOpen).
    FILE* fp = Psyz_FileOpen(cue_path, "rb");
    if (!fp) {
        ERRORF("error opening CUE file %s", cue_path);
        return -1;
    }

    // Extract base directory from CUE path for resolving relative file paths:
    // the last separator of either kind ("C:/discs\\rr4.cue" mixes them).
    const char* last_slash = strrchr(cue_path, '/');
    const char* last_backslash = strrchr(cue_path, '\\');
    if (!last_slash || (last_backslash && last_backslash > last_slash)) {
        last_slash = last_backslash;
    }
    if (last_slash) {
        unsigned int len = (unsigned int)(last_slash - cue_path + 1);
        if (len >= MAX_PATH_LEN) {
            len = MAX_PATH_LEN - 1;
        }
        strncpy(g_cue_base_path, cue_path, len);
        g_cue_base_path[len] = '\0';
    } else {
        g_cue_base_path[0] = '\0';
    }

    char full_line[1024];
    char current_file[MAX_PATH_LEN] = {0};
    char current_file_full_path[MAX_PATH_LEN] = {0};
    int current_track = -1;
    int current_file_abs_sector =
        0; // Absolute sector where current file starts

    g_track_count = 0;
    int first_line = 1;
    while (fgets(full_line, sizeof(full_line), fp)) {
        char* line = full_line;
        // A UTF-8 byte order mark, as Windows editors save CUE sheets.
        if (first_line && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
            line += 3;
        }
        first_line = 0;
        line = str_trim(line);
        if (strncmp(line, "FILE", 4) == 0) {
            // Before switching to new file, calculate size of previous file
            if (current_file[0] != '\0') {
                int prev_file_size =
                    get_file_size_in_sectors(current_file_full_path);
                current_file_abs_sector += prev_file_size;
            }

            char* p = line + 4;
            extract_token(p, current_file, MAX_PATH_LEN);

            // Build full path for the new file
            if (current_file[0] == '/' || current_file[0] == '\\' ||
                (current_file[0] != '\0' && current_file[1] == ':')) {
                // Absolute path
                strncpy(current_file_full_path, current_file, MAX_PATH_LEN - 1);
            } else {
                // Relative path - prepend base directory
                snprintf(current_file_full_path, MAX_PATH_LEN, "%s%s",
                         g_cue_base_path, current_file);
            }
        } else if (strncmp(line, "TRACK", 5) == 0) {
            int track_num = 1;
            char type_str[64];
            if (sscanf(line, "TRACK %d %63s", &track_num, type_str) == 2) {
                if (track_num < 1 || track_num > MAX_TRACKS) {
                    ERRORF("Invalid track number: %d", track_num);
                    continue;
                }

                current_track = track_num - 1;
                TrackEntry* track = &g_tracks[current_track];
                track->track_num = track_num;
                track->is_valid = 1;
                track->start_sector = 0; // Will be set by INDEX 01
                track->abs_sector =
                    0; // Will be calculated when INDEX 01 is parsed

                // Store file path
                strncpy(
                    track->file_path, current_file_full_path, MAX_PATH_LEN - 1);

                if (strcmp(type_str, "AUDIO") == 0) {
                    track->type = TRACK_AUDIO;
                } else if (strcmp(type_str, "MODE1/2352") == 0) {
                    track->type = TRACK_MODE1_2352;
                } else if (strcmp(type_str, "MODE2/2352") == 0) {
                    track->type = TRACK_MODE2_2352;
                } else {
                    track->type = TRACK_UNKNOWN;
                    DEBUGF("Unknown track type: %s", type_str);
                }

                if (track_num > g_track_count) {
                    g_track_count = track_num;
                }
            }
        } else if (strncmp(line, "INDEX", 5) == 0) {
            if (current_track < 0) {
                ERRORF("INDEX found before TRACK");
                continue;
            }
            int index_num;
            char msf_str[32];
            if (sscanf(line, "INDEX %d %31s", &index_num, msf_str) == 2) {
                int minute = 0, second = 0, frame = 0;
                if (sscanf(msf_str, "%d:%d:%d", &minute, &second, &frame) ==
                    3) {
                    int sector = minute * 60 * 75 + second * 75 + frame;
                    if (index_num == 1) { // INDEX 01 is the track data
                        TrackEntry* track = &g_tracks[current_track];
                        track->start_sector = sector;
                        track->abs_sector = current_file_abs_sector + sector;
                        DEBUGF("Track %02d: %s at sector %d", track->track_num,
                               track->file_path, track->abs_sector);
                    }
                }
            }
        } else {
            WARNF("CUE line not recognized: %s", line);
        }
    }
    fclose(fp);

    if (g_track_count == 0) {
        ERRORF("No valid tracks found in CUE file");
        return -1;
    }
    DEBUGF("Parsed CUE file: %d tracks", g_track_count);
    return 0;
}

// Number of bytes CD_cw writes for the short status-style responses.
#define CD_STATUS_RESULT_LEN 4

// Cached last-command response, exposed via CD_sync().
static u_char last_result[8];
static size_t last_result_len = 0;
static int last_intr = CdlComplete;
static unsigned last_result_count; // cache_last_result calls

// `result` points at a caller-owned buffer sized for its own command, which is
// usually shorter than last_result, so only `len` bytes may be touched.
static void cache_last_result(const u_char* result, size_t len, int intr) {
    if (len > sizeof(last_result)) {
        len = sizeof(last_result);
    }
    memset(last_result, 0, sizeof(last_result));
    memcpy(last_result, result, len);
    last_result_len = len;
    last_intr = intr;
    last_result_count++;
}

static int is_disk_loaded = 0;
static u_char data_sector[SECTOR_SIZE];
static int data_read_active = 0;
static int data_sector_valid = 0;
static int data_sector_number = 0;
static int data_getsector_count = 0;

// Compute the absolute sector of the disc lead-out, used by CdlGetTD with
// param = 0x00. Equals the absolute sector where the last track's file ends.
static int lead_out_sector(void) {
    if (g_track_count == 0) {
        return 0;
    }
    TrackEntry* last = &g_tracks[g_track_count - 1];
    return last->abs_sector + get_file_size_in_sectors(last->file_path);
}

// 1 = disc lid is open, 0 = lid closed
static int shell_is_open = 0;
void Psyz_CdShellOpen(int is_open) {
    shell_is_open = is_open ? 1 : 0;
    if (shell_is_open) {
        CD_status |= CdlStatShellOpen;
        CD_status &= ~CdlStatStandby;
    } else {
        CD_status &= ~CdlStatShellOpen;
        CD_status |= CdlStatStandby;
    }
}

static void close_track_file(void);

static int read_raw_sector(int sector, u_char* out);

int Psyz_CdSetDiskPath(const char* diskPath) {
    close_track_file();
    is_disk_loaded = 0;
    g_track_count = 0;
    memset(g_tracks, 0, sizeof(g_tracks));
    if (!diskPath) {
        DEBUGF("Disk path unset");
        return 0;
    }
    if (parse_cue_file(diskPath) < 0) {
        return -1;
    }
    is_disk_loaded = 1;
    DEBUGF("Disk path set: %s", diskPath);
    return 0;
}

static PsyzCdReadCB disk_read_cb = NULL;
void Psyz_CdSetReadCB(PsyzCdReadCB cb) { disk_read_cb = cb; }

#define N_CHANNELS 2
#define SAMPLE_SIZE sizeof(short)
#define SECTOR_SIZE 2352
#define BUFFER_SECTORS 8 // Read 8 sectors at a time
#define CD_BUF_FRAMES                                                          \
    (SECTOR_SIZE * BUFFER_SECTORS / (N_CHANNELS * SAMPLE_SIZE))
static FILE* track_file;
static const TrackEntry* track_file_track;

static int read_sector_unlocked(int sector, u_char* out);

static void close_track_file(void) {
    if (track_file) {
        fclose(track_file);
        track_file = NULL;
    }
    track_file_track = NULL;
}
static s16 cd_buf[CD_BUF_FRAMES * N_CHANNELS];
static size_t cd_buf_pos = 0;   // current read position (in frames)
static size_t cd_buf_count = 0; // number of valid frames in buffer
static int is_playing = 0; // pause or unpause seeking through the CD stream
static int is_muted = 0;   // return empty samples while CD keeps streaming
// Silence before an XA read's first audio sector: the data sectors before
// it, at the disc's rate, so the sound keeps its place in the stream.
static uint64_t cd_delay_frames;

// Emits the silence still due before CD audio starts; returns its frames.
static size_t cd_pull_delay(short* buf, size_t max_frames) {
    size_t n = cd_delay_frames < max_frames ? (size_t)cd_delay_frames : max_frames;
    memset(buf, 0, n * N_CHANNELS * SAMPLE_SIZE);
    cd_delay_frames -= n;
    return n;
}

static void cd_data_end(void) {
    if (CD_cbready) {
        CD_cbready(CdlDataEnd, NULL);
    }
    Psyz_KernelRaise(HwCdRom, EvSpTRAP);
}

// CD audio pull callback — called by SPU when its internal ring buffer
// runs low. Fills `buf` with up to `max_frames` interleaved stereo frames.
// One mutex lock per batch rather than per frame.
static size_t cdda_pull_samples(short* buf, size_t max_frames) {
    size_t written = 0;
    int hit_eof = 0;
    Psyz_AudioLock();
    if (!is_playing || !track_file) {
        goto end;
    }
    written = cd_pull_delay(buf, max_frames);
    while (written < max_frames) {
        // Refill file read buffer if consumed
        if (cd_buf_pos >= cd_buf_count) {
            size_t nRead = fread(cd_buf, 1, sizeof(cd_buf), track_file);
            if (nRead > 0) {
                cd_buf_count = (int)(nRead / (N_CHANNELS * SAMPLE_SIZE));
                cd_buf_pos = 0;
            } else {
                DEBUGF("cd audio playback reached end of file");
                is_playing = 0;
                hit_eof = 1;
                break;
            }
        }
        // Copy as many frames as possible from file buffer to output
        const size_t avail = cd_buf_count - cd_buf_pos;
        const size_t want = max_frames - written;
        const size_t n = avail < want ? avail : want;
        if (is_muted) {
            memset(&buf[written * 2], 0, n * N_CHANNELS * SAMPLE_SIZE);
        } else {
            memcpy(&buf[written * 2], &cd_buf[cd_buf_pos * 2],
                   n * N_CHANNELS * SAMPLE_SIZE);
        }
        cd_buf_pos += n;
        written += n;
    }
end:
    Psyz_AudioUnlock();
    if (hit_eof) {
        Psyz_KernelPost(cd_data_end);
    }
    return written;
}

#define XA_DECODED_MAX_FRAMES 4032            // 18 blocks * 4 sub * 28 samples
// Real hardware keeps reading sectors and discards the ones rejected by the
// filter, so once the selected file/channel ends playback goes silent. Give up
// after this many consecutive rejected sectors instead of scanning the disc
// for the next matching one, which is unrelated audio.
#define XA_MAX_SECTOR_GAP 64

static struct {
    short decoded[XA_DECODED_MAX_FRAMES * 2];
    int active;
    unsigned char filter_file;
    unsigned char filter_channel;
    int decoded_count;
    int decoded_pos;
    int hist_l_old, hist_l_older;
    int hist_r_old, hist_r_older;
    // the resampler (xa_zigzag): its input ring, the frames to its next
    // output burst, and that burst's frames not yet played
    short ring_l[32], ring_r[32];
    unsigned ring_p;
    int sixstep;
    short out_l[7], out_r[7];
    int out_pos, out_count;
    int cur_abs_sector; // absolute last sector used, for CdlGetlocL
    int stream_ended;   // filtered file/channel ran out, emit silence
    int started;        // the first audio sector has been decoded
    int silent_sectors_pending;
} xa;

static void xa_reset_stream(void) {
    xa.decoded_count = 0;
    xa.decoded_pos = 0;
    xa.hist_l_old = xa.hist_l_older = 0;
    xa.hist_r_old = xa.hist_r_older = 0;
    memset(xa.ring_l, 0, sizeof(xa.ring_l));
    memset(xa.ring_r, 0, sizeof(xa.ring_r));
    xa.ring_p = 0;
    xa.sixstep = 6;
    xa.out_pos = xa.out_count = 0;
    xa.stream_ended = 0;
    xa.started = 0;
    xa.silent_sectors_pending = 0;
}

static int xa_sector_matches(unsigned char file, unsigned char channel) {
    if (!(CD_mode & CdlModeSF)) {
        return 1;
    }
    return file == xa.filter_file && channel == xa.filter_channel;
}

static const int xa_pos[4] = {0, 60, 115, 98};
static const int xa_neg[4] = {0, 0, -52, -55};
// Decode 28 samples for one (block, nibble) pair into `dst` (stride=2 stereo
// or stride=1 mono). Returns updated old/older via pointers.
static void xa_decode_28(const unsigned char* blk, int sub, int nibble,
                         short* dst, int stride, int* prev, int* prev2) {
    const unsigned char hdr = blk[4 + sub * 2 + nibble];
    /* As DuckStation's DecodeXAADPCMChunks: the nibble in the top of a
     * 16-bit sample shifted right by the header's shift (9 for 13..15), each
     * filter term shifted down on its own, the clamped sample kept as the
     * history; filters 4..15 are none. */
    const int shift_in = hdr & 0x0F;
    const int shift = (shift_in > 12) ? 9 : shift_in;
    const int filter = (hdr >> 4) & 0x0F;
    const int f0 = filter < 4 ? xa_pos[filter] : 0;
    const int f1 = filter < 4 ? xa_neg[filter] : 0;
    for (int j = 0; j < 28; j++) {
        const unsigned char byte = blk[16 + sub + j * 4];
        const int t = (short)(unsigned short)(((byte >> (nibble * 4)) & 0x0F) << 12) >> shift;
        const int s = clamp16(t + (((*prev) * f0) >> 6) + (((*prev2) * f1) >> 6));
        dst[j * stride] = (short)s;
        *prev2 = *prev;
        *prev = s;
    }
}

// Decode a Form2 user-data area (0x914 = 2324 bytes) of 4-bit stereo XA.
// Writes interleaved L,R s16 frames into xa.decoded; returns frame count.
static int xa_decode_user_stereo_4bit(const unsigned char* user) {
    int frame = 0;
    for (int i = 0; i < 18; i++) {
        const unsigned char* block = user + i * 128;
        for (int sub = 0; sub < 4; sub++) {
            xa_decode_28(block, sub, 0, &xa.decoded[(frame) * 2 + 0], 2,
                         &xa.hist_l_old, &xa.hist_l_older);
            xa_decode_28(block, sub, 1, &xa.decoded[(frame) * 2 + 1], 2,
                         &xa.hist_r_old, &xa.hist_r_older);
            frame += 28;
        }
    }
    return frame;
}

// Decode a Form2 user-data area of 4-bit mono XA: both nibble passes write
// to the same mono buffer; we duplicate to L and R for output.
static int xa_decode_user_mono_4bit(const unsigned char* user) {
    short mono[XA_DECODED_MAX_FRAMES];
    int frame = 0;
    for (int i = 0; i < 18; i++) {
        const unsigned char* block = user + i * 128;
        for (int sub = 0; sub < 4; sub++) {
            xa_decode_28(block, sub, 0, &mono[frame], 1, &xa.hist_l_old,
                         &xa.hist_l_older);
            frame += 28;
            xa_decode_28(block, sub, 1, &mono[frame], 1, &xa.hist_l_old,
                         &xa.hist_l_older);
            frame += 28;
        }
    }
    for (int i = 0; i < frame; i++) {
        xa.decoded[i * 2 + 0] = mono[i];
        xa.decoded[i * 2 + 1] = mono[i];
    }
    return frame;
}

static int xa_read_and_decode_sector(void) {
    unsigned char sector[SECTOR_SIZE];
    int gap = 0;
    /* Once the selected clip ends, keep consuming the physical image while
     * emitting silence. Otherwise GetlocL freezes and bounded image readers
     * never reach EOF. One silent 2016-frame block lasts 8 sectors at 2x
     * (4 at 1x). Do not decode unrelated clips after the filter gap. */
    int silent_sectors = (CD_mode & CdlModeSpeed) ? 8 : 4;
    if (xa.silent_sectors_pending) {
        xa.silent_sectors_pending =
            xa.silent_sectors_pending > silent_sectors
                ? xa.silent_sectors_pending - silent_sectors
                : 0;
        goto silence;
    }
    while (!xa.stream_ended || silent_sectors-- > 0) {
        // Seek every sector: data reads of the same image (streaming) share
        // track_file with this decoder.
        const size_t n =
            disk_read_cb ? (size_t)disk_read_cb(&(struct PsyzCdRead){
                               (unsigned int)(xa.cur_abs_sector + 1),
                               SECTOR_SIZE, sector})
            : read_sector_unlocked(xa.cur_abs_sector + 1, sector) == 0
                ? SECTOR_SIZE
                : 0;
        if (n != SECTOR_SIZE) {
            return 0; // EOF or short read
        }
        xa.cur_abs_sector++;
        if (xa.stream_ended)
            continue;
        const unsigned char file = sector[0x10];
        const unsigned char channel = sector[0x11];
        const unsigned char submode = sector[0x12];
        const unsigned char ci = sector[0x13];
        if ((submode & 0x44) != 0x44 || !xa_sector_matches(file, channel)) {
            if (!xa.started) {
                // the sound starts with the first audio sector, the data
                // sectors before it silent at the disc's rate
                cd_delay_frames += PSYZ_SPU_SAMPLE_RATE / ((CD_mode & CdlModeSpeed) ? 150 : 75);
            }
            // data sector or filtered out, skip from XA stream
            if (++gap >= XA_MAX_SECTOR_GAP) {
                xa.stream_ended = 1;
                /* The search already consumed these sectors. Account for
                 * their elapsed silence before reading the rest of the tail. */
                xa.silent_sectors_pending = gap - silent_sectors;
                goto silence;
            }
            continue;
        }
        xa.started = 1;
        const unsigned char* user = &sector[0x18];
        int stereo = (ci & 0x03) == 1;
        int rate18900 = (ci & 0x04) != 0;
        int bits8 = (ci & 0x30) != 0;
        if (rate18900 || bits8) {
            LOG_ONCE(
                "XA: 18900Hz or 8-bit not supported, decoding as 4bit/37800");
        }
        if (stereo) {
            xa.decoded_count = xa_decode_user_stereo_4bit(user);
        } else {
            xa.decoded_count = xa_decode_user_mono_4bit(user);
        }
        xa.decoded_pos = 0;
        return 1;
    }
silence:
    memset(xa.decoded, 0, sizeof(xa.decoded));
    xa.decoded_count = XA_DECODED_MAX_FRAMES / 2;
    xa.decoded_pos = 0;
    return 1;
}

/* The XA resampler, the console's and DuckStation's (cdrom.cpp
 * ResampleXAADPCM, psx-spx "zigzag" interpolation): every 6 input frames
 * (37800 Hz) give 7 output frames (44100 Hz), each from the ring of the last
 * 32 inputs through one of seven 29-tap tables (about 15 output frames of
 * delay). With the decoder above it gives DuckStation's frames to the
 * sample: the Time Attack scenario's race XA (1.5M frames, PSYZ_XA_DUMP
 * against the regtest's DUCKSTATION_XA_OUT_DUMP) is identical but for the
 * frames where a stream starts or stops; the 4-point Hermite interpolation
 * it replaces, with the same decoder, matched 1% of them. */
static const short xa_zigzag_tables[7][29] = {
    {0, 0x0, 0x0, 0x0, 0x0, -0x0002, 0x000A, -0x0022, 0x0041, -0x0054,
     0x0034, 0x0009, -0x010A, 0x0400, -0x0A78, 0x234C, 0x6794, -0x1780,
     0x0BCD, -0x0623, 0x0350, -0x016D, 0x006B, 0x000A, -0x0010, 0x0011,
     -0x0008, 0x0003, -0x0001},
    {0, 0x0, 0x0, -0x0002, 0x0, 0x0003, -0x0013, 0x003C, -0x004B, 0x00A2,
     -0x00E3, 0x0132, -0x0043, -0x0267, 0x0C9D, 0x74BB, -0x11B4, 0x09B8,
     -0x05BF, 0x0372, -0x01A8, 0x00A6, -0x001B, 0x0005, 0x0006, -0x0008,
     0x0003, -0x0001, 0x0},
    {0, 0x0, -0x0001, 0x0003, -0x0002, -0x0005, 0x001F, -0x004A, 0x00B3,
     -0x0192, 0x02B1, -0x039E, 0x04F8, -0x05A6, 0x7939, -0x05A6, 0x04F8,
     -0x039E, 0x02B1, -0x0192, 0x00B3, -0x004A, 0x001F, -0x0005, -0x0002,
     0x0003, -0x0001, 0x0, 0x0},
    {0, -0x0001, 0x0003, -0x0008, 0x0006, 0x0005, -0x001B, 0x00A6, -0x01A8,
     0x0372, -0x05BF, 0x09B8, -0x11B4, 0x74BB, 0x0C9D, -0x0267, -0x0043,
     0x0132, -0x00E3, 0x00A2, -0x004B, 0x003C, -0x0013, 0x0003, 0x0,
     -0x0002, 0x0, 0x0, 0x0},
    {-0x0001, 0x0003, -0x0008, 0x0011, -0x0010, 0x000A, 0x006B, -0x016D,
     0x0350, -0x0623, 0x0BCD, -0x1780, 0x6794, 0x234C, -0x0A78, 0x0400,
     -0x010A, 0x0009, 0x0034, -0x0054, 0x0041, -0x0022, 0x000A, -0x0001,
     0x0, 0x0001, 0x0, 0x0, 0x0},
    {0x0002, -0x0008, 0x0010, -0x0023, 0x002B, 0x001A, -0x00EB, 0x027B,
     -0x0548, 0x0AFA, -0x16FA, 0x53E0, 0x3C07, -0x1249, 0x080E, -0x0347,
     0x015B, -0x0044, -0x0017, 0x0046, -0x0023, 0x0011, -0x0005, 0x0, 0x0,
     0x0, 0x0, 0x0, 0x0},
    {-0x0005, 0x0011, -0x0023, 0x0046, -0x0017, -0x0044, 0x015B, -0x0347,
     0x080E, -0x1249, 0x3C07, 0x53E0, -0x16FA, 0x0AFA, -0x0548, 0x027B,
     -0x00EB, 0x001A, 0x002B, -0x0023, 0x0010, -0x0008, 0x0002, 0x0, 0x0,
     0x0, 0x0, 0x0, 0x0},
};

static short xa_zigzag(const short* ring, int table, unsigned p) {
    const short* t = xa_zigzag_tables[table];
    int sum = 0;
    for (unsigned i = 0; i < 29; i++) {
        sum += ((int)ring[(p - i) & 31] * (int)t[i]) >> 15;
    }
    return clamp16(sum);
}

// Feeds the next 37800 Hz stereo input frame to the resampler, which makes
// a burst of 7 output frames every 6th. Returns 0 if the stream ended.
static int xa_advance_input(void) {
    if (xa.decoded_pos >= xa.decoded_count) {
        if (!xa_read_and_decode_sector()) {
            return 0;
        }
    }
    xa.ring_l[xa.ring_p] = xa.decoded[xa.decoded_pos * 2 + 0];
    xa.ring_r[xa.ring_p] = xa.decoded[xa.decoded_pos * 2 + 1];
    xa.decoded_pos++;
    xa.ring_p = (xa.ring_p + 1) & 31;
    if (--xa.sixstep == 0) {
        xa.sixstep = 6;
        for (int j = 0; j < 7; j++) {
            xa.out_l[j] = xa_zigzag(xa.ring_l, j, xa.ring_p);
            xa.out_r[j] = xa_zigzag(xa.ring_r, j, xa.ring_p);
        }
        xa.out_pos = 0;
        xa.out_count = 7;
    }
    return 1;
}

/* PSYZ_XA_DUMP=<file>: every resampled XA frame played (s16 L, R, before
 * the CD volume), to compare with DuckStation's DUCKSTATION_XA_OUT_DUMP. */
static void xa_dump(const short* lr) {
    static FILE* f;
    static int init;
    if (!init) {
        init = 1;
        if (getenv("PSYZ_XA_DUMP")) f = fopen(getenv("PSYZ_XA_DUMP"), "wb");
    }
    if (f) fwrite(lr, 4, 1, f);
}

static size_t xa_pull_samples(short* buf, size_t max_frames) {
    size_t written = 0;
    int hit_eof = 0;
    Psyz_AudioLock();
    if (!is_playing || (!track_file && !disk_read_cb) || !xa.active) {
        goto end;
    }
    while (written < max_frames) {
        if (cd_delay_frames) {
            written += cd_pull_delay(&buf[written * 2], max_frames - written);
            continue;
        }
        while (xa.out_pos >= xa.out_count) {
            if (!xa_advance_input()) {
                hit_eof = 1;
                goto end;
            }
        }
        if (cd_delay_frames) {
            continue; // the silence before the first audio sector
        }
        if (is_muted) {
            buf[written * 2 + 0] = 0;
            buf[written * 2 + 1] = 0;
        } else {
            buf[written * 2 + 0] = xa.out_l[xa.out_pos];
            buf[written * 2 + 1] = xa.out_r[xa.out_pos];
        }
        if (!is_muted) xa_dump(&buf[written * 2]);
        xa.out_pos++;
        written++;
    }
end:
    if (hit_eof) {
        is_playing = 0;
        xa.active = 0;
        if (xa.cur_abs_sector >= 0)
            CdIntToPos(xa.cur_abs_sector, &CD_pos);
    }
    Psyz_AudioUnlock();
    if (hit_eof) {
        Psyz_KernelPost(cd_data_end);
    }
    return written;
}

size_t Psyz_CdPullSamples(short* out, size_t num_frames) {
    size_t read_frames;
    Psyz_AudioLock();
    u_char mode = CD_mode;
    Psyz_AudioUnlock();
    if (mode & CdlModeDA) {
        read_frames = cdda_pull_samples(out, num_frames);
    } else if (mode & CdlModeRT) {
        read_frames = xa_pull_samples(out, num_frames);
    } else {
        read_frames = 0;
    }
    Psyz_AudioLock();
    if (!enable_passthrough) {
        for (size_t i = 0; i < read_frames; i++) {
            int l = out[i * 2 + 0];
            int r = out[i * 2 + 1];
            int l_out = (l * ctx.cd_src_l_dst_l + r * ctx.cd_src_r_dst_l) >> 7;
            int r_out = (l * ctx.cd_src_l_dst_r + r * ctx.cd_src_r_dst_r) >> 7;
            out[i * 2 + 0] = clamp16(l_out);
            out[i * 2 + 1] = clamp16(r_out);
        }
    }
    Psyz_AudioUnlock();
    return read_frames;
}

// Open the track containing the absolute sector encoded in CD_pos. Closes
// any previously-open track_file. Returns 0 on success, -1 on failure.
static TrackEntry* find_track_for_sector(int sector) {
    for (int i = 0; i < g_track_count; i++) {
        if (!g_tracks[i].is_valid) {
            continue;
        }
        int next_abs_sector = INT_MAX;
        if (i + 1 < g_track_count && g_tracks[i + 1].is_valid) {
            next_abs_sector = g_tracks[i + 1].abs_sector;
        }
        if (sector >= g_tracks[i].abs_sector && sector < next_abs_sector) {
            return &g_tracks[i];
        }
    }
    return NULL;
}

static int open_track_at_cd_pos(void) {
    int sector = CdPosToInt(&CD_pos);
    TrackEntry* track = find_track_for_sector(sector);
    if (!track) {
        WARNF("no track found for sector %d", sector);
        return -1;
    }
    Psyz_AudioLock();
    close_track_file();
    FILE* file = Psyz_FileOpen(track->file_path, "rb");
    if (!file) {
        ERRORF("failed to open audio file: %s", track->file_path);
        Psyz_AudioUnlock();
        return -1;
    }
    int sector_offset = sector - track->abs_sector;
    sector_offset += track->start_sector;
    int64_t byte_offset = (int64_t)sector_offset * SECTOR_SIZE;
    if (Psyz_FileSeek(file, byte_offset, SEEK_SET) != 0) {
        ERRORF("failed to seek to sector %d in %s", sector, track->file_path);
        fclose(file);
        Psyz_AudioUnlock();
        return -1;
    }
    DEBUGF("opened track %s at sector %d (offset %d)", track->file_path, sector,
           sector_offset);
    track_file = file;
    track_file_track = track;
    Psyz_AudioUnlock();
    return 0;
}

// Reads one raw sector through track_file. The caller holds the audio lock.
static int read_sector_unlocked(int sector, u_char* out) {
    TrackEntry* track = find_track_for_sector(sector);
    if (!track) {
        return -1;
    }
    if (track != track_file_track || !track_file) {
        close_track_file();
        track_file = Psyz_FileOpen(track->file_path, "rb");
        if (!track_file) {
            ERRORF("failed to open data file: %s", track->file_path);
            return -1;
        }
        track_file_track = track;
    }
    int64_t offset =
        (int64_t)(sector - track->abs_sector + track->start_sector) *
        SECTOR_SIZE;
    if (Psyz_FileSeek(track_file, offset, SEEK_SET) == 0 &&
        fread(out, 1, SECTOR_SIZE, track_file) == SECTOR_SIZE) {
        return 0;
    }
    return -1;
}

static int read_raw_sector(int sector, u_char* out) {
    Psyz_AudioLock();
    int result = read_sector_unlocked(sector, out);
    Psyz_AudioUnlock();
    return result;
}

int Psyz_CdReadImageSector(const char* cue_path, int lba, void* out) {
    // The image is parsed into the drive's track table, which is restored
    // afterwards; the audio lock keeps the mixer off it meanwhile.
    static TrackEntry saved_tracks[MAX_TRACKS];
    static char saved_base[MAX_PATH_LEN];
    int saved_count, result = -1;
    FILE* saved_file;
    const TrackEntry* saved_file_track;

    if (!cue_path || !out || lba < 0) {
        return -1;
    }
    Psyz_AudioLock();
    memcpy(saved_tracks, g_tracks, sizeof(g_tracks));
    memcpy(saved_base, g_cue_base_path, sizeof(g_cue_base_path));
    saved_count = g_track_count;
    saved_file = track_file;
    saved_file_track = track_file_track;
    track_file = NULL;
    track_file_track = NULL;
    memset(g_tracks, 0, sizeof(g_tracks));
    if (parse_cue_file(cue_path) == 0) {
        result = read_sector_unlocked(lba, out);
    }
    close_track_file();
    memcpy(g_tracks, saved_tracks, sizeof(g_tracks));
    memcpy(g_cue_base_path, saved_base, sizeof(g_cue_base_path));
    g_track_count = saved_count;
    track_file = saved_file;
    track_file_track = saved_file_track;
    Psyz_AudioUnlock();
    return result;
}

static int read_data_sector_into(int sector, u_char* out) {
    if (disk_read_cb) {
        struct PsyzCdRead read = {(unsigned int)sector, SECTOR_SIZE, out};
        return disk_read_cb(&read) < 0 ? -1 : 0;
    }
    return read_raw_sector(sector, out);
}

static int read_data_sector(int sector) {
    return read_data_sector_into(sector, data_sector);
}

// XA audio sectors that the drive plays itself in CdlModeRT never reach
// the host. Subheader submode bit 2 marks an audio sector.
static int is_played_audio_sector(void) {
    return (CD_mode & CdlModeRT) && (data_sector[0x12] & 0x04);
}

// Polled reads (CdReady). A ready callback receives the sectors instead,
// and polled ADPCM playback (CdlModeRT) keeps this data path idle.
// Frame being assembled by StCdInterrupt (below).
#define ST_SLOT_FILLING 3
#define ST_SLOT_BYTES 2016
static struct {
    int active;
    u32 frame;
    int slot, next, count;
} st_frame;


static int read_data_sector_into(int sector, u_char* out);

/* The drive. Commands complete at once and data comes straight from the
 * image: a read of plain data (CdRead) hands its sectors to the ready
 * callback as fast as the host reads them, at the next service of the drive
 * (the interrupt thread, a VSync, a poll). A read in CdlModeRT plays its XA
 * audio as it goes, so its data sectors keep the disc's rate, 75 or 150 a
 * second, on the kernel's clock: they belong to the same stream as the
 * sound (a movie's frames, the XA player's). */
#define DRIVE_MAX_BURST 600 // sectors a service delivers at most
static struct {
    int setloc_pending; // a Setloc not yet used by a read or play
    int paced;          // the read keeps the disc's rate (CdlModeRT)
    uint64_t start_ns;  // ... its rate counts from here
    uint64_t base;      // ... with this many sectors delivered then
    uint64_t delivered; // sectors the read has delivered
    unsigned generation; // counts read commands
    int servicing;
    int commanding; // inside CD_cw: nothing is delivered in a command
    int sync_cb_due;    // CdSyncCallback for the last command
    int error_ready_due; // a refused command's DiskError for the ready callback
    int timer_posted;   // drive_timer is queued
    uint64_t lead_ns;   // a paced read's sectors due this much later are due now
    int hold_sector;    // a stream frame's last sector held until hold_ns
    uint64_t hold_ns;
} drive;

/* A movie's pictures at an even rate. A frame of an XA-interleaved stream
 * is complete when its last data sector arrives, which the audio sectors
 * between shift by a sector period from frame to frame; shown at the blank
 * after that, its pictures would alternate one and three fields where the
 * frame times cross the blanks. A frame's last sector is held instead
 * until the frame's place on an even grid: the stream's own period (its
 * sectors a frame, from where its frames start, e.g. 6 at double speed
 * for 25 frames a second) from its first frame, plus the most any frame
 * so far came after that grid. */
static struct {
    int active;
    u32 k0;          // the first frame
    int s0;          // ... its first sector
    uint64_t t0;     // ... its completion
    int spf;         // sectors a frame, once known (0 before)
    uint64_t late;   // the latest completion past the grid so far
} st_pace;
static int drive_ready_sector = -1; // sector the ready callback may fetch

// PSYZ_CD_TRACE=1: each drive command and read on stderr, with the
// kernel's time in microseconds.
static int drive_trace(void) {
    static int trace = -1;
    if (trace < 0) {
        const char* env = getenv("PSYZ_CD_TRACE");
        trace = env && *env && *env != '0';
    }
    return trace;
}

static int drive_reading(void) {
    return data_read_active || xa.active || is_playing;
}

static uint64_t drive_rate(void) { return (CD_mode & CdlModeSpeed) ? 150 : 75; }

// Sectors a paced read has reached at `now`, the first a period after its
// start.
static uint64_t drive_sectors_due(uint64_t now) {
    if (now < drive.start_ns) {
        return drive.base;
    }
    return drive.base + (now - drive.start_ns) * drive_rate() / 1000000000u;
}

// When a paced read's next sector is due.
static uint64_t drive_next_sector_ns(void) {
    uint64_t n = drive.delivered + 1 - drive.base;
    uint64_t at = drive.start_ns + (n * 1000000000u + drive_rate() - 1) / drive_rate();
    return drive.hold_sector == data_sector_number && drive.hold_ns > at ? drive.hold_ns : at;
}

static int read_data_sector_into(int sector, u_char* out);

/* When the paced read's next sector, due at due_ns, may be delivered: later
 * than that only for a stream frame's last sector (st_pace). */
static uint64_t st_release_ns(uint64_t due_ns) {
    int sector = data_sector_number;
    if (drive.hold_sector == sector) {
        return drive.hold_ns;
    }
    drive.hold_sector = sector;
    drive.hold_ns = due_ns;
    u_char buf[SECTOR_SIZE];
    StHEADER h;
    if (!StRingAddr || read_data_sector_into(sector, buf) < 0 || (buf[0x12] & 0x04)) {
        return due_ns;
    }
    memcpy(&h, buf + 24, sizeof(h));
    if (h.nSectors <= 0 || h.secCount != h.nSectors - 1) {
        return due_ns;
    }
    int first = sector - (h.nSectors - 1); // (the frame's data; audio may be between)
    if (!st_pace.active || h.frameCount <= st_pace.k0 || h.frameCount - st_pace.k0 > 100000) {
        st_pace.active = 1;
        st_pace.k0 = h.frameCount;
        st_pace.s0 = first;
        st_pace.t0 = due_ns;
        st_pace.spf = 0;
        st_pace.late = 0;
        return due_ns;
    }
    uint64_t n = h.frameCount - st_pace.k0;
    if (!st_pace.spf) {
        if (n < 4) {
            return due_ns;
        }
        st_pace.spf = (int)(((uint64_t)(first - st_pace.s0) + n / 2) / n);
        if (st_pace.spf <= 0) {
            st_pace.active = 0;
            return due_ns;
        }
    }
    uint64_t grid = st_pace.t0 + (n * (uint64_t)st_pace.spf * 1000000000u) / drive_rate();
    if (due_ns > grid + st_pace.late) {
        st_pace.late = due_ns - grid;
    }
    drive.hold_ns = grid + st_pace.late;
    return drive.hold_ns;
}

static void drive_update_status(void) {
    int st = CD_status & ~(CdlStatRead | CdlStatSeek | CdlStatPlay);
    if (!shell_is_open) {
        st |= CdlStatStandby;
    }
    if (drive_reading()) {
        st |= (is_playing && !xa.active && !data_read_active) ? CdlStatPlay
                                                              : CdlStatRead;
    }
    CD_status = st;
}

static void start_data_read(void) {
    data_sector_number = CdPosToInt(&CD_pos);
    data_sector_valid = 0;
    data_getsector_count = 0;
    data_read_active = 1;
    drive.paced = (CD_mode & CdlModeRT) != 0;
    drive.start_ns = Psyz_KernelNowNs();
    drive.base = drive.delivered = 0;
    drive.hold_sector = -1;
    st_pace.active = 0;
    drive.generation++;
    memset(&st_frame, 0, sizeof(st_frame));
    CD_status |= CdlStatRead;
}

static void deliver_data_sector(void) {
    int sector = data_sector_number;
    drive.delivered++;
    if (read_data_sector(sector) < 0) {
        data_read_active = 0;
        CD_status &= ~CdlStatRead;
        if (CD_cbready) {
            CD_cbready(CdlDataEnd, NULL);
        }
        return;
    }
    data_sector_number = sector + 1;
    if (is_played_audio_sector() || !CD_cbready) {
        return;
    }
    u_char result[8] = {(u_char)CD_status};
    // CdGetSector inside the callback reads this sector.
    data_sector_number = sector;
    data_sector_valid = 1;
    data_getsector_count = 0;
    drive_ready_sector = sector;
    unsigned generation = drive.generation;
    CD_cbready(CdlDataReady, result);
    drive_ready_sector = -1;
    if (generation == drive.generation) { // the callback issued no new read
        data_sector_valid = 0;
        data_getsector_count = 0;
        data_sector_number = sector + 1;
    }
}

static void drive_timer(void);

/* The drive's interrupts: a paced read's next sector at its time, anything
 * else (a command's completion, the sectors of a plain read) at once. */
static void drive_schedule(void) {
    Psyz_KernelCancel(drive_timer);
    if (drive.sync_cb_due || drive.error_ready_due ||
        (data_read_active && CD_cbready && !drive.paced)) {
        if (!drive.timer_posted) {
            drive.timer_posted = 1;
            Psyz_KernelPost(drive_timer);
        }
    } else if (data_read_active && CD_cbready) {
        Psyz_KernelPostAtNs(drive_next_sector_ns(), drive_timer);
    }
}

void Psyz_CdServiceDrive(void) {
    /* Both main-thread polls and the interrupt thread service this state.
     * The recursive kernel lock also serializes callbacks with commands. */
    Psyz_KernelLockInterrupts();
    if (drive.servicing || drive.commanding) {
        Psyz_KernelUnlockInterrupts();
        return;
    }
    drive.servicing = 1;
    unsigned generation = drive.generation;
    for (int more = 1; more;) {
        more = 0;
        drive_update_status();
        if (drive.sync_cb_due) {
            drive.sync_cb_due = 0;
            more = 1;
            if (drive_trace()) {
                fprintf(stderr, "cd sync callback %p\n", (void*)CD_cbsync);
            }
            if (CD_cbsync) {
                u_char result[8];
                memcpy(result, last_result, sizeof(result));
                CD_cbsync((u_char)last_intr, result);
            }
        }
        if (drive.error_ready_due) {
            drive.error_ready_due = 0;
            more = 1;
            if (CD_cbready) {
                u_char result[8];
                memcpy(result, last_result, sizeof(result));
                CD_cbready(CdlDiskError, result);
            }
        }
        // a read the callbacks restarted (a retry) goes on at the next service
        if (data_read_active && CD_cbready && generation == drive.generation) {
            uint64_t due = drive.paced ? drive_sectors_due(Psyz_KernelNowNs() + drive.lead_ns)
                                       : drive.delivered + DRIVE_MAX_BURST;
            if (due > drive.delivered + DRIVE_MAX_BURST) {
                // a long stall: the drive moved on meanwhile
                uint64_t skipped = due - drive.delivered - DRIVE_MAX_BURST;
                data_sector_number += (int)skipped;
                drive.delivered += skipped;
            }
            if (drive_trace()) {
                fprintf(stderr, "cd deliver %d..%" PRIu64 " paced %d\n", data_sector_number,
                        due, drive.paced);
            }
            while (data_read_active && CD_cbready && drive.delivered < due) {
                if (drive.paced &&
                    st_release_ns(drive_next_sector_ns()) > Psyz_KernelNowNs() + drive.lead_ns) {
                    break; // a frame's last sector, held to the stream's grid
                }
                deliver_data_sector();
                more = 1;
                if (generation != drive.generation) {
                    break; // the callback restarted the read
                }
            }
            if (!drive.paced && data_read_active) {
                more = 0; // the rest at the next service
            }
        }
    }
    drive.servicing = 0;
    drive_schedule();
    Psyz_KernelUnlockInterrupts();
}

static void drive_timer(void) {
    Psyz_KernelLockInterrupts();
    drive.timer_posted = 0;
    Psyz_KernelUnlockInterrupts();
    Psyz_CdServiceDrive();
}

/* A LIBCD loop that waits on the drive (CdReadSync(0), a read's ending
 * Pause) and found it not done: a millisecond passes on the kernel's clock
 * (the host sleeps, the offline clock moves on), so a read that cannot end
 * runs into LIBCD's timeouts, which count fields, then the drive is
 * serviced. */
void Psyz_CdWaitDrive(void) {
    Psyz_KernelWaitUntilNs(Psyz_KernelNowNs() + 1000000);
    Psyz_CdServiceDrive();
}

/* StGetNext found no frame in the ring: a paced read's next sector is
 * waited for (a movie's frames come at the stream's rate). A sector due in
 * the last few milliseconds before the field's blank is taken that much
 * early, so a movie frame due before a blank is decoded in time to be shown
 * at it, whatever the host's decode time: each picture is shown at the
 * first blank at or after its due time, not one more or less by jitter. */
#define DRIVE_BLANK_LEAD_NS 4000000u
void Psyz_CdWaitSector(void) {
    Psyz_KernelLockInterrupts();
    uint64_t at = data_read_active && CD_cbready && drive.paced ? drive_next_sector_ns() : 0;
    Psyz_KernelUnlockInterrupts();
    if (at) {
        uint64_t now = Psyz_KernelNowNs();
        uint64_t until = Psyz_KernelOfflineClock() ? UINT64_MAX : Psyz_VideoNsUntilBlank();
        uint64_t lead = 0;
        if (until != UINT64_MAX && at > now && at - now < until &&
            at - now + DRIVE_BLANK_LEAD_NS > until) {
            lead = at - now + DRIVE_BLANK_LEAD_NS - until;
        }
        Psyz_KernelWaitUntilNs(at - lead);
        Psyz_KernelLockInterrupts();
        drive.lead_ns = lead;
        Psyz_KernelUnlockInterrupts();
    }
    Psyz_CdServiceDrive();
    Psyz_KernelLockInterrupts();
    drive.lead_ns = 0;
    Psyz_KernelUnlockInterrupts();
}


static int need_cdda_rewind = 1;
// play on CDDA mode only (not XA)
static void psyz_play() {
    if (!(CD_mode & CdlModeDA)) {
        WARNF("CdlModeDA not active, audio will not be played");
        return;
    }
    if (is_playing) {
        DEBUGF("audio already playing");
        return;
    }
    data_read_active = 0;
    data_sector_valid = 0;
    if (!need_cdda_rewind) {
        Psyz_AudioLock();
        is_playing = 1;
        Psyz_AudioUnlock();
        return;
    }
    if (open_track_at_cd_pos() != 0) {
        return;
    }
    Psyz_AudioLock();
    cd_buf_pos = 0;
    cd_buf_count = 0;
    need_cdda_rewind = 0;
    is_playing = 1;
    Psyz_AudioUnlock();
}

// Begin XA streaming at CD_pos (CdlReadN / CdlReadS).
static void psyz_xa_read(void) {
    if (!(CD_mode & CdlModeRT)) {
        LOG_ONCE("CdlReadN/ReadS without CdlModeRT not implemented");
        return;
    }
    data_read_active = 0;
    data_sector_valid = 0;
    if (!disk_read_cb && open_track_at_cd_pos()) {
        return;
    }
    Psyz_AudioLock();
    xa_reset_stream();
    Psyz_SpuFlushCdSamples();
    xa.cur_abs_sector = CdPosToInt(&CD_pos) - 1;
    xa.active = 1;
    is_playing = 1;
    Psyz_AudioUnlock();
}

static void psyz_stop() {
    need_cdda_rewind = 1;
    Psyz_AudioLock();
    cd_delay_frames = 0;
    is_playing = 0;
    xa.active = 0;
    Psyz_SpuFlushCdSamples();
    data_read_active = 0;
    data_sector_valid = 0;
    close_track_file();
    cd_buf_pos = 0;
    cd_buf_count = 0;
    Psyz_AudioUnlock();
}

static void psyz_pause() {
    Psyz_AudioLock();
    cd_delay_frames = 0;
    is_playing = 0;
    xa.active = 0;
    Psyz_SpuFlushCdSamples();
    data_read_active = 0;
    data_sector_valid = 0;
    Psyz_AudioUnlock();
}

static void psyz_mute() {
    Psyz_AudioLock();
    is_muted = 1;
    Psyz_AudioUnlock();
}

static void psyz_demute() {
    Psyz_AudioLock();
    is_muted = 0;
    Psyz_AudioUnlock();
}



static void callback(void) { NOT_IMPLEMENTED; }

int CD_init(void) {
    Psyz_KernelLockInterrupts();
    CD_com = 0;
    Psyz_AudioLock();
    CD_mode = 0;
    Psyz_AudioUnlock();
    CD_cbready = 0;
    CD_cbsync = 0;
    CD_status1 = 0;
    CD_status = 0;
    ResetCallback();
    InterruptCallback(2, &callback);
    Psyz_KernelUnlockInterrupts();
    // (LIBCD's CD_init asks for no result: CdStatus has the shell's state)
    CD_cw(CdlNop, 0, 0, 0);
    if (CdStatus() & CdlStatShellOpen) {
        CD_cw(CdlNop, 0, 0, 0);
    }
    if (CD_cw(CdlReset, 0, 0, 0)) {
        return -1;
    }
    if (CD_cw(CdlDemute, 0, 0, 0)) {
        return -1;
    }
    if (CD_sync(0, 0) != 2) {
        return -1;
    }
    return 0;
}

void CD_initintr(void) { NOT_IMPLEMENTED; }
void CD_flush(void) { NOT_IMPLEMENTED; }

int CD_vol(CdlATV* vol) {
    Psyz_AudioLock();
    // 0x1F801800 = 2
    ctx.cd_src_l_dst_l = vol->val0; // 0x1F801802
    ctx.cd_src_l_dst_r = vol->val1; // 0x1F801803

    // 0x1F801800 = 3
    ctx.cd_src_r_dst_r = vol->val2; // 0x1F801801
    ctx.cd_src_r_dst_l = vol->val3; // 0x1F801802
    CHNGATV();                      // 0x1F801803 = 0x20
    Psyz_AudioUnlock();
    return 0;
}

int CD_getsector(void* madr, int size) {
    size_t bytes = (size_t)size * 4;
    if (!data_sector_valid || !madr)
        return -1;
    size_t offset = data_getsector_count++ == 0 ? 12u : 24u;
    if (offset + bytes > sizeof(data_sector))
        return -1;
    memcpy(madr, data_sector + offset, bytes);
    if (offset == 24u) {
        data_sector_valid = 0;
        data_sector_number++;
        data_getsector_count = 0;
    }
    return 0;
}
int CD_getsector2(void) {
    NOT_IMPLEMENTED;
    return 0; // LIBCD returns 0 once the DMA is started
}
void CD_datasync(int mode) { NOT_IMPLEMENTED; }

int CD_initvol(void) {
    CdlATV vol = {128, 0, 128, 0};
    if (SPUR(main_volx.left) == 0 && SPUR(main_volx.right) == 0) {
        SPUW(main_vol.left, 0x3FFF);
        SPUW(main_vol.right, 0x3FFF);
    }
    SPUW(cd_vol.left, 0x3FFF);
    SPUW(cd_vol.right, 0x3FFF);
    SPUW(spucnt, SPU_CTRL_MASK_SPU_ENABLE | SPU_CTRL_MASK_MUTE_SPU |
                     SPU_CTRL_MASK_CD_AUDIO_ENABLE);
    CD_vol(&vol);
    return 0;
}


/* CdSync: the state of the last command, which has completed: its
 * callback is delivered first. */
int CD_sync(int mode, u_char* result) {
    (void)mode;
    Psyz_CdServiceDrive();
    Psyz_KernelLockInterrupts();
    int intr = last_intr;
    if (result) {
        memcpy(result, last_result, last_result_len);
    }
    Psyz_KernelUnlockInterrupts();
    return intr;
}

// Polled reads (CdReady). A ready callback receives the sectors instead,
// and polled ADPCM playback (CdlModeRT) keeps this data path idle.
int CD_ready(int mode, u_char* result) {
    (void)mode;
    Psyz_CdServiceDrive();
    Psyz_KernelLockInterrupts();
    if (!data_read_active || CD_cbready || (CD_mode & CdlModeRT)) {
        Psyz_KernelUnlockInterrupts();
        return CdlNoIntr;
    }
    if (!data_sector_valid) {
        if (read_data_sector(data_sector_number) < 0) {
            Psyz_KernelUnlockInterrupts();
            return CdlDiskError;
        }
        drive.delivered++;
        data_sector_valid = 1;
        data_getsector_count = 0;
    }
    if (result) {
        result[0] = (u_char)CD_status;
    }
    Psyz_KernelUnlockInterrupts();
    return CdlDataReady;
}

/* StCdInterrupt: the ready-callback half of LIBCD streaming. It moves the
 * current sector into the ring StSetRing installed: StRingSize 32-byte
 * StHEADERs followed by as many 2016-byte (504 word) payload slots. A sector
 * of a movie frame carries its StHEADER in the first 32 bytes of the Mode 2
 * user data and 2016 bytes of frame data after it; the nSectors sectors of a
 * frame fill consecutive slots so the frame is contiguous. Slot ids: 0 free,
 * 1 wrap to slot 0, 2 frame ready, 4 frame held by StGetNext (StFreeRing
 * clears it), ST_SLOT_FILLING while a frame is assembled. When the last
 * sector of a frame lands, the CD DMA completion callback (CdRead2 installs
 * data_ready_callback) publishes it. A frame that does not fit in the free
 * slots, or misses a sector, is dropped, as on hardware. */

static int st_slots_free(int start, int count) {
    for (int i = start; i < start + count; i++) {
        if (StRingAddr[i].id != 0) {
            return 0;
        }
    }
    return 1;
}

static void st_drop_frame(void) {
    if (st_frame.active) {
        for (int i = 0; i < st_frame.next; i++) {
            StRingAddr[st_frame.slot + i].id = 0;
        }
    }
    st_frame.active = 0;
}

// A new frame starts: reserve its slots, wrapping to slot 0 when the rest
// of the ring cannot hold it. Leaves a free slot at the end of the ring, so
// the read index never passes the last header.
static int st_begin_frame(const StHEADER* h) {
    int count = h->nSectors;
    if (count <= 0 || count >= StRingSize) {
        return 0;
    }
    if (StSTART_FLAG && (h->frameCount < (u32)StStartFrame ||
                         h->frameCount > (u32)StEndFrame)) {
        return 0;
    }
    int pos = StRingIdx2;
    if (pos < 0 || pos >= StRingSize) {
        pos = 0;
    }
    int wrap = pos + count >= StRingSize;
    int start = wrap ? 0 : pos;
    if ((wrap && StRingAddr[pos].id != 0) || !st_slots_free(start, count)) {
        return 0; // ring full: the decoder is behind, skip this frame
    }
    if (wrap && pos != 0) {
        StRingAddr[pos].id = 1;
    }
    StRingIdx2 = start;
    st_frame.active = 1;
    st_frame.frame = h->frameCount;
    st_frame.slot = start;
    st_frame.next = 0;
    st_frame.count = count;
    return 1;
}


void StCdInterrupt(void) {
    StHEADER h;
    const u_char* sector_data = data_sector;
    int sector = drive_ready_sector;
    StCdIntrFlag = 0;
    if (sector < 0 || !StRingAddr || StRingSize < 2) {
        return; // DataEnd/DiskError or no ring: nothing to store
    }
    if (sector_data[0x12] & 0x04) {
        return; // XA audio interleaved with the frames
    }
    memcpy(&h, sector_data + 24, sizeof(h));
    if (h.secCount == 0) {
        st_drop_frame(); // previous frame lost a sector
        if (!st_begin_frame(&h)) {
            return;
        }
    } else if (!st_frame.active || h.frameCount != st_frame.frame ||
               h.secCount != st_frame.next || h.nSectors != st_frame.count) {
        st_drop_frame();
        return;
    }
    int slot = st_frame.slot + h.secCount;
    StHEADER* dst = &StRingAddr[slot];
    u_char* payload =
        (u_char*)&StRingAddr[StRingSize] + (size_t)slot * ST_SLOT_BYTES;
    memcpy(payload, sector_data + 24 + sizeof(StHEADER), ST_SLOT_BYTES);
    *dst = h;
    dst->id = ST_SLOT_FILLING;
    CdIntToPos(sector, &dst->loc);
    Stsector_offset = (s16)h.secCount;
    if (++st_frame.next < st_frame.count) {
        return;
    }
    st_frame.active = 0;
    Stframe_no = (s32)h.frameCount;
    StRingIdx1 = st_frame.slot + st_frame.count;
    StFinalSector = 1;
    Psyz_DmaComplete(3); // data_ready_callback publishes the frame
    if (StFunc2 && StEndFrame && h.frameCount >= (u32)StEndFrame) {
        StFunc2();
    }
}

static int CD_command(u_char com, u_char* param, u_char* result, s32 arg3);

/* LIBCD's CD_cw: the command is done at once. CD_cw waits for its
 * acknowledge, so its completion (CdSyncCallback) is delivered before it
 * returns; one not waited for (CdControlF) completes at the drive's next
 * service. A command the drive refuses answers with an error, which LIBCD's
 * handler hands to the ready callback too (CdlDiskError). */
int CD_cw(u_char com, u_char* param, u_char* result, s32 arg3) {
    Psyz_CdServiceDrive(); // the previous command's callback first
    Psyz_KernelLockInterrupts();
    drive.commanding++;
    CD_com = com; // CdLastCom
    drive.sync_cb_due = 1;
    drive_update_status();
    unsigned cached = last_result_count;
    int ret = CD_command(com, param, result, arg3);
    if (drive_trace()) {
        fprintf(stderr, "cd %" PRIu64 " %s pos %d ret %d%s\n", Psyz_KernelNowUs(),
                com < LEN(CD_comstr) ? CD_comstr[com] : "?", CdPosToInt(&CD_pos), ret,
                arg3 ? " (no wait)" : "");
    }
    if (ret >= 0 && last_result_count == cached) {
        // LIBCD's CD_cw clears the interrupt status and the acknowledge
        // sets it: a command accepted leaves no earlier error for CdSync
        u_char status = (u_char)CD_status;
        cache_last_result(&status, 1, CdlComplete);
    }
    if (ret < 0 && CD_cbready) {
        drive.error_ready_due = 1;
    }
    drive_update_status();
    Psyz_KernelRaise(HwCdRom, EvSpTRAP);
    drive_schedule();
    drive.commanding--;
    Psyz_KernelUnlockInterrupts();
    if (!arg3) {
        Psyz_CdServiceDrive();
    }
    return ret;
}

static int CD_command(u_char com, u_char* param, u_char* result, s32 arg3) {
    int total;
    int t;
    int abs_sector;

    (void)arg3;
    switch (com) {
    case CdlNop:
        if (shell_is_open) {
            if (result) {
                result[0] = (u_char)(CD_status | CdlStatError);
                result[1] = 0x80;
                result[2] = 0x00;
                result[3] = 0x00;
                cache_last_result(result, CD_STATUS_RESULT_LEN, CdlDiskError);
            }
            return -1;
        }
        if (result) {
            result[0] = (u_char)CD_status;
            result[1] = 0x00;
            result[2] = 0x00;
            result[3] = 0x00;
            cache_last_result(result, CD_STATUS_RESULT_LEN, CdlComplete);
        }
        break;
    case CdlSetloc:
        if (!param) {
            ERRORF("%s got NULL param", CD_comstr[com]);
            return -2;
        }
        CD_pos = *(CdlLOC*)param;
        drive.setloc_pending = 1;
        break;
    case CdlPlay:
        if (!is_playing || drive.setloc_pending) {
            drive.setloc_pending = 0;
            psyz_play();
        }
        break;
    case CdlReadN:
    case CdlReadS:
        // With CdlModeRT the drive plays XA audio sectors itself and hands
        // the remaining (data) sectors to the host, as interleaved movie
        // streams need; without it every sector is data.
        // Already reading, with no new position: the drive carries on.
        if (data_read_active && !drive.setloc_pending) {
            break;
        }
        drive.setloc_pending = 0;
        if (CD_mode & CdlModeRT) {
            psyz_xa_read();
        } else {
            Psyz_AudioLock();
            is_playing = 0;
            xa.active = 0;
            need_cdda_rewind = 1;
            Psyz_AudioUnlock();
        }
        start_data_read();
        break;
    case CdlSetfilter:
        if (!param) {
            ERRORF("%s got NULL param", CD_comstr[com]);
            return -2;
        }
        xa.filter_file = param[0];
        xa.filter_channel = param[1];
        break;
    case CdlStop:
        psyz_stop();
        CD_status &= ~(CdlStatRead | CdlStatSeek | CdlStatPlay | CdlStatStandby);
        break;
    case CdlPause:
        psyz_pause();
        CD_status &= ~(CdlStatRead | CdlStatSeek | CdlStatPlay);
        break;
    case CdlSeekL:
    case CdlSeekP:
        // the head moves to the Setloc position and holds there
        psyz_pause();
        drive.setloc_pending = 0;
        CD_status &= ~(CdlStatRead | CdlStatPlay);
        break;
    case CdlStandby:
        CD_status |= CdlStatStandby;
        break;
    case CdlReset:
        psyz_pause();
        drive.setloc_pending = 0;
        CD_status &= ~(CdlStatRead | CdlStatSeek | CdlStatPlay);
        CD_status |= CdlStatStandby;
        break;
    case CdlMute:
        psyz_mute();
        break;
    case CdlDemute:
        psyz_demute();
        break;
    case CdlSetmode:
        if (!param) {
            ERRORF("%s got NULL param", CD_comstr[com]);
            return -2;
        }
        if (*param & CdlModeDA) {
            Psyz_AudioInit();
        }
        if (*param & CdlModeRT) {
            Psyz_AudioInit();
        }
        if (*param & CdlModeAP) {
            LOG_ONCE("%s does not support CdlModeAP", CD_comstr[com]);
        }
        if (*param & CdlModeRept) {
            LOG_ONCE("%s does not support CdlModeRept", CD_comstr[com]);
        }
        if (*param & CdlModeSize0) {
            LOG_ONCE("%s does not support CdlModeSize0", CD_comstr[com]);
        }
        if (*param & CdlModeSize1) {
            LOG_ONCE("%s does not support CdlModeSize1", CD_comstr[com]);
        }
        // CdlModeSpeed sets the rate of a paced read: one under way carries
        // on at the new rate from here. The CdlModeStream bits never reach
        // the drive: CdRead2 keeps them in StMode and installs the stream
        // callbacks.
        if (((*param ^ CD_mode) & CdlModeSpeed) && data_read_active) {
            drive.base = drive.delivered;
            drive.start_ns = Psyz_KernelNowNs();
        }
        Psyz_AudioLock();
        CD_mode = *param;
        Psyz_AudioUnlock();
        break;
    case CdlGetlocL: {
        if (!result) {
            ERRORF("%s got NULL result", CD_comstr[com]);
            return -2;
        }
        // always return XA sector if XA playback is active
        Psyz_AudioLock();
        int cur = xa.active ? xa.cur_abs_sector : CdPosToInt(&CD_pos);
        if (cur < 0) {
            cur = 0;
        }
        CdlLOC loc;
        CdIntToPos(cur, &loc);
        result[0] = loc.minute;
        result[1] = loc.second;
        result[2] = loc.sector;
        result[3] = 0x01;
        result[4] = xa.filter_file;
        result[5] = xa.filter_channel;
        result[6] = 0x00;
        result[7] = 0x00;
        cache_last_result(result, 8, CdlComplete);
        Psyz_AudioUnlock();
        break;
    }
    case CdlGetlocP: {
        // Subchannel Q position: track, index, position within the track and
        // absolute position (MSF, BCD). The result may be fetched later with
        // CdSync, so a NULL buffer is allowed.
        u_char loc_result[8];
        Psyz_AudioLock();
        int cur = xa.active          ? xa.cur_abs_sector
                  : data_read_active ? data_sector_number
                                     : CdPosToInt(&CD_pos);
        Psyz_AudioUnlock();
        if (cur < 0) {
            cur = 0;
        }
        TrackEntry* track = find_track_for_sector(cur);
        int rel = track ? cur - track->abs_sector : cur;
        int track_num = track ? track->track_num : 1;
        CdlLOC abs_loc;
        CdIntToPos(cur, &abs_loc);
        loc_result[0] = (u_char)(((track_num / 10) << 4) | (track_num % 10));
        loc_result[1] = 0x01;
        loc_result[2] = (u_char)((((rel / 4500) / 10) << 4) | ((rel / 4500) % 10));
        loc_result[3] = (u_char)(((((rel / 75) % 60) / 10) << 4) | ((rel / 75) % 60 % 10));
        loc_result[4] = (u_char)((((rel % 75) / 10) << 4) | (rel % 75 % 10));
        loc_result[5] = abs_loc.minute;
        loc_result[6] = abs_loc.second;
        loc_result[7] = abs_loc.sector;
        if (result) {
            memcpy(result, loc_result, sizeof(loc_result));
        }
        cache_last_result(loc_result, sizeof(loc_result), CdlComplete);
        break;
    }
    case CdlGetTN:
        if (!result) {
            ERRORF("%s got NULL result", CD_comstr[com]);
            return -2;
        }
        if (shell_is_open || !is_disk_loaded || g_track_count == 0) {
            result[0] = (u_char)(CD_status | CdlStatError);
            result[1] = 0x80; // error-response sentinel
            result[2] = 0x00;
            result[3] = 0x00;
            cache_last_result(result, CD_STATUS_RESULT_LEN, CdlDiskError);
            return -1;
        }
        result[0] = (u_char)CD_status;
        result[1] = itob(1);
        result[2] = itob(g_track_count);
        result[3] = 0x00;
        cache_last_result(result, CD_STATUS_RESULT_LEN, CdlComplete);
        break;
    case CdlGetTD:
        if (!param || !result) {
            ERRORF("%s got NULL param/result", CD_comstr[com]);
            return -2;
        }
        if (shell_is_open || !is_disk_loaded || g_track_count == 0) {
            result[0] = (u_char)(CD_status | CdlStatError);
            result[1] = 0x80;
            result[2] = 0x00;
            result[3] = 0x00;
            cache_last_result(result, CD_STATUS_RESULT_LEN, CdlDiskError);
            return -1;
        }
        t = btoi(param[0]);
        if (t == 0) {
            abs_sector = lead_out_sector();
        } else if (t < 1 || t > g_track_count || !g_tracks[t - 1].is_valid) {
            result[0] = (u_char)(CD_status | CdlStatError);
            result[1] = 0x80;
            result[2] = 0x00;
            result[3] = 0x00;
            cache_last_result(result, CD_STATUS_RESULT_LEN, CdlDiskError);
            return -1;
        } else {
            abs_sector = g_tracks[t - 1].abs_sector;
        }
        // The +150 lead-in pregap is part of the controller-reported MSF.
        total = abs_sector + 150;
        result[0] = (u_char)CD_status;
        result[1] = itob(total / 75 / 60);
        result[2] = itob(total / 75 % 60);
        result[3] = 0x00;
        cache_last_result(result, CD_STATUS_RESULT_LEN, CdlComplete);
        break;
    default:
        if (com >= LEN(CD_comstr)) {
            ERRORF("com %X invalid", com);
            return -1;
        }
        LOG_ONCE("com %s not implemented", CD_comstr[com]);
    }
    return 0;
}

// LIBCD's: up to five attempts at one Init with the volume reset. (LIBCD
// then installs callbacks delivering HwCdRom events; PsyZ raises those from
// CD_cw, and a ready callback would take the sectors of polled reads.)
int CdInit(void) {
    for (int i = 0; i < 5; i++) {
        if (CD_init() == 0 && CD_initvol() == 0) {
            return 1;
        }
    }
    return 0;
}

int CdReading() {
    NOT_IMPLEMENTED;
    return 0;
}

void ExecCd() { NOT_IMPLEMENTED; }

/* CdRead, CdReadSync and CdReadCallback are LIBCD's (decomp cdread.c): a
 * ready callback that takes each sector the drive delivers. */
