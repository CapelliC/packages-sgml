/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        jan@swi.psy.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2002, University of Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#define DTD_IMPLEMENTATION 1
#include <stdio.h>
#include "dtd.h"
#include "model.h"
#include "util.h"
#include "catalog.h"
#include "parser.h"
#include <stdlib.h>
#include <assert.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include "utf8.h"
#include <errno.h>

#define DEBUG(g) ((void)0)

		 /*******************************
		 *	    LOCAL TYPES		*
		 *******************************/

typedef struct locbuf
{ dtd_srcloc start;			/* p->startloc */
  dtd_srcloc here;			/* p->location */
} locbuf;


		 /*******************************
		 *	      PROTOYPES		*
		 *******************************/

static const ichar *	itake_name(dtd *dtd, const ichar *in, dtd_symbol **id);
static const ichar *	itake_entity_name(dtd *dtd, const ichar *in,
					  dtd_symbol **id);
static const ichar *	itake_namegroup(dtd *dtd,
					charfunc sep, const ichar *decl,
					dtd_symbol **names, int *n);
static const ichar *	iskip_layout(dtd *dtd, const ichar *in);
static dtd_parser *	clone_dtd_parser(dtd_parser *p);
static void		free_model(dtd_model *m);
static int		process_entity_declaration(dtd_parser *p,
						    const ichar *decl);
static void		free_notations(dtd_notation *n);
static void		free_shortrefs(dtd_shortref *sr);
static int		process_cdata(dtd_parser *p, int last);
static int		process_entity(dtd_parser *p, const ichar *name);
static int		emit_cdata(dtd_parser *p, int last);
static dtd_space_mode	istr_to_space_mode(const ichar *val);
static void		update_space_mode(dtd_parser *p, dtd_element *e,
					  int natts, sgml_attribute *atts);
static dtd_model *	make_model(dtd *dtd, const ichar *decl,
				   const ichar **end);
static void		for_elements_in_model(dtd_model *m,
					      void (*f)(dtd_element *e,
							void *closure),
					      void *closure);
void			putchar_dtd_parser(dtd_parser *p, int chr);
int			load_dtd_from_file(dtd_parser *p, const char *file);
void			free_dtd_parser(dtd_parser *p);
static const ichar *	isee_character_entity(dtd *dtd, const ichar *in,
					      int *chr);
static int		add_default_attributes(dtd_parser *p, dtd_element *e,
					       int natts,
					       sgml_attribute *atts);
static int		prepare_cdata(dtd_parser *p);


		 /*******************************
		 *	      MACROS		*
		 *******************************/

#define WITH_CLASS(p, c, g) \
	{ sgml_event_class _oc = p->event_class; \
	  p->event_class = c; \
	  g; \
	  p->event_class = _oc; \
	}

#define WITH_PARSER(p, g) \
	{ dtd_parser *_old = p; \
	  current_parser = p; \
	  g; \
	  current_parser = _old; \
	}

		 /*******************************
		 *	   SRC LOCATION		*
		 *******************************/


static void				/* TBD: also handle startloc */
push_location(dtd_parser *p, locbuf *save)
{ save->here  = p->location;
  save->start = p->startloc;

  p->location.parent = &save->here;
  p->startloc.parent = &save->start;
}


static void
pop_location(dtd_parser *p, locbuf *saved)
{ p->location = saved->here;
  p->startloc = saved->start;
}


static __inline void
_sgml_cplocation(dtd_srcloc *d, dtd_srcloc *loc)
{ d->type    = loc->type;
  d->name    = loc->name;
  d->line    = loc->line;
  d->linepos = loc->linepos;
  d->charpos = loc->charpos;
					/* but not the parent! */
}

void
sgml_cplocation(dtd_srcloc *d, dtd_srcloc *loc)
{ _sgml_cplocation(d, loc);
}

#define sgml_cplocation(d,s) _sgml_cplocation(d, s)

static void
inc_location(dtd_srcloc *l, int chr)
{ if ( chr == '\n' )
  { l->linepos = 0;
    l->line++;
  }

  l->linepos++;
  l->charpos++;
}


static void
dec_location(dtd_srcloc *l, int chr)
{ if ( chr == '\n' )
  { l->linepos = 2;			/* not good! */
    l->line--;
  }
  l->linepos--;
  l->charpos--;
}

		 /*******************************
		 *   CLASSIFICATION PRIMITIVES	*
		 *******************************/

#define HasClass(dtd, chr, mask) \
	(dtd->charclass->class[(chr)] & (mask))

static const ichar *
isee_func(dtd *dtd, const ichar *in, charfunc func)
{ if ( dtd->charfunc->func[func] == *in )
    return ++in;

  return NULL;
}

		 /*******************************
		 *	      SYMBOLS		*
		 *******************************/

static dtd_symbol_table *
new_symbol_table()
{ dtd_symbol_table *t = sgml_calloc(1, sizeof(*t));
  t->size    = SYMBOLHASHSIZE;
  t->entries = sgml_calloc(t->size, sizeof(dtd_symbol*));

  return t;
}


static void
free_symbol_table(dtd_symbol_table *t)
{ int i;

  for(i=0; i<t->size; i++)
  { dtd_symbol *s, *next;

    for(s=t->entries[i]; s; s=next)
    { next = s->next;

      sgml_free((char *)s->name);
      sgml_free(s);
    }
  }

  sgml_free(t->entries);
  sgml_free(t);
}


dtd_symbol *
dtd_find_symbol(dtd *dtd, const ichar *name)
{ dtd_symbol_table *t = dtd->symbols;

  if ( dtd->case_sensitive )
  { int k = istrhash(name, t->size);
    dtd_symbol *s;

    for(s=t->entries[k]; s; s = s->next)
    { if ( istreq(s->name, name) )
	return s;
    }
  } else
  { int k = istrcasehash(name, t->size);
    dtd_symbol *s;

    for(s=t->entries[k]; s; s = s->next)
    { if ( istrcaseeq(s->name, name) )
	return s;
    }
  }

  return NULL;
}


static dtd_symbol *
dtd_find_entity_symbol(dtd *dtd, const ichar *name)
{ dtd_symbol_table *t = dtd->symbols;

  if ( dtd->ent_case_sensitive )
  { int k = istrhash(name, t->size);
    dtd_symbol *s;

    for(s=t->entries[k]; s; s = s->next)
    { if ( istreq(s->name, name) )
	return s;
    }
  } else
  { int k = istrcasehash(name, t->size);
    dtd_symbol *s;

    for(s=t->entries[k]; s; s = s->next)
    { if ( istrcaseeq(s->name, name) )
	return s;
    }
  }

  return NULL;
}


dtd_symbol *
dtd_add_symbol(dtd *dtd, const ichar *name)
{ dtd_symbol_table *t = dtd->symbols;
  int k = istrhash(name, t->size);
  dtd_symbol *s;

  for(s=t->entries[k]; s; s = s->next)
  { if ( istreq(s->name, name) )
      return s;
  }

  s = sgml_calloc(1, sizeof(*s));
  s->name = istrdup(name);
  s->next = t->entries[k];
  t->entries[k] = s;

  return s;
}


		 /*******************************
		 *	    ENTITIES		*
		 *******************************/

static void
free_entity_list(dtd_entity *e)
{ dtd_entity *next;

  for( ; e; e=next)
  { next = e->next;

    if ( e->value )   sgml_free(e->value);
    if ( e->extid )   sgml_free(e->extid);
    if ( e->exturl )  sgml_free(e->exturl);
    if ( e->baseurl ) sgml_free(e->baseurl);

    sgml_free(e);
  }
}


static dtd_entity *
find_pentity(dtd *dtd, dtd_symbol *id)
{ dtd_entity *e;

  for(e = dtd->pentities; e; e=e->next)
  { if ( e->name == id )
      return e;
  }

  return NULL;
}


static const char *
entity_file(dtd *dtd, dtd_entity *e)
{ switch(e->type)
  { case ET_SYSTEM:
    case ET_PUBLIC:
    { const char *f;

      if ( (f = find_in_catalogue(e->catalog_location,
				  e->name->name,
				  e->extid,
				  e->exturl,
				  dtd->dialect != DL_SGML)) )
      { char *file;

	if ( is_absolute_path(f) ||
	     !e->baseurl )
	  file = (char *)f;
	else
	  file = localpath(e->baseurl, f);

	return file;
      }
    }
    default:
      return NULL;
  }
}


static const ichar *
entity_value(dtd_parser *p, dtd_entity *e, int *len)
{ const char *file;

  if ( !e->value && (file=entity_file(p->dtd, e)) )
  { int normalise = (e->content == EC_SGML || e->content == EC_CDATA);

    e->value = load_sgml_file_to_charp(file, normalise, &e->length);
  }

  if ( len )
    *len = e->length;

  return e->value;
}

/* Can we represent this character in a normal CDATA string?
*/

static int
representable_char(dtd_parser *p, int chr)
{ if ( chr < 0 )
    return FALSE;			/* should not happen */
  if ( chr < 128 )			/* basic ASCII set */
    return TRUE;
  if ( p->utf8_decode )
    return FALSE;
  if ( chr < OUTPUT_CHARSET_SIZE )
    return TRUE;
  return FALSE;
}


static int
expand_pentities(dtd_parser *p, const ichar *in, ichar *out, int len)
{ dtd *dtd = p->dtd;
  int pero = dtd->charfunc->func[CF_PERO]; /* % */
  int ero = dtd->charfunc->func[CF_ERO]; /* & */
  const ichar *s;

  while(*in)
  { if ( *in == pero )
    { dtd_symbol *id;

      if ( (s = itake_entity_name(dtd, in+1, &id)) )
      { dtd_entity *e = find_pentity(dtd, id);
	const ichar *eval;
	int l;

	in = s;
	if ( (s=isee_func(dtd, s, CF_ERC)) ) /* ; is not obligatory? */
	  in = s;

	if ( !e )
	  return gripe(ERC_EXISTENCE, "parameter entity", id->name);

	if ( !(eval = entity_value(p, e, NULL)) )
	  return FALSE;

	if ( !expand_pentities(p, eval, out, len) )
	  return FALSE;
	l = strlen(out);		/* could be better */
	out += l;
	len -= l;

	continue;
      }
    }

    if ( --len <= 0 )
    { gripe(ERC_REPRESENTATION, "Declaration too long");
      return FALSE;
    }

    if ( *in == ero && in[1] == '#' )	/* &# */
    { int chr;

      if ( (s=isee_character_entity(dtd, in, &chr)) &&
	   representable_char(p, chr) )
      { *out++ = chr;
        in = s;
        continue;
      }
    }

    *out++ = *in++;
  }

  *out = '\0';

  return TRUE;
}


static int
char_entity_value(const ichar *decl)
{ if ( *decl == '#' )
  { const ichar *s = decl+1;
    char *end;
    long v;

					/* do octal too? */
    if ( s[0] == 'x' || s[0] == 'X' )
      v = strtoul((char *)s+1, &end, 16);
    else
      v = strtoul((char *)s, &end, 10);

    if ( *end == '\0' )
    { return (int)v;
    } else if ( istreq(s, "RS") )
    { return '\n';
    } else if ( istreq(s, "RE") )
    { return '\r';
    } else if ( istreq(s, "TAB") )
    { return '\t';
    } else if ( istreq(s, "SPACE") )
    { return ' ';
    }
  }

  return -1;
}


static const ichar *
isee_character_entity(dtd *dtd, const ichar *in, int *chr)
{ const ichar *s;

  if ( (s=isee_func(dtd, in, CF_ERO)) && *s == '#' )
  { ichar e[32];
    ichar *o = e;
    int v;

    *o++ = *s++;
    while(o < e+sizeof(e)-1 && HasClass(dtd, *s, CH_NAME))
      *o++ = *s++;
    if ( isee_func(dtd, s, CF_ERC))	/* skip ; */
      s++;

    *o = '\0';
    if ( (v=char_entity_value(e)) >= 0 )
    { *chr = v;
      return s;
    }
  }

  return NULL;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Expand entities in a string.  Used to expand CDATA attribute values.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
expand_entities(dtd_parser *p, const ichar *in, ochar *out, int len)
{ const ichar *s;
  dtd *dtd = p->dtd;
  int ero = dtd->charfunc->func[CF_ERO]; /* & */
  const ochar *map = dtd->charmap->map;

  while(*in)
  { if ( *in == ero )
    { const ichar *estart = in;		/* for recovery */
      int chr;

      if ( (s=isee_character_entity(dtd, in, &chr)) )
      { if ( chr <= 0 || chr >= OUTPUT_CHARSET_SIZE )
	  gripe(ERC_REPRESENTATION, "character");
	if ( --len <= 0 )
	  return gripe(ERC_REPRESENTATION, "CDATA string too long");
	*out++ = chr;
	in = s;
	continue;
      }

      if ( HasClass(dtd, in[1], CH_NMSTART) )
      { dtd_symbol *id;
	dtd_entity *e;
	const ichar *eval;
	int l;
	
	in = itake_name(dtd, in+1, &id);
	if ( isee_func(dtd, in, CF_ERC) || *in == '\n' )
	  in++;
  
	if ( !(e = id->entity) && !(e=dtd->default_entity) )
	{ gripe(ERC_EXISTENCE, "entity", id->name);
	  in = estart;
	  goto recover;
	}
  
	if ( !(eval = entity_value(p, e, NULL)) )
	{ gripe(ERC_NO_VALUE, e->name->name);
	  in = estart;
	  goto recover;
	}

	if ( !expand_entities(p, eval, out, len) )
	  return FALSE;
	l = ostrlen(out);		/* could be better */
	out += l;
	len -= l;

	continue;
      }
    }

  recover:
    if ( --len <= 0 )
      return gripe(ERC_REPRESENTATION, "CDATA string too long");

#ifdef UTF8
    if ( p->utf8_decode && ISUTF8_MB(*in) )
    { int chr;

      in = __utf8_get_char(in, &chr);
      if ( chr >= OUTPUT_CHARSET_SIZE )
	gripe(ERC_REPRESENTATION, "character");
      *out++ = chr;
    }
#endif
    *out++ = map[*in++];
  }

  *out = 0;

  return TRUE;
}



		 /*******************************
		 *	      ELEMENTS		*
		 *******************************/

static dtd_element *
find_element(dtd *dtd, dtd_symbol *id)
{ dtd_element *e;

  if ( id->element )
    return id->element;			/* must check */

  e = sgml_calloc(1, sizeof(*e));
  e->space_mode = SP_INHERIT;
  e->undefined = TRUE;
  e->name = id;
  id->element = e;
  
  e->next = dtd->elements;
  dtd->elements = e;

  return e;
}


static dtd_element *
def_element(dtd *dtd, dtd_symbol *id)
{ dtd_element *e = find_element(dtd, id);

  if ( !e->structure )
  { e->structure = sgml_calloc(1, sizeof(*e->structure));
    e->structure->references = 1;
    e->structure->type = C_EMPTY;
  }

  return e;
}


static void
free_name_list(dtd_name_list *nl)
{ dtd_name_list *next;

  for( ; nl; nl=next)
  { next = nl->next;

    sgml_free(nl);
  }
}


static void
free_attribute(dtd_attr *a)
{ if ( --a->references == 0 )
  { switch(a->type)
    { case AT_NAMEOF:
      case AT_NOTATION:
	free_name_list(a->typeex.nameof);
      default:
	;
    }
    switch(a->def)
    { case AT_DEFAULT:
      { if ( a->type == AT_CDATA )
	  sgml_free(a->att_def.cdata);
	else if ( a->islist )
	  sgml_free(a->att_def.list);
      }
      default:
	;
    }

    sgml_free(a);
  }
}


static void
free_attribute_list(dtd_attr_list *l)
{ dtd_attr_list *next;

  for(; l; l=next)
  { next = l->next;

    free_attribute(l->attribute);
    sgml_free(l);
  } 
}


static void
free_element_list(dtd_element_list *l)
{ dtd_element_list *next;

  for( ; l; l=next)
  { next = l->next;

    sgml_free(l);
  }
}


static void
free_element_definition(dtd_edef *def)
{ if ( --def->references == 0 )
  { if ( def->content )
      free_model(def->content);
    free_element_list(def->included);
    free_element_list(def->excluded);
    free_state_engine(def->initial_state);

    sgml_free(def);
  }
}


static void
free_elements(dtd_element *e)
{ dtd_element *next;

  for( ; e; e=next)
  { next = e->next;

    if ( e->structure )
      free_element_definition(e->structure);
    free_attribute_list(e->attributes);

    sgml_free(e);
  }
}


		 /*******************************
		 *	    ATTRIBUTES		*
		 *******************************/

static dtd_attr *
find_attribute(dtd_element *e, dtd_symbol *name)
{ dtd_attr_list *a;

  for(a=e->attributes; a; a=a->next)
  { if ( a->attribute->name == name )
      return a->attribute;
  }

  return NULL;
}


		 /*******************************
		 *	  PARSE PRIMITIVES	*
		 *******************************/

static const ichar *
iskip_layout(dtd *dtd, const ichar *in)
{ ichar cmt = dtd->charfunc->func[CF_CMT]; /* also skips comment */

  for( ; *in; in++ )
  { if ( HasClass(dtd, *in, CH_BLANK) )
      continue;

    if ( in[0] == cmt && in[1] == cmt )
    { in += 2;

      for( ; *in; in++ )
      { if ( in[0] == cmt && in[1] == cmt )
	  break;
      }
      in++;
      continue;
    }

    return in;
  }

  return in;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
See whether we are looking at identifier   "id". "id" must be lowercase!
This is only used for reserved words,  and parsed case-insentive in both
XML and SGML modes.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static const ichar *
isee_identifier(dtd *dtd, const ichar *in, char *id)
{ in = iskip_layout(dtd, in);

					/* match */
  while (*id && *id == tolower(*in) )
    id++, in++;
  if ( *id == 0 && !HasClass(dtd, *in, CH_NAME) )
    return iskip_layout(dtd, in);

  return NULL;
}


#if 0
static const ichar *
isee_text(dtd *dtd, const ichar *in, char *id)
{ while (*id && *id == tolower(*in) )
    id++, in++;

  if ( *id == 0 )
    return in;

  return NULL;
}
#endif


static const ichar *
itake_name(dtd *dtd, const ichar *in, dtd_symbol **id)
{ ichar buf[MAXNMLEN];
  ichar *o = buf;

  in = iskip_layout(dtd, in);
  if ( !HasClass(dtd, *in, CH_NMSTART) )
    return NULL;
  if ( dtd->case_sensitive )
  { while( HasClass(dtd, *in, CH_NAME) )
      *o++ = *in++;
  } else
  { while( HasClass(dtd, *in, CH_NAME) )
      *o++ = tolower(*in++);
  }
  *o++ = '\0';

  *id = dtd_add_symbol(dtd, buf);

  return iskip_layout(dtd, in);
}


static const ichar *
itake_entity_name(dtd *dtd, const ichar *in, dtd_symbol **id)
{ ichar buf[MAXNMLEN];
  ichar *o = buf;

  in = iskip_layout(dtd, in);
  if ( !HasClass(dtd, *in, CH_NMSTART) )
    return NULL;
  if ( dtd->ent_case_sensitive )
  { while( HasClass(dtd, *in, CH_NAME) )
      *o++ = *in++;
  } else
  { while( HasClass(dtd, *in, CH_NAME) )
      *o++ = tolower(*in++);
  }
  *o++ = '\0';

  *id = dtd_add_symbol(dtd, buf);

  return in;
}


static const ichar *
itake_nmtoken(dtd *dtd, const ichar *in, dtd_symbol **id)
{ ichar buf[MAXNMLEN];
  ichar *o = buf;

  in = iskip_layout(dtd, in);
  if ( !HasClass(dtd, *in, CH_NAME) )
    return NULL;
  if ( dtd->case_sensitive )
  { while( HasClass(dtd, *in, CH_NAME) )
      *o++ = *in++;
  } else
  { while( HasClass(dtd, *in, CH_NAME) )
      *o++ = tolower(*in++);
  }
  *o = '\0';

  *id = dtd_add_symbol(dtd, buf);

  return iskip_layout(dtd, in);
}


static const ichar *
itake_nutoken(dtd *dtd, const ichar *in, dtd_symbol **id)
{ ichar buf[MAXNMLEN];
  ichar *o = buf;

  in = iskip_layout(dtd, in);
  if ( !HasClass(dtd, *in, CH_DIGIT) )
    return NULL;
  if ( dtd->case_sensitive )
  { while( HasClass(dtd, *in, CH_NAME) )
      *o++ = *in++;
  } else
  { while( HasClass(dtd, *in, CH_NAME) )
      *o++ = tolower(*in++);
  }
  *o = '\0';
  if ( o - buf > 8 )
    gripe(ERC_LIMIT, "nutoken length");

  *id = dtd_add_symbol(dtd, buf);

  return iskip_layout(dtd, in);
}


static const ichar *
itake_number(dtd *dtd, const ichar *in, dtd_attr *at)
{ in = iskip_layout(dtd, in);

  switch(dtd->number_mode)
  { case NU_TOKEN:
    { ichar buf[MAXNMLEN];
      ichar *o = buf;

      while( HasClass(dtd, *in, CH_DIGIT) )
	*o++ = *in++;
      if ( o == buf )
	return NULL;			/* empty */
      *o = '\0';
      at->att_def.name = dtd_add_symbol(dtd, buf);

      return iskip_layout(dtd, (const ichar *)in);
    }
    case NU_INTEGER:
    { char *end;

      at->att_def.number = strtol((const char *)in, &end, 10);
      if ( end > (char *)in && errno != ERANGE )
	return iskip_layout(dtd, (const ichar *)end);
    }
  }

  return NULL;
}


static const ichar *
itake_string(dtd *dtd, const ichar *in, ichar *out, int len)
{ in = iskip_layout(dtd, in);

  if ( isee_func(dtd, in, CF_LIT) ||
       isee_func(dtd, in, CF_LITA) )
  { ichar q = *in++;

    while( *in && *in != q )
    { *out++ = *in++;
      if ( --len == 0 )
      { gripe(ERC_REPRESENTATION, "String too long");
	return NULL;
      }
    }
    if ( *in )
    { *out = '\0';
      return iskip_layout(dtd, ++in);
    }
  }

  return NULL;
}


static const ichar *
itake_dubbed_string(dtd *dtd, const ichar *in, ichar **out)
{ ichar buf[MAXSTRINGLEN];
  const ichar *end;

  if ( (end=itake_string(dtd, in, buf, sizeof(buf))) )
    *out = istrdup(buf);

  return end;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
itake_url() is used to get the argument of a SYSTEM or 2nd argument of a
PUBLIC reference. Once upon a  time  it   tried  to  tag the argument as
file:<path>, but this job cannot be before   lookup in the catalogue. We
could have replaced the calls  with   itake_string(),  but I'll leave it
this way for documentation purposes.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static const ichar *
itake_url(dtd *dtd, const ichar *in, ichar **out)
{ ichar buf[MAXSTRINGLEN];
  const ichar *end;

  if ( (end=itake_string(dtd, in, buf, sizeof(buf))) )
  { *out = istrdup(buf);
  }

  return end;
}


static const ichar *
itake_nmtoken_chars(dtd *dtd, const ichar *in, ichar *out, int len)
{ in = iskip_layout(dtd, in);
  if ( !HasClass(dtd, *in, CH_NAME) )
    return NULL;
  while( HasClass(dtd, *in, CH_NAME) )
  { if ( --len <= 0 )
      gripe(ERC_REPRESENTATION, "Name token too long");
    *out++ = (dtd->case_sensitive ? *in++ : tolower(*in++));
  }
  *out++ = '\0';

  return iskip_layout(dtd, in);
}


/*  There used to be a function

    itake_nonblank_chars(dtd, in, out, len) -> new end

    which
    - skipped layout,
    - copied characters from in[] to out[] until layout or \0 was found,
    - added a terminating \0 to out[],
    - skipped any following layout, and
    - returned the new position.

    That function was only called by get_attribute_value(), which used
    it to parse an unquoted attribute value.  According to SGML, that's
    not right:  unquoted attribute values must look like NMTOKENs (but
    have a different length bound).  In particular, elements like
	<foo a=bar>zoo</foo>
	<foo a=ugh/zip/
    are perfectly legal, so scanning an unquoted attribute value MUST
    stop at a '/' or '>'.  According to HTML practice, pretty much any
    old junk will be accepted, and some HTML parsers will allow bare
    slashes in such an attribute.

    Typical HTML is *so* bad that it doesn't agree with *any* part of
    the HTML specifications (e.g., <FONT> is commonly wrapped around
    block-level elements, which has never been legal).  It's not clear
    that there is much point in trying to accomodate bad HTML; if you
    really need to do that, use the free program HTML Tidy (from the
    http://www.w3c.org/ site) to clean up, and parse its output instead.

    However, in order to break as little as possible, the new (sgml-1.0.14)
    function accepts anything except > / \0 and blanks.

JW: I decided to accept / as part of an unquoted in SGML-mode if
    shorttag is disabled as well as in XML mode if it is not the
    end of the begin-element
*/

static ichar const *
itake_unquoted(dtd *dtd, ichar const *in, ichar *out, int len)
{ ichar const end2 = dtd->charfunc->func[CF_ETAGO2];	/* / */
  ichar c;

  /* skip leading layout.  Do NOT skip comments! --x-- is a value! */
  while (c = *in, HasClass(dtd, c, CH_BLANK))
    in++;

  /* copy the attribute to out[] */
  while ( !HasClass(dtd, c, CH_BLANK) &&
	  c != '\0' )
  { if ( c == end2 && (dtd->shorttag ||
		       (in[1] == '\0' && dtd->dialect != DL_SGML)) )
      break;

    if ( --len > 0 )
      *out++ = c;
    else if ( len == 0 )
      gripe(ERC_REPRESENTATION, "Attribute too long");
    c = *++in;
  }
  *out = '\0';

  /* skip trailing layout.  While it is kind to skip comments here,
     it is technically wrong to do so.  Tags may not contain comments.
   */

  return iskip_layout(dtd, in);
}


		 /*******************************
		 *		DTD		*
		 *******************************/

dtd *
new_dtd(const ichar *doctype)
{ dtd *dtd = sgml_calloc(1, sizeof(*dtd));

  dtd->magic	 = SGML_DTD_MAGIC;
  dtd->implicit  = TRUE;
  dtd->dialect   = DL_SGML;
  if ( doctype )
    dtd->doctype = istrdup(doctype);
  dtd->symbols	 = new_symbol_table();
  dtd->charclass = new_charclass();
  dtd->charfunc	 = new_charfunc();
  dtd->charmap	 = new_charmap();
  dtd->space_mode = SP_SGML;
  dtd->ent_case_sensitive = TRUE;	/* case-sensitive entities */
  dtd->shorttag    = TRUE;		/* allow for <tag/value/ */
  dtd->number_mode = NU_TOKEN;

  return dtd;
}


void
free_dtd(dtd *dtd)
{ if ( --dtd->references == 0 )
  { if ( dtd->doctype )
      sgml_free(dtd->doctype);
  
    free_entity_list(dtd->entities);
    free_entity_list(dtd->pentities);
    free_notations(dtd->notations);
    free_shortrefs(dtd->shortrefs);
    free_elements(dtd->elements);
    free_symbol_table(dtd->symbols);
    sgml_free(dtd->charfunc);
    sgml_free(dtd->charclass);
    sgml_free(dtd->charmap);
    dtd->magic = 0;
  
    sgml_free(dtd);
  }
}


static char *xml_entities[] =
{ "lt CDATA \"&#60;\"",			/* < */
  "gt CDATA \"&#62;\"",			/* > */
  "amp CDATA \"&#38;\"",		/* & */
  "apos CDATA \"&#39;\"",		/* ' */
  "quot CDATA \"&#34;\"",		/* " */
  NULL
};


int
set_dialect_dtd(dtd *dtd, dtd_dialect dialect)
{ dtd->dialect = dialect;

  switch(dialect)
  { case DL_SGML:
    { dtd->case_sensitive = FALSE;
      dtd->space_mode = SP_SGML;
      dtd->shorttag = TRUE;
      break;
    }
    case DL_XML:
    case DL_XMLNS:
    { char **el;
      dtd_parser p;

      dtd->case_sensitive = TRUE;
      dtd->encoding = ENC_UTF8;
      dtd->space_mode = SP_PRESERVE;
      dtd->shorttag = FALSE;

      memset(&p, 0, sizeof(p));
      p.dtd = dtd;
      for(el = xml_entities; *el; el++)
	process_entity_declaration(&p, *el);

      break;
    }
  }

  return TRUE;
}


int
set_option_dtd(dtd *dtd, dtd_option option, int set)
{ switch(option)
  { case OPT_SHORTTAG:
      dtd->shorttag = set;
      break;
  }

  return TRUE;
}


static ichar *
baseurl(dtd_parser *p)
{ if ( p->location.type == IN_FILE && p->location.name )
  { return istrdup(p->location.name);
  }

  return NULL;
}


static const ichar *
process_entity_value_declaration(dtd_parser *p,
				 const ichar *decl, dtd_entity *e)
{ dtd *dtd = p->dtd;
  const ichar *s;

  if ( e->type == ET_SYSTEM )
  { if ( (s=itake_url(dtd, decl, &e->exturl)) )
    { e->baseurl = baseurl(p);
      return s;
    }

    goto string_expected;
  } else
  { ichar buf[MAXSTRINGLEN];
    ichar val[MAXSTRINGLEN];

    if ( !(s = itake_string(dtd, decl, buf, sizeof(buf))) )
      goto string_expected;
    decl = s;

    expand_pentities(p, buf, val, sizeof(val));

    switch ( e->type )
    { case ET_PUBLIC:
      { e->extid = istrdup(val);
	if ( isee_func(dtd, decl, CF_LIT) ||
	     isee_func(dtd, decl, CF_LITA) )
	{ if ( (s=itake_url(dtd, decl, &e->exturl)) )
	  { e->baseurl = baseurl(p);
	    decl = s;
	  }
	}
	return decl;
      }
      case ET_LITERAL:
      { e->value = istrdup(val);
	e->length = strlen(e->value);
	return decl;
      }
      default:
	assert(0);
	return NULL;
    }
  }

string_expected:
  gripe(ERC_SYNTAX_ERROR, "String expected", decl);
  return NULL;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The sgml-standard tells us to accept the  first definition of an entity,
silently suppressing any further attempt to redefine the entity.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
process_entity_declaration(dtd_parser *p, const ichar *decl)
{ dtd *dtd = p->dtd;
  const ichar *s;
  dtd_symbol *id;
  dtd_entity *e;
  int isparam;
  int isdef = FALSE;
					/* parameter entity */
  if ( (s=isee_func(dtd, decl, CF_PERO)) )
  { isparam = TRUE;
    decl = s;
  } else
    isparam = FALSE;

  if ( !(s = itake_entity_name(dtd, decl, &id)) )
  { if ( !(s = isee_identifier(dtd, decl, "#default")) )
      return gripe(ERC_SYNTAX_ERROR, "Name expected", decl);
    id = dtd_add_symbol(dtd, "#DEFAULT");
    isdef = TRUE;
  }

  if ( isparam && find_pentity(dtd, id) )
  { gripe(ERC_REDEFINED, "parameter entity", id);
    return TRUE;			/* already defined parameter entity */
  }
  if ( id->entity )
  { gripe(ERC_REDEFINED, "entity", id);
    return TRUE;			/* already defined normal entity */
  }

  decl = iskip_layout(dtd, s);
  e = sgml_calloc(1, sizeof(*e));
  e->name = id;
  e->catalog_location = (isparam ? CAT_PENTITY : CAT_ENTITY);

  if ( (s = isee_identifier(dtd, decl, "system")) )
  { e->type = ET_SYSTEM;
    e->content = EC_SGML;
    decl = s;
  } else if ( (s = isee_identifier(dtd, decl, "public")) )
  { e->type = ET_PUBLIC;
    e->content = EC_SGML;
    decl = s;
  } else
  { e->type = ET_LITERAL;

    if ( !isparam )
    { if ( (s=isee_identifier(dtd, decl, "cdata")) )
      { decl = s;
	e->content = EC_CDATA;
      } else if ( (s=isee_identifier(dtd, decl, "sdata")) )
      { decl = s;
	e->content = EC_SDATA;
      } else if ( (s=isee_identifier(dtd, decl, "pi")) )
      { decl = s;
	e->content = EC_PI;
      } else if ( (s=isee_identifier(dtd, decl, "starttag")) )
      { decl = s;
	e->content = EC_STARTTAG;
      } else if ( (s=isee_identifier(dtd, decl, "endtag")) )
      { decl = s;
	e->content = EC_ENDTAG;
      } else
	e->content = EC_SGML;
    }
  }

  if ( (decl=process_entity_value_declaration(p, decl, e)) )
  { if ( e->type == ET_LITERAL )
    { switch(e->content)
      { case EC_STARTTAG:
	{ ichar *buf = sgml_malloc((e->length + 3)*sizeof(ichar));

	  buf[0] = dtd->charfunc->func[CF_STAGO];
	  istrcpy(&buf[1], e->value);
	  buf[++e->length] = dtd->charfunc->func[CF_STAGC];
	  buf[++e->length] = 0;

	  sgml_free(e->value);
	  e->value = buf;
	  e->content = EC_SGML;

	  break;
	}
	case EC_ENDTAG:
	{ ichar *buf = sgml_malloc((e->length + 4)*sizeof(ichar));

	  buf[0] = dtd->charfunc->func[CF_ETAGO1];
	  buf[1] = dtd->charfunc->func[CF_ETAGO2];
	  istrcpy(&buf[2], e->value);
	  e->length++;
	  buf[++e->length] = dtd->charfunc->func[CF_STAGC];
	  buf[++e->length] = 0;

	  sgml_free(e->value);
	  e->value = buf;
	  e->content = EC_SGML;

	  break;
	}
	default:
	  break;
      }
    } else
    { if ( *decl )
      { dtd_symbol *nname;

	if ( (s=isee_identifier(dtd, decl, "cdata")) )
	{ decl = s;
	  e->content = EC_CDATA;
	} else if ( (s=isee_identifier(dtd, decl, "sdata")) )
	{ decl = s;
	  e->content = EC_SDATA;
	} else if ( (s=isee_identifier(dtd, decl, "ndata")) )
	{ decl = s;
	  e->content = EC_NDATA;
	} else
	  return gripe(ERC_SYNTAX_ERROR, "Bad datatype declaration", decl);
  
	if ( (s=itake_name(dtd, decl, &nname)) ) /* what is this? */
	{ decl = s;
	} else
	  return gripe(ERC_SYNTAX_ERROR, "Bad notation declaration", decl);
      }
    }

    if ( *decl )
      return gripe(ERC_SYNTAX_ERROR, "Unexpected end of declaraction", decl);
  }

  if ( isparam )
  { e->next = dtd->pentities;
    dtd->pentities = e;
  } else
  { e->name->entity = e;
    e->next = dtd->entities;
    dtd->entities = e;
  }
  
  if ( isdef )
    dtd->default_entity = e;

  return TRUE;
}


		 /*******************************
		 *	      NOTATIONS		*
		 *******************************/

static dtd_notation *
find_notation(dtd *dtd, dtd_symbol *name)
{ dtd_notation *n;

  for(n=dtd->notations; n; n = n->next)
  { if ( n->name == name )
      return n;
  }

  return NULL;
}


static void
add_notation(dtd *dtd, dtd_notation *not)
{ dtd_notation *n;

  for(n=dtd->notations; n; n = n->next)
  { if ( !n->next )
    { n->next = not;
      break;
    }
  }
}


static int
process_notation_declaration(dtd_parser *p, const ichar *decl)
{ dtd *dtd = p->dtd;
  dtd_symbol *nname;
  const ichar *s;
  ichar *system = NULL, *public = NULL;
  dtd_notation *not;

  if ( !(s=itake_name(dtd, decl, &nname)) )
    return gripe(ERC_SYNTAX_ERROR, "Notation name expected", decl);
  decl = s;

  if ( find_notation(dtd, nname) )
  { gripe(ERC_REDEFINED, "notation", nname);
    return TRUE;
  }

  if ( (s=isee_identifier(dtd, decl, "system")) )
  { ;
  } else if ( (s=isee_identifier(dtd, decl, "public")) )
  { decl = s;
    if ( !(s=itake_dubbed_string(dtd, decl, &public)) )
      return gripe(ERC_SYNTAX_ERROR, "Public identifier expected", decl);
  } else
    return gripe(ERC_SYNTAX_ERROR, "SYSTEM or PUBLIC expected", decl);

  decl = s;
  if ( (s=itake_dubbed_string(dtd, decl, &system)) )
    decl = s;

  if ( *decl )
    return gripe(ERC_SYNTAX_ERROR, "Unexpected end of declaraction", decl);

  not = sgml_calloc(1, sizeof(*not));
  not->name = nname;
  not->system = system;
  not->public = public;
  not->next = NULL;
  add_notation(dtd, not);

  return TRUE;
}


static void
free_notations(dtd_notation *n)
{ dtd_notation *next;

  for( ; n; n=next)
  { next = n->next;

    sgml_free(n->system);
    sgml_free(n->public);

    sgml_free(n);
  }
}

		 /*******************************
		 *	       SHORTREF		*
		 *******************************/

static void
free_maps(dtd_map *map)
{ dtd_map *next;

  for( ; map; map=next)
  { next = map->next;
    if ( map->from )
      sgml_free(map->from);
    sgml_free(map);
  }
}


static void
free_shortrefs(dtd_shortref *sr)
{ dtd_shortref *next;

  for( ; sr; sr=next)
  { next = sr->next;
    free_maps(sr->map);
    sgml_free(sr);
  }
}


static const ichar *
shortref_add_map(dtd *dtd, const ichar *decl, dtd_shortref *sr)
{ ichar buf[MAXSTRINGLEN];
  ichar from[MAXMAPLEN];
  ichar *f = from;
  dtd_symbol *to;
  const ichar *s;
  const ichar *end;
  dtd_map **p;
  dtd_map *m;

  if ( !(s=itake_string(dtd, decl, buf, sizeof(buf))) )
  { gripe(ERC_SYNTAX_ERROR, "map-string expected", decl);
    return NULL;
  }
  decl = s;
  if ( !(s=itake_entity_name(dtd, decl, &to)) )
  { gripe(ERC_SYNTAX_ERROR, "map-to name expected", decl);
    return NULL;
  }
  end = s;

  for(decl=buf; *decl;)
  { if ( *decl == 'B' )		/* blank */
    { if ( decl[1] == 'B' )
      { *f++ = CHR_DBLANK;
	decl += 2;
        continue;
      }
      *f++ = CHR_BLANK;
      decl++;
    } else
      *f++ = *decl++;			/* any other character */
  }
  *f = 0;

  for(p=&sr->map; *p; p = &(*p)->next)
    ;
  
  m = sgml_calloc(1, sizeof(*m));
  m->from = istrdup(from);
  m->len  = istrlen(from);
  m->to   = to;

  *p = m;

  return end;
}


static dtd_shortref *
def_shortref(dtd_parser *p, dtd_symbol *name)
{ dtd *dtd = p->dtd;
  dtd_shortref *sr, **pr;

  for(pr=&dtd->shortrefs; *pr; pr = &(*pr)->next)
  { dtd_shortref *r = *pr;

    if ( r->name == name )
      return r;
  }
  
  sr = sgml_calloc(1, sizeof(*sr));
  sr->name = name;
  *pr = sr;

  return sr;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Create an array with TRUE in any character   that can be the last of the
shortref map.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
compile_map(dtd *dtd, dtd_shortref *sr)
{ dtd_map *map;

  for(map = sr->map; map; map = map->next)
  { ichar last = map->from[map->len-1];

    switch( last )
    { case CHR_BLANK:
      case CHR_DBLANK:
      { int i;

	for( i=0; i< ICHARSET_SIZE; i++)
	{ if ( HasClass(dtd, i, CH_BLANK) )
	    sr->ends[i] = TRUE;
	}
      }

      default:
	sr->ends[last] = TRUE;
    }
  }
}


static int
process_shortref_declaration(dtd_parser *p, const ichar *decl)
{ dtd *dtd = p->dtd;
  ichar buf[MAXDECL];
  dtd_shortref *sr;
  dtd_symbol *name;
  const ichar *s;

  if ( !expand_pentities(p, decl, buf, sizeof(buf)) )
    return FALSE;
  decl = buf;

  if ( !(s=itake_name(dtd, decl, &name)) )
    return gripe(ERC_SYNTAX_ERROR, "Name expected", decl);
  decl = s;

  sr = def_shortref(p, name);
  if ( sr->defined )
  { gripe(ERC_REDEFINED, "shortref", name);

    return TRUE;
  }

  sr->defined = TRUE;

  while( *(decl = iskip_layout(dtd, decl)) != '\0'
	 && (s=shortref_add_map(dtd, decl, sr)) )
    decl = s;
  compile_map(dtd, sr);

  if ( *decl )
    return gripe(ERC_SYNTAX_ERROR, "Map expected", decl);
  
  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Find named name.  The name NULL stands for the #empty map

- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static dtd_shortref *
find_map(dtd *dtd, dtd_symbol *name)
{ dtd_shortref *sr;

  if ( !name )
  { static dtd_shortref *empty;

    if ( !empty )
    { empty = sgml_calloc(1, sizeof(*empty));
      empty->name = dtd_add_symbol(dtd, "#EMPTY");
      empty->defined = TRUE;
    }

    return empty;
  }

  for( sr = dtd->shortrefs; sr; sr = sr->next )
  { if ( sr->name == name )
    { if ( !sr->defined )
	break;

      return sr;
    }
  }
       
  return NULL;
}


static void
set_map_element(dtd_element *e, void *closure)
{ e->map = closure;
}


static int
process_usemap_declaration(dtd_parser *p, const ichar *decl)
{ dtd *dtd = p->dtd;
  ichar buf[MAXDECL];
  dtd_symbol *name;
  const ichar *s;
  dtd_symbol *ename;
  dtd_element *e;
  dtd_shortref *map;

  if ( !expand_pentities(p, decl, buf, sizeof(buf)) )
    return FALSE;
  decl = buf;

  if ( !(s=itake_name(dtd, decl, &name)) )
  { if ( (s=isee_identifier(dtd, decl, "#empty")) )
      name = NULL;
    else
      return gripe(ERC_SYNTAX_ERROR, "map-name expected", decl);
  }

  decl = s;
  if ( !(map = find_map(dtd, name)) )
    map = def_shortref(p, name);	/* make undefined map */

  if ( isee_func(dtd, decl, CF_GRPO) )	/* ( */
  { dtd_model *model;

    if ( (model = make_model(dtd, decl, &s)) )
    { for_elements_in_model(model, set_map_element, map);
      free_model(model);
      decl = s;
    } else
      return FALSE;
  } else if ( (s=itake_name(dtd, decl, &ename)) )
  { e = find_element(dtd, ename);
    e->map = map;
    decl = s;
  } else if ( p->environments )
  { if ( !map->defined )
      gripe(ERC_EXISTENCE, "map", name->name);

    p->environments->map = map;
    p->map = p->environments->map;
  } else
    return gripe(ERC_SYNTAX_ERROR, "element-name expected", decl);

  if ( *decl )
    return gripe(ERC_SYNTAX_ERROR, "Unparsed", decl);

  return TRUE;
}


static int
match_map(dtd *dtd, dtd_map *map, int len, ichar *data)
{ ichar *e = data+len-1;
  ichar *m = map->from+map->len-1;

  while( m >= map->from )
  { if ( e < data )
      return 0;

    if ( *m == *e )
    { m--;
      e--;
      continue;
    }
    if ( *m == CHR_DBLANK )
    { if ( e>data && HasClass(dtd, *e, CH_WHITE) )
	e--;
      else
	return FALSE;
      goto blank;
    }
    if ( *m == CHR_BLANK )
    { blank:
      while( e>data && HasClass(dtd, *e, CH_WHITE) )
	e--;
      m--;
      continue;
    }
    return 0;
  }

  return data+len-1-e;
}


static void
match_shortref(dtd_parser *p)
{ dtd_map *map;

  for(map = p->map->map; map; map = map->next)
  { int len;

    if ( (len=match_map(p->dtd, map,
			p->cdata->size, (ichar *)p->cdata->data)) )
    { p->cdata->size -= len;

      if ( p->cdata_must_be_empty )
      { int blank = TRUE;
	const ichar *s;
	int i;

	for(s = p->cdata->data, i=0; i++ < p->cdata->size; s++)
	{ if ( !HasClass(p->dtd, *s, CH_BLANK) )
	  { blank = FALSE;
	    break;
	  }
	}

	p->blank_cdata = blank;
      }

      WITH_CLASS(p, EV_SHORTREF,
		 { sgml_cplocation(&p->startloc, &p->location);
		   p->startloc.charpos -= len;
		   p->startloc.linepos -= len;
		   if ( p->startloc.linepos < 0 )
		   { p->startloc.line--;
		     p->startloc.linepos = 0; /* not correct! */
		   }
		   DEBUG(printf("%d-%d: Matched map '%s' --> %s, len = %d\n",
				p->startloc.charpos,
				p->location.charpos,
				map->from, map->to->name, len));

		   process_entity(p, map->to->name);
		 })			/* TBD: optimise */
      break;
    }
  }
}


		 /*******************************
		 *	       ELEMENTS		*
		 *******************************/

static void
add_submodel(dtd_model *m, dtd_model *sub)
{ dtd_model **d;

  for( d = &m->content.group; *d; d = &(*d)->next )
    ;
  *d = sub;
}


/* for_elements_in_model()
   Walk along the model, calling f(e, closure) for any element found
   in the model.  Used for <!SHORTREF name model>
*/

static void
for_elements_in_model(dtd_model *m,
		      void (*f)(dtd_element *e, void *closure),
		      void *closure)
{ switch(m->type)
  { case MT_SEQ:
    case MT_AND:
    case MT_OR:
    { dtd_model *sub = m->content.group;

      for(; sub; sub = sub->next)
	for_elements_in_model(sub, f, closure);
      break;
    }
    case MT_ELEMENT:
      (*f)(m->content.element, closure);
      break;
    default:
      ;
  }
}


static void
free_model(dtd_model *m)
{ switch(m->type)
  { case MT_SEQ:
    case MT_AND:
    case MT_OR:
    { dtd_model *sub = m->content.group;
      dtd_model *next;

      for(; sub; sub = next)
      { next = sub->next;

	free_model(sub);
      }
    }
    default:
      ;
  }

  sgml_free(m);
}


static dtd_model *
make_model(dtd *dtd, const ichar *decl, const ichar **end)
{ const ichar *s;
  dtd_model *m = sgml_calloc(1, sizeof(*m));
  dtd_symbol *id;

  decl = iskip_layout(dtd, decl);

  if ( (s=isee_identifier(dtd, decl, "#pcdata")) )
  { m->type = MT_PCDATA;
    m->cardinality = MC_ONE;		/* actually don't care */
    *end = s;
    return m;
  }

  if ( (s=itake_name(dtd, decl, &id)) )
  { m->type = MT_ELEMENT;
    m->content.element = find_element(dtd, id);
    decl = s;
  } else
  { if ( !(s=isee_func(dtd, decl, CF_GRPO)) )
    { gripe(ERC_SYNTAX_ERROR, "Name group expected", decl);
      free_model(m);
      return NULL;
    }
    decl = s;

    for(;;)
    { dtd_model *sub;
      modeltype mt;

      if ( !(sub = make_model(dtd, decl, &s)) )
	return NULL;
      decl = s;
      add_submodel(m, sub);
      
      if ( (s = isee_func(dtd, decl, CF_OR)) )
      { decl = s;
	mt = MT_OR;
      } else if ( (s = isee_func(dtd, decl, CF_SEQ)) )
      { decl = s;
	mt = MT_SEQ;
      } else if ( (s = isee_func(dtd, decl, CF_AND)) )
      { decl = s;
	mt = MT_AND;
      } else if ( (s = isee_func(dtd, decl, CF_GRPC)) )
      { decl = s;
	break;
      } else
      { gripe(ERC_SYNTAX_ERROR, "Connector ('|', ',' or '&') expected", decl);
	free_model(m);
	return NULL;
      }
      decl = iskip_layout(dtd, decl);

      if ( m->type != mt )
      { if ( !m->type )
	  m->type = mt;
	else
	{ gripe(ERC_SYNTAX_ERROR, "Different connector types in model", decl);
	  free_model(m);
	  return NULL;
	}
      }
    }
  }

  if ( (s = isee_func(dtd, decl, CF_OPT)) )
  { decl = s;
    m->cardinality = MC_OPT;
  } else if ( (s=isee_func(dtd, decl, CF_REP)) )
  { decl = s;
    m->cardinality = MC_REP;
  } else if ( (s=isee_func(dtd, decl, CF_PLUS)) )
  {					/* ROK: watch out for (x) +(y) */
    if ( isee_func(dtd, iskip_layout(dtd, s), CF_GRPO) == NULL )
    { decl = s;
      m->cardinality = MC_PLUS;
    }
  } else
    m->cardinality = MC_ONE;
    
  if ( m->type == MT_UNDEF )		/* simplify (e+), etc. */
  { dtd_model *sub = m->content.group;
    modelcard card;

    assert(!sub->next);
    if ( sub->cardinality == MC_ONE )
      card = m->cardinality;
    else if ( m->cardinality == MC_ONE )
      card = sub->cardinality;
    else
    { m->type = MT_OR;
      goto out;
    }
      
    *m = *sub;
    m->cardinality = card;
    sgml_free(sub);
  }

out:
  *end = iskip_layout(dtd, decl);
  return m;
}


static const ichar *
process_model(dtd *dtd, dtd_edef *e, const ichar *decl)
{ const ichar *s;

  decl = iskip_layout(dtd, decl);
  if ( (s = isee_identifier(dtd, decl, "empty")) )
  { e->type = C_EMPTY;
    return s;
  }
  if ( (s = isee_identifier(dtd, decl, "cdata")) )
  { e->type = C_CDATA;
    return s;
  }
  if ( (s = isee_identifier(dtd, decl, "rcdata")) )
  { e->type = C_RCDATA;
    return s;
  }
  if ( (s = isee_identifier(dtd, decl, "any")) )
  { e->type = C_ANY;
    return s;
  }
  
  e->type = C_PCDATA;
  if ( !(e->content = make_model(dtd, decl, &decl)) )
    return FALSE;

  return decl;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
See a name-group separator.  As long as we haven't decided, this can be
CF_NG.  If we have decided they must all be the same.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static const ichar *
isee_ngsep(dtd *dtd, const ichar *decl, charfunc *sep)
{ const ichar *s;

  if ( (s=isee_func(dtd, decl, *sep)) )
    return iskip_layout(dtd, s);
  if ( *sep == CF_NG )			/* undecided */
  { static const charfunc ng[] = { CF_SEQ, CF_OR, CF_AND };
    int n;

    for(n=0; n<3; n++)
    { if ( (s=isee_func(dtd, decl, ng[n])) )
      { *sep = ng[n];
        return iskip_layout(dtd, s);
      }
    }
  }

  return NULL;
}



static const ichar *
itake_namegroup(dtd *dtd, charfunc sep, const ichar *decl,
		dtd_symbol **names, int *n)
{ const ichar *s;
  int en = 0;

  if ( (s=isee_func(dtd, decl, CF_GRPO)) )
  { for(;;)
    { if ( !(decl=itake_name(dtd, s, &names[en++])) )
      { gripe(ERC_SYNTAX_ERROR, "Name expected", s);
	return NULL;
      }
      if ( (s=isee_func(dtd, decl, sep)) )
      { decl = iskip_layout(dtd, s);
	continue;
      }
      if ( (s=isee_func(dtd, decl, CF_GRPC)) )
      { *n = en;
        decl = s;
	return iskip_layout(dtd, decl);
      }
      gripe(ERC_SYNTAX_ERROR, "Bad name-group", decl);
      return NULL;
    }
  }

  return NULL;
}


typedef struct
{ dtd_symbol **list;
  int size;
} namelist;


static void
add_list_element(dtd_element *e, void *closure)
{ namelist *nl = closure;

  nl->list[nl->size++] = e->name;
}


static const ichar *
itake_el_or_model_element_list(dtd *dtd, const ichar *decl, dtd_symbol **names, int *n)
{ const ichar *s;

  if ( isee_func(dtd, decl, CF_GRPO) )
  { dtd_model *model;

    if ( (model = make_model(dtd, decl, &s)) )
    { namelist nl;
      
      nl.list = names;
      nl.size = 0;
      for_elements_in_model(model, add_list_element, &nl);
      free_model(model);

      *n = nl.size;
      return s;
    } else
      return NULL;
  } else
  { if ( !(s = itake_name(dtd, decl, &names[0])) )
    { gripe(ERC_SYNTAX_ERROR, "Name expected", decl);
      return NULL;
    }
    *n = 1;
    return s;
  }
}


static void
add_element_list(dtd_element_list **l, dtd_element *e)
{ dtd_element_list *n = sgml_calloc(1, sizeof(*n));

  n->value = e;

  for( ; *l; l = &(*l)->next )
    ;
  *l = n;
}


static int
process_element_declaraction(dtd_parser *p, const ichar *decl)
{ dtd *dtd = p->dtd;
  ichar buf[MAXDECL];
  const ichar *s;
  dtd_symbol *eid[MAXATTELEM];
  dtd_edef *def;
  int en;
  int i;

					/* expand parameter entities */
  if ( !expand_pentities(p, decl, buf, sizeof(buf)) )
    return FALSE;
  decl = buf;

  if ( !(s=itake_el_or_model_element_list(dtd, decl, eid, &en)) )
    return gripe(ERC_SYNTAX_ERROR, "Name or name-group expected", decl);
  decl = s;
  if ( en == 0 )
    return TRUE;			/* 0 elements */

  def = sgml_calloc(1, sizeof(*def));
  for(i=0; i<en; i++)
  { find_element(dtd, eid[i]);
    eid[i]->element->structure = def;
    eid[i]->element->undefined = FALSE;
  }
  def->references = en;			/* for GC */

					/* omitted tag declarations (opt) */
  if ( (s = isee_identifier(dtd, decl, "-")) )
  { def->omit_close = FALSE;
    goto seeclose;
  } else if ( (s = isee_identifier(dtd, decl, "o")) )
  { def->omit_open = TRUE;

  seeclose:
    decl = s;
    if ( (s = isee_identifier(dtd, decl, "-")) )
    { def->omit_close = FALSE;
    } else if ( (s = isee_identifier(dtd, decl, "o")) )
    { for(i=0; i<en; i++)
	def->omit_close = TRUE;
    } else
      return gripe(ERC_SYNTAX_ERROR, "Bad omit-tag declaration", decl);

    decl = s;
  }
      
					/* content model */
  if ( !(decl=process_model(dtd, def, decl)) )
    return FALSE;

					/* in/excluded elements */
  if ( decl[0] == '-' || decl[0] == '+' )
  { dtd_symbol *ng[MAXNAMEGROUP];
    int ns;
    dtd_element_list **l;
    
    if ( decl[0] == '-' )
      l = &def->excluded;
    else
      l = &def->included;

    decl++;
    if ( (s=itake_namegroup(dtd, CF_OR, decl, ng, &ns)) )
    { int i;

      decl = s;

      for(i=0; i<ns; i++)
	add_element_list(l, find_element(dtd, ng[i]));
    } else
    { return gripe(ERC_SYNTAX_ERROR, "Name group expected", decl);
    }
  }

  if (*decl)
    return gripe(ERC_SYNTAX_ERROR, "Unexpected end of declaration", decl);

  return TRUE;
}


static void
add_name_list(dtd_name_list **nl, dtd_symbol *s)
{ dtd_name_list *n = sgml_calloc(1, sizeof(*n));
  
  n->value = s;

  for( ; *nl; nl = &(*nl)->next )
    ;

  *nl = n;
}


static void
set_element_properties(dtd_element *e, dtd_attr *a)
{ if ( istreq(a->name->name, "xml:space") )
  { switch(a->def)
    { case AT_FIXED:
      case AT_DEFAULT:
	break;
      default:
	return;
    }

    switch (a->type )
    { case AT_NAMEOF:
      case AT_NAME:
      case AT_NMTOKEN:
	e->space_mode = istr_to_space_mode(a->att_def.name->name);
	break;
      case AT_CDATA:
	e->space_mode = istr_to_space_mode((ichar *)a->att_def.cdata);
	break;
      default:
	break;
    }
  }
}


static void
add_attribute(dtd *dtd, dtd_element *e, dtd_attr *a)
{ dtd_attr_list **l;
  dtd_attr_list *n;

  for(l = &e->attributes; *l; l = &(*l)->next)
  { if ( (*l)->attribute->name == a->name )
    { gripe(ERC_REDEFINED, "attribute", a->name);
      a->references++;			/* attempt to redefine attribute: */
      free_attribute(a);		/* first wins according to standard */

      return;
    }
  }

  n = sgml_calloc(1, sizeof(*n));

  n->attribute = a;
  a->references++;
  *l = n;
  set_element_properties(e, a);
}


static int
process_attlist_declaraction(dtd_parser *p, const ichar *decl)
{ dtd *dtd = p->dtd;
  dtd_symbol *eid[MAXATTELEM];
  int i, en;
  ichar buf[MAXDECL];
  const ichar *s;

					/* expand parameter entities */
  if ( !expand_pentities(p, decl, buf, sizeof(buf)) )
    return FALSE;
  decl = iskip_layout(dtd, buf);
  DEBUG(printf("Expanded to %s\n", decl));

  if ( !(decl=itake_el_or_model_element_list(dtd, decl, eid, &en)) )
    return FALSE;

					/* fetch attributes */
  while(*decl)
  { dtd_attr *at = sgml_calloc(1, sizeof(*at));

					/* name of attribute */
    if ( !(s = itake_name(dtd, decl, &at->name)) )
      return gripe(ERC_SYNTAX_ERROR, "Name expected", decl);
    decl = s;

					/* (name1|name2|...) type */
    if ( (s=isee_func(dtd, decl, CF_GRPO)) )
    { charfunc ngs = CF_NG;

      at->type = AT_NAMEOF;
      decl=s;

      for(;;)
      { dtd_symbol *nm;

	if ( !(s = itake_nmtoken(dtd, decl, &nm)) )
	  return gripe(ERC_SYNTAX_ERROR, "Name expected", decl);
	decl = s;
	add_name_list(&at->typeex.nameof, nm);
	if ( (s=isee_ngsep(dtd, decl, &ngs)) )
	{ decl = s;
	  continue;
	}
	if ( (s = isee_func(dtd, decl, CF_GRPC)) )
	{ decl=s;
	  decl = iskip_layout(dtd, decl);
	  break;
	}
	return gripe(ERC_SYNTAX_ERROR, "Illegal name-group", decl);
      }
    } else if ( (s=isee_identifier(dtd, decl, "cdata")) )
    { decl = s;
      at->type = AT_CDATA;
    } else if ( (s=isee_identifier(dtd, decl, "entity")) )
    { decl = s;
      at->type = AT_ENTITY;
    } else if ( (s=isee_identifier(dtd, decl, "entities")) )
    { decl = s;
      at->type = AT_ENTITIES;
      at->islist = TRUE;
    } else if ( (s=isee_identifier(dtd, decl, "id")) )
    { decl = s;
      at->type = AT_ID;
    } else if ( (s=isee_identifier(dtd, decl, "idref")) )
    { decl = s;
      at->type = AT_IDREF;
    } else if ( (s=isee_identifier(dtd, decl, "idrefs")) )
    { decl = s;
      at->type = AT_IDREFS;
      at->islist = TRUE;
    } else if ( (s=isee_identifier(dtd, decl, "name")) )
    { decl = s;
      at->type = AT_NAME;
    } else if ( (s=isee_identifier(dtd, decl, "names")) )
    { decl = s;
      at->type = AT_NAMES;
      at->islist = TRUE;
    } else if ( (s=isee_identifier(dtd, decl, "nmtoken")) )
    { decl = s;
      at->type = AT_NMTOKEN;
    } else if ( (s=isee_identifier(dtd, decl, "nmtokens")) )
    { decl = s;
      at->type = AT_NMTOKENS;
      at->islist = TRUE;
    } else if ( (s=isee_identifier(dtd, decl, "number")) )
    { decl = s;
      at->type = AT_NUMBER;
    } else if ( (s=isee_identifier(dtd, decl, "numbers")) )
    { decl = s;
      at->type = AT_NUMBERS;
      at->islist = TRUE;
    } else if ( (s=isee_identifier(dtd, decl, "nutoken")) )
    { decl = s;
      at->type = AT_NUTOKEN;
    } else if ( (s=isee_identifier(dtd, decl, "nutokens")) )
    { decl = s;
      at->type = AT_NUTOKENS;
      at->islist = TRUE;
    } else if ( (s=isee_identifier(dtd, decl, "notation")) )
    { dtd_symbol *ng[MAXNAMEGROUP];
      int ns;

      at->type = AT_NOTATION;
      decl=s;
      if ( (s=itake_namegroup(dtd, CF_OR, decl, ng, &ns)) )
      { decl = s;

	for(i=0; i<ns; i++)
	  add_name_list(&at->typeex.nameof, ng[i]);
      } else
	return gripe(ERC_SYNTAX_ERROR, "name-group expected", decl);
    } else
      return gripe(ERC_SYNTAX_ERROR, "Attribute-type expected", decl);

					/* Attribute Defaults */
    if ( (s=isee_identifier(dtd, decl, "#fixed")) )
    { decl = s;
      at->def = AT_FIXED;
    } else if ( (s=isee_identifier(dtd, decl, "#required")) )
    { decl = s;
      at->def = AT_REQUIRED;
    } else if ( (s=isee_identifier(dtd, decl, "#current")) )
    { decl = s;
      at->def = AT_CURRENT;
    } else if ( (s=isee_identifier(dtd, decl, "#conref")) )
    { decl = s;
      at->def = AT_CONREF;
    } else if ( (s=isee_identifier(dtd, decl, "#implied")) )
    { decl = s;
      at->def = AT_IMPLIED;
    } else				/* real default */
      at->def = AT_DEFAULT;

    if ( at->def == AT_DEFAULT || at->def == AT_FIXED )
    { ichar buf[MAXSTRINGLEN];
      const ichar *end;
      
      if ( !(end=itake_string(dtd, decl, buf, sizeof(buf))) )
	end=itake_nmtoken_chars(dtd, decl, buf, sizeof(buf));
      if ( !end )
	return gripe(ERC_SYNTAX_ERROR, "Bad attribute default", decl);

      switch(at->type)
      { case AT_CDATA:
	{ at->att_def.cdata = istrdup(buf);
	  break;
	}
	case AT_ENTITY:
	case AT_NOTATION:
	case AT_NAME:
	{ if ( !(s=itake_name(dtd, buf, &at->att_def.name)) || *s )
	    return gripe(ERC_DOMAIN, "name", decl);
	  break;
	}
	case AT_NMTOKEN:
	case AT_NAMEOF:
	{ if ( !(s=itake_nmtoken(dtd, buf, &at->att_def.name)) || *s )
	    return gripe(ERC_DOMAIN, "nmtoken", decl);
	  break;
	}
	case AT_NUTOKEN:
	{ if ( !(s=itake_nutoken(dtd, buf, &at->att_def.name)) || *s )
	    return gripe(ERC_DOMAIN, "nutoken", decl);
	  break;
	}
	case AT_NUMBER:
	{ if ( !(s=itake_number(dtd, buf, at)) || *s )
	     return gripe(ERC_DOMAIN, "number", decl);
	  break;
	}
	case AT_NAMES:
	case AT_ENTITIES:
	case AT_IDREFS:
	case AT_NMTOKENS:
	case AT_NUMBERS:
	case AT_NUTOKENS:
	{ at->att_def.list = istrdup(buf);
	  break;
	}
	default:
	  return gripe(ERC_REPRESENTATION, "No default for type");
      }

      decl = end;
    }

					/* add to list */
    for(i=0; i<en; i++)
    { dtd_element *e = def_element(dtd, eid[i]);

      add_attribute(dtd, e, at);
    }
  }

  return TRUE;
}

		 /*******************************
		 *    GENERIC TAG PROCESSING	*
		 *******************************/

typedef enum
{ IE_NORMAL,
  IE_INCLUDED,				/* is included */
  IE_EXCLUDED				/* is excluded */
} includetype;


static includetype
in_or_excluded(sgml_environment *env, dtd_element *e)
{ for(; env; env=env->parent)
  { if ( env->element->structure )
    { dtd_edef *def = env->element->structure;
      dtd_element_list *el;

      for(el=def->excluded; el; el=el->next)
      { if ( el->value == e )
	  return IE_EXCLUDED;
      }
      for(el=def->included; el; el=el->next)
      { if ( el->value == e )
	  return IE_INCLUDED;
      }
    }
  }

  return IE_NORMAL;
}


static int
complete(sgml_environment *env)
{ if ( env->element->structure &&
       !env->element->undefined &&
       env->element->structure->type != C_ANY )
  { dtd_edef *def = env->element->structure;

    if ( !same_state(def->final_state, env->state) )
      return FALSE;
  }

  return TRUE;
}


static void
validate_completeness(sgml_environment *env)
{ if ( !complete(env) )
  { char buf[256];

    sprintf(buf, "Incomplete <%s> element", env->element->name->name);

    gripe(ERC_VALIDATE, buf);		/* TBD: expected */
  }
}


static sgml_environment *
push_element(dtd_parser *p, dtd_element *e, int callback)
{ if ( e != CDATA_ELEMENT )
  { sgml_environment *env = sgml_calloc(1, sizeof(*env));

    emit_cdata(p, FALSE);

    env->element = e;
    env->state = make_state_engine(e);
    env->space_mode = (p->environments ? p->environments->space_mode
				       : p->dtd->space_mode);
    env->parent = p->environments;
    p->environments = env;

    if ( p->dtd->shorttag )
    { env->saved_waiting_for_net = p->waiting_for_net;

      if ( p->event_class == EV_SHORTTAG )
      { p->waiting_for_net = TRUE;
	env->wants_net = TRUE;
      } else
      { env->wants_net = FALSE;
	if ( e->structure && e->structure->omit_close == FALSE )
	  p->waiting_for_net = FALSE;
      }
    }

    if ( e->map )
      p->map = env->map = e->map;
    else if ( env->parent )
      p->map = env->map = env->parent->map;

    p->first = TRUE;
    if ( callback && p->on_begin_element )
    { sgml_attribute atts[MAXATTRIBUTES];
      int natts = 0;

      if ( !(p->flags & SGML_PARSER_NODEFS) )
	natts = add_default_attributes(p, e, natts, atts);

      (*p->on_begin_element)(p, e, natts, atts);
    }

    if ( e->structure )
    { if ( e->structure->type == C_CDATA ||
	   e->structure->type == C_RCDATA )
      { p->state = (e->structure->type == C_CDATA ? S_CDATA : S_RCDATA);
	p->cdata_state = p->state;
	p->etag = e->name->name;
	p->etaglen = istrlen(p->etag);
	sgml_cplocation(&p->startcdata, &p->location);
      } else
	p->cdata_state = S_PCDATA;
    }
  }

  return p->environments;
}


static void
free_environment(sgml_environment *env)
{
#ifdef XMLNS
  if ( env->xmlns )
    xmlns_free(env);
#endif

  sgml_free(env);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Pop the stack,  closing  all  environment   uptil  `to'.  The  close was
initiated by pushing the element `e'.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
pop_to(dtd_parser *p, sgml_environment *to, dtd_element *e0)
{ sgml_environment *env, *parent;
  
  for(env = p->environments; env != to; env=parent)
  { dtd_element *e = env->element;

    validate_completeness(env);
    parent = env->parent;
	
    if ( e->structure && !e->structure->omit_close )
      gripe(ERC_OMITTED_CLOSE, e->name->name);

    if ( e0 != CDATA_ELEMENT )
      emit_cdata(p, TRUE);

    p->first = FALSE;
    p->environments = env;
    if ( p->dtd->shorttag )
      p->waiting_for_net = env->saved_waiting_for_net;

    WITH_CLASS(p, EV_OMITTED,
	       if ( p->on_end_element )
	         (*p->on_end_element)(p, e));
    free_environment(env);
  }
  p->environments = to;
  p->map = to->map;
  
  return TRUE;
}


static void
allow_for(dtd_element *in, dtd_element *e)
{ dtd_edef *def = in->structure;
  dtd_model *g;

  if ( def->type == C_EMPTY )
  { def->type = C_PCDATA;
    def->content = sgml_calloc(1, sizeof(*def->content));
    def->content->type = MT_OR;
    def->content->cardinality = MC_REP;
  }
  assert(def->content->type == MT_OR);

  g = def->content->content.group;

  if ( e == CDATA_ELEMENT )
  { dtd_model *m;

    for(; g; g = g->next)
    { if ( g->type == MT_PCDATA )
	return;
    }
    m = sgml_calloc(1, sizeof(*m));
    m->type	   = MT_PCDATA;
    m->cardinality = MC_ONE;		/* ignored */
    add_submodel(def->content, m);
  } else
  { dtd_model *m;

    for(; g; g = g->next)
    { if ( g->type == MT_ELEMENT && g->content.element == e )
	return;
    }
    m = sgml_calloc(1, sizeof(*m));
    m->type	   = MT_ELEMENT;
    m->cardinality = MC_ONE;		/* ignored */
    m->content.element = e;
    add_submodel(def->content, m);
  }
}



static int
open_element(dtd_parser *p, dtd_element *e, int warn)
{ if ( !p->environments && p->enforce_outer_element )
  { dtd_element *f = p->enforce_outer_element->element;

    if ( f && f != e )
    { if ( !f->structure ||
	   !f->structure->omit_open )
	gripe(ERC_OMITTED_OPEN, f->name->name);

      WITH_CLASS(p, EV_OMITTED,
		 { open_element(p, f, TRUE);
		   if ( p->on_begin_element )
		   { sgml_attribute atts[MAXATTRIBUTES];
		     int natts = 0;
		     
		     if ( !(p->flags & SGML_PARSER_NODEFS) )
		       natts = add_default_attributes(p, f, natts, atts);

		     (*p->on_begin_element)(p, f, natts, atts);
		   }
		 });
    }
  }

					/* no DTD available yet */
  if ( !p->environments && !p->dtd->doctype && e != CDATA_ELEMENT )
  { const char *file;

    if ( (file=find_in_catalogue(CAT_DOCTYPE,
				 e->name->name, NULL, NULL,
				 p->dtd->dialect != DL_SGML)) )
    { dtd_parser *clone = clone_dtd_parser(p);

      gripe(ERC_NO_DOCTYPE, e->name->name, file);

      if ( load_dtd_from_file(clone, file) )
	p->dtd->doctype = istrdup(e->name->name);
      else
	gripe(ERC_EXISTENCE, "file", file);

      free_dtd_parser(clone);
    }
  }

  if ( p->environments )
  { sgml_environment *env = p->environments;

    if ( env->element->undefined )
    { allow_for(env->element, e);	/* <!ELEMENT x - - (model) +(y)> */
      push_element(p, e, FALSE);
      return TRUE;
    }

    if ( env->element->structure &&
	 env->element->structure->type == C_ANY )
    { if ( e != CDATA_ELEMENT && e->undefined )
	gripe(ERC_EXISTENCE, "Element", e->name->name);
      push_element(p, e, FALSE);
      return TRUE;
    }

    switch(in_or_excluded(env, e))
    { case IE_INCLUDED:
        push_element(p, e, FALSE);
	return TRUE;
      case IE_EXCLUDED:
	if ( warn )
	  gripe(ERC_NOT_ALLOWED, e->name->name);
	/*FALLTHROUGH*/
      case IE_NORMAL:
	for(; env; env=env->parent)
	{ dtd_state *new;
    
	  if ( (new = make_dtd_transition(env->state, e)) )
	  { env->state = new;
	    pop_to(p, env, e);
	    push_element(p, e, FALSE);
	    return TRUE;
	  } else
	  { dtd_element *oe[MAXOMITTED]; /* omitted open */
	    int olen;
	    int i;
    
	    if ( (olen=find_omitted_path(env->state, e, oe)) > 0 )
	    { pop_to(p, env, e);
	      WITH_CLASS(p, EV_OMITTED,
	      for(i=0; i<olen; i++)
	      { env->state = make_dtd_transition(env->state, oe[i]);
		env = push_element(p, oe[i], TRUE);
	      })
	      env->state = make_dtd_transition(env->state, e);
	      push_element(p, e, FALSE);
	      return TRUE;
	    }
	  }

	  if ( !env->element->structure ||
	       !env->element->structure->omit_close )
	    break;
	}
    }

    if ( warn )
    { if ( e == CDATA_ELEMENT )
	gripe(ERC_VALIDATE, "#PCDATA not allowed here");
      else if ( e->undefined )
	gripe(ERC_EXISTENCE, "Element", e->name->name);
      else
	gripe(ERC_NOT_ALLOWED, e->name->name);
    }
  }

  if ( warn )
  { push_element(p, e, FALSE);
    return TRUE;
  } else
    return FALSE;
}


static int
close_element(dtd_parser *p, dtd_element *e, int conref)
{ sgml_environment *env;

  for(env = p->environments; env; env=env->parent)
  { if ( env->element == e )		/* element is open */
    { sgml_environment *parent;

      for(env = p->environments; ; env=parent)
      {	dtd_element *ce	= env->element;

	if ( !(conref && env == p->environments) )
	  validate_completeness(env);
	parent = env->parent;
	
	p->first = FALSE;
	if ( p->on_end_element )
	  (*p->on_end_element)(p, env->element);
	free_environment(env);
	p->environments = parent;

	if ( ce == e )			/* closing current element */
	{ p->map = (parent ? parent->map : NULL);
	  return TRUE;
	} else				/* omited close */
	{ if ( ce->structure && !ce->structure->omit_close )
	    gripe(ERC_OMITTED_CLOSE, ce->name->name);
	}
      }
    }
  }

  return gripe(ERC_NOT_OPEN, e->name->name);
}


static int
close_current_element(dtd_parser *p)
{ if ( p->environments )
  { dtd_element *e = p->environments->element;
    
    emit_cdata(p, TRUE);
    return close_element(p, e, FALSE);
  }

  return gripe(ERC_SYNTAX_ERROR, "No element to close", "");
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
get_attribute_value()

Get the value for an attribute.  Once   I  thought  this was simple, but
Richard O'Keefe pointed to the complex   handling of white-space in SGML
attributes. Basically, if the attribute is quoted, we need:

	* If CDATA, map all blank to space characters, then expand
	  entities

	* If !CDATA expand all entities, canonise white space by
	  deleting leading and trailing space and squishing multiple
	  space characters to a single (lower for us) case.

This almost, but not completely matches the XML definition. This however
is so complex we will ignore it for now.

[Rewritten by Richard O'Keefe with these addional comments]
Reads a value, the  attribute  name   and  value  indicator  having been
processed already. It calls itake_string() to   read  quoted values, and
itake_unquoted() to read unquoted values.

itake_string(dtd, in, buf, size)
	- skips layout INCLUDING comments,
	- returns NULL if the next character is not ' or ",
	- copies characters from in to buf until a matching ' or " is found,
	- adds a terminating \0,
	- skips more layout INCLUDING comments, and
	- returns the new input position.
It is quite wrong to skip leading comments here.  In the tag

    <foo bar = --ugh-- zoo>

the characters "--ugh--" *are the value*.  They are not a comment.
Comments are not in fact allowed inside tags, unfortunately.
This tag is equivalent to

    <foo bar="--ugh--" something="zoo">

where something is an attribute that has zoo as one of its enumerals.

Because itake_string() is called in many other places, this bug has
not yet been fixed.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static ichar const *
get_attribute_value(dtd_parser *p, ichar const *decl, sgml_attribute *att)
{ ichar tmp[MAXSTRINGLEN];
  ichar *buf = tmp;
  ochar cdata[MAXSTRINGLEN];
  ichar const *s;
  ichar *d;
  ichar c;
  dtd *dtd = p->dtd;
  ichar const *end;

  enum
  { DIG_FIRST = 8,		/* any token start with digit? */
    NAM_FIRST = 4,		/* any token start with non-digit name char? */
    NAM_LATER = 2,		/* any token have non-digit name char later? */
    ANY_OTHER = 1,		/* any token have illegal character? */
    YET_EMPTY = 0
  }
  token = YET_EMPTY;

  end = itake_string(dtd, decl, tmp, sizeof tmp);
  if (end != NULL)
  { if (att->definition->type == AT_CDATA)
    { int hasent = FALSE;
      ichar const ero = dtd->charfunc->func[CF_ERO];	/* & */
      ichar *q;

      for (d = q = tmp; *d; *q++ = *d++)
      { if ( d[0] == CR && d[1] == LF )
	  d++;
	if (HasClass(dtd, *d, CH_BLANK))
	{ *d = ' ';		/* map all blanks to spaces */
	} else if (*d == ero)
	{ hasent = TRUE;	/* notice char/entity references */
	}
#ifdef UTF8
	  else if ( p->utf8_decode && ISUTF8_MB(*d) )
	{ hasent = TRUE;
	}
#endif
      }
      *q = '\0';
      if (hasent)
      { expand_entities(p, tmp, cdata, MAXSTRINGLEN);
	buf = (ichar *) cdata;
      }
    } else
    { ichar *d;

      expand_entities(p, tmp, cdata, MAXSTRINGLEN);
      buf = (ichar *) cdata;

      /* canonicalise blanks */
      s = buf;
      while ((c = *s++) != '\0' && HasClass(dtd, c, CH_BLANK))
      { }
      d = buf;
      while (c != '\0')
      { token |= HasClass(dtd, c, CH_DIGIT) ? DIG_FIRST
	  : HasClass(dtd, c, CH_NAME) ? NAM_FIRST : /* oops! */ ANY_OTHER;
	if (d != buf)
	  *d++ = ' ';
	if (dtd->case_sensitive)
	{ *d++ = c;
	  while ((c = *s++) != '\0' && !HasClass(dtd, c, CH_BLANK))
	  { token |= HasClass(dtd, c, CH_DIGIT) ? 0
	      : HasClass(dtd, c, CH_NAME) ? NAM_LATER : /* oops! */ ANY_OTHER;
	    *d++ = c;
	  }
	} else
	{ *d++ = tolower(c);
	  while ((c = *s++) != '\0' && !HasClass(dtd, c, CH_BLANK))
	  { token |= HasClass(dtd, c, CH_DIGIT) ? 0
	      : HasClass(dtd, c, CH_NAME) ? NAM_LATER : /* oops! */ ANY_OTHER;
	    *d++ = tolower(c);
	  }
	}
	while (c != '\0' && HasClass(dtd, c, CH_BLANK))
	  c = *s++;
      }
      *d = '\0';
    }
  } else
  { end = itake_unquoted(dtd, decl, tmp, sizeof tmp);
    if (end == NULL)
      return NULL;

    s = buf;
    c = *s++;
    if (c != '\0')
    { token |= HasClass(dtd, c, CH_DIGIT) ? DIG_FIRST
	: HasClass(dtd, c, CH_NAME) ? NAM_FIRST : /* oops! */ ANY_OTHER;
      while ((c = *s++) != 0)
      { token |= HasClass(dtd, c, CH_DIGIT) ? 0
	  : HasClass(dtd, c, CH_NAME) ? NAM_LATER : /* oops! */ ANY_OTHER;
      }
    }
    if ( token == YET_EMPTY || (token & ANY_OTHER) != 0)
      gripe(ERC_SYNTAX_WARNING, "Attribute value requires quotes", buf);

    if (!dtd->case_sensitive && att->definition->type != AT_CDATA)
      istrlower(buf);
  }

  att->value.cdata = NULL;
  att->value.text = NULL;
  att->value.number = 0;
  att->flags = 0;

  switch (att->definition->type)
  { case AT_NUMBER:		/* number */
      if (token != DIG_FIRST)
      { gripe(ERC_SYNTAX_WARNING, "NUMBER expected", decl);
      } else if (dtd->number_mode == NU_INTEGER)
      { (void) istrtol(buf, &att->value.number);
      } else
      { att->value.text = istrdup(buf);
      }
      return end;
    case AT_CDATA:		/* CDATA attribute */
      att->value.cdata = ostrdup((ochar *) buf);
      return end;
    case AT_ID:		/* identifier */
    case AT_IDREF:		/* identifier reference */
    case AT_NAME:		/* name token */
    case AT_NOTATION:		/* notation-name */
      if (token == YET_EMPTY || (token & (DIG_FIRST | ANY_OTHER)) != 0)
	gripe(ERC_SYNTAX_WARNING, "NAME expected", decl);
      break;
    case AT_NAMEOF:		/* one of these names */
    case AT_NMTOKEN:		/* name-token */
      if (token == YET_EMPTY || (token & ANY_OTHER) != 0)
	gripe(ERC_SYNTAX_WARNING, "NMTOKEN expected", decl);
      if ( att->definition->type == AT_NAMEOF )
      { dtd_name_list *nl;

	for(nl=att->definition->typeex.nameof; nl; nl = nl->next)
	{ if ( istreq(nl->value->name, buf) )
	    goto passed;
	}
	gripe(ERC_SYNTAX_WARNING, "unexpected value", decl);
      }
      break;
    case AT_NUTOKEN:		/* number token */
      if ((token & (NAM_FIRST | ANY_OTHER)) != 0)
	gripe(ERC_SYNTAX_WARNING, "NUTOKEN expected", decl);
      break;
    case AT_ENTITY:		/* entity-name */
      if (token == YET_EMPTY || (token & (DIG_FIRST | ANY_OTHER)) != 0)
	gripe(ERC_SYNTAX_WARNING, "entity NAME expected", decl);
      break;
    case AT_NAMES:		/* list of names */
    case AT_IDREFS:		/* list of identifier references */
      if (token == YET_EMPTY || (token & (DIG_FIRST | ANY_OTHER)) != 0)
	gripe(ERC_SYNTAX_WARNING, "NAMES expected", decl);
      break;
    case AT_ENTITIES:		/* entity-name list */
      if (token == YET_EMPTY || (token & (DIG_FIRST | ANY_OTHER)) != 0)
	gripe(ERC_SYNTAX_WARNING, "entity NAMES expected", decl);
      break;
    case AT_NMTOKENS:		/* name-token list */
      if (token == YET_EMPTY || (token & ANY_OTHER) != 0)
	gripe(ERC_SYNTAX_WARNING, "NMTOKENS expected", decl);
      break;
    case AT_NUMBERS:		/* number list */
      if (token != DIG_FIRST)
	gripe(ERC_SYNTAX_WARNING, "NUMBERS expected", decl);
      break;
    case AT_NUTOKENS:
      if ((token & (NAM_FIRST | ANY_OTHER)) != 0)
	gripe(ERC_SYNTAX_WARNING, "NUTOKENS expected", decl);
      break;
    default:
      assert(0);
      return NULL;
  }

passed:
  att->value.text = istrdup(buf);	/* TBD: more validation */
  return end;
}


static const ichar *
process_attributes(dtd_parser *p, dtd_element *e, const ichar *decl,
		   sgml_attribute *atts, int *argc)
{ int attn = 0;
  dtd *dtd = p->dtd;

  decl = iskip_layout(dtd, decl);
  while(decl && *decl)
  { dtd_symbol *nm;
    const ichar *s;

    if ( (s=itake_nmtoken(dtd, decl, &nm)) )
    { decl = s;

      if ( (s=isee_func(dtd, decl, CF_VI)) ) /* name= */
      { dtd_attr *a;

	if ( !HasClass(dtd, nm->name[0], CH_NMSTART) )
	  gripe(ERC_SYNTAX_WARNING,
		"Illegal start of attribute-name", decl);

	decl = s;
	if ( !(a=find_attribute(e, nm)) )
	{ a = sgml_calloc(1, sizeof(*a));

	  a->name = nm;
	  a->type = AT_CDATA;
	  a->def  = AT_IMPLIED;
	  add_attribute(dtd, e, a);

	  if ( !e->undefined &&
	       !(dtd->dialect != DL_SGML &&
		 (istreq("xmlns", nm->name) ||
		  istrprefix("xmlns:", nm->name))) )
	    gripe(ERC_NO_ATTRIBUTE, e->name->name, nm->name);
	}
	atts[attn].definition = a;
	if ( (decl=get_attribute_value(p, decl, atts+attn)) )
	{ attn++;
	  continue;
	}
      } else if ( e->structure )
      { dtd_attr_list *al;		/* value shorthand */

	for(al=e->attributes; al; al=al->next)
	{ dtd_attr *a = al->attribute;

	  if ( a->type == AT_NAMEOF || a->type == AT_NOTATION )
	  { dtd_name_list *nl;

	    for(nl=a->typeex.nameof; nl; nl = nl->next)
	    { if ( nl->value == nm )
	      { if ( dtd->dialect != DL_SGML )
		  gripe(ERC_SYNTAX_WARNING,
			"Value short-hand in XML mode", decl);
		atts[attn].definition   = a;
		atts[attn].value.cdata  = NULL;
		atts[attn].value.number = 0;
		atts[attn].value.text   = istrdup(nm->name);
		attn++;
		goto next;
	      }
	    }
	  }
	}
	gripe(ERC_NO_ATTRIBUTE_VALUE, e->name->name, nm->name);
	decl = s;
      } else
      { gripe(ERC_SYNTAX_ERROR, "Bad attribute", decl);
	decl = s;
      }
    } else
    { *argc = attn;
      return decl;
    }
    
  next:
    ;
  }

  *argc = attn;
  return decl;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
sgml_add_default_attributes()

This function adds attributes for omitted  default and fixed attributes.
These attributes are added to  the  end   of  the  attribute  list. This
function returns the new  number  of   attributes.  The  `atts' array is
assumed   to   be   MAXATTRIBUTES    long,     normally    passed   from
process_begin_element.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
add_default_attributes(dtd_parser *p, dtd_element *e,
		       int natts, sgml_attribute *atts)
{ dtd_attr_list *al;

  if ( e == CDATA_ELEMENT )
    return natts;

  for(al=e->attributes; al; al=al->next)
  { dtd_attr *a = al->attribute;

    switch(a->def)
    { case AT_REQUIRED:			/* TBD: check if present */
      case AT_CURRENT:			/* TBD: register in DTD and reuse */
      case AT_CONREF:
      case AT_IMPLIED:
	goto next;
      case AT_FIXED:
      case AT_DEFAULT:
      { int i;
	sgml_attribute *ap;

	for(i=0, ap=atts; i<natts; i++, ap++)
	{ if ( ap->definition == a )
	    goto next;
	}

        ap->definition   = a;
	ap->value.cdata  = NULL;
	ap->value.text   = NULL;
	ap->value.number = 0;
	ap->flags        = SGML_AT_DEFAULT;

	switch(a->type)
	{ case AT_CDATA:
	    ap->value.cdata = a->att_def.cdata;
	    break;
	  case AT_NUMBER:
	    if ( p->dtd->number_mode == NU_TOKEN )
	      ap->value.text = (ichar *)a->att_def.name->name;
	    else
	      ap->value.number = a->att_def.number;
	    break;
	  default:
	    if ( a->islist )
	      ap->value.text = a->att_def.list;
	    else
	      ap->value.text = (ichar *)a->att_def.name->name;
	}

	natts++;
      }
    }
  next:;
  }

  return natts;
}


static void
free_attribute_values(int argc, sgml_attribute *argv)
{ int i;

  for(i=0; i<argc; i++, argv++)
  { if ( (argv->flags & SGML_AT_DEFAULT) )
      continue;				/* shared with the DTD */

    if ( argv->value.cdata )
      sgml_free(argv->value.cdata);
    if ( argv->value.text )
      sgml_free(argv->value.text);
  }
}


static int
process_begin_element(dtd_parser *p, const ichar *decl)
{ dtd *dtd = p->dtd;
  dtd_symbol *id;
  const ichar *s;

  if ( (s=itake_name(dtd, decl, &id)) )
  { sgml_attribute atts[MAXATTRIBUTES];
    int natts;
    dtd_element *e = find_element(dtd, id);
    int empty = FALSE;
    int conref = FALSE;

    if ( !e->structure )
    { dtd_edef *def;
      e->undefined = TRUE;
      def_element(dtd, id);
      def = e->structure;
      def->type = C_EMPTY;
    }

    open_element(p, e, TRUE);

    decl=s;
    if ( (s=process_attributes(p, e, decl, atts, &natts)) )
      decl=s;

    if ( dtd->dialect != DL_SGML )
    { if ( (s=isee_func(dtd, decl, CF_ETAGO2)) )
      { empty = TRUE;			/* XML <tag/> */
	decl = s;
      }
#ifdef XMLNS
      if ( dtd->dialect == DL_XMLNS )
	update_xmlns(p, e, natts, atts);
#endif
      if ( dtd->dialect != DL_SGML )
	update_space_mode(p, e, natts, atts);
    } else
    { int i;

      for(i=0; i<natts; i++)
      { if ( atts[i].definition->def == AT_CONREF )
	{ empty = TRUE;
	  conref = TRUE;
	}
      }
    }
    if ( *decl )
      gripe(ERC_SYNTAX_ERROR, "Bad attribute list", decl);

    if ( !(p->flags & SGML_PARSER_NODEFS) )
      natts = add_default_attributes(p, e, natts, atts);

    if ( empty ||
	 (dtd->dialect == DL_SGML &&
	  e->structure &&
	  e->structure->type == C_EMPTY &&
	  !e->undefined) )
      p->empty_element = e;
    else
      p->empty_element = NULL;

    if ( p->on_begin_element )
      (*p->on_begin_element)(p, e, natts, atts);

    free_attribute_values(natts, atts);

    if ( p->empty_element )
    { p->empty_element = NULL;
      close_element(p, e, conref);
      if ( conref )	/* might be S_CDATA due to declared content */
	p->cdata_state = p->state = S_PCDATA;
    }

    return TRUE;
  }

  return gripe(ERC_SYNTAX_ERROR, "Bad open-element tag", decl);
}


static int
process_end_element(dtd_parser *p, const ichar *decl)
{ dtd *dtd = p->dtd;
  dtd_symbol *id;
  const ichar *s;
  
  emit_cdata(p, TRUE);
  if ( (s=itake_name(dtd, decl, &id)) && *s == '\0' )
    return close_element(p, find_element(dtd, id), FALSE);

  if ( p->dtd->shorttag && *decl == '\0' ) /* </>: close current element */
    return close_current_element(p);

  return gripe(ERC_SYNTAX_ERROR, "Bad close-element tag", decl);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
process_net(dtd_parser *p)
    We've seen a / of a shorttag element.  Close this one.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
process_net(dtd_parser *p)
{ sgml_environment *env;

  prepare_cdata(p);
  for(env = p->environments; env; env=env->parent)
  { if ( env->wants_net )
    { sgml_environment *parent;

      pop_to(p, env, NULL);		/* close parents */
      validate_completeness(env);
      parent = env->parent;

      emit_cdata(p, TRUE);
      p->first = FALSE;

      if ( p->on_end_element )
      { WITH_CLASS(p, EV_SHORTTAG,
		   (*p->on_end_element)(p, env->element));
      }

      free_environment(env);
      p->environments = parent;
      p->map = (parent ? parent->map : NULL);

      return TRUE;
    }
  }

  return FALSE;
}


static int				/* <!DOCTYPE ...> */
process_doctype(dtd_parser *p, const ichar *decl, const ichar *decl0)
{ dtd *dtd = p->dtd;
  dtd_symbol *id;
  const ichar *s;
  dtd_entity *et = NULL;

  if ( !(s=itake_name(dtd, decl, &id)) )
    return gripe(ERC_SYNTAX_ERROR, "Name expected", decl);
  decl = s;

  if ( (s=isee_identifier(dtd, decl, "system")) )
  { et = sgml_calloc(1, sizeof(*et));
    et->type = ET_SYSTEM;
    decl = s;
  } else if ( (s=isee_identifier(dtd, decl, "public")) )
  { et = sgml_calloc(1, sizeof(*et));
    et->type = ET_PUBLIC;
    decl = s;
  } else if ( isee_func(dtd, decl, CF_DSO) )
    goto local;

  if ( et )
  { et->name = id;
    et->catalog_location = CAT_DOCTYPE;
    if ( !(s=process_entity_value_declaration(p, decl, et)) )
      return FALSE;
    decl = s;
  }

  if ( !dtd->doctype )			/* i.e. anonymous DTD */
  { const char *file;
    dtd_parser *clone;

    dtd->doctype = istrdup(id->name);	/* Fill it */
    if ( et )
      file = entity_file(dtd, et);
    else
      file = find_in_catalogue(CAT_DOCTYPE,
			       dtd->doctype, NULL, NULL,
			       dtd->dialect != DL_SGML);

    if ( !file )
    { gripe(ERC_EXISTENCE, "DTD", dtd->doctype);
    } else
    { clone = clone_dtd_parser(p);
      if ( !load_dtd_from_file(clone, file) )
	gripe(ERC_EXISTENCE, "file", file);
      free_dtd_parser(clone);
    }
  }

  if ( et )
    free_entity_list(et);

local:
  if ( (s=isee_func(dtd, decl, CF_DSO)) ) /* [...] */
  { int grouplevel = 1;
    data_mode oldmode  = p->dmode;
    dtdstate  oldstate = p->state;
    locbuf oldloc;
    const ichar *q;
    icharbuf *saved_ibuf = p->buffer;

    push_location(p, &oldloc);
					/* try to find start-location. */
					/* fails if there is comment before */
					/* the []! */
    sgml_cplocation(&p->location, &p->startloc);
    inc_location(&p->location, '<');
    for(q=decl0; q < s; q++)
      inc_location(&p->location, *q);
    p->dmode = DM_DTD;
    p->state = S_PCDATA;
    p->buffer = new_icharbuf();

    for( ; *s; s++ )
    { if ( isee_func(dtd, s, CF_LIT) ||	/* skip quoted strings */
	   isee_func(dtd, s, CF_LITA) )
      { ichar q = *s;

	putchar_dtd_parser(p, *s++);	/* pass open quote */

	for( ; *s && *s != q; s++ )
	  putchar_dtd_parser(p, *s);

	if ( *s == q )			/* pass closing quote */
	  putchar_dtd_parser(p, *s);
	continue;
      }

      if ( isee_func(dtd, s, CF_DSO) )
	grouplevel++;
      else if ( isee_func(dtd, s, CF_DSC) && --grouplevel == 0 )
	break;
      putchar_dtd_parser(p, *s);
    }
    p->dtd->implicit = FALSE;

    p->state    = oldstate;
    p->dmode    = oldmode;
    free_icharbuf(p->buffer);
    p->buffer = saved_ibuf;
    pop_location(p, &oldloc);
  }

  p->enforce_outer_element = id;	/* make this the outer element */

  return TRUE;
}


static void
init_decoding(dtd_parser *p)
{
#ifdef UTF8
  int decode;
  dtd *dtd = p->dtd;

  if ( dtd->encoding == ENC_UTF8 &&
       p->encoding   == ENC_ISO_LATIN1 )
    decode = TRUE;
  else
    decode = FALSE;

  if ( p->utf8_decode != decode )
  { DEBUG(fprintf(stderr, "%s UTF-8 decoding on %p\n",
		  decode ? "Enable" : "Disable",
		  p));

    p->utf8_decode = decode;
  }
#endif
}


static void
set_encoding(dtd_parser *p, const ichar *enc)
{ dtd *dtd = p->dtd;

  if ( istrcaseeq(enc, "iso-8859-1") )
  { dtd->encoding = ENC_ISO_LATIN1;
  } else if ( istrcaseeq(enc, "utf-8") )
  { dtd->encoding = ENC_UTF8;
  } else
    gripe(ERC_EXISTENCE, "character encoding", enc);

  init_decoding(p);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Process <? ... ?>

Should deal with character encoding for XML documents.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
process_pi(dtd_parser *p, const ichar *decl)
{ const ichar *s;
  dtd *dtd = p->dtd;

  if ( (s=isee_identifier(dtd, decl, "xml")) ) /* <?xml version="1.0"?> */
  { decl = s;

    while(*decl)
    { dtd_symbol *nm;

      if ( (s=itake_name(dtd, decl, &nm)) &&
	   (s=isee_func(dtd, s, CF_VI)) )
      { ichar buf[MAXSTRINGLEN];
	const ichar *end;

	if ( !(end=itake_string(dtd, s, buf, sizeof(buf))) )
	  end=itake_nmtoken_chars(dtd, s, buf, sizeof(buf));

	if ( end )
	{ decl = end;

	  if ( istrcaseeq(nm->name, "encoding") )
	    set_encoding(p, buf);

	  /* fprintf(stderr, "XML %s = %s\n", nm->name, buf); */

	  continue;
	}
      }

      gripe(ERC_SYNTAX_ERROR, "Illegal XML parameter", decl);
      break;
    }

    switch(dtd->dialect)
    { case DL_SGML:
	set_dialect_dtd(dtd, DL_XML);
        break;
      case DL_XML:
      case DL_XMLNS:
	break;
    }
    return TRUE;
  }

  if ( p->on_pi )
    (*p->on_pi)(p, decl);

  return FALSE;				/* Warn? */
}


static int
process_sgml_declaration(dtd_parser *p, const ichar *decl)
{ return gripe(ERC_SYNTAX_WARNING, "Ignored <!SGML ...> declaration", NULL);
}


static int
process_declaration(dtd_parser *p, const ichar *decl)
{ const ichar *s;
  dtd *dtd = p->dtd;

  if ( p->dmode != DM_DTD )
  { if ( (s=isee_func(dtd, decl, CF_ETAGO2)) ) /* </ ... > */
    { return process_end_element(p, s);
    } else if ( HasClass(dtd, *decl, CH_NAME) ) /* <letter */
    { return process_begin_element(p, decl);
    }
  }

  if ( (s=isee_func(dtd, decl, CF_MDO2)) ) /* <! ... >*/
  { decl = s;

    if ( p->on_decl )
      (*p->on_decl)(p, decl);

    if ( (s = isee_identifier(dtd, decl, "entity")) )
      process_entity_declaration(p, s);
    else if ( (s = isee_identifier(dtd, decl, "element")) )
      process_element_declaraction(p, s);
    else if ( (s = isee_identifier(dtd, decl, "attlist")) )
      process_attlist_declaraction(p, s);
    else if ( (s = isee_identifier(dtd, decl, "notation")) )
      process_notation_declaration(p, s);
    else if ( (s = isee_identifier(dtd, decl, "shortref")) )
      process_shortref_declaration(p, s);
    else if ( (s = isee_identifier(dtd, decl, "usemap")) )
      process_usemap_declaration(p, s);
    else if ( (s = isee_identifier(dtd, decl, "sgml")) )
      process_sgml_declaration(p, s);
    else if ( (s = isee_identifier(dtd, decl, "doctype")) )
    { if ( p->dmode != DM_DTD )
	process_doctype(p, s, decl-1);
    } else
    { s = iskip_layout(dtd, decl);
  
      if ( *s )
	gripe(ERC_SYNTAX_ERROR, "Invalid declaration", s);
    }

    return TRUE;
  }

  return gripe(ERC_SYNTAX_ERROR, "Invalid declaration", decl);
}

		 /*******************************
		 *	  STREAM BINDING	*
		 *******************************/

static dtd_parser *current_parser;	/* For gripes */

void
set_src_dtd_parser(dtd_parser *p, input_type type, const char *name)
{ p->location.type    = type;
  p->location.name    = name;
  p->location.line    = 1;
  p->location.linepos = 0;
  p->location.charpos = 0;
}


void
set_mode_dtd_parser(dtd_parser *p, data_mode m)
{ p->dmode = m;				/* DM_DTD or DM_DATA */
  p->state = S_PCDATA;
  p->blank_cdata = TRUE;
}


dtd_parser *
new_dtd_parser(dtd *dtd)
{ dtd_parser *p = sgml_calloc(1, sizeof(*p));
  
  if ( !dtd )
    dtd = new_dtd(NULL);
  dtd->references++;

  p->magic       = SGML_PARSER_MAGIC;
  p->dtd	 = dtd;
  p->state	 = S_PCDATA;
  p->mark_state	 = MS_INCLUDE;
  p->dmode       = DM_DTD;
  p->encoding	 = ENC_ISO_LATIN1;
  p->buffer	 = new_icharbuf();
  p->cdata	 = new_ocharbuf();
  p->event_class = EV_EXPLICIT;
  set_src_dtd_parser(p, IN_NONE, NULL);

  return p;
}


static dtd_parser *
clone_dtd_parser(dtd_parser *p)
{ dtd_parser *clone = sgml_calloc(1, sizeof(*p));
  
  *clone = *p;
  clone->dtd->references++;
  clone->environments =	NULL;
  clone->marked	      =	NULL;
  clone->etag	      =	NULL;
  clone->grouplevel   =	0;
  clone->state	      =	S_PCDATA;
  clone->mark_state   =	MS_INCLUDE;
  clone->dmode	      =	DM_DTD;
  clone->buffer	      =	new_icharbuf();
  clone->cdata	      =	new_ocharbuf();

  return clone;
}


void
free_dtd_parser(dtd_parser *p)
{ free_icharbuf(p->buffer);
  free_ocharbuf(p->cdata);

  free_dtd(p->dtd);

  sgml_free(p);
}


static int
process_chars(dtd_parser *p, input_type in, const ichar *name, const ichar *s)
{ locbuf old;
  
  push_location(p, &old);
  set_src_dtd_parser(p, in, (char *)name);
  empty_icharbuf(p->buffer);		/* dubious */
  for(; *s; s++)
    putchar_dtd_parser(p, *s);
  pop_location(p, &old);

  return TRUE;
}


static int
process_include(dtd_parser *p, const ichar *entity_name)
{ dtd_symbol *id;
  dtd_entity *pe;
  dtd *dtd = p->dtd;

  if ( (id=dtd_find_entity_symbol(dtd, entity_name)) &&
       (pe=find_pentity(p->dtd, id)) )
  { const char *file;

    if ( (file = entity_file(dtd, pe)) )
      return sgml_process_file(p, file, SGML_SUB_DOCUMENT);
    else
    { const ichar *text = entity_value(p, pe, NULL);

      if ( !text )
	return gripe(ERC_NO_VALUE, pe->name->name);

      return process_chars(p, IN_ENTITY, entity_name, text);
    }
  }
  
  return gripe(ERC_EXISTENCE, "parameter entity", entity_name);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Process <![ KEYWORD [

Switches ->mark_state according to KEYWORD. Processes the rest in normal
S_PCDATA style, which pops the mark-stack on seeing ]]>

For the purpose of <!DOCTYPE spec [additions]> we switch to S_GROUP if
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
process_marked_section(dtd_parser *p)
{ ichar buf[MAXDECL];
  dtd *dtd = p->dtd;
  const ichar *decl = p->buffer->data;
  const ichar *s;

  if ( (decl=isee_func(dtd, decl, CF_MDO2)) && /* ! */
       (decl=isee_func(dtd, decl, CF_DSO)) && /* [ */
       expand_pentities(p, decl, buf, sizeof(buf)) )
  { dtd_symbol *kwd;

    decl = buf;
    if ( (s=itake_name(dtd, decl, &kwd)) &&
	 isee_func(dtd, s, CF_DSO) )	/* [ */
    { dtd_marked *m = sgml_calloc(1, sizeof(*m));

      m->keyword = kwd;			/* push on the stack */
      m->parent = p->marked;
      p->marked = m;

      if ( istrcaseeq(kwd->name, "IGNORE") )
	m->type = MS_IGNORE;
      else if ( istrcaseeq(kwd->name, "INCLUDE") )
	m->type = MS_INCLUDE;
      else if ( istrcaseeq(kwd->name, "TEMP") )
	m->type = MS_INCLUDE;
      else if ( istrcaseeq(kwd->name, "CDATA") )
	m->type = MS_CDATA;
      else if ( istrcaseeq(kwd->name, "RCDATA") )
	m->type = MS_RCDATA;
      else
	m->type = MS_INCLUDE;		/* default */

      empty_icharbuf(p->buffer);
      if ( m->type == MS_CDATA )
	p->state = S_MSCDATA;
      else
	p->state = S_PCDATA;
      if ( p->mark_state != MS_IGNORE )
	p->mark_state = m->type;
    }
  } else
  { decl = p->buffer->data;

    if ( (decl=isee_func(dtd, decl, CF_MDO2)) && /* ! */
	 !isee_func(dtd, decl, CF_DSO) ) /* [ */
    { p->state = S_GROUP;
      p->grouplevel = 1;
    }
  }
}


static void
pop_marked_section(dtd_parser *p)
{ dtd_marked *m = p->marked;

  if ( m )
  { p->marked = m->parent;
    sgml_free(m);
    p->mark_state = (p->marked ? p->marked->type : MS_INCLUDE);
  }
} 


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Update the space-mode for the current element.  The space mode defines
how spaces are handled in the CDATA output.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static dtd_space_mode
istr_to_space_mode(const ichar *val)
{ if ( istreq(val, "default") )
    return SP_DEFAULT;
  if ( istreq(val, "preserve") )
    return SP_PRESERVE;
  if ( istreq(val, "sgml") )
    return SP_SGML;
  if ( istreq(val, "remove") )
    return SP_REMOVE;

  return SP_INHERIT;			/* interpret as error */
}


static void
update_space_mode(dtd_parser *p, dtd_element *e,
		  int natts, sgml_attribute *atts)
{ for( ; natts-- > 0; atts++ )
  { const ichar *name = atts->definition->name->name;

    if ( istreq(name, "xml:space") && atts->definition->type == AT_CDATA )
    { dtd_space_mode m = istr_to_space_mode(atts->value.cdata);

      if ( m != SP_INHERIT )
	p->environments->space_mode = m;
      else
	gripe(ERC_EXISTENCE, "xml:space-mode", atts->value.cdata);

      return;
    }
  }

  if ( e->space_mode != SP_INHERIT )
    p->environments->space_mode = e->space_mode;
}


static void
empty_cdata(dtd_parser *p)
{ if ( p->dmode == DM_DATA )
  { empty_ocharbuf(p->cdata);
    p->blank_cdata = TRUE;
    p->cdata_must_be_empty = FALSE;
  }
}


static int
emit_cdata(dtd_parser *p, int last)
{ dtd *dtd = p->dtd;
  ichar *s, *data = p->cdata->data;
  locbuf locsafe;
  
  if ( p->cdata->size == 0 )
    return TRUE;			/* empty or done */

  push_location(p, &locsafe);
  sgml_cplocation(&p->location, &p->startloc); 	/* start of markup */
  sgml_cplocation(&p->startloc, &p->startcdata); 	/* real start of CDATA */

  if ( p->environments )
  { switch(p->environments->space_mode)
    { case SP_SGML:
      case SP_DEFAULT:
	if ( p->first )
	{ if ( HasClass(dtd, *data, CH_RE) )
	  { inc_location(&p->startloc, *data);
	    data++;
	    p->cdata->size--;
	  }
	  if ( HasClass(dtd, *data, CH_RS) )
	  { inc_location(&p->startloc, *data);
	    data++;
	    p->cdata->size--;
	  }
	}
	if ( last )
	{ ichar *e = data + p->cdata->size;

	  if ( e > data && HasClass(dtd, e[-1], CH_RS) )
	  { dec_location(&p->location, e[-1]);
	    *--e = '\0';
	    p->cdata->size--;
	  }
	  if ( e>data && HasClass(dtd, e[-1], CH_RE) )
	  { dec_location(&p->location, e[-1]);
	    *--e = '\0';
	    p->cdata->size--;
	  }
	}
	if ( p->environments->space_mode == SP_DEFAULT )
	{ ichar *o = data;
  
	  for(s=data; *s; s++)
	  { if ( HasClass(dtd, *s, CH_BLANK) )
	    { while(s[1] && HasClass(dtd, s[1], CH_BLANK))
		s++;
	      *o++ = ' ';
	      continue;
	    }
	    *o++ = *s;
	  }
	  *o = '\0';
	  p->cdata->size = o-data;
	}
	break;
      case SP_REMOVE:
      { ichar *o = data;
	ichar *end = data;

	for(s=data; *s && HasClass(dtd, *s, CH_BLANK); )
	  inc_location(&p->startloc, *s++);

	if ( *s )
	{ for(; *s; s++)
	  { if ( HasClass(dtd, *s, CH_BLANK) )
	    { while(s[1] && HasClass(dtd, s[1], CH_BLANK))
		s++;
	      *o++ = ' ';
	      continue;
	    }
	    *o++ = *s;
	    end = o;
	  }
	}
					/* TBD: adjust end */
	*end = '\0';
	p->cdata->size = end-data;
	break;
      }
      case SP_PRESERVE:
	break;
      case SP_INHERIT:
	assert(0);
	return FALSE;
    }
  }

  if ( p->cdata->size == 0 )
  { pop_location(p, &locsafe);
    return TRUE;
  }

  assert(p->cdata->size > 0);
  if ( !p->blank_cdata )
  { if ( p->cdata_must_be_empty )
    { terminate_ocharbuf(p->cdata);
      gripe(ERC_NOT_ALLOWED_PCDATA, p->cdata->data);
    }
    if ( p->on_data )
      (*p->on_data)(p, EC_CDATA, p->cdata->size, data);
  } else if ( p->environments )
  { sgml_environment *env = p->environments;
    dtd_state *new;
    
				/* If an element is not in the DTD we must */
				/* assume mixed content and emit spaces */

    if ( (new=make_dtd_transition(env->state, CDATA_ELEMENT)) )
    { env->state = new;
      if ( p->on_data )
	(*p->on_data)(p, EC_CDATA, p->cdata->size, data);
    } else if ( env->element->undefined &&
		p->environments->space_mode == SP_PRESERVE )
    { if ( p->on_data )
	(*p->on_data)(p, EC_CDATA, p->cdata->size, data);
    }
  }
  
  pop_location(p, &locsafe);

  empty_cdata(p);

  return TRUE;
}


static int
prepare_cdata(dtd_parser *p)
{ if ( p->cdata->size == 0 )
    return TRUE;

  terminate_ocharbuf(p->cdata);

  if ( p->mark_state == MS_INCLUDE )
  { dtd *dtd = p->dtd;

    if ( p->environments )		/* needed for <img> <img> */
    { dtd_element *e = p->environments->element;

      if ( e->structure && e->structure->type == C_EMPTY && !e->undefined )
	close_element(p, e, FALSE);
    }

    if ( p->blank_cdata == TRUE )
    { int blank = TRUE;
      const ichar *s;

      for(s = p->cdata->data; *s; s++)
      { if ( !HasClass(dtd, *s, CH_BLANK) )
	{ blank = FALSE;
	  break;
	}
      }

      p->blank_cdata = blank;
      if ( !blank )
      { if ( p->dmode == DM_DTD )
	  gripe(ERC_SYNTAX_ERROR, "CDATA in DTD", p->cdata->data);
	else
	  open_element(p, CDATA_ELEMENT, TRUE);
      }
    }
  }

  return TRUE;
}


static int
process_cdata(dtd_parser *p, int last)
{ prepare_cdata(p);

  return emit_cdata(p, last);
}


static int
process_entity(dtd_parser *p, const ichar *name)
{ if ( name[0] == '#' )			/* #charcode: character entity */
  { int v = char_entity_value(name);

    if ( v < 0 )
      return gripe(ERC_SYNTAX_ERROR, "Bad character entity", name);

    if ( v >= OUTPUT_CHARSET_SIZE )
    { if ( p->on_entity )
      { process_cdata(p, FALSE);
	(*p->on_entity)(p, NULL, v);
      } else
	return gripe(ERC_REPRESENTATION, "character");
    } else
      add_ocharbuf(p->cdata, v);
  } else
  { dtd_symbol *id;
    dtd_entity *e;
    dtd *dtd = p->dtd;
    int len;
    const ichar *text;
    const ichar *s;
    int   chr;
    const char *file;

    if ( !(id=dtd_find_entity_symbol(dtd, name)) ||
	 !(e=id->entity) )
    { if ( dtd->default_entity )
	e = dtd->default_entity;
      else
	return gripe(ERC_EXISTENCE, "entity", name);
    }

    if ( !e->value &&
	 e->content == EC_SGML &&
	 (file=entity_file(p->dtd, e)) )
    { empty_icharbuf(p->buffer);		/* dubious */

      return sgml_process_file(p, file, SGML_SUB_DOCUMENT);
    }

    if ( !(text = entity_value(p, e, &len)) )
      return gripe(ERC_NO_VALUE, e->name->name);

    switch ( e->content )
    { case EC_SGML:
      case EC_CDATA:
	if ( (s=isee_character_entity(dtd, text, &chr)) && *s == '\0' )
	{ if ( p->blank_cdata == TRUE && !HasClass(dtd, chr, CH_BLANK) )
	  { p->cdata_must_be_empty = !open_element(p, CDATA_ELEMENT, FALSE);
	    p->blank_cdata = FALSE;
	  }
	    
	  if ( chr > 0 && chr < OUTPUT_CHARSET_SIZE )
	  { add_ocharbuf(p->cdata, chr);
	    return TRUE;
	  } else
	  { if ( p->on_entity )
	    { process_cdata(p, FALSE);
	      (*p->on_entity)(p, e, chr);
	    } else
	      return gripe(ERC_REPRESENTATION, "character");
	  }
	  break;
	}
	if ( e->content == EC_SGML )
	{ locbuf oldloc;

	  push_location(p, &oldloc);
	  set_src_dtd_parser(p, IN_ENTITY, e->name->name);
	  empty_icharbuf(p->buffer);		/* dubious */
	  for(s=text; *s; s++)
	    putchar_dtd_parser(p, *s);
	  pop_location(p, &oldloc);
	} else if ( *text )
	{ const ochar *o;

	  if ( p->blank_cdata == TRUE )
	  { p->cdata_must_be_empty = !open_element(p, CDATA_ELEMENT, FALSE);
	    p->blank_cdata = FALSE;
	  }

	  for(o=(const ochar *)text; *o; o++)
	    add_ocharbuf(p->cdata, *o);
	}
	break;
      case EC_SDATA:
      case EC_NDATA:
	process_cdata(p, FALSE);
	if ( p->on_data )
	  (*p->on_data)(p, e->content, len, text);
	break;
      case EC_PI:
	process_cdata(p, FALSE);
	if ( p->on_pi )
	  (*p->on_pi)(p, text);
      case EC_STARTTAG:
#if 0
	prepare_cdata(p);
	process_begin_element(p, text);
#endif
	break;
      case EC_ENDTAG:
#if 0
	prepare_cdata(p);
	process_end_element(p, text);
#endif
	break;
    }

    return TRUE;
  }

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Deal with end of input.  We should give a proper error message depending
on the state and the start-location of the error.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
end_document_dtd_parser_(dtd_parser *p)
{ int rval;

  switch(p->state)
  { case S_RCDATA:
    case S_CDATA:
    case S_PCDATA:
      rval = TRUE;
      break;
    case S_CMT:
    case S_CMTE0:
    case S_CMTE1:
    case S_DECLCMT0:
    case S_DECLCMT:
    case S_DECLCMTE0:
      rval = gripe(ERC_SYNTAX_ERROR,
		   "Unexpected end-of-file in comment", "");
      break;
    case S_ECDATA1:
    case S_ECDATA2:
    case S_EMSC1:
    case S_EMSC2:
    case S_DECL0:
    case S_DECL:
    case S_MDECL0:
    case S_STRING:
    case S_CMTO:
    case S_GROUP:
    case S_PENT:
    case S_ENT:
    case S_ENT0:
      rval = gripe(ERC_SYNTAX_ERROR,
		   "Unexpected end-of-file", "");
      break;
#ifdef UTF8
    case S_UTF8:
      rval = gripe(ERC_SYNTAX_ERROR,
		   "Unexpected end-of-file in UTF-8 sequence", "");
      break;
#endif
    case S_MSCDATA:
    case S_EMSCDATA1:
    case S_EMSCDATA2:
      rval = gripe(ERC_SYNTAX_ERROR,
		   "Unexpected end-of-file in CDATA marked section", "");
      break;
    case S_PI:
    case S_PI2:
      rval = gripe(ERC_SYNTAX_ERROR,
		   "Unexpected end-of-file in processing instruction", "");
      break;
    default:
      rval = gripe(ERC_SYNTAX_ERROR,
		   "Unexpected end-of-file in ???");
      break;
  }

  if ( p->dmode == DM_DATA )
  { sgml_environment *env;

    process_cdata(p, TRUE);

    if ( (env=p->environments) )
    { dtd_element *e;

      while(env->parent)
	env = env->parent;

      pop_to(p, env, CDATA_ELEMENT);
      e = env->element;
      if ( e->structure && !e->structure->omit_close )
	gripe(ERC_OMITTED_CLOSE, e->name->name);
      close_element(p, e, FALSE);
    }
  }

  return rval;
}


int
end_document_dtd_parser(dtd_parser *p)
{ int rval;

  WITH_PARSER(p, rval = end_document_dtd_parser_(p));

  return rval;
}


int
begin_document_dtd_parser(dtd_parser *p)
{ init_decoding(p);

  return TRUE;
}


void
reset_document_dtd_parser(dtd_parser *p)
{ if ( p->environments )
  { sgml_environment *env, *parent;

    for(env = p->environments; env; env=parent)
    { parent = env->parent;

      free_environment(env);
    }

    p->environments = NULL;
  }

  while(p->marked)
    pop_marked_section(p);

  empty_icharbuf(p->buffer);
  empty_ocharbuf(p->cdata);

  p->mark_state	   = MS_INCLUDE;
  p->state	   = S_PCDATA;
  p->grouplevel	   = 0;
  p->blank_cdata   = TRUE;
  p->event_class   = EV_EXPLICIT;
  p->dmode	   = DM_DATA;

  begin_document_dtd_parser(p);
}



/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Set the UTF-8 state
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef UTF8
static void
process_utf8(dtd_parser *p, int chr)
{ int bytes;
  int mask;

  for( bytes=1, mask=0x20; chr&mask; bytes++, mask >>= 1 )
    ;
  mask--;				/* 0x20 --> 0x1f */

  p->utf8_saved_state = p->state;		/* state to return to */
  p->state = S_UTF8;
  p->utf8_char = chr & mask;
  p->utf8_left = bytes;
}
#endif


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
add_cdata() adds a character to the output  data. It also maps \r\n onto
a single \n for Windows newline conventions.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
add_cdata(dtd_parser *p, int chr)
{ if ( p->mark_state == MS_INCLUDE )
  { ocharbuf *buf = p->cdata;

    if ( p->blank_cdata == TRUE && !HasClass(p->dtd, chr, CH_BLANK) )
    { p->cdata_must_be_empty = !open_element(p, CDATA_ELEMENT, FALSE);
      p->blank_cdata = FALSE;
    }

    if ( chr == '\n' )			/* insert missing CR */
    { int sz;

      if ( (sz=buf->size) == 0 ||
	   buf->data[sz-1] != CR )
	add_cdata(p, CR);
    }

    add_ocharbuf(buf, chr);
  
    if ( p->map && p->map->ends[chr] )
      match_shortref(p);

    if ( chr == '\n' )			/* dubious.  Whould we do that */
    { int sz;				/* here or in the space-handling? */

      if ( (sz=buf->size) > 1 &&
	   buf->data[sz-1] == LF &&
	   buf->data[sz-2] == CR )
      { buf->data[sz-2] = LF;
	buf->size--;
      }
    }
  }
}


static void
add_verbatim_cdata(dtd_parser *p, int chr)
{ if ( p->mark_state != MS_IGNORE )
  { ocharbuf *buf = p->cdata;

    if ( p->blank_cdata == TRUE && !HasClass(p->dtd, chr, CH_BLANK) )
    { p->cdata_must_be_empty = !open_element(p, CDATA_ELEMENT, FALSE);
      p->blank_cdata = FALSE;
    }

    if ( chr == '\n' && buf->size > 0 && buf->data[buf->size-1] == '\r' )
      buf->size--;
  
    add_ocharbuf(buf, chr);
  }
}


/* We discovered illegal markup and now process it as normal CDATA
*/

static void
recover_parser(dtd_parser *p)
{ const ichar *s;
  dtd *dtd = p->dtd;

  terminate_icharbuf(p->buffer);
  add_cdata(p, dtd->charmap->map[p->saved]);
  for(s=p->buffer->data; *s; s++)
    add_cdata(p, dtd->charmap->map[*s]);
  p->state = S_PCDATA;
}


static __inline void
setlocation(dtd_srcloc *d, dtd_srcloc *loc, int line, int lpos)
{ d->line    = line;
  d->linepos = lpos;
  d->charpos = loc->charpos - 1;
  d->type    = loc->type;
  d->name    = loc->name;
}


void
putchar_dtd_parser(dtd_parser *p, int chr)
{ dtd *dtd = p->dtd;
  const ichar *f = dtd->charfunc->func;
  int line = p->location.line;
  int lpos = p->location.linepos;
    
  if ( f[CF_RS] == chr )
  { p->location.line++;
    p->location.linepos = 0;
  } else
  { if ( f[CF_RE] == chr )
      p->location.linepos = 0;
    else
      p->location.linepos++;
  }

  p->location.charpos++;

reprocess:
  switch(p->state)
  { case S_PCDATA:
    { if ( f[CF_MDO1] == chr )		/* < */
      { setlocation(&p->startloc, &p->location, line, lpos);
	p->state = S_DECL0;
	empty_icharbuf(p->buffer);
	return;
      }
      if ( p->dmode == DM_DTD )
      { if ( f[CF_PERO] == chr )	/* % */
	{ setlocation(&p->startloc, &p->location, line, lpos);
	  p->state = S_PENT;
	  return;
	}
      } else
      { if ( f[CF_ERO] == chr )		/* & */
	{ setlocation(&p->startloc, &p->location, line, lpos);
	  p->state = S_ENT0;
	  return;
	}
      }
      
      if ( p->marked && f[CF_DSC] == chr ) /* ] in marked section */
      { empty_icharbuf(p->buffer);
	p->state = S_EMSC1;
	p->saved = chr;			/* for recovery */
	return;
      }

      if ( p->waiting_for_net && f[CF_ETAGO2] == chr ) /* shorttag */
      { setlocation(&p->startloc, &p->location, line, lpos);
	WITH_PARSER(p,
		    process_net(p));
	return;
      }
					/* Real character data */
#ifdef UTF8
      if ( p->utf8_decode && ISUTF8_MB(chr) )
      { process_utf8(p, chr);
	return;
      }
#endif
      if ( p->cdata->size == 0 )
        setlocation(&p->startcdata, &p->location, line, lpos);
      add_cdata(p, dtd->charmap->map[chr]);
      return;
    }
    case S_ECDATA2:			/* Seen </ in CDATA/RCDATA */
    { if ( f[CF_MDC] == chr &&
	   p->etaglen == p->buffer->size &&
	   istrncaseeq(p->buffer->data, p->etag, p->etaglen) )
      { p->cdata->size -= p->etaglen+2;	/* 2 for </ */
	terminate_ocharbuf(p->cdata);
	terminate_icharbuf(p->buffer);
	if ( p->mark_state == MS_INCLUDE )
	{ WITH_PARSER(p,
		      process_cdata(p, TRUE);
		      process_end_element(p, p->buffer->data));
	  empty_cdata(p);
	}
	empty_icharbuf(p->buffer);
	p->cdata_state = p->state = S_PCDATA;
      } else
      { add_verbatim_cdata(p, dtd->charmap->map[chr]);
	if ( p->etaglen < p->buffer->size || !HasClass(dtd, chr, CH_NAME))
	{ empty_icharbuf(p->buffer);	/* mismatch */
	  p->state = p->cdata_state;
	} else
	  add_icharbuf(p->buffer, chr);
      }
      return;
    }
    case S_ECDATA1:			/* seen < in CDATA */
    { add_verbatim_cdata(p, dtd->charmap->map[chr]);
      if ( f[CF_ETAGO2] == chr )	/* / */
      { empty_icharbuf(p->buffer);
	p->state = S_ECDATA2;
      } else if ( f[CF_ETAGO1] != chr )	/* <: do not change state */
	p->state = p->cdata_state;
      return;
    }
    case S_RCDATA:
    { if ( f[CF_ERO] == chr ) /* & */
      { setlocation(&p->startloc, &p->location, line, lpos);
	p->state = S_ENT0;
	return;
      }
      /*FALLTHROUGH*/
    }
    case S_CDATA:
    { add_verbatim_cdata(p, dtd->charmap->map[chr]);

      if ( f[CF_MDO1] == chr )		/* < */
      { setlocation(&p->startloc, &p->location, line, lpos);
	p->state = S_ECDATA1;
      }

					/* / in CDATA shorttag element */
      if ( p->waiting_for_net && f[CF_ETAGO2] == chr )
      { setlocation(&p->startloc, &p->location, line, lpos);
	p->cdata->size--;
	terminate_ocharbuf(p->cdata);
	terminate_icharbuf(p->buffer);
	if ( p->mark_state == MS_INCLUDE )
	{ WITH_PARSER(p,
		      process_cdata(p, TRUE);
		      process_net(p));
	  empty_cdata(p);
	}
	empty_icharbuf(p->buffer);
	p->cdata_state = p->state = S_PCDATA;
      }

      return;
    }
    case S_MSCDATA:
    { add_verbatim_cdata(p, dtd->charmap->map[chr]);
      if ( f[CF_DSC] == chr )		/* ] */
        p->state = S_EMSCDATA1;
      return;
    }
    case S_EMSCDATA1:
    { add_verbatim_cdata(p, dtd->charmap->map[chr]);
      if ( f[CF_DSC] == chr )		/* ]] */
        p->state = S_EMSCDATA2;
      else
        p->state = S_MSCDATA;
      return;
    }
    case S_EMSCDATA2:
    { add_verbatim_cdata(p, dtd->charmap->map[chr]);
      if ( f[CF_MDC] == chr )		/* ]]> */
      { p->cdata->size -= 3;		/* Delete chars for ]] */
	pop_marked_section(p);
	p->state = S_PCDATA;
      } else if ( f[CF_DSC] != chr )	/* if ]]], stay in this state */
        p->state = S_MSCDATA;
      return;
    }
    case S_EMSC1:
    { if ( f[CF_DSC] == chr )		/* ]] in marked section */
      { p->state = S_EMSC2;
	return;
      } else
      { add_icharbuf(p->buffer, chr);
	recover_parser(p);
	return;
      }
    }
    case S_EMSC2:
    { if ( f[CF_MDC] == chr )		/* ]]> in marked section */
      { pop_marked_section(p);
	p->state = S_PCDATA;
	return;
      } else
      { add_icharbuf(p->buffer, chr);
	recover_parser(p);
	return;
      }
    }
    case S_PENT:			/* %parameter entity; */
    { if ( f[CF_ERC] == chr )
      { p->state = S_PCDATA;
	terminate_icharbuf(p->buffer);
	if ( p->mark_state == MS_INCLUDE )
	{ WITH_PARSER(p, process_include(p, p->buffer->data));
	}
	empty_icharbuf(p->buffer);
	return;
      }
      if ( HasClass(dtd, chr, CH_NAME) )
      { add_icharbuf(p->buffer, chr);
	return;
      }

      terminate_icharbuf(p->buffer);
      gripe(ERC_SYNTAX_ERROR, "Illegal parameter entity", p->buffer->data);
      break;
    }
    case S_ENT0:			/* Seen & */
    { if ( chr == '#' || HasClass(dtd, chr, CH_NAME) )
      { empty_icharbuf(p->buffer);
	add_icharbuf(p->buffer, chr);
	p->state = S_ENT;
      } else
      { add_cdata(p, f[CF_ERO]);
	p->state = p->cdata_state;
	goto reprocess;
      }

      return;
    }    
    case S_ENT:				/* &entity; */
    { if ( HasClass(dtd, chr, CH_NAME) )
      { add_icharbuf(p->buffer, chr);
	return;
      }

      terminate_icharbuf(p->buffer);
      p->state = p->cdata_state;
      if ( p->mark_state == MS_INCLUDE )
      { WITH_PARSER(p, process_entity(p, p->buffer->data));
      }
      empty_icharbuf(p->buffer);
      
      if ( chr == CR )
	p->state = S_ENTCR;
      else if ( f[CF_ERC] != chr && chr != '\n' )
	goto reprocess;

      break;
    }
    case S_ENTCR:			/* seen &entCR, eat the LF */
    { p->state = p->cdata_state;
      if ( chr != LF )
	goto reprocess;

      break;
    }
    case S_DECL0:			/* Seen < */
    { if ( f[CF_ETAGO2] == chr )	/* </ */
      { add_icharbuf(p->buffer, chr);
	p->state = S_DECL;
      } else if ( HasClass(dtd, chr, CH_NAME) ) /* <letter */
      { add_icharbuf(p->buffer, chr);
	p->state = S_DECL;
      } else if ( f[CF_MDO2] == chr )	/* <! */
      { p->state = S_MDECL0;
      } else if ( f[CF_PRO2] == chr )	/* <? */
      { p->state = S_PI;
      } else				/* recover */
      { add_cdata(p, f[CF_MDO1]);
	add_cdata(p, chr);
	p->state = S_PCDATA;
      }

      return;
    }
    case S_MDECL0:			/* Seen <! */
    { if ( f[CF_CMT] == chr )		/* <!- */
      { p->state = S_CMTO;
	return;
      }
      add_icharbuf(p->buffer, f[CF_MDO2]);
      add_icharbuf(p->buffer, chr);
      p->state = S_DECL;
      return;
    }
    case S_DECL:			/* <...> */
    { if ( f[CF_MDC] == chr )		/* > */
      { prepare_cdata(p);
	p->state = S_PCDATA;
	terminate_icharbuf(p->buffer);
	if ( p->mark_state == MS_INCLUDE )
	{ WITH_PARSER(p, process_declaration(p, p->buffer->data));
	}
	empty_icharbuf(p->buffer);
	return;
      }
      if ( dtd->shorttag && f[CF_ETAGO2] == chr && p->buffer->size > 0 )
      { prepare_cdata(p);
	p->state = S_PCDATA;
	terminate_icharbuf(p->buffer);
	if ( p->mark_state == MS_INCLUDE )
	{ WITH_CLASS(p, EV_SHORTTAG,
		     WITH_PARSER(p, process_declaration(p, p->buffer->data)));
	}
	empty_icharbuf(p->buffer);
	p->waiting_for_net = TRUE;
	return;
      }

      add_icharbuf(p->buffer, chr);

      if ( f[CF_LIT] == chr )		/* " */
      { p->state = S_STRING;
	p->saved = chr;
	p->lit_saved_state = S_DECL;
      } else if ( f[CF_LITA] == chr )	/* ' */
      { p->state = S_STRING;
	p->saved = chr;
	p->lit_saved_state = S_DECL;
	return;
      } else if ( f[CF_CMT] == chr &&	/* - */
		  p->buffer->data[0] == f[CF_MDO2] ) /* Started <! */
      { p->state = S_DECLCMT0;
      } else if ( f[CF_DSO] == chr )	/* [: marked section */
      { terminate_icharbuf(p->buffer);

	process_marked_section(p);
      }

      break;
    }
    case S_DECLCMT0:			/* <...- */
    { if ( f[CF_CMT] == chr )
      { p->buffer->size--;
	p->state = S_DECLCMT;
      } else
      { add_icharbuf(p->buffer, chr);
	p->state = S_DECL;
      }
      break;
    }
    case S_DECLCMT:			/* <...--.. */
    { if ( f[CF_CMT] == chr )
	p->state = S_DECLCMTE0;
      break;
    }
    case S_DECLCMTE0:			/* <...--..- */
    { if ( f[CF_CMT] == chr )
	p->state = S_DECL;
      else
	p->state = S_DECLCMT;
      break;
    }
    case S_PI:
    { add_icharbuf(p->buffer, chr);
      if ( f[CF_PRO2] == chr )		/* <? ... ? */
	p->state = S_PI2;
      if ( f[CF_PRC] == chr )		/* no ? is ok too (XML/SGML) */
	goto pi;
      return;
    }
    case S_PI2:
    { if ( f[CF_PRC] == chr )
      { pi:
	process_cdata(p, FALSE);
	p->state = S_PCDATA;
	p->buffer->size--;
	terminate_icharbuf(p->buffer);
	if ( p->mark_state == MS_INCLUDE )
	{ WITH_PARSER(p, process_pi(p, p->buffer->data));
	}
	empty_icharbuf(p->buffer);
	return;
      }
      add_icharbuf(p->buffer, chr);
      p->state = S_PI;
      return;
    }
    case S_STRING:
    { add_icharbuf(p->buffer, chr);
      if ( chr == p->saved )
	p->state = p->lit_saved_state;
      break;
    }
    case S_CMTO:			/* Seen <!- */
    { if ( f[CF_CMT] == chr )		/* - */
      { p->state = S_CMT;
	return;
      } else
      { add_cdata(p, f[CF_MDO1]);
	add_cdata(p, f[CF_MDO2]);
	add_cdata(p, f[CF_CMT]);
	add_cdata(p, chr);
	p->state = S_PCDATA;
	return;
      }
    }
    case S_CMT:
    { if ( f[CF_CMT] == chr )
	p->state = S_CMTE0;		/* <!--...- */
      break;
    }
    case S_CMTE0:			/* <!--... -- */
    { if ( f[CF_CMT] == chr )
	p->state = S_CMTE1;
      else
	p->state = S_CMT;
      break;
    }
    case S_CMTE1:			/* <!--...-- seen */
    { if ( f[CF_MDC] == chr )		/* > */
      { if ( p->on_decl )
	  (*p->on_decl)(p, "");
	p->state = S_PCDATA;
      } else
	p->state = S_CMT;
      break;
    }
    case S_GROUP:			/* [...] in declaration */
    { add_icharbuf(p->buffer, chr);
      if ( f[CF_DSO] == chr )
      { p->grouplevel++;
      } else if ( f[CF_DSC] == chr )
      { if ( --p->grouplevel == 0 )
	  p->state = S_DECL;
      } else if ( f[CF_LIT] == chr )	/* " */
      { p->state = S_STRING;
	p->saved = chr;
	p->lit_saved_state = S_GROUP;
      } else if ( f[CF_LITA] == chr )	/* ' */
      { p->state = S_STRING;
	p->saved = chr;
	p->lit_saved_state = S_GROUP;
	return;
      }
      break;
    }
#ifdef UTF8
    case S_UTF8:
      if ( (chr & 0xc0) != 0x80 )	/* TBD: recover */
	gripe(ERC_SYNTAX_ERROR, "Bad UTF-8 sequence", "");
      p->utf8_char <<= 6;
      p->utf8_char |= (chr & ~0xc0);
      if ( --p->utf8_left == 0 )
      { if ( p->utf8_char >= OUTPUT_CHARSET_SIZE &&
	     p->mark_state == MS_INCLUDE )
	{ if ( p->on_entity )
	  { process_cdata(p, FALSE);
	    (*p->on_entity)(p, NULL, p->utf8_char);
	    goto utf8_done;
	  } else
	    gripe(ERC_REPRESENTATION, "character");
	}
	add_cdata(p, p->utf8_char);	/* verbatim? */
      utf8_done:
	p->state = p->utf8_saved_state;
      }

      break;
#endif
  }
}


		 /*******************************
		 *	     TOPLEVEL		*
		 *******************************/

int
load_dtd_from_file(dtd_parser *p, const char *file)
{ FILE *fd;
  int rval;
  data_mode   oldmode  = p->dmode;
  dtdstate    oldstate = p->state;
  locbuf      oldloc;

  push_location(p, &oldloc);
  p->dmode = DM_DTD;
  p->state = S_PCDATA;
  empty_icharbuf(p->buffer);		/* dubious */
  set_src_dtd_parser(p, IN_FILE, file);

  if ( (fd = fopen(file, "rb")) )
  { int chr;

    while( (chr = getc(fd)) != EOF )
      putchar_dtd_parser(p, chr);

    fclose(fd);

    p->dtd->implicit = FALSE;
    rval = TRUE;
  } else
    rval = FALSE;

  pop_location(p, &oldloc);
  p->dmode = oldmode;
  p->state = oldstate;

  return rval;
}


dtd *
file_to_dtd(const char *file, const char *doctype, dtd_dialect dialect)
{ dtd_parser *p = new_dtd_parser(new_dtd(doctype));

  set_dialect_dtd(p->dtd, dialect);

  if ( load_dtd_from_file(p, file) )
  { dtd *dtd = p->dtd;

    dtd->references++;			/* avoid deletion */
    free_dtd_parser(p);
    return dtd;
  } else
  { free_dtd_parser(p);

    return NULL;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
SGML sees a file as

[<LF>]Line 1<CR>
 <LF> Line 2<CR>

I.e. the newline  appearing  just  before   the  end-of-file  should  be
ignored. In addition, Unix-style files are   mapped  to CR-LF. Thanks to
Richard O'Keefe.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
sgml_process_stream(dtd_parser *p, FILE *fd, unsigned flags)
{ int p0, p1;

  if ( (p0 = getc(fd)) == EOF )
    return TRUE;
  if ( (p1 = getc(fd)) == EOF )
  { putchar_dtd_parser(p, p0);
    return end_document_dtd_parser(p);
  }

  for(;;)
  { int p2 = getc(fd);
    
    if ( p2 == EOF )
    { putchar_dtd_parser(p, p0);
      if ( p1 != LF )
	putchar_dtd_parser(p, p1);
      else if ( p0 != CR )
	putchar_dtd_parser(p, CR);

      if ( flags & SGML_SUB_DOCUMENT )
	return TRUE;
      else
	return end_document_dtd_parser(p);
    }

    putchar_dtd_parser(p, p0);
    p0 = p1;
    p1 = p2;
  } 
}


int
sgml_process_file(dtd_parser *p, const char *file, unsigned flags)
{ FILE *fd;
  int rval;
  locbuf oldloc;

  push_location(p, &oldloc);
  set_src_dtd_parser(p, IN_FILE, file);
  if ( !(flags & SGML_SUB_DOCUMENT) )
    set_mode_dtd_parser(p, DM_DATA);

  if ( (fd = fopen(file, "rb")) )
    rval = sgml_process_stream(p, fd, flags);
  else
    rval = FALSE;

  pop_location(p, &oldloc);

  return rval;
}



		 /*******************************
		 *	       ERRORS		*
		 *******************************/

static char *
format_location(char *s, dtd_srcloc *l)
{ int first = TRUE;

  if ( !l || l->type == IN_NONE )
    return s;

  for( ; l && l->type != IN_NONE;
         l = l->parent, first = FALSE )
  { if ( !first )
    { sprintf(s, " (from ");
      s += strlen(s);
    }

    switch(l->type)
    { case IN_NONE:
	assert(0);
      case IN_FILE:
	sprintf(s, "%s:%d:%d", l->name, l->line, l->linepos);
        break;
      case IN_ENTITY:
        sprintf(s, "&%s;%d:%d", l->name, l->line, l->linepos);
        break;
    }

    s += strlen(s);
    if ( !first )
    { *s++ = ')';
    }
  }

  *s++ = ':';
  *s++ = ' ';

  return s;
}


static void
format_message(dtd_error *e)
{ char buf[1024];
  char *s;
  int prefix_len;

  switch(e->severity)
  { case ERS_ERROR:
      strcpy(buf, "Error: ");
      break;
    case ERS_WARNING:
      strcpy(buf, "Warning: ");
      break;
    default:
      buf[0] = '\0';
  }
  s = buf+strlen(buf);

  s = format_location(s, e->location);
  prefix_len = s-buf;

  switch(e->id)
  { case ERC_REPRESENTATION:
      sprintf(s, "Cannot represent due to %s", e->argv[0]);
      break;
    case ERC_RESOURCE:
      sprintf(s, "Insufficient %s resources", e->argv[0]);
      break;
    case ERC_LIMIT:
      sprintf(s, "%s limit exceeded", e->argv[0]);
      break;
    case ERC_VALIDATE:
      sprintf(s, "%s", e->argv[0]);
      break;
    case ERC_SYNTAX_ERROR:
      sprintf(s, "Syntax error: %s", e->argv[0]);
      break;
    case ERC_EXISTENCE:
      sprintf(s, "%s \"%s\" does not exist", e->argv[0], e->argv[1]);
      break;
    case ERC_REDEFINED:
      sprintf(s, "Redefined %s \"%s\"", e->argv[0], e->argv[1]);
      break;
    default:
      ;
  }

  e->message = str2ring(buf);
  e->plain_message = e->message + prefix_len;
}


int
gripe(dtd_error_id e, ...)
{ va_list args;
  char buf[1024];
  dtd_error error;
  int dtdmode = FALSE;

  va_start(args, e);

  memset(&error, 0, sizeof(error));
  error.minor = e;			/* detailed error code */

  if ( current_parser )
  { error.location = &current_parser->location;
    if ( current_parser->dmode == DM_DTD )
      dtdmode = TRUE;
  } else
  { error.location = NULL;
  }

  switch(e)
  { case ERC_REPRESENTATION:
    case ERC_RESOURCE:
      error.severity = ERS_ERROR;
      error.argv[0]  = va_arg(args, char *);
      break;
    case ERC_LIMIT:
      error.severity = ERS_WARNING;
      error.argv[0]  = va_arg(args, char *);
      break;
    case ERC_SYNTAX_ERROR:
    case ERC_SYNTAX_WARNING:
    { char *m = va_arg(args, char *);
      const char *s = va_arg(args, const char *);

      if ( s && *s )
      { sprintf(buf, "%s, found \"%s\"", m, str_summary(s, 25));
	error.argv[0] = buf;
      } else
	error.argv[0] = m;
      
      error.severity = (e == ERC_SYNTAX_WARNING ? ERS_WARNING : ERS_ERROR);
      e = ERC_SYNTAX_ERROR;
      break;
    }
    case ERC_DOMAIN:
    { const char *expected = va_arg(args, const char *);
      const char *found    = str_summary(va_arg(args, const char *), 25);

      sprintf(buf, "Expected type %s, found \"%s\"", expected, found);
      error.argv[0] = buf;
      error.severity = ERS_ERROR;
      e = (dtdmode ? ERC_SYNTAX_ERROR : ERC_VALIDATE);
      break;
    }
    case ERC_REDEFINED:
    { dtd_symbol *name;
      error.argv[0] = va_arg(args, char *); /* type */
      name = va_arg(args, dtd_symbol *); /* name */
      error.argv[1] = (char *)name->name;
      error.severity = ERS_STYLE;
      break;
    }
    case ERC_EXISTENCE:
    { error.argv[0] = va_arg(args, char *); /* type */
      error.argv[1] = va_arg(args, char *); /* name */
      error.severity = ERS_ERROR;
      break;
    }
    case ERC_VALIDATE:
    { error.argv[0] = va_arg(args, char *); /* message */
      error.severity = ERS_WARNING;
      break;
    }
    case ERC_OMITTED_CLOSE:
    { const char *element = va_arg(args, const char *); 

      sprintf(buf, "Inserted omitted end-tag for \"%s\"", element);
      error.argv[0] = buf;
      error.severity = ERS_WARNING;
      e = ERC_VALIDATE;
      break;
    }
    case ERC_OMITTED_OPEN:
    { const char *element = va_arg(args, const char *); 

      sprintf(buf, "Inserted omitted start-tag for \"%s\"", element);
      error.argv[0] = buf;
      error.severity = ERS_WARNING;
      e = ERC_VALIDATE;
      break;
    }
    case ERC_NOT_OPEN:
    { const char *element = va_arg(args, const char *); 

      sprintf(buf, "Ignored end-tag for \"%s\" which is not open", element);
      error.argv[0] = buf;
      error.severity = ERS_WARNING;
      e = ERC_VALIDATE;
      break;
    }
    case ERC_NOT_ALLOWED:
    { const char *element = va_arg(args, const char *); 

      sprintf(buf, "Element \"%s\" not allowed here", element);
      error.argv[0] = buf;
      error.severity = ERS_WARNING;
      e = ERC_VALIDATE;
      break;
    }
    case ERC_NOT_ALLOWED_PCDATA:
    { const char *text = va_arg(args, const char *); 

      sprintf(buf, "#PCDATA (\"%s\") not allowed here", str_summary(text, 25));
      error.argv[0] = buf;
      error.severity = ERS_WARNING;
      e = ERC_VALIDATE;
      break;      
    }
    case ERC_NO_ATTRIBUTE:
    { const char *elem = va_arg(args, char *); /* element */
      const char *attr = va_arg(args, char *); /* attribute */

      sprintf(buf, "Element \"%s\" has no attribute \"%s\"", elem, attr);
      error.argv[0] = buf;
      error.severity = ERS_WARNING;

      e = ERC_VALIDATE;
      break;
    }
    case ERC_NO_ATTRIBUTE_VALUE:
    { const char *elem  = va_arg(args, char *); /* element */
      const char *value = va_arg(args, char *); /* attribute value */

      sprintf(buf, "Element \"%s\" has no attribute with value \"%s\"",
	      elem, value);
      error.argv[0] = buf;
      error.severity = ERS_WARNING;

      e = ERC_VALIDATE;
      break;
    }
    case ERC_NO_VALUE:
    { error.argv[0] = "entity value";
      error.argv[1] = va_arg(args, char *); /* entity */

      error.severity = ERS_ERROR;
      e = ERC_EXISTENCE;
      break;
    }
    case ERC_NO_DOCTYPE:
    { const char *doctype = va_arg(args, char *); /* element */
      const char *file    = va_arg(args, char *); /* DTD file */

      sprintf(buf, "No <!DOCTYPE ...>, assuming \"%s\" from DTD file \"%s\"",
	      doctype, file);
      error.argv[0] = buf;
      error.severity = ERS_WARNING;
      
      e = ERC_VALIDATE;
      break;
    }
    case ERC_NO_CATALOGUE:
    { char *file = va_arg(args, char *); /* catalogue file */

      error.argv[0] = "catalogue file";
      error.argv[1] = file;
      error.severity = ERS_WARNING;
      e = ERC_EXISTENCE;

      break;
    }
  } 

  error.id      = e;
  format_message(&error);

  if ( current_parser && current_parser->on_error )
    (*current_parser->on_error)(current_parser, &error);
  else
    fprintf(stderr, "SGML: %s\n", error.message);

  va_end(args);

  return FALSE;
}
