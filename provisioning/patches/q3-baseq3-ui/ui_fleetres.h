/*
===========================================================================
ui_fleetres.h - the "Video Mode:" list of the q3_ui SYSTEM menu, rebuilt so
that the mode the game is really running in is always listed and selected.

Copyright (C) 1999-2005 Id Software, Inc. (the q3_ui it extends)
Copyright (C) 2026 retro-agent fleet (this file)

This file is part of the Quake III Arena source code, as modified for the
retro-agent fleet's baseq3/zz-fleetres-ui.pk3.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
===========================================================================

WHY: id's 1.32 menu (baseq3/pak8.pk3 vm/ui.qvm) offers a fixed table of
twelve modes, r_mode 0..11.  The fleet launcher runs every box at the
monitor's own size through r_mode -1 + r_customwidth/r_customheight
(1920x1080 on the LCDs), which that table cannot show: the menu displayed
"640x480" and any ACCEPT wrote r_mode 3, dropping the game to 640x480.

WHAT THIS DOES (everything else in the menu is id's code, unchanged):
  - the list is r_availableModes when the engine publishes it (ioquake3
    does: the modes the display driver enumerated), as ioquake3's own
    q3_ui does;
  - otherwise it is id's table, in id's order, so r_mode N is entry N
    (retail quake3.exe 1.32c publishes no mode list);
  - the mode the game is running in is always in the list and is what is
    selected: with r_mode -1 that is r_customwidth x r_customheight,
    appended as "WxH" when no entry has that size;
  - ACCEPT with that entry still selected writes back exactly the mode
    that is running (r_mode -1 keeps r_mode -1 and its custom size); any
    other entry is written as r_mode N when it is one of id's sizes, and
    as r_mode -1 plus r_customwidth/r_customheight otherwise.

This header is PURE LOGIC: no trap_ calls, no engine types.  ui_video.c
feeds it cvar values and reads the list back, and the retro-agent test
suite compiles this same file natively (tests/python/
test_patch_q3-baseq3-ui.py) - so keep it free of q_shared/bg_lib calls.
It is written for lcc (the Q3 VM compiler): C89, declarations first,
block comments only.
*/

#ifndef UI_FLEETRES_H
#define UI_FLEETRES_H

#define FR_NUM_BUILTIN		12	/* id's r_mode 0..11 */
#define FR_MAX_DETECTED		32	/* entries taken from r_availableModes */
#define FR_MAX_ENTRIES		(FR_MAX_DETECTED + FR_NUM_BUILTIN + 1)
#define FR_MAX_LABELS		4	/* "WxH" labels this module writes itself */
#define FR_LABEL_LEN		16
#define FR_MODES_LEN		1024	/* MAX_STRING_CHARS */
#define FR_NO_ENTRY			-1000	/* no such entry / nothing to write: never a real r_mode */

typedef struct {
	char		modes[FR_MODES_LEN];		/* copy of r_availableModes, split in place */
	char		labels[FR_MAX_LABELS][FR_LABEL_LEN];
	int			numLabels;
	const char	*names[FR_MAX_ENTRIES + 1];	/* the spin control's itemnames, NULL-terminated */
	int			mode[FR_MAX_ENTRIES];		/* the r_mode an entry means: 0..11, or -1 */
	int			width[FR_MAX_ENTRIES];
	int			height[FR_MAX_ENTRIES];
	int			count;
	int			detected;					/* 1 = the list came from r_availableModes */
	int			current;					/* the entry the game is running in */
	int			runMode;					/* the r_mode the game is running in ... */
	int			runW, runH;					/* ... and its size (0 when unknown) */
} frModeList_t;

/* id's 1.32 table, labels exactly as id wrote them (ui_video.c resolutions[]) */
static const char *fr_builtinNames[FR_NUM_BUILTIN + 1] =
{
	"320x240",
	"400x300",
	"512x384",
	"640x480",
	"800x600",
	"960x720",
	"1024x768",
	"1152x864",
	"1280x1024",
	"1600x1200",
	"2048x1536",
	"856x480 wide screen",
	0
};

/*
=================
FR_ParseSize

"1920x1080" (or "856x480 wide screen") -> 1920, 1080.  Returns 1 only for
a well-formed size with both numbers in 1..99999.
=================
*/
static int FR_ParseSize( const char *s, int *w, int *h ) {
	int		a, b, digits;

	a = 0;
	digits = 0;
	while ( *s >= '0' && *s <= '9' ) {
		if ( a < 100000 ) {
			a = a * 10 + ( *s - '0' );
		}
		s++;
		digits++;
	}
	if ( !digits || ( *s != 'x' && *s != 'X' ) ) {
		return 0;
	}
	s++;
	b = 0;
	digits = 0;
	while ( *s >= '0' && *s <= '9' ) {
		if ( b < 100000 ) {
			b = b * 10 + ( *s - '0' );
		}
		s++;
		digits++;
	}
	if ( !digits || ( *s != 0 && *s != ' ' ) ) {
		return 0;
	}
	if ( a <= 0 || b <= 0 || a >= 100000 || b >= 100000 ) {
		return 0;
	}
	*w = a;
	*h = b;
	return 1;
}

/*
=================
FR_BuiltinMode

The r_mode (0..11) whose size is w x h, or -1.
=================
*/
static int FR_BuiltinMode( int w, int h ) {
	int		i, bw, bh;

	for ( i = 0; i < FR_NUM_BUILTIN; i++ ) {
		if ( FR_ParseSize( fr_builtinNames[i], &bw, &bh ) && bw == w && bh == h ) {
			return i;
		}
	}
	return -1;
}

/*
=================
FR_Add

Appends an entry; returns its index, or -1 when the list is full.
=================
*/
static int FR_Add( frModeList_t *l, const char *name, int mode, int w, int h ) {
	if ( l->count >= FR_MAX_ENTRIES ) {
		return -1;
	}
	l->names[l->count] = name;
	l->mode[l->count] = mode;
	l->width[l->count] = w;
	l->height[l->count] = h;
	l->count++;
	l->names[l->count] = 0;
	return l->count - 1;
}

/*
=================
FR_Find

The first entry of size w x h, or -1.
=================
*/
static int FR_Find( const frModeList_t *l, int w, int h ) {
	int		i;

	for ( i = 0; i < l->count; i++ ) {
		if ( l->width[i] == w && l->height[i] == h ) {
			return i;
		}
	}
	return -1;
}

/*
=================
FR_AddLabel

Appends a custom "WxH" entry (r_mode -1) whose label this module owns.
=================
*/
static int FR_AddLabel( frModeList_t *l, int w, int h ) {
	char	*p;
	char	tmp[12];
	int		n, v;

	if ( l->numLabels >= FR_MAX_LABELS || l->count >= FR_MAX_ENTRIES ) {
		return -1;
	}
	if ( w <= 0 || h <= 0 || w >= 100000 || h >= 100000 ) {
		return -1;
	}
	p = l->labels[l->numLabels];
	v = w;
	n = 0;
	do {
		tmp[n++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v );
	while ( n ) {
		*p++ = tmp[--n];
	}
	*p++ = 'x';
	v = h;
	n = 0;
	do {
		tmp[n++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v );
	while ( n ) {
		*p++ = tmp[--n];
	}
	*p = 0;
	l->numLabels++;
	return FR_Add( l, l->labels[l->numLabels - 1], -1, w, h );
}

/*
=================
FR_Build

Fills the list from r_availableModes ("1920x1080 1680x1050 ..."), or with
id's table when that is empty or holds nothing usable.
=================
*/
static void FR_Build( frModeList_t *l, const char *availableModes ) {
	int		i, w, h;
	char	*s, *start;

	l->count = 0;
	l->numLabels = 0;
	l->detected = 0;
	l->current = 0;
	l->runMode = FR_NO_ENTRY;
	l->runW = 0;
	l->runH = 0;
	l->names[0] = 0;

	i = 0;
	if ( availableModes ) {
		while ( availableModes[i] && i < FR_MODES_LEN - 1 ) {
			l->modes[i] = availableModes[i];
			i++;
		}
	}
	l->modes[i] = 0;

	s = l->modes;
	while ( *s && l->count < FR_MAX_DETECTED ) {
		while ( *s == ' ' ) {
			s++;
		}
		if ( !*s ) {
			break;
		}
		start = s;
		while ( *s && *s != ' ' ) {
			s++;
		}
		if ( *s ) {
			*s++ = 0;
		}
		if ( FR_ParseSize( start, &w, &h ) ) {
			FR_Add( l, start, FR_BuiltinMode( w, h ), w, h );
		}
	}

	if ( l->count > 0 ) {
		l->detected = 1;
		return;
	}

	for ( i = 0; i < FR_NUM_BUILTIN; i++ ) {
		FR_ParseSize( fr_builtinNames[i], &w, &h );
		FR_Add( l, fr_builtinNames[i], i, w, h );
	}
}

/*
=================
FR_Select

Finds the entry the game is running in - appending it when no entry has its
size - and remembers the running mode for FR_Accept.  r_mode -1 means
r_customwidth x r_customheight; 0..11 means id's size; any other r_mode (an
engine's own extra modes) is matched by the size the renderer reports
(glconfig vidWidth x vidHeight).
=================
*/
static int FR_Select( frModeList_t *l, int rmode, int customW, int customH, int vidW, int vidH ) {
	int		i, w, h;

	w = 0;
	h = 0;
	i = -1;
	if ( rmode == -1 && customW > 0 && customH > 0 ) {
		w = customW;
		h = customH;
		i = FR_Find( l, w, h );
		if ( i < 0 ) {
			i = FR_AddLabel( l, w, h );
		}
	} else if ( rmode >= 0 && rmode < FR_NUM_BUILTIN ) {
		FR_ParseSize( fr_builtinNames[rmode], &w, &h );
		i = FR_Find( l, w, h );
		if ( i < 0 ) {
			i = FR_Add( l, fr_builtinNames[rmode], rmode, w, h );
		}
	} else if ( vidW > 0 && vidH > 0 ) {
		w = vidW;
		h = vidH;
		i = FR_Find( l, w, h );
		if ( i < 0 ) {
			i = FR_AddLabel( l, w, h );
		}
	}

	if ( i < 0 ) {
		i = 0;
	}
	l->current = i;
	l->runMode = rmode;
	l->runW = w;
	l->runH = h;
	return i;
}

/*
=================
FR_ModeOf

The r_mode entry 'entry' stands for (0..11, or -1 for any other size), or
FR_NO_ENTRY when there is no such entry.
=================
*/
static int FR_ModeOf( const frModeList_t *l, int entry ) {
	if ( entry < 0 || entry >= l->count ) {
		return FR_NO_ENTRY;
	}
	return l->mode[entry];
}

/*
=================
FR_Accept

What ACCEPT writes for entry 'entry': returns the r_mode, and in *w x *h
the size for r_customwidth/r_customheight (meaningful only when the r_mode
is -1; 0 means leave them alone).  The entry the game is running in writes
back exactly the running mode, so an ACCEPT that did not touch Video Mode
never changes it.  FR_NO_ENTRY means write no r_mode at all.
=================
*/
static int FR_Accept( const frModeList_t *l, int entry, int *w, int *h ) {
	*w = 0;
	*h = 0;
	if ( entry < 0 || entry >= l->count ) {
		entry = l->current;
	}
	if ( entry < 0 || entry >= l->count ) {
		return FR_NO_ENTRY;
	}
	if ( entry == l->current && l->runMode != FR_NO_ENTRY ) {
		*w = l->runW;
		*h = l->runH;
		return l->runMode;
	}
	*w = l->width[entry];
	*h = l->height[entry];
	return l->mode[entry];
}

/*
=================
FR_PresetIndex

The entry for id's r_mode 'mode' (a "Graphics Settings" preset), appended
when the list lacks that size; -1 when it cannot be placed.
=================
*/
static int FR_PresetIndex( frModeList_t *l, int mode ) {
	int		i, w, h;

	if ( mode < 0 || mode >= FR_NUM_BUILTIN ) {
		return -1;
	}
	FR_ParseSize( fr_builtinNames[mode], &w, &h );
	i = FR_Find( l, w, h );
	if ( i < 0 ) {
		i = FR_Add( l, fr_builtinNames[mode], mode, w, h );
	}
	return i;
}

#endif	/* UI_FLEETRES_H */
