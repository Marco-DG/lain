#include "utils/common/def.h"
#include "utils/file.h" /* file */

#include "utils/arena.h"
#include "utils/common/system/memory.h" /* memory_alloc, MEMORY_PAGE_MINIMUM_SIZE */

#include "args.h"



int main(int argc, char **argv) {

    Args args = args_parse(argc, argv);

    Arena file_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*128);

    File f = file_read_into_arena(&file_arena, args.filename);
    if (!f.contents) {
        fprintf(stderr, "Error: Cannot open module file '%s'\n", args.filename);
        exit(1);
    }

    


    return 0;
}