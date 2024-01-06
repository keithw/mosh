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

#include <cstdio>

#include "src/terminal/terminalframebuffer.h"
#include "terminaldisplay.h"

using namespace Terminal;

/* Print a new "frame" to the terminal, using ANSI/ECMA-48 escape codes. */

static const Renditions& initial_rendition( void )
{
  const static Renditions blank = Renditions( 0 );
  return blank;
}

std::string Display::open() const
{
  return std::string( smcup ? smcup : "" ) + std::string( "\033[?1h" );
}

std::string Display::close() const
{
  return std::string( "\033[?1l\033[0m\033[?25h"
                      "\033[?1003l\033[?1002l\033[?1001l\033[?1000l"
                      "\033[?1015l\033[?1006l\033[?1005l" )
         + std::string( rmcup ? rmcup : "" );
}

std::string Display::new_frame( bool initialized, const Framebuffer& last, const Framebuffer& f ) const
{
  FrameState frame( last );

  char tmp[64];

  /* has bell been rung? */
  if ( f.get_bell_count() != frame.last_frame.get_bell_count() ) {
    frame.append( '\007' );
  }
  using title_type = Terminal::Framebuffer::title_type;

  /* has icon name or window title changed? */
  if ( has_title && f.is_title_initialized()
       && ( ( !initialized ) || ( f.get_icon_name() != frame.last_frame.get_icon_name() )
            || ( f.get_window_title() != frame.last_frame.get_window_title() ) ) ) {
    /* set icon name and window title */
    if ( f.get_icon_name() == f.get_window_title() ) {
      /* write combined Icon Name and Window Title */
      frame.append( "\033]0;" );
      const title_type& window_title( f.get_window_title() );
      for ( title_type::const_iterator i = window_title.begin(); i != window_title.end(); i++ ) {
        frame.append( *i );
      }
      frame.append( '\007' );
      /* ST is more correct, but BEL more widely supported */
    } else {
      /* write Icon Name */
      frame.append( "\033]1;" );
      const title_type& icon_name( f.get_icon_name() );
      for ( title_type::const_iterator i = icon_name.begin(); i != icon_name.end(); i++ ) {
        frame.append( *i );
      }
      frame.append( '\007' );

      frame.append( "\033]2;" );
      const title_type& window_title( f.get_window_title() );
      for ( title_type::const_iterator i = window_title.begin(); i != window_title.end(); i++ ) {
        frame.append( *i );
      }
      frame.append( '\007' );
    }
  }

  /* has clipboard changed? */
  if ( f.get_clipboard_seqnum() != frame.last_frame.get_clipboard_seqnum() ) {
    frame.append( "\033]52;" );
    const title_type& clipboard( f.get_clipboard() );
    for ( title_type::const_iterator i = clipboard.begin(); i != clipboard.end(); i++ ) {
      frame.append( *i );
    }
    frame.append( '\007' );
  }

  /* has reverse video state changed? */
  if ( ( !initialized ) || ( f.ds.reverse_video != frame.last_frame.ds.reverse_video ) ) {
    /* set reverse video */
    snprintf( tmp, 64, "\033[?5%c", ( f.ds.reverse_video ? 'h' : 'l' ) );
    frame.append( tmp );
  }

  /* has size changed? */
  if ( ( !initialized ) || ( f.ds.get_width() != frame.last_frame.ds.get_width() )
       || ( f.ds.get_height() != frame.last_frame.ds.get_height() ) ) {
    /* reset scrolling region */
    frame.append( "\033[r" );

    /* clear screen */
    frame.append( "\033[0m\033[H\033[2J" );
    initialized = false;
    frame.cursor_x = frame.cursor_y = 0;
    frame.current_rendition = initial_rendition();
  } else {
    frame.cursor_x = frame.last_frame.ds.get_cursor_col();
    frame.cursor_y = frame.last_frame.ds.get_cursor_row();
    frame.current_rendition = frame.last_frame.ds.get_renditions();
  }

  /* is cursor visibility initialized? */
  if ( !initialized ) {
    frame.cursor_visible = false;
    frame.append( "\033[?25l" );
  }

  int frame_y = 0;
  Framebuffer::row_pointer blank_row;
  Framebuffer::rows_type rows( frame.last_frame.get_rows() );
  /* Extend rows if we've gotten a resize and new is wider than old */
  if ( frame.last_frame.ds.get_width() < f.ds.get_width() ) {
    for ( Framebuffer::rows_type::iterator p = rows.begin(); p != rows.end(); p++ ) {
      *p = std::make_shared<Row>( **p );
      ( *p )->cells.resize( f.ds.get_width(), Cell( f.ds.get_background_rendition() ) );
    }
  }
  /* Add rows if we've gotten a resize and new is taller than old */
  if ( static_cast<int>( rows.size() ) < f.ds.get_height() ) {
    // get a proper blank row
    const size_t w = f.ds.get_width();
    const color_type c = 0;
    blank_row = std::make_shared<Row>( w, c );
    rows.resize( f.ds.get_height(), blank_row );
  }

  /* shortcut -- has display moved up by a certain number of lines? */
  if ( initialized ) {
    int lines_scrolled = 0;
    int scroll_height = 0;

    for ( int row = 0; row < f.ds.get_height(); row++ ) {
      const Row* new_row = f.get_row( 0 );
      const Row* old_row = &*rows.at( row );
      if ( !( new_row == old_row || *new_row == *old_row ) ) {
        continue;
      }
      /* if row 0, we're looking at ourselves and probably didn't scroll */
      if ( row == 0 ) {
        break;
      }
      /* found a scroll */
      lines_scrolled = row;
      scroll_height = 1;

      /* how big is the region that was scrolled? */
      for ( int region_height = 1; lines_scrolled + region_height < f.ds.get_height(); region_height++ ) {
        if ( *f.get_row( region_height ) == *rows.at( lines_scrolled + region_height ) ) {
          scroll_height = region_height + 1;
        } else {
          break;
        }
      }

      break;
    }

    if ( scroll_height ) {
      frame_y = scroll_height;

      if ( lines_scrolled ) {
        /* Now we need a proper blank row. */
        if ( blank_row.get() == NULL ) {
          const size_t w = f.ds.get_width();
          const color_type c = 0;
          blank_row = std::make_shared<Row>( w, c );
        }
        frame.update_rendition( initial_rendition(), true );

        int top_margin = 0;
        int bottom_margin = top_margin + lines_scrolled + scroll_height - 1;

        assert( bottom_margin < f.ds.get_height() );

        /* Common case:  if we're already on the bottom line and we're scrolling the whole
         * screen, just do a CR and LFs.
         */
        if ( scroll_height + lines_scrolled == f.ds.get_height() && frame.cursor_y + 1 == f.ds.get_height() ) {
          frame.append( '\r' );
          frame.append( lines_scrolled, '\n' );
          frame.cursor_x = 0;
        } else {
          /* set scrolling region */
          snprintf( tmp, 64, "\033[%d;%dr", top_margin + 1, bottom_margin + 1 );
          frame.append( tmp );

          /* go to bottom of scrolling region */
          frame.cursor_x = frame.cursor_y = -1;
          frame.append_silent_move( bottom_margin, 0 );

          /* scroll */
          frame.append( lines_scrolled, '\n' );

          /* reset scrolling region */
          frame.append( "\033[r" );
          /* invalidate cursor position after unsetting scrolling region */
          frame.cursor_x = frame.cursor_y = -1;
        }

        /* do the move in our local index */
        for ( int i = top_margin; i <= bottom_margin; i++ ) {
          if ( i + lines_scrolled <= bottom_margin ) {
            rows.at( i ) = rows.at( i + lines_scrolled );
          } else {
            rows.at( i ) = blank_row;
          }
        }
      }
    }
  }

  /* Now update the display, row by row */
  bool wrap = false;
  for ( ; frame_y < f.ds.get_height(); frame_y++ ) {
    wrap = put_row( initialized, frame, f, frame_y, *rows.at( frame_y ), wrap );
  }

  /* has cursor location changed? */
  if ( ( !initialized ) || ( f.ds.get_cursor_row() != frame.cursor_y )
       || ( f.ds.get_cursor_col() != frame.cursor_x ) ) {
    frame.append_move( f.ds.get_cursor_row(), f.ds.get_cursor_col() );
  }

  /* has cursor visibility changed? */
  if ( ( !initialized ) || ( f.ds.cursor_visible != frame.cursor_visible ) ) {
    if ( f.ds.cursor_visible ) {
      frame.append( "\033[?25h" );
    } else {
      frame.append( "\033[?25l" );
    }
  }

  /* have renditions changed? */
  frame.update_rendition( f.ds.get_renditions(), !initialized );

  /* has bracketed paste mode changed? */
  if ( ( !initialized ) || ( f.ds.bracketed_paste != frame.last_frame.ds.bracketed_paste ) ) {
    frame.append( f.ds.bracketed_paste ? "\033[?2004h" : "\033[?2004l" );
  }

  /* has mouse reporting mode changed? */
  if ( ( !initialized ) || ( f.ds.mouse_reporting_mode != frame.last_frame.ds.mouse_reporting_mode ) ) {
    if ( f.ds.mouse_reporting_mode == DrawState::MOUSE_REPORTING_NONE ) {
      frame.append( "\033[?1003l" );
      frame.append( "\033[?1002l" );
      frame.append( "\033[?1001l" );
      frame.append( "\033[?1000l" );
    } else {
      if ( frame.last_frame.ds.mouse_reporting_mode != DrawState::MOUSE_REPORTING_NONE ) {
        snprintf( tmp, sizeof( tmp ), "\033[?%dl", frame.last_frame.ds.mouse_reporting_mode );
        frame.append( tmp );
      }
      snprintf( tmp, sizeof( tmp ), "\033[?%dh", f.ds.mouse_reporting_mode );
      frame.append( tmp );
    }
  }

  /* has mouse focus mode changed? */
  if ( ( !initialized ) || ( f.ds.mouse_focus_event != frame.last_frame.ds.mouse_focus_event ) ) {
    frame.append( f.ds.mouse_focus_event ? "\033[?1004h" : "\033[?1004l" );
  }

  /* has mouse encoding mode changed? */
  if ( ( !initialized ) || ( f.ds.mouse_encoding_mode != frame.last_frame.ds.mouse_encoding_mode ) ) {
    if ( f.ds.mouse_encoding_mode == DrawState::MOUSE_ENCODING_DEFAULT ) {
      frame.append( "\033[?1015l" );
      frame.append( "\033[?1006l" );
      frame.append( "\033[?1005l" );
    } else {
      if ( frame.last_frame.ds.mouse_encoding_mode != DrawState::MOUSE_ENCODING_DEFAULT ) {
        snprintf( tmp, sizeof( tmp ), "\033[?%dl", frame.last_frame.ds.mouse_encoding_mode );
        frame.append( tmp );
      }
      snprintf( tmp, sizeof( tmp ), "\033[?%dh", f.ds.mouse_encoding_mode );
      frame.append( tmp );
    }
  }

  return frame.str;
}

bool Display::put_row( bool initialized,
                       FrameState& frame,
                       const Framebuffer& f,
                       int frame_y,
                       const Row& old_row,
                       bool wrap ) const
{
  char tmp[64];
  int frame_x = 0;

  const Row& row = *f.get_row( frame_y );
  const Row::cells_type& cells = row.cells;
  const Row::cells_type& old_cells = old_row.cells;

  /* If we're forced to write the first column because of wrap, go ahead and do so. */
  if ( wrap ) {
    const Cell& cell = cells.at( 0 );
    frame.update_rendition( cell.get_renditions() );
    frame.append_cell( cell );
    frame_x += cell.get_width();
    frame.cursor_x += cell.get_width();
  }

  /* If rows are the same object, we don't need to do anything at all. */
  if ( initialized && &row == &old_row ) {
    return false;
  }

  const bool wrap_this = row.get_wrap();
  const int row_width = f.ds.get_width();
  int clear_count = 0;
  bool wrote_last_cell = false;
  Renditions blank_renditions = initial_rendition();

  /* iterate for every cell */
  while ( frame_x < row_width ) {

    const Cell& cell = cells.at( frame_x );

    /* Does cell need to be drawn?  Skip all this. */
    if ( initialized && !clear_count && ( cell == old_cells.at( frame_x ) ) ) {
      frame_x += cell.get_width();
      continue;
    }

    /* Slurp up all the empty cells */
    if ( cell.empty() ) {
      if ( !clear_count ) {
        blank_renditions = cell.get_renditions();
      }
      if ( cell.get_renditions() == blank_renditions ) {
        /* Remember run of blank cells */
        clear_count++;
        frame_x++;
        continue;
      }
    }

    /* Clear or write cells within the row (not to end). */
    if ( clear_count ) {
      /* Move to the right position. */
      frame.append_silent_move( frame_y, frame_x - clear_count );
      frame.update_rendition( blank_renditions );
      bool can_use_erase = has_bce || ( frame.current_rendition == initial_rendition() );
      if ( can_use_erase && has_ech && clear_count > 4 ) {
        snprintf( tmp, 64, "\033[%dX", clear_count );
        frame.append( tmp );
      } else {
        frame.append( clear_count, ' ' );
        frame.cursor_x = frame_x;
      }
      clear_count = 0;
      // If the current character is *another* empty cell in a different rendition,
      // we restart counting and continue here
      if ( cell.empty() ) {
        blank_renditions = cell.get_renditions();
        clear_count = 1;
        frame_x++;
        continue;
      }
    }

    /* Now draw a character cell. */
    /* Move to the right position. */
    const int cell_width = cell.get_width();
    /* If we are about to print the last character in a wrapping row,
       trash the cursor position to force explicit positioning.  We do
       this because our input terminal state may have the cursor on
       the autowrap column ("column 81"), but our output terminal
       states always snap the cursor to the true last column ("column
       80"), and we want to be able to apply the diff to either, for
       verification. */
    if ( wrap_this && frame_x + cell_width >= row_width ) {
      frame.cursor_x = frame.cursor_y = -1;
    }
    frame.append_silent_move( frame_y, frame_x );
    frame.update_rendition( cell.get_renditions() );
    frame.append_cell( cell );
    frame_x += cell_width;
    frame.cursor_x += cell_width;
    if ( frame_x >= row_width ) {
      wrote_last_cell = true;
    }
  }

  /* End of line. */

  /* Clear or write empty cells at EOL. */
  if ( clear_count ) {
    /* Move to the right position. */
    frame.append_silent_move( frame_y, frame_x - clear_count );
    frame.update_rendition( blank_renditions );

    bool can_use_erase = has_bce || ( frame.current_rendition == initial_rendition() );
    if ( can_use_erase && !wrap_this ) {
      frame.append( "\033[K" );
    } else {
      frame.append( clear_count, ' ' );
      frame.cursor_x = frame_x;
      wrote_last_cell = true;
    }
  }

  if ( !( wrote_last_cell && ( frame_y < f.ds.get_height() - 1 ) ) ) {
    return false;
  }
  /* To hint that a word-select should group the end of one line
     with the beginning of the next, we let the real cursor
     actually wrap around in cases where it wrapped around for us. */
  if ( wrap_this ) {
    /* Update our cursor, and ask for wrap on the next row. */
    frame.cursor_x = 0;
    frame.cursor_y++;
    return true;
  }
  /* Resort to CR/LF and update our cursor. */
  frame.append( "\r\n" );
  frame.cursor_x = 0;
  frame.cursor_y++;
  return false;
}

FrameState::FrameState( const Framebuffer& s_last )
  : str(), cursor_x( 0 ), cursor_y( 0 ), current_rendition( 0 ), cursor_visible( s_last.ds.cursor_visible ),
    last_frame( s_last )
{
  /* Preallocate for better performance.  Make a guess-- doesn't matter for correctness */
  str.reserve( last_frame.ds.get_width() * last_frame.ds.get_height() * 4 );
}

void FrameState::append_silent_move( int y, int x )
{
  if ( cursor_x == x && cursor_y == y )
    return;
  /* turn off cursor if necessary before moving cursor */
  if ( cursor_visible ) {
    append( "\033[?25l" );
    cursor_visible = false;
  }
  append_move( y, x );
}

void FrameState::append_move( int y, int x )
{
  const int last_x = cursor_x;
  const int last_y = cursor_y;
  cursor_x = x;
  cursor_y = y;
  // Only optimize if cursor pos is known
  if ( last_x != -1 && last_y != -1 ) {
    // Can we use CR and/or LF?  They're cheap and easier to trace.
    if ( x == 0 && y - last_y >= 0 && y - last_y < 5 ) {
      if ( last_x != 0 ) {
        append( '\r' );
      }
      append( y - last_y, '\n' );
      return;
    }
    // Backspaces are good too.
    if ( y == last_y && x - last_x < 0 && x - last_x > -5 ) {
      append( last_x - x, '\b' );
      return;
    }
    // More optimizations are possible.
  }
  char tmp[64];
  snprintf( tmp, 64, "\033[%d;%dH", y + 1, x + 1 );
  append( tmp );
}

void FrameState::update_rendition( const Renditions& r, bool force )
{
  if ( force || !( current_rendition == r ) ) {
    /* print renditions */
    append_string( r.sgr() );
    current_rendition = r;
  }
}
