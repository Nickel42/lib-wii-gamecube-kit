// lib-wii-gamecube-kit: force-included into Dolphin's sources on Android
// (see CMakeLists.txt). Bionic's <sys/endian.h> defines swap16, swap32 and
// swap64 as macros, which would rewrite Common::swap16() and friends; Dolphin
// undefines them only when ANDROID is defined, and this build undefines ANDROID.
#pragma once
#include <sys/endian.h>
#undef swap16
#undef swap32
#undef swap64
