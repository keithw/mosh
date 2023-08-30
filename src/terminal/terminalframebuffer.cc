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
#include <cstdio>
#include <cstdlib>

#include "src/terminal/terminalframebuffer.h"

using namespace Terminal;

Cell::Cell( color_type background_color )
  : contents(), renditions( background_color ), wide( false ), fallback( false ), wrap( false )
{}

void Cell::reset( color_type background_color )
{
  contents.clear();
  renditions = Renditions( background_color );
  wide = false;
  fallback = false;
  wrap = false;
}

void DrawState::reinitialize_tabs( unsigned int start )
{
  assert( default_tabs );
  for ( unsigned int i = start; i < tabs.size(); i++ ) {
    tabs[i] = ( ( i % 8 ) == 0 );
  }
}

DrawState::DrawState( int s_width, int s_height )
  : width( s_width ), height( s_height ), cursor_col( 0 ), cursor_row( 0 ), combining_char_col( 0 ),
    combining_char_row( 0 ), default_tabs( true ), tabs( s_width ), scrolling_region_top_row( 0 ),
    scrolling_region_bottom_row( height - 1 ), renditions( 0 ), save(),
    cursor_style( Terminal::CursorStyle::BLINKING_BLOCK ), next_print_will_wrap( false ), origin_mode( false ),
    auto_wrap_mode( true ), insert_mode( false ), cursor_visible( true ), reverse_video( false ),
    bracketed_paste( false ), mouse_reporting_mode( MOUSE_REPORTING_NONE ), mouse_focus_event( false ),
    mouse_alternate_scroll( false ), mouse_encoding_mode( MOUSE_ENCODING_DEFAULT ),
    application_mode_cursor_keys( false )
{
  reinitialize_tabs( 0 );
}

Framebuffer::Framebuffer( int s_width, int s_height )
  : rows(), icon_name(), window_title(), clipboard(), bell_count( 0 ), title_initialized( false ),
    ds( s_width, s_height )
{
  assert( s_height > 0 );
  assert( s_width > 0 );
  const size_t w = s_width;
  const color_type c = 0;
  rows = rows_type( s_height, row_pointer( std::make_shared<Row>( w, c ) ) );
}

Framebuffer::Framebuffer( const Framebuffer& other )
  : rows( other.rows ), icon_name( other.icon_name ), window_title( other.window_title ),
    clipboard( other.clipboard ), bell_count( other.bell_count ), title_initialized( other.title_initialized ),
    ds( other.ds )
{}

Framebuffer& Framebuffer::operator=( const Framebuffer& other )
{
  if ( this != &other ) {
    rows = other.rows;
    icon_name = other.icon_name;
    window_title = other.window_title;
    clipboard = other.clipboard;
    bell_count = other.bell_count;
    title_initialized = other.title_initialized;
    ds = other.ds;
  }
  return *this;
}

void Framebuffer::scroll( int N )
{
  if ( N >= 0 ) {
    delete_line( ds.get_scrolling_region_top_row(), N );
  } else {
    insert_line( ds.get_scrolling_region_top_row(), -N );
  }
}

void DrawState::new_grapheme( void )
{
  combining_char_col = cursor_col;
  combining_char_row = cursor_row;
}

void DrawState::snap_cursor_to_border( void )
{
  if ( cursor_row < limit_top() )
    cursor_row = limit_top();
  if ( cursor_row > limit_bottom() )
    cursor_row = limit_bottom();
  if ( cursor_col < 0 )
    cursor_col = 0;
  if ( cursor_col >= width )
    cursor_col = width - 1;
}

void DrawState::move_row( int N, bool relative )
{
  if ( relative ) {
    cursor_row += N;
  } else {
    cursor_row = N + limit_top();
  }

  snap_cursor_to_border();
  new_grapheme();
  next_print_will_wrap = false;
}

void DrawState::move_col( int N, bool relative, bool implicit )
{
  if ( implicit ) {
    new_grapheme();
  }

  if ( relative ) {
    cursor_col += N;
  } else {
    cursor_col = N;
  }

  if ( implicit ) {
    next_print_will_wrap = ( cursor_col >= width );
  }

  snap_cursor_to_border();
  if ( !implicit ) {
    new_grapheme();
    next_print_will_wrap = false;
  }
}

void Framebuffer::move_rows_autoscroll( int rows )
{
  /* don't scroll if outside the scrolling region */
  if ( ( ds.get_cursor_row() < ds.get_scrolling_region_top_row() )
       || ( ds.get_cursor_row() > ds.get_scrolling_region_bottom_row() ) ) {
    ds.move_row( rows, true );
    return;
  }

  if ( ds.get_cursor_row() + rows > ds.get_scrolling_region_bottom_row() ) {
    int N = ds.get_cursor_row() + rows - ds.get_scrolling_region_bottom_row();
    scroll( N );
    ds.move_row( -N, true );
  } else if ( ds.get_cursor_row() + rows < ds.get_scrolling_region_top_row() ) {
    int N = ds.get_cursor_row() + rows - ds.get_scrolling_region_top_row();
    scroll( N );
    ds.move_row( -N, true );
  }

  ds.move_row( rows, true );
}

Cell* Framebuffer::get_combining_cell( void )
{
  if ( ( ds.get_combining_char_col() < 0 ) || ( ds.get_combining_char_row() < 0 )
       || ( ds.get_combining_char_col() >= ds.get_width() )
       || ( ds.get_combining_char_row() >= ds.get_height() ) ) {
    return NULL;
  } /* can happen if a resize came in between */

  return get_mutable_cell( ds.get_combining_char_row(), ds.get_combining_char_col() );
}

void DrawState::set_tab( void )
{
  tabs[cursor_col] = true;
}

void DrawState::clear_tab( int col )
{
  tabs[col] = false;
}

int DrawState::get_next_tab( int count ) const
{
  if ( count >= 0 ) {
    for ( int i = cursor_col + 1; i < width; i++ ) {
      if ( tabs[i] && --count == 0 ) {
        return i;
      }
    }
    return -1;
  }
  for ( int i = cursor_col - 1; i > 0; i-- ) {
    if ( tabs[i] && ++count == 0 ) {
      return i;
    }
  }
  return 0;
}

void DrawState::set_scrolling_region( int top, int bottom )
{
  if ( height < 1 ) {
    return;
  }

  scrolling_region_top_row = top;
  scrolling_region_bottom_row = bottom;

  if ( scrolling_region_top_row < 0 )
    scrolling_region_top_row = 0;
  if ( scrolling_region_bottom_row >= height )
    scrolling_region_bottom_row = height - 1;

  if ( scrolling_region_bottom_row < scrolling_region_top_row )
    scrolling_region_bottom_row = scrolling_region_top_row;
  /* real rule requires TWO-line scrolling region */

  if ( origin_mode ) {
    snap_cursor_to_border();
    new_grapheme();
  }
}

int DrawState::limit_top( void ) const
{
  return origin_mode ? scrolling_region_top_row : 0;
}

int DrawState::limit_bottom( void ) const
{
  return origin_mode ? scrolling_region_bottom_row : height - 1;
}

void Framebuffer::apply_renditions_to_cell( Cell* cell )
{
  if ( !cell ) {
    cell = get_mutable_cell();
  }
  cell->set_renditions( ds.get_renditions() );
}

SavedCursor::SavedCursor()
  : cursor_col( 0 ), cursor_row( 0 ), renditions( 0 ), auto_wrap_mode( true ), origin_mode( false )
{}

void DrawState::save_cursor( void )
{
  save.cursor_col = cursor_col;
  save.cursor_row = cursor_row;
  save.renditions = renditions;
  save.auto_wrap_mode = auto_wrap_mode;
  save.origin_mode = origin_mode;
}

void DrawState::restore_cursor( void )
{
  cursor_col = save.cursor_col;
  cursor_row = save.cursor_row;
  renditions = save.renditions;
  auto_wrap_mode = save.auto_wrap_mode;
  origin_mode = save.origin_mode;

  snap_cursor_to_border(); /* we could have resized in between */
  new_grapheme();
}

void Framebuffer::insert_line( int before_row, int count )
{
  if ( ( before_row < ds.get_scrolling_region_top_row() )
       || ( before_row > ds.get_scrolling_region_bottom_row() + 1 ) ) {
    return;
  }

  int scroll = ds.get_scrolling_region_bottom_row() + 1 - before_row;
  if ( count < scroll ) {
    scroll = count;
  }

  if ( scroll == 0 ) {
    return;
  }

  // delete old rows
  rows_type::iterator start = rows.begin() + ds.get_scrolling_region_bottom_row() + 1 - scroll;
  rows.erase( start, start + scroll );
  // insert new rows
  start = rows.begin() + before_row;
  rows.insert( start, scroll, newrow() );
}

void Framebuffer::delete_line( int row, int count )
{
  if ( ( row < ds.get_scrolling_region_top_row() ) || ( row > ds.get_scrolling_region_bottom_row() ) ) {
    return;
  }

  int scroll = ds.get_scrolling_region_bottom_row() + 1 - row;
  if ( count < scroll ) {
    scroll = count;
  }

  if ( scroll == 0 ) {
    return;
  }

  // delete old rows
  rows_type::iterator start = rows.begin() + row;
  rows.erase( start, start + scroll );
  // insert a block of dummy rows
  start = rows.begin() + ds.get_scrolling_region_bottom_row() + 1 - scroll;
  rows.insert( start, scroll, newrow() );
}

Row::Row( const size_t s_width, const color_type background_color )
  : cells( s_width, Cell( background_color ) ), gen( get_gen() )
{}

uint64_t Row::get_gen() const
{
  static uint64_t gen_counter = 0;
  return gen_counter++;
}

void Row::insert_cell( int col, color_type background_color )
{
  cells.insert( cells.begin() + col, Cell( background_color ) );
  cells.pop_back();
}

void Row::delete_cell( int col, color_type background_color )
{
  cells.push_back( Cell( background_color ) );
  cells.erase( cells.begin() + col );
}

void Framebuffer::insert_cell( int row, int col )
{
  get_mutable_row( row )->insert_cell( col, ds.get_background_rendition() );
}

void Framebuffer::delete_cell( int row, int col )
{
  get_mutable_row( row )->delete_cell( col, ds.get_background_rendition() );
}

void Framebuffer::reset( void )
{
  int width = ds.get_width(), height = ds.get_height();
  ds = DrawState( width, height );
  rows = rows_type( height, newrow() );
  window_title.clear();
  clipboard.clear();
  /* do not reset bell_count */
}

void Framebuffer::soft_reset( void )
{
  ds.insert_mode = false;
  ds.origin_mode = false;
  ds.cursor_visible = true; /* per xterm and gnome-terminal */
  ds.cursor_style = Terminal::CursorStyle::BLINKING_BLOCK;
  ds.application_mode_cursor_keys = false;
  ds.set_scrolling_region( 0, ds.get_height() - 1 );
  ds.add_rendition( 0 );
  ds.clear_saved_cursor();
}

void Framebuffer::resize( int s_width, int s_height )
{
  assert( s_width > 0 );
  assert( s_height > 0 );

  int oldheight = ds.get_height();
  int oldwidth = ds.get_width();
  ds.resize( s_width, s_height );

  row_pointer blankrow( newrow() );
  if ( oldheight != s_height ) {
    rows.resize( s_height, blankrow );
  }
  if ( oldwidth == s_width ) {
    return;
  }
  for ( rows_type::iterator i = rows.begin(); i != rows.end() && *i != blankrow; i++ ) {
    *i = std::make_shared<Row>( **i );
    ( *i )->set_wrap( false );
    ( *i )->cells.resize( s_width, Cell( ds.get_background_rendition() ) );
  }
}

void DrawState::resize( int s_width, int s_height )
{
  if ( ( width != s_width ) || ( height != s_height ) ) {
    /* reset entire scrolling region on any resize */
    /* xterm and rxvt-unicode do this. gnome-terminal only
       resets scrolling region if it has to become smaller in resize */
    scrolling_region_top_row = 0;
    scrolling_region_bottom_row = s_height - 1;
  }

  tabs.resize( s_width );
  if ( default_tabs ) {
    reinitialize_tabs( width );
  }

  width = s_width;
  height = s_height;

  snap_cursor_to_border();

  /* saved cursor will be snapped to border on restore */

  /* invalidate combining char cell if necessary */
  if ( ( combining_char_col >= width ) || ( combining_char_row >= height ) ) {
    combining_char_col = combining_char_row = -1;
  }
}

Renditions::Renditions( color_type s_background )
  : foreground_color( 0 ), background_color( s_background ), attributes( 0 )
{}

/* This routine cannot be used to set a color beyond the 16-color set. */
void Renditions::set_rendition( color_type num )
{
  if ( num == 0 ) {
    clear_attributes();
    foreground_color = background_color = 0;
    return;
  }

  if ( num == 39 ) {
    foreground_color = 0;
    return;
  } else if ( num == 49 ) {
    background_color = 0;
    return;
  }

  if ( ( 30 <= num ) && ( num <= 37 ) ) { /* foreground color in 8-color set */
    foreground_color = num;
    return;
  } else if ( ( 40 <= num ) && ( num <= 47 ) ) { /* background color in 8-color set */
    background_color = num;
    return;
  } else if ( ( 90 <= num ) && ( num <= 97 ) ) { /* foreground color in 16-color set */
    foreground_color = num - 90 + 38;
    return;
  } else if ( ( 100 <= num ) && ( num <= 107 ) ) { /* background color in 16-color set */
    background_color = num - 100 + 48;
    return;
  }

  bool value = num < 9;
  switch ( num ) {
    case 1:
    case 22:
      set_attribute( bold, value );
      break;
    case 3:
    case 23:
      set_attribute( italic, value );
      break;
    case 4:
    case 24:
      set_attribute( underlined, value );
      break;
    case 5:
    case 25:
      set_attribute( blink, value );
      break;
    case 7:
    case 27:
      set_attribute( inverse, value );
      break;
    case 8:
    case 28:
      set_attribute( invisible, value );
      break;
    default:
      break; /* ignore unknown rendition */
  }
}

void Renditions::set_foreground_color( int num )
{
  if ( ( 0 <= num ) && ( num <= 255 ) ) {
    foreground_color = 30 + num;
  } else if ( is_true_color( num ) ) {
    foreground_color = num;
  }
}

void Renditions::set_background_color( int num )
{
  if ( ( 0 <= num ) && ( num <= 255 ) ) {
    background_color = 40 + num;
  } else if ( is_true_color( num ) ) {
    background_color = num;
  }
}

std::string Renditions::sgr( void ) const
{
  std::string ret;
  char col[64];

  ret.append( "\033[0" );
  if ( get_attribute( bold ) )
    ret.append( ";1" );
  if ( get_attribute( italic ) )
    ret.append( ";3" );
  if ( get_attribute( underlined ) )
    ret.append( ";4" );
  if ( get_attribute( blink ) )
    ret.append( ";5" );
  if ( get_attribute( inverse ) )
    ret.append( ";7" );
  if ( get_attribute( invisible ) )
    ret.append( ";8" );

  if ( foreground_color ) {
    // Since foreground_color is a 25-bit field, it is promoted to an int when
    // manipulated. (See [conv.prom] in various C++ standards, e.g.,
    // https://timsong-cpp.github.io/cppwp/n4659/conv.prom#5.) The correct
    // printf format specifier is thus %d.
    if ( is_true_color( foreground_color ) ) {
      snprintf( col,
                sizeof( col ),
                ";38;2;%d;%d;%d",
                ( foreground_color >> 16 ) & 0xff,
                ( foreground_color >> 8 ) & 0xff,
                foreground_color & 0xff );
    } else if ( foreground_color > 37 ) { /* use 256-color set */
      snprintf( col, sizeof( col ), ";38;5;%d", foreground_color - 30 );
    } else { /* ANSI foreground color */
      // Unfortunately, some versions of GCC (notably including GCC 9.3) give
      // -Wformat warnings when relying on [conv.prom] to promote
      // foreground_color in calls to printf. Explicitly promote it to silence
      // the warning.
      int fg = foreground_color;
      snprintf( col, sizeof( col ), ";%d", fg );
    }
    ret.append( col );
  }
  if ( background_color ) {
    // See comment above about bit-field promotion; it applies here as well.
    if ( is_true_color( background_color ) ) {
      snprintf( col,
                sizeof( col ),
                ";48;2;%d;%d;%d",
                ( background_color >> 16 ) & 0xff,
                ( background_color >> 8 ) & 0xff,
                background_color & 0xff );
    } else if ( background_color > 47 ) { /* use 256-color set */
      snprintf( col, sizeof( col ), ";48;5;%d", background_color - 40 );
    } else { /* ANSI background color */
      // See comment above about explicit promotion; it applies here as well.
      int bg = background_color;
      snprintf( col, sizeof( col ), ";%d", bg );
    }
    ret.append( col );
  }
  ret.append( "m" );

  return ret;
}

void Row::reset( color_type background_color )
{
  gen = get_gen();
  for ( cells_type::iterator i = cells.begin(); i != cells.end(); i++ ) {
    i->reset( background_color );
  }
}

void Framebuffer::prefix_window_title( const title_type& s )
{
  if ( icon_name == window_title ) {
    /* preserve equivalence */
    icon_name.insert( icon_name.begin(), s.begin(), s.end() );
  }
  window_title.insert( window_title.begin(), s.begin(), s.end() );
}

std::string Cell::debug_contents( void ) const
{
  if ( contents.empty() ) {
    return "'_' ()";
  }
  std::string chars( 1, '\'' );
  print_grapheme( chars );
  chars.append( "' [" );
  const char* lazycomma = "";
  char buf[64];
  for ( content_type::const_iterator i = contents.begin(); i < contents.end(); i++ ) {

    snprintf( buf, sizeof buf, "%s0x%02x", lazycomma, static_cast<uint8_t>( *i ) );
    chars.append( buf );
    lazycomma = ", ";
  }
  chars.append( "]" );
  return chars;
}

bool Cell::compare( const Cell& other ) const
{
  bool ret = false;

  std::string grapheme, other_grapheme;

  print_grapheme( grapheme );
  other.print_grapheme( other_grapheme );

  if ( grapheme != other_grapheme ) {
    ret = true;
    fprintf( stderr, "Graphemes: '%s' vs. '%s'\n", grapheme.c_str(), other_grapheme.c_str() );
  }

  if ( !contents_match( other ) ) {
    // ret = true;
    fprintf( stderr,
             "Contents: %s (%ld) vs. %s (%ld)\n",
             debug_contents().c_str(),
             static_cast<long int>( contents.size() ),
             other.debug_contents().c_str(),
             static_cast<long int>( other.contents.size() ) );
  }

  if ( fallback != other.fallback ) {
    // ret = true;
    // Since fallback is a 1-bit field, it is promoted to an int when
    // manipulated. (See [conv.prom] in various C++ standards, e.g.,
    // https://timsong-cpp.github.io/cppwp/n4659/conv.prom#5.) The correct
    // printf format specifier is thus %d.
    fprintf( stderr, "fallback: %d vs. %d\n", fallback, other.fallback );
  }

  if ( wide != other.wide ) {
    ret = true;
    // See comment above about bit-field promotion; it applies here as well.
    fprintf( stderr, "width: %d vs. %d\n", wide, other.wide );
  }

  if ( !( renditions == other.renditions ) ) {
    ret = true;
    fprintf( stderr, "renditions differ\n" );
  }

  if ( wrap != other.wrap ) {
    ret = true;
    // See comment above about bit-field promotion; it applies here as well.
    fprintf( stderr, "wrap: %d vs. %d\n", wrap, other.wrap );
  }

  return ret;
}
