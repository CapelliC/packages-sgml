/*  $Id$

    Part of SWI-Prolog SGML/XML parser

    Author:  Jan Wielemaker
    E-mail:  jan@swi.psy.uva.nl
    WWW:     http://www.swi.psy.uva.nl/projects/SWI-Prolog/
    Copying: LGPL-2.  See the file COPYING or http://www.gnu.org

    Copyright (C) 1990-2000 SWI, University of Amsterdam. All rights reserved.
*/

#include "dtd.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#define streq(s1, s2) (strcmp(s1, s2) == 0)

char *program;

static void
usage()
{ fprintf(stderr, "Usage: %s [-xml] [-s] [file.dtd] file\n", program);
  exit(1);
}


static int
print_close(dtd_parser *p, dtd_element *e)
{ ichar name[MAXNMLEN];

  istrcpy(name, e->name->name);
  printf(")%s\n", istrupper(name));

  return TRUE;
}

typedef struct _atdef
{ attrtype	type;			/* AT_* */
  const char *	name;			/* name */
  int	       islist;			/* list-type */
} atdef;


static atdef attrs[] = 
{ { AT_CDATA,	 "cdata",    FALSE },
  { AT_ENTITY,	 "entity",   FALSE },
  { AT_ENTITIES, "entity",   TRUE },
  { AT_ID,	 "id",	     FALSE },
  { AT_IDREF,	 "idref",    FALSE },
  { AT_IDREFS,	 "idref",    TRUE },
  { AT_NAME,	 "name",     FALSE },
  { AT_NAMES,	 "name",     TRUE },
  { AT_NMTOKEN,	 "nmtoken",  FALSE },
  { AT_NMTOKENS, "nmtoken",  TRUE },
  { AT_NUMBER,	 "number",   FALSE },
  { AT_NUMBERS,	 "number",   TRUE },
  { AT_NUTOKEN,	 "nutoken",  FALSE },
  { AT_NUTOKENS, "nutoken",  TRUE },
  { AT_NOTATION, "notation", FALSE },

  { 0, NULL }
};


static const ichar *
find_attrdef(attrtype type)
{ atdef *ad = attrs;

  for(; ad->name; ad++)
  { if ( ad->type == type )
      return ad->name;
  }

  assert(0);
  return NULL;
}


static char *
mkupper(const ichar *s)
{ int len = strlen(s)+1;
  ichar *buf = alloca(len);

  istrcpy(buf, s);
  return str2ring((char *)istrupper(buf));
}


static int
print_open(dtd_parser *p, dtd_element *e, int argc, sgml_attribute *argv)
{ int i;

  for(i=0; i<argc; i++)
  { switch(argv[i].definition->type)
    { case AT_CDATA:
	printf("A%s CDATA %s\n",
	       mkupper(argv[i].definition->name->name),
	       argv[i].value.cdata);
	break;
      case AT_NUMBER:
	printf("A%s NUMBER ",
	       mkupper(argv[i].definition->name->name));

	if ( argv[i].value.text )
	  printf("%s\n", argv[i].value.text);
	else
	  printf("%ld\n", argv[i].value.number);

	break;
      case AT_NAMEOF:
	printf("A%s NAME %s\n",
	       mkupper(argv[i].definition->name->name),
	       mkupper(argv[i].value.text));
	break;
      default:
	printf("A%s %s %s\n",
	       mkupper(argv[i].definition->name->name),
	       mkupper(find_attrdef(argv[i].definition->type)),
	       mkupper(argv[i].value.text));
	break;
    }
  }

  printf("(%s\n", mkupper(e->name->name));

  return TRUE;
}


static int
print_data(dtd_parser *p, data_type type, int len, const ochar *data)
{ switch(type)
  { case EC_CDATA:
      putchar('-');
      break;
    case EC_NDATA:
      putchar('N');
      break;
    case EC_SDATA:
      putchar('S');
      break;
    default:
      assert(0);
  }

  for( ; *data; data++ )
  { if ( *data == '\n' )
    { putchar('\\');
      putchar('n');
    } else
      putchar(*data);
  }
  putchar('\n');

  return TRUE;
}


static int
on_entity(dtd_parser *p, dtd_entity *e, int chr)
{ if ( e )
  { printf("&%s;", e->name->name);
  } else
    printf("&#%d;", chr);

  return TRUE;
}


static int
on_pi(dtd_parser *p, const ichar *pi)
{ printf("?%s?\n", pi);

  return TRUE;
}


static void
set_functions(dtd_parser *p)
{ p->on_end_element = print_close;
  p->on_begin_element = print_open;
  p->on_data = print_data;
  p->on_entity = on_entity;
  p->on_pi = on_pi;
}

#define shift (argc--, argv++)

int
main(int argc, char **argv)
{ dtd_parser *p = NULL;
  char *s, *ext;
  int xml = FALSE;
  int output = TRUE;

  if ( (s=strrchr(argv[0], '/')) )
    program = s+1;
  else
    program = argv[0];

  if ( streq(program, "xml") )
    xml = TRUE;

  shift;

  while(argc>0 && argv[0][0] == '-')
  { if ( streq(argv[0], "-xml") )
    { xml = TRUE;
      shift;
    } else if ( streq(argv[0], "-s") )
    { output = FALSE;
      shift;
    } else
      usage();
  }

  if ( argc == 0 )
    usage();

  ext = strchr(argv[0], '.');
  if ( streq(ext, ".dtd") )
  { char doctype[256];
    
    strncpy(doctype, argv[0], ext-argv[0]);
    doctype[ext-argv[0]] = '\0';
      
    p = new_dtd_parser(new_dtd(doctype));
    load_dtd_from_file(p, argv[0]);
    argc--; argv++;
  } else if ( istrcaseeq(ext, ".html") ||
	      istrcaseeq(ext, ".htm" ) )
  { p = new_dtd_parser(new_dtd("html"));

    load_dtd_from_file(p, "html.dtd");
  } else if ( xml || istrcaseeq(ext, ".xml") )
  { dtd *dtd = new_dtd(NULL);

    set_dialect_dtd(dtd, DL_XML);
    p = new_dtd_parser(dtd);
  } else
  { p = new_dtd_parser(new_dtd(NULL));
  }

  if ( argc == 1 )
  { if ( output )
      set_functions(p);
    sgml_process_file(p, argv[0]);
    free_dtd_parser(p);
    if ( output )
      printf("C\n");
    return 0;
  } else
  { usage();
    return 1;
  }
}



