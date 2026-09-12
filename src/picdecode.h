/**
 * @file picdecode.h
 * @brief Decoder interface for multi-platform PIC/APIC images.
 */
#ifndef PICDECODE_H
#define PICDECODE_H

#include <stddef.h>
#include <stdint.h>

/** Decoded PIC image stored as tightly packed top-down pixels. */
typedef struct PicImage {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint8_t bits_per_pixel;
    uint8_t palette[256][3];
} PicImage;

/**
 * Decode an X68000, PC-88VA, FM-TOWNS, or Macintosh PIC stream without
 * applying display scaling or color reduction.
 *
 * @param data Complete PIC file data.
 * @param size Number of bytes in @p data.
 * @param image Receives either a 24-bit BGR image or an 8-bit indexed image.
 * @return 0 on success, 1 for invalid data, or 3 on allocation failure.
 */
int PicDecode(const uint8_t *data, size_t size, PicImage *image);

/**
 * Locate the Shift-JIS comment field between the PIC signature and 0x1a.
 *
 * The returned pointer refers to @p data and remains valid only while that
 * buffer is alive. Structured /MM/ flags are retained because they belong
 * to the PIC comment field defined by the format.
 *
 * @param data Complete PIC file data.
 * @param size Number of bytes in @p data.
 * @param comment Receives the first byte of the comment.
 * @param length Receives the comment length, excluding 0x1a.
 * @return 0 on success or 1 if the PIC header is invalid.
 */
int PicGetComment(const uint8_t *data, size_t size,
                  const uint8_t **comment, size_t *length);

/** Release pixels owned by a decoded image. */
void PicFree(PicImage *image);

#endif
