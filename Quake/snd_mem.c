/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2010-2011 O. Sezer <sezero@users.sourceforge.net>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

// snd_mem.c: sound caching

#include "quakedef.h"

/*
================
ResampleSfx
================
*/
static void ResampleSfx (sfx_t *sfx, int inrate, int inwidth, byte *data)
{
	int		outcount;
	int		srcsample;
	float	stepscale;
	int		i;
	int		sample, samplefrac, fracstep;
	sfxcache_t	*sc;

	sc = (sfxcache_t *) Cache_Check (&sfx->cache);
	if (!sc)
		return;

	stepscale = (float)inrate / shm->speed;	// this is usually 0.5, 1, or 2

	outcount = sc->length / stepscale;
	sc->length = outcount;
	if (sc->loopstart != -1)
		sc->loopstart = sc->loopstart / stepscale;

	sc->speed = shm->speed;
	if (loadas8bit.value)
		sc->width = 1;
	else
		sc->width = inwidth;
	sc->stereo = 0;

// resample / decimate to the current source rate

	if (stepscale == 1 && inwidth == 1 && sc->width == 1)
	{
// fast special case
		for (i = 0; i < outcount; i++)
			((signed char *)sc->data)[i] = (int)( (unsigned char)(data[i]) - 128);
	}
	else
	{
// general case
		srcsample = samplefrac = 0;
		fracstep = stepscale*256;
		for (i = 0; i < outcount; i++)
		{
			if (inwidth == 2)
				sample = LittleShort ( ((short *)data)[srcsample] );
			else
				sample = (int)( (unsigned char)(data[srcsample]) - 128) << 8;
			if (sc->width == 2)
				((short *)sc->data)[i] = sample;
			else
				((signed char *)sc->data)[i] = sample >> 8;
			samplefrac += fracstep;
			srcsample += samplefrac >> 8;
			samplefrac &= 255;
		}
	}
}

//=============================================================================

/*
==============
S_LoadSound
==============
*/
sfxcache_t *S_LoadSound (sfx_t *s)
{
	char	namebuffer[256];
	byte	*data;
	wavinfo_t	info;
	uint64_t	len;
	sfxcache_t	*sc;

// see if still in memory
	sc = (sfxcache_t *) Cache_Check (&s->cache);
	if (sc)
		return sc;

//	Con_Printf ("S_LoadSound: %x\n", (int)stackbuf);

// load it in
	q_strlcpy(namebuffer, "sound/", sizeof(namebuffer));
	q_strlcat(namebuffer, s->name, sizeof(namebuffer));

//	Con_Printf ("loading %s\n",namebuffer);

	data = COM_LoadMallocFile (namebuffer, NULL);

	if (!data)
	{
		Con_Printf ("Couldn't load %s\n", namebuffer);
		return NULL;
	}

	info = GetWavinfo (s->name, data, com_filesize);
	if (info.samples < 1)
	{
		free (data);
		return NULL;
	}

	len = (uint64_t)info.samples * shm->speed / info.rate;
	if (len == 0)
	{
		free (data);
		Con_Printf("%s has zero samples\n", s->name);
		return NULL;
	}

	len = len * info.width * info.channels;
	if (len > INT_MAX - sizeof(sfxcache_t))
	{
		free (data);
		Con_Printf("%s has too many samples\n", s->name);
		return NULL;
	}

	sc = (sfxcache_t *) Cache_Alloc ( &s->cache, len + sizeof(sfxcache_t), s->name);
	if (!sc)
	{
		free (data);
		return NULL;
	}

	sc->length = info.samples;
	sc->loopstart = info.loopstart;
	sc->speed = info.rate;
	sc->width = info.width;
	sc->stereo = info.channels;

	ResampleSfx (s, sc->speed, sc->width, data + info.dataofs);

	free (data);

	return sc;
}



/*
===============================================================================

WAV loading

===============================================================================
*/

#define TAG_RIFF	MakeLittleLong('R','I','F','F')
#define TAG_WAVE	MakeLittleLong('W','A','V','E')
#define TAG_fmt		MakeLittleLong('f','m','t',' ')
#define TAG_cue		MakeLittleLong('c','u','e',' ')
#define TAG_LIST	MakeLittleLong('L','I','S','T')
#define TAG_mark	MakeLittleLong('m','a','r','k')
#define TAG_data	MakeLittleLong('d','a','t','a')

static int FindChunk(sizebuf_t *sz, uint32_t search)
{
	while (sz->readcount + 8 < sz->cursize) {
		uint32_t chunk = SZ_ReadLong(sz);
		uint32_t len   = SZ_ReadLong(sz);

		len = q_min(len, sz->cursize - sz->readcount);
		if (chunk == search)
			return len;

		sz->readcount += Q_ALIGN(len, 2);
	}

	return 0;
}

/*
============
GetWavinfo
============
*/
wavinfo_t GetWavinfo (const char *name, byte *wav, int wavlength)
{
	int format, samples, width, chunk_len, next_chunk;
	wavinfo_t info;
	sizebuf_t sz;

	memset (&info, 0, sizeof(info));
	SZ_InitRead(&sz, wav, wavlength);

// find "RIFF" chunk
	if (SZ_ReadLong(&sz) != TAG_RIFF) {
		Con_Printf("%s is missing RIFF chunk\n", name);
		return info;
	}

	sz.readcount += 4;
	if (SZ_ReadLong(&sz) != TAG_WAVE) {
		Con_Printf("%s is missing WAVE chunk\n", name);
		return info;
	}

// save position after "WAVE" tag
	next_chunk = sz.readcount;

// find "fmt " chunk
	if (!FindChunk(&sz, TAG_fmt)) {
		Con_Printf("%s is missing fmt chunk\n", name);
		return info;
	}

	format = SZ_ReadShort(&sz);
	if (format != WAV_FORMAT_PCM) {
		Con_Printf("%s is not Microsoft PCM format\n", name);
		return info;
	}

	info.channels = SZ_ReadShort(&sz);
	if (info.channels != 1) {
		Con_Printf("%s has %d channels\n", name, info.channels);
		return info;
	}

	info.rate = SZ_ReadLong(&sz);
	if (info.rate < 1) {
		Con_Printf("%s has bad sample rate\n", name);
		return info;
	}

	sz.readcount += 6;
	width = SZ_ReadShort(&sz);
	switch (width) {
	case 8:
	case 16:
		info.width = width / 8;
		break;
	default:
		Con_Printf("%s is not 8 or 16 bit\n", name);
		return info;
	}

// find "data" chunk
	sz.readcount = next_chunk;
	chunk_len = FindChunk(&sz, TAG_data);
	if (!chunk_len) {
		Con_Printf("%s is missing data chunk\n", name);
		return info;
	}

// calculate length in samples
	info.samples = chunk_len / (info.width * info.channels);
	if (info.samples < 1) {
		Con_Printf("%s has zero samples\n", name);
		return info;
	}

// any errors are non-fatal from this point
	info.dataofs = sz.readcount;
	info.loopstart = -1;

// find "cue " chunk
	sz.readcount = next_chunk;
	chunk_len = FindChunk(&sz, TAG_cue);
	if (!chunk_len)
		return info;

// save position after "cue " chunk
	next_chunk = sz.readcount + Q_ALIGN(chunk_len, 2);

	sz.readcount += 24;
	samples = SZ_ReadLong(&sz);
	if (samples < 0 || samples >= info.samples) {
		Con_Warning("%s has bad loop start\n", name);
		return info;
	}
	info.loopstart = samples;

// if the next chunk is a "LIST" chunk, look for a cue length marker
	sz.readcount = next_chunk;
	if (!FindChunk(&sz, TAG_LIST))
		return info;

	sz.readcount += 20;
	if (SZ_ReadLong(&sz) != TAG_mark)
		return info;

// this is not a proper parse, but it works with cooledit...
	sz.readcount -= 8;
	samples = SZ_ReadLong(&sz);  // samples in loop
	if (samples < 1 || samples > info.samples - info.loopstart) {
		Con_Warning("%s has bad loop length\n", name);
		return info;
	}
	info.samples = info.loopstart + samples;

	return info;
}

