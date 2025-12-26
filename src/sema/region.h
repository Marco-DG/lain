#ifndef SEMA_REGION_H
#define SEMA_REGION_H

/*
 * Region-Based Borrowing for Lain
 * 
 * Regions are lexical scopes where variables are valid.
 * This module tracks borrows and ensures references don't outlive their owners.
 * 
 * Key invariants:
 * 1. A borrow's region must not outlive the owner's region
 * 2. Only one mutable borrow OR multiple shared borrows at a time (aliasing rule)
 * 3. No borrow can be used after owner is moved
 */

#include "../ast.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Debug flag
#ifndef SEMA_REGION_DEBUG
#define SEMA_REGION_DEBUG 0
#endif

#if SEMA_REGION_DEBUG
#define REGION_DBG(fmt, ...) fprintf(stderr, "[region] " fmt "\n", ##__VA_ARGS__)
#else
#define REGION_DBG(fmt, ...) do {} while(0)
#endif

/*───────────────────────────────────────────────────────────────────╗
│ Region: represents a lexical scope                                │
╚───────────────────────────────────────────────────────────────────*/

static int next_region_id = 0;

typedef struct Region {
    int id;                    // unique identifier
    int depth;                 // nesting depth (0 = function scope)
    struct Region *parent;     // enclosing scope (NULL for function scope)
} Region;

// Create a new region as child of parent
static Region *region_new(Arena *arena, Region *parent) {
    Region *r = arena_push_aligned(arena, Region);
    r->id = next_region_id++;
    r->depth = parent ? parent->depth + 1 : 0;
    r->parent = parent;
    REGION_DBG("region_new: id=%d depth=%d parent=%d", 
               r->id, r->depth, parent ? parent->id : -1);
    return r;
}

// Check if region 'inner' is contained within 'outer' (inner outlived by outer)
// Returns true if 'outer' contains 'inner' (inner will die before outer)
static bool region_contains(Region *outer, Region *inner) {
    if (!outer || !inner) return false;
    // Walk up from inner to see if we reach outer
    for (Region *r = inner; r; r = r->parent) {
        if (r->id == outer->id) return true;
    }
    return false;
}

// Check if two regions are the same or one is ancestor of other
static bool region_related(Region *a, Region *b) {
    return region_contains(a, b) || region_contains(b, a);
}

/*───────────────────────────────────────────────────────────────────╗
│ BorrowEntry: tracks an active borrow                              │
╚───────────────────────────────────────────────────────────────────*/

typedef struct BorrowEntry {
    Id *var;                   // the variable being borrowed
    Id *owner_var;             // the original owner (for tracking moves)
    OwnershipMode mode;        // MODE_SHARED or MODE_MUTABLE
    Region *borrow_region;     // scope where the borrow is used
    Region *owner_region;      // scope where the owner is defined
    struct BorrowEntry *next;
} BorrowEntry;

/*───────────────────────────────────────────────────────────────────╗
│ BorrowTable: tracks all active borrows in current function        │
╚───────────────────────────────────────────────────────────────────*/

typedef struct BorrowTable {
    BorrowEntry *head;
    Region *current_region;    // current scope
    Region *function_region;   // root scope for this function
} BorrowTable;

// Create new borrow table for a function
static BorrowTable *borrow_table_new(Arena *arena) {
    BorrowTable *t = arena_push_aligned(arena, BorrowTable);
    t->head = NULL;
    t->function_region = region_new(arena, NULL);
    t->current_region = t->function_region;
    return t;
}

// Enter a new scope (e.g., if body, for body, block)
static Region *borrow_enter_scope(Arena *arena, BorrowTable *t) {
    Region *r = region_new(arena, t->current_region);
    t->current_region = r;
    return r;
}

// Exit current scope - invalidates borrows in this scope
static void borrow_exit_scope(BorrowTable *t) {
    if (t->current_region && t->current_region->parent) {
        REGION_DBG("borrow_exit_scope: leaving region %d, back to %d",
                   t->current_region->id, t->current_region->parent->id);
        t->current_region = t->current_region->parent;
    }
}

// Find existing borrow for a variable
static BorrowEntry *borrow_find(BorrowTable *t, Id *var) {
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (e->var->length == var->length &&
            strncmp(e->var->name, var->name, var->length) == 0) {
            return e;
        }
    }
    return NULL;
}

// Count borrows for a specific owner
static int borrow_count_for_owner(BorrowTable *t, Id *owner, OwnershipMode mode) {
    int count = 0;
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (e->owner_var && 
            e->owner_var->length == owner->length &&
            strncmp(e->owner_var->name, owner->name, owner->length) == 0) {
            if (mode == MODE_MUTABLE || e->mode == mode) {
                count++;
            }
        }
    }
    return count;
}

// Check if a borrow would conflict with existing borrows (aliasing rules)
// Returns true if conflict detected
static bool borrow_check_conflict(BorrowTable *t, Id *owner, OwnershipMode requested) {
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (!e->owner_var) continue;
        if (e->owner_var->length != owner->length) continue;
        if (strncmp(e->owner_var->name, owner->name, owner->length) != 0) continue;
        
        // Found existing borrow of the same owner
        if (requested == MODE_MUTABLE) {
            // Requesting mutable: conflicts with ANY existing borrow
            fprintf(stderr, "borrow error: cannot borrow '%.*s' as mutable because it is already borrowed\n",
                    (int)owner->length, owner->name);
            return true;
        } else if (e->mode == MODE_MUTABLE) {
            // Requesting shared: conflicts with existing MUTABLE borrow
            fprintf(stderr, "borrow error: cannot borrow '%.*s' as shared because it is borrowed as mutable\n",
                    (int)owner->length, owner->name);
            return true;
        }
        // Multiple shared borrows are OK
    }
    return false;
}

// Register a new borrow
static void borrow_register(Arena *arena, BorrowTable *t, Id *var, Id *owner, 
                           OwnershipMode mode, Region *owner_region) {
    // Check for conflicts first
    if (borrow_check_conflict(t, owner, mode)) {
        exit(1);  // Fatal error on conflict
    }
    
    // Check that borrow doesn't outlive owner
    if (!region_contains(owner_region, t->current_region)) {
        fprintf(stderr, "borrow error: reference '%.*s' would outlive its owner\n",
                (int)var->length, var->name);
        exit(1);
    }
    
    // Create entry
    BorrowEntry *e = arena_push_aligned(arena, BorrowEntry);
    e->var = var;
    e->owner_var = owner;
    e->mode = mode;
    e->borrow_region = t->current_region;
    e->owner_region = owner_region;
    e->next = t->head;
    t->head = e;
    
    REGION_DBG("borrow_register: '%.*s' borrows '%.*s' as %s in region %d",
               (int)var->length, var->name,
               (int)owner->length, owner->name,
               mode == MODE_MUTABLE ? "mut" : "shared",
               t->current_region->id);
}

// Invalidate all borrows of a specific owner (called when owner is moved)
static void borrow_invalidate_owner(BorrowTable *t, Id *owner) {
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (e->owner_var && 
            e->owner_var->length == owner->length &&
            strncmp(e->owner_var->name, owner->name, owner->length) == 0) {
            REGION_DBG("borrow_invalidate: '%.*s' invalidated (owner moved)",
                       (int)e->var->length, e->var->name);
            e->owner_var = NULL;  // Mark as invalid
        }
    }
}

// Check if a variable has active borrows (prevents move while borrowed)
static bool borrow_is_borrowed(BorrowTable *t, Id *owner) {
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (e->owner_var && 
            e->owner_var->length == owner->length &&
            strncmp(e->owner_var->name, owner->name, owner->length) == 0) {
            return true;
        }
    }
    return false;
}

// Check if using a borrowed reference after owner was moved
static bool borrow_check_use_after_move(BorrowTable *t, Id *var) {
    BorrowEntry *e = borrow_find(t, var);
    if (e && e->owner_var == NULL) {
        fprintf(stderr, "borrow error: use of reference '%.*s' after owner was moved\n",
                (int)var->length, var->name);
        return true;
    }
    return false;
}

// Clear all borrows (called after each statement to implement NLL-like behavior)
// This allows sequential borrows of the same variable across statements
static void borrow_clear_all(BorrowTable *t) {
    if (!t) return;
    REGION_DBG("borrow_clear_all: clearing all active borrows");
    t->head = NULL;  // Simply reset the list (arena allocations will be freed later)
}

#endif /* SEMA_REGION_H */
