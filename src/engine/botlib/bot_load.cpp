/*
===========================================================================

Daemon BSD Source Code
Copyright (c) 2013 Daemon Developers
All rights reserved.

This file is part of the Daemon BSD Source Code (Daemon Source Code).

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Daemon developers nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL DAEMON DEVELOPERS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS

===========================================================================
*/

#include "common/Common.h"

#include "DetourAssert.h"

#include "bot_local.h"
#include "nav.h"

int numNavData = 0;
NavData_t BotNavData[ MAX_NAV_DATA ];

LinearAllocator alloc( 1024 * 1024 * 16 );
FastLZCompressor comp;

// Recast uses NDEBUG to determine whether assertions are enabled.
// Make sure this is in sync with DEBUG_BUILD
#if defined(DEBUG_BUILD) != !defined(NDEBUG)
#error
#endif

#ifdef DEBUG_BUILD
static void FailAssertion(const char* expression, const char* file, int line)
{
	DAEMON_ASSERT_CALLSITE_HELPER(
		file, "Detour assert", line, , , false, Str::Format("\"%s\" is false", expression));
}
#endif

void BotInit()
{
#ifdef DEBUG_BUILD
	dtAssertFailSetCustom(FailAssertion);
#endif
}

void BotSaveOffMeshConnections( NavData_t *nav )
{
	char mapname[ MAX_QPATH ];
	char filePath[ MAX_QPATH ];
	fileHandle_t f = 0;

	Cvar_VariableStringBuffer( "mapname", mapname, sizeof( mapname ) );
	Com_sprintf( filePath, sizeof( filePath ), "maps/%s-%s.navcon", mapname, nav->name );
	f = FS_FOpenFileWrite( filePath );

	if ( !f )
	{
		return;
	}

	int conCount = nav->process.con.offMeshConCount;
	OffMeshConnectionHeader header;
	header.version = LittleLong( NAVMESHCON_VERSION );
	header.numConnections = LittleLong( conCount );
	FS_Write( &header, sizeof( header ), f );

	size_t size = sizeof( float ) * 6 * conCount;
	float *verts = ( float * ) dtAlloc( size, DT_ALLOC_TEMP );
	memcpy( verts, nav->process.con.verts, size );
	SwapArray( verts, conCount * 6 );
	FS_Write( verts, size, f );
	dtFree( verts );

	size = sizeof( float ) * conCount;
	float *rad = ( float * ) dtAlloc( size, DT_ALLOC_TEMP );
	memcpy( rad, nav->process.con.rad, size );
	SwapArray( rad, conCount );
	FS_Write( rad, size, f );
	dtFree( rad );

	size = sizeof( unsigned short ) * conCount;
	unsigned short *flags = ( unsigned short * ) dtAlloc( size, DT_ALLOC_TEMP );
	memcpy( flags, nav->process.con.flags, size );
	SwapArray( flags, conCount );
	FS_Write( flags, size, f );
	dtFree( flags );

	FS_Write( nav->process.con.areas, sizeof( unsigned char ) * conCount, f );
	FS_Write( nav->process.con.dirs, sizeof( unsigned char ) * conCount, f );

	size = sizeof( unsigned int ) * conCount;
	unsigned int *userids = ( unsigned int * ) dtAlloc( size, DT_ALLOC_TEMP );
	memcpy( userids, nav->process.con.userids, size );
	SwapArray( userids, conCount );
	FS_Write( userids, size, f );
	dtFree( userids );

	FS_FCloseFile( f );
}

void BotLoadOffMeshConnections( const char *filename, NavData_t *nav )
{
	char mapname[ MAX_QPATH ];
	char filePath[ MAX_QPATH ];
	fileHandle_t f = 0;

	Cvar_VariableStringBuffer( "mapname", mapname, sizeof( mapname ) );
	Com_sprintf( filePath, sizeof( filePath ), "maps/%s-%s.navcon", mapname, filename );
	FS_FOpenFileRead( filePath, &f, true );

	if ( !f )
	{
		return;
	}

	OffMeshConnectionHeader header;
	FS_Read( &header, sizeof( header ), f );

	header.version = LittleLong( header.version );
	header.numConnections = LittleLong( header.numConnections );

	if ( header.version != NAVMESHCON_VERSION )
	{
		FS_FCloseFile( f );
		return;
	}

	int conCount = header.numConnections;

	if ( conCount > nav->process.con.MAX_CON )
	{
		FS_FCloseFile( f );
		return;
	}

	nav->process.con.offMeshConCount = conCount;

	FS_Read( nav->process.con.verts, sizeof( float ) * 6 * conCount, f );
	SwapArray( nav->process.con.verts, conCount * 6 );

	FS_Read( nav->process.con.rad, sizeof( float ) * conCount, f );
	SwapArray( nav->process.con.rad, conCount );

	FS_Read( nav->process.con.flags, sizeof( unsigned short ) * conCount, f );
	SwapArray( nav->process.con.flags, conCount );

	FS_Read( nav->process.con.areas, sizeof( unsigned char ) * conCount, f );
	FS_Read( nav->process.con.dirs, sizeof( unsigned char ) * conCount, f );

	FS_Read( nav->process.con.userids, sizeof( unsigned int ) * conCount, f );
	SwapArray( nav->process.con.userids, conCount );

	FS_FCloseFile( f );
}

bool BotLoadNavMesh( const char *filename, NavData_t &nav )
{
	char mapname[ MAX_QPATH ];
	char filePath[ MAX_QPATH ];
	char gameName[ MAX_STRING_CHARS ];
	fileHandle_t f = 0;

	BotLoadOffMeshConnections( filename, &nav );

	Cvar_VariableStringBuffer( "mapname", mapname, sizeof( mapname ) );
	Cvar_VariableStringBuffer( "fs_game", gameName, sizeof( gameName ) );
	Com_sprintf( filePath, sizeof( filePath ), "maps/%s-%s.navMesh", mapname, filename );
	Log::Notice( " loading navigation mesh file '%s'...", filePath );

	int len = FS_FOpenFileRead( filePath, &f, true );

	if ( !f )
	{
		Log::Warn("Cannot open Navigation Mesh file" );
		return false;
	}

	if ( len < 0 )
	{
		Log::Warn("Negative Length for Navigation Mesh file");
		return false;
	}

	NavMeshSetHeader header;
	
	FS_Read( &header, sizeof( header ), f );

	SwapNavMeshSetHeader( header );

	if ( header.magic != NAVMESHSET_MAGIC )
	{
		Log::Warn("File is wrong magic" );
		FS_FCloseFile( f );
		return false;
	}

	if ( header.version != NAVMESHSET_VERSION )
	{
		Log::Warn("File is wrong version found: %d want: %d", header.version, NAVMESHSET_VERSION );
		FS_FCloseFile( f );
		return false;
	}

	nav.mesh = dtAllocNavMesh();

	if ( !nav.mesh )
	{
		Log::Warn("Unable to allocate nav mesh" );
		FS_FCloseFile( f );
		return false;
	}

	dtStatus status = nav.mesh->init( &header.params );

	if ( dtStatusFailed( status ) )
	{
		Log::Warn("Could not init navmesh" );
		dtFreeNavMesh( nav.mesh );
		nav.mesh = nullptr;
		FS_FCloseFile( f );
		return false;
	}

	nav.cache = dtAllocTileCache();

	if ( !nav.cache )
	{
		Log::Warn("Could not allocate tile cache" );
		dtFreeNavMesh( nav.mesh );
		nav.mesh = nullptr;
		FS_FCloseFile( f );
		return false;
	}

	status = nav.cache->init( &header.cacheParams, &alloc, &comp, &nav.process );

	if ( dtStatusFailed( status ) )
	{
		Log::Warn("Could not init tile cache" );
		dtFreeNavMesh( nav.mesh );
		dtFreeTileCache( nav.cache );
		nav.mesh = nullptr;
		nav.cache = nullptr;
		FS_FCloseFile( f );
		return false;
	}

	for ( int i = 0; i < header.numTiles; i++ )
	{
		NavMeshTileHeader tileHeader;

		FS_Read( &tileHeader, sizeof( tileHeader ), f );

		SwapNavMeshTileHeader( tileHeader );

		if ( !tileHeader.tileRef || !tileHeader.dataSize )
		{
			Log::Warn("Null Tile in navmesh" );
			dtFreeNavMesh( nav.mesh );
			dtFreeTileCache( nav.cache );
			nav.cache = nullptr;
			nav.mesh = nullptr;
			FS_FCloseFile( f );
			return false;
		}

		unsigned char *data = ( unsigned char * ) dtAlloc( tileHeader.dataSize, DT_ALLOC_PERM );

		if ( !data )
		{
			Log::Warn("Failed to allocate memory for tile data" );
			dtFreeNavMesh( nav.mesh );
			dtFreeTileCache( nav.cache );
			nav.cache = nullptr;
			nav.mesh = nullptr;
			FS_FCloseFile( f );
			return false;
		}

		memset( data, 0, tileHeader.dataSize );

		FS_Read( data, tileHeader.dataSize, f );

		if ( LittleLong( 1 ) != 1 )
		{
			dtTileCacheHeaderSwapEndian( data, tileHeader.dataSize );
		}

		dtCompressedTileRef tile = 0;
		dtStatus status = nav.cache->addTile( data, tileHeader.dataSize, DT_TILE_FREE_DATA, &tile );

		if ( dtStatusFailed( status ) )
		{
			Log::Warn("Failed to add tile to navmesh" );
			dtFree( data );
			dtFreeTileCache( nav.cache );
			dtFreeNavMesh( nav.mesh );
			nav.cache = nullptr;
			nav.mesh = nullptr;
			FS_FCloseFile( f );
			return false;
		}

		if ( tile )
		{
			nav.cache->buildNavMeshTile( tile, nav.mesh );
		}
	}

	FS_FCloseFile( f );
	return true;
}

inline void *dtAllocCustom( size_t size, dtAllocHint )
{
	return Z_TagMalloc( size, memtag_t::TAG_BOTLIB );
}

inline void dtFreeCustom( void *ptr )
{
	Z_Free( ptr );
}

void BotShutdownNav()
{
	for ( int i = 0; i < numNavData; i++ )
	{
		NavData_t *nav = &BotNavData[ i ];

		if ( nav->cache )
		{
			dtFreeTileCache( nav->cache );
			nav->cache = nullptr;
		}

		if ( nav->mesh )
		{
			dtFreeNavMesh( nav->mesh );
			nav->mesh = nullptr;
		}

		if ( nav->query )
		{
			dtFreeNavMeshQuery( nav->query );
			nav->query = nullptr;
		}

		nav->process.con.reset();
		memset( nav->name, 0, sizeof( nav->name ) );
	}

#ifndef BUILD_SERVER
	NavEditShutdown();
#endif
	numNavData = 0;
}

bool BotSetupNav( const botClass_t *botClass, qhandle_t *navHandle )
{
	cvar_t *maxNavNodes = Cvar_Get( "bot_maxNavNodes", "4096",  CVAR_LATCH );

	if ( !numNavData )
	{
		vec3_t clearVec = { 0, 0, 0 };

		dtAllocSetCustom( dtAllocCustom, dtFreeCustom );

		for ( int i = 0; i < MAX_CLIENTS; i++ )
		{
			// should only init the corridor once
			if ( !agents[ i ].corridor.getPath() )
			{
				if ( !agents[ i ].corridor.init( MAX_BOT_PATH ) )
				{
					return false;
				}
			}

			agents[ i ].corridor.reset( 0, clearVec );
			agents[ i ].clientNum = i;
			agents[ i ].needReplan = true;
			agents[ i ].nav = nullptr;
			agents[ i ].offMesh = false;
			memset( agents[ i ].routeResults, 0, sizeof( agents[ i ].routeResults ) );
		}
#ifndef BUILD_SERVER
		NavEditInit();
#endif
	}

	if ( numNavData == MAX_NAV_DATA )
	{
		Log::Warn( "maximum number of navigation meshes exceeded" );
		return false;
	}

	NavData_t *nav = &BotNavData[ numNavData ];
	const char *filename = botClass->name;

	if ( !BotLoadNavMesh( filename, *nav ) )
	{
		BotShutdownNav();
		return false;
	}

	Q_strncpyz( nav->name, botClass->name, sizeof( nav->name ) );
	nav->query = dtAllocNavMeshQuery();

	if ( !nav->query )
	{
		Log::Notice( "Could not allocate Detour Navigation Mesh Query for navmesh %s", filename );
		BotShutdownNav();
		return false;
	}

	if ( dtStatusFailed( nav->query->init( nav->mesh, maxNavNodes->integer ) ) )
	{
		Log::Notice( "Could not init Detour Navigation Mesh Query for navmesh %s", filename );
		BotShutdownNav();
		return false;
	}

	nav->filter.setIncludeFlags( botClass->polyFlagsInclude );
	nav->filter.setExcludeFlags( botClass->polyFlagsExclude );
	*navHandle = numNavData;
	numNavData++;
	return true;
}
