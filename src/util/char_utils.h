#ifndef CHAR_UTILS_HPP
#define CHAR_UTILS_HPP

#include "widechar_width.h"

static int mosh_wcwidth( uint32_t c )
{
  int width = widechar_wcwidth( c );
  if ( width >= 0 ) {
    return width;
  }

  /* https://github.com/ridiculousfish/widecharwidth/tree/master#c-usage */
  switch ( width ) {
    case widechar_nonprint:
      return -1;
    case widechar_combining:
      return 0;
    case widechar_ambiguous:
      return 1;
    case widechar_private_use:
      return 1;
    case widechar_unassigned:
      return -1;
    case widechar_non_character:
      return -1;
    case widechar_widened_in_9:
      return 2;
    default:
      return -1;
  }
}

#endif
