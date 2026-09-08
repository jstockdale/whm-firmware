/* Proves the implementation itself compiles cleanly as C++ (in case a firmware
 * puts the impl in a .cpp). Compile-only check (object not linked here). */
#define WH_LINK_IMPLEMENTATION
#include "wh_link.h"
