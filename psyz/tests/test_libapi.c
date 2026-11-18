#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ztest.h"
#include <psyz.h>
#include <kernel.h>
#include <libapi.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir(path) _rmdir(path)
#define PATH_SEP "\\"
#elif defined(__psx__)
// No host filesystem on the console; the groups using it skip on ps1.
#define mkdir(path, mode) (-1)
#define rmdir(path) (-1)
#define PATH_SEP "/"
#else
#include <sys/stat.h>
#define PATH_SEP "/"
#endif

// Fixture steps must run in release builds too, where NDEBUG removes the
// whole expression of an assert().
#define BU_CHECK(cond)                                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: fixture step failed: %s\n", __FILE__,      \
                    __LINE__, #cond);                                          \
            abort();                                                           \
        }                                                                      \
    } while (0)

static void bu_make_file(const char* path, int len, int val, int inc) {
    FILE* f = fopen(path, "wb");
    BU_CHECK(f != NULL);
    unsigned char* d = (unsigned char*)malloc(len);
    BU_CHECK(d != NULL);
    for (int i = 0; i < len; i++) {
        d[i] = val;
        val += inc;
    }
    BU_CHECK(fwrite(d, 1, len, f) == (size_t)len);
    BU_CHECK(fclose(f) == 0);
    free(d);
}

ZTEST_SETUP(bu) {
    zskip_targets("ps1");
    mkdir("bu00", 0755);
    mkdir("bu10", 0755);
    bu_make_file("bu00/BASLUS-00000PSYZ00", 8192, 0, 1);
    bu_make_file("bu00/BASLUS-00000PSYZ01", 8192, 0, 2);
}

ZTEST_TEARDOWN(bu) {
    zskip_targets("ps1");
    BU_CHECK(remove("bu00/BASLUS-00000PSYZ00") == 0);
    BU_CHECK(remove("bu00/BASLUS-00000PSYZ01") == 0);
    BU_CHECK(rmdir("bu00") == 0);
    BU_CHECK(rmdir("bu10") == 0);
}

ZTEST(bu, for_invalid_path) {
    struct DIRENTRY d = {0};
    struct DIRENTRY* pd;
    pd = firstfile((char*)"none:", &d);
    zexpect_ptr_eq(NULL, pd);
    zexpect_str_eq("", d.name);
    zexpect_s32_eq(0, d.attr);
    zexpect_s32_eq(0, d.size);
}

ZTEST(bu, for_empty_path) {
    struct DIRENTRY d = {0};
    struct DIRENTRY* pd;
    pd = firstfile((char*)"bu10:", &d);
    zexpect_ptr_eq(NULL, pd);
    zexpect_str_eq("", d.name);
    zexpect_s32_eq(0, d.attr);
    zexpect_s32_eq(0, d.size);
}

ZTEST(bu, for_first_file) {
    struct DIRENTRY d = {0};
    struct DIRENTRY* pd;
    pd = firstfile((char*)"bu00:", &d);
    zexpect_ptr_eq(&d, pd);
    zexpect_str_eq("BASLUS-00000PSYZ00", d.name);
    zexpect_s32_eq(80, d.attr);
    zexpect_s32_eq(8192, d.size);
}

ZTEST(bu, for_multiple_files) {
    struct DIRENTRY d[2];
    struct DIRENTRY* pd;
    pd = nextfile(firstfile((char*)"bu00:", d) + 1);
    zexpect_ptr_eq(&d[1], pd);
    zexpect_str_eq("BASLUS-00000PSYZ01", pd->name);
    zexpect_s32_eq(80, pd->attr);
    zexpect_s32_eq(8192, pd->size);
}

ZTEST(bu, for_end_of_file_list) {
    struct DIRENTRY d[3];
    struct DIRENTRY* pd;
    pd = nextfile(nextfile(firstfile((char*)"bu00:", d) + 1) + 2);
    zexpect_ptr_eq(NULL, pd);
}

ZTEST(bu, open_existing_file) {
    int fd = open("bu00:BASLUS-00000PSYZ00", FREAD);
    zexpect_s32_ne(-1, fd);
    close(fd);
}

ZTEST(bu, create_and_write_file) {
    unsigned short expected = 1234, actual;
    int fd = open("bu00:BASLUS-00000PSYZ00", FCREAT);
    zexpect_s32_ne(-1, fd);
    write(fd, (char*)&expected, sizeof(unsigned short));
    close(fd);

    FILE* f = fopen("bu00/BASLUS-00000PSYZ00", "rb");
    int read = fread(&actual, 1, sizeof(unsigned short), f);
    fclose(f);
    zexpect_s32_eq(sizeof(unsigned short), read);
    zexpect_u16_eq(expected, actual);
}

ZTEST(bu, reopen_existing_file_for_write_and_read_write) {
    const char* name = "bu00:BASLUS-00000PSYZ00";
    const unsigned char replacement[] = {0x5A, 0xA5, 0x36, 0xC9};
    unsigned char actual[4] = {0};
    int fd = open(name, FWRITE);
    zassert_s32_ne(-1, fd);
    zexpect_s32_eq(4, write(fd, (char*)replacement, 4));
    close(fd);
    fd = open(name, FREAD | FWRITE);
    zassert_s32_ne(-1, fd);
    zexpect_s32_eq(4, read(fd, actual, 4));
    zexpect_u8array_eq(replacement, actual, 4);
    zexpect_s32_eq(0, lseek(fd, 0, SEEK_SET));
    zexpect_s32_eq(2, write(fd, (char*)replacement + 2, 2));
    close(fd);
    fd = open(name, FREAD);
    zassert_s32_ne(-1, fd);
    zexpect_s32_eq(4, read(fd, actual, 4));
    zexpect_u16_eq(0xC936, (unsigned)actual[0] | ((unsigned)actual[1] << 8));
    close(fd);
}

ZTEST(path_adjustment, basic_memory_card_path) {
    char dst[256] = {0};
    Psyz_AdjustPath(dst, "bu00:TESTFILE", sizeof(dst));
    zexpect_str_eq("bu00" PATH_SEP "TESTFILE", dst);
}

ZTEST(path_adjustment, basic_path_no_adjustment) {
    char dst[256] = {0};
    Psyz_AdjustPath(dst, "regular/path/file.txt", sizeof(dst));
    zexpect_str_eq("regular/path/file.txt", dst);
}

static int test_callback_override(char* dst, const char* src, int maxlen) {
    if (strncmp(src, "custom:", 7) == 0) {
        snprintf(dst, maxlen, "overridden/%s", src + 7);
        return (int)strlen(dst);
    }
    return -1;
}

ZTEST(path_adjustment, callback_override_positive_return) {
    char dst[256] = {0};
    Psyz_AdjustPathCB(test_callback_override);
    Psyz_AdjustPath(dst, "custom:myfile", sizeof(dst));
    zexpect_str_eq("overridden/myfile", dst);
    Psyz_AdjustPathCB(NULL);
}

ZTEST(path_adjustment, callback_negative_return_falls_back) {
    char dst[256] = {0};
    Psyz_AdjustPathCB(test_callback_override);
    Psyz_AdjustPath(dst, "bu00:FALLBACK", sizeof(dst));
    zexpect_str_eq("bu00" PATH_SEP "FALLBACK", dst);
    Psyz_AdjustPathCB(NULL);
}

ZTEST(path_adjustment, callback_null_uses_internal) {
    char dst[256] = {0};
    Psyz_AdjustPathCB(NULL);
    Psyz_AdjustPath(dst, "bu10:TESTFILE", sizeof(dst));
    zexpect_str_eq("bu10" PATH_SEP "TESTFILE", dst);
}

ZTEST(path_adjustment, preserves_twenty_byte_filenames) {
    char dst[256] = {0};
    Psyz_AdjustPath(dst, "bu00:1234567890123456789", sizeof(dst));
    zexpect_str_eq("bu00" PATH_SEP "1234567890123456789", dst);
    Psyz_AdjustPath(dst, "bu00:12345678901234567890", sizeof(dst));
    zexpect_str_eq("bu00" PATH_SEP "12345678901234567890", dst);
    Psyz_AdjustPath(dst, "bu00:123456789012345678901", sizeof(dst));
    zexpect_str_eq("bu00" PATH_SEP "12345678901234567890", dst);
}

static int test_callback_with_long_name(
    char* dst, const char* src, int maxlen) {
    if (strncmp(src, "custom:", 7) == 0) {
        snprintf(dst, maxlen, "custom_dir/%s", src + 7);
        return (int)strlen(dst);
    }
    return -1;
}

ZTEST(path_adjustment, callback_result_also_truncated) {
    char dst[256] = {0};
    Psyz_AdjustPathCB(test_callback_with_long_name);
    Psyz_AdjustPath(dst, "custom:VERYLONGFILENAME1234567890", sizeof(dst));
    zexpect_str_eq("custom_dir/VERYLONGFILENAME1234", dst);
    Psyz_AdjustPathCB(NULL);
}

#define LONG_FILENAME_21 "BISLUS-00005aaa195120"
#define TRUNCATED_NAME_20 "BISLUS-00005aaa19512"

ZTEST_SETUP(truncation) {
    zskip_targets("ps1");
    mkdir("bu00", 0755);
}

ZTEST_TEARDOWN(truncation) {
    zskip_targets("ps1");
    remove("bu00/BISLUS-00005aaa195120");
    remove("bu00/BISLUS-00005aaa19512");
    remove("bu00/AAAA-SHORT");
    rmdir("bu00");
}

ZTEST(truncation, open_truncates_long_filename) {
    FILE* f = fopen("bu00/BISLUS-00005aaa19512", "wb");
    zassert_ptr_ne(NULL, f);
    fclose(f);

    int fd = open("bu00:BISLUS-00005aaa195120", FREAD);
    zprintf("open() should truncate filename and find the 20-char file\n");
    zexpect_s32_ne(-1, fd);
    if (fd != -1) {
        close(fd);
    }
}

ZTEST(truncation, firstfile_truncates_long_filename) {
    FILE* f = fopen("bu00/BISLUS-00005aaa195120", "wb");
    zassert_ptr_ne(NULL, f);
    fclose(f);

    struct DIRENTRY d = {0};
    struct DIRENTRY* pd = firstfile((char*)"bu00:", &d);

    zprintf("firstfile() should find the file\n");
    zassert_ptr_ne(NULL, pd);
    zprintf("File name should be bounded to 20 bytes\n");
    zexpect_u8array_eq(TRUNCATED_NAME_20, d.name, 20);
}

ZTEST(truncation, nextfile_truncates_long_filename) {
    FILE* f1 = fopen("bu00/BISLUS-00005aaa195120", "wb");
    zassert_ptr_ne(NULL, f1);
    fclose(f1);

    FILE* f2 = fopen("bu00/AAAA-SHORT", "wb");
    zassert_ptr_ne(NULL, f2);
    fclose(f2);

    struct DIRENTRY d[2] = {0};
    struct DIRENTRY* pd;

    pd = firstfile((char*)"bu00:", &d[0]);
    zassert_ptr_ne(NULL, pd);
    zexpect_str_eq("AAAA-SHORT", d[0].name);

    pd = nextfile(&d[1]);
    zprintf("nextfile() should find the second file\n");
    zassert_ptr_ne(NULL, pd);
    zprintf("File name should be bounded to 20 bytes\n");
    zexpect_u8array_eq(TRUNCATED_NAME_20, d[1].name, 20);
}

#if defined(__psyz) && !defined(__PSP__)
#include <SDL3/SDL.h>

// Memory cards in a directory whose UTF-8 name is not ASCII, as a profile
// directory ("C:\Users\Zoë\...") gives it: save a file the way games do
// (create, write, reopen, seek, read back), list it with its size and erase
// it. The narrow CRT calls of the Windows backend failed all of these.
#define CARD_UTF8_DIR "psyz_card_\xc3\xab\xe6\x97\xa5"

static int card_utf8_path(char* dst, const char* src, int maxlen) {
    if (strncmp(src, "bu00:", 5) != 0) {
        return -1;
    }
    snprintf(dst, maxlen, CARD_UTF8_DIR "/%s", src + 5);
    return (int)strlen(dst);
}

ZTEST_SETUP(card_utf8) {
    SDL_CreateDirectory(CARD_UTF8_DIR);
    Psyz_AdjustPathCB(card_utf8_path);
}

ZTEST_TEARDOWN(card_utf8) {
    Psyz_AdjustPathCB(NULL);
    SDL_RemovePath(CARD_UTF8_DIR "/BESCES-00000RR4TEST");
    SDL_RemovePath(CARD_UTF8_DIR);
}

ZTEST(card_utf8, save_list_reload_and_erase) {
    static unsigned char block[8192], back[8192];
    const char* name = "bu00:BESCES-00000RR4TEST";
    for (int i = 0; i < (int)sizeof(block); i++) {
        block[i] = (unsigned char)(i * 7 + (i >> 8));
    }
    block[100] = 0x0A; // a text-mode write would expand these
    block[101] = 0x0D;
    block[102] = 0x1A; // and a text-mode read stop at this one

    int fd = open(name, FCREAT | (1 << 16));
    zassert_s32_ne(-1, fd);
    close(fd);
    fd = open(name, FWRITE);
    zassert_s32_ne(-1, fd);
    zexpect_s32_eq(8192, write(fd, (char*)block, 8192));
    close(fd);

    struct DIRENTRY d = {0};
    zassert_ptr_eq(&d, firstfile((char*)"bu00:", &d));
    zexpect_str_eq("BESCES-00000RR4TEST", d.name);
    zexpect_s32_eq(8192, d.size);

    fd = open(name, FREAD);
    zassert_s32_ne(-1, fd);
    zexpect_s32_eq(8192, read(fd, back, 8192));
    zexpect_u8array_eq(block, back, 8192);
    zexpect_s32_eq(4096, lseek(fd, 4096, SEEK_SET));
    zexpect_s32_eq(256, read(fd, back, 256));
    zexpect_u8array_eq(block + 4096, back, 256);
    zprintf("a negative SEEK_CUR offset moves back\n");
    zexpect_s32_eq(4096, lseek(fd, (unsigned int)-256, SEEK_CUR));
    close(fd);

    zexpect_s32_ne(0, erase((char*)name));
    zexpect_s32_eq(-1, open(name, FREAD));
}
#endif

#ifndef __psx__
#include <libetc.h>
#include <psyz/input.h>

// The controllers' side of the serial exchange (src/internal.h)
int Psyz_PadsTransfer(int port, const char* frame, unsigned char in,
                      unsigned char* out);
void Psyz_PadsDeselect(int port);

void Psyz_VSyncBlank(void);

// Emulated controller frames (pad.c) and libpad's view of them (libetc.c).
static const PsyzAxisResponse identity_response = {0.0f, 1.0f, 0.0f, 1.0f, 0xFF};

ZTEST_SETUP(pad_frames) {
    for (int a = 0; a < PSYZ_PAD_AXIS_COUNT; a++) {
        Psyz_PadsSetAxisResponse((PsyzPadAxis)a, &identity_response);
    }
}

ZTEST_TEARDOWN(pad_frames) {
    Psyz_PadsSetKind(0, 0, PSYZ_CTRL_DIGITAL_PAD);
    Psyz_PadsSetKind(1, 0, PSYZ_CTRL_DIGITAL_PAD);
}

static PsyzPadInput centred_input(void) {
    PsyzPadInput in;
    memset(&in, 0, sizeof(in));
    in.right_x = in.right_y = in.left_x = in.left_y = 0x80;
    return in;
}

ZTEST(pad_frames, digital_and_dualshock) {
    char f[PSYZ_PAD_BUF_LEN];
    PsyzPadInput in = centred_input();
    in.buttons = PADRdown | PADLleft | PADstart;
    in.left_x = 0x10;
    zassert_s32_eq(0, Psyz_PadsBuildFrame(0, PSYZ_CTRL_DIGITAL_PAD, &in, f));
    zexpect_u8_eq(0x00, f[0]);
    zexpect_u8_eq(0x41, f[1]);
    zexpect_u8_eq(0x77, f[2]); // Start (bit 3), Left (bit 7)
    zexpect_u8_eq(0xBF, f[3]); // Cross (bit 6)
    zassert_s32_eq(0, Psyz_PadsBuildFrame(0, PSYZ_CTRL_ANALOG_PAD, &in, f));
    zexpect_u8_eq(0x73, f[1]);
    zexpect_u8_eq(0x80, f[4]);
    zexpect_u8_eq(0x80, f[5]);
    zexpect_u8_eq(0x10, f[6]);
    zexpect_u8_eq(0x80, f[7]);
}

ZTEST(pad_frames, negcon) {
    char f[PSYZ_PAD_BUF_LEN];
    PsyzPadInput in = centred_input();
    zassert_s32_eq(0, Psyz_PadsBuildFrame(0, PSYZ_CTRL_NEGCON, &in, f));
    zexpect_u8_eq(0x23, f[1]);
    zexpect_u8_eq(0xFF, f[2]);
    zexpect_u8_eq(0xFF, f[3]);
    zexpect_u8_eq(0x80, f[4]); // twist centre
    zexpect_u8_eq(0x00, f[5]);
    zexpect_u8_eq(0x00, f[6]);
    zexpect_u8_eq(0x00, f[7]);
    // DuckStation's units: twist -1..1 -> 0x00..0xFF, pedals 0..1 -> 0..0xFF
    in.steering = 0.5f;
    in.pedal[0] = 1.0f;
    in.pedal[1] = 0.5f;
    in.buttons = PADRright | PADRup | PADR1 | PADselect | PADL2;
    Psyz_PadsBuildFrame(0, PSYZ_CTRL_NEGCON, &in, f);
    zexpect_u8_eq(0xC0, f[4]);
    zexpect_u8_eq(0xFF, f[5]);
    zexpect_u8_eq(0x80, f[6]);
    zexpect_u8_eq(0xFF, f[2]); // no Select
    zexpect_u8_eq(0xC7, f[3]); // A (Circle's bit), B (Triangle's), R; no L2
    in.steering = -1.0f;
    Psyz_PadsBuildFrame(0, PSYZ_CTRL_NEGCON, &in, f);
    zexpect_u8_eq(0x00, f[4]);
    // digital stand-ins: Right steers fully, Cross / Square / L1 press
    // I / II / L fully, and none of them is sent as a button
    in = centred_input();
    in.buttons = PADLright | PADRdown | PADRleft | PADL1;
    Psyz_PadsBuildFrame(0, PSYZ_CTRL_NEGCON, &in, f);
    zexpect_u8_eq(0xFF, f[4]);
    zexpect_u8_eq(0xFF, f[5]);
    zexpect_u8_eq(0xFF, f[6]);
    zexpect_u8_eq(0xFF, f[7]);
    zexpect_u8_eq(0xDF, f[2]); // Right
    zexpect_u8_eq(0xFF, f[3]);
}

ZTEST(pad_frames, axis_response) {
    char f[PSYZ_PAD_BUF_LEN];
    PsyzPadInput in = centred_input();
    PsyzAxisResponse r = {0.5f, 1.0f, 0.0f, 1.0f, 0x6A};
    zassert_s32_eq(0, Psyz_PadsSetAxisResponse(PSYZ_PAD_AXIS_I, &r));
    in.pedal[0] = 0.75f; // halfway past the dead zone
    Psyz_PadsBuildFrame(0, PSYZ_CTRL_NEGCON, &in, f);
    zexpect_u8_eq(0x35, f[5]);
    in.pedal[0] = 0.25f;
    Psyz_PadsBuildFrame(0, PSYZ_CTRL_NEGCON, &in, f);
    zexpect_u8_eq(0x00, f[5]);
    // e^linearity: linearity ln(2) squares the travelled fraction
    r = (PsyzAxisResponse){0.0f, 1.0f, 0.693147f, 1.0f, 0};
    zexpect_s32_eq(25, (int)(Psyz_AxisCurve(0.5f, &r) * 100.0f + 0.5f));
    r = (PsyzAxisResponse){0.0f, 0.5f, 0.0f, 1.0f, 0};
    zexpect_s32_eq(100, (int)(Psyz_AxisCurve(0.6f, &r) * 100.0f + 0.5f));
    r = (PsyzAxisResponse){0.0f, 1.0f, 0.0f, 0.5f, 0};
    zexpect_s32_eq(50, (int)(Psyz_AxisCurve(1.0f, &r) * 100.0f + 0.5f));
    zexpect_s32_eq(-1, Psyz_PadsSetAxisResponse(PSYZ_PAD_AXIS_COUNT, &r));
}

ZTEST(pad_frames, jogcon_frame) {
    char f[PSYZ_PAD_BUF_LEN];
    PsyzPadInput in = centred_input();
    Psyz_PadsBuildFrame(1, PSYZ_CTRL_JOGCON, &in, f);
    zexpect_u8_eq(0xE3, f[1]);
    zexpect_u8_eq(0x00, f[4]);
    in.steering = 1.0f;
    Psyz_PadsBuildFrame(1, PSYZ_CTRL_JOGCON, &in, f);
    zexpect_u8_eq(0x7F, f[4]);
    zexpect_u8_eq(0x00, f[5]);
    in.steering = -1.0f;
    Psyz_PadsBuildFrame(1, PSYZ_CTRL_JOGCON, &in, f);
    zexpect_u8_eq(0x80, f[4]); // -128
    zexpect_u8_eq(0xFF, f[5]);
}

/* One command as libpad clocks it: the address byte 0x01, then the n bytes
 * of tx; rx gets the replies to them, the result is how many were ACKed
 * (the address byte's included). */
static int pad_command(int port, const char* frame, const unsigned char* tx,
                       int n, unsigned char* rx) {
    unsigned char out;
    int acked = Psyz_PadsTransfer(port, frame, 0x01, &out);
    for (int i = 0; i < n && acked == i + 1; i++) {
        acked += Psyz_PadsTransfer(port, frame, tx[i], &rx[i]);
    }
    Psyz_PadsDeselect(port);
    return acked;
}

// The DUAL SHOCK answers as DuckStation's analog_controller.cpp does.
ZTEST(pad_frames, dualshock_commands) {
    char f[PSYZ_PAD_BUF_LEN];
    unsigned char rx[8];
    PsyzPadInput in = centred_input();
    static const unsigned char poll[8] = {0x42, 0, 0, 0, 0, 0, 0, 0};
    static const unsigned char enter[8] = {0x43, 0, 1, 0, 0, 0, 0, 0};
    static const unsigned char leave[8] = {0x43, 0, 0, 0, 0, 0, 0, 0};
    static const unsigned char status[8] = {0x45, 0, 0, 0, 0, 0, 0, 0};
    static const unsigned char act1[8] = {0x46, 0, 1, 0, 0, 0, 0, 0};
    static const unsigned char digital[8] = {0x44, 0, 0, 3, 0, 0, 0, 0};
    static const unsigned char map[8] = {0x4D, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0xFF};
    static const unsigned char motors[8] = {0x42, 0, 1, 0xC0, 0, 0, 0, 0};
    unsigned low, high;

    Psyz_PadsSetKind(0, 0, PSYZ_CTRL_ANALOG_PAD);
    in.buttons = PADstart;
    in.left_x = 0x10;
    Psyz_PadsBuildFrame(0, PSYZ_CTRL_ANALOG_PAD, &in, f);
    // analog mode: ID 0x73, the sticks; the last byte is not ACKed
    zexpect_s32_eq(8, pad_command(0, f, poll, 8, rx));
    zexpect_u8_eq(0x73, rx[0]);
    zexpect_u8_eq(0x5A, rx[1]);
    zexpect_u8_eq(0xF7, rx[2]);
    zexpect_u8_eq(0x10, rx[6]);
    zexpect_s32_eq(8, pad_command(0, f, enter, 8, rx));
    zexpect_u8_eq(0x73, rx[0]); // still the pad's answer
    zexpect_s32_eq(8, pad_command(0, f, status, 8, rx));
    zexpect_u8_eq(0xF3, rx[0]);
    zexpect_u8_eq(0x01, rx[2]);
    zexpect_u8_eq(0x02, rx[3]);
    zexpect_u8_eq(0x01, rx[4]); // analog
    zexpect_u8_eq(0x02, rx[5]);
    zexpect_s32_eq(8, pad_command(0, f, act1, 8, rx));
    zexpect_u8_eq(0x01, rx[4]);
    zexpect_u8_eq(0x01, rx[5]);
    zexpect_u8_eq(0x01, rx[6]);
    zexpect_u8_eq(0x14, rx[7]);
    // the motors: actuator byte 0 the small one, byte 1 the large one
    zexpect_s32_eq(8, pad_command(0, f, map, 8, rx));
    zexpect_u8_eq(0xFF, rx[2]); // the mapping before
    zexpect_s32_eq(8, pad_command(0, f, leave, 8, rx));
    zexpect_s32_eq(8, pad_command(0, f, motors, 8, rx));
    Psyz_PadsGetRumble(0, &low, &high);
    zexpect_u32_eq(0xC0C0, low);
    zexpect_u32_eq(0xFFFF, high);
    // the commands past 0x43 only in configuration mode
    zexpect_s32_eq(1, pad_command(0, f, status, 8, rx));
    zexpect_s32_eq(8, pad_command(0, f, enter, 8, rx));
    zexpect_s32_eq(8, pad_command(0, f, digital, 8, rx));
    Psyz_PadsGetRumble(0, &low, &high); // 0x44 clears the mapping
    zexpect_u32_eq(0, low | high);
    zexpect_s32_eq(8, pad_command(0, f, leave, 8, rx));
    zexpect_s32_eq(1, Psyz_PadsDigitalMode(0));
    // digital mode: ID 0x41 and the left stick on the D-pad
    zexpect_s32_eq(4, pad_command(0, f, poll, 4, rx));
    zexpect_u8_eq(0x41, rx[0]);
    zexpect_u8_eq(0x77, rx[2]); // Start and Left
    // a digital pad takes the poll only
    Psyz_PadsSetKind(0, 0, PSYZ_CTRL_DIGITAL_PAD);
    Psyz_PadsBuildFrame(0, PSYZ_CTRL_DIGITAL_PAD, &in, f);
    zexpect_s32_eq(1, pad_command(0, f, enter, 4, rx));
    zexpect_s32_eq(4, pad_command(0, f, poll, 4, rx));
    zexpect_u8_eq(0x41, rx[0]);
    zexpect_u8_eq(0x5A, rx[1]);
    // no controller: nothing is ACKed
    Psyz_PadsBuildFrame(0, PSYZ_CTRL_DISCONNECTED, &in, f);
    zexpect_s32_eq(0, pad_command(0, f, poll, 4, rx));
}

// The controllers' frames at the next blanks (Psyz_SetPadsInputCb).
static char store_frames[2][PSYZ_PAD_BUF_LEN];
static void store_input(void) {
    Psyz_PadsSet(0, store_frames[0], PSYZ_PAD_BUF_LEN);
    Psyz_PadsSet(1, store_frames[1], PSYZ_PAD_BUF_LEN);
}

static void store_set(int port, PsyzControllerKind kind, unsigned buttons) {
    PsyzPadInput in = centred_input();
    in.buttons = buttons;
    Psyz_PadsSetKind(port, 0, kind == PSYZ_CTRL_DISCONNECTED
                                  ? PSYZ_CTRL_DIGITAL_PAD
                                  : kind);
    Psyz_PadsBuildFrame(port, kind, &in, store_frames[port]);
}

static void blanks(int n) {
    while (n-- > 0) {
        Psyz_VSyncBlank();
    }
}

/* libpad finds a DUAL SHOCK's modes, actuators and combinations over 21
 * exchanges after its first poll (as on DuckStation: 0x43, 0x45 0x45,
 * 0x4C 0x4C, 0x47 0x47, 0x4C twice per mode, 0x46 twice per actuator, 0x47
 * twice per combination, 0x43 0x43: the first exit is still answered in
 * configuration mode); meanwhile the receive buffer keeps the pad's
 * answers and PadGetState reads 1 .. 4. */
ZTEST(pad_frames, libpad_dualshock) {
    static unsigned char buf[2][PSYZ_PAD_BUF_LEN];
    static char align[6] = {0, 1, (char)0xFF, (char)0xFF, (char)0xFF,
                            (char)0xFF};
    static unsigned char act[2];
    unsigned low, high;
    int n;
    PsyzPadsInputCb prev = Psyz_SetPadsInputCb(store_input);
    store_set(0, PSYZ_CTRL_ANALOG_PAD, 0);
    store_set(1, PSYZ_CTRL_DISCONNECTED, 0);
    PadInitDirect(buf[0], buf[1]);
    PadStartCom();
    blanks(2); // the first poll reaches the buffer
    zexpect_u8_eq(0x00, buf[0][0]);
    zexpect_u8_eq(0x73, buf[0][1]);
    zexpect_u8_eq(0xFF, buf[0][2]);
    store_set(0, PSYZ_CTRL_ANALOG_PAD, PADstart);
    for (n = 2; n < 40 && PadGetState(0x00) != 6; n++) {
        zexpect_s32_ge(1, PadGetState(0x00));
        zexpect_s32_le(4, PadGetState(0x00));
        zexpect_u8_eq(0x73, buf[0][1]); // configuration answers not stored
        Psyz_VSyncBlank();
    }
    zexpect_s32_eq(22, n); // then a poll and its packet
    zexpect_u8_eq(0xF7, buf[0][2]);
    zexpect_s32_eq(7, PadInfoMode(0x00, 1, 0));
    zexpect_s32_eq(7, PadInfoMode(0x00, 2, 0));
    zexpect_s32_eq(1, PadInfoMode(0x00, 3, 0));
    zexpect_s32_eq(2, PadInfoMode(0x00, 4, -1));
    zexpect_s32_eq(4, PadInfoMode(0x00, 4, 0));
    zexpect_s32_eq(7, PadInfoMode(0x00, 4, 1));
    zexpect_s32_eq(2, PadInfoAct(0x00, -1, 0));
    zexpect_s32_eq(2, PadInfoAct(0x00, 0, 2));
    zexpect_s32_eq(0, PadInfoAct(0x00, 0, 3));
    zexpect_s32_eq(10, PadInfoAct(0x00, 0, 4));
    zexpect_s32_eq(1, PadInfoAct(0x00, 1, 3));
    zexpect_s32_eq(20, PadInfoAct(0x00, 1, 4));
    zexpect_s32_eq(1, PadInfoComb(0x00, -1, 0));
    zexpect_s32_eq(2, PadInfoComb(0x00, 0, -1));
    zexpect_s32_eq(1, PadInfoComb(0x00, 0, 1));
    zexpect_s32_eq(0, PadInfoMode(0x10, 1, 0)); // port 2: nothing
    zexpect_s32_eq(0, PadGetState(0x10));
    // the actuators: 0x43, 0x4D, 0x43 0x43, from the next exchange on
    act[0] = 1;
    act[1] = 0x80;
    PadSetAct(0x00, act, 2);
    zexpect_s32_eq(1, PadSetActAlign(0x00, align));
    zexpect_s32_eq(0, PadSetActAlign(0x00, align)); // busy
    Psyz_VSyncBlank();
    for (n = 1; n < 10 && PadGetState(0x00) != 6; n++) {
        zexpect_s32_eq(4, PadGetState(0x00));
        Psyz_VSyncBlank();
    }
    zexpect_s32_eq(6, n);
    Psyz_PadsGetRumble(0, &low, &high);
    zexpect_u32_eq(0x8080, low);
    zexpect_u32_eq(0xFFFF, high);
    // byte 0 goes out through the gate its value opened at the exchange
    // before: the small motor stops at once, and starts an exchange late
    act[0] = 0;
    Psyz_VSyncBlank();
    Psyz_PadsGetRumble(0, &low, &high);
    zexpect_u32_eq(0, high);
    zexpect_u32_eq(0x8080, low);
    act[0] = 1;
    Psyz_VSyncBlank();
    Psyz_PadsGetRumble(0, &low, &high);
    zexpect_u32_eq(0, high);
    Psyz_VSyncBlank();
    Psyz_PadsGetRumble(0, &low, &high);
    zexpect_u32_eq(0xFFFF, high);
    // digital mode: the channel finds the pad again
    zexpect_s32_eq(1, PadSetMainMode(0x00, 0, 0));
    for (n = 0; n < 60 && !(PadGetState(0x00) == 6 &&
                            PadInfoMode(0x00, 3, 0) == 0);
         n++) {
        Psyz_VSyncBlank();
    }
    zexpect_s32_lt(60, n);
    zexpect_s32_eq(4, PadInfoMode(0x00, 1, 0));
    zexpect_s32_eq(4, PadInfoMode(0x00, 2, 0));
    zexpect_u8_eq(0x41, buf[0][1]);
    zexpect_s32_eq(1, PadSetMainMode(0x00, 1, 0)); // back to analog
    blanks(60);
    zexpect_s32_eq(6, PadGetState(0x00));
    zexpect_s32_eq(7, PadInfoMode(0x00, 2, 0));
    PadSetAct(0x00, NULL, 0);
    Psyz_SetPadsInputCb(prev);
}

/* A digital pad refuses 0x43; libpad tries eleven more times, keeping the
 * buffer, then stores a port's bytes when it next starts a transfer: port
 * 1's at the same vblank when port 2 has a pad, else at the next; port 2's
 * at the next; a port without a controller reads 0xFF 0x00 at once. */
ZTEST(pad_frames, libpad_store_order) {
    static unsigned char buf[2][PSYZ_PAD_BUF_LEN];
    PsyzPadsInputCb prev = Psyz_SetPadsInputCb(store_input);
    store_set(0, PSYZ_CTRL_DIGITAL_PAD, 0);
    store_set(1, PSYZ_CTRL_DISCONNECTED, 0);
    PadInitDirect(buf[0], buf[1]);
    zexpect_u8_eq(0xFF, buf[0][0]);
    zexpect_u8_eq(0x00, buf[0][1]);
    PadStartCom();
    Psyz_VSyncBlank();
    zexpect_u8_eq(0xFF, buf[0][0]); // received, not stored yet
    zexpect_u8_eq(0xFF, buf[1][0]);
    zexpect_u8_eq(0x00, buf[1][1]);
    store_set(0, PSYZ_CTRL_DIGITAL_PAD, PADstart);
    Psyz_VSyncBlank();
    zexpect_u8_eq(0x00, buf[0][0]);
    zexpect_u8_eq(0x41, buf[0][1]);
    zexpect_u8_eq(0xFF, buf[0][2]); // the first exchange's bytes
    zexpect_s32_eq(1, PadGetState(0x00));
    blanks(11); // the twelve refused 0x43
    zexpect_u8_eq(0xFF, buf[0][2]);
    Psyz_VSyncBlank(); // a poll again
    zexpect_u8_eq(0xFF, buf[0][2]);
    zexpect_s32_eq(1, PadGetState(0x00));
    Psyz_VSyncBlank();
    zexpect_u8_eq(0xF7, buf[0][2]); // Start, one vblank after it was sent
    zexpect_s32_eq(2, PadGetState(0x00));
    zexpect_s32_eq(4, PadInfoMode(0x00, 1, 0));
    zexpect_s32_eq(0, PadInfoMode(0x00, 2, 0));
    zexpect_s32_eq(0, PadSetActAlign(0x00, NULL));
    zexpect_s32_eq(0, PadSetMainMode(0x00, 1, 0));
    // a pad in port 2: while it refuses 0x43 it has no packet to clock,
    // and port 1's bytes wait for port 1's next transfer
    store_set(0, PSYZ_CTRL_DIGITAL_PAD, 0);
    store_set(1, PSYZ_CTRL_DIGITAL_PAD, PADstart);
    Psyz_VSyncBlank();
    zexpect_u8_eq(0xFF, buf[1][0]);
    store_set(0, PSYZ_CTRL_DIGITAL_PAD, PADstart);
    Psyz_VSyncBlank();
    zexpect_u8_eq(0xFF, buf[0][2]); // the exchange before
    zexpect_u8_eq(0x00, buf[1][0]);
    zexpect_u8_eq(0xF7, buf[1][2]);
    Psyz_VSyncBlank();
    zexpect_u8_eq(0xF7, buf[0][2]);
    blanks(12);
    zexpect_s32_eq(2, PadGetState(0x10));
    // then port 1's bytes at once, port 2's a vblank later
    store_set(0, PSYZ_CTRL_DIGITAL_PAD, 0);
    store_set(1, PSYZ_CTRL_DIGITAL_PAD, 0);
    Psyz_VSyncBlank();
    zexpect_u8_eq(0xFF, buf[0][2]); // this exchange, released
    zexpect_u8_eq(0xF7, buf[1][2]);
    store_set(0, PSYZ_CTRL_DIGITAL_PAD, PADstart);
    Psyz_VSyncBlank();
    zexpect_u8_eq(0xF7, buf[0][2]); // this exchange
    zexpect_u8_eq(0xFF, buf[1][2]); // the exchange before
    // unplugged: a pad libpad has found keeps its bytes for eleven failed
    // exchanges; then 0xFF 0x00 and the channel starts again
    store_set(0, PSYZ_CTRL_DISCONNECTED, 0);
    blanks(11);
    zexpect_u8_eq(0x00, buf[0][0]);
    zexpect_u8_eq(0x41, buf[0][1]);
    zexpect_s32_eq(2, PadGetState(0x00)); // still the digital pad
    Psyz_VSyncBlank();
    zexpect_u8_eq(0xFF, buf[0][0]);
    zexpect_u8_eq(0x00, buf[0][1]);
    zexpect_s32_eq(0, PadGetState(0x00));
    store_set(1, PSYZ_CTRL_DISCONNECTED, 0);
    Psyz_VSyncBlank();
    Psyz_SetPadsInputCb(prev);
}
#endif
