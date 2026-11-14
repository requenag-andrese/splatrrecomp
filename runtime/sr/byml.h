// sr-recomp: the game's parameter tables (BYML, little-endian "YB") as a tree that can be changed and written again
// (sr/byml.c). For the tables the runtime rewrites at start (sr/loadout.c); sr/ink_color.c patches its table in place
// and needs none of this.
//
// A node is a container or a value. Containers: an array (0xC0), a dictionary (0xC1: keys sorted by name), a hash map
// (0x20 / 0x21: kept as its bytes, and only with plain values inside). Values: a string (0xA0), a 32-bit scalar (bool
// 0xD0, int 0xD1, float 0xD2, unsigned 0xD3, null 0xFF: its bits in raw), or one kept as bytes (binary 0xA1, the 64-bit
// 0xD4 / 0xD5 / 0xD6). A container two entries of the file point at is one node here, so a change to it shows in both:
// to change one of them, build a new node and put it in the parent (byml_clone, byml_set).
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { BYML_STRING = 0xA0, BYML_BINARY = 0xA1, BYML_ARRAY = 0xC0, BYML_DICT = 0xC1, BYML_BOOL = 0xD0, BYML_INT = 0xD1,
       BYML_FLOAT = 0xD2, BYML_UINT = 0xD3, BYML_NULL = 0xFF };

typedef struct BymlNode BymlNode;
struct BymlNode {
    uint8_t type;
    uint32_t raw;          // a 32-bit scalar's bits
    uint32_t n;            // array / dictionary: entries
    BymlNode **items;      // array / dictionary: the values
    const char **keys;     // dictionary: the key of each value
    const char *str;       // string
    const uint8_t *blob;   // hash map, binary, 64-bit value: its bytes as in the file
    size_t blob_n;
    uint32_t out;          // byml_write: where it went (0: not written yet)
    uint8_t busy;          // byml_parse: being read (a file that points at itself)
};
typedef struct Byml Byml;

Byml *byml_parse(const uint8_t *data, size_t n);   // NULL when the bytes are not a table this reads; data must outlive it
void byml_free(Byml *b);
BymlNode *byml_root(Byml *b);
BymlNode *byml_get(const BymlNode *dict, const char *key);   // NULL: no such key, or not a dictionary
// New nodes, owned by b. Strings are copied.
BymlNode *byml_scalar(Byml *b, uint8_t type, uint32_t raw);
BymlNode *byml_string(Byml *b, const char *s);
BymlNode *byml_array(Byml *b, BymlNode *const *items, uint32_t n);
BymlNode *byml_clone(Byml *b, const BymlNode *container);   // an array or a dictionary with the same entries (shallow)
int byml_set(BymlNode *dict, const char *key, BymlNode *value);   // replaces the value of a key the dictionary has; 0: it has not
// The table as bytes (malloc'd): header, key table, string table, containers, equal ones written once. NULL: no memory.
uint8_t *byml_write(Byml *b, size_t *out_n);
