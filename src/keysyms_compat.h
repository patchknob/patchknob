//----------------------------------------------------------------------------
//  keysyms_compat.h
//
//  GDK/X11 keysym numeric values, provided so the UI-agnostic engine core
//  (perform) can keep its historical default keybindings and .seq24usr config
//  compatibility WITHOUT depending on gtkmm.  The `key_events` map is just
//  std::map<long,long> (opaque keycodes); these are the default values.  The
//  SDL frontend translates SDL keysyms into these codes.
//----------------------------------------------------------------------------
#ifndef SEQ24_KEYSYMS_COMPAT_H
#define SEQ24_KEYSYMS_COMPAT_H

/* printable ASCII == X11 keysym for these ranges */
#define GDK_space        0x020
#define GDK_apostrophe   0x027
#define GDK_comma        0x02c
#define GDK_semicolon    0x03b
#define GDK_bracketleft  0x05b
#define GDK_bracketright 0x05d

#define GDK_0 0x030
#define GDK_1 0x031
#define GDK_2 0x032
#define GDK_3 0x033
#define GDK_4 0x034
#define GDK_5 0x035
#define GDK_6 0x036
#define GDK_7 0x037
#define GDK_8 0x038
#define GDK_9 0x039

#define GDK_a 0x061
#define GDK_b 0x062
#define GDK_c 0x063
#define GDK_d 0x064
#define GDK_e 0x065
#define GDK_f 0x066
#define GDK_g 0x067
#define GDK_h 0x068
#define GDK_i 0x069
#define GDK_j 0x06a
#define GDK_k 0x06b
#define GDK_l 0x06c
#define GDK_m 0x06d
#define GDK_n 0x06e
#define GDK_o 0x06f
#define GDK_p 0x070
#define GDK_q 0x071
#define GDK_r 0x072
#define GDK_s 0x073
#define GDK_t 0x074
#define GDK_u 0x075
#define GDK_v 0x076
#define GDK_w 0x077
#define GDK_x 0x078
#define GDK_y 0x079
#define GDK_z 0x07a

#define GDK_Escape    0xff1b
#define GDK_Control_L 0xffe3
#define GDK_Control_R 0xffe4
#define GDK_Alt_L     0xffe9
#define GDK_Alt_R     0xffea

#endif /* SEQ24_KEYSYMS_COMPAT_H */
