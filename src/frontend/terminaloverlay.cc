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
#include <climits>
#include <cwchar>
#include <list>
#include <typeinfo>

#include "src/frontend/terminaloverlay.h"
#include "src/util/char_utils.h"

using namespace Overlay;

void ConditionalOverlayCell::apply( Framebuffer& fb, uint64_t confirmed_epoch, int row, bool flag ) const
{
  if ( ( !active ) || ( row >= fb.ds.get_height() ) || ( col >= fb.ds.get_width() ) ) {
    return;
  }

  if ( tentative( confirmed_epoch ) ) {
    return;
  }

  if ( replacement.is_blank() && fb.get_cell( row, col )->is_blank() ) {
    flag = false;
  }

  if ( unknown ) {
    if ( flag && ( col != fb.ds.get_width() - 1 ) ) {
      fb.get_mutable_cell( row, col )->get_renditions().set_attribute( Renditions::underlined, true );
    }
    return;
  }

  if ( *fb.get_cell( row, col ) != replacement ) {
    *( fb.get_mutable_cell( row, col ) ) = replacement;
    if ( flag ) {
      fb.get_mutable_cell( row, col )->get_renditions().set_attribute( Renditions::underlined, true );
    }
  }
}

Validity ConditionalOverlayCell::get_validity( const Framebuffer& fb,
                                               int row,
                                               uint64_t early_ack __attribute__( ( unused ) ),
                                               uint64_t late_ack ) const
{
  if ( !active ) {
    return Inactive;
  }

  if ( ( row >= fb.ds.get_height() ) || ( col >= fb.ds.get_width() ) ) {
    return IncorrectOrExpired;
  }

  const Cell& current = *( fb.get_cell( row, col ) );

  /* see if it hasn't been updated yet */
  if ( late_ack < expiration_frame ) {
    return Pending;
  }

  if ( unknown ) {
    return CorrectNoCredit;
  }

  if ( replacement.is_blank() ) { /* too easy for this to trigger falsely */
    return CorrectNoCredit;
  }

  if ( current.contents_match( replacement ) ) {
    std::vector<Cell>::const_iterator it = original_contents.begin();
    for ( ; it != original_contents.end(); it++ ) {
      if ( it->contents_match( replacement ) )
        break;
    }
    if ( it == original_contents.end() ) {
      return Correct;
    }
    return CorrectNoCredit;
  }
  return IncorrectOrExpired;
}

Validity ConditionalCursorMove::get_validity( const Framebuffer& fb,
                                              uint64_t early_ack __attribute( ( unused ) ),
                                              uint64_t late_ack ) const
{
  if ( !active ) {
    return Inactive;
  }

  if ( ( row >= fb.ds.get_height() ) || ( col >= fb.ds.get_width() ) ) {
    //    fprintf( stderr, "Crazy cursor (%d,%d)!\n", row, col );
    return IncorrectOrExpired;
  }

  if ( late_ack >= expiration_frame ) {
    if ( ( fb.ds.get_cursor_col() == col ) && ( fb.ds.get_cursor_row() == row ) ) {
      return Correct;
    }
    return IncorrectOrExpired;
  }

  return Pending;
}

void ConditionalCursorMove::apply( Framebuffer& fb, uint64_t confirmed_epoch ) const
{
  if ( !active ) {
    return;
  }

  if ( tentative( confirmed_epoch ) ) {
    return;
  }

  assert( row < fb.ds.get_height() );
  assert( col < fb.ds.get_width() );
  assert( !fb.ds.origin_mode );

  fb.ds.move_row( row, false );
  fb.ds.move_col( col, false, false );
}

NotificationEngine::NotificationEngine()
  : last_word_from_server( timestamp() ), last_acked_state( timestamp() ), escape_key_string(), message(),
    message_is_network_error( false ), message_expiration( -1 ), show_quit_keystroke( true )
{}

static std::string human_readable_duration( int num_seconds, const std::string& seconds_abbr )
{
  char tmp[128];
  if ( num_seconds < 60 ) {
    snprintf( tmp, 128, "%d %s", num_seconds, seconds_abbr.c_str() );
  } else if ( num_seconds < 3600 ) {
    snprintf( tmp, 128, "%d:%02d", num_seconds / 60, num_seconds % 60 );
  } else {
    snprintf( tmp, 128, "%d:%02d:%02d", num_seconds / 3600, ( num_seconds / 60 ) % 60, num_seconds % 60 );
  }
  return tmp;
}

void NotificationEngine::apply( Framebuffer& fb ) const
{
  uint64_t now = timestamp();

  bool time_expired = need_countup( now );

  if ( message.empty() && !time_expired ) {
    return;
  }

  assert( fb.ds.get_width() > 0 );
  assert( fb.ds.get_height() > 0 );

  /* hide cursor if necessary */
  if ( fb.ds.get_cursor_row() == 0 ) {
    fb.ds.cursor_visible = false;
  }

  /* draw bar across top of screen */
  Cell notification_bar( 0 );
  notification_bar.get_renditions().set_foreground_color( 7 );
  notification_bar.get_renditions().set_background_color( 4 );
  notification_bar.append( 0x20 );

  for ( int i = 0; i < fb.ds.get_width(); i++ ) {
    *( fb.get_mutable_cell( 0, i ) ) = notification_bar;
  }

  /* write message */
  wchar_t tmp[128];

  /* We want to prefer the "last contact" message if we simply haven't
     heard from the server in a while, but print the "last reply" message
     if the problem is uplink-only. */

  double since_heard = (double)( now - last_word_from_server ) / 1000.0;
  double since_ack = (double)( now - last_acked_state ) / 1000.0;
  const char server_message[] = "contact";
  const char reply_message[] = "reply";

  double time_elapsed = since_heard;
  const char* explanation = server_message;

  if ( reply_late( now ) && ( !server_late( now ) ) ) {
    time_elapsed = since_ack;
    explanation = reply_message;
  }

  const static char blank[] = "";

  const char* keystroke_str = show_quit_keystroke ? escape_key_string.c_str() : blank;

  if ( message.empty() && ( !time_expired ) ) {
    return;
  }
  if ( message.empty() && time_expired ) {
    swprintf( tmp,
              128,
              L"mosh: Last %s %s ago.%s",
              explanation,
              human_readable_duration( static_cast<int>( time_elapsed ), "seconds" ).c_str(),
              keystroke_str );
  } else if ( ( !message.empty() ) && ( !time_expired ) ) {
    swprintf( tmp, 128, L"mosh: %ls%s", message.c_str(), keystroke_str );
  } else {
    swprintf( tmp,
              128,
              L"mosh: %ls (%s without %s.)%s",
              message.c_str(),
              human_readable_duration( static_cast<int>( time_elapsed ), "s" ).c_str(),
              explanation,
              keystroke_str );
  }

  std::wstring string_to_draw( tmp );

  int overlay_col = 0;

  Cell* combining_cell = fb.get_mutable_cell( 0, 0 );

  /* We unfortunately duplicate the terminal's logic for how to render a Unicode sequence into graphemes */
  for ( std::wstring::const_iterator i = string_to_draw.begin(); i != string_to_draw.end(); i++ ) {
    if ( overlay_col >= fb.ds.get_width() ) {
      break;
    }

    wchar_t ch = *i;
    int chwidth = ch == L'\0' ? -1 : mosh_wcwidth( ch );
    Cell* this_cell = 0;

    switch ( chwidth ) {
      case 1: /* normal character */
      case 2: /* wide character */
        this_cell = fb.get_mutable_cell( 0, overlay_col );
        fb.reset_cell( this_cell );
        this_cell->get_renditions().set_attribute( Renditions::bold, true );
        this_cell->get_renditions().set_foreground_color( 7 );
        this_cell->get_renditions().set_background_color( 4 );

        this_cell->append( ch );
        this_cell->set_wide( chwidth == 2 );
        combining_cell = this_cell;

        overlay_col += chwidth;
        break;
      case 0: /* combining character */
        if ( !combining_cell ) {
          break;
        }

        if ( combining_cell->empty() ) {
          assert( !combining_cell->get_wide() );
          combining_cell->set_fallback( true );
          overlay_col++;
        }

        if ( !combining_cell->full() ) {
          combining_cell->append( ch );
        }
        break;
      case -1: /* unprintable character */
        break;
      default:
        assert( !"unexpected character width from wcwidth()" );
    }
  }
}

void NotificationEngine::adjust_message( void )
{
  if ( timestamp() >= message_expiration ) {
    message.clear();
  }
}

int NotificationEngine::wait_time( void ) const
{
  uint64_t next_expiry = INT_MAX;

  uint64_t now = timestamp();

  next_expiry = std::min( next_expiry, message_expiration - now );

  if ( need_countup( now ) ) {
    uint64_t countup_interval = 1000;
    if ( ( now - last_word_from_server ) > 60000 ) {
      /* If we've been disconnected for 60 seconds, save power by updating the
         display less often.  See #243. */
      countup_interval = Network::ACK_INTERVAL;
    }
    next_expiry = std::min( next_expiry, countup_interval );
  }

  return next_expiry;
}

void OverlayManager::apply( Framebuffer& fb )
{
  predictions.cull( fb );
  predictions.apply( fb );
  notifications.adjust_message();
  notifications.apply( fb );
  title.apply( fb );
}

void TitleEngine::set_prefix( const std::wstring& s )
{
  prefix = Terminal::Framebuffer::title_type( s.begin(), s.end() );
}

void ConditionalOverlayRow::apply( Framebuffer& fb, uint64_t confirmed_epoch, bool flag ) const
{
  for ( overlay_cells_type::const_iterator it = overlay_cells.begin(); it != overlay_cells.end(); it++ ) {
    it->apply( fb, confirmed_epoch, row_num, flag );
  }
}

void PredictionEngine::apply( Framebuffer& fb ) const
{
  if ( ( display_preference == Never )
       || !( srtt_trigger || glitch_trigger || ( display_preference == Always )
             || ( display_preference == Experimental ) ) ) {
    return;
  }

  for ( cursors_type::const_iterator it = cursors.begin(); it != cursors.end(); it++ ) {
    it->apply( fb, confirmed_epoch );
  }

  for ( overlays_type::const_iterator it = overlays.begin(); it != overlays.end(); it++ ) {
    it->apply( fb, confirmed_epoch, flagging );
  }
}

void PredictionEngine::kill_epoch( uint64_t epoch, const Framebuffer& fb )
{
  for ( cursors_type::iterator it = cursors.begin(); it != cursors.end(); ) {
    cursors_type::iterator it_next = it;
    it_next++;
    if ( it->tentative( epoch - 1 ) ) {
      cursors.erase( it );
    }
    it = it_next;
  }

  cursors.push_back( ConditionalCursorMove(
    local_frame_sent + 1, fb.ds.get_cursor_row(), fb.ds.get_cursor_col(), prediction_epoch ) );
  cursor().active = true;

  for ( overlays_type::iterator i = overlays.begin(); i != overlays.end(); i++ ) {
    for ( overlay_cells_type::iterator j = i->overlay_cells.begin(); j != i->overlay_cells.end(); j++ ) {
      if ( j->tentative( epoch - 1 ) ) {
        j->reset();
      }
    }
  }

  become_tentative();
}

void PredictionEngine::reset( void )
{
  cursors.clear();
  overlays.clear();
  become_tentative();

  //  fprintf( stderr, "RESETTING\n" );
}

void PredictionEngine::init_cursor( const Framebuffer& fb )
{
  if ( cursors.empty() ) {
    /* initialize new cursor prediction */

    cursors.push_back( ConditionalCursorMove(
      local_frame_sent + 1, fb.ds.get_cursor_row(), fb.ds.get_cursor_col(), prediction_epoch ) );

    cursor().active = true;
  } else if ( cursor().tentative_until_epoch != prediction_epoch ) {
    cursors.push_back(
      ConditionalCursorMove( local_frame_sent + 1, cursor().row, cursor().col, prediction_epoch ) );

    cursor().active = true;
  }
}

void PredictionEngine::cull( const Framebuffer& fb )
{
  if ( display_preference == Never ) {
    return;
  }

  if ( ( last_height != fb.ds.get_height() ) || ( last_width != fb.ds.get_width() ) ) {
    last_height = fb.ds.get_height();
    last_width = fb.ds.get_width();
    reset();
  }

  uint64_t now = timestamp();

  /* control srtt_trigger with hysteresis */
  if ( send_interval > SRTT_TRIGGER_HIGH ) {
    srtt_trigger = true;
  } else if ( srtt_trigger && ( send_interval <= SRTT_TRIGGER_LOW ) /* 20 ms is current minimum value */
              && ( !active() ) ) { /* only turn off when no predictions being shown */
    srtt_trigger = false;
  }

  /* control underlining with hysteresis */
  if ( send_interval > FLAG_TRIGGER_HIGH ) {
    flagging = true;
  } else if ( send_interval <= FLAG_TRIGGER_LOW ) {
    flagging = false;
  }

  /* really big glitches also activate underlining */
  if ( glitch_trigger > GLITCH_REPAIR_COUNT ) {
    flagging = true;
  }

  /* go through cell predictions */

  overlays_type::iterator i = overlays.begin();
  while ( i != overlays.end() ) {
    overlays_type::iterator inext = i;
    inext++;
    if ( ( i->row_num < 0 ) || ( i->row_num >= fb.ds.get_height() ) ) {
      overlays.erase( i );
      i = inext;
      continue;
    }

    for ( overlay_cells_type::iterator j = i->overlay_cells.begin(); j != i->overlay_cells.end(); j++ ) {
      switch ( j->get_validity( fb, i->row_num, local_frame_acked, local_frame_late_acked ) ) {
        case IncorrectOrExpired:
          if ( j->tentative( confirmed_epoch ) ) {

            /*
            fprintf( stderr, "Bad tentative prediction in row %d, col %d (think %lc, actually %lc)\n",
                     i->row_num, j->col,
                     j->replacement.debug_contents(),
                     fb.get_cell( i->row_num, j->col )->debug_contents()
                     );
            */

            if ( display_preference == Experimental ) {
              j->reset();
            } else {
              kill_epoch( j->tentative_until_epoch, fb );
            }
            /*
            if ( j->display_time != uint64_t(-1) ) {
              fprintf( stderr, "TIMING %ld - %ld (TENT)\n", time(NULL), now - j->display_time );
            }
            */
          } else {
            /*
            fprintf( stderr, "[%d=>%d] Killing prediction in row %d, col %d (think %lc, actually %lc)\n",
                     (int)local_frame_acked, (int)j->expiration_frame,
                     i->row_num, j->col,
                     j->replacement.debug_contents(),
                     fb.get_cell( i->row_num, j->col )->debug_contents() );
            */
            /*
            if ( j->display_time != uint64_t(-1) ) {
              fprintf( stderr, "TIMING %ld - %ld\n", time(NULL), now - j->display_time );
            }
            */

            if ( display_preference == Experimental ) {
              j->reset();
            } else {
              reset();
              return;
            }
          }
          break;
        case Correct:
          /*
          if ( j->display_time != uint64_t(-1) ) {
            fprintf( stderr, "TIMING %ld + %ld\n", now, now - j->display_time );
          }
          */

          if ( j->tentative_until_epoch > confirmed_epoch ) {
            confirmed_epoch = j->tentative_until_epoch;

            /*
            fprintf( stderr, "%lc in (%d,%d) confirms epoch %lu (predicting in epoch %lu)\n",
                     j->replacement.debug_contents(), i->row_num, j->col,
                     confirmed_epoch, prediction_epoch );
            */
          }

          /* When predictions come in quickly, slowly take away the glitch trigger. */
          if ( now - j->prediction_time < GLITCH_THRESHOLD
               && ( glitch_trigger > 0 && now - GLITCH_REPAIR_MININTERVAL >= last_quick_confirmation ) ) {
            glitch_trigger--;
            last_quick_confirmation = now;
          }

          /* match rest of row to the actual renditions */
          {
            const Renditions& actual_renditions = fb.get_cell( i->row_num, j->col )->get_renditions();
            for ( overlay_cells_type::iterator k = j; k != i->overlay_cells.end(); k++ ) {
              k->replacement.get_renditions() = actual_renditions;
            }
          }

          /* fallthrough */
        case CorrectNoCredit:
          j->reset();

          break;
        case Pending:
          /* When a prediction takes a long time to be confirmed, we
             activate the predictions even if SRTT is low */
          if ( ( now - j->prediction_time ) >= GLITCH_FLAG_THRESHOLD ) {
            glitch_trigger = GLITCH_REPAIR_COUNT * 2; /* display and underline */
          } else if ( ( ( now - j->prediction_time ) >= GLITCH_THRESHOLD )
                      && ( glitch_trigger < GLITCH_REPAIR_COUNT ) ) {
            glitch_trigger = GLITCH_REPAIR_COUNT; /* just display */
          }

          break;
        default:
          break;
      }
    }

    i = inext;
  }

  /* go through cursor predictions */
  if ( !cursors.empty()
       && cursor().get_validity( fb, local_frame_acked, local_frame_late_acked ) == IncorrectOrExpired ) {
    /*
      fprintf( stderr, "Sadly, we're predicting (%d,%d) vs. (%d,%d) [tau: %ld, expiration_time=%ld, now=%ld]\n",
      cursor().row, cursor().col,
      fb.ds.get_cursor_row(),
      fb.ds.get_cursor_col(),
      cursor().tentative_until_epoch,
      cursor().expiration_time,
      now );
    */
    if ( display_preference == Experimental ) {
      cursors.clear();
    } else {
      reset();
      return;
    }
  }

  /* NB: switching from list to another STL container could break this code.
     So we don't use the cursors_type typedef. */
  for ( std::list<ConditionalCursorMove>::iterator it = cursors.begin(); it != cursors.end(); ) {
    if ( it->get_validity( fb, local_frame_acked, local_frame_late_acked ) != Pending ) {
      it = cursors.erase( it );
    } else {
      it++;
    }
  }
}

ConditionalOverlayRow& PredictionEngine::get_or_make_row( int row_num, int num_cols )
{
  overlays_type::iterator it;

  for ( it = overlays.begin(); it != overlays.end(); it++ ) {
    if ( it->row_num == row_num ) {
      break;
    }
  }

  if ( it != overlays.end() ) {
    return *it;
  }
  /* make row */
  ConditionalOverlayRow r( row_num );
  r.overlay_cells.reserve( num_cols );
  for ( int i = 0; i < num_cols; i++ ) {
    r.overlay_cells.push_back( ConditionalOverlayCell( 0, i, prediction_epoch ) );
    assert( r.overlay_cells[i].col == i );
  }
  overlays.push_back( r );
  return overlays.back();
}

void PredictionEngine::new_user_byte( char the_byte, const Framebuffer& fb )
{
  if ( display_preference == Never ) {
    return;
  }
  if ( display_preference == Experimental ) {
    prediction_epoch = confirmed_epoch;
  }

  cull( fb );

  uint64_t now = timestamp();

  /* translate application-mode cursor control function to ANSI cursor control sequence */
  if ( ( last_byte == 0x1b ) && ( the_byte == 'O' ) ) {
    the_byte = '[';
  }
  last_byte = the_byte;

  Parser::Actions actions;
  parser.input( the_byte, actions );

  for ( Parser::Actions::iterator it = actions.begin(); it != actions.end(); it++ ) {
    Parser::Action& act = **it;

    /*
    fprintf( stderr, "Action: %s (%lc)\n",
             act->name().c_str(), act->char_present ? act->ch : L'_' );
    */

    const std::type_info& type_act = typeid( act );
    if ( type_act == typeid( Parser::Print ) ) {
      /* make new prediction */

      init_cursor( fb );

      assert( act.char_present );

      wchar_t ch = act.ch;
      /* XXX handle wide characters */

      if ( ch == 0x7f ) { /* backspace */
        //	fprintf( stderr, "Backspace.\n" );
        ConditionalOverlayRow& the_row = get_or_make_row( cursor().row, fb.ds.get_width() );

        if ( cursor().col > 0 ) {
          cursor().col--;
          cursor().expire( local_frame_sent + 1, now );

          if ( predict_overwrite ) {
            ConditionalOverlayCell& cell = the_row.overlay_cells[cursor().col];
            cell.reset_with_orig();
            cell.active = true;
            cell.tentative_until_epoch = prediction_epoch;
            cell.expire( local_frame_sent + 1, now );
            const Cell orig_cell = *fb.get_cell();
            cell.original_contents.push_back( orig_cell );
            cell.replacement = orig_cell;
            cell.replacement.clear();
            cell.replacement.append( ' ' );
          } else {
            for ( int i = cursor().col; i < fb.ds.get_width(); i++ ) {
              ConditionalOverlayCell& cell = the_row.overlay_cells[i];

              cell.reset_with_orig();
              cell.active = true;
              cell.tentative_until_epoch = prediction_epoch;
              cell.expire( local_frame_sent + 1, now );
              cell.original_contents.push_back( *fb.get_cell( cursor().row, i ) );

              if ( i + 2 < fb.ds.get_width() ) {
                ConditionalOverlayCell& next_cell = the_row.overlay_cells[i + 1];
                const Cell* next_cell_actual = fb.get_cell( cursor().row, i + 1 );

                if ( next_cell.active ) {
                  if ( next_cell.unknown ) {
                    cell.unknown = true;
                  } else {
                    cell.unknown = false;
                    cell.replacement = next_cell.replacement;
                  }
                } else {
                  cell.unknown = false;
                  cell.replacement = *next_cell_actual;
                }
              } else {
                cell.unknown = true;
              }
            }
          }
        }
      } else if ( ( ch < 0x20 ) || ( mosh_wcwidth( ch ) != 1 ) ) {
        /* unknown print */
        become_tentative();
        //	fprintf( stderr, "Unknown print 0x%x\n", ch );
      } else {
        assert( cursor().row >= 0 );
        assert( cursor().col >= 0 );
        assert( cursor().row < fb.ds.get_height() );
        assert( cursor().col < fb.ds.get_width() );

        ConditionalOverlayRow& the_row = get_or_make_row( cursor().row, fb.ds.get_width() );

        if ( cursor().col + 1 >= fb.ds.get_width() ) {
          /* prediction in the last column is tricky */
          /* e.g., emacs will show wrap character, shell will just put the character there */
          become_tentative();
        }

        /* do the insert */
        int rightmost_column = predict_overwrite ? cursor().col : fb.ds.get_width() - 1;
        for ( int i = rightmost_column; i > cursor().col; i-- ) {
          ConditionalOverlayCell& cell = the_row.overlay_cells[i];
          cell.reset_with_orig();
          cell.active = true;
          cell.tentative_until_epoch = prediction_epoch;
          cell.expire( local_frame_sent + 1, now );
          cell.original_contents.push_back( *fb.get_cell( cursor().row, i ) );

          ConditionalOverlayCell& prev_cell = the_row.overlay_cells[i - 1];
          const Cell* prev_cell_actual = fb.get_cell( cursor().row, i - 1 );

          if ( i == fb.ds.get_width() - 1 ) {
            cell.unknown = true;
          } else if ( prev_cell.active ) {
            if ( prev_cell.unknown ) {
              cell.unknown = true;
            } else {
              cell.unknown = false;
              cell.replacement = prev_cell.replacement;
            }
          } else {
            cell.unknown = false;
            cell.replacement = *prev_cell_actual;
          }
        }

        ConditionalOverlayCell& cell = the_row.overlay_cells[cursor().col];
        cell.reset_with_orig();
        cell.active = true;
        cell.tentative_until_epoch = prediction_epoch;
        cell.expire( local_frame_sent + 1, now );
        cell.replacement.get_renditions() = fb.ds.get_renditions();

        /* heuristic: match renditions of character to the left */
        if ( cursor().col > 0 ) {
          ConditionalOverlayCell& prev_cell = the_row.overlay_cells[cursor().col - 1];
          const Cell* prev_cell_actual = fb.get_cell( cursor().row, cursor().col - 1 );
          if ( prev_cell.active && ( !prev_cell.unknown ) ) {
            cell.replacement.get_renditions() = prev_cell.replacement.get_renditions();
          } else {
            cell.replacement.get_renditions() = prev_cell_actual->get_renditions();
          }
        }

        cell.replacement.clear();
        cell.replacement.append( ch );
        cell.original_contents.push_back( *fb.get_cell( cursor().row, cursor().col ) );

        /*
        fprintf( stderr, "[%d=>%d] Predicting %lc in row %d, col %d [tue: %lu]\n",
                 (int)local_frame_acked, (int)cell.expiration_frame,
                 ch, cursor().row, cursor().col,
                 cell.tentative_until_epoch );
        */

        cursor().expire( local_frame_sent + 1, now );

        /* do we need to wrap? */
        if ( cursor().col < fb.ds.get_width() - 1 ) {
          cursor().col++;
        } else {
          become_tentative();
          newline_carriage_return( fb );
        }
      }
    } else if ( type_act == typeid( Parser::Execute ) ) {
      if ( act.char_present && ( act.ch == 0x0d ) /* CR */ ) {
        become_tentative();
        newline_carriage_return( fb );
      } else {
        //	fprintf( stderr, "Execute 0x%x\n", act.ch );
        become_tentative();
      }
    } else if ( type_act == typeid( Parser::Esc_Dispatch ) ) {
      //      fprintf( stderr, "Escape sequence\n" );
      become_tentative();
    } else if ( type_act == typeid( Parser::CSI_Dispatch ) ) {
      if ( act.char_present && ( act.ch == L'C' ) ) { /* right arrow */
        init_cursor( fb );
        if ( cursor().col < fb.ds.get_width() - 1 ) {
          cursor().col++;
          cursor().expire( local_frame_sent + 1, now );
        }
      } else if ( act.char_present && ( act.ch == L'D' ) ) { /* left arrow */
        init_cursor( fb );

        if ( cursor().col > 0 ) {
          cursor().col--;
          cursor().expire( local_frame_sent + 1, now );
        }
      } else {
        //	fprintf( stderr, "CSI sequence %lc\n", act.ch );
        become_tentative();
      }
    }
  }
}

void PredictionEngine::newline_carriage_return( const Framebuffer& fb )
{
  uint64_t now = timestamp();
  init_cursor( fb );
  cursor().col = 0;
  if ( cursor().row == fb.ds.get_height() - 1 ) {
    /* Don't try to predict scroll until we have versioned cell predictions */
    /*
    for ( overlays_type::iterator i = overlays.begin();
          i != overlays.end();
          i++ ) {
      i->row_num--;
      for ( overlay_cells_type::iterator j = i->overlay_cells.begin();
            j != i->overlay_cells.end();
            j++ ) {
        if ( j->active ) {
          j->expire( local_frame_sent + 1, now );
        }
      }
    }
    */

    /* make blank prediction for last row */
    ConditionalOverlayRow& the_row = get_or_make_row( cursor().row, fb.ds.get_width() );
    for ( overlay_cells_type::iterator j = the_row.overlay_cells.begin(); j != the_row.overlay_cells.end(); j++ ) {
      j->active = true;
      j->tentative_until_epoch = prediction_epoch;
      j->expire( local_frame_sent + 1, now );
      j->replacement.clear();
    }
  } else {
    cursor().row++;
  }
}

void PredictionEngine::become_tentative( void )
{
  if ( display_preference != Experimental ) {
    prediction_epoch++;
  }

  /*
  fprintf( stderr, "Now tentative in epoch %lu (confirmed=%lu)\n",
           prediction_epoch, confirmed_epoch );
  */
}

bool PredictionEngine::active( void ) const
{
  if ( !cursors.empty() ) {
    return true;
  }

  for ( overlays_type::const_iterator i = overlays.begin(); i != overlays.end(); i++ ) {
    for ( overlay_cells_type::const_iterator j = i->overlay_cells.begin(); j != i->overlay_cells.end(); j++ ) {
      if ( j->active ) {
        return true;
      }
    }
  }

  return false;
}
