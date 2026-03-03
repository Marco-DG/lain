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
    Id *binding_id;            // variable that holds the borrow alive (NULL for temporaries)
    OwnershipMode mode;        // MODE_SHARED or MODE_MUTABLE
    Region *borrow_region;     // scope where the borrow is used
    Region *owner_region;      // scope where the owner is defined
    bool is_temporary;         // true if borrow expires at end of statement
    int last_use_stmt_idx;     // NLL: last top-level stmt index where binding is used (-1 = unknown)
    struct BorrowEntry *next;
} BorrowEntry;

typedef struct BorrowTable {
    BorrowEntry *head;
    Region *current_region;
    Arena *arena;
} BorrowTable;

static BorrowTable *borrow_table_new(Arena *arena) {
    BorrowTable *t = arena_push_aligned(arena, BorrowTable);
    t->head = NULL;
    t->current_region = region_new(arena, NULL); // Root region
    t->arena = arena;
    return t;
}

// Implementations
static BorrowEntry *borrow_find(BorrowTable *t, Id *var) {
    if (!t) return NULL;
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (e->var->length == var->length &&
            strncmp(e->var->name, var->name, var->length) == 0) {
            return e;
        }
    }
    return NULL;
}

static bool borrow_check_conflict(BorrowTable *t, Id *owner, OwnershipMode mode) {
    if (!t) return false;
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (e->owner_var && 
            e->owner_var->length == owner->length &&
            strncmp(e->owner_var->name, owner->name, owner->length) == 0) {
            
            if (e->mode == MODE_MUTABLE) {
                fprintf(stderr, "borrow error: cannot borrow '%.*s' because it is already mutably borrowed\n",
                        (int)owner->length, owner->name);
                return true;
            }
            if (mode == MODE_MUTABLE) {
                fprintf(stderr, "borrow error: cannot borrow '%.*s' as mutable because it is already borrowed\n",
                        (int)owner->length, owner->name);
                return true;
            }
        }
    }
    return false;
}

static void borrow_enter_scope(Arena *arena, BorrowTable *t) {
    if (!t) return;
    t->current_region = region_new(arena, t->current_region);
}

static void borrow_exit_scope(BorrowTable *t) {
    if (!t || !t->current_region->parent) return;
    t->current_region = t->current_region->parent;
}

// Register a new borrow
static void borrow_register(Arena *arena, BorrowTable *t, Id *var, Id *owner, 
                           OwnershipMode mode, Region *owner_region, bool is_temporary) {
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
    e->binding_id = NULL;
    e->mode = mode;
    e->borrow_region = t->current_region;
    e->owner_region = owner_region;
    e->is_temporary = is_temporary;
    e->next = t->head;
    t->head = e;
    
    REGION_DBG("borrow_register: '%.*s' borrows '%.*s' as %s in region %d (temp=%d)",
               (int)var->length, var->name,
               (int)owner->length, owner->name,
               mode == MODE_MUTABLE ? "mut" : "shared",
               t->current_region->id, is_temporary);
}

// Register a persistent borrow bound to a variable (NLL cross-statement)
// The borrow lives until the last use of binding_id (NLL) or scope exit.
static void borrow_register_persistent(Arena *arena, BorrowTable *t, Id *binding_id, 
                                        Id *owner, OwnershipMode mode, Region *owner_region,
                                        int last_use_stmt_idx) {
    // Check for conflicts with existing borrows
    if (borrow_check_conflict(t, owner, mode)) {
        exit(1);
    }
    
    // Create entry — NOT temporary
    BorrowEntry *e = arena_push_aligned(arena, BorrowEntry);
    e->var = binding_id;  // the reference variable IS the borrow
    e->owner_var = owner;
    e->binding_id = binding_id;
    e->mode = mode;
    e->borrow_region = t->current_region;
    e->owner_region = owner_region;
    e->is_temporary = false;
    e->last_use_stmt_idx = last_use_stmt_idx;
    e->next = t->head;
    t->head = e;
    
    REGION_DBG("borrow_register_persistent: '%.*s' borrows '%.*s' as %s (binding='%.*s', last_use=%d)",
               (int)binding_id->length, binding_id->name,
               (int)owner->length, owner->name,
               mode == MODE_MUTABLE ? "mut" : "shared",
               (int)binding_id->length, binding_id->name,
               last_use_stmt_idx);
}

// Release all borrows held by a specific binding (called when binding exits scope)
static void borrow_release_by_binding(BorrowTable *t, Id *binding_id) {
    if (!t || !binding_id) return;
    BorrowEntry **curr = &t->head;
    while (*curr) {
        if ((*curr)->binding_id &&
            (*curr)->binding_id->length == binding_id->length &&
            strncmp((*curr)->binding_id->name, binding_id->name, binding_id->length) == 0) {
            REGION_DBG("borrow_release_by_binding: releasing borrow of '%.*s' held by '%.*s'",
                       (*curr)->owner_var ? (int)(*curr)->owner_var->length : 0,
                       (*curr)->owner_var ? (*curr)->owner_var->name : "<null>",
                       (int)binding_id->length, binding_id->name);
            *curr = (*curr)->next; // Remove
        } else {
            curr = &(*curr)->next;
        }
    }
}

// Check if accessing an owner conflicts with active persistent borrows.
// access_mode: MODE_SHARED = read, MODE_MUTABLE = write/mutate
// Returns true and prints error if conflict detected.
static bool borrow_check_owner_access(BorrowTable *t, Id *owner, OwnershipMode access_mode,
                                       isize line, isize col) {
    if (!t || !owner) return false;
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (!e->owner_var) continue;  // invalidated borrow
        if (!e->binding_id) continue; // only check persistent borrows
        if (e->owner_var->length != owner->length) continue;
        if (strncmp(e->owner_var->name, owner->name, owner->length) != 0) continue;
        
        // Conflict rules:
        // 1. If owner is mutably borrowed: cannot read OR write
        // 2. If owner is shared-borrowed: cannot write (read is OK)
        if (e->mode == MODE_MUTABLE) {
            fprintf(stderr, "Error Ln %li, Col %li: cannot access '%.*s' because it is mutably borrowed by '%.*s'.\n",
                    (long)line, (long)col,
                    (int)owner->length, owner->name,
                    (int)e->binding_id->length, e->binding_id->name);
            return true;
        }
        if (access_mode == MODE_MUTABLE && e->mode == MODE_SHARED) {
            fprintf(stderr, "Error Ln %li, Col %li: cannot mutate '%.*s' because it is borrowed by '%.*s'.\n",
                    (long)line, (long)col,
                    (int)owner->length, owner->name,
                    (int)e->binding_id->length, e->binding_id->name);
            return true;
        }
    }
    return false;
}

// Clear temporary borrows (called after each statement)
static void borrow_clear_temporaries(BorrowTable *t) {
    if (!t) return;
    BorrowEntry **curr = &t->head;
    while (*curr) {
        if ((*curr)->is_temporary) {
            REGION_DBG("borrow_clear_temporaries: removing '%.*s'", (int)(*curr)->var->length, (*curr)->var->name);
            *curr = (*curr)->next; // Remove
        } else {
            curr = &(*curr)->next;
        }
    }
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

// NLL: Release persistent borrows whose binding has no more uses after current_stmt_idx.
// Called after each top-level statement during the linearity walk.
static void borrow_release_expired(BorrowTable *t, int current_stmt_idx) {
    if (!t) return;
    BorrowEntry **curr = &t->head;
    while (*curr) {
        BorrowEntry *e = *curr;
        // Consider persistent borrows:
        // - last_use >= 0: release when current_stmt_idx >= last_use (no more uses)
        // - last_use == -1: binding was never used → release immediately
        if (!e->is_temporary && e->binding_id) {
            bool should_release = (e->last_use_stmt_idx < 0) ||
                                  (current_stmt_idx >= e->last_use_stmt_idx);
            if (should_release) {
                REGION_DBG("borrow_release_expired: releasing borrow of '%.*s' held by '%.*s' "
                           "(last_use=%d, current=%d)",
                           e->owner_var ? (int)e->owner_var->length : 0,
                           e->owner_var ? e->owner_var->name : "<null>",
                           (int)e->binding_id->length, e->binding_id->name,
                           e->last_use_stmt_idx, current_stmt_idx);
                *curr = e->next;  // Remove from list
                continue;
            }
        }
        curr = &(*curr)->next;
    }
}

#endif /* SEMA_REGION_H */
