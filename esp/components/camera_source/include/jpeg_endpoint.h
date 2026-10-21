#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Return the byte length through the first structural EOI, or zero on a
 * truncated/invalid marker stream. The caller must supply the captured length,
 * never the allocation capacity. Length-delimited metadata/tables are opaque;
 * an embedded thumbnail's EOI is not the outer image's endpoint.
 *
 * This is an endpoint check, not an entropy decoder: it cannot prove that all
 * MCUs arrived, including when truncated entropy is followed by stale bytes.
 * Keep the consumer's decode/completeness checks. No allocation is performed.
 */
static inline size_t streamer_jpeg_endpoint(const uint8_t *data, size_t length)
{
    if (!data || length < 4 || data[0] != 0xff) {
        return 0;
    }
    size_t pos = 1;
    while (pos < length && data[pos] == 0xff) {
        ++pos; /* marker fill is legal before SOI too */
    }
    if (pos == length || data[pos++] != 0xd8) {
        return 0;
    }

    bool entropy = false;
    bool saw_frame = false;
    unsigned scans = 0; /* saturates at two: DNL may terminate only scan one */
    bool saw_dnl = false;
    bool needs_height = false;
    unsigned components = 0;
    while (pos < length) {
        if (entropy) {
            const uint8_t *next = (const uint8_t *)memchr(data + pos, 0xff, length - pos);
            if (!next) {
                return 0;
            }
            pos = (size_t)(next - data);
        } else if (data[pos] != 0xff) {
            return 0;
        }

        const size_t prefix = pos++;
        while (pos < length && data[pos] == 0xff) {
            ++pos;
        }
        if (pos == length) {
            return 0;
        }
        const unsigned marker = data[pos++];
        if (entropy && marker == 0x00) {
            if (pos - prefix != 2) {
                return 0; /* FF00 is one stuffed data byte, not marker fill */
            }
            continue;
        }
        if (entropy && marker >= 0xd0 && marker <= 0xd7) {
            continue; /* restart markers do not end the scan */
        }
        if (marker == 0x01) {
            continue; /* standalone TEM; retain the current scan state */
        }
        const bool ended_scan = entropy;
        entropy = false;
        if (marker == 0xd9) {
            return saw_frame && scans && !needs_height ? pos : 0;
        }
        if (marker < 0xc0 || marker == 0xc8 ||
            (marker >= 0xd0 && marker <= 0xd8) ||
            (marker >= 0xf0 && marker <= 0xfd)) {
            return 0; /* stuffed bytes, nested SOI, or undefined extensions */
        }
        if (length - pos < 2) {
            return 0;
        }
        const size_t segment = ((size_t)data[pos] << 8) | data[pos + 1];
        if (segment < 2 || segment > length - pos) {
            return 0;
        }

        const bool frame = (marker >= 0xc0 && marker <= 0xc3) ||
                           (marker >= 0xc5 && marker <= 0xc7) ||
                           (marker >= 0xc9 && marker <= 0xcb) ||
                           (marker >= 0xcd && marker <= 0xcf);
        if (frame) {
            if (saw_frame || segment < 8) {
                return 0;
            }
            components = data[pos + 7];
            if (!components || segment != 8 + 3 * components ||
                !data[pos + 2] || data[pos + 2] > 16 ||
                !(data[pos + 5] | data[pos + 6])) {
                return 0;
            }
            needs_height = !(data[pos + 3] | data[pos + 4]);
            saw_frame = true;
        } else if (marker == 0xda) {
            if (!saw_frame || segment < 6) {
                return 0;
            }
            const unsigned scan_components = data[pos + 2];
            if (!scan_components || scan_components > 4 ||
                scan_components > components || segment != 6 + 2 * scan_components) {
                return 0;
            }
            if (scans < 2) {
                ++scans;
            }
            entropy = true;
        } else if (marker == 0xdc) {
            if (!ended_scan || scans != 1 || saw_dnl || segment < 4 ||
                !(data[pos + 2] | data[pos + 3])) {
                return 0;
            }
            saw_dnl = true;
            needs_height = false; /* DNL ends the first scan; another SOS resumes */
        }
        pos += segment;
    }
    return 0;
}
