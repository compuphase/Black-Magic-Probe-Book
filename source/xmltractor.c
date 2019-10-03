
/*
	XML Tractor [v1.01]
	- goes through all that shit so you don't have to
	Copyright © 2012-2017 Arvīds Kokins
	More info on https://github.com/snake5/xml-tractor/
*/

#include <string.h>
#include <malloc.h>

#include "xmltractor.h"


#ifndef xt_alloc
#  define xt_alloc malloc
#endif
#ifndef xt_free
#  define xt_free free
#endif

#define WHITESPACE " \t\n\r"


/*  S t r i n g   h a n d l i n g  */

static
void xt_skip_ws( char** data )
{
	while( **data == ' ' || **data == '\t' || **data == '\n' || **data == '\r' )
		(*data)++;
}

static
void xt_skip_wsc( char** data )
{
	for(;;)
	{
		char* p = *data;
		if( *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '<' )
			break;
		if( *p == '<' )
		{
			if( p[1] != '!' || p[2] != '-' || p[3] != '-' )
				return;
			p += 4;
			for(;;)
			{
				if( *p == '\0' )
					break;
				if( p[0] == '-' && p[1] == '-' && p[2] == '>' )
				{
					p += 3;
					break;
				}
				p++;
			}
			*data = p;
		}
		else
			(*data)++;
	}
}

static
int xt_skip_until( char** data, char* what )
{
	for(;;)
	{
		char* wsp = what;
		while( *wsp && *wsp != **data )
			wsp++;

		if( *wsp )
			return 1;
		else
		{
			(*data)++;
			if( !**data )
				return 0;
		}
	}
}

static
int xt_skip_string( char** data, char qch )
{
	char* S = *data;
	while( *S )
	{
		if( *S == qch )
		{
			/* backtrack for backslashes */
			int bs = 0;
			char* T = S;
			while( T > *data && *(T - 1) == '\\' )
			{
				bs++;
				T--;
			}

			if( bs % 2 == 0 )
			{
				*data = S;
				return 1;
			}
		}
		S++;
	}
	return 0;
}

static
void xt_skip_hint( char** data )
{
	if( **data == '<' && (*data)[ 1 ] == '?' )
	{
		*data += 2;
		while( *(*data-1) != '?' || **data != '>' )
			(*data)++;
		(*data)++;
	}
}


/*  N o d e  */

static
xt_Node* xt_create_node()
{
	xt_Node* node = (xt_Node*) xt_alloc( sizeof( xt_Node ) );
	node->parent = NULL;
	node->firstchild = NULL;
	node->sibling = NULL;
	node->numchildren = 0;
	node->header = NULL;
	node->content = NULL;
	node->name = NULL;
	node->attribs = NULL;
	node->szheader = 0;
	node->szcontent = 0;
	node->szname = 0;
	node->numattribs = 0;
	return node;
}

void xt_destroy_node( xt_Node* root )
{
	if( root->firstchild ) xt_destroy_node( root->firstchild );
	if( root->sibling ) xt_destroy_node( root->sibling );

	if( root->attribs )	xt_free( root->attribs );
	xt_free( root );
}

static
void xt_node_add_attrib( xt_Node* node, xt_Attrib* attrib )
{
	if( node->attribs )
	{
		xt_Attrib* attrs = (xt_Attrib*) xt_alloc( sizeof( xt_Attrib ) * ( node->numattribs + 1 ) );
		memcpy( attrs, node->attribs, sizeof( xt_Attrib ) * node->numattribs );
		xt_free( node->attribs );
		node->attribs = attrs;
		node->attribs[ node->numattribs ] = *attrib;
	}
	else
	{
		node->attribs = (xt_Attrib*) xt_alloc( sizeof( xt_Attrib ) );
		node->attribs[ 0 ] = *attrib;
	}
	node->numattribs++;
}


/*  P a r s e r  */

static
xt_Node* xt_parse_node( char** data )
{
	xt_Node* node;
	xt_Node* C;
	char* S = *data;

	xt_skip_wsc( &S );
	if( *S != '<' )
		return NULL;

	node = xt_create_node();
	node->header = S;
	S++;

	/* name */
	xt_skip_ws( &S );
	node->name = S;
	if( !xt_skip_until( &S, WHITESPACE "/>" ) )
		goto fnq;
	node->szname = S - node->name;

	/* attributes */
	xt_skip_ws( &S );
	while( *S != '>' && *S != '/' )
	{
		xt_Attrib attrib = { 0 };
		attrib.name = S;

		if( !xt_skip_until( &S, WHITESPACE "=/>" ) )
			goto fnq;

		attrib.szname = S - attrib.name;
		xt_skip_ws( &S );

		/* value */
		if( *S == '=' )
		{
			S++;
			xt_skip_ws( &S );
			if( *S == '\"' || *S == '\'' )
			{
				char qch = *S;
				S++;
				attrib.value = S;
				if( !xt_skip_string( &S, qch ) )
					goto fnq;
			}
			else goto fnq;

			attrib.szvalue = S++ - attrib.value;
			xt_skip_ws( &S );
		}

		xt_node_add_attrib( node, &attrib );
	}

	if( *S == '/' )
	{
		if( S[ 1 ] != '>' )
			goto fnq;
		S += 2;
		node->szheader = S - node->header;
		goto fbe; /* finished before end */
	}

	S++;
	node->szheader = S - node->header;
	node->content = S;

	C = node;
	xt_skip_wsc( &S );
	while( *S )
	{
		if( *S == '<' )
		{
			char* RB = S;
			xt_Node* ch;

			if( S[ 1 ] == '/' )
			{
				char* EP;
				int len;

				S += 2;
				xt_skip_ws( &S );
				EP = S;
				if( !xt_skip_until( &S, WHITESPACE ">" ) )
					goto fnq;

				len = S - EP;
				if( len != node->szname || strncmp( node->name, EP, len ) != 0 )
				{
					*data = RB;
					goto fnq;
				}

				node->szcontent = RB - node->content;
				S++;
				break;
			}

			ch = xt_parse_node( &S );
			if( ch )
			{
				ch->parent = node;
				if( C == node )	C->firstchild = ch;
				else			C->sibling = ch;
				node->numchildren++;
				C = ch;
				continue;
			}
		}
		S++;
	}

fbe:
	*data = S;
	return node;

	/* free and quit */
fnq:
	xt_destroy_node( node );
	return NULL;
}

xt_Node* xt_parse( const char* data )
{
	xt_Node* root;
	char* S = (char*) data;

	if( (unsigned char) S[0] == 0xEF &&
		(unsigned char) S[1] == 0xBB &&
		(unsigned char) S[2] == 0xBF )
	{
		/* skip UTF-8 BOM */
		S += 3;
	}

	xt_skip_wsc( &S );
	xt_skip_hint( &S );
	xt_skip_wsc( &S );

	root = xt_parse_node( &S );

	return root;
}


/*  U t i l i t i e s  */

xt_Node* xt_find_child( xt_Node* node, const char* name )
{
    size_t szname = strlen( name );
	xt_Node* ch = node->firstchild;
	while( ch )
	{
		if( ch->szname == szname && strncmp( ch->name, name, szname ) == 0 )
			return ch;
		ch = ch->sibling;
	}
	return NULL;
}

xt_Node* xt_find_sibling( xt_Node* node, const char* name )
{
	size_t szname = strlen( name );
	xt_Node* ch = node->sibling;
	while( ch )
	{
		if( ch->szname == szname && strncmp( ch->name, name, szname ) == 0 )
			return ch;
		ch = ch->sibling;
	}
	return NULL;
}

xt_Attrib* xt_find_attrib( xt_Node* node, const char* name )
{
    size_t szname = strlen( name );
	xt_Attrib* p = node->attribs, * pend = node->attribs + node->numattribs;
	while( p < pend )
	{
		if( p->szname == szname && strncmp( p->name, name, szname ) == 0 )
			return p;
		p++;
	}
	return NULL;
}

