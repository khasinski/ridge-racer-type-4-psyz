#ifndef PSYZ_CD_H
#define PSYZ_CD_H

/**
 * @file cd.h
 * @brief CD-ROM drive emulation endpoints.
 */

#include <psyz/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PsyzCdRead {
    unsigned int sector;  /**< disk sector, can be used as a unique file id */
    unsigned int size;    /**< byte count required to be read */
    void* buffer;         /**< buffer to read into, max index is buffer[size-1] */
};

/**
 * @brief Callback invoked when a disk read is triggered
 *
 * @param read Read request describing sector, size and destination buffer
 * @return Bytes read; -1 if not found, unsuccessful or not implemented
 */
typedef int (*PsyzCdReadCB)(struct PsyzCdRead* read);

/**
 * @brief Set path to CUE file, simulating a CD loaded
 *
 * Passing a NULL will unset a previously set disk path.
 *
 * @param diskPath Path to the CUE file, or NULL to unset
 * @return 0 on success, otherwise CUE parsing failed
 */
int Psyz_CdSetDiskPath(const char* diskPath);

/**
 * @brief Read one raw sector of a CUE image without mounting it
 *
 * Parses the CUE sheet and reads the 2352-byte sector at the absolute
 * sector number lba (0 is the first sector of track 1; ISO 9660's volume
 * descriptor is at 16). The mounted disc, the drive's position and its
 * timing are left as they were, so a host can check an image (its
 * SYSTEM.CNF, say) before or while another one is in the drive.
 *
 * @param cue_path Path to the CUE file (UTF-8)
 * @param lba Absolute sector number
 * @param out Receives the 2352 bytes of the sector
 * @return 0 on success, -1 if the CUE does not parse or the sector cannot be
 *         read
 */
int Psyz_CdReadImageSector(const char* cue_path, int lba, void* out);

/**
 * @brief Simulate CD-ROM drive shell (lid) opening or closing
 *
 * @param is_open Non-zero to open the shell, zero to close it
 */
void Psyz_CdShellOpen(int is_open);

/**
 * @brief Pull PCM audio samples from the CD as interleaved stereo frames
 *
 * In XA mode, ADPCM sectors are decoded before being returned. CdMix
 * attenuation has already been applied to the output. Called internally from
 * libcd, and typically not used by the user.
 *
 * @param out Output buffer
 * @param num_frames Number of stereo frames requested
 * @return Number of frames read
 */
size_t Psyz_CdPullSamples(short* out, size_t num_frames);

/**
 * @brief Set callback invoked when a disk read is triggered
 *
 * Used for data reads and XA streaming. XA requests contain a full 2352-byte
 * sector at the requested LBA; return exactly size bytes, or -1 on failure.
 * A NULL callback uses the mounted CUE. Callback errors end the read/stream.
 *
 * @param cb Disk read callback, or NULL to clear
 */
void Psyz_CdSetReadCB(PsyzCdReadCB cb);

/**
 * @brief Drop the drive's mechanical timing (instant loading)
 *
 * By default the emulated drive takes the PS1's time: command
 * acknowledges, seeks, pauses and 75 or 150 sectors per second. With
 * instant timing, commands complete at once and plain data reads deliver
 * as fast as the game polls them; ADPCM (CdlModeRT) streams keep their
 * rate. The PSYZ_CD_TIMING=instant environment variable sets it as well.
 *
 * @param instant Non-zero for instant timing, zero for the PS1's timing
 */
void Psyz_CdSetInstantTiming(int instant);

#ifdef __cplusplus
}
#endif

#endif
