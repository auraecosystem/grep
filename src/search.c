/* search.c - searching subroutines using dfa, kwset and regex for grep.
   Copyright 1992, 1998, 2000, 2007, 2009 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston, MA
   02110-1301, USA.  */

/* Written August 1992 by Mike Haertel. */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE 1
#endif
#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>

#include <sys/types.h>

#include "mbsupport.h"
#ifdef MBS_SUPPORT
/* We can handle multibyte strings. */
# include <wchar.h>
# include <wctype.h>
#endif

#include "system.h"
#include "grep.h"
#ifndef FGREP_PROGRAM
# include <regex.h>
# include "dfa.h"
#endif
#include "kwset.h"
#include "error.h"
#include "xalloc.h"
#ifdef HAVE_LIBPCRE
# ifdef HAVE_DYNAMIC_LIBPCRE
#  include <dlfcn.h>
# endif
# include <pcre.h>
#endif
#ifdef HAVE_LANGINFO_CODESET
# include <langinfo.h>
#endif

#define NCHAR (UCHAR_MAX + 1)

/* For -w, we also consider _ to be word constituent.  */
#define WCHAR(C) (ISALNUM(C) || (C) == '_')

/* KWset compiled pattern.  For Ecompile and Gcompile, we compile
   a list of strings, at least one of which is known to occur in
   any string matching the regexp. */
static kwset_t kwset;

static void
kwsinit (void)
{
  static char trans[NCHAR];
  int i;

  if (match_icase)
    for (i = 0; i < NCHAR; ++i)
      trans[i] = TOLOWER (i);

  if (!(kwset = kwsalloc (match_icase ? trans : (char *) 0)))
    error (2, 0, _("memory exhausted"));
}

/* UTF-8 encoding allows some optimizations that we can't otherwise
   assume in a multibyte encoding. */
static int using_utf8;

void
check_utf8 (void)
{
#ifdef HAVE_LANGINFO_CODESET
  if (strcmp (nl_langinfo (CODESET), "UTF-8") == 0)
    using_utf8 = 1;
#endif
}

#ifndef FGREP_PROGRAM
/* DFA compiled regexp. */
static struct dfa dfa;

/* The Regex compiled patterns.  */
static struct patterns
{
  /* Regex compiled regexp. */
  struct re_pattern_buffer regexbuf;
  struct re_registers regs; /* This is here on account of a BRAIN-DEAD
			       Q@#%!# library interface in regex.c.  */
} patterns0;

struct patterns *patterns;
size_t pcount;

#ifdef HAVE_DYNAMIC_LIBPCRE

# define pcre_compile dl_pcre_compile
# define pcre_study dl_pcre_study
# define pcre_exec dl_pcre_exec
# define pcre_maketables dl_pcre_maketables

static pcre *(*pcre_compile)(const char *pattern, int options,
			        const char **errptr, int *erroffset,
			        const unsigned char *tableptr);
static pcre_extra *(*pcre_study)(const pcre *code, int options,
				    const char **errptr);
static int (*pcre_exec)(const pcre *code, const pcre_extra *extra,
			   const char *subject, int length, int startoffset,
			   int options, int *ovector, int ovecsize);
static const unsigned char *(*pcre_maketables)(void);

static int
map_pcre(void)
{
  void *library;

  if (pcre_maketables)
    return 1;

  if (!(library = dlopen("libpcre.so.3", RTLD_NOW)))
    return 0;

  if (!(pcre_compile = dlsym(library, "pcre_compile")))
    return 0;
  if (!(pcre_study = dlsym(library, "pcre_study")))
    return 0;
  if (!(pcre_exec = dlsym(library, "pcre_exec")))
    return 0;
  if (!(pcre_maketables = dlsym(library, "pcre_maketables")))
    return 0;

  return 1;
}

#else
#define map_pcre() (1)
#endif /* HAVE_DYNAMIC_LIBPCRE */

void
dfaerror (char const *mesg)
{
  error (2, 0, mesg);
}

/* Number of compiled fixed strings known to exactly match the regexp.
   If kwsexec returns < kwset_exact_matches, then we don't need to
   call the regexp matcher at all. */
static int kwset_exact_matches;

/* If the DFA turns out to have some set of fixed strings one of
   which must occur in the match, then we build a kwset matcher
   to find those strings, and thus quickly filter out impossible
   matches. */
static void
kwsmusts (void)
{
  struct dfamust const *dm;
  char const *err;

  if (dfa.musts)
    {
      kwsinit ();
      /* First, we compile in the substrings known to be exact
	 matches.  The kwset matcher will return the index
	 of the matching string that it chooses. */
      for (dm = dfa.musts; dm; dm = dm->next)
	{
	  if (!dm->exact)
	    continue;
	  ++kwset_exact_matches;
	  if ((err = kwsincr (kwset, dm->must, strlen (dm->must))) != 0)
	    error (2, 0, err);
	}
      /* Now, we compile the substrings that will require
	 the use of the regexp matcher.  */
      for (dm = dfa.musts; dm; dm = dm->next)
	{
	  if (dm->exact)
	    continue;
	  if ((err = kwsincr (kwset, dm->must, strlen (dm->must))) != 0)
	    error (2, 0, err);
	}
      if ((err = kwsprep (kwset)) != 0)
	error (2, 0, err);
    }
}
#endif /* !FGREP_PROGRAM */

#if defined(GREP_PROGRAM) || defined(EGREP_PROGRAM)
#ifdef EGREP_PROGRAM
COMPILE_FCT(Ecompile)
{
  reg_syntax_t syntax_bits = RE_SYNTAX_POSIX_EGREP;
#else
/* No __VA_ARGS__ in C89.  So we have to do it this way.  */
static COMPILE_RET
GEAcompile (char const *pattern, size_t size, reg_syntax_t syntax_bits)
{
#endif /* EGREP_PROGRAM */
  const char *err;
  const char *sep;
  size_t total = size;
  char const *motif = pattern;

  check_utf8 ();
#if 0
  if (match_icase)
    syntax_bits |= RE_ICASE;
#endif
  re_set_syntax (syntax_bits);
  dfasyntax (syntax_bits, match_icase, eolbyte);

  /* For GNU regex compiler we have to pass the patterns separately to detect
     errors like "[\nallo\n]\n".  The patterns here are "[", "allo" and "]"
     GNU regex should have raise a syntax error.  The same for backref, where
     the backref should have been local to each pattern.  */
  do
    {
      size_t len;
      sep = memchr (motif, '\n', total);
      if (sep)
	{
	  len = sep - motif;
	  sep++;
	  total -= (len + 1);
	}
      else
	{
	  len = total;
	  total = 0;
	}

      patterns = realloc (patterns, (pcount + 1) * sizeof (*patterns));
      if (patterns == NULL)
	error (2, errno, _("memory exhausted"));
      patterns[pcount] = patterns0;

      if ((err = re_compile_pattern (motif, len,
				    &(patterns[pcount].regexbuf))) != 0)
	error (2, 0, err);
      pcount++;

      motif = sep;
    } while (sep && total != 0);

  /* In the match_words and match_lines cases, we use a different pattern
     for the DFA matcher that will quickly throw out cases that won't work.
     Then if DFA succeeds we do some hairy stuff using the regex matcher
     to decide whether the match should really count. */
  if (match_words || match_lines)
    {
      static char const line_beg_no_bk[] = "^(";
      static char const line_end_no_bk[] = ")$";
      static char const word_beg_no_bk[] = "(^|[^[:alnum:]_])(";
      static char const word_end_no_bk[] = ")([^[:alnum:]_]|$)";
#ifdef EGREP_PROGRAM
# define IF_BK(x, y) (y)
      char *n = xmalloc (sizeof word_beg_no_bk - 1 + size + sizeof word_end_no_bk);
#else
      static char const line_beg_bk[] = "^\\(";
      static char const line_end_bk[] = "\\)$";
      static char const word_beg_bk[] = "\\(^\\|[^[:alnum:]_]\\)\\(";
      static char const word_end_bk[] = "\\)\\([^[:alnum:]_]\\|$\\)";
      int bk = !(syntax_bits & RE_NO_BK_PARENS);
# define IF_BK(x, y) ((bk) ? (x) : (y))
      char *n = xmalloc (sizeof word_beg_bk - 1 + size + sizeof word_end_bk);
#endif /* EGREP_PROGRAM */

      strcpy (n, match_lines ? IF_BK(line_beg_bk, line_beg_no_bk)
			     : IF_BK(word_beg_bk, word_beg_no_bk));
      total = strlen(n);
      memcpy (n + total, pattern, size);
      total += size;
      strcpy (n + total, match_lines ? IF_BK(line_end_bk, line_end_no_bk)
				     : IF_BK(word_end_bk, word_end_no_bk));
      total += strlen (n + total);
      pattern = motif = n;
      size = total;
    }
  else
    motif = NULL;

  dfacomp (pattern, size, &dfa, 1);
  kwsmusts ();

  if (motif)
    free((char *) motif);
}

#ifndef EGREP_PROGRAM
COMPILE_FCT(Gcompile)
{
  return GEAcompile (pattern, size,
		     RE_SYNTAX_GREP | RE_HAT_LISTS_NOT_NEWLINE);
}

COMPILE_FCT(Acompile)
{
  return GEAcompile (pattern, size, RE_SYNTAX_AWK);
}

COMPILE_FCT(Ecompile)
{
  return GEAcompile (pattern, size, RE_SYNTAX_POSIX_EGREP);
}
#endif /* !EGREP_PROGRAM */

EXECUTE_FCT(EGexecute)
{
  register char const *buflim, *beg, *end, *match, *best_match;
  char eol = eolbyte;
  int backref, start, len, best_len;
  struct kwsmatch kwsm;
  size_t i, ret_val;
#ifdef MBS_SUPPORT
  int mb_cur_max = MB_CUR_MAX;
  mbstate_t mbs;
  memset (&mbs, '\0', sizeof (mbstate_t));
#endif /* MBS_SUPPORT */

  buflim = buf + size;

  for (beg = end = buf; end < buflim; beg = end)
    {
      if (!start_ptr)
	{
	  /* We don't care about an exact match.  */
	  if (kwset)
	    {
	      /* Find a possible match using the KWset matcher. */
#ifdef MBS_SUPPORT
	      size_t bytes_left = 0;
#endif /* MBS_SUPPORT */
	      size_t offset;
#ifdef MBS_SUPPORT
	      /* kwsexec doesn't work with match_icase and multibyte input. */
	      if (match_icase && mb_cur_max > 1)
		/* Avoid kwset */
		offset = 0;
	      else
#endif /* MBS_SUPPORT */
	      offset = kwsexec (kwset, beg, buflim - beg, &kwsm);
	      if (offset == (size_t) -1)
		return (size_t)-1;
#ifdef MBS_SUPPORT
	      if (mb_cur_max > 1 && !using_utf8)
		{
		  bytes_left = offset;
		  while (bytes_left)
		    {
		      size_t mlen = mbrlen (beg, bytes_left, &mbs);
		      if (mlen == (size_t) -1 || mlen == 0)
			{
			  /* Incomplete character: treat as single-byte. */
			  memset (&mbs, '\0', sizeof (mbstate_t));
			  beg++;
			  bytes_left--;
			  continue;
			}

		      if (mlen == (size_t) -2)
			/* Offset points inside multibyte character:
			 * no good. */
			break;

		      beg += mlen;
		      bytes_left -= mlen;
		    }
		}
	      else
#endif /* MBS_SUPPORT */
	      beg += offset;
	      /* Narrow down to the line containing the candidate, and
		 run it through DFA. */
	      end = memchr(beg, eol, buflim - beg);
	      end++;
#ifdef MBS_SUPPORT
	      if (mb_cur_max > 1 && bytes_left)
		continue;
#endif
	      while (beg > buf && beg[-1] != eol)
		--beg;
	      if (
#ifdef MBS_SUPPORT
		  !(match_icase && mb_cur_max > 1) &&
#endif /* MBS_SUPPORT */
		  (kwsm.index < kwset_exact_matches))
		goto success;
	      if (dfaexec (&dfa, beg, end - beg, &backref) == (size_t) -1)
		continue;
	    }
	  else
	    {
	      /* No good fixed strings; start with DFA. */
#ifdef MBS_SUPPORT
	      size_t bytes_left = 0;
#endif /* MBS_SUPPORT */
	      size_t offset = dfaexec (&dfa, beg, buflim - beg, &backref);
	      if (offset == (size_t) -1)
		break;
	      /* Narrow down to the line we've found. */
#ifdef MBS_SUPPORT
	      if (mb_cur_max > 1 && !using_utf8)
		{
		  bytes_left = offset;
		  while (bytes_left)
		    {
		      size_t mlen = mbrlen (beg, bytes_left, &mbs);
		      if (mlen == (size_t) -1 || mlen == 0)
			{
			  /* Incomplete character: treat as single-byte. */
			  memset (&mbs, '\0', sizeof (mbstate_t));
			  beg++;
			  bytes_left--;
			  continue;
			}

		      if (mlen == (size_t) -2)
			/* Offset points inside multibyte character:
			 * no good. */
			break;

		      beg += mlen;
		      bytes_left -= mlen;
		    }
		}
	      else
#endif /* MBS_SUPPORT */
	      beg += offset;
	      end = memchr (beg, eol, buflim - beg);
	      end++;
#ifdef MBS_SUPPORT
	      if (mb_cur_max > 1 && bytes_left)
		continue;
#endif /* MBS_SUPPORT */
	      while (beg > buf && beg[-1] != eol)
		--beg;
	    }
	  /* Successful, no backreferences encountered! */
	  if (!backref)
	    goto success;
	}
      else
	{
	  /* We are looking for the leftmost (then longest) exact match.
	     We will go through the outer loop only once.  */
	  beg = start_ptr;
	  end = buflim;
	}

      /* If we've made it to this point, this means DFA has seen
	 a probable match, and we need to run it through Regex. */
      best_match = end;
      best_len = 0;
      for (i = 0; i < pcount; i++)
	{
	  patterns[i].regexbuf.not_eol = 0;
	  if (0 <= (start = re_search (&(patterns[i].regexbuf),
				       buf, end - buf - 1,
				       beg - buf, end - beg - 1,
				       &(patterns[i].regs))))
	    {
	      len = patterns[i].regs.end[0] - start;
	      match = buf + start;
	      if (match > best_match)
		continue;
	      if (start_ptr && !match_words)
		goto assess_pattern_match;
	      if ((!match_lines && !match_words)
		  || (match_lines && len == end - beg - 1))
		{
		  match = beg;
		  len = end - beg;
		  goto assess_pattern_match;
		}
	      /* If -w, check if the match aligns with word boundaries.
		 We do this iteratively because:
		 (a) the line may contain more than one occurence of the
		 pattern, and
		 (b) Several alternatives in the pattern might be valid at a
		 given point, and we may need to consider a shorter one to
		 find a word boundary.  */
	      if (match_words)
		while (match <= best_match)
		  {
		    if ((match == buf || !WCHAR ((unsigned char) match[-1]))
			&& (len == end - beg - 1
			    || !WCHAR ((unsigned char) match[len])))
		      goto assess_pattern_match;
		    if (len > 0)
		      {
			/* Try a shorter length anchored at the same place. */
			--len;
			patterns[i].regexbuf.not_eol = 1;
			len = re_match (&(patterns[i].regexbuf),
					buf, match + len - beg, match - buf,
					&(patterns[i].regs));
		      }
		    if (len <= 0)
		      {
			/* Try looking further on. */
			if (match == end - 1)
			  break;
			match++;
			patterns[i].regexbuf.not_eol = 0;
			start = re_search (&(patterns[i].regexbuf),
					   buf, end - buf - 1,
					   match - buf, end - match - 1,
					   &(patterns[i].regs));
			if (start < 0)
			  break;
			len = patterns[i].regs.end[0] - start;
			match = buf + start;
		      }
		  } /* while (match <= best_match) */
	      continue;
	    assess_pattern_match:
	      if (!start_ptr)
		{
		  /* Good enough for a non-exact match.
		     No need to look at further patterns, if any.  */
		  beg = match;
		  goto success_in_len;
		}
	      if (match < best_match || (match == best_match && len > best_len))
		{
		  /* Best exact match:  leftmost, then longest.  */
		  best_match = match;
		  best_len = len;
		}
	    } /* if re_search >= 0 */
	} /* for Regex patterns.  */
	if (best_match < end)
	  {
	    /* We have found an exact match.  We were just
	       waiting for the best one (leftmost then longest).  */
	    beg = best_match;
	    len = best_len;
	    goto success_in_len;
	  }
    } /* for (beg = end ..) */

 failure:
  ret_val = -1;
  goto out;

 success:
  len = end - beg;
 success_in_len:
  *match_size = len;
  ret_val = beg - buf;
 out:
  return ret_val;
}
#endif /* defined(GREP_PROGRAM) || defined(EGREP_PROGRAM) */

#ifdef MBS_SUPPORT
static int f_i_multibyte; /* whether we're using the new -Fi MB method */
static struct
{
  wchar_t **patterns;
  size_t count, maxlen;
  unsigned char *match;
} Fimb;
#endif

#if defined(GREP_PROGRAM) || defined(FGREP_PROGRAM)
COMPILE_FCT(Fcompile)
{
  int mb_cur_max = MB_CUR_MAX;
  char const *beg, *lim, *err;

  check_utf8 ();
#ifdef MBS_SUPPORT
  /* Support -F -i for UTF-8 input. */
  if (match_icase && mb_cur_max > 1)
    {
      mbstate_t mbs;
      wchar_t *wcpattern = xmalloc ((size + 1) * sizeof (wchar_t));
      const char *patternend = pattern;
      size_t wcsize;
      kwset_t fimb_kwset = NULL;
      char *starts = NULL;
      wchar_t *wcbeg, *wclim;
      size_t allocated = 0;

      memset (&mbs, '\0', sizeof (mbs));
# ifdef __GNU_LIBRARY__
      wcsize = mbsnrtowcs (wcpattern, &patternend, size, size, &mbs);
      if (patternend != pattern + size)
	wcsize = (size_t) -1;
# else
      {
	char *patterncopy = xmalloc (size + 1);

	memcpy (patterncopy, pattern, size);
	patterncopy[size] = '\0';
	patternend = patterncopy;
	wcsize = mbsrtowcs (wcpattern, &patternend, size, &mbs);
	if (patternend != patterncopy + size)
	  wcsize = (size_t) -1;
	free (patterncopy);
      }
# endif
      if (wcsize + 2 <= 2)
	{
fimb_fail:
	  free (wcpattern);
	  free (starts);
	  if (fimb_kwset)
	    kwsfree (fimb_kwset);
	  free (Fimb.patterns);
	  Fimb.patterns = NULL;
	}
      else
	{
	  if (!(fimb_kwset = kwsalloc (NULL)))
	    error (2, 0, _("memory exhausted"));

	  starts = xmalloc (mb_cur_max * 3);
	  wcbeg = wcpattern;
	  do
	    {
	      int i;
	      size_t wclen;

	      if (Fimb.count >= allocated)
		{
		  if (allocated == 0)
		    allocated = 128;
		  else
		    allocated *= 2;
		  Fimb.patterns = xrealloc (Fimb.patterns,
					    sizeof (wchar_t *) * allocated);
		}
	      Fimb.patterns[Fimb.count++] = wcbeg;
	      for (wclim = wcbeg;
		   wclim < wcpattern + wcsize && *wclim != L'\n'; ++wclim)
		*wclim = towlower (*wclim);
	      *wclim = L'\0';
	      wclen = wclim - wcbeg;
	      if (wclen > Fimb.maxlen)
		Fimb.maxlen = wclen;
	      if (wclen > 3)
		wclen = 3;
	      if (wclen == 0)
		{
		  if ((err = kwsincr (fimb_kwset, "", 0)) != 0)
		    error (2, 0, err);
		}
	      else
		for (i = 0; i < (1 << wclen); i++)
		  {
		    char *p = starts;
		    int j, k;

		    for (j = 0; j < wclen; ++j)
		      {
			wchar_t wc = wcbeg[j];
			if (i & (1 << j))
			  {
			    wc = towupper (wc);
			    if (wc == wcbeg[j])
			      continue;
			  }
			k = wctomb (p, wc);
			if (k <= 0)
			  goto fimb_fail;
			p += k;
		      }
		    if ((err = kwsincr (fimb_kwset, starts, p - starts)) != 0)
		      error (2, 0, err);
		  }
	      if (wclim < wcpattern + wcsize)
		++wclim;
	      wcbeg = wclim;
	    }
	  while (wcbeg < wcpattern + wcsize);
	  f_i_multibyte = 1;
	  kwset = fimb_kwset;
	  free (starts);
	  Fimb.match = xmalloc (Fimb.count);
	  if ((err = kwsprep (kwset)) != 0)
	    error (2, 0, err);
	  return;
	}
    }
#endif /* MBS_SUPPORT */


  kwsinit ();
  beg = pattern;
  do
    {
      for (lim = beg; lim < pattern + size && *lim != '\n'; ++lim)
	;
      if ((err = kwsincr (kwset, beg, lim - beg)) != 0)
	error (2, 0, err);
      if (lim < pattern + size)
	++lim;
      beg = lim;
    }
  while (beg < pattern + size);

  if ((err = kwsprep (kwset)) != 0)
    error (2, 0, err);
}

#ifdef MBS_SUPPORT
static int
Fimbexec (const char *buf, size_t size, size_t *plen, int exact)
{
  size_t len, letter, i;
  int ret = -1;
  mbstate_t mbs;
  wchar_t wc;
  int patterns_left;

  assert (match_icase && f_i_multibyte == 1);
  assert (MB_CUR_MAX > 1);

  memset (&mbs, '\0', sizeof (mbs));
  memset (Fimb.match, '\1', Fimb.count);
  letter = len = 0;
  patterns_left = 1;
  while (patterns_left && len <= size)
    {
      size_t c;

      patterns_left = 0;
      if (len < size)
	{
	  c = mbrtowc (&wc, buf + len, size - len, &mbs);
	  if (c + 2 <= 2)
	    return ret;

	  wc = towlower (wc);
	}
      else
	{
	  c = 1;
	  wc = L'\0';
	}

      for (i = 0; i < Fimb.count; i++)
	{
	  if (Fimb.match[i])
	    {
	      if (Fimb.patterns[i][letter] == L'\0')
		{
		  /* Found a match. */
		  *plen = len;
		  if (!exact && !match_words)
		    return 0;
		  else
		    {
		      /* For -w or exact look for longest match.  */
		      ret = 0;
		      Fimb.match[i] = '\0';
		      continue;
		    }
		}

	      if (Fimb.patterns[i][letter] == wc)
		patterns_left = 1;
	      else
		Fimb.match[i] = '\0';
	    }
	}

      len += c;
      letter++;
    }

  return ret;
}
#endif /* MBS_SUPPORT */

EXECUTE_FCT(Fexecute)
{
  register char const *beg, *try, *end;
  register size_t len;
  char eol = eolbyte;
  struct kwsmatch kwsmatch;
  size_t ret_val;
#ifdef MBS_SUPPORT
  int mb_cur_max = MB_CUR_MAX;
  mbstate_t mbs;
  memset (&mbs, '\0', sizeof (mbstate_t));
  const char *last_char = NULL;
#endif /* MBS_SUPPORT */

  for (beg = start_ptr ? start_ptr : buf; beg <= buf + size; beg++)
    {
      size_t offset = kwsexec (kwset, beg, buf + size - beg, &kwsmatch);
      if (offset == (size_t) -1)
	return offset;
#ifdef MBS_SUPPORT
      if (mb_cur_max > 1 && !using_utf8)
	{
	  size_t bytes_left = offset;
	  while (bytes_left)
	    {
	      size_t mlen = mbrlen (beg, bytes_left, &mbs);

	      last_char = beg;
	      if (mlen == (size_t) -1 || mlen == 0)
		{
		  /* Incomplete character: treat as single-byte. */
		  memset (&mbs, '\0', sizeof (mbstate_t));
		  beg++;
		  bytes_left--;
		  continue;
		}

	      if (mlen == (size_t) -2)
		/* Offset points inside multibyte character: no good. */
		break;

	      beg += mlen;
	      bytes_left -= mlen;
	    }

	  if (bytes_left)
	    continue;
	}
      else
#endif /* MBS_SUPPORT */
      beg += offset;
#ifdef MBS_SUPPORT
      /* For f_i_multibyte, the string at beg now matches first 3 chars of
	 one of the search strings (less if there are shorter search strings).
	 See if this is a real match.  */
      if (f_i_multibyte
	  && Fimbexec (beg, buf + size - beg, &kwsmatch.size[0], start_ptr == NULL))
	goto next_char;
#endif /* MBS_SUPPORT */
      len = kwsmatch.size[0];
      if (start_ptr && !match_words)
	goto success_in_beg_and_len;
      if (match_lines)
	{
	  if (beg > buf && beg[-1] != eol)
	    goto next_char;
	  if (beg + len < buf + size && beg[len] != eol)
	    goto next_char;
	  goto success;
	}
      else if (match_words)
	{
	  while (len)
	    {
	      int word_match = 0;
	      if (beg > buf)
		{
#ifdef MBS_SUPPORT
		  if (mb_cur_max > 1)
		    {
		      const char *s;
		      int mr;
		      wchar_t pwc;

		      if (using_utf8)
			{
			  s = beg - 1;
			  while (s > buf
				 && (unsigned char) *s >= 0x80
				 && (unsigned char) *s <= 0xbf)
			    --s;
			}
		      else
			s = last_char;
		      mr = mbtowc (&pwc, s, beg - s);
		      if (mr <= 0)
			memset (&mbs, '\0', sizeof (mbstate_t));
		      else if ((iswalnum (pwc) || pwc == L'_')
			       && mr == (int) (beg - s))
			goto next_char;
		    }
		  else
#endif /* MBS_SUPPORT */
		  if (WCHAR ((unsigned char) beg[-1]))
		    goto next_char;
		}
#ifdef MBS_SUPPORT
	      if (mb_cur_max > 1)
		{
		  wchar_t nwc;
		  int mr;

		  mr = mbtowc (&nwc, beg + len, buf + size - beg - len);
		  if (mr <= 0)
		    {
		      memset (&mbs, '\0', sizeof (mbstate_t));
		      word_match = 1;
		    }
		  else if (!iswalnum (nwc) && nwc != L'_')
		    word_match = 1;
		}
	      else
#endif /* MBS_SUPPORT */
		if (beg + len >= buf + size || !WCHAR ((unsigned char) beg[len]))
		  word_match = 1;
	      if (word_match)
		{
		  if (start_ptr == NULL)
		    /* Returns the whole line now we know there's a word match. */
		    goto success;
		  else {
		    /* Returns just this word match. */
		    *match_size = len;
		    return beg - buf;
		  }
		}
	      if (len > 0)
		{
		  /* Try a shorter length anchored at the same place. */
		  --len;
		  offset = kwsexec (kwset, beg, len, &kwsmatch);

		  if (offset == -1)
		    goto next_char; /* Try a different anchor. */
#ifdef MBS_SUPPORT

		  if (mb_cur_max > 1 && !using_utf8)
		    {
		      size_t bytes_left = offset;
		      while (bytes_left)
			{
			  size_t mlen = mbrlen (beg, bytes_left, &mbs);

			  last_char = beg;
			  if (mlen == (size_t) -1 || mlen == 0)
			    {
			      /* Incomplete character: treat as single-byte. */
			      memset (&mbs, '\0', sizeof (mbstate_t));
			      beg++;
			      bytes_left--;
			      continue;
			    }

			  if (mlen == (size_t) -2)
			    {
			      /* Offset points inside multibyte character:
			       * no good. */
			      break;
			    }

			  beg += mlen;
			  bytes_left -= mlen;
			}

		      if (bytes_left)
			{
			  memset (&mbs, '\0', sizeof (mbstate_t));
			  goto next_char; /* Try a different anchor. */
			}
		    }
		  else
#endif /* MBS_SUPPORT */
		  beg += offset;
#ifdef MBS_SUPPORT
		  /* The string at beg now matches first 3 chars of one of
		     the search strings (less if there are shorter search
		     strings).  See if this is a real match.  */
		  if (f_i_multibyte
		      && Fimbexec (beg, len - offset, &kwsmatch.size[0],
				   start_ptr == NULL))
		    goto next_char;
#endif /* MBS_SUPPORT */
		  len = kwsmatch.size[0];
		}
	    }
	}
       else
	goto success;
next_char:;
#ifdef MBS_SUPPORT
      /* Advance to next character.  For MB_CUR_MAX == 1 case this is handled
	 by ++beg above.  */
      if (mb_cur_max > 1)
	{
	  if (using_utf8)
	    {
	      unsigned char c = *beg;
	      if (c >= 0xc2)
		{
		  if (c < 0xe0)
		    ++beg;
		  else if (c < 0xf0)
		    beg += 2;
		  else if (c < 0xf8)
		    beg += 3;
		  else if (c < 0xfc)
		    beg += 4;
		  else if (c < 0xfe)
		    beg += 5;
		}
	    }
	  else
	    {
	      size_t l = mbrlen (beg, buf + size - beg, &mbs);

	      last_char = beg;
	      if (l + 2 >= 2)
		beg += l - 1;
	      else
		memset (&mbs, '\0', sizeof (mbstate_t));
	    }
	}
#endif /* MBS_SUPPORT */
    }

  return -1;

 success:
#ifdef MBS_SUPPORT
  if (mb_cur_max > 1 && !using_utf8)
    {
      end = beg + len;
      while (end < buf + size)
	{
	  size_t mlen = mbrlen (end, buf + size - end, &mbs);
	  if (mlen == (size_t) -1 || mlen == (size_t) -2 || mlen == 0)
	    {
	      memset (&mbs, '\0', sizeof (mbstate_t));
	      mlen = 1;
	    }
	  if (mlen == 1 && *end == eol)
	    break;

	  end += mlen;
	}
     }
  else
 #endif /* MBS_SUPPORT */
  end = memchr (beg + len, eol, (buf + size) - (beg + len));
  end++;
  while (buf < beg && beg[-1] != eol)
    --beg;
  len = end - beg;
 success_in_beg_and_len:
  *match_size = len;
  ret_val = beg - buf;
 out:
  return ret_val;
}
#endif /* defined(GREP_PROGRAM) || defined(FGREP_PROGRAM) */

#ifdef GREP_PROGRAM
#if HAVE_LIBPCRE
/* Compiled internal form of a Perl regular expression.  */
static pcre *cre;

/* Additional information about the pattern.  */
static pcre_extra *extra;
#endif

COMPILE_FCT(Pcompile)
{
#if !HAVE_LIBPCRE
  error (2, 0, "%s", _("Support for the -P option is not compiled into this --disable-perl-regexp binary"));
#else
  int e;
  char const *ep;
  char *re = xmalloc (4 * size + 7);
  int flags = PCRE_MULTILINE | (match_icase ? PCRE_CASELESS : 0);
  char const *patlim = pattern + size;
  char *n = re;
  char const *p;
  char const *pnul;

  if (!map_pcre ())
    error (2, 0, _("The -P option is not supported: libpcre.so.3 is not available"));

  /* FIXME: Remove these restrictions.  */
  if (eolbyte != '\n')
    error (2, 0, _("The -P and -z options cannot be combined"));
  if (memchr(pattern, '\n', size))
    error (2, 0, _("The -P option only supports a single pattern"));

  *n = '\0';
  if (match_lines)
    strcpy (n, "^(");
  if (match_words)
    strcpy (n, "\\b(");
  n += strlen (n);

  /* The PCRE interface doesn't allow NUL bytes in the pattern, so
     replace each NUL byte in the pattern with the four characters
     "\000", removing a preceding backslash if there are an odd
     number of backslashes before the NUL.

     FIXME: This method does not work with some multibyte character
     encodings, notably Shift-JIS, where a multibyte character can end
     in a backslash byte.  */
  for (p = pattern; (pnul = memchr (p, '\0', patlim - p)); p = pnul + 1)
    {
      memcpy (n, p, pnul - p);
      n += pnul - p;
      for (p = pnul; pattern < p && p[-1] == '\\'; p--)
	continue;
      n -= (pnul - p) & 1;
      strcpy (n, "\\000");
      n += 4;
    }

  memcpy (n, p, patlim - p);
  n += patlim - p;
  *n = '\0';
  if (match_words)
    strcpy (n, ")\\b");
  if (match_lines)
    strcpy (n, ")$");

  cre = pcre_compile (re, flags, &ep, &e, pcre_maketables ());
  if (!cre)
    error (2, 0, ep);

  extra = pcre_study (cre, 0, &ep);
  if (ep)
    error (2, 0, ep);

  free (re);
#endif
}

EXECUTE_FCT(Pexecute)
{
#if !HAVE_LIBPCRE
  abort ();
  return -1;
#else
  /* This array must have at least two elements; everything after that
     is just for performance improvement in pcre_exec.  */
  int sub[300];

  int e = pcre_exec (cre, extra, buf, size,
		     start_ptr ? (start_ptr - buf) : 0, 0,
		     sub, sizeof sub / sizeof *sub);

  if (e <= 0)
    {
      switch (e)
	{
	case PCRE_ERROR_NOMATCH:
	  return -1;

	case PCRE_ERROR_NOMEMORY:
	  error (2, 0, _("Memory exhausted"));

	default:
	  abort ();
	}
    }
  else
    {
      /* Narrow down to the line we've found.  */
      char const *beg = buf + sub[0];
      char const *end = buf + sub[1];
      char const *buflim = buf + size;
      char eol = eolbyte;
      if (!start_ptr)
	{
	  /* FIXME: The case when '\n' is not found indicates a bug:
	     Since grep is line oriented, the match should never contain
	     a newline, so there _must_ be a newline following.
	   */
	  if (!(end = memchr (end, eol, buflim - end)))
	    end = buflim;
	  else
	    end++;
	  while (buf < beg && beg[-1] != eol)
	    --beg;
	}

      *match_size = end - beg;
      return beg - buf;
    }
#endif
}

struct matcher const matchers[] = {
  { "default", Gcompile, EGexecute },
  { "grep",    Gcompile, EGexecute },
  { "egrep",   Ecompile, EGexecute },
  { "awk",     Acompile, EGexecute },
  { "fgrep",   Fcompile, Fexecute },
  { "perl",    Pcompile, Pexecute },
  { "", 0, 0 },
};
#endif /* GREP_PROGRAM */
