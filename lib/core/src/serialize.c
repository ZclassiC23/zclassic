/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "core/serialize.h"
#include <stdlib.h>
#include <string.h>
#include "util/safe_alloc.h"

void stream_init(struct byte_stream *s, size_t initial_capacity)
{
    s->data = NULL;
    s->size = 0;
    s->capacity = 0;
    s->read_pos = 0;
    s->error = false;
    s->owns_data = true;
    if (initial_capacity > 0) {
        s->data = zcl_malloc(initial_capacity, "stream_data");
        if (s->data)
            s->capacity = initial_capacity;
        else
            s->error = true;
    }
}

void stream_init_from_data(struct byte_stream *s, const unsigned char *data,
                           size_t len)
{
    s->data = (unsigned char *)data;
    s->size = len;
    s->capacity = len;
    s->read_pos = 0;
    s->error = false;
    s->owns_data = false;
}

void stream_free(struct byte_stream *s)
{
    if (s->owns_data)
        free(s->data);
    s->data = NULL;
    s->size = 0;
    s->capacity = 0;
    s->read_pos = 0;
}

static bool stream_grow(struct byte_stream *s, size_t needed)
{
    if (!s->owns_data) {
        s->error = true;
        return false;
    }
    /* Guard against size_t overflow in s->size + needed */
    if (needed > SIZE_MAX - s->size) {
        s->error = true;
        return false;
    }
    size_t target = s->size + needed;
    size_t new_cap = s->capacity ? s->capacity : 64;
    while (new_cap < target) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = target; /* cap at exact needed, no further doubling */
            break;
        }
        new_cap *= 2;
    }
    unsigned char *p = zcl_realloc(s->data, new_cap, "stream_grow");
    if (!p) {
        s->error = true;
        return false;
    }
    s->data = p;
    s->capacity = new_cap;
    return true;
}

bool stream_write(struct byte_stream *s, const void *buf, size_t len)
{
    if (s->error) return false;
    if (s->size + len > s->capacity && !stream_grow(s, len))
        return false;
    memcpy(s->data + s->size, buf, len);
    s->size += len;
    return true;
}

bool stream_read(struct byte_stream *s, void *buf, size_t len)
{
    if (s->error || s->read_pos > s->size || len > s->size - s->read_pos) {
        s->error = true;
        return false;
    }
    memcpy(buf, s->data + s->read_pos, len);
    s->read_pos += len;
    return true;
}

size_t stream_remaining(const struct byte_stream *s)
{
    if (s->read_pos >= s->size) return 0;
    return s->size - s->read_pos;
}

bool stream_write_u8(struct byte_stream *s, uint8_t v)
{
    return stream_write(s, &v, 1);
}

bool stream_write_u16_le(struct byte_stream *s, uint16_t v)
{
    unsigned char buf[2] = { (unsigned char)(v & 0xff), (unsigned char)(v >> 8) };
    return stream_write(s, buf, 2);
}

bool stream_write_u32_le(struct byte_stream *s, uint32_t v)
{
    unsigned char buf[4];
    buf[0] = (unsigned char)(v);
    buf[1] = (unsigned char)(v >> 8);
    buf[2] = (unsigned char)(v >> 16);
    buf[3] = (unsigned char)(v >> 24);
    return stream_write(s, buf, 4);
}

bool stream_write_u64_le(struct byte_stream *s, uint64_t v)
{
    unsigned char buf[8];
    for (int i = 0; i < 8; i++)
        buf[i] = (unsigned char)(v >> (8 * i));
    return stream_write(s, buf, 8);
}

bool stream_write_i32_le(struct byte_stream *s, int32_t v)
{
    return stream_write_u32_le(s, (uint32_t)v);
}

bool stream_write_i64_le(struct byte_stream *s, int64_t v)
{
    return stream_write_u64_le(s, (uint64_t)v);
}

bool stream_read_u8(struct byte_stream *s, uint8_t *v)
{
    return stream_read(s, v, 1);
}

bool stream_read_u16_le(struct byte_stream *s, uint16_t *v)
{
    unsigned char buf[2];
    if (!stream_read(s, buf, 2)) return false;
    *v = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return true;
}

bool stream_read_u32_le(struct byte_stream *s, uint32_t *v)
{
    unsigned char buf[4];
    if (!stream_read(s, buf, 4)) return false;
    *v = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
         ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return true;
}

bool stream_read_u64_le(struct byte_stream *s, uint64_t *v)
{
    unsigned char buf[8];
    if (!stream_read(s, buf, 8)) return false;
    *v = 0;
    for (int i = 0; i < 8; i++)
        *v |= (uint64_t)buf[i] << (8 * i);
    return true;
}

bool stream_read_i32_le(struct byte_stream *s, int32_t *v)
{
    uint32_t tmp = 0;
    if (!stream_read_u32_le(s, &tmp))
        return false;
    *v = (int32_t)tmp;
    return true;
}

bool stream_read_i64_le(struct byte_stream *s, int64_t *v)
{
    uint64_t tmp = 0;
    if (!stream_read_u64_le(s, &tmp))
        return false;
    *v = (int64_t)tmp;
    return true;
}

bool stream_write_compact_size(struct byte_stream *s, uint64_t size)
{
    if (size < 253) {
        return stream_write_u8(s, (uint8_t)size);
    } else if (size <= 0xffff) {
        return stream_write_u8(s, 253) && stream_write_u16_le(s, (uint16_t)size);
    } else if (size <= 0xffffffffULL) {
        return stream_write_u8(s, 254) && stream_write_u32_le(s, (uint32_t)size);
    } else {
        return stream_write_u8(s, 255) && stream_write_u64_le(s, size);
    }
}

bool stream_read_compact_size(struct byte_stream *s, uint64_t *size)
{
    uint8_t marker;
    if (!stream_read_u8(s, &marker)) return false;
    if (marker < 253) {
        *size = marker;
    } else if (marker == 253) {
        uint16_t v;
        if (!stream_read_u16_le(s, &v)) return false;
        *size = v;
    } else if (marker == 254) {
        uint32_t v;
        if (!stream_read_u32_le(s, &v)) return false;
        *size = v;
    } else {
        if (!stream_read_u64_le(s, size)) return false;
    }
    /* Note: non-canonical encodings (e.g. 0xfd for values < 253) are
     * accepted for backwards compatibility with the Bitcoin wire protocol.
     * The C++ reference node also accepts them. */
    return true;
}

bool stream_write_varint(struct byte_stream *s, uint64_t n)
{
    unsigned char tmp[(sizeof(n) * 8 + 6) / 7];
    int len = 0;
    while (true) {
        tmp[len] = (unsigned char)(n & 0x7f) | (len ? 0x80 : 0x00);
        if (n <= 0x7f)
            break;
        n = (n >> 7) - 1;
        len++;
    }
    for (int i = len; i >= 0; i--) {
        if (!stream_write_u8(s, tmp[i]))
            return false;
    }
    return true;
}

bool stream_read_varint(struct byte_stream *s, uint64_t *n)
{
    *n = 0;
    while (true) {
        uint8_t ch;
        if (!stream_read_u8(s, &ch)) return false;
        *n = (*n << 7) | (uint64_t)(ch & 0x7f);
        if (ch & 0x80)
            *n += 1;
        else
            break;
    }
    return true;
}

bool stream_write_bytes(struct byte_stream *s, const unsigned char *data, size_t len)
{
    return stream_write(s, data, len);
}

bool stream_read_bytes(struct byte_stream *s, unsigned char *data, size_t len)
{
    return stream_read(s, data, len);
}
