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

#ifndef TERMINALDISPLAY_HPP
#define TERMINALDISPLAY_HPP

#include "src/terminal/terminalframebuffer.h"

namespace Terminal {
/* variables used within a new_frame */
class FrameState
{
public:
  std::string str;

  int cursor_x, cursor_y;
  Renditions current_rendition;
  bool cursor_visible;
  int cursor_style;

  const Framebuffer& last_frame;

  FrameState( const Framebuffer& s_last );

  void append( char c ) { str.append( 1, c ); }
  void append( size_t s, char c ) { str.append( s, c ); }
  void append( wchar_t wc ) { Cell::append_to_str( str, wc ); }
  void append( const char* s ) { str.append( s ); }
  void append_string( const std::string& append ) { str.append( append ); }

  void append_cell( const Cell& cell ) { cell.print_grapheme( str ); }
  void append_silent_move( int y, int x );
  void append_move( int y, int x );
  void update_rendition( const Renditions& r, bool force = false );
};

class Display
{
private:
  bool has_ech; /* erase character is part of vt200 but not supported by tmux
                   (or by "screen" terminfo entry, which is what tmux advertises) */

  bool has_bce; /* erases result in cell filled with background color */

  bool has_title; /* supports window title and icon name */

  const char *smcup, *rmcup; /* enter and exit alternate screen mode */

  bool put_row( bool initialized,
                FrameState& frame,
                const Framebuffer& f,
                int frame_y,
                const Row& old_row,
                bool wrap ) const;

public:
  std::string open() const;
  std::string close() const;

  std::string new_frame( bool initialized, const Framebuffer& last, const Framebuffer& f ) const;

  Display( bool use_environment );
};
}

#endif
