/**
 * @file ifpic.c
 * @brief Susie I/F adapter for multi-platform PIC/APIC images.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "spibase.h"
#include "picdecode.h"

#define IFPIC_VERSION "0.13"

const int NumInfo = 4;
const LPCSTR PluginInfo[] = {
    "00IN", "Yanagisawa PIC to DIB filter ver." IFPIC_VERSION " (C) Ru^3",
    "*.pic", "PIC/APIC image"
};

/** Read a Susie input stream into an automatically growing byte buffer. */
static int read_entire_file(SPI_FILE *file, uint8_t **data, size_t *size)
{
    uint8_t *buffer = NULL;
    size_t used = 0, capacity = 0;
    for (;;) {
        DWORD read_size;
        if (used == capacity) {
            size_t new_capacity = capacity == 0 ? 65536u : capacity * 2u;
            uint8_t *new_buffer;
            if (new_capacity <= capacity || new_capacity > 0x7fffffffU) {
                free(buffer);
                return SPI_ERROR_ALLOCATE_MEMORY;
            }
            new_buffer = (uint8_t *)realloc(buffer, new_capacity);
            if (new_buffer == NULL) {
                free(buffer);
                return SPI_ERROR_ALLOCATE_MEMORY;
            }
            buffer = new_buffer;
            capacity = new_capacity;
        }
        read_size = SpiRead(buffer + used, (DWORD)(capacity - used), file);
        used += read_size;
        if (read_size == 0 || used < capacity)
            break;
    }
    if (used == 0) {
        free(buffer);
        return SPI_ERROR_FILE_READ;
    }
    *data = buffer;
    *size = used;
    return 0;
}

/** Identify PIC by its mandatory three-byte signature. */
int IsSupportedFormat(LPBYTE data, DWORD size, LPCSTR filename)
{
    (void)filename;
    return size >= 3 && memcmp(data, "PIC", 3) == 0;
}

/** Copy the PIC Shift-JIS comment to a Susie-owned local-memory block. */
static int set_picture_comment(const uint8_t *data, size_t size,
                               PictureInfo *info, int utf8)
{
    const uint8_t *comment;
    size_t comment_length;
    LPBYTE output;
    size_t output_length;

    if (PicGetComment(data, size, &comment, &comment_length) != 0)
        return SPI_ERROR_BROKEN_DATA;
    if (comment_length == 0)
        return SPI_ERROR_SUCCESS;
    if (!utf8) {
        if (comment_length >= UINT_MAX)
            return SPI_ERROR_ALLOCATE_MEMORY;
        if (SpiAllocBuffer(&info->hInfo, &output,
                           (UINT)comment_length + 1u) != SPI_ERROR_SUCCESS)
            return SPI_ERROR_ALLOCATE_MEMORY;
        memcpy(output, comment, comment_length);
        output[comment_length] = 0;
        SpiUnlockBuffer(&info->hInfo);
        return SPI_ERROR_SUCCESS;
    }
    {
        WCHAR *wide;
        int wide_length;
        int utf8_length;
        if (comment_length > INT_MAX)
            return SPI_ERROR_ALLOCATE_MEMORY;
        wide_length = MultiByteToWideChar(932, 0, (LPCCH)comment,
                                          (int)comment_length, NULL, 0);
        if (wide_length <= 0)
            return SPI_ERROR_BROKEN_DATA;
        wide = (WCHAR *)malloc((size_t)wide_length * sizeof(*wide));
        if (wide == NULL)
            return SPI_ERROR_ALLOCATE_MEMORY;
        if (MultiByteToWideChar(932, 0, (LPCCH)comment, (int)comment_length,
                                wide, wide_length) != wide_length) {
            free(wide);
            return SPI_ERROR_BROKEN_DATA;
        }
        utf8_length = WideCharToMultiByte(CP_UTF8, 0, wide, wide_length,
                                          NULL, 0, NULL, NULL);
        output_length = 3u + (size_t)utf8_length + 1u;
        if (utf8_length <= 0 || output_length > UINT_MAX ||
            SpiAllocBuffer(&info->hInfo, &output,
                           (UINT)output_length) != SPI_ERROR_SUCCESS) {
            free(wide);
            return SPI_ERROR_ALLOCATE_MEMORY;
        }
        output[0] = 0xef;
        output[1] = 0xbb;
        output[2] = 0xbf;
        if (WideCharToMultiByte(CP_UTF8, 0, wide, wide_length,
                                (LPSTR)output + 3, utf8_length,
                                NULL, NULL) != utf8_length) {
            free(wide);
            SpiUnlockBuffer(&info->hInfo);
            SpiFreeBuffer(&info->hInfo);
            return SPI_ERROR_BROKEN_DATA;
        }
        output[3 + utf8_length] = 0;
        free(wide);
        SpiUnlockBuffer(&info->hInfo);
    }
    return SPI_ERROR_SUCCESS;
}

/** Decode PIC metadata and return its comment in the requested encoding. */
static int get_image_info(SPI_FILE *file, PictureInfo *info, int utf8)
{
    uint8_t *data;
    size_t size;
    PicImage image;
    int result = read_entire_file(file, &data, &size);
    if (result != 0) return result;
    result = PicDecode(data, size, &image);
    if (result != 0) {
        free(data);
        return result == 3 ? SPI_ERROR_ALLOCATE_MEMORY : SPI_ERROR_BROKEN_DATA;
    }
    SpiSetPictureInfo(info, image.width, image.height, image.bits_per_pixel,
                      0, 0, 0, 0, NULL);
    result = set_picture_comment(data, size, info, utf8);
    free(data);
    PicFree(&image);
    return result;
}

/** Decode PIC metadata and return the original Shift-JIS comment. */
int GetImageInfo(SPI_FILE *file, PictureInfo *info)
{
    return get_image_info(file, info, 0);
}

/** Decode PIC metadata and return a UTF-8 comment prefixed by a BOM. */
int GetImageInfoW(SPI_FILE *file, PictureInfo *info)
{
    return get_image_info(file, info, 1);
}

/** Decode PIC pixels and construct a bottom-up 24bpp DIB. */
int GetImage(SPI_FILE *file, HANDLE *bitmap_info_handle, HANDLE *bitmap_handle,
             SPIPROC progress_callback, LONG_PTR callback_data)
{
    uint8_t *data;
    size_t size;
    PicImage image;
    LPBITMAPINFO bitmap_info;
    LPBYTE bitmap_bits;
    DWORD bitmap_stride;
    uint32_t y;
    int result = read_entire_file(file, &data, &size);
    if (result != 0) return result;
    result = PicDecode(data, size, &image);
    free(data);
    if (result != 0) return result == 3 ? SPI_ERROR_ALLOCATE_MEMORY : SPI_ERROR_BROKEN_DATA;
    result = SpiInitBitmap((HLOCAL *)bitmap_info_handle, &bitmap_info,
                           (HLOCAL *)bitmap_handle, &bitmap_bits, &bitmap_stride,
                           image.width, image.height, image.bits_per_pixel,
                           image.bits_per_pixel == 8 ? 256 : 0, 0, 0);
    if (result != 0) {
        PicFree(&image);
        return result;
    }
    if (image.bits_per_pixel == 8) {
        unsigned color;
        for (color = 0; color < 256; ++color) {
            bitmap_info->bmiColors[color].rgbRed = image.palette[color][0];
            bitmap_info->bmiColors[color].rgbGreen = image.palette[color][1];
            bitmap_info->bmiColors[color].rgbBlue = image.palette[color][2];
            bitmap_info->bmiColors[color].rgbReserved = 0;
        }
    }
    for (y = 0; y < image.height; ++y) {
        size_t row_size = (size_t)image.width * image.bits_per_pixel / 8u;
        memcpy(bitmap_bits + (size_t)(image.height - 1u - y) * bitmap_stride,
               image.pixels + (size_t)y * row_size, row_size);
    }
    PicFree(&image);
    SpiUnlockBuffer(bitmap_info_handle);
    SpiUnlockBuffer(bitmap_handle);
    if (progress_callback != NULL)
        progress_callback(100, 100, callback_data);
    return 0;
}
