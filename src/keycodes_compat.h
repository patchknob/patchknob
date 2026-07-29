//----------------------------------------------------------------------------
//  keycodes_compat.h
//
//  Legacy keycode numeric values, provided so the UI-neutral engine core can
//  keep its historical default keybindings and .PatchKnobusr config compatibility.
//  The `key_events` map is std::map<long,long> with opaque keycodes; these are
//  the default values. The SDL frontend translates SDL keysyms into these codes.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_KEYCODES_COMPAT_H
#define PATCHKNOB_KEYCODES_COMPAT_H

/* printable ASCII == X11 keysym for these ranges */
#define PATCHKNOB_KEY_SPACE         0x020
#define PATCHKNOB_KEY_APOSTROPHE    0x027
#define PATCHKNOB_KEY_COMMA         0x02c
#define PATCHKNOB_KEY_SEMICOLON     0x03b
#define PATCHKNOB_KEY_BRACKET_LEFT  0x05b
#define PATCHKNOB_KEY_BRACKET_RIGHT 0x05d

#define PATCHKNOB_KEY_0 0x030
#define PATCHKNOB_KEY_1 0x031
#define PATCHKNOB_KEY_2 0x032
#define PATCHKNOB_KEY_3 0x033
#define PATCHKNOB_KEY_4 0x034
#define PATCHKNOB_KEY_5 0x035
#define PATCHKNOB_KEY_6 0x036
#define PATCHKNOB_KEY_7 0x037
#define PATCHKNOB_KEY_8 0x038
#define PATCHKNOB_KEY_9 0x039

#define PATCHKNOB_KEY_A 0x061
#define PATCHKNOB_KEY_B 0x062
#define PATCHKNOB_KEY_C 0x063
#define PATCHKNOB_KEY_D 0x064
#define PATCHKNOB_KEY_E 0x065
#define PATCHKNOB_KEY_F 0x066
#define PATCHKNOB_KEY_G 0x067
#define PATCHKNOB_KEY_H 0x068
#define PATCHKNOB_KEY_I 0x069
#define PATCHKNOB_KEY_J 0x06a
#define PATCHKNOB_KEY_K 0x06b
#define PATCHKNOB_KEY_L 0x06c
#define PATCHKNOB_KEY_M 0x06d
#define PATCHKNOB_KEY_N 0x06e
#define PATCHKNOB_KEY_O 0x06f
#define PATCHKNOB_KEY_P 0x070
#define PATCHKNOB_KEY_Q 0x071
#define PATCHKNOB_KEY_R 0x072
#define PATCHKNOB_KEY_S 0x073
#define PATCHKNOB_KEY_T 0x074
#define PATCHKNOB_KEY_U 0x075
#define PATCHKNOB_KEY_V 0x076
#define PATCHKNOB_KEY_W 0x077
#define PATCHKNOB_KEY_X 0x078
#define PATCHKNOB_KEY_Y 0x079
#define PATCHKNOB_KEY_Z 0x07a

#define PATCHKNOB_KEY_ESCAPE        0xff1b
#define PATCHKNOB_KEY_CONTROL_LEFT  0xffe3
#define PATCHKNOB_KEY_CONTROL_RIGHT 0xffe4
#define PATCHKNOB_KEY_ALT_LEFT      0xffe9
#define PATCHKNOB_KEY_ALT_RIGHT     0xffea

#endif /* PATCHKNOB_KEYCODES_COMPAT_H */
