/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#include <algorithm>
#include <cstdio>
#include <string>

#include <unistd.h>

#include "src/terminal/parseraction.h"
#include "src/terminal/terminalframebuffer.h"
#include "terminaldispatcher.h"

using namespace Terminal;

/* Terminal functions -- routines activated by CSI, escape or a control char */

static void clearline( Framebuffer* fb, int row, int start, int end )
{
  for ( int col = start; col <= end; col++ ) {
    fb->reset_cell( fb->get_mutable_cell( row, col ) );
  }
}

/* cursor style */
static void CSI_DECSCUSR( Framebuffer* fb, Dispatcher* dispatch )
{
  int style = dispatch->getparam( 0, 0 );
  switch ( style ) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
      fb->ds.cursor_style = style;
      break;
    default:
      break;
  }
}

static Function func_CSI_DECSCUSR( CSI, " q", CSI_DECSCUSR );

/* erase in line */
static void CSI_EL( Framebuffer* fb, Dispatcher* dispatch )
{
  switch ( dispatch->getparam( 0, 0 ) ) {
    case 0: /* default: active position to end of line, inclusive */
      clearline( fb, -1, fb->ds.get_cursor_col(), fb->ds.get_width() - 1 );
      break;
    case 1: /* start of screen to active position, inclusive */
      clearline( fb, -1, 0, fb->ds.get_cursor_col() );
      break;
    case 2: /* all of line */
      fb->reset_row( fb->get_mutable_row( -1 ) );
      break;
    default:
      break;
  }
}

static Function func_CSI_EL( CSI, "K", CSI_EL );

/* erase in display */
static void CSI_ED( Framebuffer* fb, Dispatcher* dispatch )
{
  switch ( dispatch->getparam( 0, 0 ) ) {
    case 0: /* active position to end of screen, inclusive */
      clearline( fb, -1, fb->ds.get_cursor_col(), fb->ds.get_width() - 1 );
      for ( int y = fb->ds.get_cursor_row() + 1; y < fb->ds.get_height(); y++ ) {
        fb->reset_row( fb->get_mutable_row( y ) );
      }
      break;
    case 1: /* start of screen to active position, inclusive */
      for ( int y = 0; y < fb->ds.get_cursor_row(); y++ ) {
        fb->reset_row( fb->get_mutable_row( y ) );
      }
      clearline( fb, -1, 0, fb->ds.get_cursor_col() );
      break;
    case 2: /* entire screen */
      for ( int y = 0; y < fb->ds.get_height(); y++ ) {
        fb->reset_row( fb->get_mutable_row( y ) );
      }
      break;
    default:
      break;
  }
}

static Function func_CSI_ED( CSI, "J", CSI_ED );

/* cursor movement -- relative and absolute */
static void CSI_cursormove( Framebuffer* fb, Dispatcher* dispatch )
{
  int num = dispatch->getparam( 0, 1 );

  switch ( dispatch->get_dispatch_chars()[0] ) {
    case 'A':
      fb->ds.move_row( -num, true );
      break;
    case 'B':
      fb->ds.move_row( num, true );
      break;
    case 'C':
      fb->ds.move_col( num, true );
      break;
    case 'D':
      fb->ds.move_col( -num, true );
      break;
    case 'H':
    case 'f':
      fb->ds.move_row( dispatch->getparam( 0, 1 ) - 1 );
      fb->ds.move_col( dispatch->getparam( 1, 1 ) - 1 );
      break;
    default:
      break;
  }
}

static Function func_CSI_cursormove_A( CSI, "A", CSI_cursormove );
static Function func_CSI_cursormove_B( CSI, "B", CSI_cursormove );
static Function func_CSI_cursormove_C( CSI, "C", CSI_cursormove );
static Function func_CSI_cursormove_D( CSI, "D", CSI_cursormove );
static Function func_CSI_cursormove_H( CSI, "H", CSI_cursormove );
static Function func_CSI_cursormove_f( CSI, "f", CSI_cursormove );

/* device attributes */
static void CSI_DA( Framebuffer* fb __attribute( ( unused ) ), Dispatcher* dispatch )
{
  dispatch->terminal_to_host.append( "\033[?62c" ); /* plain vt220 */
}

static Function func_CSI_DA( CSI, "c", CSI_DA );

/* secondary device attributes */
static void CSI_SDA( Framebuffer* fb __attribute( ( unused ) ), Dispatcher* dispatch )
{
  dispatch->terminal_to_host.append( "\033[>1;10;0c" ); /* plain vt220 */
}

static Function func_CSI_SDA( CSI, ">c", CSI_SDA );

/* screen alignment diagnostic */
static void Esc_DECALN( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  for ( int y = 0; y < fb->ds.get_height(); y++ ) {
    for ( int x = 0; x < fb->ds.get_width(); x++ ) {
      fb->reset_cell( fb->get_mutable_cell( y, x ) );
      fb->get_mutable_cell( y, x )->append( 'E' );
    }
  }
}

static Function func_Esc_DECALN( ESCAPE, "#8", Esc_DECALN );

/* line feed */
static void Ctrl_LF( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  fb->move_rows_autoscroll( 1 );
}

/* same procedure for index, vertical tab, and form feed control codes */
static Function func_Ctrl_LF( CONTROL, "\x0a", Ctrl_LF );
static Function func_Ctrl_IND( CONTROL, "\x84", Ctrl_LF );
static Function func_Ctrl_VT( CONTROL, "\x0b", Ctrl_LF );
static Function func_Ctrl_FF( CONTROL, "\x0c", Ctrl_LF );

/* carriage return */
static void Ctrl_CR( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  fb->ds.move_col( 0 );
}

static Function func_Ctrl_CR( CONTROL, "\x0d", Ctrl_CR );

/* backspace */
static void Ctrl_BS( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  fb->ds.move_col( -1, true );
}

static Function func_Ctrl_BS( CONTROL, "\x08", Ctrl_BS );

/* reverse index -- like a backwards line feed */
static void Ctrl_RI( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  fb->move_rows_autoscroll( -1 );
}

static Function func_Ctrl_RI( CONTROL, "\x8D", Ctrl_RI );

/* newline */
static void Ctrl_NEL( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  fb->ds.move_col( 0 );
  fb->move_rows_autoscroll( 1 );
}

static Function func_Ctrl_NEL( CONTROL, "\x85", Ctrl_NEL );

/* horizontal tab */
static void HT_n( Framebuffer* fb, size_t count )
{
  int col = fb->ds.get_next_tab( count );
  if ( col == -1 ) { /* no tabs, go to end of line */
    col = fb->ds.get_width() - 1;
  }

  /* A horizontal tab is the only operation that preserves but
     does not set the wrap state. It also starts a new grapheme. */

  bool wrap_state_save = fb->ds.next_print_will_wrap;
  fb->ds.move_col( col, false );
  fb->ds.next_print_will_wrap = wrap_state_save;
}

static void Ctrl_HT( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  HT_n( fb, 1 );
}
static Function func_Ctrl_HT( CONTROL, "\x09", Ctrl_HT, false );

static void CSI_CxT( Framebuffer* fb, Dispatcher* dispatch )
{
  int param = dispatch->getparam( 0, 1 );
  if ( dispatch->get_dispatch_chars()[0] == 'Z' ) {
    param = -param;
  }
  if ( param == 0 ) {
    return;
  }
  HT_n( fb, param );
}

static Function func_CSI_CHT( CSI, "I", CSI_CxT, false );
static Function func_CSI_CBT( CSI, "Z", CSI_CxT, false );

/* horizontal tab set */
static void Ctrl_HTS( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  fb->ds.set_tab();
}

static Function func_Ctrl_HTS( CONTROL, "\x88", Ctrl_HTS );

/* tabulation clear */
static void CSI_TBC( Framebuffer* fb, Dispatcher* dispatch )
{
  int param = dispatch->getparam( 0, 0 );
  switch ( param ) {
    case 0: /* clear this tab stop */
      fb->ds.clear_tab( fb->ds.get_cursor_col() );
      break;
    case 3: /* clear all tab stops */
      fb->ds.clear_default_tabs();
      for ( int x = 0; x < fb->ds.get_width(); x++ ) {
        fb->ds.clear_tab( x );
      }
      break;
    default:
      break;
  }
}

/* TBC preserves wrap state */
static Function func_CSI_TBC( CSI, "g", CSI_TBC, false );

static bool* get_DEC_mode( int param, Framebuffer* fb )
{
  switch ( param ) {
    case 1: /* cursor key mode */
      return &( fb->ds.application_mode_cursor_keys );
    case 3: /* 80/132. Ignore but clear screen. */
      /* clear screen */
      fb->ds.move_row( 0 );
      fb->ds.move_col( 0 );
      for ( int y = 0; y < fb->ds.get_height(); y++ ) {
        fb->reset_row( fb->get_mutable_row( y ) );
      }
      return NULL;
    case 5: /* reverse video */
      return &( fb->ds.reverse_video );
    case 6: /* origin */
      fb->ds.move_row( 0 );
      fb->ds.move_col( 0 );
      return &( fb->ds.origin_mode );
    case 7: /* auto wrap */
      return &( fb->ds.auto_wrap_mode );
    case 25:
      return &( fb->ds.cursor_visible );
    case 1004: /* xterm mouse focus event */
      return &( fb->ds.mouse_focus_event );
    case 1007: /* xterm mouse alternate scroll */
      return &( fb->ds.mouse_alternate_scroll );
    case 2004: /* bracketed paste */
      return &( fb->ds.bracketed_paste );
    default:
      break;
  }
  return NULL;
}

/* helper for CSI_DECSM and CSI_DECRM */
static void set_if_available( bool* mode, bool value )
{
  if ( mode ) {
    *mode = value;
  }
}

/* set private mode */
static void CSI_DECSM( Framebuffer* fb, Dispatcher* dispatch )
{
  for ( int i = 0; i < dispatch->param_count(); i++ ) {
    int param = dispatch->getparam( i, 0 );
    if ( param == 9 || ( param >= 1000 && param <= 1003 ) ) {
      fb->ds.mouse_reporting_mode = (Terminal::DrawState::MouseReportingMode)param;
    } else if ( param == 1005 || param == 1006 || param == 1015 ) {
      fb->ds.mouse_encoding_mode = (Terminal::DrawState::MouseEncodingMode)param;
    } else {
      set_if_available( get_DEC_mode( param, fb ), true );
    }
  }
}

/* clear private mode */
static void CSI_DECRM( Framebuffer* fb, Dispatcher* dispatch )
{
  for ( int i = 0; i < dispatch->param_count(); i++ ) {
    int param = dispatch->getparam( i, 0 );
    if ( param == 9 || ( param >= 1000 && param <= 1003 ) ) {
      fb->ds.mouse_reporting_mode = Terminal::DrawState::MOUSE_REPORTING_NONE;
    } else if ( param == 1005 || param == 1006 || param == 1015 ) {
      fb->ds.mouse_encoding_mode = Terminal::DrawState::MOUSE_ENCODING_DEFAULT;
    } else {
      set_if_available( get_DEC_mode( param, fb ), false );
    }
  }
}

/* These functions don't clear wrap state. */
static Function func_CSI_DECSM( CSI, "?h", CSI_DECSM, false );
static Function func_CSI_DECRM( CSI, "?l", CSI_DECRM, false );

static bool* get_ANSI_mode( int param, Framebuffer* fb )
{
  if ( param == 4 ) { /* insert/replace mode */
    return &( fb->ds.insert_mode );
  }
  return NULL;
}

/* set mode */
static void CSI_SM( Framebuffer* fb, Dispatcher* dispatch )
{
  for ( int i = 0; i < dispatch->param_count(); i++ ) {
    bool* mode = get_ANSI_mode( dispatch->getparam( i, 0 ), fb );
    if ( mode ) {
      *mode = true;
    }
  }
}

/* clear mode */
static void CSI_RM( Framebuffer* fb, Dispatcher* dispatch )
{
  for ( int i = 0; i < dispatch->param_count(); i++ ) {
    bool* mode = get_ANSI_mode( dispatch->getparam( i, 0 ), fb );
    if ( mode ) {
      *mode = false;
    }
  }
}

static Function func_CSI_SM( CSI, "h", CSI_SM );
static Function func_CSI_RM( CSI, "l", CSI_RM );

/* set top and bottom margins */
static void CSI_DECSTBM( Framebuffer* fb, Dispatcher* dispatch )
{
  int top = dispatch->getparam( 0, 1 );
  int bottom = dispatch->getparam( 1, fb->ds.get_height() );

  if ( ( bottom <= top ) || ( top > fb->ds.get_height() ) || ( top == 0 && bottom == 1 ) ) {
    return; /* invalid, xterm ignores */
  }

  fb->ds.set_scrolling_region( top - 1, bottom - 1 );
  fb->ds.move_row( 0 );
  fb->ds.move_col( 0 );
}

static Function func_CSI_DECSTMB( CSI, "r", CSI_DECSTBM );

/* terminal bell */
static void Ctrl_BEL( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  fb->ring_bell();
}

static Function func_Ctrl_BEL( CONTROL, "\x07", Ctrl_BEL );

/* select graphics rendition -- e.g., bold, blinking, etc. */
static void CSI_SGR( Framebuffer* fb, Dispatcher* dispatch )
{
  for ( int i = 0; i < dispatch->param_count(); i++ ) {
    int rendition = dispatch->getparam( i, 0 );
    /* We need to special-case the handling of [34]8 ; 5 ; Ps,
       because Ps of 0 in that case does not mean reset to default, even
       though it means that otherwise (as usually renditions are applied
       in order). */
    if ( ( rendition == 38 || rendition == 48 ) && ( dispatch->param_count() - i >= 3 )
         && ( dispatch->getparam( i + 1, -1 ) == 5 ) ) {
      ( rendition == 38 ) ? fb->ds.set_foreground_color( dispatch->getparam( i + 2, 0 ) )
                          : fb->ds.set_background_color( dispatch->getparam( i + 2, 0 ) );
      i += 2;
      continue;
    }

    /* True color support: ESC[ ... [34]8;2;<r>;<g>;<b> ... m */
    if ( ( rendition == 38 || rendition == 48 ) && ( dispatch->param_count() - i >= 5 )
         && ( dispatch->getparam( i + 1, -1 ) == 2 ) ) {
      unsigned int red = dispatch->getparam( i + 2, 0 );
      unsigned int green = dispatch->getparam( i + 3, 0 );
      unsigned int blue = dispatch->getparam( i + 4, 0 );
      unsigned int color;

      color = Renditions::make_true_color( red, green, blue );

      if ( rendition == 38 ) {
        fb->ds.set_foreground_color( color );
      } else {
        fb->ds.set_background_color( color );
      }
      i += 4;
      continue;
    }

    fb->ds.add_rendition( rendition );
  }
}

static Function func_CSI_SGR( CSI, "m", CSI_SGR, false ); /* changing renditions doesn't clear wrap flag */

/* save and restore cursor */
static void Esc_DECSC( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  fb->ds.save_cursor();
}

static void Esc_DECRC( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  fb->ds.restore_cursor();
}

static Function func_Esc_DECSC( ESCAPE, "7", Esc_DECSC );
static Function func_Esc_DECRC( ESCAPE, "8", Esc_DECRC );

/* device status report -- e.g., cursor position (used by resize) */
static void CSI_DSR( Framebuffer* fb, Dispatcher* dispatch )
{
  int param = dispatch->getparam( 0, 0 );

  switch ( param ) {
    case 5: /* device status report requested */
      dispatch->terminal_to_host.append( "\033[0n" );
      break;
    case 6: /* report of active position requested */
      char cpr[32];
      snprintf( cpr, 32, "\033[%d;%dR", fb->ds.get_cursor_row() + 1, fb->ds.get_cursor_col() + 1 );
      dispatch->terminal_to_host.append( cpr );
      break;
    default:
      break;
  }
}

static Function func_CSI_DSR( CSI, "n", CSI_DSR );

/* insert line */
static void CSI_IL( Framebuffer* fb, Dispatcher* dispatch )
{
  int lines = dispatch->getparam( 0, 1 );

  fb->insert_line( fb->ds.get_cursor_row(), lines );

  /* vt220 manual and Ecma-48 say to move to first column */
  /* but xterm and gnome-terminal don't */
  fb->ds.move_col( 0 );
}

static Function func_CSI_IL( CSI, "L", CSI_IL );

/* delete line */
static void CSI_DL( Framebuffer* fb, Dispatcher* dispatch )
{
  int lines = dispatch->getparam( 0, 1 );

  fb->delete_line( fb->ds.get_cursor_row(), lines );

  /* same story -- xterm and gnome-terminal don't
     move to first column */
  fb->ds.move_col( 0 );
}

static Function func_CSI_DL( CSI, "M", CSI_DL );

/* insert characters */
static void CSI_ICH( Framebuffer* fb, Dispatcher* dispatch )
{
  int cells = dispatch->getparam( 0, 1 );

  for ( int i = 0; i < cells; i++ ) {
    fb->insert_cell( fb->ds.get_cursor_row(), fb->ds.get_cursor_col() );
  }
}

static Function func_CSI_ICH( CSI, "@", CSI_ICH );

/* delete character */
static void CSI_DCH( Framebuffer* fb, Dispatcher* dispatch )
{
  int cells = dispatch->getparam( 0, 1 );

  for ( int i = 0; i < cells; i++ ) {
    fb->delete_cell( fb->ds.get_cursor_row(), fb->ds.get_cursor_col() );
  }
}

static Function func_CSI_DCH( CSI, "P", CSI_DCH );

/* line position absolute */
static void CSI_VPA( Framebuffer* fb, Dispatcher* dispatch )
{
  int row = dispatch->getparam( 0, 1 );
  fb->ds.move_row( row - 1 );
}

static Function func_CSI_VPA( CSI, "d", CSI_VPA );

/* character position absolute */
static void CSI_HPA( Framebuffer* fb, Dispatcher* dispatch )
{
  int col = dispatch->getparam( 0, 1 );
  fb->ds.move_col( col - 1 );
}

static Function func_CSI_CHA( CSI, "G", CSI_HPA );    /* ECMA-48 name: CHA */
static Function func_CSI_HPA( CSI, "\x60", CSI_HPA ); /* ECMA-48 name: HPA */

/* erase character */
static void CSI_ECH( Framebuffer* fb, Dispatcher* dispatch )
{
  int num = dispatch->getparam( 0, 1 );
  int limit = fb->ds.get_cursor_col() + num - 1;
  if ( limit >= fb->ds.get_width() ) {
    limit = fb->ds.get_width() - 1;
  }

  clearline( fb, -1, fb->ds.get_cursor_col(), limit );
}

static Function func_CSI_ECH( CSI, "X", CSI_ECH );

/* reset to initial state */
static void Esc_RIS( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  fb->reset();
}

static Function func_Esc_RIS( ESCAPE, "c", Esc_RIS );

/* soft reset */
static void CSI_DECSTR( Framebuffer* fb, Dispatcher* dispatch __attribute( ( unused ) ) )
{
  fb->soft_reset();
}

static Function func_CSI_DECSTR( CSI, "!p", CSI_DECSTR );

/* xterm uses an Operating System Command to set the window title */
void Dispatcher::OSC_dispatch( const Parser::OSC_End* act __attribute( ( unused ) ), Framebuffer* fb )
{
  /* handle osc copy clipboard sequence 52;c; */
  if ( OSC_string.size() >= 5 && OSC_string[0] == L'5' && OSC_string[1] == L'2' && OSC_string[2] == L';'
       && OSC_string[3] == L'c' && OSC_string[4] == L';' ) {
    Terminal::Framebuffer::title_type clipboard( OSC_string.begin() + 5, OSC_string.end() );
    fb->set_clipboard( clipboard );
    /* handle osc terminal title sequence */
  } else if ( OSC_string.size() >= 1 ) {
    long cmd_num = -1;
    int offset = 0;
    if ( OSC_string[0] == L';' ) {
      /* OSC of the form "\033];<title>\007" */
      cmd_num = 0; /* treat it as as a zero */
      offset = 1;
    } else if ( ( OSC_string.size() >= 2 ) && ( OSC_string[1] == L';' ) ) {
      /* OSC of the form "\033]X;<title>\007" where X can be:
       * 0: set icon name and window title
       * 1: set icon name
       * 2: set window title */
      cmd_num = OSC_string[0] - L'0';
      offset = 2;
    }
    bool set_icon = cmd_num == 0 || cmd_num == 1;
    bool set_title = cmd_num == 0 || cmd_num == 2;
    if ( set_icon || set_title ) {
      fb->set_title_initialized();
      int title_length = std::min( OSC_string.size(), (size_t)256 );
      Terminal::Framebuffer::title_type newtitle( OSC_string.begin() + offset, OSC_string.begin() + title_length );
      if ( set_icon ) {
        fb->set_icon_name( newtitle );
      }
      if ( set_title ) {
        fb->set_window_title( newtitle );
      }
    }
  }
}

/* scroll down or terminfo indn */
static void CSI_SD( Framebuffer* fb, Dispatcher* dispatch )
{
  fb->scroll( dispatch->getparam( 0, 1 ) );
}

static Function func_CSI_SD( CSI, "S", CSI_SD );

/* scroll up or terminfo rin */
static void CSI_SU( Framebuffer* fb, Dispatcher* dispatch )
{
  fb->scroll( -dispatch->getparam( 0, 1 ) );
}

static Function func_CSI_SU( CSI, "T", CSI_SU );
