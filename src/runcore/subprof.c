/*
Copyright (C) 2001-2011, Parrot Foundation.

=head1 NAME

src/runcore/subprof.c - Parrot's subroutine-level profiler

=head2 Functions

=over 4

=cut

*/

#include "parrot/runcore_api.h"
#include "parrot/embed.h"
#include "parrot/runcore_subprof.h"

#include "parrot/oplib/ops.h"
#include "parrot/oplib/core_ops.h"
#include "parrot/dynext.h"

#include "pmc/pmc_sub.h"
#include "pmc/pmc_callcontext.h"

#ifdef WIN32
#  define getpid _getpid
#endif

/* HEADERIZER HFILE: include/parrot/runcore_subprof.h */

/* HEADERIZER BEGIN: static */
/* Don't modify between HEADERIZER BEGIN / HEADERIZER END.  Your changes will be lost. */

static void buildcallchain(PARROT_INTERP, PMC *ctx, PMC *subpmc)
        __attribute__nonnull__(1);

static void finishcallchain(PARROT_INTERP)
        __attribute__nonnull__(1);

static void popcallchain(PARROT_INTERP)
        __attribute__nonnull__(1);

static void printspline(PARROT_INTERP, subprofile *sp)
        __attribute__nonnull__(1);

static void printspname(PARROT_INTERP, subprofile *sp)
        __attribute__nonnull__(1);

static inline const char * str2cs(PARROT_INTERP, STRING *s)
        __attribute__nonnull__(1);

static subprofile * sub2subprofile(PARROT_INTERP, PMC *ctx, PMC *subpmc)
        __attribute__nonnull__(1);

#define ASSERT_ARGS_buildcallchain __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_finishcallchain __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_popcallchain __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_printspline __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_printspname __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_str2cs __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_sub2subprofile __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
/* Don't modify between HEADERIZER BEGIN / HEADERIZER END.  Your changes will be lost. */
/* HEADERIZER END: static */


static subprofiledata spdata;

static void
createlines(PARROT_INTERP, subprofile *sp)
{
    if (sp->subattrs->seg->annotations) {
        PackFile_Annotations *ann = sp->subattrs->seg->annotations;
        PackFile_Annotations_Key *key;
        int i, j, cnt, first;

        STRING *line_str = Parrot_str_new_constant(interp, "line");
        /* search for the first line annotation in our sub */
        for (i = 0; i < ann->num_keys; i++) {
            STRING * const test_key = ann->code->const_table->str.constants[ann->keys[i].name];
            if (STRING_equal(interp, test_key, line_str))
                break;
        }
        cnt = 0;
        first = 0;
        if (i < ann->num_keys) {
            unsigned int j;
            key = ann->keys + i;
            for (j = key->start; j < key->start + key->len; j++) {
                if ((size_t)ann->base.data[j * 2 + ANN_ENTRY_OFF] < sp->subattrs->start_offs)
                    continue;
                if ((size_t)ann->base.data[j * 2 + ANN_ENTRY_OFF] >= sp->subattrs->end_offs)
                    break;
                if (!cnt++)
                    first = j;
            }
        }
        if (cnt) {
            size_t off;
            sp->lines = (lineinfo *)calloc(sizeof(lineinfo), cnt);
            for (j = 0; j < cnt; j++, first++) {
                sp->lines[j].startop = ann->base.data[first * 2 + ANN_ENTRY_OFF];
                if (j && sp->lines[j - 1].startop == sp->lines[j].startop) {
                    /* no zero size segments, please */
                    j--;
                    cnt--;
                }
                sp->lines[j].line = ann->base.data[first * 2 + ANN_ENTRY_VAL];
                if (j)
                    sp->lines[j - 1].endop = sp->lines[j].startop;
            }
            sp->lines[0].startop = sp->subattrs->start_offs;    /* workaround */
            sp->lines[cnt - 1].endop = sp->subattrs->end_offs;
            if (sp->lines[cnt - 1].startop == sp->lines[cnt - 1].endop)
                cnt--;
            sp->nlines = cnt;
        }
    }
    if (!sp->nlines) {
        INTVAL line = Parrot_sub_get_line_from_pc(interp, sp->subpmc, sp->code_ops + sp->subattrs->start_offs);
        sp->lines = (lineinfo *)calloc(sizeof(lineinfo), 1);
        sp->lines[0].line = line;
        sp->lines[0].startop = sp->subattrs->start_offs;
        sp->lines[0].endop = sp->subattrs->end_offs;
        sp->nlines = 1;
    }
}

/*        

=item *C<static thingy sub2subprofile(...)>

...

=cut

*/

static subprofile *
sub2subprofile(PARROT_INTERP, PMC *ctx, PMC *subpmc)
{
    Parrot_Sub_attributes *subattrs;
    int h;
    subprofile *sp, **spp;
    static subprofile *lastsp;

    PMC_get_sub(interp, subpmc, subattrs);
    if (lastsp && lastsp->subattrs == subattrs)
        return lastsp;
    h = ((int)subattrs >> 5) & 32767;
    for (spp = spdata.subprofilehash + h; (sp = *spp) != 0; spp = &sp->hnext)
        if (sp->subattrs == subattrs)
            break;
    if (!sp) {
        sp           = (subprofile *)calloc(sizeof(subprofile), 1);
        sp->subattrs = subattrs;
        sp->subpmc   = subpmc;
        sp->code_ops = sp->subattrs->seg->base.data;
        *spp         = sp;
        createlines(interp, sp);
        if (!spdata.have_profile_data)
            spdata.have_profile_data = 1;
    }
    lastsp = sp;
    return sp;
}

/*

=item *C<static thingy str2cs(...)>

Convert a STRING* to a char*, or a STRINGNULL to "STRINGNULL".

=cut

*/

static inline const char *
str2cs(PARROT_INTERP, STRING *s)
{
    if (s == STRINGNULL)
        return "STRNULL";
    return Parrot_str_to_cstring(interp, s);
}

/*

=item *C<static thingy popcallchain(...)>

...

=cut

*/

static void
popcallchain(PARROT_INTERP)
{
    subprofile *sp = spdata.cursp;
    subprofile *csp = sp->caller;
    if (csp) {
        sp->callerci->ops   += sp->callerops;
        sp->callerci->ticks += sp->callerticks;
        csp->callerops      += sp->callerops;
        csp->callerticks    += sp->callerticks;
    }
    sp->ctx         = 0;
    sp->callerops   = 0;
    sp->callerticks = 0;
    sp->caller      = 0;
    sp->callerci    = 0;
    sp->ctx         = 0;
    spdata.cursubpmc       = csp ? csp->subpmc : 0;
    spdata.curctx          = csp ? csp->ctx : 0;
    spdata.cursp           = csp;
}

/*

=item * C<static void finishcallchain(PARROT_INTERP)>

Propagate timing information up the call chain, clearing out old frames during
the process.

=cut

*/

static void
finishcallchain(PARROT_INTERP)
{
    subprofile *sp, *csp;

    /* finish all calls */
    for (sp = spdata.cursp; sp; sp = csp) {
        csp = sp->caller;
        if (csp) {
            sp->callerci->ops   += sp->callerops;
            sp->callerci->ticks += sp->callerticks;
            csp->callerops      += sp->callerops;
            csp->callerticks    += sp->callerticks;
        }
        sp->callerops   = 0;
        sp->callerticks = 0;
        sp->caller      = 0;
        sp->callerci    = 0;
        sp->ctx         = 0;
    }
    spdata.cursp     = 0;
    spdata.curctx    = 0;
    spdata.cursubpmc = 0;
}

/*

=item * C<static void buildcallchain(...)>

...

=cut

*/

static void
buildcallchain(PARROT_INTERP, PMC *ctx, PMC *subpmc)
{
    PMC *cctx;
    subprofile *sp;

    cctx = Parrot_pcc_get_caller_ctx(interp, ctx);
    if (cctx) {
        PMC *csubpmc = Parrot_pcc_get_sub(interp, cctx);
        if (spdata.curctx != cctx || spdata.cursubpmc != csubpmc)
            buildcallchain(interp, cctx, csubpmc);
    }
    if (PMC_IS_NULL(subpmc))
        return;

    /* find the correct subprofile */
    sp = sub2subprofile(interp, ctx, subpmc);
    while (sp->ctx) {
        /* recursion! */
        if (!sp->rnext) {
            subprofile *rsp;
            rsp           = (subprofile *)calloc(sizeof(subprofile), 1);
            rsp->subattrs = sp->subattrs;
            rsp->subpmc   = sp->subpmc;
            rsp->code_ops = sp->code_ops;
            rsp->rcnt     = sp->rcnt + 1;
            sp->rnext     = rsp;
            if (sp->nlines) {
                int i;
                rsp->lines = (lineinfo *)calloc(sizeof(lineinfo), sp->nlines);
		rsp->nlines = sp->nlines;
                for (i = 0; i < sp->nlines; i++) {
                    rsp->lines[i].line    = sp->lines[i].line;
                    rsp->lines[i].startop = sp->lines[i].startop;
                    rsp->lines[i].endop   = sp->lines[i].endop;
                }
            }
        }
        sp = sp->rnext;
    }

    sp->ctx = ctx;
    sp->caller = spdata.cursp;
    if (sp->caller) {
        int i;
        subprofile *csp = sp->caller;
        lineinfo *li;
	callinfo *ci;

        /* get caller pc */
        opcode_t *cpc_op = Parrot_pcc_get_pc(interp, csp->ctx);
        size_t cpc = cpc_op ? cpc_op - csp->code_ops : 0;
	if (cpc > csp->subattrs->start_offs)
	    cpc--;

        /* convert cpc into line */
        for (i = 0, li = csp->lines; i < csp->nlines; i++, li++)
            if (cpc >= li->startop && cpc < li->endop)
                break;
        if (i == csp->nlines)
            li = csp->lines;    /* just in case */

        /* add caller to line */
	if (!li->calls) {
	    li->calls = (callinfo *)malloc(sizeof(*ci) * (1 + 8));
	    ci = li->calls;
	    ci->callee = 0;
	} else {
	    for (ci = li->calls; ci->callee; ci++)
		if (ci->callee == sp)
		    break;
	    if (!ci->callee) {
		int ncalls = ci - li->calls;
		if ((ncalls & 7) == 0) {
		    li->calls = (callinfo *)realloc(li->calls, sizeof(*ci) * (ncalls + (1 + 8)));
		    ci = li->calls + ncalls;
		}
	    }
	}
	if (!ci->callee) {
            memset(ci, 0, sizeof(*ci));
            ci->callee = sp;
	    ci[1].callee = 0;
	}
        sp->callerci = ci;
    } else {
        sp->callerci = 0;
    }
    spdata.cursp     = sp;
    spdata.curctx    = ctx;
    spdata.cursubpmc = subpmc;
}

static void
printspname(PARROT_INTERP, subprofile *sp)
{
    fprintf(stderr, "%p:%s", sp, str2cs(interp, sp->subattrs->name));
    if (sp->rcnt)
        fprintf(stderr, "'%d", sp->rcnt);
}

static void
printspline(PARROT_INTERP, subprofile *sp)
{
    int i;

    if (!sp->subattrs || !sp->subattrs->seg)
        return;

    /* try HLL annotations */
    if (sp->subattrs->seg->annotations) {
        PackFile_Annotations *ann = sp->subattrs->seg->annotations;
        PackFile_Annotations_Key *key;
        STRING *line_str = Parrot_str_new_constant(interp, "line");

        /* search for the first line annotation in our sub */
        for (i = 0; i < ann->num_keys; i++) {
            STRING * const test_key = ann->code->const_table->str.constants[ann->keys[i].name];
            if (STRING_equal(interp, test_key, line_str))
                break;
        }
        if (i < ann->num_keys) {
            /* ok, found the line key, now search for the sub */
            unsigned int j;
            key = ann->keys + i;
            for (j = key->start; j < key->start + key->len; j++) {
                if ((size_t)ann->base.data[j * 2 + ANN_ENTRY_OFF] < sp->subattrs->start_offs)
                    continue;
                if ((size_t)ann->base.data[j * 2 + ANN_ENTRY_OFF] >= sp->subattrs->end_offs)
                    continue;
                break;
            }
            if (j < key->start + key->len) {
                /* found it! */
                INTVAL line = ann->base.data[j * 2 + ANN_ENTRY_VAL];
                /* need +1, sigh */
                PMC *pfile = PackFile_Annotations_lookup(interp, ann, ann->base.data[j * 2 + ANN_ENTRY_OFF] + 1, Parrot_str_new_constant(interp, "file"));
#if 0
                if (PMC_IS_NULL(pfile))
                    fprintf(stderr, "???:%d", (int)line);
                else
                    fprintf(stderr, "%s:%d", str2cs(interp, VTABLE_get_string(interp, pfile)), (int)line);
#else
                if (PMC_IS_NULL(pfile))
                    fprintf(stderr, "???");
                else
                    fprintf(stderr, "%s", str2cs(interp, VTABLE_get_string(interp, pfile)));
#endif
                return;
            }
        }
    }
    /* try the debug section */
    if (sp->subattrs->seg->debugs) {
        STRING *file = Parrot_sub_get_filename_from_pc(interp, sp->subpmc, sp->code_ops + sp->subattrs->start_offs);
        INTVAL line = Parrot_sub_get_line_from_pc(interp, sp->subpmc, sp->code_ops + sp->subattrs->start_offs);
#if 0
        fprintf(stderr, "%s:%d", str2cs(interp, file), (int)line);
#else
        fprintf(stderr, "%s", str2cs(interp, file));
#endif
    }
}

/*

=item * C<void dump_profile_data(PARROT_INTERP)>

After the program has completed, print the resulting callgrind-compatible
profile to stderr.

=cut

*/


void
dump_profile_data(PARROT_INTERP)
{
    int h;
    size_t off;

    unsigned int totalops = 0;
    uint64_t totalticks = 0;

    if (!spdata.have_profile_data)
        return;

    finishcallchain(interp);    /* just in case... */

    for (h = 0; h < 32768; h++) {
        subprofile *hsp;
        for (hsp = spdata.subprofilehash[h]; hsp; hsp = hsp->hnext) {
            subprofile *sp;
            for (sp = hsp; sp; sp = sp->rnext) {
		int j;
                for (j = 0; j < sp->nlines; j++) {
                    totalops += sp->lines[j].ops;
                    totalticks += sp->lines[j].ticks;
                }
            }
        }
    }
    fprintf(stderr, "events: ops ticks\n");
    fprintf(stderr, "summary: %d %lld\n", totalops, totalticks);

    for (h = 0; h < 32768; h++) {
        subprofile *hsp;
        for (hsp = spdata.subprofilehash[h]; hsp; hsp = hsp->hnext) {
            subprofile *sp;
            for (sp = hsp; sp; sp = sp->rnext) {
                int i, j;

                fprintf(stderr, "\n");
                fprintf(stderr, "fl=");
                printspline(interp, sp);
                fprintf(stderr, "\n");
                fprintf(stderr, "fn=");
                printspname(interp, sp);
                fprintf(stderr, "\n");
                for (j = 0; j < sp->nlines; j++) {
                    lineinfo *li = sp->lines + j;
		    callinfo *ci;
		    if (li->ops || li->ticks)
                        fprintf(stderr, "%d %d %lld\n", li->line, li->ops, li->ticks);
                    for (ci = li->calls; ci && ci->callee; ci++) {
                        subprofile *csp = ci->callee;
                        fprintf(stderr, "cfl=");
                        printspline(interp, csp);
                        fprintf(stderr, "\n");
                        fprintf(stderr, "cfn=");
                        printspname(interp, csp);
                        fprintf(stderr, "\n");
                        fprintf(stderr, "calls=%d %d\n", ci->count, csp->lines[0].line);
                        fprintf(stderr, "%d %d %lld\n", li->line, ci->ops, ci->ticks);
                    }
                }
            }
        }
    }
    fprintf(stderr, "\ntotals: %d %lld\n", totalops, totalticks);
}

void
mark_profile_data(PARROT_INTERP)
{
    int h;

    if (!spdata.have_profile_data)
        return;
    for (h = 0; h < 32768; h++) {
        subprofile *sp;
        for (sp = spdata.subprofilehash[h]; sp; sp = sp->hnext) {
	    Parrot_gc_mark_PMC_alive(interp, sp->subpmc);
        }
    }
}


static __inline__ uint64_t rdtsc(void) {
    uint32_t lo, hi; 
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return (uint64_t)hi << 32 | lo; 
}



/*

=item * C<void sync_callchain(...)>

bring the profile context chain back in sync with the context's call chain

=cut

*/

static void
sync_callchainchange(PARROT_INTERP, PMC *ctx, PMC *subpmc)
{
    subprofile *sp = spdata.cursp;
    int i;

    if (sp) {
        /* optimize common cases */
        /* did we just return? */
        if (sp->caller && sp->caller->subpmc == subpmc && sp->caller->ctx == ctx) {
            /* a simple return */
            popcallchain(interp);
        }
        else {
            PMC *cctx = Parrot_pcc_get_caller_ctx(interp, ctx);
            PMC *csubpmc = Parrot_pcc_get_sub(interp, cctx);
            if (spdata.curctx == cctx && spdata.cursubpmc == csubpmc) {
                /* a simple call */
                buildcallchain(interp, ctx, subpmc);
            }
            else if (sp->caller && sp->caller->subpmc == csubpmc && sp->caller->ctx == cctx) {
                /* some kind of tailcall */
                popcallchain(interp);
                buildcallchain(interp, ctx, subpmc);
            }
        }
    }
    if (subpmc != spdata.cursubpmc || ctx != spdata.curctx) {
        /* out of luck! redo call chain */
        finishcallchain(interp);
        buildcallchain(interp, ctx, subpmc);
    }

}


/*

=item * C<void sync_callchain(...)>

bring the line data in sync with the pc

=cut

*/

static void
sync_linechange(PARROT_INTERP, opcode_t *pc_op)
{
    subprofile *sp = spdata.cursp;
    lineinfo *li;
    int i;
    size_t pc;

    if (sp->nlines > 1) {
        pc = pc_op ? pc_op - sp->code_ops : 0;
        for (i = 0, li = sp->lines; i < sp->nlines; i++, li++)
            if (pc >= li->startop && pc < li->endop)
                break;
        if (i == sp->nlines)
            li = sp->lines;    /* just in case */
    } else {
	li = sp->lines;
    }
    spdata.curline = li;
    spdata.startop = sp->code_ops + li->startop;
    spdata.endop   = sp->code_ops + li->endop;
}

/*

=item * C<void profile(...)>

main entry point, does all accounting for a single op

=cut

*/

void
profile(PARROT_INTERP, PMC *ctx, opcode_t *pc)
{
    PMC *subpmc = ((Parrot_Context *)PMC_data_typed(ctx, Parrot_Context*))->current_sub;
    subprofile *sp;

    if (PMC_IS_NULL(subpmc))
        return;

    if (subpmc != spdata.cursubpmc || ctx != spdata.curctx) {
        /* context changed! either called new sub or returned from sub */

        /* finish old ticks */
        uint64_t tick = rdtsc();
        if (spdata.tickadd) {
            uint64_t tickdiff = tick - spdata.starttick;
            *spdata.tickadd         += tickdiff;
            *spdata.tickadd2        += tickdiff;
        }
        sync_callchainchange(interp, ctx, subpmc);
        sync_linechange(interp, pc);
        sp = spdata.cursp;
        if (pc == sp->code_ops + sp->subattrs->start_offs) {
            /* assume new call */
            if (sp->callerci)
                sp->callerci->count++;
        }
        spdata.tickadd   = &spdata.curline->ticks;
        spdata.tickadd2  = &sp->callerticks;
        spdata.starttick = rdtsc();
    }

    if (pc >= spdata.endop) {
        /* finish old ticks */
        uint64_t tick = rdtsc();
        if (spdata.tickadd) {
            uint64_t tickdiff = tick - spdata.starttick;
            *spdata.tickadd         += tickdiff;
            *spdata.tickadd2        += tickdiff;
        }
        spdata.starttick = tick;
        sp = spdata.cursp;
        while (pc >= sp->code_ops + spdata.curline->endop) {
            spdata.curline++;
        }
        spdata.startop = sp->code_ops + spdata.curline->startop;
        spdata.endop   = sp->code_ops + spdata.curline->endop;
        spdata.tickadd = &spdata.curline->ticks;
    }
    else if (pc < spdata.startop) {
        /* finish old ticks */
        uint64_t tick = rdtsc();
        if (spdata.tickadd) {
            uint64_t tickdiff = tick - spdata.starttick;
            *spdata.tickadd         += tickdiff;
            *spdata.tickadd2        += tickdiff;
        }
        spdata.starttick = tick;
        sp = spdata.cursp;
        while (pc < sp->code_ops + spdata.curline->startop) {
            spdata.curline--;
        }
        spdata.startop = sp->code_ops + spdata.curline->startop;
        spdata.endop   = sp->code_ops + spdata.curline->endop;
        spdata.tickadd = &spdata.curline->ticks;
    }
    spdata.curline->ops++;
    spdata.cursp->callerops++;
}

/*

=back

=cut

*/

/*
 * Local variables:
 *   c-file-style: "parrot"
 * End:
 * vim: expandtab shiftwidth=4 cinoptions='\:2=2' :
 */
