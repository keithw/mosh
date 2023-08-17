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

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <typeinfo>

#include <unistd.h>

#include "src/terminal/terminal.h"
#include "src/util/char_utils.h"

using namespace Terminal;

Emulator::Emulator( size_t s_width, size_t s_height ) : fb( s_width, s_height ), dispatch(), user() {}

std::string Emulator::read_octets_to_host( void )
{
  std::string ret = dispatch.terminal_to_host;
  dispatch.terminal_to_host.clear();
  return ret;
}

void Emulator::execute( const Parser::Execute* act )
{
  dispatch.dispatch( CONTROL, act, &fb );
}

void Emulator::print( const Parser::Print* act )
{
  assert( act->char_present );

  const wchar_t ch = act->ch;

  /*
   * Check for printing ISO 8859-1 first, it's a cheap way to detect
   * some common narrow characters.
   */
  const int chwidth = ch == L'\0' ? -1 : ( Cell::isprint_iso8859_1( ch ) ? 1 : mosh_wcwidth( ch ) );

  Cell* this_cell = fb.get_mutable_cell();

  switch ( chwidth ) {
    case 1: /* normal character */
    case 2: /* wide character */
      if ( fb.ds.auto_wrap_mode && fb.ds.next_print_will_wrap ) {
        fb.get_mutable_row( -1 )->set_wrap( true );
        fb.ds.move_col( 0 );
        fb.move_rows_autoscroll( 1 );
        this_cell = NULL;
      } else if ( fb.ds.auto_wrap_mode && ( chwidth == 2 )
                  && ( fb.ds.get_cursor_col() == fb.ds.get_width() - 1 ) ) {
        /* wrap 2-cell chars if no room, even without will-wrap flag */
        fb.reset_cell( this_cell );
        fb.get_mutable_row( -1 )->set_wrap( false );
        /* There doesn't seem to be a consistent way to get the
           downstream terminal emulator to set the wrap-around
           copy-and-paste flag on a row that ends with an empty cell
           because a wide char was wrapped to the next line. */
        fb.ds.move_col( 0 );
        fb.move_rows_autoscroll( 1 );
        this_cell = NULL;
      }

      if ( fb.ds.insert_mode ) {
        for ( int i = 0; i < chwidth; i++ ) {
          fb.insert_cell( fb.ds.get_cursor_row(), fb.ds.get_cursor_col() );
        }
        this_cell = NULL;
      }

      if ( !this_cell ) {
        this_cell = fb.get_mutable_cell();
      }

      fb.reset_cell( this_cell );
      this_cell->append( ch );
      this_cell->set_wide( chwidth == 2 ); /* chwidth had better be 1 or 2 here */
      fb.apply_renditions_to_cell( this_cell );

      if ( chwidth == 2 && fb.ds.get_cursor_col() + 1 < fb.ds.get_width() ) { /* erase overlapped cell */
        fb.reset_cell( fb.get_mutable_cell( fb.ds.get_cursor_row(), fb.ds.get_cursor_col() + 1 ) );
      }

      fb.ds.move_col( chwidth, true, true );

      break;
    case 0: /* combining character */
    {
      Cell* combining_cell = fb.get_combining_cell(); /* can be null if we were resized */
      if ( combining_cell == NULL ) {                 /* character is now offscreen */
        break;
      }

      if ( combining_cell->empty() ) {
        /* cell starts with combining character */
        /* ... but isn't necessarily the target for a new
           base character [e.g. start of line], if the
           combining character has been cleared with
           a sequence like ED ("J") or EL ("K") */
        assert( !combining_cell->get_wide() );
        combining_cell->set_fallback( true );
        fb.ds.move_col( 1, true, true );
      }
      if ( !combining_cell->full() ) {
        combining_cell->append( ch );
      }
    } break;
    case -1: /* unprintable character */
      break;
    default:
      assert( !"unexpected character width from wcwidth()" );
      break;
  }
}

void Emulator::CSI_dispatch( const Parser::CSI_Dispatch* act )
{
  dispatch.dispatch( CSI, act, &fb );
}

void Emulator::OSC_end( const Parser::OSC_End* act )
{
  dispatch.OSC_dispatch( act, &fb );
}

void Emulator::Esc_dispatch( const Parser::Esc_Dispatch* act )
{
  /* handle 7-bit ESC-encoding of C1 control characters */
  if ( ( dispatch.get_dispatch_chars().size() == 0 ) && ( 0x40 <= act->ch ) && ( act->ch <= 0x5F ) ) {
    Parser::Esc_Dispatch act2 = *act;
    act2.ch += 0x40;
    dispatch.dispatch( CONTROL, &act2, &fb );
  } else {
    dispatch.dispatch( ESCAPE, act, &fb );
  }
}

void Emulator::resize( size_t s_width, size_t s_height )
{
  fb.resize( s_width, s_height );
}

bool Emulator::operator==( Emulator const& x ) const
{
  /* dispatcher and user are irrelevant for us */
  return fb == x.fb;
}
