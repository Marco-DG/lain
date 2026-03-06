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
static bool __attribute__((unused)) region_related(Region *a, Region *b) {
    return region_contains(a, b) || region_contains(b, a);
}

/*───────────────────────────────────────────────────────────────────╗
│ BorrowEntry: tracks an active borrow                              │
╚───────────────────────────────────────────────────────────────────*/

// Two-phase borrow: RESERVED allows shared reads, ACTIVE blocks everything
typedef enum { BORROW_RESERVED, BORROW_ACTIVE } BorrowPhase;

typedef struct BorrowEntry {
    Id *var;                   // the variable being borrowed
    Id *owner_var;             // the direct owner (for tracking moves)
    Id *root_owner;            // Phase 3: ultimate non-reference owner for transitive re-borrow chains
    Id *binding_id;            // variable that holds the borrow alive (NULL for temporaries)
    Id *borrowed_field;        // Phase 7: specific field being borrowed (NULL = whole variable)
    OwnershipMode mode;        // MODE_SHARED or MODE_MUTABLE
    BorrowPhase phase;         // Two-phase: RESERVED during arg eval, ACTIVE after
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

// Phase 7: helper to check if two field IDs overlap.
// NULL field means "whole variable" — overlaps with everything.
// Two non-NULL fields overlap only if they name the same field.
static bool fields_overlap(Id *f1, Id *f2) {
    if (!f1 || !f2) return true; // whole-var overlaps with any field
    return f1->length == f2->length && strncmp(f1->name, f2->name, f1->length) == 0;
}

static bool borrow_check_conflict_field(BorrowTable *t, Id *owner, OwnershipMode mode, Id *field);

static bool borrow_check_conflict(BorrowTable *t, Id *owner, OwnershipMode mode) {
    return borrow_check_conflict_field(t, owner, mode, NULL);
}

static bool borrow_check_conflict_field(BorrowTable *t, Id *owner, OwnershipMode mode, Id *field) {
    if (!t) return false;
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (e->owner_var && 
            e->owner_var->length == owner->length &&
            strncmp(e->owner_var->name, owner->name, owner->length) == 0) {
            
            // Phase 7: check field overlap
            if (!fields_overlap(e->borrowed_field, field)) continue; // different fields → no conflict
            
            if (e->mode == MODE_MUTABLE) {
                // Two-phase borrows: RESERVED mutable borrows allow shared reads
                if (e->phase == BORROW_RESERVED && mode == MODE_SHARED) {
                    continue; // allow shared read during reservation
                }
                if (field && e->borrowed_field) {
                    fprintf(stderr, "borrow error: cannot borrow '%.*s.%.*s' because '%.*s.%.*s' is already mutably borrowed\n",
                            (int)owner->length, owner->name, (int)field->length, field->name,
                            (int)owner->length, owner->name, (int)e->borrowed_field->length, e->borrowed_field->name);
                } else {
                    fprintf(stderr, "borrow error: cannot borrow '%.*s' because it is already mutably borrowed\n",
                            (int)owner->length, owner->name);
                }
                return true;
            }
            if (mode == MODE_MUTABLE) {
                if (field && e->borrowed_field) {
                    fprintf(stderr, "borrow error: cannot borrow '%.*s.%.*s' as mutable because '%.*s.%.*s' is already borrowed\n",
                            (int)owner->length, owner->name, (int)field->length, field->name,
                            (int)owner->length, owner->name, (int)e->borrowed_field->length, e->borrowed_field->name);
                } else {
                    fprintf(stderr, "borrow error: cannot borrow '%.*s' as mutable because it is already borrowed\n",
                            (int)owner->length, owner->name);
                }
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
    
    Region *exiting = t->current_region;
    t->current_region = t->current_region->parent;
    
    // Phase 7.1: Block-level NLL — release borrows created in the exiting scope
    // (or deeper). This prevents borrows from leaking out of if/while/block scopes.
    BorrowEntry **curr = &t->head;
    while (*curr) {
        BorrowEntry *e = *curr;
        // Release if borrow_region is the exiting scope or deeper
        if (e->borrow_region && region_contains(exiting, e->borrow_region)) {
            REGION_DBG("borrow_exit_scope: releasing scope-local borrow of '%.*s' by '%.*s'",
                       e->owner_var ? (int)e->owner_var->length : 0,
                       e->owner_var ? e->owner_var->name : "<null>",
                       e->binding_id ? (int)e->binding_id->length : 0,
                       e->binding_id ? e->binding_id->name : "<temp>");
            *curr = e->next; // Remove
        } else {
            curr = &(*curr)->next;
        }
    }
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
    e->root_owner = owner;  // Phase 3: for temporaries, root_owner == owner
    e->binding_id = NULL;
    e->borrowed_field = NULL; // Phase 7: temporaries don't have field tracking
    e->mode = mode;
    e->phase = BORROW_ACTIVE;
    e->borrow_region = t->current_region;
    e->owner_region = owner_region;
    e->is_temporary = is_temporary;
    e->last_use_stmt_idx = -1;
    e->next = t->head;
    t->head = e;
    
    REGION_DBG("borrow_register: '%.*s' borrows '%.*s' as %s in region %d (temp=%d)",
               (int)var->length, var->name,
               (int)owner->length, owner->name,
               mode == MODE_MUTABLE ? "mut" : "shared",
               t->current_region->id, is_temporary);
}

// Phase 3: Find the BorrowEntry where binding_id matches a given id.
// Used to detect re-borrow chains: if owner_id is itself a borrow binding,
// we can look up its root_owner.
static BorrowEntry *borrow_find_by_binding(BorrowTable *t, Id *binding_id) {
    if (!t || !binding_id) return NULL;
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (!e->binding_id) continue;
        if (e->binding_id->length == binding_id->length &&
            strncmp(e->binding_id->name, binding_id->name, binding_id->length) == 0) {
            return e;
        }
    }
    return NULL;
}

// Register a persistent borrow bound to a variable (NLL cross-statement)
// The borrow lives until the last use of binding_id (NLL) or scope exit.
// root_owner: the ultimate original owner (for transitive re-borrow chains).
//   If NULL, defaults to owner.
static void borrow_register_persistent(Arena *arena, BorrowTable *t, Id *binding_id, 
                                        Id *owner, OwnershipMode mode, Region *owner_region,
                                        int last_use_stmt_idx, Id *root_owner) {
    // Phase 3: determine the effective root owner.
    // If root_owner is explicitly provided (transitive re-borrow), use it.
    // Otherwise, check if 'owner' is itself a persistent borrow binding
    // and inherit its root_owner.
    Id *effective_root = root_owner;
    bool is_reborrow = false;
    if (!effective_root) {
        BorrowEntry *owner_borrow = borrow_find_by_binding(t, owner);
        if (owner_borrow && owner_borrow->root_owner) {
            effective_root = owner_borrow->root_owner;
            is_reborrow = true;
            REGION_DBG("borrow_register_persistent: transitive re-borrow detected! "
                       "'%.*s' -> '%.*s' -> root '%.*s'",
                       (int)binding_id->length, binding_id->name,
                       (int)owner->length, owner->name,
                       (int)effective_root->length, effective_root->name);
        } else {
            effective_root = owner;
        }
    }

    // Check for conflicts with the ROOT owner (not just direct owner).
    // Skip conflict check for re-borrows: they extend an existing borrow chain
    // (e.g., ref2 = transform(var ref) where ref already borrows data).
    if (!is_reborrow && borrow_check_conflict(t, effective_root, mode)) {
        exit(1);
    }
    
    // Create entry — NOT temporary
    BorrowEntry *e = arena_push_aligned(arena, BorrowEntry);
    e->var = binding_id;  // the reference variable IS the borrow
    e->owner_var = effective_root;  // Phase 3: always track against root owner
    e->root_owner = effective_root;
    e->binding_id = binding_id;
    e->borrowed_field = NULL; // Phase 7: will be set by caller if needed
    e->mode = mode;
    e->phase = BORROW_ACTIVE;
    e->borrow_region = t->current_region;
    e->owner_region = owner_region;
    e->is_temporary = false;
    e->last_use_stmt_idx = last_use_stmt_idx;
    e->next = t->head;
    t->head = e;
    
    REGION_DBG("borrow_register_persistent: '%.*s' borrows '%.*s' (root='%.*s') as %s (last_use=%d)",
               (int)binding_id->length, binding_id->name,
               (int)owner->length, owner->name,
               (int)effective_root->length, effective_root->name,
               mode == MODE_MUTABLE ? "mut" : "shared",
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
static bool borrow_check_owner_access_field(BorrowTable *t, Id *owner, OwnershipMode access_mode,
                                             Id *field, isize line, isize col);

static bool borrow_check_owner_access(BorrowTable *t, Id *owner, OwnershipMode access_mode,
                                       isize line, isize col) {
    return borrow_check_owner_access_field(t, owner, access_mode, NULL, line, col);
}

// Phase 7: field-aware owner access check
static bool borrow_check_owner_access_field(BorrowTable *t, Id *owner, OwnershipMode access_mode,
                                             Id *field, isize line, isize col) {
    if (!t || !owner) return false;
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (!e->owner_var) continue;  // invalidated borrow
        if (!e->binding_id) continue; // only check persistent borrows
        if (e->owner_var->length != owner->length) continue;
        if (strncmp(e->owner_var->name, owner->name, owner->length) != 0) continue;
        
        // Phase 7: check field overlap
        if (!fields_overlap(e->borrowed_field, field)) continue;
        
        // Conflict rules:
        // 1. If owner is mutably borrowed: cannot read OR write
        //    EXCEPT: two-phase RESERVED borrows allow shared reads
        // 2. If owner is shared-borrowed: cannot write (read is OK)
        if (e->mode == MODE_MUTABLE) {
            if (e->phase == BORROW_RESERVED && access_mode == MODE_SHARED) {
                continue; // two-phase: allow shared reads during reservation
            }
            fprintf(stderr, "Error Ln %li, Col %li: cannot access '%.*s' because it is mutably borrowed by '%.*s'.\n",
                    (long)line, (long)col,
                    (int)owner->length, owner->name,
                    (int)e->binding_id->length, e->binding_id->name);
            diagnostic_show_line(line, col);
            return true;
        }
        if (access_mode == MODE_MUTABLE && e->mode == MODE_SHARED) {
            fprintf(stderr, "Error Ln %li, Col %li: cannot mutate '%.*s' because it is borrowed by '%.*s'.\n",
                    (long)line, (long)col,
                    (int)owner->length, owner->name,
                    (int)e->binding_id->length, e->binding_id->name);
            diagnostic_show_line(line, col);
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

// Two-phase borrows: promote all RESERVED borrows to ACTIVE
// Called after all function arguments have been evaluated
static void borrow_promote_to_active(BorrowTable *t) {
    if (!t) return;
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (e->phase == BORROW_RESERVED) {
            e->phase = BORROW_ACTIVE;
            REGION_DBG("borrow_promote_to_active: promoted '%.*s' borrow of '%.*s'",
                       (int)e->var->length, e->var->name,
                       e->owner_var ? (int)e->owner_var->length : 0,
                       e->owner_var ? e->owner_var->name : "<null>");
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
static bool __attribute__((unused)) borrow_check_use_after_move(BorrowTable *t, Id *var) {
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
static void __attribute__((unused)) borrow_clear_all(BorrowTable *t) {
    if (!t) return;
    REGION_DBG("borrow_clear_all: clearing all active borrows");
    t->head = NULL;  // Simply reset the list (arena allocations will be freed later)
}

// Phase 3: Check if any other active persistent borrow shares the same root_owner
// and is still alive at current_stmt_idx.
// Used by borrow_release_expired to prevent releasing a root borrow while
// transitive dependents are still alive.
static bool borrow_has_transitive_dependent(BorrowTable *t, BorrowEntry *candidate, int current_stmt_idx) {
    if (!t || !candidate || !candidate->root_owner) return false;
    for (BorrowEntry *e = t->head; e; e = e->next) {
        if (e == candidate) continue;  // skip self
        if (!e->binding_id || e->is_temporary) continue;
        if (!e->root_owner) continue;
        // Same root owner?
        if (e->root_owner->length == candidate->root_owner->length &&
            strncmp(e->root_owner->name, candidate->root_owner->name, candidate->root_owner->length) == 0) {
            // This other borrow shares the same root — check if it's still alive
            bool other_expired = (e->last_use_stmt_idx < 0) ||
                                 (current_stmt_idx >= e->last_use_stmt_idx);
            if (!other_expired) {
                return true;  // There's a transitive dependent still alive
            }
        }
    }
    return false;
}

// NLL: Release persistent borrows whose binding has no more uses after current_stmt_idx.
// Called after each top-level statement during the linearity walk.
// Phase 3: Also checks for transitive re-borrow dependents before releasing.
// Uses a fixpoint loop so cascading releases work (ref3 → ref2 → ref).
static void borrow_release_expired(BorrowTable *t, int current_stmt_idx) {
    if (!t) return;
    bool changed = true;
    while (changed) {
        changed = false;
        BorrowEntry **curr = &t->head;
        while (*curr) {
            BorrowEntry *e = *curr;
            if (!e->is_temporary && e->binding_id) {
                bool should_release = (e->last_use_stmt_idx < 0) ||
                                      (current_stmt_idx >= e->last_use_stmt_idx);
                if (should_release) {
                    // Phase 3: Don't release if transitive dependents are still alive
                    if (borrow_has_transitive_dependent(t, e, current_stmt_idx)) {
                        REGION_DBG("borrow_release_expired: keeping borrow of '%.*s' by '%.*s' "
                                   "(transitive dependent still alive)",
                                   e->owner_var ? (int)e->owner_var->length : 0,
                                   e->owner_var ? e->owner_var->name : "<null>",
                                   (int)e->binding_id->length, e->binding_id->name);
                        curr = &(*curr)->next;
                        continue;
                    }
                    REGION_DBG("borrow_release_expired: releasing borrow of '%.*s' held by '%.*s' "
                               "(last_use=%d, current=%d)",
                               e->owner_var ? (int)e->owner_var->length : 0,
                               e->owner_var ? e->owner_var->name : "<null>",
                               (int)e->binding_id->length, e->binding_id->name,
                               e->last_use_stmt_idx, current_stmt_idx);
                    *curr = e->next;  // Remove from list
                    changed = true;   // Something changed, iterate again
                    continue;
                }
            }
            curr = &(*curr)->next;
        }
    }
}

#endif /* SEMA_REGION_H */
