/*
 * hwextra.h - the HWPROFILE fields that describe hardware the ACTIVE adapter
 * answer cannot see.
 *
 * Separate from handlers.h because these take util.h's json_t, which is an
 * anonymous-struct typedef and so cannot be forward-declared; handlers.h stays
 * free of that include, as it is for every other module.
 *
 * Each emitter appends one key to an OPEN JSON object and emits nothing else,
 * so hwprofile.c composes them in whatever order it likes. Every one of them
 * fails soft: a probe that cannot answer contributes an empty array rather
 * than failing the record, because a hardware inventory that refuses to report
 * anything when one probe fails is how a box ends up undocumented.
 */
#ifndef HWEXTRA_H
#define HWEXTRA_H

#include "util.h"

/* "video_cards": every display-class instance, each marked with whether it is
 * the one attached to the desktop. .143 renders on a GeForce 6800 with its
 * Voodoo5 5500 behind it as a second adapter. */
void hwextra_emit_video_cards(json_t *j);

/* "accelerators": every VEN_121A device in the PCI enumerator, however it is
 * classed. A Voodoo 2's INF is Class=MEDIA, so it is not a display device at
 * all and appears in no display-class scan; Enum\PCI lists a fitted card even
 * with no driver bound to it. */
void hwextra_emit_accelerators(json_t *j);

/* "network": the box's own addresses and MAC, so a record on the share can be
 * matched back to the machine that wrote it - a computer name is not an
 * identity. */
void hwextra_emit_network(json_t *j);

#endif /* HWEXTRA_H */
