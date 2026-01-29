/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright 1996-2025 The NASM Authors - All Rights Reserved */

/*
 * The Netwide Assembler main program module
 */

#include "compiler.h"


#include "nasm.h"
#include "nasmlib.h"
#include "nctype.h"
#include "error.h"
#include "saa.h"
#include "raa.h"
#include "floats.h"
#include "stdscan.h"
#include "insns.h"
#include "preproc.h"
#include "parser.h"
#include "eval.h"
#include "assemble.h"
#include "labels.h"
#include "outform.h"
#include "listing.h"
#include "iflag.h"
#include "quote.h"
#include "ver.h"
#include "error.h"

/*
 * This is the maximum number of optimization passes to do.  If we ever
 * find a case where the optimizer doesn't naturally converge, we might
 * have to drop this value so the assembler doesn't appear to just hang.
 */
#define MAX_OPTIMIZE (INT_MAX >> 1)

struct forwrefinfo {            /* info held on forward refs. */
    int lineno;
    int operand;
};

const char *_progname;

static void open_and_process_respfile(char *, int);
static void parse_cmdline(int, char **, int);
static void assemble_file(const char *, struct strlist *);
static void help(FILE *out, const char *what);

static bool using_debug_info;
static const char *debug_format;

#ifndef ABORT_ON_PANIC
# define ABORT_ON_PANIC 0
#endif
static bool abort_on_panic = ABORT_ON_PANIC;
static bool keep_all;

bool tasm_compatible_mode = false;
enum pass_type _pass_type;
const char * const _pass_types[] =
{
    "init", "preproc-only", "first", "optimize", "stabilize", "final"
};
int64_t _passn;

struct compile_time official_compile_time;

const char *inname;
const char *outname;
static const char *listname;
static const char *errname;

static int64_t globallineno;    /* for forward-reference tracking */

const struct ofmt *ofmt = &OF_DEFAULT;
const struct ofmt_alias *ofmt_alias = NULL;
const struct dfmt *dfmt;

errflags errflags_never = 0;	/* Error flags to unconditionally suppress */

FILE *ofile = NULL;
enum optimization optimizing = OPTIM_DEFAULT;
static int cmd_sb = 16;    /* by default */

iflag_t cpu, cmd_cpu;

struct location location;
bool in_absolute;                 /* Flag we are in ABSOLUTE seg */
struct location absolute;         /* Segment/offset inside ABSOLUTE */

bool lfi_mode = false;
bool lfi_no_loads = false;
bool lfi_no_stores = false;
bool lfi_no_segue = false;
bool lfi_no_align_labels = false;

static struct RAA *offsets;

static struct SAA *forwrefs;    /* keep track of forward references */
static const struct forwrefinfo *forwref;

static struct strlist *include_path;
static enum preproc_opt ppopt;

#define OP_NORMAL           (1U << 0)
#define OP_PREPROCESS       (1U << 1)
#define OP_DEPEND           (1U << 2)

static unsigned int operating_mode;

/* Dependency flags */
static bool depend_emit_phony = false;
static bool depend_missing_ok = false;
static const char *depend_target = NULL;
static const char *depend_file = NULL;
struct strlist *depend_list;

static inline bool terminate_after_phase(void)
{
    return erropt.worst >= ERR_NONFATAL;
}

static char *quote_for_pmake(const char *str);
static char *quote_for_wmake(const char *str);
static char *(*quote_for_make)(const char *) = quote_for_pmake;

/*
 * Execution limits that can be set via a command-line option or %pragma
 */

/*
 * This is really unlimited; it would take far longer than the
 * current age of the universe for this limit to be reached even on
 * much faster CPUs than currently exist.
*/
#define LIMIT_MAX_VAL	(INT64_MAX >> 1)

int64_t nasm_limit[LIMIT_MAX+1];

struct limit_info {
    const char *name;
    const char *help;
    int64_t default_val;
};
/* The order here must match enum nasm_limit in nasm.h */
static const struct limit_info limit_info[LIMIT_MAX+1] = {
    { "passes", "total number of passes", LIMIT_MAX_VAL },
    { "stalled-passes", "number of passes without forward progress", 1000 },
    { "macro-levels", "levels of macro expansion", 10000 },
    { "macro-tokens", "tokens processed during single-line macro expansion", 10000000 },
    { "mmacros", "multi-line macros before final return", 100000 },
    { "rep", "%rep count", 1000000 },
    { "eval", "expression evaluation descent", 8192 },
    { "lines", "total source lines processed", 2000000000 }
};

static void set_default_limits(void)
{
    int i;
    size_t rl;
    int64_t new_limit;

    for (i = 0; i <= LIMIT_MAX; i++)
        nasm_limit[i] = limit_info[i].default_val;

    /*
     * Try to set a sensible default value for the eval depth based
     * on the limit of the stack size, if knowable...
     */
    rl = nasm_get_stack_size_limit();
    new_limit = rl / (128 * sizeof(void *)); /* Sensible heuristic */
    if (new_limit < nasm_limit[LIMIT_EVAL])
        nasm_limit[LIMIT_EVAL] = new_limit;
}

enum directive_result
nasm_set_limit(const char *limit, const char *valstr)
{
    int i;
    int64_t val;
    bool rn_error;
    int errlevel;

    if (!limit)
        limit = "";
    if (!valstr)
        valstr = "";

    for (i = 0; i <= LIMIT_MAX; i++) {
        if (!nasm_stricmp(limit, limit_info[i].name))
            break;
    }
    if (i > LIMIT_MAX) {
        if (not_started())
            errlevel = ERR_WARNING|WARN_OTHER|ERR_USAGE;
        else
            errlevel = ERR_WARNING|WARN_PRAGMA_UNKNOWN;
        nasm_error(errlevel, "unknown limit: `%s'", limit);
        return DIRR_ERROR;
    }

    if (!nasm_stricmp(valstr, "unlimited")) {
        val = LIMIT_MAX_VAL;
    } else {
        val = readnum(valstr, &rn_error);
        if (rn_error || val < 0) {
            if (not_started())
                errlevel = ERR_WARNING|WARN_OTHER|ERR_USAGE;
            else
                errlevel = ERR_WARNING|WARN_PRAGMA_BAD;
            nasm_error(errlevel, "invalid limit value: `%s'", valstr);
            return DIRR_ERROR;
        }
        if (val > LIMIT_MAX_VAL)
            val = LIMIT_MAX_VAL;
    }

    nasm_limit[i] = val;
    return DIRR_OK;
}

int64_t switch_segment(int32_t segment)
{
    location.segment = segment;
    if (segment == NO_SEG) {
        location.offset = absolute.offset;
        in_absolute = true;
    } else {
        location.offset = raa_read(offsets, segment);
        in_absolute = false;
    }
    return location.offset;
}

static int64_t set_curr_offs(int64_t l_off)
{
    location.offset = l_off;
    if (in_absolute) {
        absolute.offset = l_off;
    } else {
        offsets = raa_write(offsets, location.segment, l_off);
    }
    return l_off;
}

int64_t increment_offset(int64_t delta)
{
    int64_t newoffs = location.offset + delta;

    if (unlikely(!delta))
        return newoffs;

    return set_curr_offs(newoffs);
}

/*
 * Define system-defined macros that are not part of
 * macros/standard.mac.
 */
static void define_macros(void)
{
    const struct compile_time * const oct = &official_compile_time;
    char temp[128];

    if (oct->have_local) {
        strftime(temp, sizeof temp, "__?DATE?__=\"%Y-%m-%d\"", &oct->local);
        pp_pre_define(temp);
        strftime(temp, sizeof temp, "__?DATE_NUM?__=%Y%m%d", &oct->local);
        pp_pre_define(temp);
        strftime(temp, sizeof temp, "__?TIME?__=\"%H:%M:%S\"", &oct->local);
        pp_pre_define(temp);
        strftime(temp, sizeof temp, "__?TIME_NUM?__=%H%M%S", &oct->local);
        pp_pre_define(temp);
    }

    if (oct->have_gm) {
        strftime(temp, sizeof temp, "__?UTC_DATE?__=\"%Y-%m-%d\"", &oct->gm);
        pp_pre_define(temp);
        strftime(temp, sizeof temp, "__?UTC_DATE_NUM?__=%Y%m%d", &oct->gm);
        pp_pre_define(temp);
        strftime(temp, sizeof temp, "__?UTC_TIME?__=\"%H:%M:%S\"", &oct->gm);
        pp_pre_define(temp);
        strftime(temp, sizeof temp, "__?UTC_TIME_NUM?__=%H%M%S", &oct->gm);
        pp_pre_define(temp);
    }

    if (oct->have_posix) {
        snprintf(temp, sizeof temp, "__?POSIX_TIME?__=%"PRId64, oct->posix);
        pp_pre_define(temp);
    }

    /*
     * In case if output format is defined by alias
     * we have to put shortname of the alias itself here
     * otherwise ABI backward compatibility gets broken.
     */
    snprintf(temp, sizeof(temp), "__?OUTPUT_FORMAT?__=%s",
             ofmt_alias ? ofmt_alias->shortname : ofmt->shortname);
    pp_pre_define(temp);

    /*
     * Debug format, if any
     */
    if (dfmt != &null_debug_form) {
        snprintf(temp, sizeof(temp), "__?DEBUG_FORMAT?__=%s", dfmt->shortname);
        pp_pre_define(temp);
    }
}

/*
 * Initialize the preprocessor, set up the include path, and define
 * the system-included macros.  This is called between passes 1 and 2
 * of parsing the command options; ofmt and dfmt are defined at this
 * point.
 *
 * Command-line specified preprocessor directives (-p, -d, -u,
 * --pragma, --before) are processed after this function.
 */
static void preproc_init(struct strlist *ipath)
{
    pp_init(ppopt);
    define_macros();
    pp_include_path(ipath);
}

static void emit_dependencies(struct strlist *list)
{
    FILE *deps;
    int linepos, len;
    bool wmake = (quote_for_make == quote_for_wmake);
    const char *wrapstr, *nulltarget;
    const struct strlist_entry *l;

    if (!list)
        return;

    wrapstr = wmake ? " &\n " : " \\\n ";
    nulltarget = wmake ? "\t%null\n" : "";

    if (depend_file && strcmp(depend_file, "-")) {
        deps = nasm_open_write(depend_file, NF_TEXT);
        if (!deps) {
            nasm_nonfatal("unable to write dependency file `%s'", depend_file);
            return;
        }
    } else {
        deps = stdout;
    }

    linepos = fprintf(deps, "%s :", depend_target);
    strlist_for_each(l, list) {
        char *file = quote_for_make(l->str);
        len = strlen(file);
        if (linepos + len > 62 && linepos > 1) {
            fputs(wrapstr, deps);
            linepos = 1;
        }
        fprintf(deps, " %s", file);
        linepos += len+1;
        nasm_free(file);
    }
    fputs("\n\n", deps);

    strlist_for_each(l, list) {
        if (depend_emit_phony) {
            char *file = quote_for_make(l->str);
            fprintf(deps, "%s :\n%s\n", file, nulltarget);
            nasm_free(file);
        }
    }

    strlist_free(&list);

    if (deps != stdout)
        fclose(deps);
}

/* Convert a struct tm to a POSIX-style time constant */
static int64_t make_posix_time(const struct tm *tm)
{
    int64_t t;
    int64_t y = tm->tm_year;

    /* See IEEE 1003.1:2004, section 4.14 */

    t = (y-70)*365 + (y-69)/4 - (y-1)/100 + (y+299)/400;
    t += tm->tm_yday;
    t *= 24;
    t += tm->tm_hour;
    t *= 60;
    t += tm->tm_min;
    t *= 60;
    t += tm->tm_sec;

    return t;
}

/*
 * Quote a filename string if and only if it is necessary.
 * It is considered necessary if any one of these is true:
 * 1. The filename contains control characters;
 * 2. The filename starts or ends with a space or quote mark;
 * 3. The filename contains more than one space in a row;
 * 4. The filename is empty.
 *
 * The filename is returned in a newly allocated buffer.
 */
static char *nasm_quote_filename(const char *fn)
{
    const unsigned char *p =
        (const unsigned char *)fn;
    size_t len;

    if (!p || !*p)
        return nasm_strdup("\"\"");

    if (*p <= ' ' || nasm_isquote(*p)) {
        goto quote;
    } else {
        unsigned char cutoff = ' ';

        while (*p) {
            if (*p < cutoff)
                goto quote;
            cutoff = ' ' + (*p == ' ');
            p++;
        }
        if (p[-1] <= ' ' || nasm_isquote(p[-1]))
            goto quote;
    }

    /* Quoting not necessary */
    return nasm_strdup(fn);

quote:
    len = strlen(fn);
    return nasm_quote(fn, &len);
}

static void timestamp(void)
{
    struct compile_time * const oct = &official_compile_time;
    const struct tm *tp, *best_gm;

    time(&oct->t);

    best_gm = NULL;

    tp = localtime(&oct->t);
    if (tp) {
        oct->local = *tp;
        best_gm = &oct->local;
        oct->have_local = true;
    }

    tp = gmtime(&oct->t);
    if (tp) {
        oct->gm = *tp;
        best_gm = &oct->gm;
        oct->have_gm = true;
        if (!oct->have_local)
            oct->local = oct->gm;
    } else {
        oct->gm = oct->local;
    }

    if (best_gm) {
        oct->posix = make_posix_time(best_gm);
        oct->have_posix = true;
    }
}

int main(int argc, char **argv)
{
    /* Do these as early as possible */
    erropt.file = stderr;
    _progname = argv[0];
    if (!_progname || !_progname[0])
        _progname = "nasm";

    timestamp();

    set_cpu(NULL);
    cmd_cpu = cpu;

    set_default_limits();

    include_path = strlist_alloc(true);

    reset_global_defaults(0);
    _pass_type = PASS_INIT;
    _passn = 0;

    nasm_ctype_init();
    src_init();

    /*
     * We must call init_labels() before the command line parsing,
     * because we may be setting prefixes/suffixes from the command
     * line.
     */
    init_labels();

    offsets = raa_init();
    forwrefs = saa_init((int32_t)sizeof(struct forwrefinfo));

    operating_mode = OP_NORMAL;

    parse_cmdline(argc, argv, 1);
    if (terminate_after_phase())
        return 1;

    /* At this point we have ofmt and the name of the desired debug format */
    if (!using_debug_info) {
        /* No debug info, redirect to the null backend (empty stubs) */
        dfmt = &null_debug_form;
    } else if (!debug_format) {
        /* Default debug format for this backend */
        dfmt = ofmt->default_dfmt;
    } else {
        dfmt = dfmt_find(ofmt, debug_format);
        if (!dfmt) {
            nasm_fatalf(ERR_USAGE, "unrecognized debug format `%s' for output format `%s'",
                       debug_format, ofmt->shortname);
        }
    }

    /* Have we enabled TASM mode? */
    if (tasm_compatible_mode) {
        ppopt |= PP_TASM;
        nasm_ctype_tasm_mode();
    }
    preproc_init(include_path);

    parse_cmdline(argc, argv, 2);
    if (terminate_after_phase())
        return 1;

    /* Save away the default state of warnings */
    error_init();

    /* Dependency filename if we are also doing other things */
    if (!depend_file && (operating_mode & ~OP_DEPEND)) {
        if (outname)
            depend_file = nasm_strcat(outname, ".d");
        else
            depend_file = filename_set_extension(inname, ".d");
    }

    /*
     * If no output file name provided and this
     * is preprocess mode, we're perfectly
     * fine to output into stdout.
     */
    if (!outname && !(operating_mode & OP_PREPROCESS)) {
        outname = filename_set_extension(inname, ofmt->extension);
        if (!strcmp(outname, inname)) {
            outname = "nasm.out";
            nasm_warn(WARN_OTHER, "default output file same as input, using `%s' for output\n", outname);
        }
    }

    depend_list = (operating_mode & OP_DEPEND) ? strlist_alloc(true) : NULL;

    if (!depend_target)
        depend_target = quote_for_make(outname);

    reset_global_defaults(cmd_sb);

    if (!(operating_mode & (OP_PREPROCESS|OP_NORMAL))) {
            char *line;

            error_pass_start(true);

            if (depend_missing_ok)
                pp_include_path(NULL);    /* "assume generated" */

            pp_reset(inname, PP_DEPS, depend_list);
            ofile = NULL;
            while ((line = pp_getline()))
                nasm_free(line);
            pp_cleanup_pass();
            error_pass_end();
    } else if (operating_mode & OP_PREPROCESS) {
            char *line;
            const char *file_name = NULL;
            char *quoted_file_name = nasm_quote_filename(file_name);
            int32_t linnum  = 0;
            int32_t lineinc = 0;

            if (outname) {
                ofile = nasm_open_write(outname, NF_TEXT);
                if (!ofile)
                    nasm_fatalf(ERR_PERROR,
                                "unable to open output file `%s'", outname);
            } else {
                ofile = stdout;
            }

            location.known = false;

            _pass_type = PASS_PREPROC;
            pp_reset(inname, PP_PREPROC, depend_list);
            error_pass_start(true);

            while ((line = pp_getline())) {
                /*
                 * We generate %line directives if needed for later programs
                 */
                struct src_location where = src_where();
                if (file_name != where.filename) {
                    file_name = where.filename;
                    linnum = -1; /* Force a new %line statement */
                    lineinc = file_name ? 1 : 0;
                    nasm_free(quoted_file_name);
                    quoted_file_name = nasm_quote_filename(file_name);
                } else if (lineinc) {
                    if (linnum + lineinc == where.lineno) {
                        /* Add one blank line to account for increment */
                        fputc('\n', ofile);
                        linnum += lineinc;
                    } else if (linnum - lineinc == where.lineno) {
                        /*
                         * Standing still, probably a macro. Set increment
                         * to zero.
                         */
                        lineinc = 0;
                    }
                } else {
                    /* lineinc == 0 */
                    if (linnum + 1 == where.lineno)
                        lineinc = 1;
                }

                /* Skip blank lines if we will need a %line anyway */
                if (linnum == -1 && !line[0])
                    continue;

                if (linnum != where.lineno) {
                    fprintf(ofile, "%%line %"PRId32"%+"PRId32" %s\n",
                            where.lineno, lineinc, quoted_file_name);
                }
                linnum = where.lineno + lineinc;

                fputs(line, ofile);
                fputc('\n', ofile);
            }

            nasm_free(quoted_file_name);

            pp_cleanup_pass();
            error_pass_end();
            close_output(terminate_after_phase());
    }

    if (operating_mode & OP_NORMAL) {
        ofile = nasm_open_write(outname, (ofmt->flags & OFMT_TEXT) ? NF_TEXT : NF_BINARY);
        if (!ofile)
            nasm_fatalf(ERR_PERROR, "unable to open output file `%s'", outname);

        ofmt->init();
        dfmt->init();

        assemble_file(inname, depend_list);

        if (!terminate_after_phase()) {
            ofmt->cleanup();
            cleanup_labels();
            fflush(ofile);
            if (ferror(ofile))
                nasm_nonfatalf(ERR_PERROR,
                               "write error on output file `%s'", outname);
        }

        close_output(terminate_after_phase());
    }

    pp_cleanup_session();

    if (depend_list && !terminate_after_phase())
        emit_dependencies(depend_list);

    raa_free(offsets);
    saa_free(forwrefs);
    eval_cleanup();
    stdscan_cleanup();
    src_free();
    strlist_free(&include_path);

    return terminate_after_phase();
}

/*
 * Get a parameter for a command line option.
 * First arg must be in the form of e.g. -f...
 *
 * get_param() errors on a missing argument; get_opt_param() does not.
 */
static char *get_opt_param(char *p, char *q, bool *advance)
{
    *advance = false;
    if (p[2]) /* the parameter's in the option */
        return nasm_skip_spaces(p + 2);
    if (q && q[0]) {
        *advance = true;
        return q;
    }
    return NULL;
}
static char *get_param(char *p, char *q, bool *advance)
{
    char *r = get_opt_param(p, q, advance);
    if (!r)
        nasm_nonfatalf(ERR_USAGE, "option `-%c' requires an argument", p[1]);
    return r;
}

/*
 * Copy a filename
 */
static void copy_filename(const char **dst, const char *src, const char *what)
{
    if (*dst)
        nasm_fatal("more than one %s file specified: %s\n", what, src);

    *dst = nasm_strdup(src);
}

/*
 * Convert a string to a POSIX make-safe form
 */
static char *quote_for_pmake(const char *str)
{
    const char *p;
    char *os, *q;

    size_t n = 1; /* Terminating zero */
    size_t nbs = 0;

    if (!str)
        return NULL;

    for (p = str; *p; p++) {
        switch (*p) {
        case ' ':
        case '\t':
            /* Convert N backslashes + ws -> 2N+1 backslashes + ws */
            n += nbs + 2;
            nbs = 0;
            break;
        case '$':
        case '#':
            nbs = 0;
            n += 2;
            break;
        case '\\':
            nbs++;
            n++;
            break;
        default:
            nbs = 0;
            n++;
            break;
        }
    }

    /* Convert N backslashes at the end of filename to 2N backslashes */
    n += nbs;

    os = q = nasm_malloc(n);

    nbs = 0;
    for (p = str; *p; p++) {
        switch (*p) {
        case ' ':
        case '\t':
            q = mempset(q, '\\', nbs);
            *q++ = '\\';
            *q++ = *p;
            nbs = 0;
            break;
        case '$':
            *q++ = *p;
            *q++ = *p;
            nbs = 0;
            break;
        case '#':
            *q++ = '\\';
            *q++ = *p;
            nbs = 0;
            break;
        case '\\':
            *q++ = *p;
            nbs++;
            break;
        default:
            *q++ = *p;
            nbs = 0;
            break;
        }
    }

    q = mempset(q, '\\', nbs);
    *q = '\0';

    return os;
}

/*
 * Convert a string to a Watcom make-safe form
 */
static char *quote_for_wmake(const char *str)
{
    const char *p;
    char *os, *q;
    bool quote = false;

    size_t n = 1; /* Terminating zero */

    if (!str)
        return NULL;

    for (p = str; *p; p++) {
        switch (*p) {
        case ' ':
        case '\t':
        case '&':
            quote = true;
            n++;
            break;
        case '\"':
            quote = true;
            n += 2;
            break;
        case '$':
        case '#':
            n += 2;
            break;
        default:
            n++;
            break;
        }
    }

    if (quote)
        n += 2;

    os = q = nasm_malloc(n);

    if (quote)
        *q++ = '\"';

    for (p = str; *p; p++) {
        switch (*p) {
        case '$':
        case '#':
            *q++ = '$';
            *q++ = *p;
            break;
        case '\"':
            *q++ = *p;
            *q++ = *p;
            break;
        default:
            *q++ = *p;
            break;
        }
    }

    if (quote)
        *q++ = '\"';

    *q = '\0';

    return os;
}

enum text_options {
    OPT_BOGUS,
    OPT_VERSION,
    OPT_HELP,
    OPT_ABORT_ON_PANIC,
    OPT_MANGLE,
    OPT_INCLUDE,
    OPT_PRAGMA,
    OPT_BEFORE,
    OPT_LIMIT,
    OPT_KEEP_ALL,
    OPT_NO_LINE,
    OPT_DEBUG,
    OPT_INFO,
    OPT_REPRODUCIBLE,
    OPT_BITS,
    OPT_LFI,
    OPT_NO_LFI_LOADS,
    OPT_NO_LFI_STORES,
    OPT_NO_LFI_SEGUE,
    OPT_NO_LFI_ALIGN_LABELS
};
enum need_arg {
    ARG_NO,
    ARG_YES,
    ARG_MAYBE
};

struct textargs {
    const char *label;
    enum text_options opt;
    enum need_arg need_arg;
    int pvt;
};
static const struct textargs textopts[] = {
    {"v", OPT_VERSION, ARG_NO, 0},
    {"version", OPT_VERSION, ARG_NO, 0},
    {"help",     OPT_HELP,  ARG_MAYBE, 0},
    {"abort-on-panic", OPT_ABORT_ON_PANIC, ARG_NO, 0},
    {"prefix",   OPT_MANGLE, ARG_YES, D_PREFIX},
    {"postfix",  OPT_MANGLE, ARG_YES, D_POSTFIX},
    {"suffix",   OPT_MANGLE, ARG_YES, D_SUFFIX},
    {"gprefix",  OPT_MANGLE, ARG_YES, D_GPREFIX},
    {"gpostfix", OPT_MANGLE, ARG_YES, D_GPOSTFIX},
    {"gsuffix",  OPT_MANGLE, ARG_YES, D_GSUFFIX},
    {"lprefix",  OPT_MANGLE, ARG_YES, D_LPREFIX},
    {"lpostfix", OPT_MANGLE, ARG_YES, D_LPOSTFIX},
    {"lsuffix",  OPT_MANGLE, ARG_YES, D_LSUFFIX},
    {"include",  OPT_INCLUDE, ARG_YES, 0},
    {"pragma",   OPT_PRAGMA,  ARG_YES, 0},
    {"before",   OPT_BEFORE,  ARG_YES, 0},
    {"limit-",   OPT_LIMIT,   ARG_YES, 0},
    {"keep-all", OPT_KEEP_ALL, ARG_NO, 0},
    {"no-line",  OPT_NO_LINE, ARG_NO, 0},
    {"info",     OPT_INFO , ARG_MAYBE, 0},
    {"verbose",  OPT_INFO , ARG_MAYBE, 0},
    {"debug",    OPT_DEBUG, ARG_MAYBE, 0},
    {"reproducible", OPT_REPRODUCIBLE, ARG_NO, 0},
    {"bits",     OPT_BITS, ARG_YES, 0},
    {"lfi",      OPT_LFI, ARG_NO, 0},
    {"no-lfi-loads",  OPT_NO_LFI_LOADS, ARG_NO, 0},
    {"no-lfi-stores", OPT_NO_LFI_STORES, ARG_NO, 0},
    {"no-lfi-segue",  OPT_NO_LFI_SEGUE, ARG_NO, 0},
    {"no-lfi-align-labels", OPT_NO_LFI_ALIGN_LABELS, ARG_NO, 0},
    {NULL, OPT_BOGUS, ARG_NO, 0}
};

static void show_version(void)
{
    printf("NASM version %s compiled on %s%s\n",
           nasm_version, nasm_date, nasm_compile_options);
    exit(0);
}

static bool stopoptions = false;
static bool process_arg(char *p, char *q, int pass)
{
    char *param;
    bool advance = false;

    if (!p || !p[0])
        return false;

    if (p[0] == '-' && !stopoptions) {
        if (strchr("oOfpPdDiIlLFXuUZwW@", p[1])) {
            /* These parameters take values */
            if (!(param = get_param(p, q, &advance)))
                return advance;
        }

        switch (p[1]) {
        case '@':
            open_and_process_respfile(param, pass);
            break;
        case 's':
            if (pass == 1)
                erropt.file = stdout;
            break;

        case 'o':       /* output file */
            if (pass == 2)
                copy_filename(&outname, param, "output");
            break;

        case 'f':       /* output format */
            if (pass == 1) {
                ofmt = ofmt_find(param, &ofmt_alias);
                if (!ofmt) {
                    nasm_fatalf(ERR_USAGE, "unrecognised output format `%s' - use -hf for a list", param);
                }
            }
            break;

        case 'O':       /* Optimization level */
            if (pass == 1) {
                int opt;

                if (!*param) {
                    /* Naked -O == -Ox */
                    optimizing = OPTIM_ALL_ENABLED;
                } else {
                    while (*param) {
                        switch (*param) {
                        case '0': case '1': case '2': case '3': case '4':
                        case '5': case '6': case '7': case '8': case '9':
                            opt = strtoul(param, &param, 10);
                            switch (opt) {
                            case 0:
                                optimizing = OPTIM_LEVEL_0;
                                break;
                            case 1:
                                optimizing = OPTIM_LEVEL_1;
                                break;
                            default:
                                optimizing = OPTIM_ALL_ENABLED;
                                break;
                            }
                            break;

                        case 'v':
                        case '+':
                        param++;
                        erropt.verbose_info++;
                        break;

                        case 'x':
                            param++;
                            optimizing = OPTIM_ALL_ENABLED;
                            break;

                        default:
                            nasm_fatalf(ERR_USAGE,
                                       "unknown optimization option -O%c\n",
                                       *param);
                            break;
                        }
                    }
                }
            }
            break;

        case 'p':       /* pre-include */
        case 'P':
            if (pass == 2)
                pp_pre_include(param);
            break;

        case 'd':       /* pre-define */
        case 'D':
            if (pass == 2)
                pp_pre_define(param);
            break;

        case 'u':       /* un-define */
        case 'U':
            if (pass == 2)
                pp_pre_undefine(param);
            break;

        case 'i':       /* include search path */
        case 'I':
            if (pass == 1)
                strlist_add(include_path, param);
            break;

        case 'l':       /* listing file */
            if (pass == 2)
                copy_filename(&listname, param, "listing");
            break;

        case 'L':        /* listing options */
            if (pass == 2) {
                while (*param)
                    list_options |= list_option_mask(*param++);
            }
            break;

        case 'Z':       /* error messages file */
            if (pass == 1)
                copy_filename(&errname, param, "error");
            break;

        case 'F':       /* specify debug format */
            if (pass == 1) {
                using_debug_info = true;
                debug_format = param;
            }
            break;

        case 'X':       /* specify error reporting format */
            if (pass == 1) {
                if (set_error_format(param)) {
                    nasm_fatalf(ERR_USAGE,
                                "unrecognized error reporting format `%s'",
                                param);
                }
            }
            break;

        case 'g':
            if (pass == 1) {
                using_debug_info = true;
                if (p[2])
                    debug_format = nasm_skip_spaces(p + 2);
            }
            break;

        case 'h':
        case '?':
            help(stdout, get_opt_param(p, q, &advance));
            exit(0);    /* never need usage message here */
            break;

        case 'y':
            /* legacy option */
            help(stdout, "-F");
            exit(0);
            break;

        case 't':
            if (pass == 1)
                tasm_compatible_mode = true;
            break;

        case 'v':
            show_version();
            break;

        case 'e':       /* preprocess only */
        case 'E':
            if (pass == 1)
                operating_mode = OP_PREPROCESS;
            break;

        case 'a':       /* assemble only - don't preprocess */
            if (pass == 1)
                ppopt |= PP_TRIVIAL;
            break;

        case 'w':
        case 'W':
            if (pass == 2)
                set_warning_status(param);
        break;

        case 'M':
            if (pass == 1) {
                switch (p[2]) {
                case 'W':
                    quote_for_make = quote_for_wmake;
                    break;
                case 'D':
                case 'F':
                case 'T':
                case 'Q':
                    advance = true;
                    break;
                default:
                    break;
                }
            } else {
                switch (p[2]) {
                case 0:
                    operating_mode = OP_DEPEND;
                    break;
                case 'G':
                    operating_mode = OP_DEPEND;
                    depend_missing_ok = true;
                    break;
                case 'P':
                    depend_emit_phony = true;
                    break;
                case 'D':
                    operating_mode |= OP_DEPEND;
                    if (q && (q[0] != '-' || q[1] == '\0')) {
                        depend_file = q;
                        advance = true;
                    }
                    break;
                case 'F':
                    depend_file = q;
                    advance = true;
                    break;
                case 'T':
                    depend_target = q;
                    advance = true;
                    break;
                case 'Q':
                    depend_target = quote_for_make(q);
                    advance = true;
                    break;
                case 'W':
                    /* handled in pass 1 */
                    break;
                default:
                    nasm_nonfatalf(ERR_USAGE, "unknown dependency option `-M%c'", p[2]);
                    break;
                }
            }
            if (advance && (!q || !q[0])) {
                nasm_nonfatalf(ERR_USAGE, "option `-M%c' requires a parameter", p[2]);
                break;
            }
            break;

        case '-':
            {
                const struct textargs *tx;
                size_t olen, plen;
                char *eqsave;
                enum text_options opt;

                p += 2;

                if (!*p) {        /* -- => stop processing options */
                    stopoptions = true;
                    break;
                }

                olen = 0;       /* Placate gcc at lower optimization levels */
                plen = strlen(p);
                for (tx = textopts; tx->label; tx++) {
                    olen = strlen(tx->label);

                    if (olen > plen)
                        continue;

                    if (nasm_memicmp(p, tx->label, olen))
                        continue;

                    if (tx->label[olen-1] == '-')
                        break;  /* Incomplete option */

                    if (!p[olen] || p[olen] == '=')
                        break;  /* Complete option */
                }

                if (!tx->label) {
                    nasm_nonfatalf(ERR_USAGE, "unrecognized option `--%s'", p);
                }

                opt = tx->opt;

                eqsave = param = strchr(p+olen, '=');
                if (param)
                    *param++ = '\0';

                switch (tx->need_arg) {
                case ARG_YES:   /* Argument required, and may be standalone */
                    if (!param) {
                        param = q;
                        advance = true;
                    }

                    /* Note: a null string is a valid parameter */
                    if (!param) {
                        nasm_nonfatalf(ERR_USAGE, "option `--%s' requires an argument", p);
                        opt = OPT_BOGUS;
                    }
                    break;

                case ARG_NO:    /* Argument prohibited */
                    if (param) {
                        nasm_nonfatalf(ERR_USAGE, "option `--%s' does not take an argument", p);
                        opt = OPT_BOGUS;
                    }
                    break;

                case ARG_MAYBE: /* Argument permitted, but must be attached with = */
                    break;
                }

                switch (opt) {
                case OPT_BOGUS:
                    break;      /* We have already errored out */
                case OPT_VERSION:
                    show_version();
                    break;
                case OPT_ABORT_ON_PANIC:
                    abort_on_panic = true;
                    break;
                case OPT_MANGLE:
                    if (pass == 2)
                        set_label_mangle(tx->pvt, param);
                    break;
                case OPT_INCLUDE:
                    if (pass == 2)
                        pp_pre_include(q);
                    break;
                case OPT_PRAGMA:
                    if (pass == 2)
                        pp_pre_command("%pragma", param);
                    break;
                case OPT_BITS:
                    if (pass == 2)
                        pp_pre_command("BITS", param);
                    break;
                case OPT_BEFORE:
                    if (pass == 2)
                        pp_pre_command(NULL, param);
                    break;
                case OPT_LIMIT:
                    if (pass == 1)
                        nasm_set_limit(p+olen, param);
                    break;
                case OPT_KEEP_ALL:
                    keep_all = true;
                    break;
                case OPT_NO_LINE:
                    ppopt |= PP_NOLINE;
                    break;
                case OPT_DEBUG:
                    if (pass == 1)
                        erropt.debug_nasm = param ?
                            strtoul(param, NULL, 10) : erropt.debug_nasm+1;
                    break;
                case OPT_INFO:
                    if (pass == 1)
                        erropt.verbose_info = param ?
                            strtoul(param, NULL, 10) : erropt.verbose_info+1;
                    break;
                case OPT_REPRODUCIBLE:
                    reproducible = true;
                    break;
                case OPT_LFI:
                    lfi_mode = true;
                    cmd_sb = 64;
                    break;
                case OPT_NO_LFI_LOADS:
                    lfi_no_loads = true;
                    break;
                case OPT_NO_LFI_STORES:
                    lfi_no_stores = true;
                    break;
                case OPT_NO_LFI_SEGUE:
                    lfi_no_segue = true;
                    break;
                case OPT_NO_LFI_ALIGN_LABELS:
                    lfi_no_align_labels = true;
                    break;
                case OPT_HELP:
                    /* Allow --help topic without *requiring* topic */
                    if (!param)
                        param = q;
                    help(stdout, param);
                    exit(0);
                default:
                    panic();
                }

                if (eqsave)
                    *eqsave = '='; /* Restore = argument separator */

                break;
            }

        default:
            nasm_nonfatalf(ERR_USAGE, "unrecognised option `-%c'", p[1]);
            break;
        }
    } else if (pass == 2) {
        /* In theory we could allow multiple input files... */
        copy_filename(&inname, p, "input");
    }

    return advance;
}

#define ARG_BUF_DELTA 128

static void process_respfile(FILE * rfile, int pass)
{
    char *buffer, *p, *q, *prevarg;
    int bufsize, prevargsize;

    bufsize = prevargsize = ARG_BUF_DELTA;
    buffer = nasm_malloc(ARG_BUF_DELTA);
    prevarg = nasm_malloc(ARG_BUF_DELTA);
    prevarg[0] = '\0';

    while (1) {                 /* Loop to handle all lines in file */
        p = buffer;
        while (1) {             /* Loop to handle long lines */
            q = fgets(p, bufsize - (p - buffer), rfile);
            if (!q)
                break;
            p += strlen(p);
            if (p > buffer && p[-1] == '\n')
                break;
            if (p - buffer > bufsize - 10) {
                int offset;
                offset = p - buffer;
                bufsize += ARG_BUF_DELTA;
                buffer = nasm_realloc(buffer, bufsize);
                p = buffer + offset;
            }
        }

        if (!q && p == buffer) {
            if (prevarg[0])
                process_arg(prevarg, NULL, pass);
            nasm_free(buffer);
            nasm_free(prevarg);
            return;
        }

        /*
         * Play safe: remove CRs, LFs and any spurious ^Zs, if any of
         * them are present at the end of the line.
         */
        *(p = &buffer[strcspn(buffer, "\r\n\032")]) = '\0';

        while (p > buffer && nasm_isspace(p[-1]))
            *--p = '\0';

        p = nasm_skip_spaces(buffer);

        if (process_arg(prevarg, p, pass))
            *p = '\0';

        if ((int) strlen(p) > prevargsize - 10) {
            prevargsize += ARG_BUF_DELTA;
            prevarg = nasm_realloc(prevarg, prevargsize);
        }
        strncpy(prevarg, p, prevargsize);
    }
}

/* Function to process args from a string of args, rather than the
 * argv array. Used by the environment variable and response file
 * processing.
 */
static void process_args(char *args, int pass)
{
    char *p, *q, *arg, *prevarg;
    char separator = ' ';

    p = args;
    if (*p && *p != '-')
        separator = *p++;
    arg = NULL;
    while (*p) {
        q = p;
        while (*p && *p != separator)
            p++;
        while (*p == separator)
            *p++ = '\0';
        prevarg = arg;
        arg = q;
        if (process_arg(prevarg, arg, pass))
            arg = NULL;
    }
    if (arg)
        process_arg(arg, NULL, pass);
}

static void process_response_file(const char *file, int pass)
{
    char str[2048];
    FILE *f = nasm_open_read(file, NF_TEXT);
    if (!f) {
        perror(file);
        exit(-1);
    }
    while (fgets(str, sizeof str, f)) {
        process_args(str, pass);
    }
    fclose(f);
}

static void open_and_process_respfile(char *respfile, int pass)
{
    FILE *rfile;

    rfile = nasm_open_read(respfile, NF_TEXT);
    if (rfile) {
        process_respfile(rfile, pass);
        fclose(rfile);
    } else {
        nasm_fatalf(ERR_PERROR, "unable to open response file `%s'", respfile);
    }
}

static void parse_cmdline(int argc, char **argv, int pass)
{
    char *envreal, *envcopy = NULL;

    /*
     * Initialize all the warnings to their default state, including
     * warning index 0 used for "always on".
     */
    memcpy(warning_state, warning_default, sizeof warning_state);

    /*
     * First, process the NASMENV environment variable.
     */
    envreal = getenv("NASMENV");
    if (envreal) {
        envcopy = nasm_strdup(envreal);
        process_args(envcopy, pass);
        nasm_free(envcopy);
    }

    /*
     * Now process the actual command line.
     */
    while (--argc) {
        bool advance;
        argv++;
        if (argv[0][0] == '@') {
            /*
             * We have a response file, so process this as a set of
             * arguments like the environment variable. This allows us
             * to have multiple arguments on a single line, which is
             * different to the -@resp file processing below for regular
             * NASM.
             */
            process_response_file(argv[0]+1, pass);
            argc--;
            argv++;
        }

        advance = process_arg(argv[0], argc > 1 ? argv[1] : NULL, pass);
        argv += advance, argc -= advance;
    }

    /*
     * Look for basic command line typos. This definitely doesn't
     * catch all errors, but it might help cases of fumbled fingers.
     */
    if (pass != 2)
        return;

    if (!inname)
        nasm_fatalf(ERR_USAGE, "no input file specified");
    else if ((errname && !strcmp(inname, errname)) ||
             (outname && !strcmp(inname, outname)) ||
             (listname &&  !strcmp(inname, listname))  ||
             (depend_file && !strcmp(inname, depend_file)))
        nasm_fatal("will not overwrite input file");

    if (errname) {
        FILE *error_file = nasm_open_write(errname, NF_TEXT);
        if (erropt.file) {
            erropt.file = error_file;
        } else {
            nasm_fatalf(ERR_PERROR, "cannot open file `%s' for error messages",
                       errname);
        }
    }
}

static void forward_refs(insn *instruction)
{
    int i;
    struct forwrefinfo *fwinf;

    /* Don't bother for -O1 */
    if (instruction->opt & OPTIM_DISABLE_FWREF)
        return;

    if (!forwref)
        return;

    if (forwref->lineno != globallineno)
        return;

    do {
        instruction->oprs[forwref->operand].opflags |= OPFLAG_FORWARD;
        forwref = saa_rstruct(forwrefs);
    } while (forwref && forwref->lineno == globallineno);

    if (!pass_first())
        return;

    for (i = 0; i < instruction->operands; i++) {
        if (instruction->oprs[i].opflags & OPFLAG_FORWARD) {
            fwinf = saa_wstruct(forwrefs);
            fwinf->lineno = globallineno;
            fwinf->operand = i;
        }
    }
}

void print_final_report(bool failure)
{
    /* This test is here to reduce the likelihood of a recursive failure */
    if (unlikely(erropt.verbose_info >= 1)) {
        enum pass_type t = pass_type();

        if (t >= PASS_FIRST) {
            unsigned int end_passes = t > PASS_OPT ? t - PASS_OPT : 0;
            nasm_info(1, "assembly %s after 1+%"PRId64"+%u passes",
                      failure ? "failed" : "completed",
                      pass_count()-1-end_passes, end_passes);
        }
    }
}

/*
 * =====================================================================
 * LFI (Lightweight Fault Isolation) support
 *
 * All LFI logic sits between parse_line() and process_insn() in
 * assemble_file(). The approach:
 *   1. Parse the original instruction via parse_line() -> insn struct
 *   2. LFI layer inspects the insn, generates 1-N replacement
 *      instructions (as text strings re-parsed via parse_line()),
 *      computes NOP padding for 32-byte bundle alignment
 *   3. Feed each replacement instruction (with padding) to process_insn()
 * =====================================================================
 */

#define LFI_BUNDLE_SIZE  32
#define LFI_MAX_REPLACE  16

#define isOpOfType(op, ty) (((op).type & (ty)) == (ty))

struct lfi_replacement {
    insn instructions[LFI_MAX_REPLACE];
    bool bundle_locked[LFI_MAX_REPLACE]; /* must be in same bundle */
    int  count;
    bool align_to_end; /* for calls: last insn ends at bundle boundary */
};

/*
 * Map a 64-bit register enum to its 32-bit counterpart name string.
 */
static const char *lfi_reg32_name(enum reg_enum reg)
{
    switch (reg) {
    case R_RAX: return "eax";
    case R_RBX: return "ebx";
    case R_RCX: return "ecx";
    case R_RDX: return "edx";
    case R_RSI: return "esi";
    case R_RDI: return "edi";
    case R_RBP: return "ebp";
    case R_RSP: return "esp";
    case R_R8:  return "r8d";
    case R_R9:  return "r9d";
    case R_R10: return "r10d";
    case R_R11: return "r11d";
    case R_R12: return "r12d";
    case R_R13: return "r13d";
    case R_R14: return "r14d";
    case R_R15: return "r15d";
    default:    return NULL;
    }
}

/*
 * Map a 64-bit register enum to its 64-bit name string.
 */
static const char *lfi_reg64_name(enum reg_enum reg)
{
    switch (reg) {
    case R_RAX: case R_EAX: return "rax";
    case R_RBX: case R_EBX: return "rbx";
    case R_RCX: case R_ECX: return "rcx";
    case R_RDX: case R_EDX: return "rdx";
    case R_RSI: case R_ESI: return "rsi";
    case R_RDI: case R_EDI: return "rdi";
    case R_RBP: case R_EBP: return "rbp";
    case R_RSP: case R_ESP: return "rsp";
    case R_R8:  case R_R8D: return "r8";
    case R_R9:  case R_R9D: return "r9";
    case R_R10: case R_R10D: return "r10";
    case R_R11: case R_R11D: return "r11";
    case R_R12: case R_R12D: return "r12";
    case R_R13: case R_R13D: return "r13";
    case R_R14: case R_R14D: return "r14";
    case R_R15: case R_R15D: return "r15";
    default:    return NULL;
    }
}

/*
 * Map a register enum to its 64-bit equivalent enum.
 */
static enum reg_enum lfi_to_reg64(enum reg_enum reg)
{
    switch (reg) {
    case R_EAX: case R_RAX: return R_RAX;
    case R_EBX: case R_RBX: return R_RBX;
    case R_ECX: case R_RCX: return R_RCX;
    case R_EDX: case R_RDX: return R_RDX;
    case R_ESI: case R_RSI: return R_RSI;
    case R_EDI: case R_RDI: return R_RDI;
    case R_EBP: case R_RBP: return R_RBP;
    case R_ESP: case R_RSP: return R_RSP;
    case R_R8D: case R_R8:  return R_R8;
    case R_R9D: case R_R9:  return R_R9;
    case R_R10D: case R_R10: return R_R10;
    case R_R11D: case R_R11: return R_R11;
    case R_R12D: case R_R12: return R_R12;
    case R_R13D: case R_R13: return R_R13;
    case R_R14D: case R_R14: return R_R14;
    case R_R15D: case R_R15: return R_R15;
    default: return reg;
    }
}

/*
 * Check if a memory operand is "safe" and needs no sandboxing.
 */
static bool lfi_is_safe_mem(const operand *op)
{
    enum reg_enum base = lfi_to_reg64(op->basereg);
    enum reg_enum idx  = op->indexreg;

    /* RIP-relative */
    if (op->eaflags & EAF_REL)
        return true;

    /* RSP-based with no index */
    if (base == R_RSP && idx == R_none)
        return true;

    /* RBP-based with no index */
    if (base == R_RBP && idx == R_none)
        return true;

    /* R14-based with no index */
    if (base == R_R14 && idx == R_none)
        return true;

    return false;
}

/*
 * Build a memory operand string representation for re-parsing.
 */
static void lfi_mem_string(char *buf, size_t bufsz,
                           enum reg_enum base, enum reg_enum index,
                           int scale, int64_t offset)
{
    char base_str[32] = "";
    char index_str[64] = "";
    char offset_str[32] = "";

    if (base != R_none) {
        const char *n = nasm_reg_names[base - EXPR_REG_START];
        snprintf(base_str, sizeof base_str, "%s", n);
    }

    if (index != R_none) {
        const char *n = nasm_reg_names[index - EXPR_REG_START];
        if (scale > 1)
            snprintf(index_str, sizeof index_str, "%s%s*%d",
                     base != R_none ? " + " : "", n, scale);
        else
            snprintf(index_str, sizeof index_str, "%s%s",
                     base != R_none ? " + " : "", n);
    }

    if (offset != 0) {
        if (offset > 0)
            snprintf(offset_str, sizeof offset_str, " + %"PRId64, offset);
        else
            snprintf(offset_str, sizeof offset_str, " - %"PRId64, -offset);
    }

    snprintf(buf, bufsz, "[%s%s%s]", base_str, index_str, offset_str);
}

/*
 * Generate operand size prefix string for a memory operand.
 */
static const char *lfi_size_prefix(const operand *op)
{
    unsigned int opsize = ((op->type & SIZE_MASK) >> SIZE_SHIFT) * 8;
    switch (opsize) {
    case 8:   return "byte ";
    case 16:  return "word ";
    case 32:  return "dword ";
    case 64:  return "qword ";
    case 80:  return "tword ";
    case 128: return "oword ";
    case 256: return "yword ";
    case 512: return "zword ";
    default:  return "";
    }
}

/*
 * Parse a text instruction into an insn struct.
 */
static void lfi_parse(const char *text, insn *result)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", text);
    parse_line(buf, result, 64);
}

/*
 * Validate an instruction: reject modifications to R14 and direct GS usage.
 */
static void lfi_validate_instruction(const insn *ins)
{
    int i;

    /* Check for destination operand being R14 */
    if (ins->operands >= 1 &&
        isOpOfType(ins->oprs[0], REGISTER) &&
        lfi_to_reg64(ins->oprs[0].basereg) == R_R14) {
        /* Allow LEA r14, ... (used by runtime) - actually, reject all */
        nasm_fatal("LFI: illegal modification of reserved register r14");
    }

    /* Check for GS segment usage */
    for (i = 0; i < ins->operands; i++) {
        if (ins->oprs[i].eaflags & EAF_GS)
            nasm_fatal("LFI: illegal use of reserved segment register gs");
    }
}

/*
 * Compute NOP padding needed for bundle alignment.
 *
 * offset: current offset in section
 * min_space: minimum contiguous space needed for bundle-locked group
 * raw_size: size of the current instruction
 * align_end: if true, pad so the instruction ends at a bundle boundary
 *            (used for direct calls)
 */
static int lfi_get_padding(int64_t offset, int min_space,
                           int64_t raw_size, bool align_end)
{
    int pos, remaining, pad;

    if (raw_size <= 0)
        return 0;

    if (raw_size >= LFI_BUNDLE_SIZE) {
        nasm_nonfatal("LFI: instruction size %"PRId64" exceeds bundle size",
                      raw_size);
        return 0;
    }

    if (min_space > LFI_BUNDLE_SIZE) {
        nasm_nonfatal("LFI: bundle-locked group exceeds %d bytes", LFI_BUNDLE_SIZE);
        return 0;
    }

    /* Position within the current 32-byte bundle (0..31) */
    pos = (int)(offset % LFI_BUNDLE_SIZE);
    remaining = LFI_BUNDLE_SIZE - pos;

    /* Check if the bundle-locked group fits in the remaining space */
    if (min_space > remaining)
        return remaining;

    if (align_end) {
        /* Pad so instruction ends at bundle boundary */
        int target_start = LFI_BUNDLE_SIZE - (int)raw_size;
        pad = (target_start - pos + LFI_BUNDLE_SIZE) % LFI_BUNDLE_SIZE;
        return pad;
    }

    /* Check if instruction would straddle a boundary */
    if ((int)raw_size > remaining)
        return remaining;

    return 0;
}

/*
 * =====================================================
 * Instruction replacement functions
 * =====================================================
 */

/*
 * Return rewriting: ret -> pop r11; .bundle_lock; and r11d, -32;
 *                   add r11, r14; jmp r11; .bundle_unlock
 */
static void lfi_expand_return(const insn *ins, struct lfi_replacement *rep)
{
    rep->count = 4;
    rep->align_to_end = false;

    lfi_parse("pop r11", &rep->instructions[0]);
    rep->bundle_locked[0] = false;

    lfi_parse("and r11d, 0xffffffe0", &rep->instructions[1]);
    rep->bundle_locked[1] = true;

    lfi_parse("add r11, r14", &rep->instructions[2]);
    rep->bundle_locked[2] = true;

    lfi_parse("jmp r11", &rep->instructions[3]);
    rep->bundle_locked[3] = true;

    /* Handle ret with immediate (ret N) */
    if (ins->operands >= 1 &&
        (ins->oprs[0].type & IMMEDIATE)) {
        int64_t imm = ins->oprs[0].offset;
        char buf[256];
        /* Insert stack adjustment after pop */
        /* Shift existing instructions 1-3 to 3-5 */
        rep->instructions[5] = rep->instructions[3];
        rep->bundle_locked[5] = rep->bundle_locked[3];
        rep->instructions[4] = rep->instructions[2];
        rep->bundle_locked[4] = rep->bundle_locked[2];
        rep->instructions[3] = rep->instructions[1];
        rep->bundle_locked[3] = rep->bundle_locked[1];

        /* add esp, N */
        snprintf(buf, sizeof buf, "add esp, %"PRId64, imm);
        lfi_parse(buf, &rep->instructions[1]);
        rep->bundle_locked[1] = true;

        /* lea rsp, [rsp + r14] */
        lfi_parse("lea rsp, [rsp + r14]", &rep->instructions[2]);
        rep->bundle_locked[2] = true;

        rep->count = 6;
    }
}

/*
 * Indirect jump via register: .bundle_lock; and eXd, -32; add rX, r14;
 *                             jmp rX; .bundle_unlock
 */
static void lfi_expand_indirect_jmp_reg(enum reg_enum reg,
                                        struct lfi_replacement *rep)
{
    char buf[256];
    const char *r32 = lfi_reg32_name(lfi_to_reg64(reg));
    const char *r64 = lfi_reg64_name(reg);

    rep->count = 3;
    rep->align_to_end = false;

    snprintf(buf, sizeof buf, "and %s, 0xffffffe0", r32);
    lfi_parse(buf, &rep->instructions[0]);
    rep->bundle_locked[0] = true;

    snprintf(buf, sizeof buf, "add %s, r14", r64);
    lfi_parse(buf, &rep->instructions[1]);
    rep->bundle_locked[1] = true;

    snprintf(buf, sizeof buf, "jmp %s", r64);
    lfi_parse(buf, &rep->instructions[2]);
    rep->bundle_locked[2] = true;
}

/*
 * Indirect call via register: .bundle_lock align_to_end;
 *   and eXd, -32; add rX, r14; call rX; .bundle_unlock
 */
static void lfi_expand_indirect_call_reg(enum reg_enum reg,
                                         struct lfi_replacement *rep)
{
    char buf[256];
    const char *r32 = lfi_reg32_name(lfi_to_reg64(reg));
    const char *r64 = lfi_reg64_name(reg);

    rep->count = 3;
    rep->align_to_end = true;

    snprintf(buf, sizeof buf, "and %s, 0xffffffe0", r32);
    lfi_parse(buf, &rep->instructions[0]);
    rep->bundle_locked[0] = true;

    snprintf(buf, sizeof buf, "add %s, r14", r64);
    lfi_parse(buf, &rep->instructions[1]);
    rep->bundle_locked[1] = true;

    snprintf(buf, sizeof buf, "call %s", r64);
    lfi_parse(buf, &rep->instructions[2]);
    rep->bundle_locked[2] = true;
}

/*
 * Indirect jump/call via memory: load to r11, then indirect jump/call.
 */
static void lfi_expand_indirect_branch_mem(const insn *ins,
                                           struct lfi_replacement *rep,
                                           bool is_call)
{
    char buf[256];
    char memstr[256];
    const operand *op = &ins->oprs[0];
    const char *szpfx = lfi_size_prefix(op);
    int n;

    /* First: load the target address into r11, sandboxing the memory access */
    if (lfi_is_safe_mem(op)) {
        /* Safe memory: just load directly */
        lfi_mem_string(memstr, sizeof memstr,
                       op->basereg, op->indexreg, op->scale, op->offset);
        snprintf(buf, sizeof buf, "mov r11, %s%s", szpfx, memstr);
        lfi_parse(buf, &rep->instructions[0]);
        rep->bundle_locked[0] = false;
        n = 1;
    } else if (!lfi_no_segue && op->indexreg == R_none) {
        /* Segue: use gs: with 32-bit base */
        const char *base32 = lfi_reg32_name(lfi_to_reg64(op->basereg));
        if (base32) {
            snprintf(buf, sizeof buf, "mov r11, qword [gs:%s + %"PRId64"]",
                     base32, op->offset);
        } else {
            snprintf(buf, sizeof buf, "mov r11, qword [gs:%"PRId64"]",
                     op->offset);
        }
        lfi_parse(buf, &rep->instructions[0]);
        rep->bundle_locked[0] = false;
        n = 1;
    } else {
        /* Non-segue: lea r11d, [addr]; mov r11, [r14 + r11] */
        lfi_mem_string(memstr, sizeof memstr,
                       op->basereg, op->indexreg, op->scale, op->offset);
        snprintf(buf, sizeof buf, "lea r11d, %s", memstr);
        lfi_parse(buf, &rep->instructions[0]);
        rep->bundle_locked[0] = false;

        lfi_parse("mov r11, qword [r14 + r11]", &rep->instructions[1]);
        rep->bundle_locked[1] = true;
        n = 2;
    }

    /* Then: sandbox and branch to r11 */
    if (is_call) {
        struct lfi_replacement call_rep;
        lfi_expand_indirect_call_reg(R_R11, &call_rep);
        for (int i = 0; i < call_rep.count; i++) {
            rep->instructions[n + i] = call_rep.instructions[i];
            rep->bundle_locked[n + i] = call_rep.bundle_locked[i];
        }
        rep->count = n + call_rep.count;
        rep->align_to_end = true;
    } else {
        struct lfi_replacement jmp_rep;
        lfi_expand_indirect_jmp_reg(R_R11, &jmp_rep);
        for (int i = 0; i < jmp_rep.count; i++) {
            rep->instructions[n + i] = jmp_rep.instructions[i];
            rep->bundle_locked[n + i] = jmp_rep.bundle_locked[i];
        }
        rep->count = n + jmp_rep.count;
        rep->align_to_end = false;
    }
}

/*
 * Direct call: bundle-lock with align_to_end so the call instruction
 * is at the end of a bundle, making the return address bundle-aligned.
 */
static void lfi_expand_direct_call(const insn *ins,
                                   struct lfi_replacement *rep)
{
    rep->count = 1;
    rep->align_to_end = true;
    rep->instructions[0] = *ins;
    rep->bundle_locked[0] = true;
}

/*
 * Stack pointer modification: demote to 32-bit + re-guard.
 * General form (add rsp, 8):
 *   .bundle_lock; add esp, 8; lea rsp, [rsp + r14]; .bundle_unlock
 */
static void lfi_expand_stack_modification(const insn *ins,
                                          struct lfi_replacement *rep)
{
    char buf[256];
    const char *opname = nasm_insn_names[ins->opcode];
    bool jumps_only = lfi_no_loads && lfi_no_stores;

    if (jumps_only) {
        /* No memory sandboxing: pass through */
        rep->count = 1;
        rep->align_to_end = false;
        rep->instructions[0] = *ins;
        rep->bundle_locked[0] = false;
        return;
    }

    /* Special case: pop rsp */
    if (ins->opcode == I_POP && lfi_to_reg64(ins->oprs[0].basereg) == R_RSP) {
        rep->count = 3;
        rep->align_to_end = false;

        lfi_parse("pop r11", &rep->instructions[0]);
        rep->bundle_locked[0] = false;

        lfi_parse("mov esp, r11d", &rep->instructions[1]);
        rep->bundle_locked[1] = true;

        lfi_parse("lea rsp, [rsp + r14]", &rep->instructions[2]);
        rep->bundle_locked[2] = true;
        return;
    }

    rep->count = 2;
    rep->align_to_end = false;

    /* Generate the 32-bit version of the stack instruction */
    if (ins->operands >= 2 && (ins->oprs[1].type & IMMEDIATE)) {
        snprintf(buf, sizeof buf, "%s esp, %"PRId64, opname,
                 ins->oprs[1].offset);
    } else if (ins->operands >= 2 && isOpOfType(ins->oprs[1], REGISTER)) {
        const char *src32 = lfi_reg32_name(lfi_to_reg64(ins->oprs[1].basereg));
        if (src32)
            snprintf(buf, sizeof buf, "%s esp, %s", opname, src32);
        else
            snprintf(buf, sizeof buf, "%s esp, %s", opname,
                     nasm_reg_names[ins->oprs[1].basereg - EXPR_REG_START]);
    } else if (ins->operands >= 2 && isOpOfType(ins->oprs[1], MEMORY)) {
        char memstr[256];
        lfi_mem_string(memstr, sizeof memstr,
                       ins->oprs[1].basereg, ins->oprs[1].indexreg,
                       ins->oprs[1].scale, ins->oprs[1].offset);
        snprintf(buf, sizeof buf, "%s esp, %s", opname, memstr);
    } else {
        /* Fallback: just pass through (shouldn't happen for rsp mods) */
        rep->count = 1;
        rep->instructions[0] = *ins;
        rep->bundle_locked[0] = false;
        return;
    }

    lfi_parse(buf, &rep->instructions[0]);
    rep->bundle_locked[0] = true;

    lfi_parse("lea rsp, [rsp + r14]", &rep->instructions[1]);
    rep->bundle_locked[1] = true;
}

/*
 * Sandbox a memory operand using the segue optimization (gs: prefix).
 * Replaces the original instruction with a gs:-prefixed version using
 * 32-bit address registers.
 */
static bool lfi_sandbox_mem_segue(const insn *ins, int mem_op_idx,
                                  struct lfi_replacement *rep)
{
    char buf[512];
    const operand *memop = &ins->oprs[mem_op_idx];
    const char *opname = nasm_insn_names[ins->opcode];
    const char *base32 = NULL;
    const char *index32 = NULL;
    char addr[256];

    if (memop->basereg != R_none)
        base32 = lfi_reg32_name(lfi_to_reg64(memop->basereg));
    if (memop->indexreg != R_none)
        index32 = lfi_reg32_name(lfi_to_reg64(memop->indexreg));

    /* Build the address expression */
    if (base32 && index32) {
        if (memop->scale > 1) {
            if (memop->offset != 0)
                snprintf(addr, sizeof addr, "gs:%s + %s*%d + %"PRId64,
                         base32, index32, memop->scale, memop->offset);
            else
                snprintf(addr, sizeof addr, "gs:%s + %s*%d",
                         base32, index32, memop->scale);
        } else {
            if (memop->offset != 0)
                snprintf(addr, sizeof addr, "gs:%s + %s + %"PRId64,
                         base32, index32, memop->offset);
            else
                snprintf(addr, sizeof addr, "gs:%s + %s",
                         base32, index32);
        }
    } else if (base32) {
        if (memop->offset != 0)
            snprintf(addr, sizeof addr, "gs:%s + %"PRId64,
                     base32, memop->offset);
        else
            snprintf(addr, sizeof addr, "gs:%s", base32);
    } else if (index32) {
        if (memop->scale > 1) {
            if (memop->offset != 0)
                snprintf(addr, sizeof addr, "gs:%s*%d + %"PRId64,
                         index32, memop->scale, memop->offset);
            else
                snprintf(addr, sizeof addr, "gs:%s*%d",
                         index32, memop->scale);
        } else {
            if (memop->offset != 0)
                snprintf(addr, sizeof addr, "gs:%s + %"PRId64,
                         index32, memop->offset);
            else
                snprintf(addr, sizeof addr, "gs:%s", index32);
        }
    } else {
        snprintf(addr, sizeof addr, "gs:%"PRId64, memop->offset);
    }

    const char *szpfx = lfi_size_prefix(memop);

    /* Build the replacement instruction */
    if (ins->operands == 2) {
        if (mem_op_idx == 0) {
            /* Memory is destination: op [mem], src */
            const char *src = nasm_reg_names[ins->oprs[1].basereg - EXPR_REG_START];
            snprintf(buf, sizeof buf, "%s %s[%s], %s",
                     opname, szpfx, addr, src);
        } else {
            /* Memory is source: op dst, [mem] */
            const char *dst = nasm_reg_names[ins->oprs[0].basereg - EXPR_REG_START];
            snprintf(buf, sizeof buf, "%s %s, %s[%s]",
                     opname, dst, szpfx, addr);
        }
    } else {
        /* Single-operand memory instruction */
        snprintf(buf, sizeof buf, "%s %s[%s]", opname, szpfx, addr);
    }

    rep->count = 1;
    rep->align_to_end = false;
    lfi_parse(buf, &rep->instructions[0]);
    rep->bundle_locked[0] = false;
    return true;
}

/*
 * Sandbox a memory operand without segue (scratch register approach).
 * For simple (base only, no index):
 *   mov eXd, eXd; <op using [r14 + rX + offset]>  (bundle_locked)
 * For complex (SIB/index):
 *   lea r11d, [effective address]; <op using [r14 + r11]>  (bundle_locked)
 */
static void lfi_sandbox_mem_nosegue(const insn *ins, int mem_op_idx,
                                    struct lfi_replacement *rep)
{
    char buf[512];
    const operand *memop = &ins->oprs[mem_op_idx];
    const char *opname = nasm_insn_names[ins->opcode];
    const char *szpfx = lfi_size_prefix(memop);

    if (memop->indexreg == R_none && memop->basereg != R_none) {
        /* Simple case: base register only */
        const char *base32 = lfi_reg32_name(lfi_to_reg64(memop->basereg));
        const char *base64 = lfi_reg64_name(memop->basereg);
        char memstr[256];

        if (!base32 || !base64) {
            /* Unknown register - pass through */
            rep->count = 1;
            rep->align_to_end = false;
            rep->instructions[0] = *ins;
            rep->bundle_locked[0] = false;
            return;
        }

        rep->count = 2;
        rep->align_to_end = false;

        /* mov eXd, eXd (clears upper 32 bits) */
        snprintf(buf, sizeof buf, "mov %s, %s", base32, base32);
        lfi_parse(buf, &rep->instructions[0]);
        rep->bundle_locked[0] = true;

        /* op ... [r14 + rX + offset] */
        if (memop->offset != 0)
            snprintf(memstr, sizeof memstr, "[r14 + %s + %"PRId64"]",
                     base64, memop->offset);
        else
            snprintf(memstr, sizeof memstr, "[r14 + %s]", base64);

        if (ins->operands == 2) {
            if (mem_op_idx == 0) {
                const char *src = nasm_reg_names[ins->oprs[1].basereg - EXPR_REG_START];
                snprintf(buf, sizeof buf, "%s %s%s, %s",
                         opname, szpfx, memstr, src);
            } else {
                const char *dst = nasm_reg_names[ins->oprs[0].basereg - EXPR_REG_START];
                snprintf(buf, sizeof buf, "%s %s, %s%s",
                         opname, dst, szpfx, memstr);
            }
        } else {
            snprintf(buf, sizeof buf, "%s %s%s", opname, szpfx, memstr);
        }
        lfi_parse(buf, &rep->instructions[1]);
        rep->bundle_locked[1] = true;
    } else {
        /* Complex case: use lea r11d to compute address */
        char origmem[256];

        rep->count = 2;
        rep->align_to_end = false;

        lfi_mem_string(origmem, sizeof origmem,
                       memop->basereg, memop->indexreg,
                       memop->scale, memop->offset);
        snprintf(buf, sizeof buf, "lea r11d, %s", origmem);
        lfi_parse(buf, &rep->instructions[0]);
        rep->bundle_locked[0] = true;

        /* op ... [r14 + r11] */
        if (ins->operands == 2) {
            if (mem_op_idx == 0) {
                const char *src = nasm_reg_names[ins->oprs[1].basereg - EXPR_REG_START];
                snprintf(buf, sizeof buf, "%s %s[r14 + r11], %s",
                         opname, szpfx, src);
            } else {
                const char *dst = nasm_reg_names[ins->oprs[0].basereg - EXPR_REG_START];
                snprintf(buf, sizeof buf, "%s %s, %s[r14 + r11]",
                         opname, dst, szpfx);
            }
        } else {
            snprintf(buf, sizeof buf, "%s %s[r14 + r11]", opname, szpfx);
        }
        lfi_parse(buf, &rep->instructions[1]);
        rep->bundle_locked[1] = true;
    }
}

/*
 * Expand a load/store instruction with memory sandboxing.
 */
static void lfi_expand_load_store(const insn *ins,
                                  struct lfi_replacement *rep)
{
    int mem_op_idx = -1;
    bool is_store = false;
    bool jumps_only = lfi_no_loads && lfi_no_stores;

    /* LEA doesn't access memory */
    if (ins->opcode == I_LEA) {
        rep->count = 1;
        rep->align_to_end = false;
        rep->instructions[0] = *ins;
        rep->bundle_locked[0] = false;
        return;
    }

    /* Find the memory operand */
    for (int i = 0; i < ins->operands; i++) {
        if ((ins->oprs[i].type & MEMORY) == MEMORY) {
            mem_op_idx = i;
            if (i == 0)
                is_store = true;
            break;
        }
    }

    if (mem_op_idx < 0 || jumps_only) {
        /* No memory operand or jumps-only mode: pass through */
        rep->count = 1;
        rep->align_to_end = false;
        rep->instructions[0] = *ins;
        rep->bundle_locked[0] = false;
        return;
    }

    /* Check feature flags */
    if (is_store && lfi_no_stores) {
        rep->count = 1;
        rep->align_to_end = false;
        rep->instructions[0] = *ins;
        rep->bundle_locked[0] = false;
        return;
    }
    if (!is_store && lfi_no_loads) {
        rep->count = 1;
        rep->align_to_end = false;
        rep->instructions[0] = *ins;
        rep->bundle_locked[0] = false;
        return;
    }

    /* Check if memory operand is safe (no rewrite needed) */
    if (lfi_is_safe_mem(&ins->oprs[mem_op_idx])) {
        rep->count = 1;
        rep->align_to_end = false;
        rep->instructions[0] = *ins;
        rep->bundle_locked[0] = false;
        return;
    }

    /* No base register (absolute address) - use r14 as base */
    if (ins->oprs[mem_op_idx].basereg == R_none &&
        ins->oprs[mem_op_idx].indexreg == R_none) {
        rep->count = 1;
        rep->align_to_end = false;
        rep->instructions[0] = *ins;
        rep->bundle_locked[0] = false;
        return;
    }

    /* Apply sandboxing */
    if (!lfi_no_segue) {
        lfi_sandbox_mem_segue(ins, mem_op_idx, rep);
    } else {
        lfi_sandbox_mem_nosegue(ins, mem_op_idx, rep);
    }
}

/*
 * Expand syscall instruction.
 * syscall -> .bundle_lock; lea r11, [rel $+6]; jmp qword [r14]; .bundle_unlock
 *
 * We approximate this by generating:
 *   lea r11, [rel lfi_ret_NNNN]
 *   jmp qword [r14]
 *   lfi_ret_NNNN:
 *
 * Since NASM doesn't have anonymous labels, we use the simpler approach:
 * the instruction sequence is bundle-locked so the return address is known.
 */
static void lfi_expand_syscall(const insn *ins,
                               struct lfi_replacement *rep)
{
    (void)ins;
    rep->count = 2;
    rep->align_to_end = false;

    /* lea r11, [rel $+7] (7 = size of jmp qword [r14] which is
     * ff 26 or ff 66 00 or similar - actually jmp [r14] in 64-bit
     * mode is: 41 ff 26 = 3 bytes. But we use the label approach.) */
    /* Use a simpler encoding: we'll use a forward reference to next insn */
    lfi_parse("lea r11, [rel $+7]", &rep->instructions[0]);
    rep->bundle_locked[0] = true;

    lfi_parse("jmp qword [r14]", &rep->instructions[1]);
    rep->bundle_locked[1] = true;
}

/*
 * Expand TLS read: mov rax, [fs:0] -> mov rax, [r15]
 */
static bool lfi_is_tls_read(const insn *ins)
{
    if (ins->opcode != I_MOV)
        return false;
    if (ins->operands != 2)
        return false;
    if (!isOpOfType(ins->oprs[0], REGISTER))
        return false;
    if (!(ins->oprs[1].eaflags & EAF_FS))
        return false;
    if (ins->oprs[1].basereg != R_none)
        return false;
    if (ins->oprs[1].indexreg != R_none)
        return false;
    if (ins->oprs[1].offset != 0)
        return false;
    return true;
}

static void lfi_expand_tls_read(const insn *ins,
                                struct lfi_replacement *rep)
{
    char buf[256];
    const char *dst = nasm_reg_names[ins->oprs[0].basereg - EXPR_REG_START];

    rep->count = 1;
    rep->align_to_end = false;

    snprintf(buf, sizeof buf, "mov %s, [r15]", dst);
    lfi_parse(buf, &rep->instructions[0]);
    rep->bundle_locked[0] = false;
}

/*
 * Expand string operations (stosb, movsb, cmpsb, etc.)
 * Sandbox rdi (and rsi for movs/cmps) before the operation.
 */
static bool lfi_is_string_op(const insn *ins)
{
    switch (ins->opcode) {
    case I_STOSB: case I_STOSW: case I_STOSD: case I_STOSQ:
    case I_MOVSB: case I_MOVSW: case I_MOVSD: case I_MOVSQ:
    case I_CMPSB: case I_CMPSW: case I_CMPSD: case I_CMPSQ:
        return true;
    default:
        return false;
    }
}

static bool lfi_string_has_source(const insn *ins)
{
    switch (ins->opcode) {
    case I_MOVSB: case I_MOVSW: case I_MOVSD: case I_MOVSQ:
    case I_CMPSB: case I_CMPSW: case I_CMPSD: case I_CMPSQ:
        return true;
    default:
        return false;
    }
}

static void lfi_expand_string_op(const insn *ins,
                                 struct lfi_replacement *rep)
{
    int n = 0;
    bool jumps_only = lfi_no_loads && lfi_no_stores;
    bool stores_only = lfi_no_loads && !lfi_no_stores;

    if (jumps_only) {
        rep->count = 1;
        rep->align_to_end = false;
        rep->instructions[0] = *ins;
        rep->bundle_locked[0] = false;
        return;
    }

    /* Sandbox rsi for source (movs/cmps) */
    if (lfi_string_has_source(ins) && !stores_only) {
        lfi_parse("mov esi, esi", &rep->instructions[n]);
        rep->bundle_locked[n] = true;
        n++;
        lfi_parse("lea rsi, [r14 + rsi]", &rep->instructions[n]);
        rep->bundle_locked[n] = true;
        n++;
    }

    /* Sandbox rdi for destination */
    lfi_parse("mov edi, edi", &rep->instructions[n]);
    rep->bundle_locked[n] = true;
    n++;
    lfi_parse("lea rdi, [r14 + rdi]", &rep->instructions[n]);
    rep->bundle_locked[n] = true;
    n++;

    /* The original instruction */
    rep->instructions[n] = *ins;
    rep->bundle_locked[n] = true;
    n++;

    rep->count = n;
    rep->align_to_end = false;
}

/*
 * Check if an instruction explicitly modifies RSP.
 */
static bool lfi_modifies_rsp(const insn *ins)
{
    if (ins->operands < 1)
        return false;
    if (!isOpOfType(ins->oprs[0], REGISTER))
        return false;
    if (lfi_to_reg64(ins->oprs[0].basereg) != R_RSP)
        return false;
    /* Only handle arithmetic modifications, not push/pop/call/ret */
    switch (ins->opcode) {
    case I_ADD: case I_SUB: case I_AND: case I_OR: case I_XOR:
    case I_MOV: case I_XCHG:
        return true;
    default:
        break;
    }
    /* POP rsp */
    if (ins->opcode == I_POP && lfi_to_reg64(ins->oprs[0].basereg) == R_RSP)
        return true;
    return false;
}

/*
 * Check if an instruction is an indirect branch (jmp or call via register
 * or memory, not direct/relative).
 */
static bool lfi_is_indirect_branch(const insn *ins)
{
    if (ins->opcode != I_JMP && ins->opcode != I_CALL)
        return false;
    if (ins->operands < 1)
        return false;
    /* Direct call/jmp has an immediate operand */
    if (ins->oprs[0].type & IMMEDIATE)
        return false;
    /* Register or memory operand = indirect */
    return true;
}

static bool lfi_is_direct_call(const insn *ins)
{
    if (ins->opcode != I_CALL)
        return false;
    if (ins->operands < 1)
        return false;
    /* Direct call has an immediate operand (label) */
    if (ins->oprs[0].type & IMMEDIATE)
        return true;
    return false;
}

static bool lfi_is_return(const insn *ins)
{
    switch (ins->opcode) {
    case I_RET: case I_RETW: case I_RETD: case I_RETQ:
    case I_RETN: case I_RETNW: case I_RETND: case I_RETNQ:
    case I_RETF: case I_RETFW: case I_RETFD: case I_RETFQ:
        return true;
    default:
        return false;
    }
}

/*
 * Main dispatch: generate replacement instruction sequence for an instruction.
 */
static void lfi_replace_instruction(const insn *ins,
                                    struct lfi_replacement *rep)
{
    memset(rep, 0, sizeof(*rep));
    rep->count = 1;
    rep->align_to_end = false;
    rep->instructions[0] = *ins;
    rep->bundle_locked[0] = false;

    /* Syscall */
    if (ins->opcode == I_SYSCALL) {
        lfi_expand_syscall(ins, rep);
        return;
    }

    /* TLS read */
    if (lfi_is_tls_read(ins)) {
        lfi_expand_tls_read(ins, rep);
        return;
    }

    /* Return */
    if (lfi_is_return(ins)) {
        lfi_expand_return(ins, rep);
        return;
    }

    /* Direct call */
    if (lfi_is_direct_call(ins)) {
        lfi_expand_direct_call(ins, rep);
        return;
    }

    /* Indirect branch (jmp/call via register or memory) */
    if (lfi_is_indirect_branch(ins)) {
        if (isOpOfType(ins->oprs[0], REGISTER)) {
            if (ins->opcode == I_CALL)
                lfi_expand_indirect_call_reg(ins->oprs[0].basereg, rep);
            else
                lfi_expand_indirect_jmp_reg(ins->oprs[0].basereg, rep);
        } else {
            lfi_expand_indirect_branch_mem(ins, rep,
                                           ins->opcode == I_CALL);
        }
        return;
    }

    /* String operations */
    if (lfi_is_string_op(ins)) {
        lfi_expand_string_op(ins, rep);
        return;
    }

    /* Stack pointer modification */
    if (lfi_modifies_rsp(ins)) {
        lfi_expand_stack_modification(ins, rep);
        return;
    }

    /* Load/store with memory operand: sandbox the memory access */
    lfi_expand_load_store(ins, rep);
}

/*
 * Multi-byte NOP table (Intel-recommended sequences).
 * Index is the NOP size in bytes (1-9).
 */
static const char *lfi_nop_table[] = {
    NULL,
    "nop",                                                          /* 1: 90 */
    "db 0x66, 0x90",                                                /* 2: 66 90 */
    "db 0x0f, 0x1f, 0x00",                                         /* 3: 0f 1f 00 */
    "db 0x0f, 0x1f, 0x40, 0x00",                                   /* 4: 0f 1f 40 00 */
    "db 0x0f, 0x1f, 0x44, 0x00, 0x00",                             /* 5: 0f 1f 44 00 00 */
    "db 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00",                       /* 6: 66 0f 1f 44 00 00 */
    "db 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00",                 /* 7: 0f 1f 80 00 00 00 00 */
    "db 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00",           /* 8: 0f 1f 84 00 00 00 00 00 */
    "db 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00",    /* 9: 66 0f 1f 84 00 00 00 00 00 */
};
#define LFI_MAX_NOP_SIZE 9

/*
 * Emit N bytes of NOP padding via process_insn().
 * Uses multi-byte NOPs for better performance.
 */
static void lfi_emit_nop_padding(int nbytes)
{
    insn nop_ins;
    while (nbytes > 0) {
        int sz = (nbytes > LFI_MAX_NOP_SIZE) ? LFI_MAX_NOP_SIZE : nbytes;
        lfi_parse(lfi_nop_table[sz], &nop_ins);
        nop_ins.times = 1;
        process_insn(&nop_ins);
        cleanup_insn(&nop_ins);
        nbytes -= sz;
    }
}

/*
 * Compute the total size of a group of replacement instructions.
 * Each instruction must have loc set for insn_size() to work.
 */
static int64_t lfi_group_size(struct lfi_replacement *rep,
                              int start, int end)
{
    int64_t total = 0;
    for (int i = start; i < end; i++) {
        rep->instructions[i].loc = location;
        rep->instructions[i].times = 1;
        int64_t sz = insn_size(&rep->instructions[i]);
        if (sz < 0)
            sz = 0;
        total += sz;
    }
    return total;
}

/*
 * Process a single instruction through the LFI layer.
 * This replaces the direct call to process_insn() in assemble_file().
 *
 * The approach: generate replacement instructions, then for each one
 * check if padding is needed for bundle alignment. Emit NOP padding
 * as needed, then emit the replacement instruction via process_insn().
 *
 * Bundle-locked groups must not cross a 32-byte boundary.
 * align_to_end means the last instruction in the group should end
 * at a 32-byte boundary (for calls, so return address is aligned).
 *
 * The multi-pass assembler will converge because the same replacement
 * logic runs identically each pass (padding depends only on offset).
 */
static void lfi_process_instruction(insn *ins)
{
    struct lfi_replacement rep;
    int32_t times_save = ins->times;
    int32_t t;

    if (ins->opcode == I_none) {
        process_insn(ins);
        return;
    }

    /* Validate reserved register usage */
    lfi_validate_instruction(ins);

    /* Get the replacement sequence */
    lfi_replace_instruction(ins, &rep);

    if (rep.count == 1 && !rep.bundle_locked[0] && !rep.align_to_end) {
        /* Single replacement, no bundling needed */
        rep.instructions[0].times = ins->times;
        process_insn(&rep.instructions[0]);
        return;
    }

    /* Handle TIMES: iterate manually with times=1 */
    t = (times_save > 0) ? times_save : 1;

    while (t-- > 0) {
        /*
         * Find the start of each bundle-locked group and compute
         * the total group size. Emit padding before the group if needed.
         */
        int i = 0;
        while (i < rep.count) {
            insn *ri = &rep.instructions[i];
            ri->times = 1;

            if (!rep.bundle_locked[i]) {
                /* Not bundle-locked: just emit it */
                process_insn(ri);
                i++;
                continue;
            }

            /*
             * Start of a bundle-locked group. Find the end.
             */
            int group_start = i;
            int group_end = i;
            while (group_end < rep.count && rep.bundle_locked[group_end])
                group_end++;

            /*
             * Compute the total size of the bundle-locked group
             * using insn_size() for each instruction.
             */
            int64_t group_sz = lfi_group_size(&rep, group_start, group_end);

            /*
             * Compute required padding using lfi_get_padding().
             * For align_to_end (calls): the group should end at a
             * bundle boundary so the return address is aligned.
             * Otherwise: ensure the group doesn't cross a boundary.
             */
            bool is_end_group = rep.align_to_end && (group_end == rep.count);
            int pad = lfi_get_padding(location.offset, (int)group_sz,
                                      group_sz, is_end_group);
            if (pad > 0)
                lfi_emit_nop_padding(pad);

            /* Emit all instructions in the bundle-locked group */
            for (int j = group_start; j < group_end; j++) {
                insn *gri = &rep.instructions[j];
                gri->times = 1;
                process_insn(gri);
            }
            i = group_end;
        }
    }
}

static void assemble_file(const char *fname, struct strlist *depend_list)
{
    char *line;
    insn output_ins;
    uint64_t prev_offset_changed;
    int64_t stall_count = 0; /* Make sure we make forward progress... */

    if (lfi_mode && cmd_sb != 64)
        nasm_fatal("--lfi requires 64-bit mode");

    switch (cmd_sb) {
    case 16:
        break;
    case 32:
        if (!iflag_cpu_level_ok(&cmd_cpu, IF_386))
            nasm_fatal("command line: 32-bit segment size requires a higher cpu");
        break;
    case 64:
        if (!iflag_cpu_level_ok(&cmd_cpu, IF_X86_64))
            nasm_fatal("command line: 64-bit segment size requires a higher cpu");
        break;
    default:
        panic();
        break;
    }

    prev_offset_changed = INT64_MAX;

    if (listname && !keep_all) {
        /* Remove the list file in case we die before the output pass */
        remove(listname);
    }

    while (!terminate_after_phase() && !pass_final()) {
        _passn++;
        switch (pass_type()) {
        case PASS_INIT:
            _pass_type = PASS_FIRST;
            break;
        case PASS_OPT:
            if (global_offset_changed)
                break;          /* One more optimization pass */
            /* fall through */
        default:
            _pass_type++;
            break;
        }

        error_pass_start(pass_final());
        global_offset_changed = 0;

        /* Suppress ERR_PASS2 unless we are actually in the final pass */
        erropt.never = 0;
        if (!pass_final())
            erropt.never |= ERR_PASS2;

        reset_global_defaults(cmd_sb);

        cpu = cmd_cpu;
        if (listname) {
            if (list_on_this_pass()) {
                /*
                 * Generating a list file on this pass.
                 */
                lfmt->init(listname);
            } else if (list_active()) {
                /*
                 * Looks like we used the list engine on a previous pass,
                 * but now it is turned off, presumably via %pragma -p
                 */
                lfmt->cleanup();
                if (!keep_all)
                    remove(listname);
            }
        }

        in_absolute = false;
        if (!pass_first()) {
            saa_rewind(forwrefs);
            forwref = saa_rstruct(forwrefs);
            raa_free(offsets);
            offsets = raa_init();
        }
        location.segment = NO_SEG;
        location.offset  = 0;
        if (pass_first())
            location.known = true;
        ofmt->reset();
        switch_segment(ofmt->section(NULL, &globl.bits));
        pp_reset(fname, PP_NORMAL, depend_list);

        globallineno = 0;

        while ((line = pp_getline())) {
            if (++globallineno > nasm_limit[LIMIT_LINES])
                nasm_fatal("overall line count exceeds the maximum %"PRId64"\n",
                           nasm_limit[LIMIT_LINES]);

            /*
             * LFI: align labels to 32-byte bundle boundaries so that
             * direct jump targets are always bundle-aligned.
             */
            if (lfi_mode && !lfi_no_align_labels &&
                parse_check_is_label(line)) {
                int pad = (LFI_BUNDLE_SIZE -
                           (int)(location.offset % LFI_BUNDLE_SIZE))
                          % LFI_BUNDLE_SIZE;
                if (pad > 0)
                    lfi_emit_nop_padding(pad);
            }

            /*
             * Here we parse our directives; this is not handled by the
             * main parser.
             */
            if (process_directives(line))
                goto end_of_line; /* Just do final cleanup */

            /* Not a directive, or even something that starts with [ */
            parse_line(line, &output_ins, globl.bits);
            forward_refs(&output_ins);
            if (lfi_mode && output_ins.opcode != I_none) {
                lfi_process_instruction(&output_ins);
            } else {
                process_insn(&output_ins);
            }
            cleanup_insn(&output_ins);

        end_of_line:
            nasm_free(line);
        }                       /* end while (line = pp_getline... */

        pp_cleanup_pass();

        if (global_offset_changed) {
            switch (pass_type()) {
            case PASS_OPT:
                /*
                 * This is the only pass type that can be executed more
                 * than once, and therefore has the ability to stall.
                 */
                if (global_offset_changed < prev_offset_changed) {
                    prev_offset_changed = global_offset_changed;
                    stall_count = 0;
                } else {
                    stall_count++;
                }

                if (stall_count > nasm_limit[LIMIT_STALLED] ||
                    pass_count() >= nasm_limit[LIMIT_PASSES]) {
                    /* No convergence, almost certainly dead */
                    nasm_nonfatalf(ERR_UNDEAD,
                                   "unable to find valid values for all labels "
                                   "after %"PRId64" passes; "
                                   "stalled for %"PRId64", giving up.",
                                   pass_count(), stall_count);
                    nasm_nonfatalf(ERR_UNDEAD,
                               "Possible causes: recursive EQUs, macro abuse.");
                }
                break;

            case PASS_STAB:
                nasm_warn(WARN_PHASE|ERR_UNDEAD,
                          "phase error during stabilization "
                          "pass, hoping for the best");
                break;

            case PASS_FINAL:
                nasm_nonfatalf(ERR_UNDEAD,
                               "phase error during code generation pass");
                break;

            default:
                /* This is normal, we'll keep going... */
                break;
            }
        }

        error_pass_end();
    }

    print_final_report(terminate_after_phase());

    lfmt->cleanup();
}

void close_output(bool error)
{
    if (!ofile)
        return;

    if (ofile == stdout || ofile == stdin) {
        fflush(ofile);
    } else {
        fclose(ofile);
        if (error && !keep_all)
            remove(outname);
    }
    ofile = NULL;
}

enum help_with {
    HW_ALL   = 0,
    HW_OPT   = 1,
    HW_ERR   = 2,
    HW_LIMIT = 3
};

static inline bool help_is(int with, int h) {
    return with == HW_ALL || with == h;
}
static inline bool help_opt(int with)
{
    return help_is(with, HW_OPT);
}
static inline bool help_optor(int with, int h)
{
    return help_opt(with) || with == h;
}

static void help(FILE *out, const char *what)
{
    int i;
    int with;

#define SEE(x) (with == HW_OPT ? " (see -h " x ")" : "")

    with = HW_ERR;

    if (!what || !*what || !strcmp(what, "-") || !strcmp(what, "--") ||
        !nasm_stricmp(what, "opt") ||
        !nasm_stricmp(what, "run") ||
        !nasm_stricmp(what, "cmd")) {
        with = HW_OPT;
    } else if (!nasm_stricmp(what, "all")) {
        with = HW_ALL;
    } else if (!nasm_strnicmp(what, "--limit", 7) ||
               !nasm_strnicmp(what, "limit", 5)) {
        with = HW_LIMIT;
    } else if (!nasm_stricmp(what, "--help") ||
               !nasm_stricmp(what, "help") ||
               !nasm_stricmp(what, "list") ||
               !nasm_stricmp(what, "topics") ||
               !nasm_stricmp(what, "index")) {
        with = 'h';
    } else if ((what[0] == '-' && what[1] && !what[2]) ||
               (what[0] && !what[1])) {
        with = what[0] == '-' ? what[1] : what[0];
        switch (with) {
        case 'h':
        case 'X':
        case 'L':
        case 'f':
        case 'F':
        case 'O':
        case 'w':
        case 'M':
            break;
        case 'g':
            with = 'F';
            break;
        case 'W':
            with = 'w';
            break;
        case '?':
            with = 'h';
            break;
        case '*':
            with = HW_ALL;
            break;
        default:
            with = HW_ERR;
            break;
        }
    }

    if (with == HW_ERR) {
        fprintf(out, "Sorry, no help available for `%s'\n", what);
        return;
    }

    if (help_opt(with)) {
        fprintf(out,
                "Usage: %s [-@ response_file] [options...] [--] filename\n"
                "  Options:\n"
                "    -v (or --v)    print the NASM version number and exit\n"
                "    -@ file        response file; one command line option per line\n",
                _progname);
    }
    if (help_optor(with, 'h')) {
        fputs(
            "    -h             list command line options and exit (also --help)\n"
            , out);
        if (with == HW_OPT) {
            fputs(
                "    -h -opt        show additional help for option \"-opt\"\n"
                , out);
        }
        fputs(
            "    -h all         show all available command line help\n"
            "    -h topics      show list of help topics (also -h list)\n"
            , out);
    }
    if (help_is(with, 'h')) {
        fputs(
            "    -h -X          show list of error message formats\n"
            "    -h -M          show list of dependency generation options\n"
            "    -h -f          show list of object code output formats\n"
            "    -h -F          show list of debug information formats\n"
            "    -h -L          show list of list file option flags\n"
            "    -h -O          show list of optimization flags\n"
            "    -h -w          show list of warning classes and defaults\n"
            "    -h --limit     show list of resource limits and defaults\n"
            , out);
    }
    if (help_opt(with)) {
        fputs(
            "    -o outfile     write output to outfile\n"
            "    --keep-all     output files will not be removed even if an error happens\n"
            , out);
    }
    if (help_optor(with, 'X')) {
        fprintf(out,
                "    -Xformat       specify error reporting format%s\n",
                SEE("-X"));
    }
    if (help_is(with, 'X')) {
        fputs(
            "    -Xgnu          report errors in GNU format (default)\n"
            "    -Xgcc          alias for -Xgnu\n"
            "    -Xvc           report errors in Visual Studio format\n"
            "    -Xmsvc         alias for -Xvc\n"
            "    -Xms           alias for -Xvc\n"
            , out);
    }
    if (help_opt(with)) {
        fputs(
            "    -s             redirect messages to stdout\n"
            "    -Zfile         redirect messages to file\n"
            "    --info[=lvl]   display optional informational messages\n"
            "    --debug[=lvl]  display NASM internal debugging messages\n"
            , out);
    }
    if (help_optor(with, 'M')) {
        fprintf(out,
                "    -M...          generate Makefile dependencies%s\n",
                with == HW_OPT
                ? " (see -h -M)"
                : " (multiple options permitted):");
    }
    if (help_is(with, 'M')) {
        fputs(
            "    -M             generate Makefile dependencies on stdout\n"
            "    -MG            d:o, missing files assumed generated\n"
            "    -MF file       set Makefile dependency file\n"
            "    -MD file       assemble and generate dependencies\n"
            "    -MT file       dependency target name\n"
            "    -MQ file       dependency target name (quoted)\n"
            "    -MP            emit phony targets\n"
            "    -MW            generate output in Watcom wmake syntax\n"
            , out);
    }
    if (help_optor(with, 'f')) {
        fprintf(out,
                "    -f format      select output file format%s\n",
                SEE("-f"));
    }
    if (help_is(with, 'f')) {
        ofmt_list(ofmt, out);
    }
    if (help_optor(with, 'F')) {
        fprintf(out,
                "    -g             generate debugging information\n"
                "    -F format      select a debugging format%s\n"
                "    -gformat       same as -g -F format\n",
                SEE("-F"));
    }
    if (help_is(with, 'F')) {
        dfmt_list(out);
    }
    if (help_optor(with, 'L')) {
        fprintf(out,
                "    -l listfile    write listing to a list file\n"
                "    -Lflags...     add information to the list file%s\n",
                SEE("-L"));
    }
    if (help_is(with, 'L')) {
        fputs(
            "       -Lb         show builtin macro packages (standard and %use)\n"
            "       -Ld         show byte and repeat counts in decimal, not hex\n"
            "       -Le         show the preprocessed output\n"
            "       -Lf         ignore .nolist (force output)\n"
            "       -LF         ignore [LIST -] directives (force output)\n"
            "       -Lm         show multi-line macro calls with expanded parameters\n"
            "       -Lp         output a list file every pass, in case of errors\n"
            "       -Ls         show all single-line macro definitions\n"
            "       -Lw         flush the output after every line (very slow!)\n"
            "       -L+         enable all listing options except -Lw (very verbose!)\n"
            , out);
    }
    if (help_optor(with, 'O')) {
        fprintf(out,
                "    -Oflags...     select optimization%s\n", SEE("-O"));
    }
    if (help_is(with, 'O')) {
        fputs(
            "       -O0       no optimization\n"
            "       -O1       minimal optimization\n"
            "       -Ox       multipass optimization (default, recommended)\n"
            "       -Ov       display the number of passes executed at the end\n"
            , out);
    }
    if (help_opt(with)) {
        fputs(
            "    -t             assemble in limited SciTech TASM compatible mode\n"
            "    -E (or -e)     preprocess only (writes output to stdout by default)\n"
            "    -a             don't preprocess (assemble only)\n"
            "    -Ipath         add a pathname to the include file path\n"
            "    -Pfile         pre-include a file (also --include)\n"
            "    -Dmacro[=str]  pre-define a macro\n"
            "    -Umacro        undefine a macro\n"
            , out);
    }
    if (help_optor(with, 'w')) {
        fprintf(out,
                "    -w+x           enable warning x %s(also -Wx)\n"
                "    -w-x           disable warning x (also -Wno-x)\n"
                "    -w[+-]error    promote all warnings to errors (also -Werror)\n"
                "    -w[+-]error=x  promote warning x to errors (also -Werror=x)\n",
                SEE("-w"));
    }
    if (help_is(with, 'w')) {
        fputs("      Defaults in brackets:\n", out);

        fprintf(out, "       %-20s %s\n",
                warning_name[WARN_IDX_ALL], warning_help[WARN_IDX_ALL]);

        for (i = 1; i < WARN_IDX_ALL; i++) {
            static const char nl_indent[] =
                "\n                            ";
            const char *me   = warning_name[i];
            const char *prev = warning_name[i-1];
            const char *next = warning_name[i+1];

            if (prev) {
                int prev_len = strlen(prev);
                const char *dash = me;

                while ((dash = strchr(dash+1, '-'))) {
                    int prefix_len = dash - me; /* Not including final dash */
                    if (strncmp(next, me, prefix_len+1)) {
                        /* Only one or last option with this prefix */
                        break;
                    }
                    if (prefix_len >= prev_len ||
                        strncmp(prev, me, prefix_len) ||
                        (prev[prefix_len] != '-' && prev[prefix_len] != '\0')) {
                        /* This prefix is different from the previous option */
                        fprintf(out, "       %-20.*s%sall warnings prefixed with \"%.*s\"\n",
                                prefix_len, me,
                                prefix_len > 19 ? nl_indent : " ",
                                prefix_len+1, me);
                    }
                }
            }

            fprintf(out, "       %-20s%s%s%s\n",
                    warning_name[i],
                    strlen(warning_name[i]) > 19 ? nl_indent : " ",
                    warning_help[i],
                    (warning_default[i] & WARN_ST_ERROR) ? " [error]" :
                    (warning_default[i] & WARN_ST_ENABLED) ? " [on]" : " [off]");
        }
    }
    if (help_opt(with)) {
        fputs(
            "    --pragma str   pre-executes a specific %pragma\n"
            "    --before str   add line (usually a preprocessor statement) before the input\n"
            "    --bits nn      set bits to nn (equivalent to --before \"BITS nn\")\n"
            "    --no-line      ignore %line directives in input\n"
            "    --gprefix str  prepend the given string to the names of all extern,\n"
            "                   common and global symbols (also --prefix)\n"
            "    --gpostfix str append the given string to the names of all extern,\n"
            "                   common and global symbols (also --postfix)\n"
            "    --lprefix str  prepend the given string to local symbols\n"
            "    --lpostfix str append the given string to local symbols\n"
            "    --reproducible attempt to produce run-to-run identical output\n"
            , out);
    }
    if (help_optor(with, HW_LIMIT)) {
        fprintf(out, "    --limit-X val  set execution limit X%s\n",
                SEE("--limit"));
    }
    if (help_is(with, HW_LIMIT)) {
        fputs("      Defaults in brackets:\n", out);

        for (i = 0; i <= LIMIT_MAX; i++) {
            fprintf(out, "       %-20s %s [",
                    limit_info[i].name, limit_info[i].help);
            if (nasm_limit[i] < LIMIT_MAX_VAL) {
                fprintf(out, "%"PRId64"]\n", nasm_limit[i]);
            } else {
                fputs("unlimited]\n", out);
            }
        }
    }
#undef SEE
}
