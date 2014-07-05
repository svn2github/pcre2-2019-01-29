/*************************************************
*             PCRE2 testing program              *
*************************************************/

/* PCRE2 is a library of functions to support regular expressions whose syntax
and semantics are as close as possible to those of the Perl 5 language. In 2014
the API was completely revised and '2' was added to the name, because the old
API, which had lasted for 16 years, could not accommodate new requirements. At
the same time, this testing program was re-designed because its original
hacked-up (non-) design had also run out of steam.

                       Written by Philip Hazel
     Original code Copyright (c) 1997-2012 University of Cambridge
         Rewritten code Copyright (c) 2014 University of Cambridge

-----------------------------------------------------------------------------
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    * Neither the name of the University of Cambridge nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
-----------------------------------------------------------------------------
*/

/* FIXME: These are the as-yet-unimplemented features:
. save code and #load
. JIT - compile, time, verify
. memory handling testing
. stackguard testing
*/



/* This program supports testing of the 8-bit, 16-bit, and 32-bit PCRE2
libraries in a single program, though its input and output are always 8-bit.
It is different from modules such as pcre2_compile.c in the library itself,
which are compiled separately for each code unit width. If two widths are
enabled, for example, pcre2_compile.c is compiled twice. In contrast,
pcre2test.c is compiled only once, and linked with all the enabled libraries.
Therefore, it must not make use of any of the macros from pcre2.h or
pcre2_internal.h that depend on PCRE2_CODE_UNIT_WIDTH. It does, however, make
use of SUPPORT_PCRE8, SUPPORT_PCRE16, and SUPPORT_PCRE32, to ensure that it
references only the enabled library functions. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <locale.h>
#include <errno.h>

/* Both libreadline and libedit are optionally supported. The user-supplied
original patch uses readline/readline.h for libedit, but in at least one system
it is installed as editline/readline.h, so the configuration code now looks for
that first, falling back to readline/readline.h. */

#if defined(SUPPORT_LIBREADLINE) || defined(SUPPORT_LIBEDIT)
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#if defined(SUPPORT_LIBREADLINE)
#include <readline/readline.h>
#include <readline/history.h>
#else
#if defined(HAVE_EDITLINE_READLINE_H)
#include <editline/readline.h>
#else
#include <readline/readline.h>
#endif
#endif
#endif


/* ---------------------- System-specific definitions ---------------------- */

/* A number of things vary for Windows builds. Originally, pcretest opened its
input and output without "b"; then I was told that "b" was needed in some
environments, so it was added for release 5.0 to both the input and output. (It
makes no difference on Unix-like systems.) Later I was told that it is wrong
for the input on Windows. I've now abstracted the modes into two macros that
are set here, to make it easier to fiddle with them, and removed "b" from the
input mode under Windows. */

#if defined(_WIN32) || defined(WIN32)
#include <io.h>                /* For _setmode() */
#include <fcntl.h>             /* For _O_BINARY */
#define INPUT_MODE   "r"
#define OUTPUT_MODE  "wb"

#ifndef isatty
#define isatty _isatty         /* This is what Windows calls them, I'm told, */
#endif                         /* though in some environments they seem to   */
                               /* be already defined, hence the #ifndefs.    */
#ifndef fileno
#define fileno _fileno
#endif

/* A user sent this fix for Borland Builder 5 under Windows. */

#ifdef __BORLANDC__
#define _setmode(handle, mode) setmode(handle, mode)
#endif

/* Not Windows */

#else
#include <sys/time.h>          /* These two includes are needed */
#include <sys/resource.h>      /* for setrlimit(). */
#if defined NATIVE_ZOS         /* z/OS uses non-binary I/O */
#define INPUT_MODE   "r"
#define OUTPUT_MODE  "w"
#else
#define INPUT_MODE   "rb"
#define OUTPUT_MODE  "wb"
#endif
#endif

#ifdef __VMS
#include <ssdef.h>
void vms_setsymbol( char *, char *, int );
#endif

/* ------------------End of system-specific definitions -------------------- */

/* Glueing macros that are used in several places below. */

#define glue(a,b) a##b
#define G(a,b) glue(a,b)

/* Miscellaneous parameters and manifests */

#ifndef CLOCKS_PER_SEC
#ifdef CLK_TCK
#define CLOCKS_PER_SEC CLK_TCK
#else
#define CLOCKS_PER_SEC 100
#endif
#endif

#define CFAIL_UNSET UINT32_MAX  /* Unset value for cfail fields */
#define DFA_WS_DIMENSION 1000   /* Size of DFA workspace */
#define DEFAULT_OVECCOUNT 15    /* Default ovector count */
#define LOOPREPEAT 500000       /* Default loop count for timing. */
#define VERSION_SIZE 64         /* Size of buffer for the version string. */

/* Execution modes */

#define PCRE8_MODE   8
#define PCRE16_MODE 16
#define PCRE32_MODE 32

/* Processing returns */

enum { PR_OK, PR_SKIP, PR_ABEND };

/* The macro PRINTABLE determines whether to print an output character as-is or
as a hex value when showing compiled patterns. is We use it in cases when the
locale has not been explicitly changed, so as to get consistent output from
systems that differ in their output from isprint() even in the "C" locale. */

#ifdef EBCDIC
#define PRINTABLE(c) ((c) >= 64 && (c) < 255)
#else
#define PRINTABLE(c) ((c) >= 32 && (c) < 127)
#endif

#define PRINTOK(c) ((locale_tables != NULL)? isprint(c) : PRINTABLE(c))

/* We have to include some of the library source files because we need
to use some of the macros, internal structure definitions, and other internal
values - pcre2test has "inside information" compared to an application program
that strictly follows the PCRE2 API.

Before including pcre2_internal.h we define PRIV so that it does not get
defined therein. This ensures that PRIV names in the included files do not
clash with those in the libraries. Also, although pcre2_internal.h does itself
include pcre2.h, we explicitly include it beforehand, along with pcre2posix.h,
so that the PCRE2_EXP_xxx macros get set appropriately for an application, not
for building the library. */

#define PRIV(name) name
#include "pcre2.h"
#include "pcre2posix.h"
#include "pcre2_internal.h"

/* We need access to some of the data tables that PCRE uses. Defining
PCRE2_PCRETEST makes some minor changes in the files. The previous definition
of PRIV avoids name clashes. */

#define PCRE2_PCRE2TEST
#include "pcre2_tables.c"
#include "pcre2_ucd.c"

/* When PCRE2_CODE_UNIT_WIDTH is unset, pcre2_internal.h does not include
pcre2_intmodedep.h, which is where mode-dependent macros and structures are
defined. We can now include it for each supported code unit width. Because
PCRE2_CODE_UNIT_WIDTH was not defined before including pcre2.h, it will have
left PCRE2_SUFFIX defined as a no-op. We must re-define it appropriately while
including these files, and then restore it to a no-op. Because LINK_SIZE may be
changed in 16-bit mode and forced to 1 in 32-bit mode, the order of these
inclusions should not be changed. */

#undef PCRE2_SUFFIX

#ifdef   SUPPORT_PCRE8
#define  PCRE2_CODE_UNIT_WIDTH 8
#define  PCRE2_SUFFIX(a) G(a,8)
#include "pcre2_intmodedep.h"
#include "pcre2_printint.c"
#undef   PCRE2_CODE_UNIT_WIDTH
#undef   PCRE2_SUFFIX
#endif   /* SUPPORT_PCRE8 */

#ifdef   SUPPORT_PCRE16
#define  PCRE2_CODE_UNIT_WIDTH 16
#define  PCRE2_SUFFIX(a) G(a,16)
#include "pcre2_intmodedep.h"
#include "pcre2_printint.c"
#undef   PCRE2_CODE_UNIT_WIDTH
#undef   PCRE2_SUFFIX
#endif   /* SUPPORT_PCRE16 */

#ifdef   SUPPORT_PCRE32
#define  PCRE2_CODE_UNIT_WIDTH 32
#define  PCRE2_SUFFIX(a) G(a,32)
#include "pcre2_intmodedep.h"
#include "pcre2_printint.c"
#undef   PCRE2_CODE_UNIT_WIDTH
#undef   PCRE2_SUFFIX
#endif   /* SUPPORT_PCRE32 */

#define PCRE2_SUFFIX(a) a

/* If we have 8-bit support, default to it; if there is also 16-or 32-bit
support, it can be selected by a command-line option. If there is no 8-bit
support, there must be 16- or 32-bit support, so default to one of them. The
config function, JIT stack, contexts, and version string are the same in all
modes, so use the form of the first that is available. */

#if defined SUPPORT_PCRE8
#define DEFAULT_TEST_MODE PCRE8_MODE
#define VERSION_TYPE PCRE2_UCHAR8
#define PCRE2_CONFIG pcre2_config_8
#define PCRE2_JIT_STACK pcre2_jit_stack_8
#define PCRE2_REAL_GENERAL_CONTEXT pcre2_real_general_context_8
#define PCRE2_REAL_COMPILE_CONTEXT pcre2_real_compile_context_8
#define PCRE2_REAL_MATCH_CONTEXT pcre2_real_match_context_8
#define VERSION_TYPE PCRE2_UCHAR8

#elif defined SUPPORT_PCRE16
#define DEFAULT_TEST_MODE PCRE16_MODE
#define VERSION_TYPE PCRE2_UCHAR16
#define PCRE2_CONFIG pcre2_config_16
#define PCRE2_JIT_STACK pcre2_jit_stack_16
#define PCRE2_REAL_GENERAL_CONTEXT pcre2_real_general_context_16
#define PCRE2_REAL_COMPILE_CONTEXT pcre2_real_compile_context_16
#define PCRE2_REAL_MATCH_CONTEXT pcre2_real_match_context_16

#elif defined SUPPORT_PCRE32
#define DEFAULT_TEST_MODE PCRE32_MODE
#define VERSION_TYPE PCRE2_UCHAR32
#define PCRE2_CONFIG pcre2_config_32
#define PCRE2_JIT_STACK pcre2_jit_stack_32
#define PCRE2_REAL_GENERAL_CONTEXT pcre2_real_general_context_32
#define PCRE2_REAL_COMPILE_CONTEXT pcre2_real_compile_context_32
#define PCRE2_REAL_MATCH_CONTEXT pcre2_real_match_context_32
#endif


/* ------------- Structures and tables for handling modifiers -------------- */

/* Table of names for newline types. Must be kept in step with the definitions
of PCRE2_NEWLINE_xx in pcre2.h. */

static const char *newlines[] = {
  "DEFAULT", "CR", "LF", "CRLF", "ANY", "ANYCRLF" };

/* Modifier types and applicability */

enum { MOD_CTB,    /* Applies to a compile or a match context */
       MOD_CTC,    /* Applies to a compile context */
       MOD_CTM,    /* Applies to a match context */
       MOD_PAT,    /* Applies to a pattern */
       MOD_PATP,   /* Ditto, OK for Perl test */
       MOD_DAT,    /* Applies to a data line */
       MOD_PD,     /* Applies to a pattern or a data line */
       MOD_PDP,    /* As MOD_PD, OK for Perl test */
       MOD_PND,    /* As MOD_PD, but not for a default pattern */
       MOD_PNDP,   /* As MOD_PND, OK for Perl test */
       MOD_CTL,    /* Is a control bit */
       MOD_BSR,    /* Is a BSR value */
       MOD_IN2,    /* Is one or two unsigned integers */
       MOD_INS,    /* Is a signed integer */
       MOD_INT,    /* Is an unsigned integer */
       MOD_IND,    /* Is an unsigned integer, but no value => default */
       MOD_NL,     /* Is a newline value */
       MOD_NN,     /* Is a number or a name; more than one may occur */
       MOD_OPT,    /* Is an option bit */
       MOD_STR };  /* Is a string */

/* Control bits. Some apply to compiling, some to matching, but some can be set
either on a pattern or a data line, so they must all be distinct. */

#define CTL_AFTERTEXT        0x00000001
#define CTL_ALLAFTERTEXT     0x00000002
#define CTL_ALLCAPTURES      0x00000004
#define CTL_ALTGLOBAL        0x00000008
#define CTL_BINCODE          0x00000010
#define CTL_CALLOUT_CAPTURE  0x00000020
#define CTL_CALLOUT_NONE     0x00000040
#define CTL_DFA              0x00000080
#define CTL_FINDLIMITS       0x00000100
#define CTL_FLIPBYTES        0x00000200
#define CTL_FULLBINCODE      0x00000400
#define CTL_GETALL           0x00000800
#define CTL_GLOBAL           0x00001000
#define CTL_HEXPAT           0x00002000
#define CTL_INFO             0x00004000
#define CTL_JITVERIFY        0x00008000
#define CTL_MARK             0x00010000
#define CTL_MEMORY           0x00020000
#define CTL_PATLEN           0x00040000
#define CTL_POSIX            0x00080000

#define CTL_DEBUG            (CTL_FULLBINCODE|CTL_INFO)  /* For setting */
#define CTL_ANYINFO          (CTL_DEBUG|CTL_BINCODE)     /* For testing */
#define CTL_ANYGLOB          (CTL_ALTGLOBAL|CTL_GLOBAL)

/* These are all the controls that may be set either on a pattern or on a
data line. */

#define CTL_ALLPD            (CTL_AFTERTEXT|CTL_ALLAFTERTEXT|CTL_ALLCAPTURES|\
                              CTL_ALTGLOBAL|CTL_GLOBAL|CTL_JITVERIFY|CTL_MARK|\
                              CTL_MEMORY)

typedef struct patctl {    /* Structure for pattern modifiers. */
  uint32_t  options;       /* Must be in same position as datctl */
  uint32_t  control;       /* Must be in same position as datctl */
  uint32_t  jit;
  uint32_t  stackguard_test;
  uint32_t  tables_id;
  uint8_t   locale[32];
  uint8_t   save[64];
} patctl;

#define MAXCPYGET 10
#define LENCPYGET 64

typedef struct datctl {    /* Structure for data line modifiers. */
  uint32_t  options;       /* Must be in same position as patctl */
  uint32_t  control;       /* Must be in same position as patctl */
  uint32_t  cfail[2];
   int32_t  callout_data;
   int32_t  copy_numbers[MAXCPYGET];
   int32_t  get_numbers[MAXCPYGET];
  uint32_t  jitstack;
  uint32_t  oveccount;
  uint32_t  offset;
  uint8_t   copy_names[LENCPYGET];
  uint8_t   get_names[LENCPYGET];
} datctl;

/* Ids for which context to modify. */

enum { CTX_PAT,            /* Active pattern context */
       CTX_DEFPAT,         /* Default pattern context */
       CTX_DAT,            /* Active data (match) context */
       CTX_DEFDAT,         /* Default data (match) context */
       CTX_DEFANY };       /* Any default context (depends on the modifier) */

/* Macros to simplify the big table below. */

#define CO(name) offsetof(PCRE2_REAL_COMPILE_CONTEXT, name)
#define MO(name) offsetof(PCRE2_REAL_MATCH_CONTEXT, name)
#define PO(name) offsetof(patctl, name)
#define PD(name) PO(name)
#define DO(name) offsetof(datctl, name)

/* Table of all long-form modifiers. Must be in collating sequence of modifier
name because it is searched by binary chop. */

typedef struct modstruct {
  const char   *name;
  uint16_t      which;
  uint16_t      type;
  uint32_t      value;
  PCRE2_OFFSET  offset;
} modstruct;

static modstruct modlist[] = {
  { "aftertext",           MOD_PNDP, MOD_CTL, CTL_AFTERTEXT,             PO(control) },
  { "allaftertext",        MOD_PNDP, MOD_CTL, CTL_ALLAFTERTEXT,          PO(control) },
  { "allcaptures",         MOD_PND,  MOD_CTL, CTL_ALLCAPTURES,           PO(control) },
  { "allow_empty_class",   MOD_PAT,  MOD_OPT, PCRE2_ALLOW_EMPTY_CLASS,   PO(options) },
  { "alt_bsux",            MOD_PAT,  MOD_OPT, PCRE2_ALT_BSUX,            PO(options) },
  { "altglobal",           MOD_PND,  MOD_CTL, CTL_ALTGLOBAL,             PO(control) },
  { "anchored",            MOD_PD,   MOD_OPT, PCRE2_ANCHORED,            PD(options) },
  { "auto_callout",        MOD_PAT,  MOD_OPT, PCRE2_AUTO_CALLOUT,        PO(options) },
  { "bincode",             MOD_PAT,  MOD_CTL, CTL_BINCODE,               PO(control) },
  { "bsr",                 MOD_CTB,  MOD_BSR, MO(bsr_convention),        CO(bsr_convention) },
  { "callout_capture",     MOD_DAT,  MOD_CTL, CTL_CALLOUT_CAPTURE,       DO(control) },
  { "callout_data",        MOD_DAT,  MOD_INS, 0,                         DO(callout_data) },
  { "callout_fail",        MOD_DAT,  MOD_IN2, 0,                         DO(cfail) },
  { "callout_none",        MOD_DAT,  MOD_CTL, CTL_CALLOUT_NONE,          DO(control) },
  { "caseless",            MOD_PATP, MOD_OPT, PCRE2_CASELESS,            PO(options) },
  { "copy",                MOD_DAT,  MOD_NN,  DO(copy_numbers),          DO(copy_names) },
  { "debug",               MOD_PAT,  MOD_CTL, CTL_DEBUG,                 PO(control) },
  { "dfa",                 MOD_DAT,  MOD_CTL, CTL_DFA,                   DO(control) },
  { "dfa_restart",         MOD_DAT,  MOD_OPT, PCRE2_DFA_RESTART,         DO(options) },
  { "dfa_shortest",        MOD_DAT,  MOD_OPT, PCRE2_DFA_SHORTEST,        DO(options) },
  { "dollar_endonly",      MOD_PAT,  MOD_OPT, PCRE2_DOLLAR_ENDONLY,      PO(options) },
  { "dotall",              MOD_PATP, MOD_OPT, PCRE2_DOTALL,              PO(options) },
  { "dupnames",            MOD_PAT,  MOD_OPT, PCRE2_DUPNAMES,            PO(options) },
  { "extended",            MOD_PATP, MOD_OPT, PCRE2_EXTENDED,            PO(options) },
  { "find_limits",         MOD_DAT,  MOD_CTL, CTL_FINDLIMITS,            DO(control) },
  { "firstline",           MOD_PAT,  MOD_OPT, PCRE2_FIRSTLINE,           PO(options) },
  { "flipbytes",           MOD_PAT,  MOD_CTL, CTL_FLIPBYTES,             PO(control) },
  { "fullbincode",         MOD_PAT,  MOD_CTL, CTL_FULLBINCODE,           PO(control) },
  { "get",                 MOD_DAT,  MOD_NN,  DO(get_numbers),           DO(get_names) },
  { "getall",              MOD_DAT,  MOD_CTL, CTL_GETALL,                DO(control) },
  { "global",              MOD_PNDP, MOD_CTL, CTL_GLOBAL,                PO(control) },
  { "hex",                 MOD_PAT,  MOD_CTL, CTL_HEXPAT,                PO(control) },
  { "info",                MOD_PAT,  MOD_CTL, CTL_INFO,                  PO(control) },
  { "jit",                 MOD_PAT,  MOD_IND, 7,                         PO(jit) },
  { "jitstack",            MOD_DAT,  MOD_INT, 0,                         DO(jitstack) },
  { "jitverify",           MOD_PND,  MOD_CTL, CTL_JITVERIFY,             PO(control) },
  { "locale",              MOD_PAT,  MOD_STR, 0,                         PO(locale) },
  { "mark",                MOD_PNDP, MOD_CTL, CTL_MARK,                  PO(control) },
  { "match_limit",         MOD_CTM,  MOD_INT, 0,                         MO(match_limit) },
  { "match_unset_backref", MOD_PAT,  MOD_OPT, PCRE2_MATCH_UNSET_BACKREF, PO(options) },
  { "memory",              MOD_PD,   MOD_CTL, CTL_MEMORY,                PD(control) },
  { "multiline",           MOD_PATP, MOD_OPT, PCRE2_MULTILINE,           PO(options) },
  { "never_ucp",           MOD_PAT,  MOD_OPT, PCRE2_NEVER_UCP,           PO(options) },
  { "never_utf",           MOD_PAT,  MOD_OPT, PCRE2_NEVER_UTF,           PO(options) },
  { "newline",             MOD_CTB,  MOD_NL,  MO(newline_convention),    CO(newline_convention) },
  { "no_auto_capture",     MOD_PAT,  MOD_OPT, PCRE2_NO_AUTO_CAPTURE,     PO(options) },
  { "no_auto_possess",     MOD_PATP, MOD_OPT, PCRE2_NO_AUTO_POSSESS,     PO(options) },
  { "no_start_optimize",   MOD_PDP,  MOD_OPT, PCRE2_NO_START_OPTIMIZE,   PD(options) },
  { "no_utf_check",        MOD_PD,   MOD_OPT, PCRE2_NO_UTF_CHECK,        PD(options) },
  { "notbol",              MOD_DAT,  MOD_OPT, PCRE2_NOTBOL,              DO(options) },
  { "notempty",            MOD_DAT,  MOD_OPT, PCRE2_NOTEMPTY,            DO(options) },
  { "notempty_atstart",    MOD_DAT,  MOD_OPT, PCRE2_NOTEMPTY_ATSTART,    DO(options) },
  { "noteol",              MOD_DAT,  MOD_OPT, PCRE2_NOTEOL,              DO(options) },
  { "offset",              MOD_DAT,  MOD_INT, 0,                         DO(offset) },
  { "ovector",             MOD_DAT,  MOD_INT, 0,                         DO(oveccount) },
  { "parens_nest_limit",   MOD_CTC,  MOD_INT, 0,                         CO(parens_nest_limit) },
  { "partial_hard",        MOD_DAT,  MOD_OPT, PCRE2_PARTIAL_HARD,        DO(options) },
  { "partial_soft",        MOD_DAT,  MOD_OPT, PCRE2_PARTIAL_SOFT,        DO(options) },
  { "posix",               MOD_PAT,  MOD_CTL, CTL_POSIX,                 PO(control) },
  { "recursion_limit",     MOD_CTM,  MOD_INT, 0,                         MO(recursion_limit) },
  { "save",                MOD_PAT,  MOD_STR, 0,                         PO(save) },
  { "stackguard",          MOD_PAT,  MOD_INT, 0,                         PO(stackguard_test) },
  { "tables",              MOD_PAT,  MOD_INT, 0,                         PO(tables_id) },
  { "ucp",                 MOD_PATP, MOD_OPT, PCRE2_UCP,                 PO(options) },
  { "ungreedy",            MOD_PAT,  MOD_OPT, PCRE2_UNGREEDY,            PO(options) },
  { "use_length",          MOD_PAT,  MOD_CTL, CTL_PATLEN,                PO(control) },
  { "utf",                 MOD_PATP, MOD_OPT, PCRE2_UTF,                 PO(options) }
};

#define MODLISTCOUNT sizeof(modlist)/sizeof(modstruct)

/* Controls and options that are supported for use with the POSIX interface. */

#define POSIX_SUPPORTED_COMPILE_OPTIONS ( \
  PCRE2_CASELESS|PCRE2_DOTALL|PCRE2_MULTILINE|PCRE2_NO_AUTO_CAPTURE| \
  PCRE2_UCP|PCRE2_UTF|PCRE2_UNGREEDY)

#define POSIX_SUPPORTED_COMPILE_CONTROLS ( \
  CTL_AFTERTEXT|CTL_ALLAFTERTEXT|CTL_POSIX)

#define POSIX_SUPPORTED_MATCH_OPTIONS ( \
  PCRE2_NOTBOL|PCRE2_NOTEMPTY|PCRE2_NOTEOL)

#define POSIX_SUPPORTED_MATCH_CONTROLS ( 0 )

/* Table of single-character and doubled-character abbreviated modifiers. The
index field is initialized to -1, but the first time the modifier is
encountered, it is filled in with the index of the full entry in modlist, to
save repeated searching when processing multiple test items. This short list is
searched serially, so its order does not matter. */

typedef struct c1modstruct {
  const char *fullname;
  uint32_t    onechar;
  int         index;
} c1modstruct;

static c1modstruct c1modlist[] = {
  { "bincode",      'B',           -1 },
  { "fullbincode",  ('B'<<8)|'B',  -1 },
  { "debug",        'D',           -1 },
  { "info",         'I',           -1 },
  { "partial_soft", 'P',           -1 },
  { "partial_hard", ('P'<<8)|'P',  -1 },
  { "global",       'g',           -1 },
  { "altglobal",    ('g'<<8)|'g',  -1 },
  { "caseless",     'i',           -1 },
  { "multiline",    'm',           -1 },
  { "dotall",       's',           -1 },
  { "extended",     'x',           -1 }
};

#define C1MODLISTCOUNT sizeof(c1modlist)/sizeof(c1modstruct)

/* Table of arguments for the -C command line option. Use macros to make the
table itself easier to read. */

#if defined SUPPORT_PCRE8
#define SUPPORT_8 1
#endif
#if defined SUPPORT_PCRE16
#define SUPPORT_16 1
#endif
#if defined SUPPORT_PCRE32
#define SUPPORT_32 1
#endif

#ifndef SUPPORT_8
#define SUPPORT_8 0
#endif
#ifndef SUPPORT_16
#define SUPPORT_16 0
#endif
#ifndef SUPPORT_32
#define SUPPORT_32 0
#endif

#ifdef EBCDIC
#define SUPPORT_EBCDIC 1
#define EBCDIC_NL CHAR_LF
#else
#define SUPPORT_EBCDIC 0
#define EBCDIC_NL 0
#endif

typedef struct coptstruct {
  const char *name;
  uint32_t    type;
  uint32_t    value;
} coptstruct;

enum { CONF_BSR,
       CONF_FIX,
       CONF_FIZ,
       CONF_INT,
       CONF_NL
};

static coptstruct coptlist[] = {
  { "bsr",       CONF_BSR, PCRE2_CONFIG_BSR },
  { "ebcdic",    CONF_FIX, SUPPORT_EBCDIC },
  { "ebcdic-nl", CONF_FIZ, EBCDIC_NL },
  { "jit",       CONF_INT, PCRE2_CONFIG_JIT },
  { "linksize",  CONF_INT, PCRE2_CONFIG_LINKSIZE },
  { "newline",   CONF_NL,  PCRE2_CONFIG_NEWLINE },
  { "pcre16",    CONF_FIX, SUPPORT_16 },
  { "pcre32",    CONF_FIX, SUPPORT_32 },
  { "pcre8",     CONF_FIX, SUPPORT_8 },
  { "utf",       CONF_INT, PCRE2_CONFIG_UTF }
};

#define COPTLISTCOUNT sizeof(coptlist)/sizeof(coptstruct)

#undef SUPPORT_8
#undef SUPPORT_16
#undef SUPPORT_32
#undef SUPPORT_EBCDIC


/* ----------------------- Static variables ------------------------ */

static FILE *infile;
static FILE *outfile;

static const void *last_callout_mark;

static BOOL first_callout;
static BOOL restrict_for_perl_test = FALSE;
static BOOL show_memory = FALSE;

static int code_unit_size;                    /* Bytes */
static int test_mode = DEFAULT_TEST_MODE;
static int timeit = 0;
static int timeitm = 0;

clock_t total_compile_time = 0;
clock_t total_match_time = 0;

static uint32_t dfa_matched;
static uint32_t max_oveccount;
static uint32_t callout_count;

static VERSION_TYPE version[VERSION_SIZE];

static patctl def_patctl;
static patctl pat_patctl;
static datctl def_datctl;
static datctl dat_datctl;

static regex_t preg = { NULL, NULL, 0, 0 };

static int *dfa_workspace = NULL;
static const uint8_t *locale_tables = NULL;
static uint8_t locale_name[32];

/* We need buffers for building 16/32-bit strings; 8-bit strings don't need
rebuilding, but set up the same naming scheme for use in macros. The "buffer"
buffer is where all input lines are read. Its size is the same as pbuffer8.
Pattern lines are always copied to pbuffer8 for use in callouts, even if they
are actually compiled from pbuffer16 or pbuffer32. */

static int       pbuffer8_size  = 50000;        /* Initial size, bytes */
static uint8_t  *pbuffer8 = NULL;
static uint8_t  *buffer = NULL;

/* The dbuffer is where all processed data lines are put. In non-8-bit modes it
is cast as needed. For long data lines it grows as necessary. */

static size_t dbuffer_size = 1u << 14;    /* Initial size, bytes */
static uint8_t *dbuffer = NULL;


/* ---------------- Mode-dependent variables -------------------*/

#ifdef SUPPORT_PCRE8
static pcre2_code_8             *compiled_code8;
static pcre2_general_context_8  *general_context8;
static pcre2_compile_context_8  *pat_context8, *default_pat_context8;
static pcre2_match_context_8    *dat_context8, *default_dat_context8;
static pcre2_match_data_8       *match_data8;
#endif

#ifdef SUPPORT_PCRE16
static pcre2_code_16            *compiled_code16;
static pcre2_general_context_16 *general_context16;
static pcre2_compile_context_16 *pat_context16, *default_pat_context16;
static pcre2_match_context_16   *dat_context16, *default_dat_context16;
static pcre2_match_data_16      *match_data16;
static int pbuffer16_size = 0;        /* Set only when needed */
static uint16_t *pbuffer16 = NULL;
#endif

#ifdef SUPPORT_PCRE32
static pcre2_code_32            *compiled_code32;
static pcre2_general_context_32 *general_context32;
static pcre2_compile_context_32 *pat_context32, *default_pat_context32;
static pcre2_match_context_32   *dat_context32, *default_dat_context32;
static pcre2_match_data_32      *match_data32;
static int pbuffer32_size = 0;        /* Set only when needed */
static uint32_t *pbuffer32 = NULL;
#endif


/* ---------------- Macros that work in all modes ----------------- */

#define CAST8VAR(x) CASTVAR(uint8_t *, x)
#define SET(x,y) SETOP(x,y,=)
#define SETPLUS(x,y) SETOP(x,y,+=)


/* ---------------- Mode-dependent, runtime-testing macros ------------------*/

/* Define macros for variables and functions that must be selected dynamically
depending on the mode setting (8, 16, 32). These are dependent on which modes
are supported. */

#if (defined (SUPPORT_PCRE8) + defined (SUPPORT_PCRE16) + \
     defined (SUPPORT_PCRE32)) >= 2

/* ----- All three modes supported ----- */

#if defined(SUPPORT_PCRE8) && defined(SUPPORT_PCRE16) && defined(SUPPORT_PCRE32)

#define CASTFLD(t,a,b) ((test_mode == PCRE8_MODE)? (t)(G(a,8)->b) : \
  (test_mode == PCRE16_MODE)? (t)(G(a,16)->b) : (t)(G(a,32)->b))

#define CASTVAR(t,x) ( \
  (test_mode == PCRE8_MODE)? (t)G(x,8) : \
  (test_mode == PCRE16_MODE)? (t)G(x,16) : (t)G(x,32))

#define CODE_UNIT(a,b) ( \
  (test_mode == PCRE8_MODE)? (uint32_t)(((PCRE2_SPTR8)(a))[b]) : \
  (test_mode == PCRE16_MODE)? (uint32_t)(((PCRE2_SPTR16)(a))[b]) : \
  (uint32_t)(((PCRE2_SPTR32)(a))[b]))

#define DATCTXCPY(a,b) \
  if (test_mode == PCRE8_MODE) \
    memcpy(G(a,8),G(b,8),sizeof(pcre2_match_context_8)); \
  else if (test_mode == PCRE16_MODE) \
    memcpy(G(a,16),G(b,16),sizeof(pcre2_match_context_16)); \
  else memcpy(G(a,32),G(b,32),sizeof(pcre2_match_context_32))

#define FLD(a,b) ((test_mode == PCRE8_MODE)? G(a,8)->b : \
  (test_mode == PCRE16_MODE)? G(a,16)->b : G(a,32)->b)

#define PATCTXCPY(a,b) \
  if (test_mode == PCRE8_MODE) \
    memcpy(G(a,8),G(b,8),sizeof(pcre2_compile_context_8)); \
  else if (test_mode == PCRE16_MODE) \
    memcpy(G(a,16),G(b,16),sizeof(pcre2_compile_context_16)); \
  else memcpy(G(a,32),G(b,32),sizeof(pcre2_compile_context_32))

#define PCHARS(lv, p, offset, len, utf, f) \
  if (test_mode == PCRE32_MODE) \
    lv = pchars32((PCRE2_SPTR32)(p)+offset, len, utf, f); \
  else if (test_mode == PCRE16_MODE) \
    lv = pchars16((PCRE2_SPTR16)(p)+offset, len, utf, f); \
  else \
    lv = pchars8((PCRE2_SPTR8)(p)+offset, len, utf, f)

#define PCHARSV(p, offset, len, utf, f) \
  if (test_mode == PCRE32_MODE) \
    (void)pchars32((PCRE2_SPTR32)(p)+offset, len, utf, f); \
  else if (test_mode == PCRE16_MODE) \
    (void)pchars16((PCRE2_SPTR16)(p)+offset, len, utf, f); \
  else \
    (void)pchars8((PCRE2_SPTR8)(p)+offset, len, utf, f)

#define PCRE2_COMPILE(a,b,c,d,e,f,g) \
  if (test_mode == PCRE8_MODE) \
    G(a,8) = pcre2_compile_8(G(b,8),c,d,e,f,G(g,8)); \
  else if (test_mode == PCRE16_MODE) \
    G(a,16) = pcre2_compile_16(G(b,16),c,d,e,f,G(g,16)); \
  else \
    G(a,32) = pcre2_compile_32(G(b,32),c,d,e,f,G(g,32))

#define PCRE2_DFA_MATCH(a,b,c,d,e,f,g,h,i,j) \
  if (test_mode == PCRE8_MODE) \
    a = pcre2_dfa_match_8(G(b,8),(PCRE2_SPTR8)c,d,e,f,G(g,8),G(h,8),i,j); \
  else if (test_mode == PCRE16_MODE) \
    a = pcre2_dfa_match_16(G(b,16),(PCRE2_SPTR16)c,d,e,f,G(g,16),G(h,16),i,j); \
  else \
    a = pcre2_dfa_match_32(G(b,32),(PCRE2_SPTR32)c,d,e,f,G(g,32),G(h,32),i,j)

#define PCRE2_GET_ERROR_MESSAGE(r,a,b) \
  if (test_mode == PCRE8_MODE) \
    r = pcre2_get_error_message_8(a,G(b,8),G(G(b,8),_size)); \
  else if (test_mode == PCRE16_MODE) \
    r = pcre2_get_error_message_16(a,G(b,16),G(G(b,16),_size)); \
  else \
    r = pcre2_get_error_message_32(a,G(b,32),G(G(b,32),_size))

#define PCRE2_JIT_COMPILE(a,b) \
  if (test_mode == PCRE8_MODE) pcre2_jit_compile_8(G(a,8),b); \
  else if (test_mode == PCRE16_MODE) pcre2_jit_compile_16(G(a,16),b); \
  else pcre2_jit_compile_32(G(a,32),b)

#define PCRE2_MAKETABLES(a) \
  if (test_mode == PCRE8_MODE) a = pcre2_maketables_8(NULL); \
  else if (test_mode == PCRE16_MODE) a = pcre2_maketables_16(NULL); \
  else a = pcre2_maketables_32(NULL)

#define PCRE2_MATCH(a,b,c,d,e,f,g,h) \
  if (test_mode == PCRE8_MODE) \
    a = pcre2_match_8(G(b,8),(PCRE2_SPTR8)c,d,e,f,G(g,8),G(h,8)); \
  else if (test_mode == PCRE16_MODE) \
    a = pcre2_match_16(G(b,16),(PCRE2_SPTR16)c,d,e,f,G(g,16),G(h,16)); \
  else \
    a = pcre2_match_32(G(b,32),(PCRE2_SPTR32)c,d,e,f,G(g,32),G(h,32))

#define PCRE2_MATCH_DATA_CREATE(a,b,c) \
  if (test_mode == PCRE8_MODE) \
    G(a,8) = pcre2_match_data_create_8(b,c); \
  else if (test_mode == PCRE16_MODE) \
    G(a,16) = pcre2_match_data_create_16(b,c); \
  else \
    G(a,32) = pcre2_match_data_create_32(b,c)

#define PCRE2_MATCH_DATA_FREE(a) \
  if (test_mode == PCRE8_MODE) \
    pcre2_match_data_free_8(G(a,8)); \
  else if (test_mode == PCRE16_MODE) \
    pcre2_match_data_free_16(G(a,16)); \
  else \
    pcre2_match_data_free_32(G(a,32))

#define PCRE2_PATTERN_INFO(a,b,c,d) \
  if (test_mode == PCRE8_MODE) \
    a = pcre2_pattern_info_8(G(b,8),c,d); \
  else if (test_mode == PCRE16_MODE) \
    a = pcre2_pattern_info_16(G(b,16),c,d); \
  else \
    a = pcre2_pattern_info_32(G(b,32),c,d)

#define PCRE2_PRINTINT(a) \
  if (test_mode == PCRE8_MODE) \
    pcre2_printint_8(compiled_code8,outfile,a); \
  else if (test_mode == PCRE16_MODE) \
    pcre2_printint_16(compiled_code16,outfile,a); \
  else \
    pcre2_printint_32(compiled_code32,outfile,a)

#define PCRE2_SET_CALLOUT(a,b,c) \
  if (test_mode == PCRE8_MODE) \
    pcre2_set_callout_8(G(a,8),(int (*)(pcre2_callout_block_8 *))b,c); \
  else if (test_mode == PCRE16_MODE) \
    pcre2_set_callout_16(G(a,16),(int (*)(pcre2_callout_block_16 *))b,c); \
  else \
    pcre2_set_callout_32(G(a,32),(int (*)(pcre2_callout_block_32 *))b,c);

#define PCRE2_SET_CHARACTER_TABLES(a,b) \
  if (test_mode == PCRE8_MODE) \
    pcre2_set_character_tables_8(G(a,8),b); \
  else if (test_mode == PCRE16_MODE) \
    pcre2_set_character_tables_16(G(a,16),b); \
  else \
    pcre2_set_character_tables_32(G(a,32),b)

#define PCRE2_SET_MATCH_LIMIT(a,b) \
  if (test_mode == PCRE8_MODE) \
    pcre2_set_match_limit_8(G(a,8),b); \
  else if (test_mode == PCRE16_MODE) \
    pcre2_set_match_limit_16(G(a,16),b); \
  else \
    pcre2_set_match_limit_32(G(a,32),b)

#define PCRE2_SET_RECURSION_LIMIT(a,b) \
  if (test_mode == PCRE8_MODE) \
    pcre2_set_recursion_limit_8(G(a,8),b); \
  else if (test_mode == PCRE16_MODE) \
    pcre2_set_recursion_limit_16(G(a,16),b); \
  else \
    pcre2_set_recursion_limit_32(G(a,32),b)

#define PCRE2_SUBSTRING_COPY_BYNAME(a,b,c,d,e) \
  if (test_mode == PCRE8_MODE) \
    a = pcre2_substring_copy_byname_8(G(b,8),G(c,8),(PCRE2_UCHAR8 *)d,e); \
  else if (test_mode == PCRE16_MODE) \
    a = pcre2_substring_copy_byname_16(G(b,16),G(c,16),(PCRE2_UCHAR16 *)d,e); \
  else \
    a = pcre2_substring_copy_byname_32(G(b,32),G(c,32),(PCRE2_UCHAR32 *)d,e)

#define PCRE2_SUBSTRING_COPY_BYNUMBER(a,b,c,d,e) \
  if (test_mode == PCRE8_MODE) \
    a = pcre2_substring_copy_bynumber_8(G(b,8),c,(PCRE2_UCHAR8 *)d,e); \
  else if (test_mode == PCRE16_MODE) \
    a = pcre2_substring_copy_bynumber_16(G(b,16),c,(PCRE2_UCHAR16 *)d,e); \
  else \
    a = pcre2_substring_copy_bynumber_32(G(b,32),c,(PCRE2_UCHAR32 *)d,e)

#define PCRE2_SUBSTRING_FREE(a) \
  if (test_mode == PCRE8_MODE) pcre2_substring_free_8((PCRE2_UCHAR8 *)a); \
  else if (test_mode == PCRE16_MODE) \
    pcre2_substring_free_16((PCRE2_UCHAR16 *)a); \
  else pcre2_substring_free_32((PCRE2_UCHAR32 *)a)

#define PCRE2_SUBSTRING_GET_BYNAME(a,b,c,d) \
  if (test_mode == PCRE8_MODE) \
    a = pcre2_substring_get_byname_8(G(b,8),G(c,8),(PCRE2_UCHAR8 **)d); \
  else if (test_mode == PCRE16_MODE) \
    a = pcre2_substring_get_byname_16(G(b,16),G(c,16),(PCRE2_UCHAR16 **)d); \
  else \
    a = pcre2_substring_get_byname_32(G(b,32),G(c,32),(PCRE2_UCHAR32 **)d)

#define PCRE2_SUBSTRING_GET_BYNUMBER(a,b,c,d) \
  if (test_mode == PCRE8_MODE) \
    a = pcre2_substring_get_bynumber_8(G(b,8),c,(PCRE2_UCHAR8 **)d); \
  else if (test_mode == PCRE16_MODE) \
    a = pcre2_substring_get_bynumber_16(G(b,16),c,(PCRE2_UCHAR16 **)d); \
  else \
    a = pcre2_substring_get_bynumber_32(G(b,32),c,(PCRE2_UCHAR32 **)d)

#define PCRE2_SUBSTRING_LIST_GET(a,b,c,d) \
  if (test_mode == PCRE8_MODE) \
    a = pcre2_substring_list_get_8(G(b,8),(PCRE2_UCHAR8 ***)c,d); \
  else if (test_mode == PCRE16_MODE) \
    a = pcre2_substring_list_get_16(G(b,16),(PCRE2_UCHAR16 ***)c,d); \
  else \
    a = pcre2_substring_list_get_32(G(b,32),(PCRE2_UCHAR32 ***)c,d)

#define PCRE2_SUBSTRING_LIST_FREE(a) \
  if (test_mode == PCRE8_MODE) \
    pcre2_substring_list_free_8((PCRE2_SPTR8 *)a); \
  else if (test_mode == PCRE16_MODE) \
    pcre2_substring_list_free_16((PCRE2_SPTR16 *)a); \
  else \
    pcre2_substring_list_free_32((PCRE2_SPTR32 *)a)

#define PTR(x) ( \
  (test_mode == PCRE8_MODE)? (void *)G(x,8) : \
  (test_mode == PCRE16_MODE)? (void *)G(x,16) : \
  (void *)G(x,32))

#define SETFLD(x,y,z) \
  if (test_mode == PCRE8_MODE) G(x,8)->y = z; \
  else if (test_mode == PCRE16_MODE) G(x,16)->y = z; \
  else G(x,32)->y = z

#define SETFLDVEC(x,y,v,z) \
  if (test_mode == PCRE8_MODE) G(x,8)->y[v] = z; \
  else if (test_mode == PCRE16_MODE) G(x,16)->y[v] = z; \
  else G(x,32)->y[v] = z

#define SETOP(x,y,z) \
  if (test_mode == PCRE8_MODE) G(x,8) z y; \
  else if (test_mode == PCRE16_MODE) G(x,16) z y; \
  else G(x,32) z y

#define SETCASTPTR(x,y) \
  if (test_mode == PCRE8_MODE) \
    G(x,8) = (uint8_t *)y; \
  else if (test_mode == PCRE16_MODE) \
    G(x,16) = (uint16_t *)y; \
  else \
    G(x,32) = (uint32_t *)y

#define STRLEN(p) ((test_mode == PCRE8_MODE)? ((int)strlen((char *)p)) : \
  (test_mode == PCRE16_MODE)? ((int)strlen16((PCRE2_SPTR16)p)) : \
  ((int)strlen32((PCRE2_SPTR32)p)))

#define SUB1(a,b) \
  if (test_mode == PCRE8_MODE) G(a,8)(G(b,8)); \
  else if (test_mode == PCRE16_MODE) G(a,16)(G(b,16)); \
  else G(a,32)(G(b,32))

#define SUB2(a,b,c) \
  if (test_mode == PCRE8_MODE) G(a,8)(G(b,8),G(c,8)); \
  else if (test_mode == PCRE16_MODE) G(a,16)(G(b,16),G(c,16)); \
  else G(a,32)(G(b,32),G(c,32))

#define TEST(x,r,y) ( \
  (test_mode == PCRE8_MODE && G(x,8) r (y)) || \
  (test_mode == PCRE16_MODE && G(x,16) r (y)) || \
  (test_mode == PCRE32_MODE && G(x,32) r (y)))

#define TESTFLD(x,f,r,y) ( \
  (test_mode == PCRE8_MODE && G(x,8)->f r (y)) || \
  (test_mode == PCRE16_MODE && G(x,16)->f r (y)) || \
  (test_mode == PCRE32_MODE && G(x,32)->f r (y)))



/* ----- Two out of three modes are supported ----- */

#else

/* We can use some macro trickery to make a single set of definitions work in
the three different cases. */

/* ----- 32-bit and 16-bit but not 8-bit supported ----- */

#if defined(SUPPORT_PCRE32) && defined(SUPPORT_PCRE16)
#define BITONE 32
#define BITTWO 16

/* ----- 32-bit and 8-bit but not 16-bit supported ----- */

#elif defined(SUPPORT_PCRE32) && defined(SUPPORT_PCRE8)
#define BITONE 32
#define BITTWO 8

/* ----- 16-bit and 8-bit but not 32-bit supported ----- */

#else
#define BITONE 16
#define BITTWO 8
#endif


/* ----- Common macros for two-mode cases ----- */

#define CASTFLD(t,a,b) \
  ((test_mode == G(G(PCRE,BITONE),_MODE))? (t)(G(a,BITONE)->b) : \
    (t)(G(a,BITTWO)->b))

#define CASTVAR(t,x) ( \
  (test_mode == G(G(PCRE,BITONE(,_MODE))? \
    (t)G(x,BITONE) : (t)G(x,BITTWO))

#define CODE_UNIT(a,b) ( \
  (test_mode == G(G(PCRE,BITONE(,_MODE))? \
  (uint32_t)(((G(PCRE2_SPTR,BITONE))(a))[b]) : \
  (uint32_t)(((G(PCRE2_SPTR,BITTWO))(a))[b]))

#define DATCTXCPY(a,b) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    memcpy(G(a,BITONE),G(b,BITONE),sizeof(G(pcre2_match_context_,BITONE))); \
  else \
    memcpy(G(a,BITTWO),G(b,BITTWO),sizeof(G(pcre2_match_context_,BITTWO)))

#define FLD(a,b) \
  ((test_mode == G(G(PCRE,BITONE),_MODE))? G(a,BITONE)->b : G(a,BITTWO)->b)

#define PATCTXCPY(a,b) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    memcpy(G(a,BITONE),G(b,BITONE),sizeof(G(pcre2_compile_context_,BITONE))); \
  else \
    memcpy(G(a,BITTWO),G(b,BITTWO),sizeof(G(pcre2_compile_context_,BITTWO)))

#define PCHARS(lv, p, offset, len, utf, f) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    lv = G(pchars,BITONE)((G(PCRE2_SPTR,BITONE))(p)+offset, len, utf, f); \
  else \
    lv = G(PCHARS,BITTWO)((G(PCRE2_SPTR,BITTWO))(p)+offset, len, utf, f)

#define PCHARSV(p, offset, len, utf, f) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    (void)G(pchars,BITONE)((G(PCRE2_SPTR,BITONE))(p)+offset, len, utf, f); \
  else \
    (void)G(PCHARS,BITTWO)((G(PCRE2_SPTR,BITTWO))(p)+offset, len, utf, f)

#define PCRE2_COMPILE(a,b,c,d,e,f,g) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(a,BITONE) = G(pcre2_compile_,BITONE)(G(b,BITONE),c,d,e,f,G(g,BITONE)); \
  else \
    G(a,BITTWO) = G(pcre2_compile_,BITTWO)(G(b,BITTWO),c,d,e,f,G(g,BITTWO))

#define PCRE2_DFA_MATCH(a,b,c,d,e,f,g,h,i,j) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    a = G(pcre2_dfa_match_,BITONE)(G(b,BITONE),(G(PCRE2_SPTR,BITONE))c,d,e,f, \
      G(g,BITONE),G(h,BITONE),i,j); \
  else \
    a = G(pcre2_dfa_match_,BITTWO)(G(b,BITTWO),(G(PCRE2_SPTR,BITTWO))c,d,e,f, \
      G(g,BITTWO),G(h,BITTWO),i,j)

#define PCRE2_GET_ERROR_MESSAGE(r,a,b) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    r = G(pcre2_get_error_message_,BITONE)(a,G(b,BITONE),G(G(b,BITONE),_size)); \
  else \
    r = G(pcre2_get_error_message_,BITTWO)(a,G(b,BITTWO),G(G(b,BITTWO),_size))

#define PCRE2_JIT_COMPILE(a,b) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(pcre2_jit_compile_,BITONE)(G(a,BITONE),b); \
  else \
    G(pcre2_jit_compile_,BITTWO)(G(a,BITTWO),b)

#define PCRE2_MAKETABLES(a) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    a = G(pcre2_maketables_,BITONE)(NULL); \
  else \
    a = G(pcre2_maketables_,BITTWO)(NULL)

#define PCRE2_MATCH(a,b,c,d,e,f,g,h) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    a = G(pcre2_match_,BITONE)(G(b,BITONE),(G(PCRE2_SPTR,BITONE))c,d,e,f, \
      G(g,BITONE),G(h,BITONE)); \
  else \
    a = G(pcre2_match_,BITTWO)(G(b,BITTWO),(G(PCRE2_SPTR,BITTWO))c,d,e,f, \
      G(g,BITTWO),G(h,BITTWO))

#define PCRE2_MATCH_DATA_CREATE(a,b,c) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(a,BITONE) = G(pcre2_match_data_create_,BITONE)(b,c); \
  else \
    G(a,BITTWO) = G(pcre2_match_data_create_,BITTWO)(b,c)

#define PCRE2_MATCH_DATA_FREE(a) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(pcre2_match_data_free_,BITONE)(G(a,BITONE)); \
  else \
    G(pcre2_match_data_free_,BITTWO)(G(a,BITTWO))

#define PCRE2_PATTERN_INFO(a,b,c,d) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    a = G(pcre2_pattern_info_,BITONE)(G(b,BITONE),c,d); \
  else \
    a = G(pcre2_pattern_info_,BITTWO)(G(b,BITTWO),c,d)

#define PCRE2_PRINTINT(a) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(pcre2_printint_,BITONE)(G(compiled_code,BITONE),outfile,a); \
  else \
    G(pcre2_printint_,BITTWO)(G(compiled_code,BITTWO),outfile,a)

#define PCRE2_SET_CALLOUT(a,b,c) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(pcre2_set_callout_,BITONE)(G(a,BITONE), \
      (int (*)(G(pcre2_callout_block_BITONE) *))b,c); \
  else \
    G(pcre2_set_callout_,BITTWO)(G(a,BITTWO), \
      (int (*)(G(pcre2_callout_block_BITTWO) *))b,c);

#define PCRE2_SET_CHARACTER_TABLES(a,b) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(pcre2_set_character_tables_,BITONE)(G(a,BITONE),b); \
  else \
    G(pcre2_set_character_tables_,BITTWO)(G(a,BITTWO),b)

#define PCRE2_SET_MATCH_LIMIT(a,b) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(pcre2_set_match_limit_,BITONE)(G(a,BITONE),b); \
  else \
    G(pcre2_set_match_limit_,BITTWO)(G(a,BITTWO),b)

#define PCRE2_SET_RECURSION_LIMIT(a,b) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(pcre2_set_recursion_limit_,BITONE)(G(a,BITONE),b); \
  else \
    G(pcre2_set_recursion_limit_,BITTWO)(G(a,BITTWO),b)

#define PCRE2_SUBSTRING_COPY_BYNAME(a,b,c,d,e) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    a = G(pcre2_substring_copy_bynumber_,BITONE)(G(b,BITONE),G(c,BITONE),\
      (G(PCRE2_UCHAR,BITONE) *)d,e); \
  else \
    a = G(pcre2_substring_copy_bynumber_,BITTWO)(G(b,BITTWO),G(c,BITTWO),\
      (G(PCRE2_UCHAR,BITTWO) *)d,e)

#define PCRE2_SUBSTRING_COPY_BYNUMBER(a,b,c,d,e) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    a = G(pcre2_substring_copy_bynumber_,BITONE)(G(b,BITONE),c,\
      (G(PCRE2_UCHAR,BITONE) *)d,e); \
  else \
    a = G(pcre2_substring_copy_bynumber_,BITTWO)(G(b,BITTWO),c,\
      (G(PCRE2_UCHAR,BITTWO) *)d,e)

#define PCRE2_SUBSTRING_FREE(a) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(pcre2_substring_free_,BITONE)((G(PCRE2_UCHAR,BITONE) *)a); \
  else G(pcre2_substring_free_,BITTWO)((G(PCRE2_UCHAR,BITTWO) *)a)

#define PCRE2_SUBSTRING_GET_BYNAME(a,b,c,d) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    a = G(pcre2_substring_get_byname_,BITONE)(G(b,BITONE),G(c,BITONE),\
      (G(PCRE2_UCHAR,BITONE) **)d); \
  else \
    a = G(pcre2_substring_get_byname_,BITTWO)(G(b,BITTWO),G(c,BITTWO),\
      (G(PCRE2_UCHAR,BITTWO) **)d)

#define PCRE2_SUBSTRING_GET_BYNUMBER(a,b,c,d) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    a = G(pcre2_substring_get_bynumber_,BITONE)(G(b,BITONE),c,\
      (G(PCRE2_UCHAR,BITONE) **)d); \
  else \
    a = G(pcre2_substring_get_bynumber_,BITTWO)(G(b,BITTWO),c,\
      (G(PCRE2_UCHAR,BITTWO) **)d)

#define PCRE2_SUBSTRING_LIST_GET(a,b,c,d) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    a = G(pcre2_substring_list_get_,BITONE)(G(b,BITONE), \
      (G(PCRE2_UCHAR,BITONE) ***)c,d); \
  else \
    a = G(pcre2_substring_list_get_,BITTWO)(G(b,BITTWO), \
      (G(PCRE2_UCHAR,BITTWO) ***)c,d)

#define PCRE2_SUBSTRING_LIST_FREE(a) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(pcre2_substring_list_free_,BITONE)((G(PCRE2_SPTR,BITONE) *)a); \
  else \
    G(pcre2_substring_list_free_,BITTWO)((G(PCRE2_SPTR,BITTWO) *)a)

#define PTR(x) ( \
  (test_mode == G(G(PCRE,BITONE),_MODE))? (void *)G(x,BITONE) : \
  (void *)G(x,BITTWO))

#define SETFLD(x,y,z) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) G(x,BITONE)->y = z; \
  else G(x,BITTWO)->y = z

#define SETFLDVEC(x,y,v,z) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) G(x,BITONE)->y[v] = z; \
  else G(x,BITTWO)->y[v] = z

#define SETOP(x,y,z) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) G(x,BITONE) z y; \
  else G(x,BITTWO) z y

#define SETCASTPTR(x,y) \
  if (test_mode == PCRE8_MODE) \
    G(x,BITONE) = (G(G(uint,BITONE),_t) *)y; \
  else \
    G(x,BITTWO) = (G(G(uint,BITTWO),_t) *)y

#define STRLEN(p) ((test_mode == G(G(PCRE,BITONE),_MODE))? \
  G(strlen,BITONE)((G(PCRE2_SPTR,BITONE))p)) : \
  G(strlen(BITTWO)((G(PCRE2_SPTR,BITTWO))p)))

#define SUB1(a,b) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(a,BITONE))(G(b,BITONE)); \
  else \
    G(a,BITTWO))(G(b,BITTWO))

#define SUB2(a,b,c) \
  if (test_mode == G(G(PCRE,BITONE),_MODE)) \
    G(a,BITONE))(G(b,BITONE),G(c,BITONE)); \
  else \
    G(a,BITTWO))(G(b,BITTWO),G(c,BITTWO))

#define TEST(x,r,y) ( \
  (test_mode == G(G(PCRE,BITONE),_MODE) && G(x,BITONE) r (y)) || \
  (test_mode == G(G(PCRE,BITTWO),_MODE) && G(x,BITTWO) r (y)))

#define TESTFLD(x,f,r,y) ( \
  (test_mode == G(G(PCRE,BITONE),_MODE) && G(x,BITONE)->f r (y)) || \
  (test_mode == G(G(PCRE,BITTWO),_MODE) && G(x,BITTWO)->f r (y)))


#endif  /* Two out of three modes */

/* ----- End of cases where more than one mode is supported ----- */


/* ----- Only 8-bit mode is supported ----- */

#elif defined SUPPORT_PCRE8
#define CASTFLD(t,a,b) (t)(G(a,8)->b)
#define CASTVAR(t,x) (t)G(x,8)
#define CODE_UNIT(a,b) (uint32_t)(((PCRE2_SPTR8)(a))[b])
#define DATCTXCPY(a,b) memcpy(G(a,8),G(b,8),sizeof(pcre2_match_context_8))
#define FLD(a,b) G(a,8)->b
#define PATCTXCPY(a,b) memcpy(G(a,8),G(b,8),sizeof(pcre2_compile_context_8))
#define PCHARS(lv, p, offset, len, utf, f) \
  lv = pchars8((PCRE2_SPTR8)(p)+offset, len, utf, f)
#define PCHARSV(p, offset, len, utf, f) \
  (void)pchars8((PCRE2_SPTR8)(p)+offset, len, utf, f)
#define PCRE2_COMPILE(a,b,c,d,e,f,g) \
  G(a,8) = pcre2_compile_8(G(b,8),c,d,e,f,G(g,8))
#define PCRE2_DFA_MATCH(a,b,c,d,e,f,g,h,i,j) \
  a = pcre2_dfa_match_8(G(b,8),(PCRE2_SPTR8)c,d,e,f,G(g,8),G(h,8),i,j)
#define PCRE2_GET_ERROR_MESSAGE(r,a,b) \
  r = pcre2_get_error_message_8(a,G(b,8),G(G(b,8),_size))
#define PCRE2_JIT_COMPILE(a,b) pcre2_jit_compile_8(G(a,8),b)
#define PCRE2_MATCH(a,b,c,d,e,f,g,h) \
  a = pcre2_match_8(G(b,8),(PCRE2_SPTR8)c,d,e,f,G(g,8),G(h,8))
#define PCRE2_MAKETABLES(a) a = pcre2_maketables_8(NULL)
#define PCRE2_MATCH_DATA_CREATE(a,b,c) G(a,8) = pcre2_match_data_create_8(b,c)
#define PCRE2_MATCH_DATA_FREE(a) pcre2_match_data_free_8(G(a,8))
#define PCRE2_PATTERN_INFO(a,b,c,d) a = pcre2_pattern_info_8(G(b,8),c,d)
#define PCRE2_PRINTINT(a) pcre2_printint_8(compiled_code8,outfile,a)
#define PCRE2_SET_CALLOUT(a,b,c) \
  pcre2_set_callout_8(G(a,8),(int (*)(pcre2_callout_block_8 *))b,c);
#define PCRE2_SET_CHARACTER_TABLES(a,b) pcre2_set_character_tables_8(G(a,8),b)
#define PCRE2_SET_MATCH_LIMIT(a,b) pcre2_set_match_limit_8(G(a,8),b)
#define PCRE2_SET_RECURSION_LIMIT(a,b) pcre2_set_recursion_limit_8(G(a,8),b)
#define PCRE2_SUBSTRING_COPY_BYNAME(a,b,c,d,e) \
  a = pcre2_substring_copy_byname_8(G(b,8),G(c,8),(PCRE2_UCHAR8 *)d,e)
#define PCRE2_SUBSTRING_COPY_BYNUMBER(a,b,c,d,e) \
  a = pcre2_substring_copy_bynumber_8(G(b,8),c,(PCRE2_UCHAR8 *)d,e)
#define PCRE2_SUBSTRING_FREE(a) pcre2_substring_free_8((PCRE2_UCHAR8 *)a)
#define PCRE2_SUBSTRING_GET_BYNAME(a,b,c,d) \
  a = pcre2_substring_get_byname_8(G(b,8),G(c,8),(PCRE2_UCHAR8 **)d)
#define PCRE2_SUBSTRING_GET_BYNUMBER(a,b,c,d) \
  a = pcre2_substring_get_bynumber_8(G(b,8),c,(PCRE2_UCHAR8 **)d)
#define PCRE2_SUBSTRING_LIST_GET(a,b,c,d) \
  a = pcre2_substring_list_get_8(G(b,8),(PCRE2_UCHAR8 ***)c,d)
#define PCRE2_SUBSTRING_LIST_FREE(a) \
  pcre2_substring_list_free_8((PCRE2_SPTR8 *)a)
#define PTR(x) (void *)G(x,8)
#define SETFLD(x,y,z) G(x,8)->y = z
#define SETFLDVEC(x,y,v,z) G(x,8)->y[v] = z
#define SETOP(x,y,z) G(x,8) z y
#define SETCASTPTR(x,y) G(x,8) = (uint8_t *)y
#define STRLEN(p) (int)strlen((char *)p)
#define SUB1(a,b) G(a,8)(G(b,8))
#define SUB2(a,b,c) G(a,8)(G(b,8),G(c,8))
#define TEST(x,r,y) (G(x,8) r (y))
#define TESTFLD(x,f,r,y) (G(x,8)->f r (y))


/* ----- Only 16-bit mode is supported ----- */

#elif defined SUPPORT_PCRE16
#define CASTFLD(t,a,b) (t)(G(a,16)->b)
#define CASTVAR(t,x) (t)G(x,16)
#define CODE_UNIT(a,b) (uint32_t)(((PCRE2_SPTR16)(a))[b])
#define DATCTXCPY(a,b) memcpy(G(a,16),G(b,16),sizeof(pcre2_match_context_16))
#define FLD(a,b) G(a,16)->b
#define PATCTXCPY(a,b) memcpy(G(a,16),G(b,16),sizeof(pcre2_compile_context_16))
#define PCHARS(lv, p, offset, len, utf, f) \
  lv = pchars16((PCRE2_SPTR16)(p)+offset, len, utf, f)
#define PCHARSV(p, offset, len, utf, f) \
  (void)pchars16((PCRE2_SPTR16)(p)+offset, len, utf, f)
#define PCRE2_COMPILE(a,b,c,d,e,f,g) \
  G(a,16) = pcre2_compile_16(G(b,16),c,d,e,f,G(g,16))
#define PCRE2_DFA_MATCH(a,b,c,d,e,f,g,h,i,j) \
  a = pcre2_dfa_match_16(G(b,16),(PCRE2_SPTR16)c,d,e,f,G(g,16),G(h,16),i,j)
#define PCRE2_GET_ERROR_MESSAGE(r,a,b) \
  r = pcre2_get_error_message_16(a,G(b,16),G(G(b,16),_size))
#define PCRE2_JIT_COMPILE(a,b) pcre2_jit_compile_16(G(a,16),b)
#define PCRE2_MAKETABLES(a) a = pcre2_maketables_16(NULL)
#define PCRE2_MATCH(a,b,c,d,e,f,g,h) \
  a = pcre2_match_16(G(b,16),(PCRE2_SPTR16)c,d,e,f,G(g,16),G(h,16))
#define PCRE2_MATCH_DATA_CREATE(a,b,c) G(a,16) = pcre2_match_data_create_16(b,c)
#define PCRE2_MATCH_DATA_FREE(a) pcre2_match_data_free_16(G(a,16))
#define PCRE2_PATTERN_INFO(a,b,c,d) G(a,16) = pcre2_pattern_info_16(G(b,16),c,d)
#define PCRE2_PRINTINT(a) pcre2_printint_16(compiled_code16,outfile,a)
#define PCRE2_SET_CALLOUT(a,b,c) \
  pcre2_set_callout_16(G(a,16),(int (*)(pcre2_callout_block_16 *))b,c);
#define PCRE2_SET_CHARACTER_TABLES(a,b) pcre2_set_character_tables_16(G(a,16),b)
#define PCRE2_SET_MATCH_LIMIT(a,b) pcre2_set_match_limit_16(G(a,16),b)
#define PCRE2_SET_RECURSION_LIMIT(a,b) pcre2_set_recursion_limit_16(G(a,16),b)
#define PCRE2_SUBSTRING_COPY_BYNAME(a,b,c,d,e) \
  a = pcre2_substring_copy_byname_16(G(b,16),G(c,16),(PCRE2_UCHAR16 *)d,e);
#define PCRE2_SUBSTRING_COPY_BYNUMBER(a,b,c,d,e) \
  a = pcre2_substring_copy_bynumber_16(G(b,16),c,(PCRE2_UCHAR16 *)d,e);
#define PCRE2_SUBSTRING_FREE(a) pcre2_substring_free_16((PCRE2_UCHAR16 *)a)
#define PCRE2_SUBSTRING_GET_BYNAME(a,b,c,d) \
  a = pcre2_substring_get_byname_16(G(b,16),G(c,16),(PCRE2_UCHAR16 **)d)
#define PCRE2_SUBSTRING_GET_BYNUMBER(a,b,c,d) \
  a = pcre2_substring_get_bynumber_16(G(b,16),c,(PCRE2_UCHAR16 **)d)
#define PCRE2_SUBSTRING_LIST_GET(a,b,c,d) \
  a = pcre2_substring_list_get_16(G(b,16),(PCRE2_UCHAR16 ***)c,d)
#define PCRE2_SUBSTRING_LIST_FREE(a) \
  pcre2_substring_list_free_16((PCRE2_SPTR16 *)a)
#define PTR(x) (void *)G(x,16)
#define SETFLD(x,y,z) G(x,16)->y = z
#define SETFLDVEC(x,y,v,z) G(x,16)->y[v] = z
#define SETOP(x,y,z) G(x,16) z y
#define SETCASTPTR(x,y) G(x,16) = (uint16_t *)y
#define STRLEN(p) (int)strlen16(p)
#define SUB1(a,b) G(a,16)(G(b,16))
#define SUB2(a,b,c) G(a,16)(G(b,16),G(c,16))
#define TEST(x,r,y) (G(x,16) r (y))
#define TESTFLD(x,f,r,y) (G(x,16)->f r (y))


/* ----- Only 32-bit mode is supported ----- */

#elif defined SUPPORT_PCRE32
#define CASTFLD(t,a,b) (t)(G(a,32)->b)
#define CASTVAR(t,x) (t)G(x,32)
#define CODE_UNIT(a,b) (uint32_t)(((PCRE2_SPTR32)(a))[b])
#define DATCTXCPY(a,b) memcpy(G(a,32),G(b,32),sizeof(pcre2_match_context_32))
#define FLD(a,b) G(a,32)->b
#define PATCTXCPY(a,b) memcpy(G(a,32),G(b,32),sizeof(pcre2_compile_context_32))
#define PCHARS(lv, p, offset, len, utf, f) \
  lv = pchars32((PCRE2_SPTR32)(p)+offset, len, utf, f)
#define PCHARSV(p, offset, len, utf, f) \
  (void)pchars32((PCRE2_SPTR32)(p)+offset, len, utf, f)
#define PCRE2_COMPILE(a,b,c,d,e,f,g) \
  G(a,32) = pcre2_compile_32(G(b,32),c,d,e,f,G(g,32))
#define PCRE2_DFA_MATCH(a,b,c,d,e,f,g,h,i,j) \
  a = pcre2_dfa_match_32(G(b,32),(PCRE2_SPTR32)c,d,e,f,G(g,32),G(h,32),i,j)
#define PCRE2_GET_ERROR_MESSAGE(r,a,b) \
  r = pcre2_get_error_message_32(a,G(b,32),G(G(b,32),_size))
#define PCRE2_JIT_COMPILE(a,b) pcre2_jit_compile_32(G(a,32),b)
#define PCRE2_MATCH(a,b,c,d,e,f,g,h) \
  a = pcre2_match_32(G(b,32),(PCRE2_SPTR32)c,d,e,f,G(g,32),g(h,32))
#define PCRE2_MAKETABLES(a) a = pcre2_maketables_32(NULL)
#define PCRE2_MATCH_DATA_CREATE(a,b,c) G(a,32) = pcre2_match_data_create_32(b,c)
#define PCRE2_MATCH_DATA_FREE(a) pcre2_match_data_free_32(G(a,32))
#define PCRE2_PATTERN_INFO(a,b,c,d) G(a,32) = pcre2_pattern_info_32(G(b,32),c,d)
#define PCRE2_PRINTINT(a) pcre2_printint_32(compiled_code32,outfile,a)
#define PCRE2_SET_CALLOUT(a,b,c) \
  pcre2_set_callout_32(G(a,32),(int (*)(pcre2_callout_block_32 *))b,c);
#define PCRE2_SET_CHARACTER_TABLES(a,b) pcre2_set_character_tables_32(G(a,32),b)
#define PCRE2_SET_MATCH_LIMIT(a,b) pcre2_set_match_limit_32(G(a,32),b)
#define PCRE2_SET_RECURSION_LIMIT(a,b) pcre2_set_recursion_limit_32(G(a,32),b)
#define PCRE2_SUBSTRING_COPY_BYNAME(a,b,c,d,e) \
  a = pcre2_substring_copy_byname_32(G(b,32),G(c,32),(PCRE2_UCHAR32 *)d,e);
#define PCRE2_SUBSTRING_COPY_BYNUMBER(a,b,c,d,e) \
  a = pcre2_substring_copy_bynumber_32(G(b,32),c,(PCRE2_UCHAR32 *)d,e);
#define PCRE2_SUBSTRING_FREE(a) pcre2_substring_free_32((PCRE2_UCHAR32 *)a)
##define PCRE2_SUBSTRING_GET_BYNAME(a,b,c,d) \
  a = pcre2_substring_get_byname_32(G(b,32),G(c,32),(PCRE2_UCHAR32 **)d)
define PCRE2_SUBSTRING_GET_BYNUMBER(a,b,c,d) \
  a = pcre2_substring_get_bynumber_32(G(b,32),c,(PCRE2_UCHAR32 **)d)
#define PCRE2_SUBSTRING_LIST_GET(a,b,c,d) \
  a = pcre2_substring_list_get_32(G(b,32),(PCRE2_UCHAR32 ***)c,d)
#define PCRE2_SUBSTRING_LIST_FREE(a) \
  pcre2_substring_list_free_32((PCRE2_SPTR32 *)a)
#define PTR(x) (void *)G(x,32)
#define SETFLD(x,y,z) G(x,32)->y = z
#define SETFLDVEC(x,y,v,z) G(x,32)->y[v] = z
#define SETOP(x,y,z) G(x,32) z y
#define SETCASTPTR(x,y) G(x,32) = (uint32_t *)y
#define STRLEN(p) (int)strle32(p)
#define SUB1(a,b) G(a,32)(G(b,32))
#define SUB2(a,b,c) G(a,32)(G(b,32),G(c,32))
#define TEST(x,r,y) (G(x,32) r (y))
#define TESTFLD(x,f,r,y) (G(x,32)->f r (y))

#endif

/* ----- End of mode-specific function call macros ----- */




/*************************************************
*         Alternate character tables             *
*************************************************/

/* By default, the "tables" pointer in the compile context when calling
pcre2_compile() is not set (= NULL), thereby using the default tables of the
library. However, the tables modifier can be used to select alternate sets of
tables, for different kinds of testing. Note that the locale modifier also
adjusts the tables. */

/* This is the set of tables distributed as default with PCRE2. It recognizes
only ASCII characters. */

static const uint8_t tables1[] = {

/* This table is a lower casing table. */

    0,  1,  2,  3,  4,  5,  6,  7,
    8,  9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23,
   24, 25, 26, 27, 28, 29, 30, 31,
   32, 33, 34, 35, 36, 37, 38, 39,
   40, 41, 42, 43, 44, 45, 46, 47,
   48, 49, 50, 51, 52, 53, 54, 55,
   56, 57, 58, 59, 60, 61, 62, 63,
   64, 97, 98, 99,100,101,102,103,
  104,105,106,107,108,109,110,111,
  112,113,114,115,116,117,118,119,
  120,121,122, 91, 92, 93, 94, 95,
   96, 97, 98, 99,100,101,102,103,
  104,105,106,107,108,109,110,111,
  112,113,114,115,116,117,118,119,
  120,121,122,123,124,125,126,127,
  128,129,130,131,132,133,134,135,
  136,137,138,139,140,141,142,143,
  144,145,146,147,148,149,150,151,
  152,153,154,155,156,157,158,159,
  160,161,162,163,164,165,166,167,
  168,169,170,171,172,173,174,175,
  176,177,178,179,180,181,182,183,
  184,185,186,187,188,189,190,191,
  192,193,194,195,196,197,198,199,
  200,201,202,203,204,205,206,207,
  208,209,210,211,212,213,214,215,
  216,217,218,219,220,221,222,223,
  224,225,226,227,228,229,230,231,
  232,233,234,235,236,237,238,239,
  240,241,242,243,244,245,246,247,
  248,249,250,251,252,253,254,255,

/* This table is a case flipping table. */

    0,  1,  2,  3,  4,  5,  6,  7,
    8,  9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23,
   24, 25, 26, 27, 28, 29, 30, 31,
   32, 33, 34, 35, 36, 37, 38, 39,
   40, 41, 42, 43, 44, 45, 46, 47,
   48, 49, 50, 51, 52, 53, 54, 55,
   56, 57, 58, 59, 60, 61, 62, 63,
   64, 97, 98, 99,100,101,102,103,
  104,105,106,107,108,109,110,111,
  112,113,114,115,116,117,118,119,
  120,121,122, 91, 92, 93, 94, 95,
   96, 65, 66, 67, 68, 69, 70, 71,
   72, 73, 74, 75, 76, 77, 78, 79,
   80, 81, 82, 83, 84, 85, 86, 87,
   88, 89, 90,123,124,125,126,127,
  128,129,130,131,132,133,134,135,
  136,137,138,139,140,141,142,143,
  144,145,146,147,148,149,150,151,
  152,153,154,155,156,157,158,159,
  160,161,162,163,164,165,166,167,
  168,169,170,171,172,173,174,175,
  176,177,178,179,180,181,182,183,
  184,185,186,187,188,189,190,191,
  192,193,194,195,196,197,198,199,
  200,201,202,203,204,205,206,207,
  208,209,210,211,212,213,214,215,
  216,217,218,219,220,221,222,223,
  224,225,226,227,228,229,230,231,
  232,233,234,235,236,237,238,239,
  240,241,242,243,244,245,246,247,
  248,249,250,251,252,253,254,255,

/* This table contains bit maps for various character classes. Each map is 32
bytes long and the bits run from the least significant end of each byte. The
classes that have their own maps are: space, xdigit, digit, upper, lower, word,
graph, print, punct, and cntrl. Other classes are built from combinations. */

  0x00,0x3e,0x00,0x00,0x01,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x03,
  0x7e,0x00,0x00,0x00,0x7e,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x03,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0xfe,0xff,0xff,0x07,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0xfe,0xff,0xff,0x07,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x03,
  0xfe,0xff,0xff,0x87,0xfe,0xff,0xff,0x07,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0xfe,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0xfe,0xff,0x00,0xfc,
  0x01,0x00,0x00,0xf8,0x01,0x00,0x00,0x78,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

/* This table identifies various classes of character by individual bits:
  0x01   white space character
  0x02   letter
  0x04   decimal digit
  0x08   hexadecimal digit
  0x10   alphanumeric or '_'
  0x80   regular expression metacharacter or binary zero
*/

  0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*   0-  7 */
  0x00,0x01,0x01,0x01,0x01,0x01,0x00,0x00, /*   8- 15 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*  16- 23 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*  24- 31 */
  0x01,0x00,0x00,0x00,0x80,0x00,0x00,0x00, /*    - '  */
  0x80,0x80,0x80,0x80,0x00,0x00,0x80,0x00, /*  ( - /  */
  0x1c,0x1c,0x1c,0x1c,0x1c,0x1c,0x1c,0x1c, /*  0 - 7  */
  0x1c,0x1c,0x00,0x00,0x00,0x00,0x00,0x80, /*  8 - ?  */
  0x00,0x1a,0x1a,0x1a,0x1a,0x1a,0x1a,0x12, /*  @ - G  */
  0x12,0x12,0x12,0x12,0x12,0x12,0x12,0x12, /*  H - O  */
  0x12,0x12,0x12,0x12,0x12,0x12,0x12,0x12, /*  P - W  */
  0x12,0x12,0x12,0x80,0x80,0x00,0x80,0x10, /*  X - _  */
  0x00,0x1a,0x1a,0x1a,0x1a,0x1a,0x1a,0x12, /*  ` - g  */
  0x12,0x12,0x12,0x12,0x12,0x12,0x12,0x12, /*  h - o  */
  0x12,0x12,0x12,0x12,0x12,0x12,0x12,0x12, /*  p - w  */
  0x12,0x12,0x12,0x80,0x80,0x00,0x00,0x00, /*  x -127 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 128-135 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 136-143 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 144-151 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 152-159 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 160-167 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 168-175 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 176-183 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 184-191 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 192-199 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 200-207 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 208-215 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 216-223 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 224-231 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 232-239 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 240-247 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};/* 248-255 */

/* This is a set of tables that came originally from a Windows user. It seems
to be at least an approximation of ISO 8859. In particular, there are
characters greater than 128 that are marked as spaces, letters, etc. */

static const uint8_t tables2[] = {
0,1,2,3,4,5,6,7,
8,9,10,11,12,13,14,15,
16,17,18,19,20,21,22,23,
24,25,26,27,28,29,30,31,
32,33,34,35,36,37,38,39,
40,41,42,43,44,45,46,47,
48,49,50,51,52,53,54,55,
56,57,58,59,60,61,62,63,
64,97,98,99,100,101,102,103,
104,105,106,107,108,109,110,111,
112,113,114,115,116,117,118,119,
120,121,122,91,92,93,94,95,
96,97,98,99,100,101,102,103,
104,105,106,107,108,109,110,111,
112,113,114,115,116,117,118,119,
120,121,122,123,124,125,126,127,
128,129,130,131,132,133,134,135,
136,137,138,139,140,141,142,143,
144,145,146,147,148,149,150,151,
152,153,154,155,156,157,158,159,
160,161,162,163,164,165,166,167,
168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,
184,185,186,187,188,189,190,191,
224,225,226,227,228,229,230,231,
232,233,234,235,236,237,238,239,
240,241,242,243,244,245,246,215,
248,249,250,251,252,253,254,223,
224,225,226,227,228,229,230,231,
232,233,234,235,236,237,238,239,
240,241,242,243,244,245,246,247,
248,249,250,251,252,253,254,255,
0,1,2,3,4,5,6,7,
8,9,10,11,12,13,14,15,
16,17,18,19,20,21,22,23,
24,25,26,27,28,29,30,31,
32,33,34,35,36,37,38,39,
40,41,42,43,44,45,46,47,
48,49,50,51,52,53,54,55,
56,57,58,59,60,61,62,63,
64,97,98,99,100,101,102,103,
104,105,106,107,108,109,110,111,
112,113,114,115,116,117,118,119,
120,121,122,91,92,93,94,95,
96,65,66,67,68,69,70,71,
72,73,74,75,76,77,78,79,
80,81,82,83,84,85,86,87,
88,89,90,123,124,125,126,127,
128,129,130,131,132,133,134,135,
136,137,138,139,140,141,142,143,
144,145,146,147,148,149,150,151,
152,153,154,155,156,157,158,159,
160,161,162,163,164,165,166,167,
168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,
184,185,186,187,188,189,190,191,
224,225,226,227,228,229,230,231,
232,233,234,235,236,237,238,239,
240,241,242,243,244,245,246,215,
248,249,250,251,252,253,254,223,
192,193,194,195,196,197,198,199,
200,201,202,203,204,205,206,207,
208,209,210,211,212,213,214,247,
216,217,218,219,220,221,222,255,
0,62,0,0,1,0,0,0,
0,0,0,0,0,0,0,0,
32,0,0,0,1,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,255,3,
126,0,0,0,126,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,255,3,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,12,2,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
254,255,255,7,0,0,0,0,
0,0,0,0,0,0,0,0,
255,255,127,127,0,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,254,255,255,7,
0,0,0,0,0,4,32,4,
0,0,0,128,255,255,127,255,
0,0,0,0,0,0,255,3,
254,255,255,135,254,255,255,7,
0,0,0,0,0,4,44,6,
255,255,127,255,255,255,127,255,
0,0,0,0,254,255,255,255,
255,255,255,255,255,255,255,127,
0,0,0,0,254,255,255,255,
255,255,255,255,255,255,255,255,
0,2,0,0,255,255,255,255,
255,255,255,255,255,255,255,127,
0,0,0,0,255,255,255,255,
255,255,255,255,255,255,255,255,
0,0,0,0,254,255,0,252,
1,0,0,248,1,0,0,120,
0,0,0,0,254,255,255,255,
0,0,128,0,0,0,128,0,
255,255,255,255,0,0,0,0,
0,0,0,0,0,0,0,128,
255,255,255,255,0,0,0,0,
0,0,0,0,0,0,0,0,
128,0,0,0,0,0,0,0,
0,1,1,0,1,1,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
1,0,0,0,128,0,0,0,
128,128,128,128,0,0,128,0,
28,28,28,28,28,28,28,28,
28,28,0,0,0,0,0,128,
0,26,26,26,26,26,26,18,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,18,
18,18,18,128,128,0,128,16,
0,26,26,26,26,26,26,18,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,18,
18,18,18,128,128,0,0,0,
0,0,0,0,0,1,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
1,0,0,0,0,0,0,0,
0,0,18,0,0,0,0,0,
0,0,20,20,0,18,0,0,
0,20,18,0,0,0,0,0,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,0,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,0,
18,18,18,18,18,18,18,18
};



/*************************************************
*            Local memory functions              *
*************************************************/

/* Alternative memory functions, to test functionality. */

static void *my_malloc(size_t size, void *data)
{
void *block = malloc(size);
(void)data;
if (show_memory)
  fprintf(outfile, "malloc       %3d %p\n", (int)size, block);
return block;
}

static void my_free(void *block, void *data)
{
(void)data;
if (show_memory)
  fprintf(outfile, "free             %p\n", block);
free(block);
}

/* For recursion malloc/free, to test stacking calls */

#ifdef NO_RECURSE
static void *my_stack_malloc(size_t size, void *data)
{
void *block = malloc(size);
(void)data;
if (show_memory)
  fprintf(outfile, "stack_malloc %3d %p\n", (int)size, block);
return block;
}

static void my_stack_free(void *block, void *data)
{
(void)data;
if (show_memory)
  fprintf(outfile, "stack_free       %p\n", block);
free(block);
}
#endif  /* NO_RECURSE */


/*************************************************
*      Convert UTF-8 character to code point     *
*************************************************/

/* This function reads one or more bytes that represent a UTF-8 character,
and returns the codepoint of that character. Note that the function supports
the original UTF-8 definition of RFC 2279, allowing for values in the range 0
to 0x7fffffff, up to 6 bytes long. This makes it possible to generate
codepoints greater than 0x10ffff which are useful for testing PCRE's error
checking, and also for generating 32-bit non-UTF data values above the UTF
limit.

Argument:
  utf8bytes   a pointer to the byte vector
  vptr        a pointer to an int to receive the value

Returns:      >  0 => the number of bytes consumed
              -6 to 0 => malformed UTF-8 character at offset = (-return)
*/

static int
utf82ord(PCRE2_SPTR8 utf8bytes, uint32_t *vptr)
{
uint32_t c = *utf8bytes++;
uint32_t d = c;
int i, j, s;

for (i = -1; i < 6; i++)               /* i is number of additional bytes */
  {
  if ((d & 0x80) == 0) break;
  d <<= 1;
  }

if (i == -1) { *vptr = c; return 1; }  /* ascii character */
if (i == 0 || i == 6) return 0;        /* invalid UTF-8 */

/* i now has a value in the range 1-5 */

s = 6*i;
d = (c & utf8_table3[i]) << s;

for (j = 0; j < i; j++)
  {
  c = *utf8bytes++;
  if ((c & 0xc0) != 0x80) return -(j+1);
  s -= 6;
  d |= (c & 0x3f) << s;
  }

/* Check that encoding was the correct unique one */

for (j = 0; j < utf8_table1_size; j++)
  if (d <= (uint32_t)utf8_table1[j]) break;
if (j != i) return -(i+1);

/* Valid value */

*vptr = d;
return i+1;
}



/*************************************************
*             Print one character                *
*************************************************/

/* Print a single character either literally, or as a hex escape, and count how
many printed characters are used.

Arguments:
  c            the character
  utf          TRUE in UTF mode
  f            the FILE to print to, or NULL just to count characters

Returns:       number of characters written
*/

static int
pchar(uint32_t c, BOOL utf, FILE *f)
{
int n = 0;
if (PRINTOK(c))
  {
  if (f != NULL) fprintf(f, "%c", c);
  return 1;
  }

if (c < 0x100)
  {
  if (utf)
    {
    if (f != NULL) fprintf(f, "\\x{%02x}", c);
    return 6;
    }
  else
    {
    if (f != NULL) fprintf(f, "\\x%02x", c);
    return 4;
    }
  }

if (f != NULL) n = fprintf(f, "\\x{%02x}", c);
return n >= 0 ? n : 0;
}



#ifdef SUPPORT_PCRE16
/*************************************************
*    Find length of 0-terminated 16-bit string   *
*************************************************/

static int strlen16(PCRE2_SPTR16 p)
{
PCRE2_SPTR16 pp = p;
while (*pp != 0) pp++;
return (int)(pp - p);
}
#endif  /* SUPPORT_PCRE16 */



#ifdef SUPPORT_PCRE32
/*************************************************
*    Find length of 0-terminated 32-bit string   *
*************************************************/

static int strlen32(PCRE2_SPTR32 p)
{
PCRE2_SPTR32 pp = p;
while (*pp != 0) pp++;
return (int)(pp - p);
}
#endif  /* SUPPORT_PCRE32 */


#ifdef SUPPORT_PCRE8
/*************************************************
*         Print 8-bit character string           *
*************************************************/

/* Must handle UTF-8 strings in utf8 mode. Yields number of characters printed.
If handed a NULL file, just counts chars without printing. */

static int pchars8(PCRE2_SPTR8 p, int length, BOOL utf, FILE *f)
{
uint32_t c = 0;
int yield = 0;
if (length < 0) length = strlen((char *)p);
while (length-- > 0)
  {
  if (utf)
    {
    int rc = utf82ord(p, &c);
    if (rc > 0 && rc <= length + 1)   /* Mustn't run over the end */
      {
      length -= rc - 1;
      p += rc;
      yield += pchar(c, utf, f);
      continue;
      }
    }
  c = *p++;
  yield += pchar(c, utf, f);
  }
return yield;
}
#endif


#ifdef SUPPORT_PCRE16
/*************************************************
*           Print 16-bit character string        *
*************************************************/

/* Must handle UTF-16 strings in utf mode. Yields number of characters printed.
If handed a NULL file, just counts chars without printing. */

static int pchars16(PCRE2_SPTR16 p, int length, BOOL utf, FILE *f)
{
int yield = 0;
if (length < 0) length = strlen16(p);
while (length-- > 0)
  {
  uint32_t c = *p++ & 0xffff;
  if (utf && c >= 0xD800 && c < 0xDC00 && length > 0)
    {
    int d = *p & 0xffff;
    if (d >= 0xDC00 && d <= 0xDFFF)
      {
      c = ((c & 0x3ff) << 10) + (d & 0x3ff) + 0x10000;
      length--;
      p++;
      }
    }
  yield += pchar(c, utf, f);
  }
return yield;
}
#endif  /* SUPPORT_PCRE16 */



#ifdef SUPPORT_PCRE32
/*************************************************
*           Print 32-bit character string        *
*************************************************/

/* Must handle UTF-32 strings in utf mode. Yields number of characters printed.
If handed a NULL file, just counts chars without printing. */

static int pchars32(PCRE2_SPTR32 p, int length, BOOL utf, FILE *f)
{
int yield = 0;
(void)(utf);  /* Avoid compiler warning */
if (length < 0) length = strlen32(p);
while (length-- > 0)
  {
  uint32_t c = *p++;
  yield += pchar(c, utf, f);
  }
return yield;
}
#endif  /* SUPPORT_PCRE32 */




/*************************************************
*       Convert character value to UTF-8         *
*************************************************/

/* This function takes an integer value in the range 0 - 0x7fffffff
and encodes it as a UTF-8 character in 0 to 6 bytes.

Arguments:
  cvalue     the character value
  utf8bytes  pointer to buffer for result - at least 6 bytes long

Returns:     number of characters placed in the buffer
*/

static int
ord2utf8(uint32_t cvalue, uint8_t *utf8bytes)
{
register int i, j;
if (cvalue > 0x7fffffffu)
  return -1;
for (i = 0; i < utf8_table1_size; i++)
  if (cvalue <= (uint32_t)utf8_table1[i]) break;
utf8bytes += i;
for (j = i; j > 0; j--)
 {
 *utf8bytes-- = 0x80 | (cvalue & 0x3f);
 cvalue >>= 6;
 }
*utf8bytes = utf8_table2[i] | cvalue;
return i + 1;
}



#ifdef SUPPORT_PCRE16
/*************************************************
*         Convert a string to 16-bit             *
*************************************************/

/* The input is always interpreted as a string of UTF-8 bytes. If all the input
bytes are ASCII, the space needed for a 16-bit string is exactly double the
8-bit size. Otherwise, the size needed for a 16-bit string is no more than
double, because up to 0xffff uses no more than 3 bytes in UTF-8 but possibly 4
in UTF-16. Higher values use 4 bytes in UTF-8 and up to 4 bytes in UTF-16. The
result is always left in pbuffer16. Impose a minimum size to save repeated
re-sizing.

Note that this function does not object to surrogate values. This is
deliberate; it makes it possible to construct UTF-16 strings that are invalid,
for the purpose of testing that they are correctly faulted.

Arguments:
  p          points to a byte string
  utf        non-zero if converting to UTF-16
  len        number of bytes in the string (excluding trailing zero)

Returns:     number of 16-bit data items used (excluding trailing zero)
             OR -1 if a UTF-8 string is malformed
             OR -2 if a value > 0x10ffff is encountered in UTF mode
             OR -3 if a value > 0xffff is encountered when not in UTF mode
*/

static int
to16(uint8_t *p, int utf, int len)
{
uint16_t *pp;

if (pbuffer16_size < 2*len + 2)
  {
  if (pbuffer16 != NULL) free(pbuffer16);
  pbuffer16_size = 2*len + 2;
  if (pbuffer16_size < 256) pbuffer16_size = 256;
  pbuffer16 = (uint16_t *)malloc(pbuffer16_size);
  if (pbuffer16 == NULL)
    {
    fprintf(stderr, "pcretest: malloc(%d) failed for pbuffer16\n", pbuffer16_size);
    exit(1);
    }
  }
pp = pbuffer16;

while (len > 0)
  {
  uint32_t c;
  int chlen = utf82ord(p, &c);
  if (chlen <= 0) return -1;
  if (c > 0x10ffff) return -2;
  p += chlen;
  len -= chlen;
  if (c < 0x10000) *pp++ = c; else
    {
    if (!utf) return -3;
    c -= 0x10000;
    *pp++ = 0xD800 | (c >> 10);
    *pp++ = 0xDC00 | (c & 0x3ff);
    }
  }

*pp = 0;
return pp - pbuffer16;
}
#endif



#ifdef SUPPORT_PCRE32
/*************************************************
*         Convert a string to 32-bit             *
*************************************************/

/* The input is always interpreted as a string of UTF-8 bytes. If all the input
bytes are ASCII, the space needed for a 32-bit string is exactly four times the
8-bit size. Otherwise, the size needed for a 32-bit string is no more than four
times, because the number of characters must be less than the number of bytes.
The result is always left in pbuffer32. Impose a minimum size to save repeated
re-sizing.

Note that this function does not object to surrogate values. This is
deliberate; it makes it possible to construct UTF-32 strings that are invalid,
for the purpose of testing that they are correctly faulted.

Arguments:
  p          points to a byte string
  utf        true if UTF-8 (to be converted to UTF-32)
  len        number of bytes in the string (excluding trailing zero)

Returns:     number of 32-bit data items used (excluding trailing zero)
             OR -1 if a UTF-8 string is malformed
             OR -2 if a value > 0x10ffff is encountered in UTF mode
*/

static int
to32(uint8_t *p, int utf, int len)
{
uint32_t *pp;

if (pbuffer32_size < 4*len + 4)
  {
  if (pbuffer32 != NULL) free(pbuffer32);
  pbuffer32_size = 4*len + 4;
  if (pbuffer32_size < 256) pbuffer32_size = 256;
  pbuffer32 = (uint32_t *)malloc(pbuffer32_size);
  if (pbuffer32 == NULL)
    {
    fprintf(stderr, "pcretest: malloc(%d) failed for pbuffer32\n", pbuffer32_size);
    exit(1);
    }
  }
pp = pbuffer32;

while (len > 0)
  {
  uint32_t c;
  int chlen = utf82ord(p, &c);
  if (chlen <= 0) return -1;
  if (utf && c > 0x10ffff) return -2;
  p += chlen;
  len -= chlen;
  *pp++ = c;
  }

*pp = 0;
return pp - pbuffer32;
}
#endif /* SUPPORT_PCRE32 */



/*************************************************
*        Read or extend an input line            *
*************************************************/

/* Input lines are read into buffer, but both patterns and data lines can be
continued over multiple input lines. In addition, if the buffer fills up, we
want to automatically expand it so as to be able to handle extremely large
lines that are needed for certain stress tests. When the input buffer is
expanded, the other two buffers must also be expanded likewise, and the
contents of pbuffer, which are a copy of the input for callouts, must be
preserved (for when expansion happens for a data line). This is not the most
optimal way of handling this, but hey, this is just a test program!

Arguments:
  f            the file to read
  start        where in buffer to start (this *must* be within buffer)
  prompt       for stdin or readline()

Returns:       pointer to the start of new data
               could be a copy of start, or could be moved
               NULL if no data read and EOF reached
*/

static uint8_t *
extend_inputline(FILE *f, uint8_t *start, const char *prompt)
{
uint8_t *here = start;

for (;;)
  {
  size_t rlen = (size_t)(pbuffer8_size - (here - buffer));

  if (rlen > 1000)
    {
    int dlen;

    /* If libreadline or libedit support is required, use readline() to read a
    line if the input is a terminal. Note that readline() removes the trailing
    newline, so we must put it back again, to be compatible with fgets(). */

#if defined(SUPPORT_LIBREADLINE) || defined(SUPPORT_LIBEDIT)
    if (isatty(fileno(f)))
      {
      size_t len;
      char *s = readline(prompt);
      if (s == NULL) return (here == start)? NULL : start;
      len = strlen(s);
      if (len > 0) add_history(s);
      if (len > rlen - 1) len = rlen - 1;
      memcpy(here, s, len);
      here[len] = '\n';
      here[len+1] = 0;
      free(s);
      }
    else
#endif

    /* Read the next line by normal means, prompting if the file is stdin. */

      {
      if (f == stdin) printf("%s", prompt);
      if (fgets((char *)here, rlen,  f) == NULL)
        return (here == start)? NULL : start;
      }

    dlen = (int)strlen((char *)here);
    if (dlen > 0 && here[dlen - 1] == '\n') return start;
    here += dlen;
    }

  else
    {
    int new_pbuffer8_size = 2*pbuffer8_size;
    uint8_t *new_buffer = (uint8_t *)malloc(new_pbuffer8_size);
    uint8_t *new_pbuffer8 = (uint8_t *)malloc(new_pbuffer8_size);

    if (new_buffer == NULL || new_pbuffer8 == NULL)
      {
      fprintf(stderr, "pcre2test: malloc(%d) failed\n", new_pbuffer8_size);
      exit(1);
      }

    memcpy(new_buffer, buffer, pbuffer8_size);
    memcpy(new_pbuffer8, pbuffer8, pbuffer8_size);

    pbuffer8_size = new_pbuffer8_size;

    start = new_buffer + (start - buffer);
    here = new_buffer + (here - buffer);

    free(buffer);
    free(pbuffer8);

    buffer = new_buffer;
    pbuffer8 = new_pbuffer8;
    }
  }

/* Control never gets here */
}



/*************************************************
*         Case-independent strncmp() function    *
*************************************************/

/*
Arguments:
  s         first string
  t         second string
  n         number of characters to compare

Returns:    < 0, = 0, or > 0, according to the comparison
*/

static int
strncmpic(const uint8_t *s, const uint8_t *t, int n)
{
while (n--)
  {
  int c = tolower(*s++) - tolower(*t++);
  if (c) return c;
  }
return 0;
}



/*************************************************
*          Read number from string               *
*************************************************/

/* We don't use strtoul() because SunOS4 doesn't have it. Rather than mess
around with conditional compilation, just do the job by hand. It is only used
for unpicking arguments, so just keep it simple.

Arguments:
  str           string to be converted
  endptr        where to put the end pointer

Returns:        the unsigned long
*/

static int
get_value(const char *str, const char **endptr)
{
int result = 0;
while(*str != 0 && isspace(*str)) str++;
while (isdigit(*str)) result = result * 10 + (int)(*str++ - '0');
*endptr = str;
return(result);
}



/*************************************************
*          Scan the main modifier list           *
*************************************************/

/* This function searches the modifier list for a long modifier name.

Argument:
  p         start of the name
  lenp      length of the name

Returns:    an index in the modifier list, or -1 on failure
*/

static int
scan_modifiers(const uint8_t *p, unsigned int len)
{
int bot = 0;
int top = MODLISTCOUNT;

while (top > bot)
  {
  int mid = (bot + top)/2;
  unsigned int mlen = strlen(modlist[mid].name);
  int c = strncmp((char *)p, modlist[mid].name, (len < mlen)? len : mlen);
  if (c == 0)
    {
    if (len == mlen) return mid;
    c = len - mlen;
    }
  if (c > 0) bot = mid + 1; else top = mid;
  }

return -1;

}



/*************************************************
*        Check a modifer and find its field      *
*************************************************/

/* This function is called when a modifier has been identified. We check that
it is allowed here and find the field that is to be changed.

Arguments:
  m          the modifier list entry
  ctx        CTX_PAT     => pattern context
             CTX_DEFPAT  => default pattern context
             CTX_DAT     => data context
             CTX_DEFDAT  => default data context
             CTX_DEFANY  => any default context (depends on the modifier)
  pctl       point to pattern control block
  dctl       point to data control block
  c          a single character or 0

Returns:     a field pointer or NULL
*/

static void *
check_modifier(modstruct *m, int ctx, patctl *pctl, datctl *dctl, uint32_t c)
{
void *field = NULL;
PCRE2_OFFSET offset = m->offset;

if (restrict_for_perl_test) switch(m->which)
  {
  case MOD_PNDP:
  case MOD_PATP:
  case MOD_PDP:
  break;

  default:
  fprintf(outfile, "** '%s' is not allowed in a Perl-compatible test\n",
    m->name);
  return NULL;
  }

switch (m->which)
  {
  case MOD_CTB:  /* Compile or match context modifier */
  case MOD_CTC:  /* Compile context modifier */
  if (ctx == CTX_DEFPAT || ctx == CTX_DEFANY) field = PTR(default_pat_context);
    else if (ctx == CTX_PAT) field = PTR(pat_context);
  if (field != NULL || m->which == MOD_CTC) break;

  /* Fall through for something that can also be in a match context. In this
  case the offset is taken from the other field. */

  offset = (PCRE2_OFFSET)(m->value);

  case MOD_CTM:  /* Match context modifier */
  if (ctx == CTX_DEFDAT || ctx == CTX_DEFANY) field = PTR(default_dat_context);
    else if (ctx == CTX_DAT) field = PTR(dat_context);
  break;

  case MOD_DAT:  /* Data line modifier */
  if (dctl != NULL) field = dctl;
  break;

  case MOD_PAT:  /* Pattern modifier */
  case MOD_PATP: /* Allowed for Perl test */
  if (pctl != NULL) field = pctl;
  break;

  case MOD_PD:   /* Pattern or data line modifier */
  case MOD_PDP:  /* Ditto, allowed for Perl test */
  case MOD_PND:  /* Ditto, but not default pattern */
  case MOD_PNDP: /* Ditto, allowed for Perl test */
  if (dctl != NULL) field = dctl;
    else if (pctl != NULL && (m->which == MOD_PD || ctx != CTX_DEFPAT))
      field = pctl;
  break;
  }

if (field == NULL)
  {
  if (c == 0)
    fprintf(outfile, "** '%s' is not valid here\n", m->name);
  else
    fprintf(outfile, "** /%c is not valid here\n", c);
  return NULL;
  }

return (char *)field + offset;
}



/*************************************************
*            Decode a modifier list              *
*************************************************/

/* A pointer to a control block is NULL when called in cases when that block is
not relevant. They are never all relevant in one call. At least one of patctl
and datctl is NULL. The second argument specifies which context to use for
modifiers that apply to contexts.

Arguments:
  p          point to modifier string
  ctx        CTX_PAT     => pattern context
             CTX_DEFPAT  => default pattern context
             CTX_DAT     => data context
             CTX_DEFDAT  => default data context
             CTX_DEFANY  => any default context (depends on the modifier)
  pctl       point to pattern control block
  dctl       point to data control block

Returns: TRUE if successful decode, FALSE otherwise
*/

static BOOL
decode_modifiers(uint8_t *p, int ctx, patctl *pctl, datctl *dctl)
{
uint8_t *ep, *pp;
BOOL first = TRUE;

for (;;)
  {
  void *field;
  modstruct *m;
  BOOL off = FALSE;
  unsigned int i, len;
  int index;
  char *endptr;

  /* Skip white space and commas; after a comma we have passed the first
  item. */

  while (isspace(*p)) p++;
  if (*p == ',') first = FALSE;
  while (isspace(*p) || *p == ',') p++;
  if (*p == 0) break;

  /* Find the end of the item. */

  for (ep = p; *ep != 0 && *ep != ',' && !isspace(*ep); ep++);

  /* Remember if the first character is '-'. */

  if (*p == '-')
    {
    off = TRUE;
    p++;
    }

  /* Find the length of a full-length modifier name, and scan for it. */

  pp = p;
  while (pp < ep && *pp != '=') pp++;
  index = scan_modifiers(p, pp - p);

  /* If the first modifier is unrecognized, try to interpret it as a sequence
  of single-character abbreviated modifiers. None of these modifiers have any
  associated data. They just set options or control bits. */

  if (index < 0)
    {
    uint32_t cc;
    uint8_t *mp = p;

    if (!first)
      {
      fprintf(outfile, "** Unrecognized modifier '%.*s'\n", (int)(ep-p), p);
      if (ep - p == 1)
        fprintf(outfile, "** Single-character modifiers must come first\n");
      return FALSE;
      }

    for (cc = *p; cc != ',' && cc != '\n' && cc != 0; cc = *(++p))
      {
      if (p[1] == cc)           /* Handle doubled characters */
        {
        cc = (cc << 8) | cc;
        p++;
        }

      for (i = 0; i < C1MODLISTCOUNT; i++)
        if (cc == c1modlist[i].onechar) break;

      if (i >= C1MODLISTCOUNT)
        {
        fprintf(outfile, "** Unrecognized modifier '%c' in '%.*s'\n",
          *p, (int)(ep-mp), mp);
        return FALSE;
        }

      if (c1modlist[i].index >= 0)
        {
        index = c1modlist[i].index;
        }

      else
        {
        index = scan_modifiers((uint8_t *)(c1modlist[i].fullname),
          strlen(c1modlist[i].fullname));
        if (index < 0)
          {
          fprintf(outfile, "** Internal error: single-character equivalent "
            "modifier '%s' not found\n", c1modlist[i].fullname);
          return FALSE;
          }
        c1modlist[i].index = index;     /* Cache for next time */
        }

      field = check_modifier(modlist + index, ctx, pctl, dctl, *p);
      if (field == NULL) return FALSE;
      *((uint32_t *)field) |= modlist[index].value;
      }

    continue;    /* With tne next (fullname) modifier */
    }

  /* We have a match on a full-name modifier. Check for the existence of data
  when needed. */

  m = modlist + index;      /* Save typing */
  if (m->type != MOD_CTL && m->type != MOD_OPT &&
      (m->type != MOD_IND || *pp == '='))
    {
    if (*pp++ != '=')
      {
      fprintf(outfile, "** '=' expected after '%s'\n", m->name);
      return FALSE;
      }
    if (off)
      {
      fprintf(outfile, "** '-' is not valid for '%s'\n", m->name);
      return FALSE;
      }
    }

  /* These on/off types have no data. */

  else if (*pp != ',' && *pp != '\n' && *pp != 0)
    {
    fprintf(outfile, "** Unrecognized modifier '%.*s'\n", (int)(ep-p), p);
    return FALSE;
    }

  /* Set the data length for those types that have data. Then find the field
  that is to be set. If check_modifier() returns NULL, it has already output an
  error message. */

  len = ep - pp;
  field = check_modifier(m, ctx, pctl, dctl, 0);
  if (field == NULL) return FALSE;

  /* Process according to data type. */

  switch (m->type)
    {
    case MOD_CTL:
    case MOD_OPT:
    if (off) *((uint32_t *)field) &= ~m->value;
      else *((uint32_t *)field) |= m->value;
    break;

    case MOD_BSR:
    if (len == 7 && strncmpic(pp, (const uint8_t *)"anycrlf", 7) == 0)
      *((uint16_t *)field) = PCRE2_BSR_ANYCRLF;
    else if (len == 7 && strncmpic(pp, (const uint8_t *)"unicode", 7) == 0)
      *((uint16_t *)field) = PCRE2_BSR_UNICODE;
    else goto INVALID_VALUE;
    pp = ep;
    break;

    case MOD_IN2:    /* One or two unsigned integers */
    if (!isdigit(*pp)) goto INVALID_VALUE;
    ((uint32_t *)field)[0] = (uint32_t)strtoul((const char *)pp, &endptr, 10);
    if (*endptr == ':')
      ((uint32_t *)field)[1] = (uint32_t)strtoul((const char *)endptr+1, &endptr, 10);
    else ((uint32_t *)field)[1] = 0;
    pp = (uint8_t *)endptr;
    break;

    case MOD_IND:    /* Unsigned integer with default */
    if (len == 0)
      {
      *((uint32_t *)field) = (uint32_t)(m->value);
      break;
      }
    /* Fall through */

    case MOD_INT:    /* Unsigned integer */
    if (!isdigit(*pp)) goto INVALID_VALUE;
    *((uint32_t *)field) = (uint32_t)strtoul((const char *)pp, &endptr, 10);
    pp = (uint8_t *)endptr;
    break;

    case MOD_INS:   /* Signed integer */
    if (!isdigit(*pp) && *pp != '-') goto INVALID_VALUE;
    *((int32_t *)field) = (int32_t)strtol((const char *)pp, &endptr, 10);
    pp = (uint8_t *)endptr;
    break;

    case MOD_NL:
    for (i = 0; i < sizeof(newlines)/sizeof(char *); i++)
      if (len == strlen(newlines[i]) &&
        strncmpic(pp, (const uint8_t *)newlines[i], len) == 0) break;
    if (i >= sizeof(newlines)/sizeof(char *)) goto INVALID_VALUE;
    *((uint16_t *)field) = i;
    pp = ep;
    break;

    case MOD_NN:              /* Name or (signed) number; may be several */
    if (isdigit(*pp) || *pp == '-')
      {
      int ct = MAXCPYGET - 1;
      int32_t value = (int32_t)strtol((const char *)pp, &endptr, 10);
      field = (char *)field - m->offset + m->value;      /* Adjust field ptr */
      if (value >= 0)                                    /* Add new number */
        {
        while (*((int32_t *)field) >= 0 && ct-- > 0)   /* Skip previous */
          field = (char *)field + sizeof(int32_t);
        if (ct <= 0)
          {
          fprintf(outfile, "** Too many numeric '%s' modifiers\n", m->name);
          return FALSE;
          }
        }
      *((int32_t *)field) = value;
      if (ct > 0) ((int32_t *)field)[1] = -1;
      pp = (uint8_t *)endptr;
      }

    /* Multiple strings are put end to end. */

    else
      {
      char *nn = (char *)field;
      if (len > 0)                    /* Add new name */
        {
        while (*nn != 0) nn += strlen(nn) + 1;
        if (nn + len + 1 - (char *)field > LENCPYGET)
          {
          fprintf(outfile, "** Too many named '%s' modifiers\n", m->name);
          return FALSE;
          }
        memcpy(nn, pp, len);
        }
      nn[len] = 0 ;
      nn[len+1] = 0;
      pp = ep;
      }
    break;

    case MOD_STR:
    memcpy(field, pp, len);
    ((uint8_t *)field)[len] = 0;
    pp = ep;
    break;
    }

  if (*pp != ',' && *pp != '\n' && *pp != 0)
    {
    fprintf(outfile, "** Comma expected after modifier item '%s'\n", m->name);
    return FALSE;
    }

  p = pp;
  }

return TRUE;

INVALID_VALUE:
fprintf(outfile, "** Invalid value in '%.*s'\n", (int)(ep-p), p);
return FALSE;
}


/*************************************************
*             Get info from a pattern            *
*************************************************/

/* A wrapped call to pcre2_pattern_info(), applied to the current compiled
pattern.

Arguments:
  what        code for the required information
  where       where to put the answer

Returns:      the return from pcre2_pattern_info()
*/

static int
pattern_info(int what, void *where)
{
int rc;
PCRE2_PATTERN_INFO(rc, compiled_code, what, where);
if (rc >= 0) return 0;
fprintf(outfile, "Error %d from pcre2_pattern_info_%d(%d)\n", rc, test_mode,
  what);
if (rc == PCRE2_ERROR_BADMODE)
  fprintf(outfile, "Running in %d-bit mode but pattern was compiled in "
    "%d-bit mode\n", test_mode,
    8 * (FLD(compiled_code, flags) & PCRE2_MODE_MASK));
return rc;
}



/*************************************************
*             Show something in a list           *
*************************************************/

/* This function just helps to keep the code that uses it tidier. It's used for
various lists of things where there needs to be introductory text before the
first item. */

static void
prmsg(const char **msg, const char *s)
{
fprintf(outfile, "%s %s", *msg, s);
*msg = "";
}



/*************************************************
*                Show compile controls           *
*************************************************/

/* Called for unsupported POSIX modifiers.

Arguments:
  controls    control bits
  before      text to print before
  after       text to print after

Returns:      nothing
*/

static void
show_compile_controls(uint32_t controls, const char *before, const char *after)
{
fprintf(outfile, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
  before,
  ((controls & CTL_AFTERTEXT) != 0)? " aftertext" : "",
  ((controls & CTL_ALLAFTERTEXT) != 0)? " allaftertext" : "",
  ((controls & CTL_ALLCAPTURES) != 0)? " allcaptures" : "",
  ((controls & CTL_ALTGLOBAL) != 0)? " altglobal" : "",
  ((controls & CTL_BINCODE) != 0)? " bincode" : "",
  ((controls & CTL_FLIPBYTES) != 0)? " flipbytes" : "",
  ((controls & CTL_FULLBINCODE) != 0)? " fullbincode" : "",
  ((controls & CTL_GLOBAL) != 0)? " global" : "",
  ((controls & CTL_HEXPAT) != 0)? " hex" : "",
  ((controls & CTL_INFO) != 0)? " info" : "",
  ((controls & CTL_JITVERIFY) != 0)? " jitverify" : "",
  ((controls & CTL_MARK) != 0)? " mark" : "",
  ((controls & CTL_PATLEN) != 0)? " use_length" : "",
  ((controls & CTL_POSIX) != 0)? " posix" : "",
  after);
}



/*************************************************
*                Show compile options            *
*************************************************/

/* Called from show_pattern_info() and for unsupported POSIX options.

Arguments:
  options     an options word
  before      text to print before
  after       text to print after

Returns:      nothing
*/

static void
show_compile_options(uint32_t options, const char *before, const char *after)
{
if (options == 0) fprintf(outfile, "%s <none>%s", before, after);
else fprintf(outfile, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
  before,
  ((options & PCRE2_ANCHORED) != 0)? " anchored" : "",
  ((options & PCRE2_CASELESS) != 0)? " caseless" : "",
  ((options & PCRE2_EXTENDED) != 0)? " extended" : "",
  ((options & PCRE2_MULTILINE) != 0)? " multiline" : "",
  ((options & PCRE2_FIRSTLINE) != 0)? " firstline" : "",
  ((options & PCRE2_DOTALL) != 0)? " dotall" : "",
  ((options & PCRE2_DOLLAR_ENDONLY) != 0)? " dollar_endonly" : "",
  ((options & PCRE2_UNGREEDY) != 0)? " ungreedy" : "",
  ((options & PCRE2_NO_AUTO_CAPTURE) != 0)? " no_auto_capture" : "",
  ((options & PCRE2_NO_AUTO_POSSESS) != 0)? " no_auto_possessify" : "",
  ((options & PCRE2_UTF) != 0)? " utf" : "",
  ((options & PCRE2_UCP) != 0)? " ucp" : "",
  ((options & PCRE2_NO_UTF_CHECK) != 0)? " no_utf_check" : "",
  ((options & PCRE2_NO_START_OPTIMIZE) != 0)? " no_start_optimize" : "",
  ((options & PCRE2_DUPNAMES) != 0)? " dupnames" : "",
  ((options & PCRE2_ALT_BSUX) != 0)? " alt_bsux" : "",
  ((options & PCRE2_ALLOW_EMPTY_CLASS) != 0)? " allow_empty_class" : "",
  ((options & PCRE2_AUTO_CALLOUT) != 0)? " auto_callout" : "",
  ((options & PCRE2_MATCH_UNSET_BACKREF) != 0)? " match_unset_backref" : "",
  ((options & PCRE2_NEVER_UCP) != 0)? " never_ucp" : "",
  ((options & PCRE2_NEVER_UTF) != 0)? " never_utf" : "",
  after);
}



/*************************************************
*                Show match controls           *
*************************************************/

/* Called for unsupported POSIX modifiers. */

static void
show_match_controls(uint32_t controls)
{
fprintf(outfile, "%s%s%s%s%s%s%s%s%s%s%s%s%s",
  ((controls & CTL_AFTERTEXT) != 0)? " aftertext" : "",
  ((controls & CTL_ALLAFTERTEXT) != 0)? " allaftertext" : "",
  ((controls & CTL_ALLCAPTURES) != 0)? " allcaptures" : "",
  ((controls & CTL_ALTGLOBAL) != 0)? " altglobal" : "",
  ((controls & CTL_CALLOUT_CAPTURE) != 0)? " callout_capture" : "",
  ((controls & CTL_CALLOUT_NONE) != 0)? " callout_none" : "",
  ((controls & CTL_DFA) != 0)? " dfa" : "",
  ((controls & CTL_FINDLIMITS) != 0)? " find_limits" : "",
  ((controls & CTL_GETALL) != 0)? " getall" : "",
  ((controls & CTL_GLOBAL) != 0)? " global" : "",
  ((controls & CTL_JITVERIFY) != 0)? " jitverify" : "",
  ((controls & CTL_MARK) != 0)? " mark" : "",
  ((controls & CTL_MEMORY) != 0)? " memory" : "");
}



/*************************************************
*                Show match options              *
*************************************************/

/* Called for unsupported POSIX options. */

static void
show_match_options(uint32_t options)
{
fprintf(outfile, "%s%s%s%s%s%s%s%s%s%s%s",
  ((options & PCRE2_ANCHORED) != 0)? " anchored" : "",
  ((options & PCRE2_DFA_RESTART) != 0)? " dfa_restart" : "",
  ((options & PCRE2_DFA_SHORTEST) != 0)? " dfa_shortest" : "",
  ((options & PCRE2_NO_START_OPTIMIZE) != 0)? " no_start_optimize" : "",
  ((options & PCRE2_NO_UTF_CHECK) != 0)? " no_utf_check" : "",
  ((options & PCRE2_NOTBOL) != 0)? " notbol" : "",
  ((options & PCRE2_NOTEMPTY) != 0)? " notempty" : "",
  ((options & PCRE2_NOTEMPTY_ATSTART) != 0)? " notempty_atstart" : "",
  ((options & PCRE2_NOTEOL) != 0)? " noteol" : "",
  ((options & PCRE2_PARTIAL_HARD) != 0)? " partial_hard" : "",
  ((options & PCRE2_PARTIAL_SOFT) != 0)? " partial_soft" : "");
}



/*************************************************
*        Show information about a pattern        *
*************************************************/

/* This function is called after a pattern has been compiled or loaded from a
file, if any of the information-requesting controls have been set.

Arguments:  none

Returns:    PR_OK     continue processing next line
            PR_SKIP   skip to a blank line
            PR_ABEND  abort the pcre2test run
*/

static int
show_pattern_info(void)
{
uint32_t compile_options, overall_options;

if ((pat_patctl.control & (CTL_BINCODE|CTL_FULLBINCODE)) != 0)
  {
  fprintf(outfile, "------------------------------------------------------------------\n");
  PCRE2_PRINTINT((pat_patctl.control & CTL_FULLBINCODE) != 0);
  }

if ((pat_patctl.control & CTL_INFO) != 0)
  {
  const void *nametable;
  const uint8_t *start_bits;
  int count, backrefmax, first_ctype, last_ctype, jchanged,
    hascrorlf, maxlookbehind, match_empty, minlength;
  int nameentrysize, namecount;
  uint32_t bsr_convention, newline_convention;
  uint32_t first_cunit, last_cunit;
  uint32_t match_limit, recursion_limit;

  /* These info requests should always succeed. */

  if (pattern_info(PCRE2_INFO_BACKREFMAX, &backrefmax) +
      pattern_info(PCRE2_INFO_BSR, &bsr_convention) +
      pattern_info(PCRE2_INFO_CAPTURECOUNT, &count) +
      pattern_info(PCRE2_INFO_FIRSTBITMAP, &start_bits) +
      pattern_info(PCRE2_INFO_FIRSTCODEUNIT, &first_cunit) +
      pattern_info(PCRE2_INFO_FIRSTCODETYPE, &first_ctype) +
      pattern_info(PCRE2_INFO_HASCRORLF, &hascrorlf) +
      pattern_info(PCRE2_INFO_JCHANGED, &jchanged) +
      pattern_info(PCRE2_INFO_LASTCODEUNIT, &last_cunit) +
      pattern_info(PCRE2_INFO_LASTCODETYPE, &last_ctype) +
      pattern_info(PCRE2_INFO_MATCHEMPTY, &match_empty) +
      pattern_info(PCRE2_INFO_MATCHLIMIT, &match_limit) +
      pattern_info(PCRE2_INFO_MAXLOOKBEHIND, &maxlookbehind) +
      pattern_info(PCRE2_INFO_MINLENGTH, &minlength) +
      pattern_info(PCRE2_INFO_NAMECOUNT, &namecount) +
      pattern_info(PCRE2_INFO_NAMEENTRYSIZE, &nameentrysize) +
      pattern_info(PCRE2_INFO_NAMETABLE, &nametable) +
      pattern_info(PCRE2_INFO_NEWLINE, &newline_convention) +
      pattern_info(PCRE2_INFO_RECURSIONLIMIT, &recursion_limit)
      != 0)
    return PR_ABEND;

  fprintf(outfile, "Capturing subpattern count = %d\n", count);

  if (backrefmax > 0)
    fprintf(outfile, "Max back reference = %d\n", backrefmax);

  if (maxlookbehind > 0)
    fprintf(outfile, "Max lookbehind = %d\n", maxlookbehind);

  if (match_limit != UINT32_MAX)
    fprintf(outfile, "Match limit = %u\n", match_limit);

  if (recursion_limit != UINT32_MAX)
    fprintf(outfile, "Recursion limit = %u\n", recursion_limit);

  if (namecount > 0)
    {
    fprintf(outfile, "Named capturing subpatterns:\n");
    while (namecount-- > 0)
      {
      int imm2_size = test_mode == PCRE8_MODE ? 2 : 1;
      int length = (int)STRLEN(nametable + imm2_size);
      fprintf(outfile, "  ");
      PCHARSV(nametable, imm2_size, length, FALSE, outfile);
      while (length++ < nameentrysize - imm2_size) putc(' ', outfile);
#ifdef SUPPORT_PCRE32
      if (test_mode == PCRE32_MODE)
        fprintf(outfile, "%3d\n", (int)(((PCRE2_SPTR32)nametable)[0]));
#endif
#ifdef SUPPORT_PCRE16
      if (test_mode == PCRE16_MODE)
        fprintf(outfile, "%3d\n", (int)(((PCRE2_SPTR16)nametable)[0]));
#endif
#ifdef SUPPORT_PCRE8
      if (test_mode == PCRE8_MODE)
        fprintf(outfile, "%3d\n", (int)(
        ((((PCRE2_SPTR8)nametable)[0]) << 8) | ((PCRE2_SPTR8)nametable)[1]));
#endif
      nametable = (void*)((PCRE2_SPTR8)nametable + nameentrysize * code_unit_size);
      }
    }

  if (hascrorlf)   fprintf(outfile, "Contains explicit CR or LF match\n");
  if (match_empty) fprintf(outfile, "May match empty string\n");

  pattern_info(PCRE2_INFO_ARGOPTIONS, &compile_options);
  pattern_info(PCRE2_INFO_ALLOPTIONS, &overall_options);

  if ((compile_options|overall_options) == 0)
    fprintf(outfile, "No options\n");
  else
    {
    show_compile_options(compile_options, "Compile options:", "\n");
    show_compile_options(overall_options, "Overall options:", "\n");
    }

  if (jchanged) fprintf(outfile, "Duplicate name status changes\n");

  if (bsr_convention != BSR_DEFAULT)
    fprintf(outfile, "\\R matches %s\n", (bsr_convention == PCRE2_BSR_UNICODE)?
      "any Unicode newline" : "CR, LF, or CRLF");

  switch (newline_convention)
    {
    case PCRE2_NEWLINE_CR:
    fprintf(outfile, "Newline is CR\n");
    break;

    case PCRE2_NEWLINE_LF:
    fprintf(outfile, "Newline is LF\n");
    break;

    case PCRE2_NEWLINE_CRLF:
    fprintf(outfile, "Newline is CRLF\n");
    break;

    case PCRE2_NEWLINE_ANYCRLF:
    fprintf(outfile, "Newline is CR, LF, or CRLF\n");
    break;

    case PCRE2_NEWLINE_ANY:
    fprintf(outfile, "Newline is any Unicode newline\n");
    break;

    default:
    break;
    }

  if (first_ctype == 2)
    {
    fprintf(outfile, "First char at start or follows newline\n");
    }
  else if (first_ctype == 1)
    {
    const char *caseless =
      ((FLD(compiled_code, flags) & PCRE2_FIRSTCASELESS) == 0)?
      "" : " (caseless)";
    if (PRINTOK(first_cunit))
      fprintf(outfile, "First code unit = \'%c\'%s\n", first_cunit, caseless);
    else
      {
      fprintf(outfile, "First code unit = ");
      pchar(first_cunit, FALSE, outfile);
      fprintf(outfile, "%s\n", caseless);
      }
    }
  else
    {
    fprintf(outfile, "No first code unit\n");
    }

  if (last_ctype == 0)
    {
    fprintf(outfile, "No last code unit\n");
    }
  else
    {
    const char *caseless =
      ((FLD(compiled_code, flags) & PCRE2_LASTCASELESS) == 0)?
      "" : " (caseless)";
    if (PRINTOK(last_cunit))
      fprintf(outfile, "Last code unit = \'%c\'%s\n", last_cunit, caseless);
    else
      {
      fprintf(outfile, "Last code unit = ");
      pchar(last_cunit, FALSE, outfile);
      fprintf(outfile, "%s\n", caseless);
      }
    }

  fprintf(outfile, "Subject length lower bound = %d\n", minlength);

  if (start_bits == NULL)
    fprintf(outfile, "No starting code unit list\n");
  else
    {
    int i;
    int c = 24;
    fprintf(outfile, "Starting code units: ");
    for (i = 0; i < 256; i++)
      {
      if ((start_bits[i/8] & (1<<(i&7))) != 0)
        {
        if (c > 75)
          {
          fprintf(outfile, "\n  ");
          c = 2;
          }
        if (PRINTOK(i) && i != ' ')
          {
          fprintf(outfile, "%c ", i);
          c += 2;
          }
        else
          {
          fprintf(outfile, "\\x%02x ", i);
          c += 5;
          }
        }
      }
    fprintf(outfile, "\n");
    }

/* FIXME: tidy this up */

  if (pat_patctl.jit != 0)
    {
    size_t jitsize;
    if (pattern_info(PCRE2_INFO_JITSIZE, &jitsize) == 0)
      {
      if (jitsize > 0)
        fprintf(outfile, "JIT study was successful\n");
      else
#ifdef SUPPORT_JIT
        fprintf(outfile, "JIT study was not successful\n");
#else
        fprintf(outfile, "JIT support is not available in this version of PCRE\n");
#endif
      }
    }
  }

return PR_OK;
}



/*************************************************
*               Process command line             *
*************************************************/

/* This function is called for lines beginning with # and a character that is
not ! or whitespace, when encountered between tests. The line is in buffer.

Arguments:  none

Returns:    PR_OK     continue processing next line
            PR_SKIP   skip to a blank line
            PR_ABEND  abort the pcre2test run
*/

static int
process_command(void)
{
if (restrict_for_perl_test)
  {
  fprintf(outfile, "** #-commands are not allowed after #perltest\n");
  return PR_ABEND;
  }

if (strncmp((char *)buffer, "#pattern", 8) == 0 && isspace(buffer[8]))
  {
  (void)decode_modifiers(buffer + 8, CTX_DEFPAT, &def_patctl, NULL);
  }
else if (strncmp((char *)buffer, "#perltest", 9) == 0 && isspace(buffer[9]))
  {
  restrict_for_perl_test = TRUE;
  }
else if (strncmp((char *)buffer, "#subject", 8) == 0 && isspace(buffer[8]))
  {
  (void)decode_modifiers(buffer + 8, CTX_DEFDAT, NULL, &def_datctl);
  }
else if (strncmp((char *)buffer, "#load", 5) == 0 && isspace(buffer[5]))
  {
/* FIXME */
fprintf(outfile, "** #load not yet implemented\n");
return PR_ABEND;

#ifdef FIXME


/* See if the pattern is to be loaded pre-compiled from a file. */

if (*p == '<' && strchr((char *)(p+1), '<') == NULL)
  {
  uint32_t magic;
  uint8_t sbuf[8];
  FILE *f;

  p++;
  if (*p == '!')
    {
    do_debug = TRUE;
    do_showinfo = TRUE;
    p++;
    }

  pp = p + (int)strlen((char *)p);
  while (isspace(pp[-1])) pp--;
  *pp = 0;

  f = fopen((char *)p, "rb");
  if (f == NULL)
    {
    fprintf(outfile, "Failed to open %s: %s\n", p, strerror(errno));
    continue;
    }
  if (fread(sbuf, 1, 8, f) != 8) goto FAIL_READ;

  true_size =
    (sbuf[0] << 24) | (sbuf[1] << 16) | (sbuf[2] << 8) | sbuf[3];
  true_study_size =
    (sbuf[4] << 24) | (sbuf[5] << 16) | (sbuf[6] << 8) | sbuf[7];

  re = (pcre *)new_malloc(true_size);
  if (re == NULL)
    {
    printf("** Failed to get %d bytes of memory for pcre object\n",
      (int)true_size);
    yield = 1;
    goto EXIT;
    }
  if (fread(re, 1, true_size, f) != true_size) goto FAIL_READ;

  magic = REAL_PCRE_MAGIC(re);
  if (magic != MAGIC_NUMBER)
    {
    if (swap_uint32(magic) == MAGIC_NUMBER)
      {
      do_flip = 1;
      }
    else
      {
      fprintf(outfile, "Data in %s is not a compiled PCRE regex\n", p);
      new_free(re);
      fclose(f);
      continue;
      }
    }

  /* We hide the byte-invert info for little and big endian tests. */
  fprintf(outfile, "Compiled pattern%s loaded from %s\n",
    do_flip && (p[-1] == '<') ? " (byte-inverted)" : "", p);

  /* Now see if there is any following study data. */

  if (true_study_size != 0)
    {
    pcre_study_data *psd;

    extra = (pcre_extra *)new_malloc(sizeof(pcre_extra) + true_study_size);
    extra->flags = PCRE_EXTRA_STUDY_DATA;

    psd = (pcre_study_data *)(((char *)extra) + sizeof(pcre_extra));
    extra->study_data = psd;

    if (fread(psd, 1, true_study_size, f) != true_study_size)
      {
      FAIL_READ:
      fprintf(outfile, "Failed to read data from %s\n", p);
      if (extra != NULL)
        {
        PCRE_FREE_STUDY(extra);
        }
      new_free(re);
      fclose(f);
      continue;
      }
    fprintf(outfile, "Study data loaded from %s\n", p);
    do_study = 1;     /* To get the data output if requested */
    }
  else fprintf(outfile, "No study data\n");

  /* Flip the necessary bytes. */
  if (do_flip)
    {
    int rc;
    PCRE_PATTERN_TO_HOST_BYTE_ORDER(rc, re, extra, NULL);
    if (rc == PCRE_ERROR_BADMODE)
      {
      uint32_t flags_in_host_byte_order;
      if (REAL_PCRE_MAGIC(re) == MAGIC_NUMBER)
        flags_in_host_byte_order = REAL_PCRE_FLAGS(re);
      else
        flags_in_host_byte_order = swap_uint32(REAL_PCRE_FLAGS(re));
      /* Simulate the result of the function call below. */
      fprintf(outfile, "Error %d from pcre%s_fullinfo(%d)\n", rc,
        test_mode == PCRE32_MODE ? "32" : test_mode == PCRE16_MODE ? "16" : "",
        PCRE_INFO_OPTIONS);
      fprintf(outfile, "Running in %d-bit mode but pattern was compiled in "
        "%d-bit mode\n", test_mode, 8 * (flags_in_host_byte_order & test_mode_MASK));
      new_free(re);
      fclose(f);
      continue;
      }
    }

  /* Need to know if UTF-8 for printing data strings. */

  if (new_info(re, NULL, PCRE_INFO_OPTIONS, &get_options) < 0)
    {
    new_free(re);
    fclose(f);
    continue;
    }
  use_utf = (get_options & PCRE_UTF8) != 0;

  fclose(f);
  goto SHOW_INFO;
  }

#endif  /* FIXME */

  }

else
  {
  fprintf(outfile, "** Unknown command: %s", buffer);
  }

return PR_OK;
}



/*************************************************
*               Process pattern line             *
*************************************************/

/* This function is called when the input buffer contains the start of a
pattern. The first character is known to be a valid delimiter. The pattern is
read, modifiers are interpreted, and a suitable local context is set up for
this test. The pattern is then compiled.

Arguments:  none

Returns:    PR_OK     continue processing next line
            PR_SKIP   skip to a blank line
            PR_ABEND  abort the pcre2test run
*/

static int
process_pattern(void)
{
BOOL utf;
uint8_t *p = buffer;
const uint8_t *use_tables;
unsigned int delimiter = *p++;
int patlen, errorcode;
PCRE2_OFFSET erroroffset;

/* Initialize the context and pattern/data controls for this test from the
defaults. */

PATCTXCPY(pat_context, default_pat_context);
memcpy(&pat_patctl, &def_patctl, sizeof(patctl));

/* Find the end of the pattern, reading more lines if necessary. */

for(;;)
  {
  while (*p != 0)
    {
    if (*p == '\\' && p[1] != 0) p++;
      else if (*p == delimiter) break;
    p++;
    }
  if (*p != 0) break;
  if ((p = extend_inputline(infile, p, "    > ")) == NULL)
    {
    fprintf(outfile, "** Unexpected EOF\n");
    return PR_ABEND;
    }
  if (infile != stdin) fprintf(outfile, "%s", (char *)p);
  }

/* If the first character after the delimiter is backslash, make the pattern
end with backslash. This is purely to provide a way of testing for the error
message when a pattern ends with backslash. */

if (p[1] == '\\') *p++ = '\\';

/* Terminate the pattern at the delimiter, and compute the length. */

*p++ = 0;
patlen = p - buffer - 2;

/* Look for modifiers and options after the final delimiter. */

if (!decode_modifiers(p, CTX_PAT, &pat_patctl, NULL)) return PR_SKIP;
utf = (pat_patctl.options & PCRE2_UTF) != 0;

/* Now copy the pattern to pbuffer8 for use in 8-bit testing and for reflecting
in callouts. Convert to binary if required. */

if ((pat_patctl.control & CTL_HEXPAT) != 0)
  {
  uint8_t *pp, *pt;
  uint32_t c, d;

  if ((pat_patctl.control & CTL_POSIX) != 0)
    {
    fprintf(outfile, "** Hex patterns are not supported for the POSIX API\n");
    return PR_SKIP;
    }

  pt = pbuffer8;
  for (pp = buffer + 1; *pp != 0; pp++)
    {
    if (isspace(*pp)) continue;
    c = toupper(*pp++);
    if (*pp == 0)
      {
      fprintf(outfile, "** Odd number of digits in hex pattern.\n");
      return PR_SKIP;
      }
    d = toupper(*pp);
    if (!isxdigit(c) || !isxdigit(d))
      {
      fprintf(outfile, "** Non-hex-digit in hex pattern.\n");
      return PR_SKIP;
      }
    *pt++ = ((isdigit(c)? (c - '0') : (c - 'A' + 10)) << 4) +
             (isdigit(d)? (d - '0') : (d - 'A' + 10));
    }
  *pt = 0;
  patlen = pt - pbuffer8;
  }
else
  {
  strncpy((char *)pbuffer8, (char *)(buffer+1), patlen + 1);
  }

/* Sort out character tables */

if (pat_patctl.locale[0] != 0)
  {
  if (pat_patctl.tables_id != 0)
    {
    fprintf(outfile, "** 'Locale' and 'tables' must not both be set.\n");
    return PR_SKIP;
    }
  if (setlocale(LC_CTYPE, (const char *)pat_patctl.locale) == NULL)
    {
    fprintf(outfile, "** Failed to set locale '%s'\n", pat_patctl.locale);
    return PR_SKIP;
    }
  if (strcmp((const char *)pat_patctl.locale, (const char *)locale_name) != 0)
    {
    strcpy((char *)locale_name, (char *)pat_patctl.locale);
    if (locale_tables != NULL) free((void *)locale_tables);
    PCRE2_MAKETABLES(locale_tables);
    }
  use_tables = locale_tables;
  }

else switch (pat_patctl.tables_id)
  {
  case 0: use_tables = NULL; break;
  case 1: use_tables = tables1; break;
  case 2: use_tables = tables2; break;
  default:
  fprintf(outfile, "** 'Tables' must specify 0, 1, or 2.\n");
  return PR_SKIP;
  }

PCRE2_SET_CHARACTER_TABLES(pat_context, use_tables);

/* Handle compiling via the POSIX interface, which doesn't support the
timing, showing, or debugging options, nor the ability to pass over
local character tables. Neither does it have 16-bit or 32-bit support. */

if ((pat_patctl.control & CTL_POSIX) != 0)
  {
  int rc;
  int cflags = 0;
  const char *msg = "** Ignored with POSIX interface:";

  if (test_mode != 8)
    {
    fprintf(outfile, "** The POSIX interface is available only in 8-bit mode\n");
    return PR_SKIP;
    }

#ifdef SUPPORT_PCRE8
  /* Check for features that the POSIX interface does not support. */

  if (pat_patctl.locale[0] != 0) prmsg(&msg, "locale");
  if (pat_patctl.tables_id != 0) prmsg(&msg, "tables");
  if (pat_patctl.stackguard_test != 0) prmsg(&msg, "stackguard");
  if (timeit > 0) prmsg(&msg, "timing");
  if (pat_patctl.jit != 0) prmsg(&msg, "JIT");
  if (pat_patctl.save[0] != 0) prmsg(&msg, "save");

  if ((pat_patctl.options & ~POSIX_SUPPORTED_COMPILE_OPTIONS) != 0)
    {
    show_compile_options(
      pat_patctl.options & ~POSIX_SUPPORTED_COMPILE_OPTIONS, msg, "");
    msg = "";
    }
  if ((pat_patctl.control & ~POSIX_SUPPORTED_COMPILE_CONTROLS) != 0)
    {
    show_compile_controls(
      pat_patctl.control & ~POSIX_SUPPORTED_COMPILE_CONTROLS, msg, "");
    msg = "";
    }

  if (msg[0] == 0) fprintf(outfile, "\n");

  /* Translate PCRE2 options to POSIX options and then compile. On success, set
  up a match_data block to be used for all matches. */

  if (utf) cflags |= REG_UTF;
  if ((pat_patctl.options & PCRE2_UCP) != 0) cflags |= REG_UCP;
  if ((pat_patctl.options & PCRE2_CASELESS) != 0) cflags |= REG_ICASE;
  if ((pat_patctl.options & PCRE2_MULTILINE) != 0) cflags |= REG_NEWLINE;
  if ((pat_patctl.options & PCRE2_DOTALL) != 0) cflags |= REG_DOTALL;
  if ((pat_patctl.options & PCRE2_NO_AUTO_CAPTURE) != 0) cflags |= REG_NOSUB;
  if ((pat_patctl.options & PCRE2_UNGREEDY) != 0) cflags |= REG_UNGREEDY;

  rc = regcomp(&preg, (char *)pbuffer8, cflags);
  if (rc != 0)   /* Failure */
    {
    (void)regerror(rc, &preg, (char *)pbuffer8, pbuffer8_size);
    fprintf(outfile, "Failed: POSIX code %d: %s\n", rc, pbuffer8);
    return PR_SKIP;
    }
  return PR_OK;
#endif  /* SUPPORT_PCRE8 */
  }

/* Handle compiling via the native interface, converting the input in non-8-bit
modes. */

#ifdef SUPPORT_PCRE16
if (test_mode == PCRE16_MODE)
  patlen = to16(pbuffer8, utf, patlen);
#endif

#ifdef SUPPORT_PCRE32
if (test_mode == PCRE32_MODE)
  patlen = to32(pbuffer8, utf, patlen);
#endif

switch(patlen)
  {
  case -1:
  fprintf(outfile, "** Failed: invalid UTF-8 string cannot be "
    "converted to %d-bit string\n", (test_mode == PCRE16_MODE)? 16:32);
  return PR_SKIP;

  case -2:
  fprintf(outfile, "** Failed: character value greater than 0x10ffff "
    "cannot be converted to UTF\n");
  return PR_SKIP;

  case -3:
  fprintf(outfile, "** Failed: character value greater than 0xffff "
    "cannot be converted to 16-bit in non-UTF mode\n");
  return PR_SKIP;

  default:
  break;
  }

/* The pattern in now in pbuffer[8|16|32], with the length in patlen. By
default, however, we pass a zero-terminated pattern. The length is passed only
if we had a hex pattern or if use_length was set. */

if ((pat_patctl.control & (CTL_PATLEN|CTL_HEXPAT)) == 0) patlen = -1;

/* Compile many times when timing. */

if (timeit > 0)
  {
  register int i;
  clock_t time_taken;
  clock_t start_time = clock();
  for (i = 0; i < timeit; i++)
    {
    PCRE2_COMPILE(compiled_code, pbuffer, patlen,
      pat_patctl.options, &errorcode, &erroroffset, pat_context);
    if (TEST(compiled_code, !=, NULL))
      { SUB1(pcre2_code_free, compiled_code); }
    }
  total_compile_time += (time_taken = clock() - start_time);
  fprintf(outfile, "Compile time %.4f milliseconds\n",
    (((double)time_taken * 1000.0) / (double)timeit) /
      (double)CLOCKS_PER_SEC);
  }

/* FIXME: implement timing for JIT compile. */

/* A final compile that is used "for real". */

PCRE2_COMPILE(compiled_code, pbuffer, patlen, pat_patctl.options, &errorcode,
  &erroroffset, pat_context);

/* Compilation failed; go back for another re, skipping to blank line
if non-interactive. */

if (TEST(compiled_code, ==, NULL))
  {
  int len;
  fprintf(outfile, "Failed: error %d at offset %d: ", errorcode,
    (int)erroroffset);
  PCRE2_GET_ERROR_MESSAGE(len, errorcode, pbuffer);
  PCHARSV(CASTVAR(void *, pbuffer), 0, len, FALSE, outfile);
  fprintf(outfile, "\n");
  return PR_SKIP;
  }

/* Call the JIT compiler if requested. */

if (pat_patctl.jit != 0)
  { PCRE2_JIT_COMPILE(compiled_code, pat_patctl.jit); }

/* Output code size and other information if requested. */

if ((pat_patctl.control & CTL_MEMORY) != 0)
  {
  int name_count;
  size_t size, cblock_size, name_entry_size;

#ifdef SUPPORT_PCRE8
  if (test_mode == 8) cblock_size = sizeof(pcre2_real_code_8);
#endif
#ifdef SUPPORT_PCRE16
  if (test_mode == 16) cblock_size = sizeof(pcre2_real_code_16);
#endif
#ifdef SUPPORT_PCRE32
  if (test_mode == 32) cblock_size = sizeof(pcre2_real_code_32);
#endif

  (void)pattern_info(PCRE2_INFO_SIZE, &size);
  (void)pattern_info(PCRE2_INFO_NAMECOUNT, &name_count);
  (void)pattern_info(PCRE2_INFO_NAMEENTRYSIZE, &name_entry_size);
  fprintf(outfile, "Memory allocation (code space): %d\n",
    (int)(size - name_count*name_entry_size*code_unit_size - cblock_size));
  if (pat_patctl.jit != 0)
    {
    (void)pattern_info(PCRE2_INFO_JITSIZE, &size);
    fprintf(outfile, "Memory allocation (JIT code): %d\n", (int)size);
    }
  }

if ((pat_patctl.control & CTL_ANYINFO) != 0)
  {
  int rc = show_pattern_info();
  if (rc != PR_OK) return rc;
  }


#ifdef FIXME

/* If the '>' option was present, we write out the regex to a file, and
that is all. The first 8 bytes of the file are the regex length and then
the study length, in big-endian order. */

if (to_file != NULL)
  {
  FILE *f = fopen((char *)to_file, "wb");
  if (f == NULL)
    {
    fprintf(outfile, "Unable to open %s: %s\n", to_file, strerror(errno));
    }
  else
    {
    uint8_t sbuf[8];

/* Extract the size for possible writing before possibly flipping it,
and remember the store that was got. */

true_size = REAL_PCRE_SIZE(re);

    if (do_flip) regexflip(re, extra);
    sbuf[0] = (uint8_t)((true_size >> 24) & 255);
    sbuf[1] = (uint8_t)((true_size >> 16) & 255);
    sbuf[2] = (uint8_t)((true_size >>  8) & 255);
    sbuf[3] = (uint8_t)((true_size) & 255);
    sbuf[4] = (uint8_t)((true_study_size >> 24) & 255);
    sbuf[5] = (uint8_t)((true_study_size >> 16) & 255);
    sbuf[6] = (uint8_t)((true_study_size >>  8) & 255);
    sbuf[7] = (uint8_t)((true_study_size) & 255);

    if (fwrite(sbuf, 1, 8, f) < 8 ||
        fwrite(re, 1, true_size, f) < true_size)
      {
      fprintf(outfile, "Write error on %s: %s\n", to_file, strerror(errno));
      }
    else
      {
      fprintf(outfile, "Compiled pattern written to %s\n", to_file);

      /* If there is study data, write it. */

      if (extra != NULL)
        {
        if (fwrite(extra->study_data, 1, true_study_size, f) <
            true_study_size)
          {
          fprintf(outfile, "Write error on %s: %s\n", to_file,
            strerror(errno));
          }
        else fprintf(outfile, "Study data written to %s\n", to_file);
        }
      }
    fclose(f);
    }

  new_free(re);
  if (extra != NULL)
    {
    PCRE_FREE_STUDY(extra);
    }
  continue;  /* With next regex */
  }

#endif /* FIXME */

return PR_OK;
}



/*************************************************
*        Check match or recursion limit          *
*************************************************/

static int
check_match_limit(uint8_t *pp, size_t ulen, int errnumber, const char *msg)
{
int capcount;
uint32_t min = 0;
uint32_t mid = 64;
uint32_t max = UINT32_MAX;

PCRE2_SET_MATCH_LIMIT(dat_context, max);
PCRE2_SET_RECURSION_LIMIT(dat_context, max);

for (;;)
  {
  if (errnumber == PCRE2_ERROR_MATCHLIMIT)
    {
    PCRE2_SET_MATCH_LIMIT(dat_context, mid);
    }
  else
    {
    PCRE2_SET_RECURSION_LIMIT(dat_context, mid);
    }

  PCRE2_MATCH(capcount, compiled_code, pp, ulen, dat_datctl.offset,
    dat_datctl.options, match_data, dat_context);

  if (capcount == errnumber)
    {
    min = mid;
    mid = (mid == max - 1)? max : (max != UINT32_MAX)? (min + max)/2 : mid*2;
    }

  else if (capcount >= 0 ||
           capcount == PCRE2_ERROR_NOMATCH ||
           capcount == PCRE2_ERROR_PARTIAL)
    {
    if (mid == min + 1)
      {
      if (capcount != PCRE2_ERROR_NOMATCH)
        fprintf(outfile, "Minimum %s limit = %d\n", msg, mid);
      break;
      }
    max = mid;
    mid = (min + mid)/2;
    }
  else break;    /* Some other error */
  }

return capcount;
}



/*************************************************
*              Callout function                  *
*************************************************/

/* Called from PCRE as a result of the (?C) item. We print out where we are in
the match. Yield zero unless more callouts than the fail count, or the callout
data is not zero. The only differences in the callout block for different code
unit widths are that the pointers to the subject and the most recent MARK point
to strings of the appropriate width. Casts can be used to deal with this.

Argument:  a pointer to a callout block
Return:
*/

static int
callout_function(pcre2_callout_block_8 *cb)
{
uint32_t i, pre_start, post_start, subject_length;
BOOL utf = (FLD(compiled_code, compile_options) & PCRE2_UTF) != 0;
BOOL callout_capture = (dat_datctl.control & CTL_CALLOUT_CAPTURE) != 0;
FILE *f = (first_callout || callout_capture)? outfile : NULL;

if (callout_capture)
  {
  fprintf(f, "Callout %d: last capture = %d\n",
    cb->callout_number, cb->capture_last);

  for (i = 0; i < cb->capture_top * 2; i += 2)
    {
    if (cb->offset_vector[i] == PCRE2_UNSET)
      fprintf(f, "%2d: <unset>\n", i/2);
    else
      {
      fprintf(f, "%2d: ", i/2);
      PCHARSV(cb->subject, cb->offset_vector[i],
        cb->offset_vector[i+1] - cb->offset_vector[i], utf, f);
      fprintf(f, "\n");
      }
    }
  }

/* Re-print the subject in canonical form, the first time or if giving full
datails. On subsequent calls in the same match, we use pchars just to find the
printed lengths of the substrings. */

if (f != NULL) fprintf(f, "--->");

PCHARS(pre_start, cb->subject, 0, cb->start_match, utf, f);

PCHARS(post_start, cb->subject, cb->start_match,
  cb->current_position - cb->start_match, utf, f);

PCHARS(subject_length, cb->subject, 0, cb->subject_length, utf, NULL);

PCHARSV(cb->subject, cb->current_position,
  cb->subject_length - cb->current_position, utf, f);

if (f != NULL) fprintf(f, "\n");

/* Always print appropriate indicators, with callout number if not already
shown. For automatic callouts, show the pattern offset. */

if (cb->callout_number == 255)
  {
  fprintf(outfile, "%+3d ", (int)cb->pattern_position);
  if (cb->pattern_position > 99) fprintf(outfile, "\n    ");
  }
else
  {
  if (callout_capture) fprintf(outfile, "    ");
    else fprintf(outfile, "%3d ", cb->callout_number);
  }

for (i = 0; i < pre_start; i++) fprintf(outfile, " ");
fprintf(outfile, "^");

if (post_start > 0)
  {
  for (i = 0; i < post_start - 1; i++) fprintf(outfile, " ");
  fprintf(outfile, "^");
  }

for (i = 0; i < subject_length - pre_start - post_start + 4; i++)
  fprintf(outfile, " ");

fprintf(outfile, "%.*s",
  (int)((cb->next_item_length == 0)? 1 : cb->next_item_length),
  pbuffer8 + cb->pattern_position);

fprintf(outfile, "\n");
first_callout = 0;

if (cb->mark != last_callout_mark)
  {
  if (cb->mark == NULL)
    fprintf(outfile, "Latest Mark: <unset>\n");
  else
    {
    fprintf(outfile, "Latest Mark: ");
    PCHARSV(cb->mark, 0, -1, utf, outfile);
    putc('\n', outfile);
    }
  last_callout_mark = cb->mark;
  }

if (cb->callout_data != NULL)
  {
  int callout_data = *((int32_t *)(cb->callout_data));
  if (callout_data != 0)
    {
    fprintf(outfile, "Callout data = %d\n", callout_data);
    return callout_data;
    }
  }

return (cb->callout_number != dat_datctl.cfail[0])? 0 :
       (++callout_count >= dat_datctl.cfail[1])? 1 : 0;
}



/*************************************************
*               Process a data line              *
*************************************************/

/* The line is in buffer; it will not be empty.

Arguments:  none

Returns:    PR_OK     continue processing next line
            PR_SKIP   skip to a blank line
            PR_ABEND  abort the pcre2test run
*/

static int
process_data(void)
{
size_t len, ulen;
uint32_t gmatched;
uint32_t c;
uint32_t g_notempty = 0;
uint8_t *p, *pp, *start_rep;
size_t needlen;
BOOL utf;

#ifdef SUPPORT_PCRE8
uint8_t *q8 = NULL;
#endif
#ifdef SUPPORT_PCRE16
uint16_t *q16 = NULL;
#endif
#ifdef SUPPORT_PCRE32
uint32_t *q32 = NULL;
#endif

/* Copy the default context and data control blocks to the active ones. Then
copy from the pattern the controls that can be set in either the pattern or the
data. This allows them to be unset in the data line. We do not do this for
options because those that are common apply separately to compiling and
matching. */

DATCTXCPY(dat_context, default_dat_context);
memcpy(&dat_datctl, &def_datctl, sizeof(datctl));
dat_datctl.control |= (pat_patctl.control & CTL_ALLPD);

/* Initialize for scanning the data line. */

utf = (pat_patctl.control & CTL_POSIX) == 0 &&
  (FLD(compiled_code, compile_options) & PCRE2_UTF) != 0;
start_rep = NULL;
len = strlen((const char *)buffer);
while (len > 0 && isspace(buffer[len-1])) len--;
buffer[len] = 0;
p = buffer;
while (isspace(*p)) p++;

/* Check that the data is well-formed UTF-8 if we're in UTF mode. To create
invalid input to pcre2_exec, you must use \x?? or \x{} sequences. */

if (utf)
  {
  uint8_t *q;
  uint32_t cc;
  int n = 1;
  for (q = p; n > 0 && *q; q += n) n = utf82ord(q, &cc);
  if (n <= 0)
    {
    fprintf(outfile, "** Failed: invalid UTF-8 string cannot be used as input "
      "in UTF mode\n");
    return PR_OK;
    }
  }

#ifdef SUPPORT_VALGRIND
/* Mark the dbuffer as addressable but undefined again. */
if (dbuffer != NULL)
  {
  VALGRIND_MAKE_MEM_UNDEFINED(dbuffer, dbuffer_size);
  }
#endif

/* Allocate a buffer to hold the data line; len+1 is an upper bound on
the number of code units that will be needed. */

needlen = (size_t)(len * code_unit_size);
while (dbuffer == NULL || needlen >= dbuffer_size)
  {
  dbuffer_size *= 2;
  dbuffer = (uint8_t *)realloc(dbuffer, dbuffer_size);
  if (dbuffer == NULL)
    {
    fprintf(stderr, "pcre2test: realloc(%d) failed\n", (int)dbuffer_size);
    exit(1);
    }
  }
SETCASTPTR(q, dbuffer);  /* Sets q8, q16, or q32, as appropriate. */

/* Scan the data line, interpreting data escapes, and put the result into a
buffer the appropriate width buffer. In UTF mode, input can be UTF-8. */

while ((c = *p++) != 0)
  {
  int i = 0;
  size_t replen;

  /* ] may mark the end of a replicated sequence */

  if (c == ']' && start_rep != NULL)
    {
    size_t qoffset = CAST8VAR(q) - (uint8_t *)dbuffer;

    if (*p++ != '{')
      {
      fprintf(outfile, "** Expected '{' after \\[....]\n");
      return PR_OK;
      }
    while (isdigit(*p)) i = i * 10 + *p++ - '0';
    if (*p++ != '}')
      {
      fprintf(outfile, "** Expected '}' after \\[...]{...\n");
      return PR_OK;
      }
    if (i-- == 0)
      {
      fprintf(outfile, "** Zero repeat not allowed\n");
      return PR_OK;
      }

    replen = CAST8VAR(q) - start_rep;
    needlen += replen * (i - 1);
    while (needlen >= dbuffer_size)
      {
      dbuffer_size *= 2;
      dbuffer = (uint8_t *)realloc(dbuffer, dbuffer_size);
      if (dbuffer == NULL)
        {
        fprintf(stderr, "pcre2test: realloc(%d) failed\n", (int)dbuffer_size);
        exit(1);
        }
      SETCASTPTR(q, dbuffer + qoffset);
      }

    while (i-- > 0)
      {
      memcpy(CAST8VAR(q), start_rep, replen);
      SETPLUS(q, replen/code_unit_size);
      }

    start_rep = NULL;
    continue;
    }

  /* Handle a non-escaped character */

  if (c != '\\')
    {
    if (utf && HASUTF8EXTRALEN(c)) { GETUTF8INC(c, p); }
    }

  /* Handle backslash escapes */

  else switch ((c = *p++))
    {
    case '\\': break;
    case 'a': c =    7; break;
    case 'b': c = '\b'; break;
    case 'e': c =   27; break;
    case 'f': c = '\f'; break;
    case 'n': c = '\n'; break;
    case 'r': c = '\r'; break;
    case 't': c = '\t'; break;
    case 'v': c = '\v'; break;

    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7':
    c -= '0';
    while (i++ < 2 && isdigit(*p) && *p != '8' && *p != '9')
      c = c * 8 + *p++ - '0';
    break;

    case 'o':
    if (*p == '{')
      {
      uint8_t *pt = p;
      c = 0;
      for (pt++; isdigit(*pt) && *pt != '8' && *pt != '9'; pt++)
        {
        if (++i == 12)
          fprintf(outfile, "** Too many octal digits in \\o{...} item; "
                           "using only the first twelve.\n");
        else c = c * 8 + *pt - '0';
        }
      if (*pt == '}') p = pt + 1;
        else fprintf(outfile, "** Missing } after \\o{ (assumed)\n");
      }
    break;

    case 'x':
    if (*p == '{')
      {
      uint8_t *pt = p;
      c = 0;

      /* We used to have "while (isxdigit(*(++pt)))" here, but it fails
      when isxdigit() is a macro that refers to its argument more than
      once. This is banned by the C Standard, but apparently happens in at
      least one MacOS environment. */

      for (pt++; isxdigit(*pt); pt++)
        {
        if (++i == 9)
          fprintf(outfile, "** Too many hex digits in \\x{...} item; "
                           "using only the first eight.\n");
        else c = c * 16 + tolower(*pt) - ((isdigit(*pt))? '0' : 'a' - 10);
        }
      if (*pt == '}')
        {
        p = pt + 1;
        break;
        }
      /* Not correct form for \x{...}; fall through */
      }

    /* \x without {} always defines just one byte in 8-bit mode. This
    allows UTF-8 characters to be constructed byte by byte, and also allows
    invalid UTF-8 sequences to be made. Just copy the byte in UTF-8 mode.
    Otherwise, pass it down as data. */

    c = 0;
    while (i++ < 2 && isxdigit(*p))
      {
      c = c * 16 + tolower(*p) - ((isdigit(*p))? '0' : 'a' - 10);
      p++;
      }
#if defined SUPPORT_PCRE8
    if (utf && (test_mode == PCRE8_MODE))
      {
      *q8++ = c;
      continue;
      }
#endif
    break;

    case 0:     /* \ followed by EOF allows for an empty line */
    p--;
    continue;

    case '=':   /* \= terminates the data, starts modifiers */
    goto ENDSTRING;

    case '[':   /* \[ introduces a replicated character sequence */
    if (start_rep != NULL)
      {
      fprintf(outfile, "** Nested replication is not supported\n");
      return PR_OK;
      }
    start_rep = CAST8VAR(q);
    continue;

    default:
    fprintf(outfile, "** Unrecognized escape sequence \"\\%c\"\n", c);
    return PR_OK;
    }

  /* We now have a character value in c that may be greater than 255.
  In 8-bit mode we convert to UTF-8 if we are in UTF mode. Values greater
  than 127 in UTF mode must have come from \x{...} or octal constructs
  because values from \x.. get this far only in non-UTF mode. */

#ifdef SUPPORT_PCRE8
  if (test_mode == PCRE8_MODE)
    {
    if (utf)
      {
      if (c > 0x7fffffff)
        {
        fprintf(outfile, "** Character \\x{%x} is greater than 0x7fffffff "
          "and so cannot be converted to UTF-8\n", c);
        return PR_OK;
        }
      q8 += ord2utf8(c, q8);
      }
    else
      {
      if (c > 0xffu)
        {
        fprintf(outfile, "** Character \\x{%x} is greater than 255 "
          "and UTF-8 mode is not enabled.\n", c);
        fprintf(outfile, "** Truncation will probably give the wrong "
          "result.\n");
        }
      *q8++ = c;
      }
    }
#endif
#ifdef SUPPORT_PCRE16
  if (test_mode == PCRE16_MODE)
    {
    if (utf)
      {
      if (c > 0x10ffffu)
        {
        fprintf(outfile, "** Failed: character \\x{%x} is greater than "
          "0x10ffff and so cannot be converted to UTF-16\n", c);
        return PR_OK;
        }
      else if (c >= 0x10000u)
        {
        c-= 0x10000u;
        *q16++ = 0xD800 | (c >> 10);
        *q16++ = 0xDC00 | (c & 0x3ff);
        }
      else
        *q16++ = c;
      }
    else
      {
      if (c > 0xffffu)
        {
        fprintf(outfile, "** Character \\x{%x} is greater than 0xffff "
          "and UTF-16 mode is not enabled.\n", c);
        fprintf(outfile, "** Truncation will probably give the wrong "
          "result.\n");
        }

      *q16++ = c;
      }
    }
#endif
#ifdef SUPPORT_PCRE32
  if (test_mode == PCRE32_MODE)
    {
    *q32++ = c;
    }
#endif
  }

ENDSTRING:
SET(*q, 0);
len = CASTVAR(uint8_t *, q) - dbuffer;    /* Length in bytes */
ulen = len/code_unit_size;                /* Length in code units */

/* If we have explicit valgrind support, mark the data from after its end to
the end of the buffer as unaddressable, so that a read over the end of the
buffer will be seen by valgrind, even if it doesn't cause a crash. If we're not
building with valgrind support, at least move the data to the end of the buffer
so that it might at least cause a crash. If we are using the POSIX interface,
we must include the terminating zero. */

pp = dbuffer;
c = code_unit_size * ((pat_patctl.control & CTL_POSIX) != 0)? 1:0;

#ifdef SUPPORT_VALGRIND
  VALGRIND_MAKE_MEM_NOACCESS(dbuffer + len + c, dbuffer_size - (len + c));
#else
  pp = memmove(pp + dbuffer_size - len - c, pp, len + c);
#endif

/* If the string was terminated by \= we must now interpret modifiers. */

if (p[-1] != 0 && !decode_modifiers(p, CTX_DAT, NULL, &dat_datctl))
  return PR_OK;

/* Now run the pattern match: len contains the byte length, ulen contains the
code unit length, and pp points to the subject string. POSIX matching is only
possible in 8-bit mode, and it does not support timing or other fancy features.
Some were checked at compile time, but we need to check the match-time settings
here. */

if ((pat_patctl.control & CTL_POSIX) != 0)
  {
  int rc;
  int eflags = 0;
  regmatch_t *pmatch = NULL;
  const char *msg = "** Ignored with POSIX interface:";

  if (dat_datctl.cfail[0] != CFAIL_UNSET || dat_datctl.cfail[1] != CFAIL_UNSET)
    prmsg(&msg, "callout_fail");
  if (dat_datctl.copy_numbers[0] >= 0 || dat_datctl.copy_names[0] != 0)
    prmsg(&msg, "copy");
  if (dat_datctl.get_numbers[0] >= 0 || dat_datctl.get_names[0] != 0)
    prmsg(&msg, "get");
  if (dat_datctl.jitstack != 0) prmsg(&msg, "jitstack");

  if ((dat_datctl.options & ~POSIX_SUPPORTED_MATCH_OPTIONS) != 0)
    {
    fprintf(outfile, "%s", msg);
    show_match_options(dat_datctl.options & ~POSIX_SUPPORTED_MATCH_OPTIONS);
    msg = "";
    }
  if ((dat_datctl.control & ~POSIX_SUPPORTED_MATCH_CONTROLS) != 0)
    {
    fprintf(outfile, "%s", msg);
    show_match_controls(dat_datctl.control & ~POSIX_SUPPORTED_MATCH_CONTROLS);
    msg = "";
    }

  if (msg[0] == 0) fprintf(outfile, "\n");

  if (dat_datctl.oveccount > 0)
    pmatch = (regmatch_t *)malloc(sizeof(regmatch_t) * dat_datctl.oveccount);
  if ((dat_datctl.options & PCRE2_NOTBOL) != 0) eflags |= REG_NOTBOL;
  if ((dat_datctl.options & PCRE2_NOTEOL) != 0) eflags |= REG_NOTEOL;
  if ((dat_datctl.options & PCRE2_NOTEMPTY) != 0) eflags |= REG_NOTEMPTY;

  rc = regexec(&preg, (const char *)pp + dat_datctl.offset,
    dat_datctl.oveccount, pmatch, eflags);
  if (rc != 0)
    {
    (void)regerror(rc, &preg, (char *)pbuffer8, pbuffer8_size);
    fprintf(outfile, "No match: POSIX code %d: %s\n", rc, pbuffer8);
    }
  else if ((pat_patctl.options & PCRE2_NO_AUTO_CAPTURE) != 0)
    fprintf(outfile, "Matched with REG_NOSUB\n");
  else if (dat_datctl.oveccount == 0)
    fprintf(outfile, "Matched without capture\n");
  else
    {
    size_t i;
    for (i = 0; i < (size_t)dat_datctl.oveccount; i++)
      {
      if (pmatch[i].rm_so >= 0)
        {
        fprintf(outfile, "%2d: ", (int)i);
        PCHARSV(dbuffer, pmatch[i].rm_so,
          pmatch[i].rm_eo - pmatch[i].rm_so, FALSE, outfile);
        fprintf(outfile, "\n");
        if ((i == 0 && (dat_datctl.control & CTL_AFTERTEXT) != 0) ||
            (dat_datctl.control & CTL_ALLAFTERTEXT) != 0)
          {
          fprintf(outfile, "%2d+ ", (int)i);
          PCHARSV(dbuffer, pmatch[i].rm_eo, len - pmatch[i].rm_eo,
            FALSE, outfile);
          fprintf(outfile, "\n");
          }
        }
      }
    }
  free(pmatch);
  return PR_OK;
  }

 /* Handle matching via the native interface. Check for consistency of
modifiers. */

if ((dat_datctl.control & (CTL_DFA|CTL_FINDLIMITS)) == (CTL_DFA|CTL_FINDLIMITS))
  {
  printf("** Finding match limits is not relevant for DFA matching: ignored\n");
  dat_datctl.control &= ~CTL_FINDLIMITS;
  }

if ((dat_datctl.control & CTL_ANYGLOB) != 0 && dat_datctl.oveccount < 1)
  {
  printf("** Global matching requires a non-zero ovector count: ignored\n");
  dat_datctl.control &= ~CTL_ANYGLOB;
  }

/* Enable display of malloc/free if wanted. */

show_memory = (dat_datctl.control & CTL_MEMORY) != 0;

/* Ensure that there is a JIT callback if we want to verify that JIT was
actually used. If jit_stack == NULL, no stack has yet been assigned. */

#ifdef FIXME
if ((dat_datctl.control & CTL_JITVERIFY) != 0 &&

  jit_stack == NULL && extra != NULL)
   { PCRE2_ASSIGN_JIT_STACK(extra, jit_callback, jit_stack); }
#endif


/* Loop for global matching */

for (gmatched = 0;; gmatched++)
  {
  int capcount;

#ifdef FIXME
  jit_was_used = FALSE;
#endif

  /* Adjust match_data according to size of offsets required. */

  if (dat_datctl.oveccount <= max_oveccount)
    {
    SETFLD(match_data, oveccount, dat_datctl.oveccount);
    }
  else
    {
    max_oveccount = dat_datctl.oveccount;
    PCRE2_MATCH_DATA_FREE(match_data);
    PCRE2_MATCH_DATA_CREATE(match_data, max_oveccount, NULL);
    }

  /* Do timing if required. */

  if (timeitm > 0)
    {
    register int i;
    clock_t time_taken;
    clock_t start_time = clock();

    if ((dat_datctl.control & CTL_DFA) != 0)
      {
      if ((dat_datctl.options & PCRE2_DFA_RESTART) != 0)
        {
        fprintf(outfile, "Timing DFA restarts is not supported\n");
        return PR_OK;
        }
      if (dfa_workspace == NULL)
        dfa_workspace = (int *)malloc(DFA_WS_DIMENSION*sizeof(int));
      for (i = 0; i < timeitm; i++)
        {
        PCRE2_DFA_MATCH(capcount, compiled_code, pp, ulen,
          dat_datctl.offset, dat_datctl.options | g_notempty, match_data,
          dat_context, dfa_workspace, DFA_WS_DIMENSION);
        }
      }
    else for (i = 0; i < timeitm; i++)
      {
      PCRE2_MATCH(capcount, compiled_code, pp, ulen,
        dat_datctl.offset, dat_datctl.options | g_notempty, match_data,
        dat_context);
      }
    total_match_time += (time_taken = clock() - start_time);
    fprintf(outfile, "Match time %.4f milliseconds\n",
      (((double)time_taken * 1000.0) / (double)timeitm) /
        (double)CLOCKS_PER_SEC);
    }

  /* Find the match and recursion limits if requested. */

  if ((dat_datctl.control & CTL_FINDLIMITS) != 0)
    {
    (void)check_match_limit(pp, ulen, PCRE2_ERROR_MATCHLIMIT, "match");
    capcount = check_match_limit(pp, ulen, PCRE2_ERROR_RECURSIONLIMIT,
      "recursion");
    }

  /* Otherwise just run a single match, setting up a callout if required (the
  default). */

  else
    {
    if ((dat_datctl.control & CTL_CALLOUT_NONE) == 0)
      {
      PCRE2_SET_CALLOUT(dat_context, callout_function,
        (void *)(&dat_datctl.callout_data));
      first_callout = TRUE;
      last_callout_mark = NULL;
      callout_count = 0;
      }
    else
      {
      PCRE2_SET_CALLOUT(dat_context, NULL, NULL);  /* No callout */
      }

    /* Run a single DFA or NFA match. */

    if ((dat_datctl.control & CTL_DFA) != 0)
      {
      if (dfa_workspace == NULL)
        dfa_workspace = (int *)malloc(DFA_WS_DIMENSION*sizeof(int));
      if (dfa_matched++ == 0)
        dfa_workspace[0] = -1;  /* To catch bad restart */
      PCRE2_DFA_MATCH(capcount, compiled_code, pp, ulen,
        dat_datctl.offset, dat_datctl.options | g_notempty, match_data,
        dat_context, dfa_workspace, DFA_WS_DIMENSION);
      if (capcount == 0)
        {
        fprintf(outfile, "Matched, but offsets vector is too small to show all matches\n");
        capcount = dat_datctl.oveccount;
        }
      }
    else
      {
      PCRE2_MATCH(capcount, compiled_code, pp, ulen, dat_datctl.offset,
        dat_datctl.options | g_notempty, match_data, dat_context);
      if (capcount == 0)
        {
        fprintf(outfile, "Matched, but too many substrings\n");
        capcount = dat_datctl.oveccount;
        }
      }
    }

  /* The result of the match is in now capcount. First handle a successful
  match. */

  if (capcount >= 0)
    {
    int i;
    uint8_t *nptr;
    PCRE2_OFFSET *ovector;

    /* This is a check against a lunatic return value. */

    if (capcount > (int)dat_datctl.oveccount)
      {
      fprintf(outfile,
        "** PCRE error: returned count %d is too big for ovector count %d\n",
        capcount, dat_datctl.oveccount);
      capcount = dat_datctl.oveccount;
      if ((dat_datctl.control & CTL_ANYGLOB) != 0)
        {
        fprintf(outfile, "** Global loop abandoned\n");
        pat_patctl.options &= ~CTL_ANYGLOB;        /* Break g/G loop */
        }
      }

    /* "allcaptures" requests showing of all captures in the pattern, to check
    unset ones at the end. It may be set on the pattern or the data. Implement
    by setting capcount to the maximum. */

    if ((dat_datctl.control & CTL_ALLCAPTURES) != 0)
      {
      if (pattern_info(PCRE2_INFO_CAPTURECOUNT, &capcount) < 0)
        return PR_SKIP;
      capcount++;   /* Allow for full match */
      if (capcount > (int)dat_datctl.oveccount) capcount = dat_datctl.oveccount;
      }

    /* Output the captured substrings. Note that, for the matched string,
    the use of \K in an assertion can make the start later than the end. */

    ovector = FLD(match_data, ovector);
    for (i = 0; i < 2*capcount; i += 2)
      {
      PCRE2_OFFSET start = ovector[i];
      PCRE2_OFFSET end = ovector[i+1];

      if (start > end)
        {
        start = ovector[i+1];
        end = ovector[i];
        fprintf(outfile, "Start of matched string is beyond its end - "
          "displaying from end to start.\n");
        }

      fprintf(outfile, "%2d: ", i/2);
      if (start == PCRE2_UNSET)
        {
        fprintf(outfile, "<unset>\n");
        continue;
        }
      PCHARSV(pp, start, end - start, utf, outfile);
#ifdef FIXME
      if (verify_jit && jit_was_used) fprintf(outfile, " (JIT)");
#endif
      fprintf(outfile, "\n");

      /* Note: don't use the start/end variables here because we want to
      show the text from what is reported as the end. */

      if ((dat_datctl.control & CTL_ALLAFTERTEXT) != 0 ||
          (i == 0 && (dat_datctl.control & CTL_AFTERTEXT) != 0))
        {
        fprintf(outfile, "%2d+ ", i/2);
        PCHARSV(pp, ovector[i+1], ulen - ovector[i+1], utf, outfile);
        fprintf(outfile, "\n");
        }
      }

    /* Output mark data if requested. */

    if ((dat_datctl.control & CTL_MARK) != 0 &&
         TESTFLD(match_data, mark, !=, NULL))
      {
      fprintf(outfile, "MK: ");
      PCHARSV(CASTFLD(void *, match_data, mark), 0, -1, utf, outfile);
      fprintf(outfile, "\n");
      }

    /* Test copy strings by number */

    for (i = 0; i < MAXCPYGET && dat_datctl.copy_numbers[i] >= 0; i++)
      {
      int rc;
      uint32_t copybuffer[256];
      uint32_t n = (uint32_t)(dat_datctl.copy_numbers[i]);
      PCRE2_SUBSTRING_COPY_BYNUMBER(rc, match_data, n, copybuffer,
        sizeof(copybuffer)/code_unit_size);
      if (rc < 0)
        fprintf(outfile, "copy substring %d failed %d\n", n, rc);
      else
        {
        fprintf(outfile, "%2dC ", n);
        PCHARSV(copybuffer, 0, rc, utf, outfile);
        fprintf(outfile, " (%d)\n", rc);
        }
      }

    /* Test copy strings by name */

    nptr = dat_datctl.copy_names;
    for (;;)
      {
      int rc;
      uint32_t copybuffer[256];
      int namelen = strlen((const char *)nptr);
      if (namelen == 0) break;

#ifdef SUPPORT_PCRE8
      if (test_mode == PCRE8_MODE) strcpy((char *)pbuffer8, (char *)nptr);
#endif
#ifdef SUPPORT_PCRE16
      if (test_mode == PCRE16_MODE)(void)to16(nptr, utf, namelen);
#endif
#ifdef SUPPORT_PCRE32
      if (test_mode == PCRE32_MODE)(void)to32(nptr, utf, namelen);
#endif

      PCRE2_SUBSTRING_COPY_BYNAME(rc, match_data, pbuffer,
        copybuffer, sizeof(copybuffer)/code_unit_size);
      if (rc < 0)
        {
        fprintf(outfile, "copy substring '%s' failed %d\n", nptr, rc);
        }
      else
        {
        fprintf(outfile, "  C ");
        PCHARSV(copybuffer, 0, rc, utf, outfile);
        fprintf(outfile, " (%d) %s\n", rc, nptr);
        }
      nptr += namelen + 1;
      }

    /* Test get strings by number */

    for (i = 0; i < MAXCPYGET && dat_datctl.get_numbers[i] >= 0; i++)
      {
      int rc;
      void *gotbuffer;
      uint32_t n = (uint32_t)(dat_datctl.get_numbers[i]);
      PCRE2_SUBSTRING_GET_BYNUMBER(rc, match_data, n, &gotbuffer);
      if (rc < 0)
        fprintf(outfile, "get substring %d failed %d\n", n, rc);
      else
        {
        fprintf(outfile, "%2dG ", n);
        PCHARSV(gotbuffer, 0, rc, utf, outfile);
        fprintf(outfile, " (%d)\n", rc);
        PCRE2_SUBSTRING_FREE(gotbuffer);
        }
      }

    /* Test get strings by name */

    nptr = dat_datctl.get_names;
    for (;;)
      {
      void *gotbuffer;
      int rc;
      int namelen = strlen((const char *)nptr);
      if (namelen == 0) break;

#ifdef SUPPORT_PCRE8
      if (test_mode == PCRE8_MODE) strcpy((char *)pbuffer8, (char *)nptr);
#endif
#ifdef SUPPORT_PCRE16
      if (test_mode == PCRE16_MODE)(void)to16(nptr, utf, namelen);
#endif
#ifdef SUPPORT_PCRE32
      if (test_mode == PCRE32_MODE)(void)to32(nptr, utf, namelen);
#endif

      PCRE2_SUBSTRING_GET_BYNAME(rc, match_data, pbuffer, &gotbuffer);
      if (rc < 0)
        {
        fprintf(outfile, "get substring '%s' failed %d\n", nptr, rc);
        }
      else
        {
        fprintf(outfile, "  G ");
        PCHARSV(gotbuffer, 0, rc, utf, outfile);
        fprintf(outfile, " (%d) %s\n", rc, nptr);
        PCRE2_SUBSTRING_FREE(gotbuffer);
        }
      nptr += namelen + 1;
      }

    /* Test getting the complete list of captured strings. */

    if ((dat_datctl.control & CTL_GETALL) != 0)
      {
      int rc;
      void **stringlist;
      size_t *lengths;
      PCRE2_SUBSTRING_LIST_GET(rc, match_data, &stringlist, &lengths);
      if (rc < 0)
        fprintf(outfile, "get substring list failed %d\n", rc);
      else
        {
        for (i = 0; i < capcount; i++)
          {
          fprintf(outfile, "%2dL ", i);
          PCHARSV(stringlist[i], 0, lengths[i], utf, outfile);
          putc('\n', outfile);
          }
        if (stringlist[i] != NULL)
          fprintf(outfile, "string list not terminated by NULL\n");
        PCRE2_SUBSTRING_LIST_FREE(stringlist);
        }
      }
    }    /* End of handling a successful match */

  /* There was a partial match. If the bumpalong point is not the same as
  the first inspected character, show the offset explicitly. */

  else if (capcount == PCRE2_ERROR_PARTIAL)
    {
    PCRE2_OFFSET leftchar = FLD(match_data, leftchar);

    fprintf(outfile, "Partial match");
    if (leftchar != FLD(match_data, startchar))
      fprintf(outfile, " at offset %d", (int)FLD(match_data, startchar));

    if ((dat_datctl.control & CTL_MARK) != 0 &&
         TESTFLD(match_data, mark, !=, NULL))
      {
      fprintf(outfile, ", mark=");
      PCHARSV(CASTFLD(void *, match_data, mark), 0, -1, utf, outfile);
      }

    fprintf(outfile, ": ");
    PCHARSV(pp, leftchar, ulen - leftchar, utf, outfile);

#ifdef FIXME
    if (verify_jit && jit_was_used) fprintf(outfile, " (JIT)");
#endif

    fprintf(outfile, "\n");
    break;  /* Out of the /g loop */
    }       /* End of handling partial match */

  /* Failed to match. If this is a /g or /G loop, we might previously have
  set g_notempty (to PCRE2_NOTEMPTY_ATSTART|PCRE2_ANCHORED) after a null match.
  If that is the case, this is not necessarily the end. We want to advance the
  start offset, and continue. We won't be at the end of the string - that was
  checked before setting g_notempty. We achieve the effect by pretending that a
  single character was matched. We know that match_data->oveccount is at least
  1 because that was checked above.

  Complication arises in the case when the newline convention is "any", "crlf",
  or "anycrlf". If the previous match was at the end of a line terminated by
  CRLF, an advance of one character just passes the CR, whereas we should
  prefer the longer newline sequence, as does the code in pcre2_match().

  Otherwise, in the case of UTF-8 or UTF-16 matching, the advance must be one
  character, not one byte. */

  else if (g_notempty != 0)   /* There was a previous null match */
    {
    uint16_t nl = FLD(compiled_code, newline_convention);
    PCRE2_OFFSET start_offset = dat_datctl.offset;    /* Where the match was */
    PCRE2_OFFSET end_offset = start_offset + 1;

    if ((nl == PCRE2_NEWLINE_CRLF || nl == PCRE2_NEWLINE_ANY ||
         nl == PCRE2_NEWLINE_ANYCRLF) &&
        start_offset < ulen - 1 &&
        CODE_UNIT(pp, start_offset) == '\r' &&
        CODE_UNIT(pp, end_offset) == '\n')
      end_offset++;

    else if (utf && test_mode != PCRE32_MODE)
      {
      if (test_mode == PCRE8_MODE)
        for (; end_offset < ulen; end_offset++)
          if ((((PCRE2_SPTR8)pp)[end_offset] & 0xc0) != 0x80) break;
      else
        for (; end_offset < ulen; end_offset++)
          if ((((PCRE2_SPTR16)pp)[end_offset] & 0xfc00) != 0xdc00) break;
      }

    SETFLDVEC(match_data, ovector, 0, start_offset);
    SETFLDVEC(match_data, ovector, 1, end_offset);
    }  /* End of handling null match in a global loop */

  /* A "normal" match failure. There will be a negative error number in
  capcount. */

  else
    {
    int mlen;

    switch(capcount)
      {
      case PCRE2_ERROR_NOMATCH:
      if (gmatched == 0)
        {
        fprintf(outfile, "No match");
        if ((dat_datctl.control & CTL_MARK) != 0 &&
             TESTFLD(match_data, mark, !=, NULL))
          {
          fprintf(outfile, ", mark = ");
          PCHARSV(CASTFLD(void *, match_data, mark), 0, -1, utf, outfile);
          }

#ifdef FIXME
        if (verify_jit && jit_was_used) fprintf(outfile, " (JIT)");
#endif

        fprintf(outfile, "\n");
        }
      break;

      case PCRE2_ERROR_BADUTFOFFSET:
      fprintf(outfile, "Error %d (bad UTF-%d offset)\n", capcount, test_mode);
      break;

      default:
      fprintf(outfile, "Failed: error %d: ", capcount);
      PCRE2_GET_ERROR_MESSAGE(mlen, capcount, pbuffer);
      PCHARSV(CASTVAR(void *, pbuffer), 0, mlen, FALSE, outfile);
      fprintf(outfile, "\n");
      break;
      }

    break;  /* Out of the /g loop */
    }       /* End of failed match handling */

  /* Control reaches here in two circumstances: (a) after a match, and (b)
  after a non-match that immediately followed a match on an empty string when
  doing a global search. Such a match is done with PCRE2_NOTEMPTY_ATSTART and
  PCRE2_ANCHORED set in g_notempty. The code above turns it into a fake match
  of one character. So effectively we get here only after a match. If we
  are not doing a global search, we are done. */

  if ((dat_datctl.control & CTL_ANYGLOB) == 0) break; else
    {
    PCRE2_OFFSET end_offset = FLD(match_data, ovector)[1];

    /* We must now set up for the next iteration of a global search. If we have
    matched an empty string, first check to see if we are at the end of the
    subject. If so, the loop is over. Otherwise, mimic what Perl's /g option
    does. Set PCRE2_NOTEMPTY_ATSTART and PCRE2_ANCHORED and try the match again
    at the same point. If this fails it will be picked up above, where a fake
    match is set up so that at this point we advance to the next character. */

    if (FLD(match_data, ovector)[0] == end_offset)
      {
      if (end_offset == ulen) break;      /* End of subject */
      g_notempty = PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED;
      }
    else g_notempty = 0;

    /* For /g, update the start offset, leaving the rest alone */

    if ((dat_datctl.control & CTL_GLOBAL) != 0) dat_datctl.offset = end_offset;

    /* For /G, update the pointer and length */

    else
      {
      pp += end_offset * code_unit_size;
      len -= end_offset;
      ulen -= end_offset *code_unit_size;
      }
    }
  }  /* End of global loop */

show_memory = FALSE;
return PR_OK;
}




/*************************************************
*                Print PCRE version              *
*************************************************/

/* The version string was read into 'version' at the start of execution. */

static void
print_version(FILE *f)
{
VERSION_TYPE *vp;
fprintf(f, "PCRE version ");
for (vp = version; *vp != 0; vp++) fprintf(f, "%c", *vp);
fprintf(f, "\n");
}



/*************************************************
*       Print newline configuration              *
*************************************************/

/* Output is always to stdout.

Arguments:
  rc         the return code from PCRE_CONFIG_NEWLINE
  isc        TRUE if called from "-C newline"
Returns:     nothing
*/

static void
print_newline_config(unsigned int rc, BOOL isc)
{
if (!isc) printf("  Newline sequence is ");
if (rc < sizeof(newlines)/sizeof(char *))
  printf("%s\n", newlines[rc]);
else
  printf("a non-standard value: %d\n", rc);
}



/*************************************************
*             Usage function                     *
*************************************************/

static void
usage(void)
{
printf("Usage:     pcre2test [options] [<input file> [<output file>]]\n\n");
printf("Input and output default to stdin and stdout.\n");
#if defined(SUPPORT_LIBREADLINE) || defined(SUPPORT_LIBEDIT)
printf("If input is a terminal, readline() is used to read from it.\n");
#else
printf("This version of pcre2test is not linked with readline().\n");
#endif
printf("\nOptions:\n");
#ifdef SUPPORT_PCRE8
printf("  -8            use the 8-bit library\n");
#endif
#ifdef SUPPORT_PCRE16
printf("  -16           use the 16-bit library\n");
#endif
#ifdef SUPPORT_PCRE32
printf("  -32           use the 32-bit library\n");
#endif
printf("  -b            set default pattern control 'fullbincode'\n");
printf("  -C            show PCRE2 compile-time options and exit\n");
printf("  -C arg        show a specific compile-time option and exit with its\n");
printf("                  value if numeric (else 0). The arg can be:\n");
printf("     bsr            \\R type [ANYCRLF, ANY]\n");
printf("     ebcdic         compiled for EBCDIC character code [0,1]\n");
printf("     ebcdic-nl      NL code if compiled for EBCDIC\n");
printf("     jit            just-in-time compiler supported [0, 1]\n");
printf("     linksize       internal link size [2, 3, 4]\n");
printf("     newline        newline type [CR, LF, CRLF, ANYCRLF, ANY]\n");
printf("     pcre8          8 bit library support enabled [0, 1]\n");
printf("     pcre16         16 bit library support enabled [0, 1]\n");
printf("     pcre32         32 bit library support enabled [0, 1]\n");
printf("     utf            Unicode Transformation Format supported [0, 1]\n");
printf("  -d            set default pattern control 'debug'\n");
printf("  -dfa          set default subject control 'dfa'\n");
printf("  -help         show usage information\n");
printf("  -i            set default pattern control 'info'\n");
printf("  -q            quiet: do not output PCRE version number at start\n");
printf("  -pattern <s>  set default pattern control fields\n");
printf("  -subject <s>  set default subject control fields\n");
printf("  -S <n>        set stack size to <n> megabytes\n");
printf("  -t [<n>]      time compilation and execution, repeating <n> times\n");
printf("  -tm [<n>]     time execution (matching) only, repeating <n> times\n");
printf("  -T            same as -t, but show total times at the end\n");
printf("  -TM           same as -tm, but show total time at the end\n");
printf("  -version      show PCRE2 version and exit\n");
}



/*************************************************
*             Handle -C option                   *
*************************************************/

/* This option outputs configuration options and sets an appropriate return
code when asked for a single option. The code is abstracted into a separate
function because of its size. Use whichever pcre2_config() function is
available.

Argument:   an option name or NULL
Returns:    the return code
*/

static int
c_option(const char *arg)
{
unsigned long int lrc;
int rc;
int yield = 0;

if (arg != NULL)
  {
  unsigned int i;

  for (i = 0; i < COPTLISTCOUNT; i++)
    if (strcmp(arg, coptlist[i].name) == 0) break;

  if (i >= COPTLISTCOUNT)
    {
    fprintf(stderr, "** Unknown -C option '%s'\n", arg);
    return -1;
    }

  switch (coptlist[i].type)
    {
    case CONF_BSR:
    (void)PCRE2_CONFIG(coptlist[i].value, &rc, sizeof(rc));
    printf("%s\n", rc? "ANYCRLF" : "ANY");
    break;

    case CONF_FIX:
    yield = coptlist[i].value;
    printf("%d\n", yield);
    break;

    case CONF_FIZ:
    rc = coptlist[i].value;
    printf("%d\n", rc);
    break;

    case CONF_INT:
    (void)PCRE2_CONFIG(coptlist[i].value, &yield, sizeof(yield));
    printf("%d\n", yield);
    break;

    case CONF_NL:
    (void)PCRE2_CONFIG(coptlist[i].value, &rc, sizeof(rc));
    print_newline_config(rc, TRUE);
    break;
    }

/* For VMS, return the value by setting a symbol, for certain values only. */

#ifdef __VMS
  if (copytlist[i].type == CONF_FIX || coptlist[i].type == CONF_INT)
    {
    char ucname[16];
    strcpy(ucname, coptlist[i].name);
    for (i = 0; ucname[i] != 0; i++) ucname[i] = toupper[ucname[i];
    vms_setsymbol(ucname, 0, rc);
    }
#endif

  return yield;
  }

/* No argument for -C: output all configuration information. */

print_version(stdout);
printf("Compiled with\n");

#ifdef EBCDIC
printf("  EBCDIC code support: LF is 0x%02x\n", CHAR_LF);
#endif

#ifdef SUPPORT_PCRE8
printf("  8-bit support\n");
#endif
#ifdef SUPPORT_PCRE16
printf("  16-bit support\n");
#endif
#ifdef SUPPORT_PCRE32
printf("  32-bit support\n");
#endif

(void)PCRE2_CONFIG(PCRE2_CONFIG_UTF, &rc, sizeof(rc));
printf ("  %sUTF support\n", rc ? "" : "No ");
(void)PCRE2_CONFIG(PCRE2_CONFIG_JIT, &rc, sizeof(rc));
if (rc != 0)
  {
/* FIXME: config needs sorting for jit target
  const char *arch;
  (void)PCRE2_CONFIG(PCRE2_CONFIG_JITTARGET, (void *)(&arch));
  printf("  Just-in-time compiler support: %s\n", arch);
*/
  printf("  Just-in-time compiler support: FIXME\n");
  }
else
  {
  printf("  No just-in-time compiler support\n");
  }
(void)PCRE2_CONFIG(PCRE2_CONFIG_NEWLINE, &rc, sizeof(rc));
print_newline_config(rc, FALSE);
(void)PCRE2_CONFIG(PCRE2_CONFIG_BSR, &rc, sizeof(rc));
printf("  \\R matches %s\n", rc? "CR, LF, or CRLF only" :
                                 "all Unicode newlines");
(void)PCRE2_CONFIG(PCRE2_CONFIG_LINKSIZE, &rc, sizeof(rc));
printf("  Internal link size = %d\n", rc);
(void)PCRE2_CONFIG(PCRE2_CONFIG_PARENSLIMIT, &lrc, sizeof(lrc));
printf("  Parentheses nest limit = %ld\n", lrc);
(void)PCRE2_CONFIG(PCRE2_CONFIG_MATCHLIMIT, &lrc, sizeof(lrc));
printf("  Default match limit = %ld\n", lrc);
(void)PCRE2_CONFIG(PCRE2_CONFIG_RECURSIONLIMIT, &lrc, sizeof(lrc));
printf("  Default recursion depth limit = %ld\n", lrc);
(void)PCRE2_CONFIG(PCRE2_CONFIG_STACKRECURSE, &rc, sizeof(rc));
printf("  Match recursion uses %s", rc? "stack" : "heap");

printf("\n");
return 0;
}



/*************************************************
*                Main Program                    *
*************************************************/

int
main(int argc, char **argv)
{
uint32_t yield = 0;
uint32_t op = 1;
uint32_t stack_size;
BOOL notdone = TRUE;
BOOL quiet = FALSE;
BOOL showtotaltimes = FALSE;
BOOL skipping = FALSE;
char *arg_subject = NULL;
char *arg_pattern = NULL;

PCRE2_JIT_STACK *jit_stack = NULL;

/* The offsets to the options and control bits fields of the pattern and data
control blocks must be the same so that common options and controls such as
"anchored" or "memory" can work for either of them from a single table entry.
We cannot test this till runtime because "offsetof" does not work in the
preprocessor. */

if (PO(options) != DO(options) || PO(control) != DO(control))
  {
  fprintf(stderr, "** Coding error: "
    "options and control offsets for pattern and data must be the same.\n");
  return 1;
  }

/* Get the PCRE version number information. */

PCRE2_CONFIG(PCRE2_CONFIG_VERSION, version, sizeof(VERSION_TYPE)*VERSION_SIZE);

/* Get buffers from malloc() so that valgrind will check their misuse when
debugging. They grow automatically when very long lines are read. The 16-
and 32-bit buffers (pbuffer16, pbuffer32) are obtained only if needed. */

buffer = (uint8_t *)malloc(pbuffer8_size);
pbuffer8 = (uint8_t *)malloc(pbuffer8_size);

/* The following  _setmode() stuff is some Windows magic that tells its runtime
library to translate CRLF into a single LF character. At least, that's what
I've been told: never having used Windows I take this all on trust. Originally
it set 0x8000, but then I was advised that _O_BINARY was better. */

#if defined(_WIN32) || defined(WIN32)
_setmode( _fileno( stdout ), _O_BINARY );
#endif

/* Initialization that does not depend on the running mode. */

memset(&def_patctl, 0, sizeof(patctl));
memset(&def_datctl, 0, sizeof(datctl));
def_datctl.oveccount = DEFAULT_OVECCOUNT;
def_datctl.copy_numbers[0] = -1;
def_datctl.get_numbers[0] = -1;
def_datctl.cfail[0] = def_datctl.cfail[1] = CFAIL_UNSET;

/* Scan command line options. */

while (argc > 1 && argv[op][0] == '-')
  {
  const char *endptr;
  char *arg = argv[op];

  /* Display and/or set return code for configuration options. */

  if (strcmp(arg, "-C") == 0)
    {
    yield = c_option(argv[op + 1]);
    goto EXIT;
    }

  /* Select operating mode */

  if (strcmp(arg, "-8") == 0)
    {
#ifdef SUPPORT_PCRE8
    test_mode = PCRE8_MODE;
#else
    fprintf(stderr,
      "** This version of PCRE was built without 8-bit support\n");
    exit(1);
#endif
    }
  else if (strcmp(arg, "-16") == 0)
    {
#ifdef SUPPORT_PCRE16
    test_mode = PCRE16_MODE;
#else
    fprintf(stderr,
      "** This version of PCRE was built without 16-bit support\n");
    exit(1);
#endif
    }
  else if (strcmp(arg, "-32") == 0)
    {
#ifdef SUPPORT_PCRE32
    test_mode = PCRE32_MODE;
#else
    fprintf(stderr,
      "** This version of PCRE was built without 32-bit support\n");
    exit(1);
#endif
    }

  /* Set quiet (no version verification) */

  else if (strcmp(arg, "-q") == 0) quiet = TRUE;

  /* Set system stack size */

  else if (strcmp(arg, "-S") == 0 && argc > 2 &&
      ((stack_size = get_value(argv[op+1], &endptr)), *endptr == 0))
    {
#if defined(_WIN32) || defined(WIN32) || defined(__minix) || defined(NATIVE_ZOS) || defined(__VMS)
    fprintf(stderr, "PCRE: -S is not supported on this OS\n");
    exit(1);
#else
    int rc;
    struct rlimit rlim;
    getrlimit(RLIMIT_STACK, &rlim);
    rlim.rlim_cur = stack_size * 1024 * 1024;
    rc = setrlimit(RLIMIT_STACK, &rlim);
    if (rc != 0)
      {
      fprintf(stderr, "PCRE: setrlimit() failed with error %d\n", rc);
      exit(1);
      }
    op++;
    argc--;
#endif
    }

  /* Set some common pattern and subject controls */

  else if (strcmp(arg, "-b") == 0) def_patctl.control |= CTL_FULLBINCODE;
  else if (strcmp(arg, "-d") == 0) def_patctl.control |= CTL_DEBUG;
  else if (strcmp(arg, "-i") == 0) def_patctl.control |= CTL_INFO;
  else if (strcmp(arg, "-dfa") == 0) def_datctl.control |= CTL_DFA; 

  /* Set timing parameters */

  else if (strcmp(arg, "-t") == 0 || strcmp(arg, "-tm") == 0 ||
           strcmp(arg, "-T") == 0 || strcmp(arg, "-TM") == 0)
    {
    int temp;
    int both = arg[2] == 0;
    showtotaltimes = arg[1] == 'T';
    if (argc > 2 && (temp = get_value(argv[op+1], &endptr), *endptr == 0))
      {
      timeitm = temp;
      op++;
      argc--;
      }
    else timeitm = LOOPREPEAT;
    if (both) timeit = timeitm;
    }

  /* Give help */

  else if (strcmp(arg, "-help") == 0 ||
           strcmp(arg, "--help") == 0)
    {
    usage();
    goto EXIT;
    }

  /* Show version */

  else if (strcmp(arg, "-version") == 0 ||
           strcmp(arg, "--version") == 0)
    {
    print_version(stdout);
    goto EXIT;
    }

  /* The following options save their data for processing once we know what
  the running mode is. */

  else if (strcmp(arg, "-subject") == 0)
    {
    arg_subject = argv[op+1];
    goto CHECK_VALUE_EXISTS;
    }

  else if (strcmp(arg, "-pattern") == 0)
    {
    arg_pattern = argv[op+1];
    CHECK_VALUE_EXISTS:
    if (argc <= 2)
      {
      fprintf(stderr, "** Missing value for %s\n", arg);
      yield = 1;
      goto EXIT;
      }
    op++;
    argc--;
    }

  /* Unrecognized option */

  else
    {
    fprintf(stderr, "** Unknown or malformed option '%s'\n", arg);
    usage();
    yield = 1;
    goto EXIT;
    }
  op++;
  argc--;
  }

/* Initialize things that cannot be done until we know which test mode we are
running in. */

code_unit_size = test_mode/8;
max_oveccount = DEFAULT_OVECCOUNT;

#ifdef SUPPORT_PCRE8
if (test_mode == PCRE8_MODE)
  {
  general_context8 = pcre2_general_context_create_8(&my_malloc, &my_free, NULL);
  default_pat_context8 = pcre2_compile_context_create_8(general_context8);
  pat_context8 = pcre2_compile_context_create_8(general_context8);
  default_dat_context8 = pcre2_match_context_create_8(general_context8);
  dat_context8 = pcre2_match_context_create_8(general_context8);
  match_data8 = pcre2_match_data_create_8(max_oveccount, general_context8);
#ifdef NO_RECURSE
  (void)pcre2_set_recursion_memory_management_8(default_dat_context8,
    &my_stack_malloc, &my_stack_free, NULL);  
#endif
  }
#endif

#ifdef SUPPORT_PCRE16
if (test_mode == PCRE16_MODE)
  {
  general_context16 = pcre2_general_context_create_16(&my_malloc, &my_free,
    NULL);
  default_pat_context16 = pcre2_compile_context_create_16(general_context16);
  pat_context16 = pcre2_compile_context_create_16(general_context16);
  default_dat_context16 = pcre2_match_context_create_16(general_context16);
  dat_context16 = pcre2_match_context_create_16(general_context16);
  match_data16 = pcre2_match_data_create_16(max_oveccount, general_context16);
#ifdef NO_RECURSE
  (void)pcre2_set_recursion_memory_management_16(default_dat_context16,
    &my_stack_malloc, &my_stack_free, NULL);  
#endif
  }
#endif

#ifdef SUPPORT_PCRE32
if (test_mode == PCRE32_MODE)
  {
  general_context32 = pcre2_general_context_create_32(&my_malloc, &my_free,
    NULL);
  default_pat_context32 = pcre2_compile_context_create_32(general_context32);
  pat_context32 = pcre2_compile_context_create_32(general_context32);
  default_dat_context32 = pcre2_match_context_create_32(general_context32);
  dat_context32 = pcre2_match_context_create_32(general_context32);
  match_data32 = pcre2_match_data_create_32(max_oveccount, general_context32);
#ifdef NO_RECURSE
  (void)pcre2_set_recursion_memory_management_32(default_dat_context32,
    &my_stack_malloc, &my_stack_free, NULL);  
#endif
  }
#endif

/* Handle command line modifier settings, sending any error messages to
stderr. We need to know the mode before modifying the context, and it is tidier
to do them all in the same way. */

outfile = stderr;
if ((arg_pattern != NULL &&
    !decode_modifiers((uint8_t *)arg_pattern, CTX_DEFPAT, &def_patctl, NULL)) ||
    (arg_subject != NULL &&
    !decode_modifiers((uint8_t *)arg_subject, CTX_DEFDAT, NULL, &def_datctl)))
  {
  yield = 1;
  goto EXIT;
  }

/* Sort out the input and output files, defaulting to stdin/stdout. */

infile = stdin;
outfile = stdout;

if (argc > 1)
  {
  infile = fopen(argv[op], INPUT_MODE);
  if (infile == NULL)
    {
    printf("** Failed to open %s\n", argv[op]);
    yield = 1;
    goto EXIT;
    }
  }

if (argc > 2)
  {
  outfile = fopen(argv[op+1], OUTPUT_MODE);
  if (outfile == NULL)
    {
    printf("** Failed to open %s\n", argv[op+1]);
    yield = 1;
    goto EXIT;
    }
  }

/* Output a heading line unless quiet, then process input lines. */

if (!quiet) print_version(outfile);

SET(compiled_code, NULL);
preg.re_pcre2_code = NULL;
preg.re_match_data = NULL;

while (notdone)
  {
  uint8_t *p;
  int rc = PR_OK;
  BOOL expectdata = TEST(compiled_code, !=, NULL) || preg.re_pcre2_code != NULL;

  if (extend_inputline(infile, buffer, expectdata? "data> " : "  re> ") == NULL)
    break;
  if (infile != stdin) fprintf(outfile, "%s", (char *)buffer);
  fflush(outfile);
  p = buffer;

  /* If we have a pattern set up for testing, or we are skipping after a
  compile failure, a blank line terminates this test; otherwise process the
  line as a data line. */

  if (expectdata || skipping)
    {
    while (isspace(*p)) p++;
    if (*p == 0)
      {
      if (preg.re_pcre2_code != NULL)
        {
        regfree(&preg);
        preg.re_pcre2_code = NULL;
        preg.re_match_data = NULL;
        }
      if (TEST(compiled_code, !=, NULL))
        {
        SUB1(pcre2_code_free, compiled_code);
        SET(compiled_code, NULL);
        }
      skipping = FALSE;
      setlocale(LC_CTYPE, "C");
      }
    else if (!skipping) rc = process_data();
    }

  /* We do not have a pattern set up for testing. Lines starting with # are
  either comments or special commands. Blank lines are ignored. Otherwise, the
  line must start with a valid delimiter. It is then processed as a pattern
  line. */

  else if (*p == '#')
    {
    if (isspace(p[1]) || p[1] == '!' || p[1] == 0) continue;
    rc = process_command();
    }

  else if (strchr("\"/!'`-+=:;.,", *p) != NULL)
    {
    rc = process_pattern();
    dfa_matched = 0;
    }

  else
    {
    while (isspace(*p)) p++;
    if (*p != 0)
      {
      fprintf(stderr, "** Invalid pattern delimiter '%c'.\n", *buffer);
      rc = PR_SKIP;
      }
    }

  if (rc == PR_SKIP && infile != stdin) skipping = TRUE;
  else if (rc == PR_ABEND)
    {
    fprintf(outfile, "** pcre2test run abandoned\n");
    yield = 1;
    goto EXIT;
    }
  }

/* Finish off a normal run. */

if (infile == stdin) fprintf(outfile, "\n");

if (showtotaltimes)
  {
  fprintf(outfile, "--------------------------------------\n");
  if (timeit > 0)
    {
    fprintf(outfile, "Total compile time %.4f milliseconds\n",
      (((double)total_compile_time * 1000.0) / (double)timeit) /
        (double)CLOCKS_PER_SEC);
    }
  fprintf(outfile, "Total match time %.4f milliseconds\n",
    (((double)total_match_time * 1000.0) / (double)timeitm) /
      (double)CLOCKS_PER_SEC);
  }


EXIT:

if (infile != NULL && infile != stdin) fclose(infile);
if (outfile != NULL && outfile != stdout) fclose(outfile);

free(buffer);
free(dbuffer);
free(pbuffer8);
free(dfa_workspace);
free((void *)locale_tables);
regfree(&preg);
PCRE2_MATCH_DATA_FREE(match_data);
SUB1(pcre2_code_free, compiled_code);

#ifdef SUPPORT_PCRE8
pcre2_general_context_free_8(general_context8);
pcre2_compile_context_free_8(pat_context8);
pcre2_compile_context_free_8(default_pat_context8);
pcre2_match_context_free_8(dat_context8);
pcre2_match_context_free_8(default_dat_context8);
#endif

#ifdef SUPPORT_PCRE16
free(pbuffer16);
pcre2_general_context_free_16(general_context16);
pcre2_compile_context_free_16(pat_context16);
pcre2_compile_context_free_16(default_pat_context16);
pcre2_match_context_free_16(dat_context16);
pcre2_match_context_free_16(default_dat_context16);
#endif

#ifdef SUPPORT_PCRE32
free(pbuffer32);
pcre2_general_context_free_32(general_context32);
pcre2_compile_context_free_32(pat_context32);
pcre2_compile_context_free_32(default_pat_context32);
pcre2_match_context_free_32(dat_context32);
pcre2_match_context_free_32(default_dat_context32);
#endif

#if defined(__VMS)
  yield = SS$_NORMAL;  /* Return values via DCL symbols */
#endif

/* FIXME: temp avoid compiler warnings. */

(void)jit_stack;

return yield;
}

/* End of pcre2test.c */