#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "xnu/apple_dtree.h"

/*
 * apple_dtree.c: Minimalist device tree parser
 *
 * Intentionally provides no capability of creating new nodes or resizing
 * existing nodes. Assumes that device trees we get were run through
 * dt_fixup from the darwin-vm main repository.
 */

#define ROUND_DOWN_POW2(x,sz)      ((       (x)        & (~((sz) - 1)) ))
#define ROUND_UP_POW2(x,sz)        (( ((x) + ((sz)-1)) & (~((sz) - 1)) ))

#define DTREE_ALIGNMENT 4

// BIT(31) is some flag device trees from IPSW images sometimes have set in the
// length field of a dtree prop; iBoot apparently uses and removes this flag
// during boot. We just strip it out if we encounter it.
#define STRIP_LENGTH_FLAGS(sz)     (( ((sz)) & (~BIT(31)) ))

static struct dtree_prop *next_property(struct dtree_prop *p) {
    return (struct dtree_prop*)( ((uint8_t*)(p+1)) + ROUND_UP_POW2(STRIP_LENGTH_FLAGS(p->length), DTREE_ALIGNMENT) );
}

// Returns the next node in memory after this one (skipping all properties)- aka the first child
static struct dtree_node *first_child(struct dtree_node *n) {
    struct dtree_prop *cursor = (struct dtree_prop*)(n+1);
    for (size_t i = 0; i < n->nProperties; i++) {
        cursor = next_property(cursor);
    }
    return (struct dtree_node*)cursor;
}

// Returns the next sibling node (skipping this node, all its properties, and all its children)
static struct dtree_node *next_child(struct dtree_node *n) {
    struct dtree_node *cursor = first_child(n);
    for (size_t i = 0; i < n->nChildren; i++) {
        cursor = next_child(cursor);
    }
    return cursor;
}

static struct dtree_prop *find_prop_in_node(struct dtree_node *n, const char *prop_name) {
    struct dtree_prop *cursor = (struct dtree_prop*)(n+1);

    for (size_t i = 0; i < n->nProperties; i++) {
        if (0 == strcmp(cursor->name, prop_name)) return cursor;
        cursor = next_property(cursor);
    }

    return NULL;
}

void *adt_get_prop_val(struct dtree_node *n, const char *prop_name) {
    struct dtree_prop *p = find_prop_in_node(n,prop_name);
    if (!p) return NULL;
    return (void*)(p+1);
}

size_t adt_get_prop_len(struct dtree_node *n, const char *prop_name) {
    return STRIP_LENGTH_FLAGS(find_prop_in_node(n,prop_name)->length);
}

struct dtree_node *adt_find_node(struct dtree_node *root, const char *path) {
    char *token, *string, *tofree;
    struct dtree_node *cur_node, *cur_child;

    tofree = string = strdup(path);
    cur_node = root;

    while (NULL != (token = strsep(&string, kDTPathNameSeparator))) {
        bool found_it = false;

        cur_child = first_child(cur_node);
        for (size_t i = 0; i < cur_node->nChildren; i++) {
            if (0 == strcmp(adt_get_prop_val(cur_child, "name"), token)) {
                cur_node = cur_child;
                found_it = true;
                break;
            }
            cur_child = next_child(cur_child);
        }

        if (!found_it) {
            cur_node = NULL;
            goto out;
        }
    }

out:
    free(tofree);
    return cur_node;
}
