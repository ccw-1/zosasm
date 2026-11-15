/* listing.h - the assembly listing (-l): every source line with its location
 * counter and object bytes, the diagnostics after the line they belong to, and
 * tables at the end (files, sections, USING, symbol cross reference). */
#ifndef ZA_LISTING_H
#define ZA_LISTING_H

#include <stdio.h>

#include "asm.h"
#include "pp.h"

typedef struct {
    int data;  /* all object bytes of a statement, not only the first 8 */
    int page;  /* lines per page, 0 = one continuous page */
    int brief; /* rows and diagnostics only, no tables */
} ListOptions;

/* Writes the listing of the module made from lines (the array that was given
 * to asm_assemble_all). events are the preprocessor's record of the source
 * lines (NULL: every line is its own source line); rep holds the diagnostics. */
void listing_write(FILE *f, const Module *m, const SrcVec *lines, const PpEventVec *events,
                   const Reporter *rep, const ListOptions *opt);

#endif
