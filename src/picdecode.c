/**
 * @file picdecode.c
 * @brief Independent multi-platform PIC/APIC image decoder.
 */
#include "picdecode.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { PIC_INVALID = 1, PIC_NOMEM = 3, PIC_UNSET = UINT_MAX };

int PicGetComment(const uint8_t *data, size_t size,
                  const uint8_t **comment, size_t *length)
{
    size_t end = 3;
    if (data == NULL || comment == NULL || length == NULL ||
        size < 4 || memcmp(data, "PIC", 3) != 0)
        return PIC_INVALID;
    while (end < size && data[end] != 0x1a)
        ++end;
    if (end == size)
        return PIC_INVALID;
    *comment = data + 3;
    *length = end - 3;
    return 0;
}

typedef struct BitReader {
    const uint8_t *data;
    size_t size;
    size_t position;
    uint8_t mask;
} BitReader;

typedef struct ColorCache {
    uint32_t value[128];
    uint8_t previous[128];
    uint8_t next[128];
    uint8_t head;
} ColorCache;

static int read_bits(BitReader *reader, unsigned count, uint32_t *value)
{
    uint32_t result = 0;
    while (count-- != 0) {
        if (reader->position >= reader->size)
            return 0;
        result = (result << 1) | ((reader->data[reader->position] & reader->mask) != 0);
        reader->mask >>= 1;
        if (reader->mask == 0) {
            reader->mask = 0x80;
            ++reader->position;
        }
    }
    *value = result;
    return 1;
}

static int read_length(BitReader *reader, uint32_t *length)
{
    unsigned bits;
    uint32_t bit, suffix;
    for (bits = 1; bits < 21; ++bits) {
        if (!read_bits(reader, 1, &bit))
            return 0;
        if (bit == 0) {
            if (!read_bits(reader, bits, &suffix))
                return 0;
            *length = suffix + ((uint32_t)1 << bits) - 1u;
            return 1;
        }
    }
    return 0;
}

/* X68000 PIC stores GGGGGRRRRRBBBBBI. */
static uint32_t x68k_color(uint32_t value)
{
    /*
     * Keep the source component precision.  Apart from matching the
     * original X68000 display convention, this avoids inventing low bits
     * when producing a 24-bit DIB.
     */
    return ((value & 1984u) << 13) | (value & 63488u) |
           (((value & 62u) << 2) | 1u);
}

/* PC-88VA's 16-bit mode uses GGGGGGRRRRRBBBBB. */
static uint32_t pc88va_color(uint32_t value)
{
    return ((value & 992u) << 14) | (value & 64512u) |
           (((value & 31u) << 3) | 1u);
}

/* PC-88VA's 12-bit mode uses GGGGRRRRBBBB. */
static uint32_t pc88va_color12(uint32_t value)
{
    return ((value & 0x0f0u) << 16) | ((value & 0xf00u) << 4) |
           ((value & 0x00fu) << 4) | 1u;
}

/* Macintosh PIC stores RRRRRGGGGGBBBBB. */
static uint32_t mac_color(uint32_t value)
{
    return ((value & 0x001fu) << 3) | ((value & 0x03e0u) << 6) |
           ((value & 0x7c00u) << 9);
}

static void cache_init(ColorCache *cache)
{
    unsigned i;
    memset(cache->value, 0, sizeof(cache->value));
    cache->head = 0;
    for (i = 0; i < 128; ++i) {
        cache->previous[i] = (uint8_t)((i + 1u) & 127u);
        cache->next[i] = (uint8_t)((i - 1u) & 127u);
    }
}

static void cache_add(ColorCache *cache, uint32_t value)
{
    cache->head = cache->previous[cache->head];
    cache->value[cache->head] = value;
}

static uint32_t cache_get(ColorCache *cache, uint8_t key)
{
    if (key != cache->head) {
        uint8_t previous = cache->previous[key];
        uint8_t next = cache->next[key];
        uint8_t tail;
        cache->next[previous] = next;
        cache->previous[next] = previous;
        tail = cache->previous[cache->head];
        cache->next[tail] = key;
        cache->previous[key] = tail;
        cache->previous[cache->head] = key;
        cache->next[key] = cache->head;
        cache->head = key;
    }
    return cache->value[key];
}

static int set_chain(BitReader *reader, uint32_t *values, uint32_t width,
                     uint32_t pixel_count, uint32_t offset, uint32_t color)
{
    int64_t chain_offset = offset;
    uint32_t code, bit;
    for (;;) {
        if (!read_bits(reader, 2, &code))
            return 0;
        if (code == 0) {
            if (!read_bits(reader, 1, &bit))
                return 0;
            if (bit == 0)
                return 1;
            if (!read_bits(reader, 1, &bit))
                return 0;
            if (bit == 0) {
                chain_offset -= 2;
            } else {
                chain_offset += 2;
            }
        } else if (code == 1) {
            --chain_offset;
        } else if (code == 3) {
            ++chain_offset;
        }
        chain_offset += width;
        /*
         * The original loader keeps consuming a chain after it leaves the
         * bottom of the saved rectangle.  Several files in the wild rely on
         * this clipping behaviour, so an off-screen point is not corruption.
         */
        if (chain_offset >= 0 && chain_offset < pixel_count)
            values[(size_t)chain_offset] = color;
    }
}

static int decode_pixels(BitReader *reader, uint32_t width, uint32_t height,
                         unsigned platform, unsigned depth, const uint32_t *palette,
                         PicImage *image)
{
    size_t count = (size_t)width * height;
    unsigned machine = platform & 15u;
    unsigned mode = (platform >> 4) & 15u;
    int packed_pc88va = machine == 1 && (mode & 2u) != 0;
    uint32_t *values;
    ColorCache cache;
    /* Direct-color PIC starts with black; PC-88VA mode 0x21 stores indices. */
    uint32_t current = packed_pc88va ? 0 : 1;
    size_t offset = 0;
    size_t i;

    if (width == 0 || height == 0 || count > SIZE_MAX / sizeof(*values) ||
        count > SIZE_MAX / 3u)
        return PIC_INVALID;
    values = (uint32_t *)malloc(count * sizeof(*values));
    image->pixels = (uint8_t *)malloc(count * (packed_pc88va ? 2u : 3u));
    if (values == NULL || image->pixels == NULL) {
        free(values);
        free(image->pixels);
        image->pixels = NULL;
        return PIC_NOMEM;
    }
    for (i = 0; i < count; ++i)
        values[i] = (uint32_t)PIC_UNSET;
    cache_init(&cache);

    while (offset < count) {
        uint32_t length, raw, chained;
        if (!read_length(reader, &length) || length == 0)
            goto broken;
        while (--length != 0) {
            if (offset >= count) goto broken;
            if (values[offset] == PIC_UNSET)
                values[offset] = current;
            else
                current = values[offset];
            ++offset;
            if (offset == count)
                goto complete;
        }
        if (offset >= count) goto broken;
        if (depth <= 8) {
            if (!read_bits(reader, depth, &raw) || raw >= (1u << depth))
                goto broken;
            current = palette[raw];
        } else {
            if (!read_bits(reader, 1, &raw)) goto broken;
            if (raw == 0) {
                if (!read_bits(reader, depth, &raw)) goto broken;
                if (packed_pc88va)
                    current = raw;
                else if (machine == 1)
                    current = depth == 12 ? pc88va_color12(raw) :
                                             pc88va_color(raw);
                else if (machine == 3)
                    current = mac_color(raw);
                else
                    current = x68k_color(depth == 15 ? raw << 1 : raw);
                cache_add(&cache, current);
            } else {
                if (!read_bits(reader, 7, &raw)) goto broken;
                current = cache_get(&cache, (uint8_t)raw);
            }
        }
        values[offset] = current;
        if (++offset == count)
            break;
        if (!read_bits(reader, 1, &chained)) goto broken;
        if (chained != 0 && !set_chain(reader, values, width, (uint32_t)count,
                                        (uint32_t)(offset - 1u), current))
            goto broken;
    }

complete:
    if (packed_pc88va) {
        static const uint8_t component3[8] =
            { 0, 40, 80, 120, 160, 192, 224, 255 };
        static const uint8_t component2[4] = { 0, 96, 176, 255 };
        for (i = count; i-- != 0;) {
            uint32_t color = values[i] == PIC_UNSET ? 0 : values[i];
            image->pixels[i * 2u] = (uint8_t)color;
            image->pixels[i * 2u + 1u] = (uint8_t)(color >> 8);
        }
        for (i = 0; i < 256; ++i) {
            image->palette[i][0] = component3[(i >> 2) & 7];
            image->palette[i][1] = component3[(i >> 5) & 7];
            image->palette[i][2] = component2[i & 3];
        }
        free(values);
        image->width = width;
        image->height = height * 2u;
        image->bits_per_pixel = 8;
        return 0;
    }
    for (i = 0; i < count; ++i) {
        uint32_t color = values[i];
        /* Unspecified pixels are the PIC format's initial black colour. */
        if (color == PIC_UNSET) color = 1;
        image->pixels[i * 3] = (uint8_t)color;
        image->pixels[i * 3 + 1] = (uint8_t)(color >> 8);
        image->pixels[i * 3 + 2] = (uint8_t)(color >> 16);
    }
    free(values);
    image->width = width;
    image->height = height;
    image->bits_per_pixel = 24;
    return 0;

broken:
    /*
     * Some early savers omit the final run when the unused remainder of a
     * larger screen is entirely black.  Accept only a clean byte-boundary
     * EOF; an incomplete code at any other bit position remains an error.
     */
    if (reader->position == reader->size && reader->mask == 0x80)
        goto complete;
    free(values);
    free(image->pixels);
    image->pixels = NULL;
    return PIC_INVALID;
}

int PicDecode(const uint8_t *data, size_t size, PicImage *image)
{
    BitReader reader;
    uint32_t platform, depth, width, height;
    uint32_t palette[256];
    size_t position = 3;
    unsigned i;

    if (data == NULL || image == NULL || size < 14 || memcmp(data, "PIC", 3) != 0)
        return PIC_INVALID;
    memset(image, 0, sizeof(*image));
    while (position < size && data[position] != 0x1a)
        ++position;
    if (position == size) return PIC_INVALID;
    ++position;
    while (position < size && data[position] != 0)
        ++position;
    if (position == size) return PIC_INVALID;
    ++position;

    reader.data = data;
    reader.size = size;
    reader.position = position;
    reader.mask = 0x80;
    if (!read_bits(&reader, 16, &platform) || !read_bits(&reader, 16, &depth) ||
        !read_bits(&reader, 16, &width) || !read_bits(&reader, 16, &height))
        return PIC_INVALID;
    /*
     * The 16-bit value consists of the reserved byte followed by the
     * mode/machine byte.  Values 2 and 194 are FM-TOWNS modes; both use
     * the X68000 15-bit colour ordering.
     */
    if ((platform & 15u) != 0 && (platform & 15u) != 1 &&
        (platform & 15u) != 2 && (platform & 15u) != 3 &&
        (platform & 15u) != 15)
        return PIC_INVALID;
    if (depth != 4 && depth != 8 && depth != 12 &&
        depth != 15 && depth != 16)
        return PIC_INVALID;
    if ((platform & 15u) == 1 && ((platform >> 4) & 2u) != 0 &&
        depth != 16)
        return PIC_INVALID;
    if (platform == 2 || platform == 31) {
        if (reader.position + 6 > size || reader.mask != 0x80)
            return PIC_INVALID;
        reader.position += 6;
    }
    memset(palette, 0, sizeof(palette));
    if (depth <= 8) {
        for (i = 0; i < (1u << depth); ++i) {
            uint32_t color;
            if (!read_bits(&reader, 16, &color))
                return PIC_INVALID;
            palette[i] = x68k_color(color);
        }
    }
    return decode_pixels(&reader, width, height, platform, depth, palette, image);
}

void PicFree(PicImage *image)
{
    if (image != NULL) {
        free(image->pixels);
        image->pixels = NULL;
    }
}
