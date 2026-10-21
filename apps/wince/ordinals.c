/* Locally generated ordinal names. Legacy records are COREDLL-only;
 * qualified records must match both library and ordinal. */
#include "ordinals.h"
#include <string.h>
#include <strings.h>

bool ce_ordinal_find(const char *map, unsigned bytes, const char *dll, uint16_t ordinal,
                     char *name, unsigned capacity)
{
    for (unsigned pos = 0; pos < bytes;) {
        unsigned first = pos;
        bool numeric = true;
        while (pos < bytes && map[pos] != ' ') {
            if ((unsigned char)map[pos] < 33 || (unsigned char)map[pos] > 126) return false;
            if (map[pos] < '0' || map[pos] > '9') numeric = false;
            pos++;
        }
        if (pos == bytes || pos == first) return false;
        const char *library = numeric ? "coredll.dll" : map + first;
        unsigned library_length = numeric ? 11 : pos - first;
        if (numeric) pos = first;
        else pos++;
        unsigned start = pos, number = 0;
        while (pos < bytes && map[pos] >= '0' && map[pos] <= '9') {
            number = number * 10u + (unsigned)(map[pos++] - '0');
            if (number > 65535) return false;
        }
        if (pos == start || pos == bytes || map[pos++] != ' ') return false;
        unsigned symbol = pos;
        while (pos < bytes && map[pos] != '\n') {
            if ((unsigned char)map[pos] < 33 || (unsigned char)map[pos] > 126) return false;
            pos++;
        }
        if (pos == bytes || pos == symbol) return false;
        if (number == ordinal && strlen(dll) == library_length &&
            !strncasecmp(dll, library, library_length)) {
            unsigned length = pos - symbol;
            if (!name || length >= capacity) return false;
            memcpy(name, map + symbol, length);
            name[length] = 0;
            return true;
        }
        pos++;
    }
    return false;
}
