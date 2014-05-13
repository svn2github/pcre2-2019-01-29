/*************************************************
*      Perl-Compatible Regular Expressions       *
*************************************************/

/* PCRE is a library of functions to support regular expressions whose syntax
and semantics are as close as possible to those of the Perl 5 language.

                       Written by Philip Hazel
     Original API code Copyright (c) 1997-2012 University of Cambridge
         New API code Copyright (c) 2014 University of Cambridge

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


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pcre2_internal.h"


/* FIXME: this is currently a placeholder function */

/*************************************************
*               Free compiled code               *
*************************************************/

PCRE2_EXP_DEFN void PCRE2_CALL_CONVENTION
pcre2_code_free(pcre2_code *code)
{
if (code != NULL) code->memctl.free(code, code->memctl.memory_data);
}



/*************************************************
*        Compile a Regular Expression            *
*************************************************/

/* This function reads a regular expression in the form of a string and returns
a pointer to a block of store holding a compiled version of the expression.

Arguments:
  pattern       the regular expression
  patlen        the length of the pattern, or < 0 for zero-terminated
  options       option bits
  errorcode     pointer to error code variable (positive error code)
  erroroffset   pointer for offset in pattern where error was detected
  ccontext      points to a compile context or is NULL

Returns:        pointer to compiled data block, or NULL on error,
                with errorcode and erroroffset set
*/

/* FIXME: this is currently a placeholder function */

PCRE2_EXP_DEFN pcre2_code * PCRE2_CALL_CONVENTION
pcre2_compile(PCRE2_SPTR pattern, int patlen, uint32_t options, int *errorcode,
   size_t *erroroffset, pcre2_compile_context *ccontext)
{
pcre2_compile_context default_context;
pcre2_code *c = NULL;

patlen = patlen; 

if (ccontext == NULL)
  {
  PRIV(compile_context_init)(&default_context, TRUE);
  ccontext = &default_context;
  }   

/* Fudge while testing pcre2test. */

*erroroffset = 0;

if (pattern[0] == 'Y')
  {
  PCRE2_UCHAR *n; 
  int lennumber = (PCRE2_CODE_UNIT_WIDTH == 8)? 2 : 1;
  size_t size = sizeof(pcre2_real_code) + 
    (12 + 3*lennumber)*(PCRE2_CODE_UNIT_WIDTH/8) + CU2BYTES(20);
  c = ccontext->memctl.malloc(size, NULL);
  c->memctl = ccontext->memctl; 
  c->magic_number = MAGIC_NUMBER;
  c->size = size;
  c->compile_options = options; 
  c->flags = PCRE2_CODE_UNIT_WIDTH/8;
  c->limit_match = 0;
  c->limit_recursion = 0;
  c->max_lookbehind = 0;
  c->minlength = 3;
  c->top_bracket = 5;
  c->top_backref = 1;       
  c->bsr_convention = ccontext->bsr_convention;
  c->newline_convention = ccontext->newline_convention;  
  c->name_count = 3; 
  c->name_entry_size = 4 + lennumber; 
  
  n = (PCRE2_UCHAR *)((char *)c + sizeof(pcre2_real_code));
   
  if (lennumber == 2) *n++ = 0 ;
  *n++ = 1;
  *n++ = 'x'; *n++ = 'x'; *n++ = 'x'; *n++ = 0;

  if (lennumber == 2) *n++ = 0 ;
  *n++ = 2;
  *n++ = 'y'; *n++ = 'y'; *n++ = 'y'; *n++ = 0;

  if (lennumber == 2) *n++ = 0 ;
  *n++ = 3;
  *n++ = 'y'; *n++ = 'y'; *n++ = 'y'; *n++ = 0;


  *n++ = OP_CHAR;
  *n++ = 'x';
  *n++ = OP_CHARI;
  *n++ = 'Y';   
  
  *n++ = OP_PROP;
  *n++ = PT_SC;
  *n++ = 0;  
  
  *n++ = OP_DNRREF;
  *n++ = 0;  
 
  *n++ = OP_END;

 
  } 

else
  { 
  *errorcode = 1;
  }
    
return c;
}

/* End of pcre2_compile.c */
