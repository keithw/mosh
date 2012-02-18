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
*/

#include <boost/lambda/lambda.hpp>
#include <boost/typeof/typeof.hpp>
#include <algorithm>
#include <list>
#include <stdio.h>

#include "transportsender.h"
#include "transportfragment.h"

using namespace boost::lambda;
using namespace Network;
using namespace std;

template <class MyState>
TransportSender<MyState>::TransportSender( Connection *s_connection, MyState &initial_state )
  : connection( s_connection ), 
    current_state( initial_state ),
    sent_states( 1, TimestampedState<MyState>( timestamp(), 0, initial_state ) ),
    assumed_receiver_state( sent_states.begin() ),
    fragmenter(),
    next_ack_time( timestamp() ),
    next_send_time( timestamp() ),
    verbose( false ),
    shutdown_in_progress( false ),
    shutdown_tries( 0 ),
    ack_num( 0 ),
    pending_data_ack( false ),
    ack_timestamp( 0 ),
    ack_history(),
    SEND_MINDELAY( 15 )
{
}

/* Try to send roughly two frames per RTT, bounded by limits on frame rate */
template <class MyState>
unsigned int TransportSender<MyState>::send_interval( void ) const
{
  int SEND_INTERVAL = lrint( ceil( connection->get_SRTT() / 2.0 ) );
  if ( SEND_INTERVAL < SEND_INTERVAL_MIN ) {
    SEND_INTERVAL = SEND_INTERVAL_MIN;
  } else if ( SEND_INTERVAL > SEND_INTERVAL_MAX ) {
    SEND_INTERVAL = SEND_INTERVAL_MAX;
  }

  return SEND_INTERVAL;
}

/* How many ms can the caller wait before we will have an event (empty ack or next frame)? */
template <class MyState>
int TransportSender<MyState>::wait_time( void )
{
  if ( pending_data_ack && (next_ack_time > timestamp() + ACK_DELAY) ) {
    next_ack_time = timestamp() + ACK_DELAY;
  }

  if ( !(current_state == sent_states.back().state) ) { /* pending data to send */
    if ( next_send_time > timestamp() + SEND_MINDELAY ) {
      next_send_time = timestamp() + SEND_MINDELAY;
    }

    if ( next_send_time < sent_states.back().timestamp + send_interval() ) {
      next_send_time = sent_states.back().timestamp + send_interval();
    }
  }

  /* speed up shutdown sequence */
  if ( shutdown_in_progress || (ack_num == uint64_t(-1)) ) {
    next_ack_time = sent_states.back().timestamp + send_interval();
  }

  uint64_t next_wakeup = next_ack_time;

  if ( next_send_time < next_wakeup ) {
    next_wakeup = next_send_time;
  }

  if ( !connection->get_attached() ) {
    return -1;
  }

  if ( next_wakeup > timestamp() ) {
    return next_wakeup - timestamp();
  } else {
    return 0;
  }
}

/* Send data or an empty ack if necessary */
template <class MyState>
void TransportSender<MyState>::tick( void )
{
  wait_time();

  if ( !connection->get_attached() ) {
    return;
  }

  if ( (timestamp() < next_ack_time)
       && (timestamp() < next_send_time) ) {
    return;
  }

  /* Determine if a new diff or empty ack needs to be sent */
  /* Update assumed receiver state */
  update_assumed_receiver_state();
    
  /* Cut out common prefix of all states */
  rationalize_states();

  string diff = current_state.diff_from( assumed_receiver_state->state );

  if ( diff.empty() && (timestamp() >= next_ack_time) ) {
    send_empty_ack();
    return;
  }

  if ( !diff.empty() && ( (timestamp() >= next_send_time)
			  || (timestamp() >= next_ack_time) ) ) {
    /* Send diffs or ack */
    send_to_receiver( diff );
  }
}

template <class MyState>
void TransportSender<MyState>::send_empty_ack( void )
{
  assert ( timestamp() >= next_ack_time );

  uint64_t new_num = sent_states.back().num + 1;

  /* special case for shutdown sequence */
  if ( shutdown_in_progress ) {
    new_num = uint64_t( -1 );
  }

  //  sent_states.push_back( TimestampedState<MyState>( sent_states.back().timestamp, new_num, current_state ) );
  add_sent_state( sent_states.back().timestamp, new_num, current_state );
  send_in_fragments( "", new_num );

  next_ack_time = timestamp() + ACK_INTERVAL;
}

template <class MyState>
void TransportSender<MyState>::add_sent_state( uint64_t the_timestamp, uint64_t num, MyState &state )
{
  sent_states.push_back( TimestampedState<MyState>( the_timestamp, num, state ) );
  if ( sent_states.size() > 32 ) { /* limit on state queue */
    BOOST_AUTO( last, sent_states.end() );
    for ( int i = 0; i < 16; i++ ) { last--; }
    sent_states.erase( last ); /* erase state from middle of queue */
  }
}

template <class MyState>
void TransportSender<MyState>::send_to_receiver( string diff )
{
  uint64_t new_num;
  if ( current_state == sent_states.back().state ) { /* previously sent */
    new_num = sent_states.back().num;
  } else { /* new state */
    new_num = sent_states.back().num + 1;
  }

  /* special case for shutdown sequence */
  if ( shutdown_in_progress ) {
    new_num = uint64_t( -1 );
  }

  if ( new_num == sent_states.back().num ) {
    sent_states.back().timestamp = timestamp();
  } else {
    add_sent_state( timestamp(), new_num, current_state );
  }

  send_in_fragments( diff, new_num ); // Can throw NetworkException

  /* successfully sent, probably */
  /* ("probably" because the FIRST size-exceeded datagram doesn't get an error) */
  assumed_receiver_state = sent_states.end();
  assumed_receiver_state--;
  next_ack_time = timestamp() + ACK_INTERVAL;
  next_send_time = uint64_t(-1);
}

template <class MyState>
void TransportSender<MyState>::update_assumed_receiver_state( void )
{
  uint64_t now = timestamp();

  /* start from what is known and give benefit of the doubt to unacknowledged states
     transmitted recently enough ago */
  assumed_receiver_state = sent_states.begin();

  typename list< TimestampedState<MyState> >::iterator i = sent_states.begin();
  i++;

  while ( i != sent_states.end() ) {
    assert( now >= i->timestamp );

    if ( uint64_t(now - i->timestamp) < connection->timeout() + ACK_DELAY ) {
      assumed_receiver_state = i;
    } else {
      return;
    }

    i++;
  }
}

template <class MyState>
void TransportSender<MyState>::rationalize_states( void )
{
  const MyState * known_receiver_state = &sent_states.front().state;

  current_state.subtract( known_receiver_state );

  for ( typename list< TimestampedState<MyState> >::reverse_iterator i = sent_states.rbegin();
	i != sent_states.rend();
	i++ ) {
    i->state.subtract( known_receiver_state );
  }
}

template <class MyState>
void TransportSender<MyState>::send_in_fragments( string diff, uint64_t new_num )
{
  Instruction inst;

  uint64_t now = timestamp();

  inst.set_protocol_version( MOSH_PROTOCOL_VERSION );
  inst.set_old_num( assumed_receiver_state->num );
  inst.set_new_num( new_num );
  inst.set_ack_num( ack_num );
  inst.set_throwaway_num( sent_states.front().num );
  inst.set_late_ack_num( get_late_ack( now ) );
  inst.set_diff( diff );

  if ( new_num == uint64_t(-1) ) {
    shutdown_tries++;
  }

  vector<Fragment> fragments = fragmenter.make_fragments( inst, connection->get_MTU() );

  for ( BOOST_AUTO( i, fragments.begin() ); i != fragments.end(); i++ ) {
    connection->send( i->tostring() );

    if ( verbose ) {
      fprintf( stderr, "[%u] Sent [%d=>%d] id %d, frag %d ack=%d, late_ack=%d, throwaway=%d, len=%d, frame rate=%.2f, timeout=%d, srtt=%.1f age=%llu\n",
	       (unsigned int)(timestamp() % 100000), (int)inst.old_num(), (int)inst.new_num(), (int)i->id, (int)i->fragment_num,
	       (int)inst.ack_num(), (int)inst.late_ack_num(), (int)inst.throwaway_num(), (int)i->contents.size(),
	       1000.0 / (double)send_interval(),
	       (int)connection->timeout(), connection->get_SRTT(),
	       (long long)(now - ack_timestamp) );
    }

  }

  pending_data_ack = false;
}

template <class MyState>
void TransportSender<MyState>::process_acknowledgment_through( uint64_t ack_num )
{
  /* Ignore ack if we have culled the state it's acknowledging */

  if ( sent_states.end() != find_if( sent_states.begin(), sent_states.end(),
				     (&_1)->*&TimestampedState<MyState>::num == ack_num ) ) {
    sent_states.remove_if( (&_1)->*&TimestampedState<MyState>::num < ack_num );
  }

  assert( !sent_states.empty() );
}

/* give up on getting acknowledgement for shutdown */
template <class MyState>
bool TransportSender<MyState>::shutdown_ack_timed_out( void ) const
{
  return shutdown_tries >= SHUTDOWN_RETRIES;
}

/* Executed upon entry to new receiver state */
template <class MyState>
void TransportSender<MyState>::set_ack_num( uint64_t s_ack_num )
{
  ack_num = s_ack_num;
  ack_timestamp = timestamp();
  ack_history.push_back( make_pair( ack_num, ack_timestamp ) );
}

/* The "late" ack is for the input state that has had enough time on the host to have been echoed */
template <class MyState>
uint64_t TransportSender<MyState>::get_late_ack( uint64_t now )
{
  uint64_t newest_echo_ack = 0;

  for ( BOOST_AUTO( i, ack_history.begin() ); i != ack_history.end(); i++ ) {
    if ( i->second < now - ECHO_TIMEOUT ) {
      newest_echo_ack = i->first;
    }
  }

  ack_history.remove_if( (&_1)->*&pair<uint64_t, uint64_t>::first < newest_echo_ack );

  return newest_echo_ack;
}
