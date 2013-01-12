/*

 gg_xml.c -- XML Document implementation
    
 version 4.0, 2012 December 10

 Author: Sandro Furieri a.furieri@lqt.it

 ------------------------------------------------------------------------------
 
 Version: MPL 1.1/GPL 2.0/LGPL 2.1
 
 The contents of this file are subject to the Mozilla Public License Version
 1.1 (the "License"); you may not use this file except in compliance with
 the License. You may obtain a copy of the License at
 http://www.mozilla.org/MPL/
 
Software distributed under the License is distributed on an "AS IS" basis,
WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
for the specific language governing rights and limitations under the
License.

The Original Code is the SpatiaLite library

The Initial Developer of the Original Code is Alessandro Furieri
 
Portions created by the Initial Developer are Copyright (C) 2008-2012
the Initial Developer. All Rights Reserved.

Contributor(s):

Alternatively, the contents of this file may be used under the terms of
either the GNU General Public License Version 2 or later (the "GPL"), or
the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
in which case the provisions of the GPL or the LGPL are applicable instead
of those above. If you wish to allow use of your version of this file only
under the terms of either the GPL or the LGPL, and not to allow others to
use your version of this file under the terms of the MPL, indicate your
decision by deleting the provisions above and replace them with the notice
and other provisions required by the GPL or the LGPL. If you do not delete
the provisions above, a recipient may use your version of this file under
the terms of any one of the MPL, the GPL or the LGPL.
 
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) && !defined(__MINGW32__)
#include "config-msvc.h"
#else
#include "config.h"
#endif

#ifdef ENABLE_LIBXML2		/* LIBXML2 enabled: supporting XML documents */

#include <zlib.h>
#include <libxml/parser.h>
#include <libxml/xmlschemas.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include <spatialite_private.h>
#include <spatialite/sqlite.h>
#include <spatialite/debug.h>
#include <spatialite/gaiageo.h>
#include <spatialite/gaiaaux.h>

static void
spliteSilentError (void *ctx, const char *msg, ...)
{
/* shutting up XML Errors */
    if (ctx != NULL)
	ctx = NULL;		/* suppressing stupid compiler warnings (unused args) */
    if (msg != NULL)
	ctx = NULL;		/* suppressing stupid compiler warnings (unused args) */
}

static void
spliteParsingError (void *ctx, const char *msg, ...)
{
/* appending to the current Parsing Error buffer */
    struct splite_internal_cache *cache = (struct splite_internal_cache *) ctx;
    gaiaOutBufferPtr buf = (gaiaOutBufferPtr) (cache->xmlParsingErrors);
    char out[65536];
    va_list args;

    if (ctx != NULL)
	ctx = NULL;		/* suppressing stupid compiler warnings (unused args) */

    va_start (args, msg);
    vsnprintf (out, 65536, msg, args);
    gaiaAppendToOutBuffer (buf, out);
    va_end (args);
}

static void
spliteSchemaValidationError (void *ctx, const char *msg, ...)
{
/* appending to the current SchemaValidation Error buffer */
    struct splite_internal_cache *cache = (struct splite_internal_cache *) ctx;
    gaiaOutBufferPtr buf =
	(gaiaOutBufferPtr) (cache->xmlSchemaValidationErrors);
    char out[65536];
    va_list args;

    if (ctx != NULL)
	ctx = NULL;		/* suppressing stupid compiler warnings (unused args) */

    va_start (args, msg);
    vsnprintf (out, 65536, msg, args);
    gaiaAppendToOutBuffer (buf, out);
    va_end (args);
}

static void
spliteResetXmlErrors (struct splite_internal_cache *cache)
{
/* resetting the XML Error buffers */
    gaiaOutBufferPtr buf = (gaiaOutBufferPtr) (cache->xmlParsingErrors);
    gaiaOutBufferReset (buf);
    buf = (gaiaOutBufferPtr) (cache->xmlSchemaValidationErrors);
    gaiaOutBufferReset (buf);
}

GAIAGEO_DECLARE char *
gaiaXmlBlobGetLastParseError (void *ptr)
{
/* get the most recent XML Parse error/warning message */
    struct splite_internal_cache *cache = (struct splite_internal_cache *) ptr;
    gaiaOutBufferPtr buf = (gaiaOutBufferPtr) (cache->xmlParsingErrors);
    return buf->Buffer;
}

GAIAGEO_DECLARE char *
gaiaXmlBlobGetLastValidateError (void *ptr)
{
/* get the most recent XML Validate error/warning message */
    struct splite_internal_cache *cache = (struct splite_internal_cache *) ptr;
    gaiaOutBufferPtr buf =
	(gaiaOutBufferPtr) (cache->xmlSchemaValidationErrors);
    return buf->Buffer;
}

GAIAGEO_DECLARE char *
gaiaXmlBlobGetLastXPathError (void *ptr)
{
/* get the most recent XML Validate error/warning message */
    struct splite_internal_cache *cache = (struct splite_internal_cache *) ptr;
    gaiaOutBufferPtr buf = (gaiaOutBufferPtr) (cache->xmlXPathErrors);
    return buf->Buffer;
}

SPATIALITE_PRIVATE void
splite_free_xml_schema_cache_item (struct splite_xmlSchema_cache_item *p)
{
/* freeing an XmlSchema Cache Item */
    if (p->schemaURI)
	free (p->schemaURI);
    if (p->parserCtxt)
	xmlSchemaFreeParserCtxt (p->parserCtxt);
    if (p->schema)
	xmlSchemaFree (p->schema);
    if (p->schemaDoc)
	xmlFreeDoc (p->schemaDoc);
    p->schemaURI = NULL;
    p->parserCtxt = NULL;
    p->schemaDoc = NULL;
    p->schema = NULL;
}

static int
splite_xmlSchemaCacheFind (struct splite_internal_cache *cache,
			   const char *schemaURI, xmlDocPtr * schema_doc,
			   xmlSchemaParserCtxtPtr * parser_ctxt,
			   xmlSchemaPtr * schema)
{
/* attempting to retrive some XmlSchema from within the Cache */
    int i;
    time_t now;
    struct splite_xmlSchema_cache_item *p;
    for (i = 0; i < MAX_XMLSCHEMA_CACHE; i++)
      {
	  p = &(cache->xmlSchemaCache[i]);
	  if (p->schemaURI)
	    {
		if (strcmp (schemaURI, p->schemaURI) == 0)
		  {
		      /* found a matching cache-item */
		      *schema_doc = p->schemaDoc;
		      *parser_ctxt = p->parserCtxt;
		      *schema = p->schema;
		      /* updating the timestamp */ time (&now);
		      p->timestamp = now;
		      return 1;
		  }
	    }
      }
    return 0;
}

static void
splite_xmlSchemaCacheInsert (struct splite_internal_cache *cache,
			     const char *schemaURI, xmlDocPtr schema_doc,
			     xmlSchemaParserCtxtPtr parser_ctxt,
			     xmlSchemaPtr schema)
{
/* inserting a new XmlSchema item into the Cache */
    int i;
    int len = strlen (schemaURI);
    time_t now;
    time_t oldest;
    struct splite_xmlSchema_cache_item *pSlot = NULL;
    struct splite_xmlSchema_cache_item *p;
    time (&now);
    oldest = now;
    for (i = 0; i < MAX_XMLSCHEMA_CACHE; i++)
      {
	  p = &(cache->xmlSchemaCache[i]);
	  if (p->schemaURI == NULL)
	    {
		/* found an empty slot */
		pSlot = p;
		break;
	    }
	  if (p->timestamp < oldest)
	    {
		/* saving the oldest slot */
		pSlot = p;
		oldest = p->timestamp;
	    }
      }
/* inserting into the Cache Slot */
    splite_free_xml_schema_cache_item (pSlot);
    pSlot->timestamp = now;
    pSlot->schemaURI = malloc (len + 1);
    strcpy (pSlot->schemaURI, schemaURI);
    pSlot->schemaDoc = schema_doc;
    pSlot->parserCtxt = parser_ctxt;
    pSlot->schema = schema;
}

static void
sniff_payload (xmlDocPtr xml_doc, int *is_iso_metadata, int *is_sld_se_style,
	       int *is_svg)
{
/* sniffing the payload type */
    xmlNodePtr root = xmlDocGetRootElement (xml_doc);
    *is_iso_metadata = 0;
    *is_sld_se_style = 0;
    *is_svg = 0;
    if (root->name != NULL)
      {
	  if (strcmp (root->name, "MD_Metadata") == 0)
	      *is_iso_metadata = 1;
	  if (strcmp (root->name, "StyledLayerDescriptor") == 0)
	      *is_sld_se_style = 1;
	  if (strcmp (root->name, "svg") == 0)
	      *is_svg = 1;
      }
}

static void
find_iso_ids (xmlNodePtr node, const char *name, char **string, int *open_tag,
	      int *char_string, int *count)
{
/* recursively scanning the DOM tree [fileIdentifier or parentIdentifier] */
    xmlNode *cur_node = NULL;
    int open = 0;
    int cs = 0;

    for (cur_node = node; cur_node; cur_node = cur_node->next)
      {
	  if (cur_node->type == XML_ELEMENT_NODE)
	    {
		if (*open_tag == 1)
		  {
		      if (strcmp (cur_node->name, "CharacterString") == 0)
			{
			    cs = 1;
			    *char_string = 1;
			}
		  }
		if (strcmp (cur_node->name, name) == 0)
		  {
		      if (cur_node->parent != NULL)
			{
			    if (cur_node->parent->type == XML_ELEMENT_NODE)
			      {
				  if (strcmp
				      (cur_node->parent->name,
				       "MD_Metadata") == 0)
				    {
					/* 
					   / only if <MD_Metadata>
					   /           <fileIdentifier>
					   /             <CharacterString> 
					 */
					open = 1;
					*open_tag = 1;
				    }
			      }
			}
		  }
	    }
	  if (cur_node->type == XML_TEXT_NODE && *open_tag == 1
	      && *char_string == 1)
	    {
		if (cur_node->content != NULL)
		  {
		      int len = strlen ((const char *) cur_node->content);
		      char *buf = malloc (len + 1);
		      strcpy (buf, (const char *) cur_node->content);
		      if (*string)
			  free (*string);
		      *string = buf;
		      *count += 1;
		  }
	    }

	  find_iso_ids (cur_node->children, name, string, open_tag, char_string,
			count);
	  if (open)
	      *open_tag = 0;
	  if (cs)
	      *char_string = 0;
      }
}

static void
find_iso_title (xmlNodePtr node, char **string, int *open_tag, int *char_string,
		int *count)
{
/* recursively scanning the DOM tree [title] */
    xmlNode *cur_node = NULL;
    xmlNode *parent;
    int open = 0;
    int cs = 0;
    int ok_parent;

    for (cur_node = node; cur_node; cur_node = cur_node->next)
      {
	  if (cur_node->type == XML_ELEMENT_NODE)
	    {
		if (*open_tag == 1)
		  {
		      if (strcmp (cur_node->name, "CharacterString") == 0)
			{
			    cs = 1;
			    *char_string = 1;
			}
		  }
		if (strcmp (cur_node->name, "title") == 0)
		  {
		      ok_parent = 0;
		      parent = cur_node->parent;
		      if (parent)
			{
			    if (strcmp (parent->name, "CI_Citation") == 0)
				ok_parent++;
			}
		      if (ok_parent == 1)
			{
			    parent = parent->parent;
			    if (strcmp (parent->name, "citation") == 0)
				ok_parent++;
			}
		      if (ok_parent == 2)
			{
			    parent = parent->parent;
			    if (strcmp (parent->name, "MD_DataIdentification")
				== 0)
				ok_parent++;
			}
		      if (ok_parent == 3)
			{
			    parent = parent->parent;
			    if (strcmp (parent->name, "identificationInfo") ==
				0)
				ok_parent++;
			}
		      if (ok_parent == 4)
			{
			    parent = parent->parent;
			    if (strcmp (parent->name, "MD_Metadata") == 0)
				ok_parent++;
			}
		      if (ok_parent == 5)
			{
			    /* 
			       / only if <MD_Metadata>
			       /           <identificationInfo>
			       /             <MD_DataIdentification>
			       /               <citation>
			       /                 <CI_Citation>
			       /                   <title> 
			     */
			    open = 1;
			    *open_tag = 1;
			}
		  }
	    }
	  if (cur_node->type == XML_TEXT_NODE && *open_tag == 1
	      && *char_string == 1)
	    {
		if (cur_node->content != NULL)
		  {
		      int len = strlen ((const char *) cur_node->content);
		      char *buf = malloc (len + 1);
		      strcpy (buf, (const char *) cur_node->content);
		      if (*string)
			  free (*string);
		      *string = buf;
		      *count += 1;
		  }
	    }

	  find_iso_title (cur_node->children, string, open_tag, char_string,
			  count);
	  if (open)
	      *open_tag = 0;
	  if (cs)
	      *char_string = 0;
      }
}

static void
find_iso_abstract (xmlNodePtr node, char **string, int *open_tag,
		   int *char_string, int *count)
{
/* recursively scanning the DOM abstract [title] */
    xmlNode *cur_node = NULL;
    xmlNode *parent;
    int open = 0;
    int cs = 0;
    int ok_parent;

    for (cur_node = node; cur_node; cur_node = cur_node->next)
      {
	  if (cur_node->type == XML_ELEMENT_NODE)
	    {
		if (*open_tag == 1)
		  {
		      if (strcmp (cur_node->name, "CharacterString") == 0)
			{
			    cs = 1;
			    *char_string = 1;
			}
		  }
		if (strcmp (cur_node->name, "abstract") == 0)
		  {
		      ok_parent = 0;
		      parent = cur_node->parent;
		      if (parent)
			{
			    if (strcmp (parent->name, "MD_DataIdentification")
				== 0)
				ok_parent++;
			}
		      if (ok_parent == 1)
			{
			    parent = parent->parent;
			    if (strcmp (parent->name, "identificationInfo") ==
				0)
				ok_parent++;
			}
		      if (ok_parent == 2)
			{
			    parent = parent->parent;
			    if (strcmp (parent->name, "MD_Metadata") == 0)
				ok_parent++;
			}
		      if (ok_parent == 3)
			{
			    /* only if <MD_Metadata>
			       /            <identificationInfo>
			       /              <MD_DataIdentification>
			       /                <abstract> 
			     */
			    open = 1;
			    *open_tag = 1;
			}
		  }
	    }
	  if (cur_node->type == XML_TEXT_NODE && *open_tag == 1
	      && *char_string == 1)
	    {
		if (cur_node->content != NULL)
		  {
		      int len = strlen ((const char *) cur_node->content);
		      char *buf = malloc (len + 1);
		      strcpy (buf, (const char *) cur_node->content);
		      if (*string)
			  free (*string);
		      *string = buf;
		      *count += 1;
		  }
	    }

	  find_iso_abstract (cur_node->children, string, open_tag, char_string,
			     count);
	  if (open)
	      *open_tag = 0;
	  if (cs)
	      *char_string = 0;
      }
}

static void
find_bbox_coord (xmlNodePtr node, const char *name, double *coord,
		 int *open_tag, int *decimal, int *count)
{
/* recursively scanning an EX_GeographicBoundingBox sub-tree */
    xmlNode *cur_node = NULL;
    int open = 0;
    int dec = 0;

    for (cur_node = node; cur_node; cur_node = cur_node->next)
      {
	  if (cur_node->type == XML_ELEMENT_NODE)
	    {
		if (*open_tag == 1)
		  {
		      if (strcmp (cur_node->name, "Decimal") == 0)
			{
			    dec = 1;
			    *decimal = 1;
			}
		  }
		if (strcmp (cur_node->name, name) == 0)
		  {
		      open = 1;
		      *open_tag = 1;
		  }
	    }
	  if (cur_node->type == XML_TEXT_NODE && *open_tag == 1
	      && *decimal == 1)
	    {
		if (cur_node->content != NULL)
		  {
		      /* found a coord value */
		      double value = atof ((const char *) cur_node->content);
		      *coord = value;
		      *count += 1;
		  }
	    }

	  find_bbox_coord (cur_node->children, name, coord, open_tag, decimal,
			   count);
	  if (open)
	      *open_tag = 0;
	  if (dec)
	      *decimal = 0;
      }
}

static int
parse_bounding_box (xmlNodePtr node, double *minx, double *miny, double *maxx,
		    double *maxy)
{
/* attempting to parse an EX_GeographicBoundingBox sub-tree */
    int ok_minx = 0;
    int ok_miny = 0;
    int ok_maxx = 0;
    int ok_maxy = 0;
    int open_tag;
    int decimal;
    int count;
    double coord;

/* retrieving minx - West */
    open_tag = 0;
    decimal = 0;
    count = 0;
    find_bbox_coord (node, "westBoundLongitude", &coord, &open_tag, &decimal,
		     &count);
    if (count == 1)
      {
	  *minx = coord;
	  ok_minx = 1;
      }

/* retrieving maxx - East */
    open_tag = 0;
    decimal = 0;
    count = 0;
    find_bbox_coord (node, "eastBoundLongitude", &coord, &open_tag, &decimal,
		     &count);
    if (count == 1)
      {
	  *maxx = coord;
	  ok_maxx = 1;
      }

/* retrieving miny - South */
    open_tag = 0;
    decimal = 0;
    count = 0;
    find_bbox_coord (node, "southBoundLatitude", &coord, &open_tag, &decimal,
		     &count);
    if (count == 1)
      {
	  *miny = coord;
	  ok_miny = 1;
      }

/* retrieving maxy - North */
    open_tag = 0;
    decimal = 0;
    count = 0;
    find_bbox_coord (node, "northBoundLatitude", &coord, &open_tag, &decimal,
		     &count);
    if (count == 1)
      {
	  *maxy = coord;
	  ok_maxy = 1;
      }

    if (ok_minx && ok_miny && ok_maxx && ok_maxy)
      {
	  /* ok, valid BBOX */
	  return 1;
      }
    return 0;
}

static void
find_iso_geometry (xmlNodePtr node, gaiaGeomCollPtr * geom)
{
/* recursively scanning the DOM tree [geometry] */
    xmlNode *cur_node = NULL;
    xmlNode *parent;
    int ok_parent;

    for (cur_node = node; cur_node; cur_node = cur_node->next)
      {
	  if (cur_node->type == XML_ELEMENT_NODE)
	    {
		if (strcmp (cur_node->name, "EX_GeographicBoundingBox") == 0)
		  {
		      ok_parent = 0;
		      parent = cur_node->parent;
		      if (parent)
			{
			    if (strcmp (parent->name, "geographicElement") == 0)
				ok_parent++;
			}
		      if (ok_parent == 1)
			{
			    parent = parent->parent;
			    if (strcmp (parent->name, "EX_Extent") == 0)
				ok_parent++;
			}
		      if (ok_parent == 2)
			{
			    parent = parent->parent;
			    if (strcmp (parent->name, "extent") == 0)
				ok_parent++;
			}
		      if (ok_parent == 3)
			{
			    parent = parent->parent;
			    if (strcmp (parent->name, "MD_DataIdentification")
				== 0)
				ok_parent++;
			}
		      if (ok_parent == 4)
			{
			    parent = parent->parent;
			    if (strcmp (parent->name, "identificationInfo") ==
				0)
				ok_parent++;
			}
		      if (ok_parent == 5)
			{
			    parent = parent->parent;
			    if (strcmp (parent->name, "MD_Metadata") == 0)
				ok_parent++;
			}
		      if (ok_parent == 6)
			{
			    /* only if <MD_Metadata>
			       /            <identificationInfo>
			       /              <MD_DataIdentification>
			       /                <extent>
			       /                  <EX_Extent>
			       /                    <geographicElement>
			       /                      <EX_GeographicBoundingBox> 
			     */
			    double minx;
			    double maxx;
			    double miny;
			    double maxy;
			    if (parse_bounding_box
				(cur_node, &minx, &miny, &maxx, &maxy))
			      {
				  gaiaPolygonPtr pg;
				  gaiaRingPtr rng;
				  gaiaGeomCollPtr g = *geom;
				  if (g == NULL)
				    {
					g = gaiaAllocGeomColl ();
					g->Srid = 4326;
					g->DeclaredType = GAIA_MULTIPOLYGON;
				    }
				  pg = gaiaAddPolygonToGeomColl (g, 5, 0);
				  rng = pg->Exterior;
				  gaiaSetPoint (rng->Coords, 0, minx, miny);
				  gaiaSetPoint (rng->Coords, 1, maxx, miny);
				  gaiaSetPoint (rng->Coords, 2, maxx, maxy);
				  gaiaSetPoint (rng->Coords, 3, minx, maxy);
				  gaiaSetPoint (rng->Coords, 4, minx, miny);
				  *geom = g;
			      }
			}
		  }
	    }
	  find_iso_geometry (cur_node->children, geom);
      }
}

static void
retrieve_iso_identifiers (struct splite_internal_cache *cache,
			  xmlDocPtr xml_doc, char **fileIdentifier,
			  char **parentIdentifier, char **title,
			  char **abstract, unsigned char **geometry,
			  short *geometry_len)
{
/*
/ attempting to retrieve the FileIdentifier, ParentIdentifier,
/ Title, Abstract and Geometry items from an ISO Metadata document
*/
    xmlNodePtr root = xmlDocGetRootElement (xml_doc);
    int open_tag;
    int char_string;
    int count;
    char *string;
    gaiaGeomCollPtr geom = NULL;

    *fileIdentifier = NULL;
    *parentIdentifier = NULL;
    *title = NULL;
    *abstract = NULL;
    *geometry = NULL;

/* attempting to retrieve the FileIdentifier item */
    open_tag = 0;
    char_string = 0;
    count = 0;
    string = NULL;
    find_iso_ids (root, "fileIdentifier", &string, &open_tag, &char_string,
		  &count);
    if (string)
      {
	  if (count == 1)
	      *fileIdentifier = string;
	  else
	      free (string);
      }

/* attempting to retrieve the ParentIdentifier item */
    open_tag = 0;
    char_string = 0;
    count = 0;
    string = NULL;
    find_iso_ids (root, "parentIdentifier", &string, &open_tag, &char_string,
		  &count);
    if (string)
      {
	  if (count == 1)
	      *parentIdentifier = string;
	  else
	      free (string);
      }

/* attempting to retrieve the Title item */
    open_tag = 0;
    char_string = 0;
    count = 0;
    string = NULL;
    find_iso_title (root, &string, &open_tag, &char_string, &count);
    if (string)
      {
	  if (count == 1)
	      *title = string;
	  else
	      free (string);
      }

/* attempting to retrieve the Abstract item */
    open_tag = 0;
    char_string = 0;
    count = 0;
    string = NULL;
    find_iso_abstract (root, &string, &open_tag, &char_string, &count);
    if (string)
      {
	  if (count == 1)
	      *abstract = string;
	  else
	      free (string);
      }

/* attempting to retrieve the Geometry item */
    open_tag = 0;
    char_string = 0;
    count = 0;
    string = NULL;
    find_iso_geometry (root, &geom);
    if (geom)
      {
	  int blob_len;
	  unsigned char *blob = NULL;
	  gaiaMbrGeometry (geom);
	  gaiaToSpatiaLiteBlobWkb (geom, &blob, &blob_len);
	  gaiaFreeGeomColl (geom);
	  *geometry = blob;
	  *geometry_len = (short) blob_len;
      }
}

GAIAGEO_DECLARE void
gaiaXmlToBlob (void *p_cache, const char *xml, int xml_len, int compressed,
	       const char *schemaURI, unsigned char **result, int *size,
	       char **parsing_errors, char **schema_validation_errors)
{
/* attempting to build an XmlBLOB buffer */
    xmlDocPtr xml_doc;
    xmlDocPtr schema_doc;
    xmlSchemaPtr schema;
    xmlSchemaParserCtxtPtr parser_ctxt;
    xmlSchemaValidCtxtPtr valid_ctxt;
    int is_iso_metadata = 0;
    int is_sld_se_style = 0;
    int is_svg = 0;
    int len;
    int zip_len;
    short uri_len = 0;
    short fileid_len = 0;
    short parentid_len = 0;
    short title_len = 0;
    short abstract_len = 0;
    short geometry_len = 0;
    char *fileIdentifier = NULL;
    char *parentIdentifier = NULL;
    char *title = NULL;
    char *abstract = NULL;
    unsigned char *geometry = NULL;
    uLong crc;
    Bytef *zip_buf;
    unsigned char *buf;
    unsigned char *ptr;
    unsigned char flags = 0x00;
    int endian_arch = gaiaEndianArch ();
    struct splite_internal_cache *cache =
	(struct splite_internal_cache *) p_cache;
    gaiaOutBufferPtr parsingBuf = (gaiaOutBufferPtr) (cache->xmlParsingErrors);
    gaiaOutBufferPtr schemaValidationBuf =
	(gaiaOutBufferPtr) (cache->xmlSchemaValidationErrors);
    xmlGenericErrorFunc silentError = (xmlGenericErrorFunc) spliteSilentError;
    xmlGenericErrorFunc parsingError = (xmlGenericErrorFunc) spliteParsingError;
    xmlGenericErrorFunc schemaError =
	(xmlGenericErrorFunc) spliteSchemaValidationError;

    spliteResetXmlErrors (cache);

    *result = NULL;
    *size = 0;
    if (parsing_errors)
	*parsing_errors = NULL;
    if (schema_validation_errors)
	*schema_validation_errors = NULL;
    if (xml == NULL)
	return;

    xmlSetGenericErrorFunc (NULL, silentError);

    if (schemaURI != NULL)
      {
	  if (splite_xmlSchemaCacheFind
	      (cache, schemaURI, &schema_doc, &parser_ctxt, &schema))
	      ;
	  else
	    {
		/* preparing the Schema */
		xmlSetGenericErrorFunc (cache, schemaError);
		schema_doc = xmlReadFile ((const char *) schemaURI, NULL, 0);
		if (schema_doc == NULL)
		  {
		      spatialite_e ("unable to load the Schema\n");
		      if (schema_validation_errors)
			  *schema_validation_errors =
			      schemaValidationBuf->Buffer;
		      xmlSetGenericErrorFunc ((void *) stderr, NULL);
		      return;
		  }
		parser_ctxt = xmlSchemaNewDocParserCtxt (schema_doc);
		if (parser_ctxt == NULL)
		  {
		      spatialite_e ("unable to prepare the Schema Context\n");
		      xmlFreeDoc (schema_doc);
		      if (schema_validation_errors)
			  *schema_validation_errors =
			      schemaValidationBuf->Buffer;
		      xmlSetGenericErrorFunc ((void *) stderr, NULL);
		      return;
		  }
		schema = xmlSchemaParse (parser_ctxt);
		if (schema == NULL)
		  {
		      spatialite_e ("invalid Schema\n");
		      xmlFreeDoc (schema_doc);
		      if (schema_validation_errors)
			  *schema_validation_errors =
			      schemaValidationBuf->Buffer;
		      xmlSetGenericErrorFunc ((void *) stderr, NULL);
		      return;
		  }
		splite_xmlSchemaCacheInsert (cache, schemaURI, schema_doc,
					     parser_ctxt, schema);
	    }
      }

/* testing if the XMLDocument is well-formed */
    xmlSetGenericErrorFunc (cache, parsingError);
    xml_doc =
	xmlReadMemory ((const char *) xml, xml_len, "noname.xml", NULL, 0);
    if (xml_doc == NULL)
      {
	  /* parsing error; not a well-formed XML */
	  spatialite_e ("XML parsing error\n");
	  if (parsing_errors)
	      *parsing_errors = parsingBuf->Buffer;
	  xmlSetGenericErrorFunc ((void *) stderr, NULL);
	  return;
      }
    if (parsing_errors)
	*parsing_errors = parsingBuf->Buffer;

    if (schemaURI != NULL)
      {
	  /* Schema validation */
	  xmlSetGenericErrorFunc (cache, schemaError);
	  valid_ctxt = xmlSchemaNewValidCtxt (schema);
	  if (valid_ctxt == NULL)
	    {
		spatialite_e ("unable to prepare a validation context\n");
		xmlFreeDoc (xml_doc);
		if (schema_validation_errors)
		    *schema_validation_errors = schemaValidationBuf->Buffer;
		xmlSetGenericErrorFunc ((void *) stderr, NULL);
		return;
	    }
	  if (xmlSchemaValidateDoc (valid_ctxt, xml_doc) != 0)
	    {
		spatialite_e ("Schema validation failed\n");
		xmlSchemaFreeValidCtxt (valid_ctxt);
		xmlFreeDoc (xml_doc);
		if (schema_validation_errors)
		    *schema_validation_errors = schemaValidationBuf->Buffer;
		xmlSetGenericErrorFunc ((void *) stderr, NULL);
		return;
	    }
	  xmlSchemaFreeValidCtxt (valid_ctxt);
      }

/* testing for special cases: ISO Metadata, SLD/SE Styles and SVG */
    sniff_payload (xml_doc, &is_iso_metadata, &is_sld_se_style, &is_svg);
    if (is_iso_metadata)
	retrieve_iso_identifiers (cache, xml_doc, &fileIdentifier,
				  &parentIdentifier, &title, &abstract,
				  &geometry, &geometry_len);
    xmlFreeDoc (xml_doc);

    if (compressed)
      {
	  /* compressing the XML payload */
	  uLong zLen = compressBound (xml_len);
	  zip_buf = malloc (zLen);
	  if (compress (zip_buf, &zLen, (const Bytef *) xml, (uLong) xml_len) !=
	      Z_OK)
	    {
		/* compression error */
		spatialite_e ("XmlBLOB DEFLATE compress error\n");
		free (zip_buf);
		xmlSetGenericErrorFunc ((void *) stderr, NULL);
		return;
	    }
	  zip_len = (int) zLen;
      }
    else
	zip_len = xml_len;

/* reporting errors */
    if (parsing_errors)
	*parsing_errors = parsingBuf->Buffer;
    if (schema_validation_errors)
	*schema_validation_errors = schemaValidationBuf->Buffer;

/* computing the XmlBLOB size */
    len = 36;			/* fixed header-footer size */
    if (schemaURI)
	uri_len = strlen ((const char *) schemaURI);
    if (fileIdentifier)
	fileid_len = strlen ((const char *) fileIdentifier);
    if (parentIdentifier)
	parentid_len = strlen ((const char *) parentIdentifier);
    if (title)
	title_len = strlen ((const char *) title);
    if (abstract)
	abstract_len = strlen ((const char *) abstract);
    len += zip_len;
    len += uri_len;
    len += fileid_len;
    len += parentid_len;
    len += title_len;
    len += abstract_len;
    len += geometry_len;
    buf = malloc (len);
    *buf = GAIA_XML_START;	/* START signature */
    flags |= GAIA_XML_LITTLE_ENDIAN;
    if (compressed)
	flags |= GAIA_XML_COMPRESSED;
    if (schemaURI != NULL)
	flags |= GAIA_XML_VALIDATED;
    if (is_iso_metadata)
	flags |= GAIA_XML_ISO_METADATA;
    if (is_sld_se_style)
	flags |= GAIA_XML_SLD_SE_STYLE;
    if (is_svg)
	flags |= GAIA_XML_SVG;
    *(buf + 1) = flags;		/* XmlBLOB flags */
    *(buf + 2) = GAIA_XML_HEADER;	/* HEADER signature */
    gaiaExport32 (buf + 3, xml_len, 1, endian_arch);	/* the uncompressed XMLDocument size */
    gaiaExport32 (buf + 7, zip_len, 1, endian_arch);	/* the compressed XMLDocument size */
    gaiaExport16 (buf + 11, uri_len, 1, endian_arch);	/* the SchemaURI length in bytes */
    *(buf + 13) = GAIA_XML_SCHEMA;	/* SCHEMA signature */
    ptr = buf + 14;
    if (schemaURI)
      {
	  /* the SchemaURI */
	  memcpy (ptr, schemaURI, uri_len);
	  ptr += uri_len;
      }
    gaiaExport16 (ptr, fileid_len, 1, endian_arch);	/* the FileIdentifier length in bytes */
    ptr += 2;
    *ptr = GAIA_XML_FILEID;	/* FileIdentifier signature */
    ptr++;
    if (fileIdentifier)
      {
	  /* the FileIdentifier */
	  memcpy (ptr, fileIdentifier, fileid_len);
	  free (fileIdentifier);
	  ptr += fileid_len;
      }
    gaiaExport16 (ptr, parentid_len, 1, endian_arch);	/* the ParentIdentifier length in bytes */
    ptr += 2;
    *ptr = GAIA_XML_PARENTID;	/* ParentIdentifier signature */
    ptr++;
    if (parentIdentifier)
      {
	  /* the ParentIdentifier */
	  memcpy (ptr, parentIdentifier, parentid_len);
	  free (parentIdentifier);
	  ptr += parentid_len;
      }
    gaiaExport16 (ptr, title_len, 1, endian_arch);	/* the Title length in bytes */
    ptr += 2;
    *ptr = GAIA_XML_TITLE;	/* Title signature */
    ptr++;
    if (title)
      {
	  /* the Title */
	  memcpy (ptr, title, title_len);
	  free (title);
	  ptr += title_len;
      }
    gaiaExport16 (ptr, abstract_len, 1, endian_arch);	/* the Abstract length in bytes */
    ptr += 2;
    *ptr = GAIA_XML_ABSTRACT;	/* Abstract signature */
    ptr++;
    if (abstract)
      {
	  /* the Abstract */
	  memcpy (ptr, abstract, abstract_len);
	  free (abstract);
	  ptr += abstract_len;
      }
    gaiaExport16 (ptr, geometry_len, 1, endian_arch);	/* the Geometry length in bytes */
    ptr += 2;
    *ptr = GAIA_XML_GEOMETRY;	/* Geometry signature */
    ptr++;
    if (geometry)
      {
	  /* the Geometry */
	  memcpy (ptr, geometry, geometry_len);
	  free (geometry);
	  ptr += geometry_len;
      }
    *ptr = GAIA_XML_PAYLOAD;	/* PAYLOAD signature */
    ptr++;
    if (compressed)
      {
	  /* the compressed XML payload */
	  memcpy (ptr, zip_buf, zip_len);
	  free (zip_buf);
	  ptr += zip_len;
      }
    else
      {
	  /* the uncompressed XML payload */
	  memcpy (ptr, xml, xml_len);
	  ptr += xml_len;
      }
    *ptr = GAIA_XML_CRC32;	/* CRC32 signature */
    ptr++;
/* computing the CRC32 */
    crc = crc32 (0L, buf, ptr - buf);
    gaiaExportU32 (ptr, crc, 1, endian_arch);	/* the CRC32 */
    ptr += 4;
    *ptr = GAIA_XML_END;	/* END signature */

    *result = buf;
    *size = len;
    xmlSetGenericErrorFunc ((void *) stderr, NULL);
}

GAIAGEO_DECLARE void
gaiaXmlBlobCompression (const unsigned char *blob,
			int in_size, int compressed,
			unsigned char **result, int *out_size)
{
/* Return another XmlBLOB buffer compressed / uncompressed */
    int in_compressed = 0;
    int little_endian = 0;
    unsigned char flag;
    int in_xml_len;
    int in_zip_len;
    short uri_len;
    short fileid_len;
    short parentid_len;
    short title_len;
    short abstract_len;
    short geometry_len;
    int out_xml_len;
    int out_zip_len;
    uLong crc;
    Bytef *zip_buf;
    int len;
    char *schemaURI;
    char *fileIdentifier;
    char *parentIdentifier;
    char *title;
    char *abstract;
    unsigned char *geometry;
    int is_iso_metadata = 0;
    int is_sld_se_style = 0;
    int is_svg = 0;
    unsigned char *xml;
    unsigned char *buf;
    unsigned char *ptr;
    unsigned char flags;
    int endian_arch = gaiaEndianArch ();

    *result = NULL;
    *out_size = 0;
/* validity check */
    if (!gaiaIsValidXmlBlob (blob, in_size))
	return;			/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    if ((flag & GAIA_XML_COMPRESSED) == GAIA_XML_COMPRESSED)
	in_compressed = 1;
    if ((flag & GAIA_XML_ISO_METADATA) == GAIA_XML_ISO_METADATA)
	is_iso_metadata = 1;
    if ((flag & GAIA_XML_SLD_SE_STYLE) == GAIA_XML_SLD_SE_STYLE)
	is_sld_se_style = 1;
    if ((flag & GAIA_XML_SVG) == GAIA_XML_SVG)
	is_svg = 1;
    in_xml_len = gaiaImport32 (blob + 3, little_endian, endian_arch);
    in_zip_len = gaiaImport32 (blob + 7, little_endian, endian_arch);
    uri_len = gaiaImport16 (blob + 11, little_endian, endian_arch);
    ptr = (unsigned char *) blob + 14;
    if (uri_len)
      {
	  schemaURI = (char *) ptr;
	  ptr += uri_len;
      }
    else
      {
	  schemaURI = NULL;
      }
    fileid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3;
    if (fileid_len)
      {
	  fileIdentifier = (char *) ptr;
	  ptr += fileid_len;
      }
    else
      {
	  fileIdentifier = NULL;
      }
    parentid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3;
    if (parentid_len)
      {
	  parentIdentifier = (char *) ptr;
	  ptr += parentid_len;
      }
    else
      {
	  parentIdentifier = NULL;
      }
    title_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3;
    if (title_len)
      {
	  title = (char *) ptr;
	  ptr += title_len;
      }
    else
      {
	  title = NULL;
      }
    abstract_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3;
    if (abstract_len)
      {
	  abstract = (char *) ptr;
	  ptr += abstract_len;
      }
    else
      {
	  abstract = NULL;
      }
    geometry_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3;
    if (geometry_len)
      {
	  geometry = (unsigned char *) ptr;
	  ptr += geometry_len;
      }
    else
      {
	  geometry = NULL;
      }
    ptr++;

    if (in_compressed == compressed)
      {
	  /* unchanged compression */
	  out_xml_len = in_xml_len;
	  out_zip_len = in_zip_len;
	  zip_buf = (unsigned char *) ptr;
      }
    else if (compressed)
      {
	  /* compressing the XML payload */
	  uLong zLen;
	  out_xml_len = in_xml_len;
	  zLen = compressBound (out_xml_len);
	  xml = (unsigned char *) ptr;
	  zip_buf = malloc (zLen);
	  if (compress
	      (zip_buf, &zLen, (const Bytef *) xml,
	       (uLong) out_xml_len) != Z_OK)
	    {
		/* compression error */
		spatialite_e ("XmlBLOB DEFLATE compress error\n");
		free (zip_buf);
		return;
	    }
	  out_zip_len = (int) zLen;
      }
    else
      {
	  /* unzipping the XML payload */
	  uLong refLen = in_xml_len;
	  const Bytef *in = ptr;
	  xml = malloc (in_xml_len + 1);
	  if (uncompress (xml, &refLen, in, in_zip_len) != Z_OK)
	    {
		/* uncompress error */
		spatialite_e ("XmlBLOB DEFLATE uncompress error\n");
		free (xml);
		return;
	    }
	  *(xml + in_xml_len) = '\0';
	  out_xml_len = in_xml_len;
	  out_zip_len = out_xml_len;
      }

/* computing the XmlBLOB size */
    len = 36;			/* fixed header-footer size */
    len += out_zip_len;
    len += uri_len;
    len += fileid_len;
    len += parentid_len;
    len += title_len;
    len += abstract_len;
    len += geometry_len;
    buf = malloc (len);
    *buf = GAIA_XML_START;	/* START signature */
    flags = 0x00;
    flags |= GAIA_XML_LITTLE_ENDIAN;
    if (compressed)
	flags |= GAIA_XML_COMPRESSED;
    if (schemaURI != NULL)
	flags |= GAIA_XML_VALIDATED;
    if (is_iso_metadata)
	flags |= GAIA_XML_ISO_METADATA;
    if (is_sld_se_style)
	flags |= GAIA_XML_SLD_SE_STYLE;
    if (is_svg)
	flags |= GAIA_XML_SVG;
    *(buf + 1) = flags;		/* XmlBLOB flags */
    *(buf + 2) = GAIA_XML_HEADER;	/* HEADER signature */
    gaiaExport32 (buf + 3, out_xml_len, 1, endian_arch);	/* the uncompressed XMLDocument size */
    gaiaExport32 (buf + 7, out_zip_len, 1, endian_arch);	/* the compressed XMLDocument size */
    gaiaExport16 (buf + 11, uri_len, 1, endian_arch);	/* the SchemaURI length in bytes */
    *(buf + 13) = GAIA_XML_SCHEMA;	/* SCHEMA signature */
    ptr = buf + 14;
    if (schemaURI)
      {
	  /* the SchemaURI */
	  memcpy (ptr, schemaURI, uri_len);
	  ptr += uri_len;
      }
    gaiaExport16 (ptr, fileid_len, 1, endian_arch);	/* the FileIdentifier length in bytes */
    ptr += 2;
    *ptr = GAIA_XML_FILEID;	/* FileIdentifier signature */
    ptr++;
    if (fileIdentifier)
      {
	  /* the FileIdentifier */
	  memcpy (ptr, fileIdentifier, fileid_len);
	  ptr += fileid_len;
      }
    gaiaExport16 (ptr, parentid_len, 1, endian_arch);	/* the ParentIdentifier length in bytes */
    ptr += 2;
    *ptr = GAIA_XML_PARENTID;	/* ParentIdentifier signature */
    ptr++;
    if (parentIdentifier)
      {
	  /* the ParentIdentifier */
	  memcpy (ptr, parentIdentifier, parentid_len);
	  ptr += parentid_len;
      }
    gaiaExport16 (ptr, title_len, 1, endian_arch);	/* the Title length in bytes */
    ptr += 2;
    *ptr = GAIA_XML_TITLE;	/* Title signature */
    ptr++;
    if (title)
      {
	  /* the Title */
	  memcpy (ptr, title, title_len);
	  ptr += title_len;
      }
    gaiaExport16 (ptr, abstract_len, 1, endian_arch);	/* the Abstract length in bytes */
    ptr += 2;
    *ptr = GAIA_XML_ABSTRACT;	/* Abstract signature */
    ptr++;
    if (abstract)
      {
	  /* the Abstract */
	  memcpy (ptr, abstract, abstract_len);
	  ptr += abstract_len;
      }
    gaiaExport16 (ptr, geometry_len, 1, endian_arch);	/* the Geometry length in bytes */
    ptr += 2;
    *ptr = GAIA_XML_GEOMETRY;	/* Geometry signature */
    ptr++;
    if (geometry)
      {
	  /* the Geometry */
	  memcpy (ptr, geometry, geometry_len);
	  ptr += geometry_len;
      }

    *ptr = GAIA_XML_PAYLOAD;	/* PAYLOAD signature */
    ptr++;
    if (in_compressed == compressed)
      {
	  /* the unchanged XML payload */
	  memcpy (ptr, zip_buf, out_zip_len);
	  ptr += out_zip_len;
      }
    else if (compressed)
      {
	  /* the compressed XML payload */
	  memcpy (ptr, zip_buf, out_zip_len);
	  free (zip_buf);
	  ptr += out_zip_len;
      }
    else
      {
	  /* the uncompressed XML payload */
	  memcpy (ptr, xml, out_xml_len);
	  free (xml);
	  ptr += out_xml_len;
      }
    *ptr = GAIA_XML_CRC32;	/* CRC32 signature */
    ptr++;
/* computing the CRC32 */
    crc = crc32 (0L, buf, ptr - buf);
    gaiaExportU32 (ptr, crc, 1, endian_arch);	/* the CRC32 */
    ptr += 4;
    *ptr = GAIA_XML_END;	/* END signature */

    *result = buf;
    *out_size = len;
}

GAIAGEO_DECLARE int
gaiaIsValidXmlBlob (const unsigned char *blob, int blob_size)
{
/* Checks if a BLOB actually is a valid XmlBLOB buffer */
    int little_endian = 0;
    unsigned char flag;
    const unsigned char *ptr;
    short uri_len;
    short fileid_len;
    short parentid_len;
    short title_len;
    short abstract_len;
    short geometry_len;
    uLong crc;
    uLong refCrc;
    int endian_arch = gaiaEndianArch ();

/* validity check */
    if (blob_size < 36)
	return 0;		/* cannot be an XmlBLOB */
    if (*blob != GAIA_XML_START)
	return 0;		/* failed to recognize START signature */
    if (*(blob + (blob_size - 1)) != GAIA_XML_END)
	return 0;		/* failed to recognize END signature */
    if (*(blob + (blob_size - 6)) != GAIA_XML_CRC32)
	return 0;		/* failed to recognize CRC32 signature */
    if (*(blob + 2) != GAIA_XML_HEADER)
	return 0;		/* failed to recognize HEADER signature */
    if (*(blob + 13) != GAIA_XML_SCHEMA)
	return 0;		/* failed to recognize SCHEMA signature */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    ptr = blob + 11;
    uri_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 2;
    if (*ptr != GAIA_XML_SCHEMA)
	return 0;
    ptr++;
    ptr += uri_len;
    fileid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 2;
    if (*ptr != GAIA_XML_FILEID)
	return 0;
    ptr++;
    ptr += fileid_len;
    parentid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 2;
    if (*ptr != GAIA_XML_PARENTID)
	return 0;
    ptr++;
    ptr += parentid_len;
    title_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 2;
    if (*ptr != GAIA_XML_TITLE)
	return 0;
    ptr++;
    ptr += title_len;
    abstract_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 2;
    if (*ptr != GAIA_XML_ABSTRACT)
	return 0;
    ptr++;
    ptr += abstract_len;
    geometry_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 2;
    if (*ptr != GAIA_XML_GEOMETRY)
	return 0;
    ptr++;
    ptr += geometry_len;
    if (*ptr != GAIA_XML_PAYLOAD)
	return 0;

/* verifying the CRC32 */
    crc = crc32 (0L, blob, blob_size - 5);
    refCrc = gaiaImportU32 (blob + blob_size - 5, little_endian, endian_arch);
    if (crc != refCrc)
	return 0;

    return 1;
}

GAIAGEO_DECLARE char *
gaiaXmlTextFromBlob (const unsigned char *blob, int blob_size, int indent)
{
/* attempting to extract an XMLDocument from within an XmlBLOB buffer */
    int compressed = 0;
    int little_endian = 0;
    unsigned char flag;
    const unsigned char *ptr;
    int xml_len;
    int zip_len;
    short uri_len;
    short fileid_len;
    short parentid_len;
    short title_len;
    short abstract_len;
    short geometry_len;
    unsigned char *xml;
    xmlDocPtr xml_doc;
    xmlChar *out;
    int out_len;
    char *encoding = NULL;
    void *cvt;
    char *utf8;
    int err;
    int endian_arch = gaiaEndianArch ();
    xmlGenericErrorFunc silentError = (xmlGenericErrorFunc) spliteSilentError;

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return NULL;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    if ((flag & GAIA_XML_COMPRESSED) == GAIA_XML_COMPRESSED)
	compressed = 1;
    xml_len = gaiaImport32 (blob + 3, little_endian, endian_arch);
    zip_len = gaiaImport32 (blob + 7, little_endian, endian_arch);
    ptr = blob + 11;
    uri_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + uri_len;
    fileid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + fileid_len;
    parentid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + parentid_len;
    title_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + title_len;
    abstract_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + abstract_len;
    geometry_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + geometry_len;
    ptr++;

    if (compressed)
      {
	  /* unzipping the XML payload */
	  uLong refLen = xml_len;
	  const Bytef *in = ptr;
	  xml = malloc (xml_len + 1);
	  if (uncompress (xml, &refLen, in, zip_len) != Z_OK)
	    {
		/* uncompress error */
		spatialite_e ("XmlBLOB DEFLATE uncompress error\n");
		free (xml);
		return NULL;
	    }
	  *(xml + xml_len) = '\0';
      }
    else
      {
	  /* just copying the uncompressed XML payload */
	  xml = malloc (xml_len + 1);
	  memcpy (xml, ptr, xml_len);
	  *(xml + xml_len) = '\0';
      }
/* retrieving the XMLDocument encoding */
    xmlSetGenericErrorFunc (NULL, silentError);
    xml_doc =
	xmlReadMemory ((const char *) xml, xml_len, "noname.xml", NULL, 0);
    if (xml_doc == NULL)
      {
	  /* parsing error; not a well-formed XML */
	  xmlSetGenericErrorFunc ((void *) stderr, NULL);
	  return NULL;
      }
    if (xml_doc->encoding)
      {
	  /* using the internal character enconding */
	  int enclen = (int) strlen ((const char *) xml_doc->encoding);
	  encoding = malloc (enclen + 1);
	  strcpy (encoding, (const char *) (xml_doc->encoding));
      }
    else
      {
	  /* no declared encoding: defaulting to UTF-8 */
	  encoding = malloc (6);
	  strcpy (encoding, "UTF-8");
      }

    if (!indent)
      {
	  /* just returning the XMLDocument "as is" */
	  xmlFreeDoc (xml_doc);
	  cvt = gaiaCreateUTF8Converter (encoding);
	  free (encoding);
	  if (cvt == NULL)
	    {
		xmlSetGenericErrorFunc ((void *) stderr, NULL);
		return NULL;
	    }
	  utf8 = gaiaConvertToUTF8 (cvt, (const char *) xml, xml_len, &err);
	  free (xml);
	  gaiaFreeUTF8Converter (cvt);
	  if (utf8 && !err)
	    {
		xmlSetGenericErrorFunc ((void *) stderr, NULL);
		return utf8;
	    }
	  if (utf8)
	      free (utf8);
	  xmlSetGenericErrorFunc ((void *) stderr, NULL);
	  return NULL;
      }

/* properly indenting the XMLDocument */
    xmlDocDumpFormatMemory (xml_doc, &out, &out_len, 1);
    free (xml);
    xmlFreeDoc (xml_doc);
    cvt = gaiaCreateUTF8Converter (encoding);
    free (encoding);
    if (cvt == NULL)
      {
	  xmlSetGenericErrorFunc ((void *) stderr, NULL);
	  return NULL;
      }
    utf8 = gaiaConvertToUTF8 (cvt, (const char *) out, out_len, &err);
    gaiaFreeUTF8Converter (cvt);
    free (out);
    if (utf8 && !err)
      {
	  xmlSetGenericErrorFunc ((void *) stderr, NULL);
	  return utf8;
      }
    if (utf8)
	free (utf8);
    xmlSetGenericErrorFunc ((void *) stderr, NULL);
    return NULL;
}

GAIAGEO_DECLARE void
gaiaXmlFromBlob (const unsigned char *blob, int blob_size, int indent,
		 unsigned char **result, int *res_size)
{
/* attempting to extract an XMLDocument from within an XmlBLOB buffer */
    int compressed = 0;
    int little_endian = 0;
    unsigned char flag;
    const unsigned char *ptr;
    int xml_len;
    int zip_len;
    short uri_len;
    short fileid_len;
    short parentid_len;
    short title_len;
    short abstract_len;
    short geometry_len;
    unsigned char *xml;
    xmlDocPtr xml_doc;
    xmlChar *out;
    int out_len;
    int endian_arch = gaiaEndianArch ();
    xmlGenericErrorFunc silentError = (xmlGenericErrorFunc) spliteSilentError;
    *result = NULL;
    *res_size = 0;

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return;			/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    if ((flag & GAIA_XML_COMPRESSED) == GAIA_XML_COMPRESSED)
	compressed = 1;
    xml_len = gaiaImport32 (blob + 3, little_endian, endian_arch);
    zip_len = gaiaImport32 (blob + 7, little_endian, endian_arch);
    ptr = blob + 11;
    uri_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + uri_len;
    fileid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + fileid_len;
    parentid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + parentid_len;
    title_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + title_len;
    abstract_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + abstract_len;
    geometry_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + geometry_len;
    ptr++;

    if (compressed)
      {
	  /* unzipping the XML payload */
	  uLong refLen = xml_len;
	  const Bytef *in = ptr;
	  xml = malloc (xml_len + 1);
	  if (uncompress (xml, &refLen, in, zip_len) != Z_OK)
	    {
		/* uncompress error */
		spatialite_e ("XmlBLOB DEFLATE uncompress error\n");
		free (xml);
		return;
	    }
	  *(xml + xml_len) = '\0';
      }
    else
      {
	  /* just copying the uncompressed XML payload */
	  xml = malloc (xml_len + 1);
	  memcpy (xml, ptr, xml_len);
	  *(xml + xml_len) = '\0';
      }
    if (!indent)
      {
	  /* just returning the XMLDocument "as is" */
	  *result = xml;
	  *res_size = xml_len;
	  return;
      }

/* properly indenting the XMLDocument */
    xmlSetGenericErrorFunc (NULL, silentError);
    xml_doc =
	xmlReadMemory ((const char *) xml, xml_len, "noname.xml", NULL, 0);
    if (xml_doc == NULL)
      {
	  /* parsing error; not a well-formed XML */
	  *result = xml;
	  *res_size = xml_len;
	  xmlSetGenericErrorFunc ((void *) stderr, NULL);
	  return;
      }
    xmlDocDumpFormatMemory (xml_doc, &out, &out_len, 1);
    free (xml);
    xmlFreeDoc (xml_doc);
    *result = out;
    *res_size = out_len;
    xmlSetGenericErrorFunc ((void *) stderr, NULL);
}

GAIAGEO_DECLARE int
gaiaIsCompressedXmlBlob (const unsigned char *blob, int blob_size)
{
/* Checks if a valid XmlBLOB buffer is compressed or not */
    int compressed = 0;
    unsigned char flag;

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return -1;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_COMPRESSED) == GAIA_XML_COMPRESSED)
	compressed = 1;
    return compressed;
}

GAIAGEO_DECLARE int
gaiaIsSchemaValidatedXmlBlob (const unsigned char *blob, int blob_size)
{
/* Checks if a valid XmlBLOB buffer has succesfully passed a formal Schema validation or not */
    int validated = 0;
    unsigned char flag;

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return -1;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_VALIDATED) == GAIA_XML_VALIDATED)
	validated = 1;
    return validated;
}

GAIAGEO_DECLARE int
gaiaIsIsoMetadataXmlBlob (const unsigned char *blob, int blob_size)
{
/* Checks if a valid XmlBLOB buffer does actually contains an ISO Metadata or not */
    int iso_metadata = 0;
    unsigned char flag;

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return -1;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_ISO_METADATA) == GAIA_XML_ISO_METADATA)
	iso_metadata = 1;
    return iso_metadata;
}

GAIAGEO_DECLARE int
gaiaIsSldSeStyleXmlBlob (const unsigned char *blob, int blob_size)
{
/* Checks if a valid XmlBLOB buffer does actually contains an SLD/SE Style or not */
    int sld_se_style = 0;
    unsigned char flag;

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return -1;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_SLD_SE_STYLE) == GAIA_XML_SLD_SE_STYLE)
	sld_se_style = 1;
    return sld_se_style;
}

GAIAGEO_DECLARE int
gaiaIsSvgXmlBlob (const unsigned char *blob, int blob_size)
{
/* Checks if a valid XmlBLOB buffer does actually contains an SLD/SE Style or not */
    int svg = 0;
    unsigned char flag;

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return -1;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_SVG) == GAIA_XML_SVG)
	svg = 1;
    return svg;
}

GAIAGEO_DECLARE int
gaiaXmlBlobGetDocumentSize (const unsigned char *blob, int blob_size)
{
/* Return the XMLDocument size (in bytes) from a valid XmlBLOB buffer */
    int little_endian = 0;
    unsigned char flag;
    int xml_len;
    int endian_arch = gaiaEndianArch ();

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return -1;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    xml_len = gaiaImport32 (blob + 3, little_endian, endian_arch);
    return xml_len;
}

GAIAGEO_DECLARE char *
gaiaXmlBlobGetSchemaURI (const unsigned char *blob, int blob_size)
{
/* Return the SchemaURI from a valid XmlBLOB buffer */
    int little_endian = 0;
    unsigned char flag;
    short uri_len;
    char *uri;
    int endian_arch = gaiaEndianArch ();

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return NULL;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    uri_len = gaiaImport16 (blob + 11, little_endian, endian_arch);
    if (!uri_len)
	return NULL;

    uri = malloc (uri_len + 1);
    memcpy (uri, blob + 14, uri_len);
    *(uri + uri_len) = '\0';
    return uri;
}

GAIAGEO_DECLARE char *
gaiaXmlGetInternalSchemaURI (void *p_cache, const char *xml, int xml_len)
{
/* Return the internally defined SchemaURI from a valid XmlDocument */
    xmlDocPtr xml_doc;
    char *uri = NULL;
    xmlXPathContextPtr xpathCtx;
    xmlXPathObjectPtr xpathObj;
    xmlGenericErrorFunc silentError = (xmlGenericErrorFunc) spliteSilentError;

/* retrieving the XMLDocument internal SchemaURI (if any) */
    xmlSetGenericErrorFunc (NULL, silentError);
    xml_doc =
	xmlReadMemory ((const char *) xml, xml_len, "noname.xml", NULL, 0);
    if (xml_doc == NULL)
      {
	  /* parsing error; not a well-formed XML */
	  xmlSetGenericErrorFunc ((void *) stderr, NULL);
	  return NULL;
      }

    if (vxpath_eval_expr
	(p_cache, xml_doc, "/*/@xsi:schemaLocation", &xpathCtx, &xpathObj))
      {
	  /* attempting first to extract xsi:schemaLocation */
	  xmlNodeSetPtr nodeset = xpathObj->nodesetval;
	  xmlNodePtr node;
	  int num_nodes = (nodeset) ? nodeset->nodeNr : 0;
	  if (num_nodes == 1)
	    {
		node = nodeset->nodeTab[0];
		if (node->type == XML_ATTRIBUTE_NODE)
		  {
		      if (node->children != NULL)
			{
			    if (node->children->content != NULL)
			      {
				  const char *str =
				      (const char *) (node->children->content);
				  const char *ptr = str;
				  int i;
				  int len = strlen (str);
				  for (i = len - 1; i >= 0; i--)
				    {
					if (*(str + i) == ' ')
					  {
					      /* last occurrence of SPACE [namespace/schema separator] */
					      ptr = str + i + 1;
					      break;
					  }
				    }
				  len = strlen (ptr);
				  uri = malloc (len + 1);
				  strcpy (uri, ptr);
			      }
			}
		  }
	    }
	  if (uri != NULL)
	      xmlXPathFreeContext (xpathCtx);
	  xmlXPathFreeObject (xpathObj);
      }
    if (uri == NULL)
      {
	  /* checking for xsi:noNamespaceSchemaLocation */
	  if (vxpath_eval_expr
	      (p_cache, xml_doc, "/*/@xsi:noNamespaceSchemaLocation", &xpathCtx,
	       &xpathObj))
	    {
		xmlNodeSetPtr nodeset = xpathObj->nodesetval;
		xmlNodePtr node;
		int num_nodes = (nodeset) ? nodeset->nodeNr : 0;
		if (num_nodes == 1)
		  {
		      node = nodeset->nodeTab[0];
		      if (node->type == XML_ATTRIBUTE_NODE)
			{
			    if (node->children != NULL)
			      {
				  if (node->children->content != NULL)
				    {
					int len =
					    strlen ((const char *)
						    node->children->content);
					uri = malloc (len + 1);
					strcpy (uri,
						(const char *) node->children->
						content);
				    }
			      }
			}
		  }
		xmlXPathFreeContext (xpathCtx);
		xmlXPathFreeObject (xpathObj);
	    }
      }

    xmlFreeDoc (xml_doc);
    xmlSetGenericErrorFunc ((void *) stderr, NULL);
    return uri;
}

GAIAGEO_DECLARE char *
gaiaXmlBlobGetFileId (const unsigned char *blob, int blob_size)
{
/* Return the FileIdentifier from a valid XmlBLOB buffer */
    int little_endian = 0;
    unsigned char flag;
    const unsigned char *ptr;
    short uri_len;
    short fileid_len;
    char *file_identifier;
    int endian_arch = gaiaEndianArch ();

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return NULL;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    ptr = blob + 11;
    uri_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + uri_len;
    fileid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    if (!fileid_len)
	return NULL;
    ptr += 3;

    file_identifier = malloc (fileid_len + 1);
    memcpy (file_identifier, ptr, fileid_len);
    *(file_identifier + fileid_len) = '\0';
    return file_identifier;
}

GAIAGEO_DECLARE char *
gaiaXmlBlobGetParentId (const unsigned char *blob, int blob_size)
{
/* Return the ParentIdentifier from a valid XmlBLOB buffer */
    int little_endian = 0;
    unsigned char flag;
    const unsigned char *ptr;
    short uri_len;
    short fileid_len;
    short parentid_len;
    char *parent_identifier;
    int endian_arch = gaiaEndianArch ();

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return NULL;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    ptr = blob + 11;
    uri_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + uri_len;
    fileid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + fileid_len;
    parentid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    if (!parentid_len)
	return NULL;
    ptr += 3;

    parent_identifier = malloc (fileid_len + 1);
    memcpy (parent_identifier, ptr, parentid_len);
    *(parent_identifier + parentid_len) = '\0';
    return parent_identifier;
}

GAIAGEO_DECLARE char *
gaiaXmlBlobGetTitle (const unsigned char *blob, int blob_size)
{
/* Return the Title from a valid XmlBLOB buffer */
    int little_endian = 0;
    unsigned char flag;
    const unsigned char *ptr;
    short uri_len;
    short fileid_len;
    short parentid_len;
    short title_len;
    char *title;
    int endian_arch = gaiaEndianArch ();

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return NULL;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    ptr = blob + 11;
    uri_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + uri_len;
    fileid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + fileid_len;
    parentid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + parentid_len;
    title_len = gaiaImport16 (ptr, little_endian, endian_arch);
    if (!title_len)
	return NULL;
    ptr += 3;

    title = malloc (title_len + 1);
    memcpy (title, ptr, title_len);
    *(title + title_len) = '\0';
    return title;
}

GAIAGEO_DECLARE char *
gaiaXmlBlobGetAbstract (const unsigned char *blob, int blob_size)
{
/* Return the Abstract from a valid XmlBLOB buffer */
    int little_endian = 0;
    unsigned char flag;
    const unsigned char *ptr;
    short uri_len;
    short fileid_len;
    short parentid_len;
    short title_len;
    short abstract_len;
    char *abstract;
    int endian_arch = gaiaEndianArch ();

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return NULL;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    ptr = blob + 11;
    uri_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + uri_len;
    fileid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + fileid_len;
    parentid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + parentid_len;
    title_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + title_len;
    abstract_len = gaiaImport16 (ptr, little_endian, endian_arch);
    if (!abstract_len)
	return NULL;
    ptr += 3;

    abstract = malloc (abstract_len + 1);
    memcpy (abstract, ptr, abstract_len);
    *(abstract + abstract_len) = '\0';
    return abstract;
}

GAIAGEO_DECLARE void
gaiaXmlBlobGetGeometry (const unsigned char *blob, int blob_size,
			unsigned char **blob_geom, int *geom_size)
{
/* Return the Geometry from a valid XmlBLOB buffer */
    int little_endian = 0;
    unsigned char flag;
    const unsigned char *ptr;
    short uri_len;
    short fileid_len;
    short parentid_len;
    short title_len;
    short abstract_len;
    short geometry_len;
    char *geometry;
    int endian_arch = gaiaEndianArch ();

    *blob_geom = NULL;
    *geom_size = 0;

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return;			/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    ptr = blob + 11;
    uri_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + uri_len;
    fileid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + fileid_len;
    parentid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + parentid_len;
    title_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + title_len;
    abstract_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + abstract_len;
    geometry_len = gaiaImport16 (ptr, little_endian, endian_arch);
    if (!geometry_len)
	return;
    ptr += 3;

    geometry = malloc (geometry_len);
    memcpy (geometry, ptr, geometry_len);
    *blob_geom = geometry;
    *geom_size = geometry_len;
}

GAIAGEO_DECLARE char *
gaiaXmlBlobGetEncoding (const unsigned char *blob, int blob_size)
{
/* Return the Charset Encoding from a valid XmlBLOB buffer */
    int compressed = 0;
    int little_endian = 0;
    unsigned char flag;
    const unsigned char *ptr;
    int xml_len;
    int zip_len;
    short uri_len;
    short fileid_len;
    short parentid_len;
    short title_len;
    short abstract_len;
    short geometry_len;
    unsigned char *xml;
    xmlDocPtr xml_doc;
    char *encoding = NULL;
    int endian_arch = gaiaEndianArch ();
    xmlGenericErrorFunc silentError = (xmlGenericErrorFunc) spliteSilentError;

/* validity check */
    if (!gaiaIsValidXmlBlob (blob, blob_size))
	return NULL;		/* cannot be an XmlBLOB */
    flag = *(blob + 1);
    if ((flag & GAIA_XML_LITTLE_ENDIAN) == GAIA_XML_LITTLE_ENDIAN)
	little_endian = 1;
    if ((flag & GAIA_XML_COMPRESSED) == GAIA_XML_COMPRESSED)
	compressed = 1;
    xml_len = gaiaImport32 (blob + 3, little_endian, endian_arch);
    zip_len = gaiaImport32 (blob + 7, little_endian, endian_arch);
    ptr = blob + 11;
    uri_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + uri_len;
    fileid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + fileid_len;
    parentid_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + parentid_len;
    title_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + title_len;
    abstract_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + abstract_len;
    geometry_len = gaiaImport16 (ptr, little_endian, endian_arch);
    ptr += 3 + geometry_len;
    ptr++;
    if (compressed)
      {
	  /* unzipping the XML payload */
	  uLong refLen = xml_len;
	  const Bytef *in = ptr;
	  xml = malloc (xml_len + 1);
	  if (uncompress (xml, &refLen, in, zip_len) != Z_OK)
	    {
		/* uncompress error */
		spatialite_e ("XmlBLOB DEFLATE uncompress error\n");
		free (xml);
		return NULL;
	    }
	  *(xml + xml_len) = '\0';
      }
    else
      {
	  /* just copying the uncompressed XML payload */
	  xml = malloc (xml_len + 1);
	  memcpy (xml, ptr, xml_len);
	  *(xml + xml_len) = '\0';
      }
/* retrieving the XMLDocument encoding */
    xmlSetGenericErrorFunc (NULL, silentError);
    xml_doc =
	xmlReadMemory ((const char *) xml, xml_len, "noname.xml", NULL, 0);
    if (xml_doc == NULL)
      {
	  /* parsing error; not a well-formed XML */
	  xmlSetGenericErrorFunc ((void *) stderr, NULL);
	  return NULL;
      }
    free (xml);
    if (xml_doc->encoding)
      {
	  /* using the internal character enconding */
	  int enclen = strlen ((const char *) xml_doc->encoding);
	  encoding = malloc (enclen + 1);
	  strcpy (encoding, (const char *) xml_doc->encoding);
	  xmlFreeDoc (xml_doc);
	  xmlSetGenericErrorFunc ((void *) stderr, NULL);
	  return encoding;
      }
    xmlFreeDoc (xml_doc);
    xmlSetGenericErrorFunc ((void *) stderr, NULL);
    return NULL;
}

GAIAGEO_DECLARE char *
gaia_libxml2_version (void)
{
/* return the current LIBXML2 version */
    int len;
    char *version;
    const char *ver = LIBXML_DOTTED_VERSION;
    len = strlen (ver);
    version = malloc (len + 1);
    strcpy (version, ver);
    return version;
}

#endif /* end LIBXML2: supporting XML documents */
