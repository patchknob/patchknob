/* src/config.h — hand-written for the Windows (MINGW64) port.
 *
 * Replaces the autotools-generated config.h. JACK and LASH are Linux-only
 * session/audio glue and stay disabled on Windows. The new audio/MIDI/VST
 * backend is hand-rolled (RtMidi + RtAudio + VST3 SDK), not wired here.
 */
#ifndef SEQ24_CONFIG_H
#define SEQ24_CONFIG_H

#define PACKAGE          "seq24"
#define PACKAGE_NAME     "seq24"
#define PACKAGE_TARNAME  "seq24"
#define VERSION          "0.8.7-win"
#define PACKAGE_VERSION  "0.8.7-win"
#define PACKAGE_STRING   "seq24 0.8.7-win"
#define PACKAGE_BUGREPORT ""

/* Standard headers available under MINGW64. */
#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UNISTD_H 1

/* Feature toggles — disabled on the Windows port. */
/* #undef JACK_SUPPORT */
/* #undef LASH_SUPPORT */
/* #undef HAVE_LIBASOUND */
/* #undef HAVE_LIBRT */

#endif /* SEQ24_CONFIG_H */
