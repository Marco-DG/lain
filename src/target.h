#ifndef TARGET_H
#define TARGET_H

/*
   Target configuration for codegen and niche optimization.
   See internal/ai_analysis/analysis19_aggressive_niche.md §4.4.

   The sentinel pool sizes drive aggressive multi-slot niche
   packing for enum types. Host auto-detection is the default;
   --target=<triple> overrides for cross-compilation.
*/

#include "utils/common/libc.h"

typedef struct {
    const char *triple;            // canonical target triple
    size_t      pointer_size;      // bytes
    size_t      pointer_alignment; // bytes
    size_t      zero_page_size;    // bytes of low virtual memory unmappable in user-space
    bool        has_mmu;
} TargetConfig;

// Global target. Initialized from CLI flag or host detection at startup.
static TargetConfig target;

static void target_init_host(void) {
    // Host detection via predefined macros. Default to x86_64-linux-gnu
    // when nothing else matches (conservative for the analysis pipeline).
#if defined(__x86_64__) && defined(__linux__)
    target.triple            = "x86_64-linux-gnu";
    target.pointer_size      = 8;
    target.pointer_alignment = 8;
    target.zero_page_size    = 65536;
    target.has_mmu           = true;
#elif defined(__aarch64__) && defined(__linux__)
    target.triple            = "aarch64-linux-gnu";
    target.pointer_size      = 8;
    target.pointer_alignment = 8;
    target.zero_page_size    = 65536;
    target.has_mmu           = true;
#elif defined(__APPLE__) && defined(__arm64__)
    target.triple            = "aarch64-apple-darwin";
    target.pointer_size      = 8;
    target.pointer_alignment = 8;
    target.zero_page_size    = 4096; // conservative; macOS reserves more
    target.has_mmu           = true;
#elif defined(__APPLE__) && defined(__x86_64__)
    target.triple            = "x86_64-apple-darwin";
    target.pointer_size      = 8;
    target.pointer_alignment = 8;
    target.zero_page_size    = 4096;
    target.has_mmu           = true;
#else
    // Conservative fallback. zero_page_size=4096 is the minimum
    // any sane OS reserves; safe-but-not-optimal niche allocation.
    target.triple            = "unknown";
    target.pointer_size      = sizeof(void *);
    target.pointer_alignment = sizeof(void *);
    target.zero_page_size    = 4096;
    target.has_mmu           = true;
#endif
}

static void target_init_for(const char *triple) {
    if (!triple) { target_init_host(); return; }

    if (strcmp(triple, "x86_64-linux-gnu") == 0) {
        target.triple            = "x86_64-linux-gnu";
        target.pointer_size      = 8;
        target.pointer_alignment = 8;
        target.zero_page_size    = 65536;
        target.has_mmu           = true;
    } else if (strcmp(triple, "aarch64-linux-gnu") == 0) {
        target.triple            = "aarch64-linux-gnu";
        target.pointer_size      = 8;
        target.pointer_alignment = 8;
        target.zero_page_size    = 65536;
        target.has_mmu           = true;
    } else if (strcmp(triple, "x86_64-windows-msvc") == 0) {
        target.triple            = "x86_64-windows-msvc";
        target.pointer_size      = 8;
        target.pointer_alignment = 8;
        target.zero_page_size    = 65536;
        target.has_mmu           = true;
    } else if (strcmp(triple, "cortex-m4-bare") == 0) {
        // Bare-metal: address 0 may be valid (vector table on Cortex-M).
        // No mappable zero page → no pointer niche available.
        target.triple            = "cortex-m4-bare";
        target.pointer_size      = 4;
        target.pointer_alignment = 4;
        target.zero_page_size    = 0;
        target.has_mmu           = false;
    } else if (strcmp(triple, "host") == 0) {
        target_init_host();
    } else {
        fprintf(stderr, "Error: unknown target triple '%s'.\n"
                        "Supported: x86_64-linux-gnu, aarch64-linux-gnu, "
                        "x86_64-windows-msvc, cortex-m4-bare, host.\n",
                triple);
        exit(1);
    }
}

#endif /* TARGET_H */
