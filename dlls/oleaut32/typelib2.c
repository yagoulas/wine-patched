/*
 *	TYPELIB2
 *
 *	Copyright 2004  Alastair Bridgewater
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * --------------------------------------------------------------------------------------
 *  Known problems:
 *
 *    Badly incomplete.
 *
 *    Only works on little-endian systems.
 *
 */

#include "config.h"
#include "wine/port.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>

#define NONAMELESSUNION
#define NONAMELESSSTRUCT
#include "winerror.h"
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winuser.h"

#include "wine/unicode.h"
#include "objbase.h"
#include "heap.h"
#include "ole2disp.h"
#include "typelib.h"
#include "wine/debug.h"
#include "variant.h"

WINE_DEFAULT_DEBUG_CHANNEL(typelib2);
/* WINE_DEFAULT_DEBUG_CHANNEL(ole); */


/******************************************************************************
 * ICreateTypeLib2 {OLEAUT32}
 *
 * NOTES
 *  The ICreateTypeLib2 interface provides an interface whereby one may create
 *  new type library (.tlb) files.
 *
 *  This interface inherits from ICreateTypeLib, and can be freely cast back
 *  and forth between an ICreateTypeLib and an ICreateTypeLib2 on local clients.
 *  This dispensation applies only to ICreateTypeLib objects obtained on MSFT
 *  format type libraries (those made through CreateTypeLib2).
 *
 * METHODS
 */

/******************************************************************************
 * ICreateTypeInfo2 {OLEAUT32}
 *
 * NOTES
 *  The ICreateTypeInfo2 interface provides an interface whereby one may add
 *  type information to type library (.tlb) files.
 *
 *  This interface inherits from ICreateTypeInfo, and can be freely cast back
 *  and forth between an ICreateTypeInfo and an ICreateTypeInfo2 on local clients.
 *  This dispensation applies only to ICreateTypeInfo objects obtained on MSFT
 *  format type libraries (those made through CreateTypeLib2).
 *
 * METHODS
 */

/*================== Implementation Structures ===================================*/

enum MSFT_segment_index {
    MSFT_SEG_TYPEINFO = 0,  /* type information */
    MSFT_SEG_IMPORTINFO,    /* import information */
    MSFT_SEG_IMPORTFILES,   /* import filenames */
    MSFT_SEG_REFERENCES,    /* references (?) */
    MSFT_SEG_GUIDHASH,      /* hash table for guids? */
    MSFT_SEG_GUID,          /* guid storage */
    MSFT_SEG_NAMEHASH,      /* hash table for names */
    MSFT_SEG_NAME,          /* name storage */
    MSFT_SEG_STRING,        /* string storage */
    MSFT_SEG_TYPEDESC,      /* type descriptions */
    MSFT_SEG_ARRAYDESC,     /* array descriptions */
    MSFT_SEG_CUSTDATA,      /* custom data */
    MSFT_SEG_CUSTDATAGUID,  /* custom data guids */
    MSFT_SEG_UNKNOWN,       /* ??? */
    MSFT_SEG_UNKNOWN2,      /* ??? */
    MSFT_SEG_MAX            /* total number of segments */
};

typedef struct tagMSFT_ImpFile {
    int guid;
    LCID lcid;
    int version;
    char filename[0]; /* preceeded by two bytes of encoded (length << 2) + flags in the low two bits. */
} MSFT_ImpFile;

typedef struct tagICreateTypeLib2Impl
{
    ICOM_VFIELD(ICreateTypeLib2);
    UINT ref;

    WCHAR *filename;

    MSFT_Header typelib_header;
    MSFT_pSeg typelib_segdir[MSFT_SEG_MAX];
    char *typelib_segment_data[MSFT_SEG_MAX];
    int typelib_segment_block_length[MSFT_SEG_MAX];

    INT typelib_typeinfo_offsets[0x200]; /* Hope that's enough. */

    INT *typelib_namehash_segment;
    INT *typelib_guidhash_segment;

    struct tagICreateTypeInfo2Impl *typeinfos;
    struct tagICreateTypeInfo2Impl *last_typeinfo;
} ICreateTypeLib2Impl;

typedef struct tagICreateTypeInfo2Impl
{
    ICOM_VFIELD(ICreateTypeInfo2);
    UINT ref;

    ICreateTypeLib2Impl *typelib;
    MSFT_TypeInfoBase *typeinfo;

    INT *typedata;
    int typedata_allocated;
    int typedata_length;

    int indices[42];
    int names[42];
    int offsets[42];

    int datawidth;

    struct tagICreateTypeInfo2Impl *next_typeinfo;
} ICreateTypeInfo2Impl;

static ULONG WINAPI ICreateTypeLib2_fnRelease(ICreateTypeLib2 *iface);


/*================== Internal functions ===================================*/

/****************************************************************************
 *	ctl2_init_header
 *
 *  Initializes the type library header of a new typelib.
 */
static void ctl2_init_header(
	ICreateTypeLib2Impl *This) /* [I] The typelib to initialize. */
{
    This->typelib_header.magic1 = 0x5446534d;
    This->typelib_header.magic2 = 0x00010002;
    This->typelib_header.posguid = -1;
    This->typelib_header.lcid = 0x0409; /* or do we use the current one? */
    This->typelib_header.lcid2 = 0x0409;
    This->typelib_header.varflags = 0x41;
    This->typelib_header.version = 0;
    This->typelib_header.flags = 0;
    This->typelib_header.nrtypeinfos = 0;
    This->typelib_header.helpstring = -1;
    This->typelib_header.helpstringcontext = 0;
    This->typelib_header.helpcontext = 0;
    This->typelib_header.nametablecount = 0;
    This->typelib_header.nametablechars = 0;
    This->typelib_header.NameOffset = -1;
    This->typelib_header.helpfile = -1;
    This->typelib_header.CustomDataOffset = -1;
    This->typelib_header.res44 = 0x20;
    This->typelib_header.res48 = 0x80;
    This->typelib_header.dispatchpos = -1;
    This->typelib_header.res50 = 0;
}

/****************************************************************************
 *	ctl2_init_segdir
 *
 *  Initializes the segment directory of a new typelib.
 */
static void ctl2_init_segdir(
	ICreateTypeLib2Impl *This) /* [I] The typelib to initialize. */
{
    int i;
    MSFT_pSeg *segdir;

    segdir = &This->typelib_segdir[MSFT_SEG_TYPEINFO];

    for (i = 0; i < 15; i++) {
	segdir[i].offset = -1;
	segdir[i].length = 0;
	segdir[i].res08 = -1;
	segdir[i].res0c = 0x0f;
    }
}

/****************************************************************************
 *	ctl2_find_name
 *
 *  Locates a name in a type library.
 *
 * RETURNS
 *
 *  The offset into the NAME segment of the name, or -1 if not found.
 *
 * NOTES
 *
 *  The name must be encoded as with ctl2_encode_name().
 */
static int ctl2_find_name(
	ICreateTypeLib2Impl *This, /* [I] The typelib to operate against. */
	char *name)                /* [I] The encoded name to find. */
{
    int offset;
    int *namestruct;

    offset = This->typelib_namehash_segment[name[2] & 0x7f];
    while (offset != -1) {
	namestruct = (int *)&This->typelib_segment_data[MSFT_SEG_NAME][offset];

	if (!((namestruct[2] ^ *((int *)name)) & 0xffff00ff)) {
	    /* hash codes and lengths match, final test */
	    if (!strncasecmp(name+4, (void *)(namestruct+3), name[0])) break;
	}

	/* move to next item in hash bucket */
	offset = namestruct[1];
    }

    return offset;
}

/****************************************************************************
 *	ctl2_encode_name
 *
 *  Encodes a name string to a form suitable for storing into a type library
 *  or comparing to a name stored in a type library.
 *
 * RETURNS
 *
 *  The length of the encoded name, including padding and length+hash fields.
 *
 * NOTES
 *
 *  Will throw an exception if name or result are NULL. Is not multithread
 *  safe in the slightest.
 */
static int ctl2_encode_name(
	ICreateTypeLib2Impl *This, /* [I] The typelib to operate against (used for LCID only). */
	WCHAR *name,               /* [I] The name string to encode. */
	char **result)             /* [O] A pointer to a pointer to recieve the encoded name. */
{
    int length;
    static char converted_name[0x104];
    int offset;
    int value;

    length = WideCharToMultiByte(CP_ACP, 0, name, strlenW(name), converted_name+4, 0x100, NULL, NULL);
    converted_name[0] = length & 0xff;

    converted_name[length + 4] = 0;

    converted_name[1] = 0x00;

    value = LHashValOfNameSysA(This->typelib_header.varflags & 0x0f, This->typelib_header.lcid, converted_name + 4);

    converted_name[2] = value;
    converted_name[3] = value >> 8;

    for (offset = (4 - length) & 3; offset; offset--) converted_name[length + offset + 3] = 0x57;

    *result = converted_name;

    return (length + 7) & ~3;
}

/****************************************************************************
 *	ctl2_encode_string
 *
 *  Encodes a string to a form suitable for storing into a type library or
 *  comparing to a string stored in a type library.
 *
 * RETURNS
 *
 *  The length of the encoded string, including padding and length fields.
 *
 * NOTES
 *
 *  Will throw an exception if string or result are NULL. Is not multithread
 *  safe in the slightest.
 */
static int ctl2_encode_string(
	ICreateTypeLib2Impl *This, /* [I] The typelib to operate against (not used?). */
	WCHAR *string,             /* [I] The string to encode. */
	char **result)             /* [O] A pointer to a pointer to recieve the encoded string. */
{
    int length;
    static char converted_string[0x104];
    int offset;

    length = WideCharToMultiByte(CP_ACP, 0, string, strlenW(string), converted_string+2, 0x102, NULL, NULL);
    converted_string[0] = length & 0xff;
    converted_string[1] = (length >> 8) & 0xff;

    for (offset = (4 - (length + 2)) & 3; offset; offset--) converted_string[length + offset + 1] = 0x57;

    *result = converted_string;

    return (length + 5) & ~3;
}

/****************************************************************************
 *	ctl2_alloc_segment
 *
 *  Allocates memory from a segment in a type library.
 *
 * RETURNS
 *
 *  Success: The offset within the segment of the new data area.
 *  Failure: -1 (this is invariably an out of memory condition).
 *
 * BUGS
 *
 *  Does not (yet) handle the case where the allocated segment memory needs to grow.
 */
static int ctl2_alloc_segment(
	ICreateTypeLib2Impl *This,       /* [I] The type library in which to allocate. */
	enum MSFT_segment_index segment, /* [I] The segment in which to allocate. */
	int size,                        /* [I] The amount to allocate. */
	int block_size)                  /* [I] Initial allocation block size, or 0 for default. */
{
    int offset;

    if(!This->typelib_segment_data[segment]) {
	if (!block_size) block_size = 0x2000;

	This->typelib_segment_block_length[segment] = block_size;
	This->typelib_segment_data[segment] = HeapAlloc(GetProcessHeap(), 0, block_size);
	if (!This->typelib_segment_data[segment]) return -1;
	memset(This->typelib_segment_data[segment], 0x57, block_size);
    }

    while ((This->typelib_segdir[segment].length + size) > This->typelib_segment_block_length[segment]) {
	char *block;

	block_size = This->typelib_segment_block_length[segment];
	block = HeapReAlloc(GetProcessHeap(), 0, This->typelib_segment_data[segment], block_size << 1);
	if (!block) return -1;

	if (segment == MSFT_SEG_TYPEINFO) {
	    /* TypeInfos have a direct pointer to their memory space, so we have to fix them up. */
	    ICreateTypeInfo2Impl *typeinfo;

	    for (typeinfo = This->typeinfos; typeinfo; typeinfo = typeinfo->next_typeinfo) {
		typeinfo->typeinfo = (void *)&block[((char *)typeinfo->typeinfo) - This->typelib_segment_data[segment]];
	    }
	}

	memset(block + block_size, 0x57, block_size);
	This->typelib_segment_block_length[segment] = block_size << 1;
	This->typelib_segment_data[segment] = block;
    }

    offset = This->typelib_segdir[segment].length;
    This->typelib_segdir[segment].length += size;

    return offset;
}

/****************************************************************************
 *	ctl2_alloc_typeinfo
 *
 *  Allocates and initializes a typeinfo structure in a type library.
 *
 * RETURNS
 *
 *  Success: The offset of the new typeinfo.
 *  Failure: -1 (this is invariably an out of memory condition).
 */
static int ctl2_alloc_typeinfo(
	ICreateTypeLib2Impl *This, /* [I] The type library to allocate in. */
	int nameoffset)            /* [I] The offset of the name for this typeinfo. */
{
    int offset;
    MSFT_TypeInfoBase *typeinfo;

    offset = ctl2_alloc_segment(This, MSFT_SEG_TYPEINFO, sizeof(MSFT_TypeInfoBase), 0);
    if (offset == -1) return -1;

    This->typelib_typeinfo_offsets[This->typelib_header.nrtypeinfos++] = offset;

    typeinfo = (void *)(This->typelib_segment_data[MSFT_SEG_TYPEINFO] + offset);

    typeinfo->typekind = (This->typelib_header.nrtypeinfos - 1) << 16;
    typeinfo->memoffset = -1; /* should be EOF if no elements */
    typeinfo->res2 = 0;
    typeinfo->res3 = -1;
    typeinfo->res4 = 3;
    typeinfo->res5 = 0;
    typeinfo->cElement = 0;
    typeinfo->res7 = 0;
    typeinfo->res8 = 0;
    typeinfo->res9 = 0;
    typeinfo->resA = 0;
    typeinfo->posguid = -1;
    typeinfo->flags = 0;
    typeinfo->NameOffset = nameoffset;
    typeinfo->version = 0;
    typeinfo->docstringoffs = -1;
    typeinfo->helpstringcontext = 0;
    typeinfo->helpcontext = 0;
    typeinfo->oCustData = -1;
    typeinfo->cbSizeVft = 0;
    typeinfo->cImplTypes = 0;
    typeinfo->size = 0;
    typeinfo->datatype1 = -1;
    typeinfo->datatype2 = 0;
    typeinfo->res18 = 0;
    typeinfo->res19 = -1;

    return offset;
}

/****************************************************************************
 *	ctl2_alloc_guid
 *
 *  Allocates and initializes a GUID structure in a type library. Also updates
 *  the GUID hash table as needed.
 *
 * RETURNS
 *
 *  Success: The offset of the new GUID.
 *  Failure: -1 (this is invariably an out of memory condition).
 */
static int ctl2_alloc_guid(
	ICreateTypeLib2Impl *This, /* [I] The type library to allocate in. */
	MSFT_GuidEntry *guid)      /* [I] The GUID to store. */
{
    int offset;
    MSFT_GuidEntry *guid_space;
    int hash;
    int hash_key;
    int i;

    for (offset = 0; offset < This->typelib_segdir[MSFT_SEG_GUID].length;
	 offset += sizeof(MSFT_GuidEntry)) {
	if (!memcmp(&(This->typelib_segment_data[MSFT_SEG_GUID][offset]),
		    guid, sizeof(GUID))) {
	    return offset;
	}
    }

    offset = ctl2_alloc_segment(This, MSFT_SEG_GUID, sizeof(MSFT_GuidEntry), 0);
    if (offset == -1) return -1;

    guid_space = (void *)(This->typelib_segment_data[MSFT_SEG_GUID] + offset);
    *guid_space = *guid;

    hash = 0;
    for (i = 0; i < 16; i += 2) {
	hash ^= *((short *)&This->typelib_segment_data[MSFT_SEG_GUID][offset + i]);
    }

    hash_key = (hash & 0xf) | ((hash & 0x10) & (0 - !!(hash & 0xe0)));
    guid_space->unk14 = This->typelib_guidhash_segment[hash_key];
    This->typelib_guidhash_segment[hash_key] = offset;

    TRACE("Updating GUID hash table (%s,0x%x).\n", debugstr_guid(&guid->guid), hash);

    return offset;
}

/****************************************************************************
 *	ctl2_alloc_name
 *
 *  Allocates and initializes a name within a type library. Also updates the
 *  name hash table as needed.
 *
 * RETURNS
 *
 *  Success: The offset within the segment of the new name.
 *  Failure: -1 (this is invariably an out of memory condition).
 */
static int ctl2_alloc_name(
	ICreateTypeLib2Impl *This, /* [I] The type library to allocate in. */
	WCHAR *name)               /* [I] The name to store. */
{
    int length;
    int offset;
    MSFT_NameIntro *name_space;
    char *encoded_name;

    length = ctl2_encode_name(This, name, &encoded_name);

    offset = ctl2_find_name(This, encoded_name);
    if (offset != -1) return offset;

    offset = ctl2_alloc_segment(This, MSFT_SEG_NAME, length + 8, 0);
    if (offset == -1) return -1;

    name_space = (void *)(This->typelib_segment_data[MSFT_SEG_NAME] + offset);
    name_space->unk00 = -1;
    name_space->unk10 = -1;
    memcpy(&name_space->namelen, encoded_name, length);

    if (This->typelib_namehash_segment[encoded_name[2] & 0x7f] != -1)
	name_space->unk10 = This->typelib_namehash_segment[encoded_name[2] & 0x7f];

    This->typelib_namehash_segment[encoded_name[2] & 0x7f] = offset;

    This->typelib_header.nametablecount += 1;
    This->typelib_header.nametablechars += *encoded_name;

    return offset;
}

/****************************************************************************
 *	ctl2_alloc_string
 *
 *  Allocates and initializes a string in a type library.
 *
 * RETURNS
 *
 *  Success: The offset within the segment of the new string.
 *  Failure: -1 (this is invariably an out of memory condition).
 */
static int ctl2_alloc_string(
	ICreateTypeLib2Impl *This, /* [I] The type library to allocate in. */
	WCHAR *string)             /* [I] The string to store. */
{
    int length;
    int offset;
    char *string_space;
    char *encoded_string;

    length = ctl2_encode_string(This, string, &encoded_string);

    for (offset = 0; offset < This->typelib_segdir[MSFT_SEG_STRING].length;
	 offset += ((((This->typelib_segment_data[MSFT_SEG_STRING][offset + 1] << 8) & 0xff)
	     | (This->typelib_segment_data[MSFT_SEG_STRING][offset + 0] & 0xff)) + 5) & ~3) {
	if (!memcmp(encoded_string, This->typelib_segment_data[MSFT_SEG_STRING] + offset, length)) return offset;
    }

    offset = ctl2_alloc_segment(This, MSFT_SEG_STRING, length, 0);
    if (offset == -1) return -1;

    string_space = This->typelib_segment_data[MSFT_SEG_STRING] + offset;
    memcpy(string_space, encoded_string, length);

    return offset;
}

/****************************************************************************
 *	ctl2_alloc_importinfo
 *
 *  Allocates and initializes an import information structure in a type library.
 *
 * RETURNS
 *
 *  Success: The offset of the new importinfo.
 *  Failure: -1 (this is invariably an out of memory condition).
 */
static int ctl2_alloc_importinfo(
	ICreateTypeLib2Impl *This, /* [I] The type library to allocate in. */
	MSFT_ImpInfo *impinfo)     /* [I] The import information to store. */
{
    int offset;
    MSFT_ImpInfo *impinfo_space;

    for (offset = 0;
	 offset < This->typelib_segdir[MSFT_SEG_IMPORTINFO].length;
	 offset += sizeof(MSFT_ImpInfo)) {
	if (!memcmp(&(This->typelib_segment_data[MSFT_SEG_IMPORTINFO][offset]),
		    impinfo, sizeof(MSFT_ImpInfo))) {
	    return offset;
	}
    }

    offset = ctl2_alloc_segment(This, MSFT_SEG_IMPORTINFO, sizeof(MSFT_ImpInfo), 0);
    if (offset == -1) return -1;

    impinfo_space = (void *)(This->typelib_segment_data[MSFT_SEG_IMPORTINFO] + offset);
    *impinfo_space = *impinfo;

    return offset;
}

/****************************************************************************
 *	ctl2_alloc_importfile
 *
 *  Allocates and initializes an import file definition in a type library.
 *
 * RETURNS
 *
 *  Success: The offset of the new importinfo.
 *  Failure: -1 (this is invariably an out of memory condition).
 */
static int ctl2_alloc_importfile(
	ICreateTypeLib2Impl *This, /* [I] The type library to allocate in. */
	int guidoffset,            /* [I] The offset to the GUID for the imported library. */
	int major_version,         /* [I] The major version number of the imported library. */
	int minor_version,         /* [I] The minor version number of the imported library. */
	WCHAR *filename)           /* [I] The filename of the imported library. */
{
    int length;
    int offset;
    MSFT_ImpFile *importfile;
    char *encoded_string;

    length = ctl2_encode_string(This, filename, &encoded_string);

    encoded_string[0] <<= 2;
    encoded_string[0] |= 1;

    for (offset = 0; offset < This->typelib_segdir[MSFT_SEG_IMPORTFILES].length;
	 offset += ((((This->typelib_segment_data[MSFT_SEG_IMPORTFILES][offset + 0xd] << 8) & 0xff)
	     | (This->typelib_segment_data[MSFT_SEG_IMPORTFILES][offset + 0xc] & 0xff)) >> 2) + 0xc) {
	if (!memcmp(encoded_string, This->typelib_segment_data[MSFT_SEG_IMPORTFILES] + offset + 0xc, length)) return offset;
    }

    offset = ctl2_alloc_segment(This, MSFT_SEG_IMPORTFILES, length + 0xc, 0);
    if (offset == -1) return -1;

    importfile = (MSFT_ImpFile *)&This->typelib_segment_data[MSFT_SEG_IMPORTFILES][offset];
    importfile->guid = guidoffset;
    importfile->lcid = This->typelib_header.lcid2;
    importfile->version = major_version | (minor_version << 16);
    memcpy(&importfile->filename, encoded_string, length);

    return offset;
}


/*================== ICreateTypeInfo2 Implementation ===================================*/

/******************************************************************************
 * ICreateTypeInfo2_QueryInterface {OLEAUT32}
 *
 *  See IUnknown_QueryInterface.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnQueryInterface(
	ICreateTypeInfo2 * iface,
	REFIID riid,
	VOID **ppvObject)
{
    ICOM_THIS( ICreateTypeInfo2Impl, iface);

    TRACE("(%p)->(IID: %s)\n",This,debugstr_guid(riid));

    *ppvObject=NULL;
    if(IsEqualIID(riid, &IID_IUnknown) ||
       IsEqualIID(riid,&IID_ICreateTypeInfo)||
       IsEqualIID(riid,&IID_ICreateTypeInfo2))
    {
        *ppvObject = This;
    } else if (IsEqualIID(riid, &IID_ITypeInfo) ||
	       IsEqualIID(riid, &IID_ITypeInfo2)) {
	FIXME("QI for ITypeInfo interfaces not supported yet.\n");
    }

    if(*ppvObject)
    {
        ICreateTypeLib2_AddRef(iface);
        TRACE("-- Interface: (%p)->(%p)\n",ppvObject,*ppvObject);
        return S_OK;
    }
    TRACE("-- Interface: E_NOINTERFACE\n");
    return E_NOINTERFACE;
}

/******************************************************************************
 * ICreateTypeInfo2_AddRef {OLEAUT32}
 *
 *  See IUnknown_AddRef.
 */
static ULONG WINAPI ICreateTypeInfo2_fnAddRef(ICreateTypeInfo2 *iface)
{
    ICOM_THIS( ICreateTypeInfo2Impl, iface);

    TRACE("(%p)->ref was %u\n",This, This->ref);

    return ++(This->ref);
}

/******************************************************************************
 * ICreateTypeInfo2_Release {OLEAUT32}
 *
 *  See IUnknown_Release.
 */
static ULONG WINAPI ICreateTypeInfo2_fnRelease(ICreateTypeInfo2 *iface)
{
    ICOM_THIS( ICreateTypeInfo2Impl, iface);

    --(This->ref);

    TRACE("(%p)->(%u)\n",This, This->ref);

    if (!This->ref) {
	if (This->typelib) {
	    ICreateTypeLib2_fnRelease((ICreateTypeLib2 *)This->typelib);
	    This->typelib = NULL;
	}

	/* ICreateTypeLib2 frees all ICreateTypeInfos when it releases. */
	/* HeapFree(GetProcessHeap(),0,This); */
	return 0;
    }

    return This->ref;
}


/******************************************************************************
 * ICreateTypeInfo2_SetGuid {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetGuid.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetGuid(ICreateTypeInfo2 *iface, REFGUID guid)
{
    ICOM_THIS(ICreateTypeInfo2Impl, iface);

    MSFT_GuidEntry guidentry;
    int offset;

    TRACE("(%p,%s)\n", iface, debugstr_guid(guid));

    guidentry.guid = *guid;
    guidentry.unk10 = 0;
    guidentry.unk14 = 0x18;

    offset = ctl2_alloc_guid(This->typelib, &guidentry);
    
    if (offset == -1) return E_OUTOFMEMORY;

    This->typeinfo->posguid = offset;

    return S_OK;
}

/******************************************************************************
 * ICreateTypeInfo2_SetTypeFlags {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetTypeFlags.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetTypeFlags(ICreateTypeInfo2 *iface, UINT uTypeFlags)
{
    ICOM_THIS( ICreateTypeInfo2Impl, iface);

    TRACE("(%p,0x%x)\n", iface, uTypeFlags);

    This->typeinfo->flags = uTypeFlags;

    if (uTypeFlags & 0x1000) {
	MSFT_GuidEntry foo;
	int guidoffset;
	int fileoffset;
	MSFT_ImpInfo impinfo;
	WCHAR stdole2tlb[] = { 's','t','d','o','l','e','2','.','t','l','b',0 };

	foo.guid = IID_StdOle;
	foo.unk10 = 2;
	foo.unk14 = -1;
	guidoffset = ctl2_alloc_guid(This->typelib, &foo);
	if (guidoffset == -1) return E_OUTOFMEMORY;

	fileoffset =  ctl2_alloc_importfile(This->typelib, guidoffset, 2, 0, stdole2tlb);
	if (fileoffset == -1) return E_OUTOFMEMORY;

	foo.guid = IID_IDispatch;
	foo.unk10 = 1;
	foo.unk14 = -1;
	guidoffset = ctl2_alloc_guid(This->typelib, &foo);
	if (guidoffset == -1) return E_OUTOFMEMORY;

	impinfo.res0 = 0x03010000;
	impinfo.oImpFile = fileoffset;
	impinfo.oGuid = guidoffset;
	ctl2_alloc_importinfo(This->typelib, &impinfo);

	This->typelib->typelib_header.dispatchpos = 1;
	This->typelib->typelib_header.res50 = 1;

	This->typeinfo->typekind |= 0x10;
	This->typeinfo->typekind &= ~0x0f;
	This->typeinfo->typekind |= TKIND_DISPATCH;
    }

    return S_OK;
}

/******************************************************************************
 * ICreateTypeInfo2_SetDocString {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetDocString.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetDocString(
        ICreateTypeInfo2* iface,
        LPOLESTR pStrDoc)
{
    ICOM_THIS(ICreateTypeInfo2Impl, iface);

    int offset;

    TRACE("(%p,%s)\n", iface, debugstr_w(pStrDoc));

    offset = ctl2_alloc_string(This->typelib, pStrDoc);
    if (offset == -1) return E_OUTOFMEMORY;
    This->typeinfo->docstringoffs = offset;
    return S_OK;
}

/******************************************************************************
 * ICreateTypeInfo2_SetHelpContext {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetHelpContext.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetHelpContext(
        ICreateTypeInfo2* iface,
        DWORD dwHelpContext)
{
    FIXME("(%p,%ld), stub!\n", iface, dwHelpContext);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetVersion {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetVersion.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetVersion(
        ICreateTypeInfo2* iface,
        WORD wMajorVerNum,
        WORD wMinorVerNum)
{
    ICOM_THIS(ICreateTypeInfo2Impl, iface);

    TRACE("(%p,%d,%d)\n", iface, wMajorVerNum, wMinorVerNum);

    This->typeinfo->version = wMajorVerNum | (wMinorVerNum << 16);
    return S_OK;
}

/******************************************************************************
 * ICreateTypeInfo2_AddRefTypeInfo {OLEAUT32}
 *
 *  See ICreateTypeInfo_AddRefTypeInfo.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnAddRefTypeInfo(
        ICreateTypeInfo2* iface,
        ITypeInfo* pTInfo,
        HREFTYPE* phRefType)
{
    FIXME("(%p,%p,%p), stub!\n", iface, pTInfo, phRefType);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_AddFuncDesc {OLEAUT32}
 *
 *  See ICreateTypeInfo_AddFuncDesc.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnAddFuncDesc(
        ICreateTypeInfo2* iface,
        UINT index,
        FUNCDESC* pFuncDesc)
{
    FIXME("(%p,%d,%p), stub!\n", iface, index, pFuncDesc);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_AddImplType {OLEAUT32}
 *
 *  See ICreateTypeInfo_AddImplType.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnAddImplType(
        ICreateTypeInfo2* iface,
        UINT index,
        HREFTYPE hRefType)
{
    FIXME("(%p,%d,%ld), stub!\n", iface, index, hRefType);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetImplTypeFlags {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetImplTypeFlags.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetImplTypeFlags(
        ICreateTypeInfo2* iface,
        UINT index,
        INT implTypeFlags)
{
    FIXME("(%p,%d,0x%x), stub!\n", iface, index, implTypeFlags);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetAlignment {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetAlignment.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetAlignment(
        ICreateTypeInfo2* iface,
        WORD cbAlignment)
{
    FIXME("(%p,%d), stub!\n", iface, cbAlignment);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetSchema {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetSchema.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetSchema(
        ICreateTypeInfo2* iface,
        LPOLESTR pStrSchema)
{
    FIXME("(%p,%s), stub!\n", iface, debugstr_w(pStrSchema));
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_AddVarDesc {OLEAUT32}
 *
 *  See ICreateTypeInfo_AddVarDesc.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnAddVarDesc(
        ICreateTypeInfo2* iface,
        UINT index,
        VARDESC* pVarDesc)
{
    ICOM_THIS(ICreateTypeInfo2Impl, iface);
    int offset;
    INT *typedata;
    int var_datawidth;
    int alignment;

    TRACE("(%p,%d,%p), stub!\n", iface, index, pVarDesc);
    TRACE("%ld, %p, %ld, {{%lx, %d}, {%p, %x}}, 0x%x, %d\n", pVarDesc->memid, pVarDesc->lpstrSchema, pVarDesc->u.oInst,
	  pVarDesc->elemdescVar.tdesc.u.hreftype, pVarDesc->elemdescVar.tdesc.vt,
	  pVarDesc->elemdescVar.u.paramdesc.pparamdescex, pVarDesc->elemdescVar.u.paramdesc.wParamFlags,
	  pVarDesc->wVarFlags, pVarDesc->varkind);

    if ((This->typeinfo->cElement >> 16) != index) {
	TRACE("Out-of-order element.\n");
	return TYPE_E_ELEMENTNOTFOUND;
    }

    if (!This->typedata) {
	This->typedata = HeapAlloc(GetProcessHeap(), 0, 0x2000);
	This->typedata[0] = 0;
    }

    /* allocate type data space for us */
    offset = This->typedata[0];
    This->typedata[0] += 0x14;
    typedata = This->typedata + (offset >> 2) + 1;

    /* fill out the basic type information */
    typedata[0] = 0x14 | (index << 16);
    typedata[1] = 0x80000000 | (pVarDesc->elemdescVar.tdesc.vt << 16) | pVarDesc->elemdescVar.tdesc.vt;
    typedata[2] = pVarDesc->wVarFlags;
    typedata[3] = 0x00240000;
    typedata[4] = This->datawidth;

    /* update the index data */
    This->indices[index] = 0x40000000 + index;
    This->names[index] = -1;
    This->offsets[index] = offset;

    /* figure out type widths and whatnot */
    if (pVarDesc->elemdescVar.tdesc.vt == VT_UI4) {
	var_datawidth = 4;
    } else if (pVarDesc->elemdescVar.tdesc.vt == VT_BSTR) {
	var_datawidth = 4;
    } else if (pVarDesc->elemdescVar.tdesc.vt == VT_UI2) {
	var_datawidth = 2;
    } else if (pVarDesc->elemdescVar.tdesc.vt == VT_UI1) {
	var_datawidth = 1;
    } else if (pVarDesc->elemdescVar.tdesc.vt == VT_CARRAY) {
	int *typedesc;
	int *arraydesc;
	int typeoffset;
	int arrayoffset;

	FIXME("Array vartype, hacking badly.\n");
	typeoffset = ctl2_alloc_segment(This->typelib, MSFT_SEG_TYPEDESC, 8, 0);
	arrayoffset = ctl2_alloc_segment(This->typelib, MSFT_SEG_ARRAYDESC, 16, 0);

	typedesc = (void *)&This->typelib->typelib_segment_data[MSFT_SEG_TYPEDESC][typeoffset];
	arraydesc = (void *)&This->typelib->typelib_segment_data[MSFT_SEG_ARRAYDESC][arrayoffset];

	typedesc[0] = 0x7ffe001c;
	typedesc[1] = arrayoffset;

	arraydesc[0] = 0x80000000 | (pVarDesc->elemdescVar.tdesc.u.lpadesc->tdescElem.vt << 16) | pVarDesc->elemdescVar.tdesc.u.lpadesc->tdescElem.vt;
	arraydesc[1] = 0x00080001;
	arraydesc[2] = 0x8;
	arraydesc[3] = 0;

	typedata[1] = typeoffset;
	typedata[3] = 0x00380000;

	This->datawidth += 8;
	var_datawidth = 0; /* FIXME: Probably wrong. */
    } else {
	FIXME("Unrecognized vartype %d.\n", pVarDesc->elemdescVar.tdesc.vt);
	var_datawidth = 0;
    }

    if (pVarDesc->elemdescVar.tdesc.vt != VT_CARRAY) {
	/* pad out starting position to data width */
	This->datawidth += var_datawidth - 1;
	This->datawidth &= ~(var_datawidth - 1);
	typedata[4] = This->datawidth;
	
	/* add the new variable to the total data width */
	This->datawidth += var_datawidth;
    }

    /* fix type alignment */
    alignment = (This->typeinfo->typekind >> 11) & 0x1f;
    if (alignment < var_datawidth) {
	alignment = var_datawidth;
	This->typeinfo->typekind &= ~0xf800;
	This->typeinfo->typekind |= alignment << 11;
    }

    /* ??? */
    if (!This->typeinfo->res2) This->typeinfo->res2 = 0x1a;
    if ((index == 0) || (index == 1) || (index == 2) || (index == 4) || (index == 9)) {
	This->typeinfo->res2 <<= 1;
    }

    /* ??? */
    if (This->typeinfo->res3 == -1) This->typeinfo->res3 = 0;
    This->typeinfo->res3 += 0x2c;

    /* increment the number of variable elements */
    This->typeinfo->cElement += 0x10000;

    /* pad data width to alignment */
    This->typeinfo->size = (This->datawidth + (alignment - 1)) & ~(alignment - 1);

    return S_OK;
}

/******************************************************************************
 * ICreateTypeInfo2_SetFuncAndParamNames {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetFuncAndParamNames.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetFuncAndParamNames(
        ICreateTypeInfo2* iface,
        UINT index,
        LPOLESTR* rgszNames,
        UINT cNames)
{
    FIXME("(%p,%d,%s,%d), stub!\n", iface, index, debugstr_w(*rgszNames), cNames);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetVarName {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetVarName.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetVarName(
        ICreateTypeInfo2* iface,
        UINT index,
        LPOLESTR szName)
{
    ICOM_THIS(ICreateTypeInfo2Impl, iface);
    int offset;
    char *namedata;

    TRACE("(%p,%d,%s), stub!\n", iface, index, debugstr_w(szName));

    if ((This->typeinfo->cElement >> 16) <= index) {
	TRACE("Out-of-order element.\n");
	return TYPE_E_ELEMENTNOTFOUND;
    }

    offset = ctl2_alloc_name(This->typelib, szName);
    if (offset == -1) return E_OUTOFMEMORY;

    namedata = This->typelib->typelib_segment_data[MSFT_SEG_NAME] + offset;
    *((INT *)namedata) = 0;
    namedata[9] = 0x10;
    This->names[index] = offset;

    return S_OK;
}

/******************************************************************************
 * ICreateTypeInfo2_SetTypeDescAlias {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetTypeDescAlias.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetTypeDescAlias(
        ICreateTypeInfo2* iface,
        TYPEDESC* pTDescAlias)
{
    FIXME("(%p,%p), stub!\n", iface, pTDescAlias);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_DefineFuncAsDllEntry {OLEAUT32}
 *
 *  See ICreateTypeInfo_DefineFuncAsDllEntry.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnDefineFuncAsDllEntry(
        ICreateTypeInfo2* iface,
        UINT index,
        LPOLESTR szDllName,
        LPOLESTR szProcName)
{
    FIXME("(%p,%d,%s,%s), stub!\n", iface, index, debugstr_w(szDllName), debugstr_w(szProcName));
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetFuncDocString {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetFuncDocString.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetFuncDocString(
        ICreateTypeInfo2* iface,
        UINT index,
        LPOLESTR szDocString)
{
    FIXME("(%p,%d,%s), stub!\n", iface, index, debugstr_w(szDocString));
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetVarDocString {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetVarDocString.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetVarDocString(
        ICreateTypeInfo2* iface,
        UINT index,
        LPOLESTR szDocString)
{
    ICOM_THIS(ICreateTypeInfo2Impl, iface);

    FIXME("(%p,%d,%s), stub!\n", iface, index, debugstr_w(szDocString));

    ctl2_alloc_string(This->typelib, szDocString);

    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetFuncHelpContext {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetFuncHelpContext.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetFuncHelpContext(
        ICreateTypeInfo2* iface,
        UINT index,
        DWORD dwHelpContext)
{
    FIXME("(%p,%d,%ld), stub!\n", iface, index, dwHelpContext);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetVarHelpContext {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetVarHelpContext.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetVarHelpContext(
        ICreateTypeInfo2* iface,
        UINT index,
        DWORD dwHelpContext)
{
    FIXME("(%p,%d,%ld), stub!\n", iface, index, dwHelpContext);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetMops {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetMops.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetMops(
        ICreateTypeInfo2* iface,
        UINT index,
        BSTR bstrMops)
{
    FIXME("(%p,%d,%p), stub!\n", iface, index, bstrMops);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetTypeIdldesc {OLEAUT32}
 *
 *  See ICreateTypeInfo_SetTypeIdldesc.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetTypeIdldesc(
        ICreateTypeInfo2* iface,
        IDLDESC* pIdlDesc)
{
    FIXME("(%p,%p), stub!\n", iface, pIdlDesc);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_LayOut {OLEAUT32}
 *
 *  See ICreateTypeInfo_LayOut.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnLayOut(
	ICreateTypeInfo2* iface)
{
    TRACE("(%p), stub!\n", iface);
/*     return E_OUTOFMEMORY; */
    return S_OK;
}

/******************************************************************************
 * ICreateTypeInfo2_DeleteFuncDesc {OLEAUT32}
 *
 *  Delete a function description from a type.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnDeleteFuncDesc(
        ICreateTypeInfo2* iface, /* [I] The typeinfo from which to delete a function. */
        UINT index)              /* [I] The index of the function to delete. */
{
    FIXME("(%p,%d), stub!\n", iface, index);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_DeleteFuncDescByMemId {OLEAUT32}
 *
 *  Delete a function description from a type.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnDeleteFuncDescByMemId(
        ICreateTypeInfo2* iface, /* [I] The typeinfo from which to delete a function. */
        MEMBERID memid,          /* [I] The member id of the function to delete. */
        INVOKEKIND invKind)      /* [I] The invocation type of the function to delete. (?) */
{
    FIXME("(%p,%ld,%d), stub!\n", iface, memid, invKind);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_DeleteVarDesc {OLEAUT32}
 *
 *  Delete a variable description from a type.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY, E_INVALIDARG, TYPE_E_IOERROR,
 *  TYPE_E_INVDATAREAD, TYPE_E_UNSUPFORMAT or TYPE_E_INVALIDSTATE.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnDeleteVarDesc(
        ICreateTypeInfo2* iface, /* [I] The typeinfo from which to delete the variable description. */
        UINT index)              /* [I] The index of the variable description to delete. */
{
    FIXME("(%p,%d), stub!\n", iface, index);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_DeleteVarDescByMemId {OLEAUT32}
 *
 *  Delete a variable description from a type.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY, E_INVALIDARG, TYPE_E_IOERROR,
 *  TYPE_E_INVDATAREAD, TYPE_E_UNSUPFORMAT or TYPE_E_INVALIDSTATE.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnDeleteVarDescByMemId(
        ICreateTypeInfo2* iface, /* [I] The typeinfo from which to delete the variable description. */
        MEMBERID memid)          /* [I] The member id of the variable description to delete. */
{
    FIXME("(%p,%ld), stub!\n", iface, memid);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_DeleteImplType {OLEAUT32}
 *
 *  Delete an interface implementation from a type. (?)
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnDeleteImplType(
        ICreateTypeInfo2* iface, /* [I] The typeinfo from which to delete. */
        UINT index)              /* [I] The index of the interface to delete. */
{
    FIXME("(%p,%d), stub!\n", iface, index);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetCustData {OLEAUT32}
 *
 *  Set the custom data for a type.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetCustData(
        ICreateTypeInfo2* iface, /* [I] The typeinfo in which to set the custom data. */
        REFGUID guid,            /* [I] The GUID used as a key to retrieve the custom data. */
        VARIANT* pVarVal)        /* [I] The custom data. */
{
    FIXME("(%p,%s,%p), stub!\n", iface, debugstr_guid(guid), pVarVal);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetFuncCustData {OLEAUT32}
 *
 *  Set the custom data for a function.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetFuncCustData(
        ICreateTypeInfo2* iface, /* [I] The typeinfo in which to set the custom data. */
        UINT index,              /* [I] The index of the function for which to set the custom data. */
        REFGUID guid,            /* [I] The GUID used as a key to retrieve the custom data. */
        VARIANT* pVarVal)        /* [I] The custom data. */
{
    FIXME("(%p,%d,%s,%p), stub!\n", iface, index, debugstr_guid(guid), pVarVal);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetParamCustData {OLEAUT32}
 *
 *  Set the custom data for a function parameter.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetParamCustData(
        ICreateTypeInfo2* iface, /* [I] The typeinfo in which to set the custom data. */
        UINT indexFunc,          /* [I] The index of the function on which the parameter resides. */
        UINT indexParam,         /* [I] The index of the paramter on which to set the custom data. */
        REFGUID guid,            /* [I] The GUID used as a key to retrieve the custom data. */
        VARIANT* pVarVal)        /* [I] The custom data. */
{
    FIXME("(%p,%d,%d,%s,%p), stub!\n", iface, indexFunc, indexParam, debugstr_guid(guid), pVarVal);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetVarCustData {OLEAUT32}
 *
 *  Set the custom data for a variable.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetVarCustData(
        ICreateTypeInfo2* iface, /* [I] The typeinfo in which to set the custom data. */
        UINT index,              /* [I] The index of the variable on which to set the custom data. */
        REFGUID guid,            /* [I] The GUID used as a key to retrieve the custom data. */
        VARIANT* pVarVal)        /* [I] The custom data. */
{
    FIXME("(%p,%d,%s,%p), stub!\n", iface, index, debugstr_guid(guid), pVarVal);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetImplTypeCustData {OLEAUT32}
 *
 *  Set the custom data for an implemented interface.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetImplTypeCustData(
        ICreateTypeInfo2* iface, /* [I] The typeinfo on which to set the custom data. */
        UINT index,              /* [I] The index of the implemented interface on which to set the custom data. */
        REFGUID guid,            /* [I] The GUID used as a key to retrieve the custom data. */
        VARIANT* pVarVal)        /* [I] The custom data. */
{
    FIXME("(%p,%d,%s,%p), stub!\n", iface, index, debugstr_guid(guid), pVarVal);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetHelpStringContext {OLEAUT32}
 *
 *  Set the help string context for the typeinfo.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetHelpStringContext(
        ICreateTypeInfo2* iface,   /* [I] The typeinfo on which to set the help string context. */
        ULONG dwHelpStringContext) /* [I] The help string context. */
{
    FIXME("(%p,%ld), stub!\n", iface, dwHelpStringContext);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetFuncHelpStringContext {OLEAUT32}
 *
 *  Set the help string context for a function.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetFuncHelpStringContext(
        ICreateTypeInfo2* iface,   /* [I] The typeinfo on which to set the help string context. */
        UINT index,                /* [I] The index for the function on which to set the help string context. */
        ULONG dwHelpStringContext) /* [I] The help string context. */
{
    FIXME("(%p,%d,%ld), stub!\n", iface, index, dwHelpStringContext);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetVarHelpStringContext {OLEAUT32}
 *
 *  Set the help string context for a variable.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetVarHelpStringContext(
        ICreateTypeInfo2* iface,   /* [I] The typeinfo on which to set the help string context. */
        UINT index,                /* [I] The index of the variable on which to set the help string context. */
        ULONG dwHelpStringContext) /* [I] The help string context */
{
    FIXME("(%p,%d,%ld), stub!\n", iface, index, dwHelpStringContext);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_Invalidate {OLEAUT32}
 *
 *  Undocumented function. (!)
 */
static HRESULT WINAPI ICreateTypeInfo2_fnInvalidate(
        ICreateTypeInfo2* iface)
{
    FIXME("(%p), stub!\n", iface);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeInfo2_SetName {OLEAUT32}
 *
 *  Set the name for a typeinfo.
 *
 * RETURNS
 *
 *  Success: S_OK.
 *  Failure: One of STG_E_INSUFFICIENTMEMORY, E_OUTOFMEMORY, E_INVALIDARG or TYPE_E_INVALIDSTATE.
 */
static HRESULT WINAPI ICreateTypeInfo2_fnSetName(
        ICreateTypeInfo2* iface,
        LPOLESTR szName)
{
    FIXME("(%p,%s), stub!\n", iface, debugstr_w(szName));
    return E_OUTOFMEMORY;
}


static ICOM_VTABLE(ICreateTypeInfo2) ctypeinfo2vt =
{
    ICOM_MSVTABLE_COMPAT_DummyRTTIVALUE

    ICreateTypeInfo2_fnQueryInterface,
    ICreateTypeInfo2_fnAddRef,
    ICreateTypeInfo2_fnRelease,

    ICreateTypeInfo2_fnSetGuid,
    ICreateTypeInfo2_fnSetTypeFlags,
    ICreateTypeInfo2_fnSetDocString,
    ICreateTypeInfo2_fnSetHelpContext,
    ICreateTypeInfo2_fnSetVersion,
    ICreateTypeInfo2_fnAddRefTypeInfo,
    ICreateTypeInfo2_fnAddFuncDesc,
    ICreateTypeInfo2_fnAddImplType,
    ICreateTypeInfo2_fnSetImplTypeFlags,
    ICreateTypeInfo2_fnSetAlignment,
    ICreateTypeInfo2_fnSetSchema,
    ICreateTypeInfo2_fnAddVarDesc,
    ICreateTypeInfo2_fnSetFuncAndParamNames,
    ICreateTypeInfo2_fnSetVarName,
    ICreateTypeInfo2_fnSetTypeDescAlias,
    ICreateTypeInfo2_fnDefineFuncAsDllEntry,
    ICreateTypeInfo2_fnSetFuncDocString,
    ICreateTypeInfo2_fnSetVarDocString,
    ICreateTypeInfo2_fnSetFuncHelpContext,
    ICreateTypeInfo2_fnSetVarHelpContext,
    ICreateTypeInfo2_fnSetMops,
    ICreateTypeInfo2_fnSetTypeIdldesc,
    ICreateTypeInfo2_fnLayOut,

    ICreateTypeInfo2_fnDeleteFuncDesc,
    ICreateTypeInfo2_fnDeleteFuncDescByMemId,
    ICreateTypeInfo2_fnDeleteVarDesc,
    ICreateTypeInfo2_fnDeleteVarDescByMemId,
    ICreateTypeInfo2_fnDeleteImplType,
    ICreateTypeInfo2_fnSetCustData,
    ICreateTypeInfo2_fnSetFuncCustData,
    ICreateTypeInfo2_fnSetParamCustData,
    ICreateTypeInfo2_fnSetVarCustData,
    ICreateTypeInfo2_fnSetImplTypeCustData,
    ICreateTypeInfo2_fnSetHelpStringContext,
    ICreateTypeInfo2_fnSetFuncHelpStringContext,
    ICreateTypeInfo2_fnSetVarHelpStringContext,
    ICreateTypeInfo2_fnInvalidate,
    ICreateTypeInfo2_fnSetName
};

static ICreateTypeInfo2 *ICreateTypeInfo2_Constructor(ICreateTypeLib2Impl *typelib, WCHAR *szName, TYPEKIND tkind)
{
    ICreateTypeInfo2Impl *pCreateTypeInfo2Impl;

    int nameoffset;
    int typeinfo_offset;
    MSFT_TypeInfoBase *typeinfo;

    TRACE("Constructing ICreateTypeInfo2 for %s with tkind %d\n", debugstr_w(szName), tkind);

    pCreateTypeInfo2Impl = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(ICreateTypeInfo2Impl));
    if (!pCreateTypeInfo2Impl) return NULL;

    pCreateTypeInfo2Impl->lpVtbl = &ctypeinfo2vt;
    pCreateTypeInfo2Impl->ref = 1;

    pCreateTypeInfo2Impl->typelib = typelib;
    typelib->ref++;

    nameoffset = ctl2_alloc_name(typelib, szName);
    typeinfo_offset = ctl2_alloc_typeinfo(typelib, nameoffset);
    typeinfo = (MSFT_TypeInfoBase *)&typelib->typelib_segment_data[MSFT_SEG_TYPEINFO][typeinfo_offset];

    typelib->typelib_segment_data[MSFT_SEG_NAME][nameoffset + 9] = 0x38;
    *((int *)&typelib->typelib_segment_data[MSFT_SEG_NAME][nameoffset]) = typeinfo_offset;

    pCreateTypeInfo2Impl->typeinfo = typeinfo;

    if (tkind == TKIND_ENUM) {
	typeinfo->typekind |= TKIND_ENUM | 0x2120;
	typeinfo->size = 4;
    } else if (tkind == TKIND_RECORD) {
	typeinfo->typekind |= TKIND_RECORD | 0x0920;
	typeinfo->size = 0;
    } else if (tkind == TKIND_MODULE) {
	typeinfo->typekind |= TKIND_MODULE | 0x0920;
	typeinfo->size = 2;
    } else if (tkind == TKIND_INTERFACE) {
	typeinfo->typekind |= TKIND_INTERFACE | 0x2120;
	typeinfo->size = 4;
    } else if (tkind == TKIND_DISPATCH) {
	typeinfo->typekind |= TKIND_DISPATCH | 0x2120;
	typeinfo->size = 4;
    } else if (tkind == TKIND_COCLASS) {
	typeinfo->typekind |= TKIND_COCLASS | 0x2120;
	typeinfo->size = 4;
    } else if (tkind == TKIND_ALIAS) {
	typeinfo->typekind |= TKIND_ALIAS | 0x2120;
	typeinfo->size = -0x75; /* ??? */
    } else if (tkind == TKIND_UNION) {
	typeinfo->typekind |= TKIND_UNION | 0x0920;
	typeinfo->size = 0;
    } else {
	FIXME("(%s,%d), unrecognized typekind %d\n", debugstr_w(szName), tkind, tkind);
	typeinfo->typekind |= tkind;
	typeinfo->size = 0xdeadbeef;
    }

    if (typelib->last_typeinfo) typelib->last_typeinfo->next_typeinfo = pCreateTypeInfo2Impl;
    typelib->last_typeinfo = pCreateTypeInfo2Impl;
    if (!typelib->typeinfos) typelib->typeinfos = pCreateTypeInfo2Impl;

    TRACE(" -- %p\n", pCreateTypeInfo2Impl);

    return (ICreateTypeInfo2 *)pCreateTypeInfo2Impl;
}


/*================== ICreateTypeLib2 Implementation ===================================*/

/******************************************************************************
 * ICreateTypeLib2_QueryInterface {OLEAUT32}
 *
 *  See IUnknown_QueryInterface.
 */
static HRESULT WINAPI ICreateTypeLib2_fnQueryInterface(
	ICreateTypeLib2 * iface,
	REFIID riid,
	VOID **ppvObject)
{
    ICOM_THIS( ICreateTypeLib2Impl, iface);

    TRACE("(%p)->(IID: %s)\n",This,debugstr_guid(riid));

    *ppvObject=NULL;
    if(IsEqualIID(riid, &IID_IUnknown) ||
       IsEqualIID(riid,&IID_ICreateTypeLib)||
       IsEqualIID(riid,&IID_ICreateTypeLib2))
    {
        *ppvObject = This;
    } else if (IsEqualIID(riid, &IID_ITypeLib) ||
	       IsEqualIID(riid, &IID_ITypeLib2)) {
	FIXME("QI for ITypeLib interfaces not supported yet.\n");
    }

    if(*ppvObject)
    {
        ICreateTypeLib2_AddRef(iface);
        TRACE("-- Interface: (%p)->(%p)\n",ppvObject,*ppvObject);
        return S_OK;
    }
    TRACE("-- Interface: E_NOINTERFACE\n");
    return E_NOINTERFACE;
}

/******************************************************************************
 * ICreateTypeLib2_AddRef {OLEAUT32}
 *
 *  See IUnknown_AddRef.
 */
static ULONG WINAPI ICreateTypeLib2_fnAddRef(ICreateTypeLib2 *iface)
{
    ICOM_THIS( ICreateTypeLib2Impl, iface);

    TRACE("(%p)->ref was %u\n",This, This->ref);

    return ++(This->ref);
}

/******************************************************************************
 * ICreateTypeLib2_Release {OLEAUT32}
 *
 *  See IUnknown_Release.
 */
static ULONG WINAPI ICreateTypeLib2_fnRelease(ICreateTypeLib2 *iface)
{
    ICOM_THIS( ICreateTypeLib2Impl, iface);

    --(This->ref);

    TRACE("(%p)->(%u)\n",This, This->ref);

    if (!This->ref) {
	int i;

	for (i = 0; i < MSFT_SEG_MAX; i++) {
	    if (This->typelib_segment_data[i]) {
		HeapFree(GetProcessHeap(), 0, This->typelib_segment_data[i]);
		This->typelib_segment_data[i] = NULL;
	    }
	}

	if (This->filename) {
	    HeapFree(GetProcessHeap(), 0, This->filename);
	    This->filename = NULL;
	}

	while (This->typeinfos) {
	    ICreateTypeInfo2Impl *typeinfo = This->typeinfos;
	    This->typeinfos = typeinfo->next_typeinfo;
	    if (typeinfo->typedata) HeapFree(GetProcessHeap(), 0, typeinfo->typedata);
	    HeapFree(GetProcessHeap(), 0, typeinfo);
	}

	HeapFree(GetProcessHeap(),0,This);
	return 0;
    }

    return This->ref;
}


/******************************************************************************
 * ICreateTypeLib2_CreateTypeInfo {OLEAUT32}
 *
 *  See ICreateTypeLib_CreateTypeInfo.
 */
static HRESULT WINAPI ICreateTypeLib2_fnCreateTypeInfo(
	ICreateTypeLib2 * iface,
	LPOLESTR szName,
	TYPEKIND tkind,
	ICreateTypeInfo **ppCTInfo)
{
    ICOM_THIS(ICreateTypeLib2Impl, iface);

    TRACE("(%p,%s,%d,%p)\n", iface, debugstr_w(szName), tkind, ppCTInfo);

    *ppCTInfo = (ICreateTypeInfo *)ICreateTypeInfo2_Constructor(This, szName, tkind);

    if (!*ppCTInfo) return E_OUTOFMEMORY;

    return S_OK;
}

/******************************************************************************
 * ICreateTypeLib2_SetName {OLEAUT32}
 *
 *  See ICreateTypeLib_SetName.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSetName(
	ICreateTypeLib2 * iface,
	LPOLESTR szName)
{
    ICOM_THIS(ICreateTypeLib2Impl, iface);

    int offset;

    TRACE("(%p,%s)\n", iface, debugstr_w(szName));

    offset = ctl2_alloc_name(This, szName);
    if (offset == -1) return E_OUTOFMEMORY;
    This->typelib_header.NameOffset = offset;
    return S_OK;
}

/******************************************************************************
 * ICreateTypeLib2_SetVersion {OLEAUT32}
 *
 *  See ICreateTypeLib_SetVersion.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSetVersion(ICreateTypeLib2 * iface, WORD wMajorVerNum, WORD wMinorVerNum)
{
    ICOM_THIS(ICreateTypeLib2Impl, iface);

    TRACE("(%p,%d,%d)\n", iface, wMajorVerNum, wMinorVerNum);

    This->typelib_header.version = wMajorVerNum | (wMinorVerNum << 16);
    return S_OK;
}

/******************************************************************************
 * ICreateTypeLib2_SetGuid {OLEAUT32}
 *
 *  See ICreateTypeLib_SetGuid.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSetGuid(ICreateTypeLib2 * iface, REFGUID guid)
{
    ICOM_THIS(ICreateTypeLib2Impl, iface);

    MSFT_GuidEntry guidentry;
    int offset;

    TRACE("(%p,%s)\n", iface, debugstr_guid(guid));

    guidentry.guid = *guid;
    guidentry.unk10 = -2;
    guidentry.unk14 = -1;

    offset = ctl2_alloc_guid(This, &guidentry);
    
    if (offset == -1) return E_OUTOFMEMORY;

    This->typelib_header.posguid = offset;

    return S_OK;
}

/******************************************************************************
 * ICreateTypeLib2_SetDocString {OLEAUT32}
 *
 *  See ICreateTypeLib_SetDocString.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSetDocString(ICreateTypeLib2 * iface, LPOLESTR szDoc)
{
    ICOM_THIS(ICreateTypeLib2Impl, iface);

    int offset;

    TRACE("(%p,%s)\n", iface, debugstr_w(szDoc));

    offset = ctl2_alloc_string(This, szDoc);
    if (offset == -1) return E_OUTOFMEMORY;
    This->typelib_header.helpstring = offset;
    return S_OK;
}

/******************************************************************************
 * ICreateTypeLib2_SetHelpFileName {OLEAUT32}
 *
 *  See ICreateTypeLib_SetHelpFileName.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSetHelpFileName(ICreateTypeLib2 * iface, LPOLESTR szHelpFileName)
{
    ICOM_THIS(ICreateTypeLib2Impl, iface);

    int offset;

    TRACE("(%p,%s)\n", iface, debugstr_w(szHelpFileName));

    offset = ctl2_alloc_string(This, szHelpFileName);
    if (offset == -1) return E_OUTOFMEMORY;
    This->typelib_header.helpfile = offset;
    This->typelib_header.varflags |= 0x10;
    return S_OK;
}

/******************************************************************************
 * ICreateTypeLib2_SetHelpContext {OLEAUT32}
 *
 *  See ICreateTypeLib_SetHelpContext.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSetHelpContext(ICreateTypeLib2 * iface, DWORD dwHelpContext)
{
    FIXME("(%p,%ld), stub!\n", iface, dwHelpContext);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeLib2_SetLcid {OLEAUT32}
 *
 *  See ICreateTypeLib_SetLcid.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSetLcid(ICreateTypeLib2 * iface, LCID lcid)
{
    ICOM_THIS(ICreateTypeLib2Impl, iface);

    TRACE("(%p,%ld)\n", iface, lcid);

    This->typelib_header.lcid2 = lcid;

    return S_OK;
}

/******************************************************************************
 * ICreateTypeLib2_SetLibFlags {OLEAUT32}
 *
 *  See ICreateTypeLib_SetLibFlags.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSetLibFlags(ICreateTypeLib2 * iface, UINT uLibFlags)
{
    ICOM_THIS(ICreateTypeLib2Impl, iface);

    TRACE("(%p,0x%x)\n", iface, uLibFlags);

    This->typelib_header.flags = uLibFlags;

    return S_OK;
}

static int ctl2_write_chunk(HANDLE hFile, void *segment, int length)
{
    if (!WriteFile(hFile, segment, length, NULL, 0)) {CloseHandle(hFile); return 0;}
    return -1;
}

static int ctl2_write_segment(ICreateTypeLib2Impl *This, HANDLE hFile, int segment)
{
    if (!WriteFile(hFile, This->typelib_segment_data[segment],
		   This->typelib_segdir[segment].length, NULL, 0)) {
	CloseHandle(hFile);
	return 0;
    }

    return -1;
}

static void ctl2_finalize_typeinfos(ICreateTypeLib2Impl *This, int filesize)
{
    ICreateTypeInfo2Impl *typeinfo;

    for (typeinfo = This->typeinfos; typeinfo; typeinfo = typeinfo->next_typeinfo) {
	typeinfo->typeinfo->memoffset = filesize;
	if (typeinfo->typedata) {
	    ICreateTypeInfo2_fnLayOut((ICreateTypeInfo2 *)typeinfo);
	    filesize += typeinfo->typedata[0] + ((typeinfo->typeinfo->cElement >> 16) * 12) + ((typeinfo->typeinfo->cElement & 0xffff) * 12) + 4;
	}
    }
}

static int ctl2_finalize_segment(ICreateTypeLib2Impl *This, int filepos, int segment)
{
    if (This->typelib_segdir[segment].length) {
	This->typelib_segdir[segment].offset = filepos;
    } else {
	This->typelib_segdir[segment].offset = -1;
    }

    return This->typelib_segdir[segment].length;
}

static void ctl2_write_typeinfos(ICreateTypeLib2Impl *This, HANDLE hFile)
{
    ICreateTypeInfo2Impl *typeinfo;

    for (typeinfo = This->typeinfos; typeinfo; typeinfo = typeinfo->next_typeinfo) {
	if (!typeinfo->typedata) continue;

	ctl2_write_chunk(hFile, typeinfo->typedata, typeinfo->typedata[0] + 4);
	ctl2_write_chunk(hFile, typeinfo->indices, ((typeinfo->typeinfo->cElement & 0xffff) + (typeinfo->typeinfo->cElement >> 16)) * 4);
	ctl2_write_chunk(hFile, typeinfo->names, ((typeinfo->typeinfo->cElement & 0xffff) + (typeinfo->typeinfo->cElement >> 16)) * 4);
	ctl2_write_chunk(hFile, typeinfo->offsets, ((typeinfo->typeinfo->cElement & 0xffff) + (typeinfo->typeinfo->cElement >> 16)) * 4);
    }
}

/******************************************************************************
 * ICreateTypeLib2_SaveAllChanges {OLEAUT32}
 *
 *  See ICreateTypeLib_SaveAllChanges.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSaveAllChanges(ICreateTypeLib2 * iface)
{
    ICOM_THIS( ICreateTypeLib2Impl, iface);

    int retval;
    int filepos;
    HANDLE hFile;

    TRACE("(%p)\n", iface);

    retval = TYPE_E_IOERROR;

    hFile = CreateFileW(This->filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (hFile == INVALID_HANDLE_VALUE) return retval;

    filepos = sizeof(MSFT_Header) + sizeof(MSFT_SegDir);
    filepos += This->typelib_header.nrtypeinfos * 4;

    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_TYPEINFO);
    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_GUIDHASH);
    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_GUID);
    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_IMPORTINFO);
    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_IMPORTFILES);
    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_NAMEHASH);
    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_NAME);
    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_STRING);
    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_TYPEDESC);
    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_ARRAYDESC);
    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_CUSTDATA);
    filepos += ctl2_finalize_segment(This, filepos, MSFT_SEG_CUSTDATAGUID);

    ctl2_finalize_typeinfos(This, filepos);

    if (!ctl2_write_chunk(hFile, &This->typelib_header, sizeof(This->typelib_header))) return retval;
    if (!ctl2_write_chunk(hFile, This->typelib_typeinfo_offsets, This->typelib_header.nrtypeinfos * 4)) return retval;
    if (!ctl2_write_chunk(hFile, &This->typelib_segdir, sizeof(This->typelib_segdir))) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_TYPEINFO    )) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_GUIDHASH    )) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_GUID        )) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_IMPORTINFO  )) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_IMPORTFILES )) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_NAMEHASH    )) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_NAME        )) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_STRING      )) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_TYPEDESC    )) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_ARRAYDESC   )) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_CUSTDATA    )) return retval;
    if (!ctl2_write_segment(This, hFile, MSFT_SEG_CUSTDATAGUID)) return retval;

    ctl2_write_typeinfos(This, hFile);

    if (!CloseHandle(hFile)) return retval;

    retval = S_OK;
    return retval;
}


/******************************************************************************
 * ICreateTypeLib2_DeleteTypeInfo {OLEAUT32}
 *
 *  Deletes a named TypeInfo from a type library.
 *
 * RETURNS
 *
 *  Success: S_OK
 *  Failure: E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeLib2_fnDeleteTypeInfo(
	ICreateTypeLib2 * iface, /* [I] The type library to delete from. */
	LPOLESTR szName)         /* [I] The name of the typeinfo to delete. */
{
    FIXME("(%p,%s), stub!\n", iface, debugstr_w(szName));
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeLib2_SetCustData {OLEAUT32}
 *
 *  Sets custom data for a type library.
 *
 * RETURNS
 *
 *  Success: S_OK
 *  Failure: E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSetCustData(
	ICreateTypeLib2 * iface, /* [I] The type library to store the custom data in. */
	REFGUID guid,            /* [I] The GUID used as a key to retrieve the custom data. */
	VARIANT *pVarVal)        /* [I] The custom data itself. */
{
    FIXME("(%p,%s,%p), stub!\n", iface, debugstr_guid(guid), pVarVal);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeLib2_SetHelpStringContext {OLEAUT32}
 *
 *  Sets a context number for the library help string.
 *
 * RETURNS
 *
 *  Success: S_OK
 *  Failure: E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSetHelpStringContext(
	ICreateTypeLib2 * iface,   /* [I] The type library to set the help string context for. */
	ULONG dwHelpStringContext) /* [I] The help string context. */
{
    FIXME("(%p,%ld), stub!\n", iface, dwHelpStringContext);
    return E_OUTOFMEMORY;
}

/******************************************************************************
 * ICreateTypeLib2_SetHelpStringDll {OLEAUT32}
 *
 *  Sets the DLL used to look up localized help strings.
 *
 * RETURNS
 *
 *  Success: S_OK
 *  Failure: E_OUTOFMEMORY or E_INVALIDARG.
 */
static HRESULT WINAPI ICreateTypeLib2_fnSetHelpStringDll(
	ICreateTypeLib2 * iface, /* [I] The type library to set the help DLL for. */
	LPOLESTR szFileName)     /* [I] The name of the help DLL. */
{
    FIXME("(%p,%s), stub!\n", iface, debugstr_w(szFileName));
    return E_OUTOFMEMORY;
}


static ICOM_VTABLE(ICreateTypeLib2) ctypelib2vt =
{
    ICOM_MSVTABLE_COMPAT_DummyRTTIVALUE

    ICreateTypeLib2_fnQueryInterface,
    ICreateTypeLib2_fnAddRef,
    ICreateTypeLib2_fnRelease,

    ICreateTypeLib2_fnCreateTypeInfo,
    ICreateTypeLib2_fnSetName,
    ICreateTypeLib2_fnSetVersion,
    ICreateTypeLib2_fnSetGuid,
    ICreateTypeLib2_fnSetDocString,
    ICreateTypeLib2_fnSetHelpFileName,
    ICreateTypeLib2_fnSetHelpContext,
    ICreateTypeLib2_fnSetLcid,
    ICreateTypeLib2_fnSetLibFlags,
    ICreateTypeLib2_fnSaveAllChanges,

    ICreateTypeLib2_fnDeleteTypeInfo,
    ICreateTypeLib2_fnSetCustData,
    ICreateTypeLib2_fnSetHelpStringContext,
    ICreateTypeLib2_fnSetHelpStringDll
};

static ICreateTypeLib2 *ICreateTypeLib2_Constructor(SYSKIND syskind, LPCOLESTR szFile)
{
    ICreateTypeLib2Impl *pCreateTypeLib2Impl;
    int failed = 0;

    TRACE("Constructing ICreateTypeLib2 (%d, %s)\n", syskind, debugstr_w(szFile));

    pCreateTypeLib2Impl = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(ICreateTypeLib2Impl));
    if (!pCreateTypeLib2Impl) return NULL;

    pCreateTypeLib2Impl->filename = HeapAlloc(GetProcessHeap(), 0, (strlenW(szFile) + 1) * sizeof(WCHAR));
    if (!pCreateTypeLib2Impl->filename) {
	HeapFree(GetProcessHeap(), 0, pCreateTypeLib2Impl);
	return NULL;
    }
    strcpyW(pCreateTypeLib2Impl->filename, szFile);

    ctl2_init_header(pCreateTypeLib2Impl);
    ctl2_init_segdir(pCreateTypeLib2Impl);

    /*
     * The following two calls return an offset or -1 if out of memory. We
     * specifically need an offset of 0, however, so...
     */
    if (ctl2_alloc_segment(pCreateTypeLib2Impl, MSFT_SEG_GUIDHASH, 0x80, 0x80)) { failed = 1; }
    if (ctl2_alloc_segment(pCreateTypeLib2Impl, MSFT_SEG_NAMEHASH, 0x200, 0x200)) { failed = 1; }

    pCreateTypeLib2Impl->typelib_guidhash_segment = (int *)pCreateTypeLib2Impl->typelib_segment_data[MSFT_SEG_GUIDHASH];
    pCreateTypeLib2Impl->typelib_namehash_segment = (int *)pCreateTypeLib2Impl->typelib_segment_data[MSFT_SEG_NAMEHASH];

    memset(pCreateTypeLib2Impl->typelib_guidhash_segment, 0xff, 0x80);
    memset(pCreateTypeLib2Impl->typelib_namehash_segment, 0xff, 0x200);

    pCreateTypeLib2Impl->lpVtbl = &ctypelib2vt;
    pCreateTypeLib2Impl->ref = 1;

    if (failed) {
	ICreateTypeLib2_fnRelease((ICreateTypeLib2 *)pCreateTypeLib2Impl);
	return 0;
    }

    return (ICreateTypeLib2 *)pCreateTypeLib2Impl;
}

/******************************************************************************
 * CreateTypeLib2 [OLEAUT32.180]
 *
 *  Obtains an ICreateTypeLib2 object for creating a new-style (MSFT) type
 *  library.
 *
 * NOTES
 *
 *  See also CreateTypeLib.
 *
 * RETURNS
 *    Success: S_OK
 *    Failure: Status
 */
HRESULT WINAPI CreateTypeLib2(
	SYSKIND syskind,           /* [I] System type library is for */
	LPCOLESTR szFile,          /* [I] Type library file name */
	ICreateTypeLib2** ppctlib) /* [O] Storage for object returned */
{
    TRACE("(%d,%s,%p)\n", syskind, debugstr_w(szFile), ppctlib);

    if (!szFile) return E_INVALIDARG;
    *ppctlib = ICreateTypeLib2_Constructor(syskind, szFile);
    return (*ppctlib)? S_OK: E_OUTOFMEMORY;
}
