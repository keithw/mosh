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

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <langinfo.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <pwd.h>
#include <typeinfo>
#include <signal.h>
#ifdef HAVE_UTEMPTER
#include <utempter.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef HAVE_NSGETENVIRON
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#endif

#include "selfpipe.h"

#include "completeterminal.h"
#include "swrite.h"
#include "user.h"

#if HAVE_PTY_H
#include <pty.h>
#elif HAVE_UTIL_H
#include <util.h>
#endif

#include "networktransport.cc"

typedef Network::Transport< Terminal::Complete, Network::UserStream > ServerConnection;

void serve( int host_fd,
	    Terminal::Complete &terminal,
	    ServerConnection &network );

using namespace std;

int main( int argc, char *argv[] )
{
  char *desired_ip = NULL;
  if ( argc == 1 ) {
    desired_ip = NULL;
  } else if ( argc == 2 ) {
    desired_ip = argv[ 1 ];
  } else {
    fprintf( stderr, "Usage: %s [LOCALADDR]\n", argv[ 0 ] );
    exit( 1 );
  }

  /* Adopt implementation locale */
  if ( NULL == setlocale( LC_ALL, "" ) ) {
    perror( "setlocale" );
    exit( 1 );
  }

  /* Verify locale calls for UTF-8 */
  if ( strcmp( nl_langinfo( CODESET ), "UTF-8" ) != 0 ) {
    fprintf( stderr, "mosh requires a UTF-8 locale.\n" );
    exit( 1 );
  }

  /* get initial window size */
  struct winsize window_size;
  if ( ioctl( STDIN_FILENO, TIOCGWINSZ, &window_size ) < 0 ) {
    perror( "ioctl TIOCGWINSZ" );
    exit( 1 );
  }

  /* open parser and terminal */
  Terminal::Complete terminal( window_size.ws_col, window_size.ws_row );

  /* open network */
  Network::UserStream blank;
  ServerConnection network( terminal, blank, desired_ip );

  /* network.set_verbose(); */

  printf( "\nMOSH CONNECT %d %s\n", network.port(), network.get_key().c_str() );
  fflush( stdout );

  /* don't let signals kill us */
  sigset_t signals_to_block;

  assert( sigemptyset( &signals_to_block ) == 0 );
  assert( sigaddset( &signals_to_block, SIGTERM ) == 0 );
  assert( sigaddset( &signals_to_block, SIGINT ) == 0 );
  assert( sigaddset( &signals_to_block, SIGHUP ) == 0 );
  assert( sigaddset( &signals_to_block, SIGPIPE ) == 0 );
  assert( sigprocmask( SIG_BLOCK, &signals_to_block, NULL ) == 0 );

  struct termios child_termios;

  /* Get terminal configuration */
  if ( tcgetattr( STDIN_FILENO, &child_termios ) < 0 ) {
    perror( "tcgetattr" );
    exit( 1 );
  }

  /* detach from terminal */
  pid_t the_pid = fork();
  if ( the_pid < 0 ) {
    perror( "fork" );
  } else if ( the_pid > 0 ) {
    _exit( 0 );
  }

  fprintf( stderr, "[mosh-server detached, pid=%d.]\n", (int)getpid() );

  int master;

  if ( !(child_termios.c_iflag & IUTF8) ) {
    /* SSH should also convey IUTF8 across connection. */
    //    fprintf( stderr, "Warning: Locale is UTF-8 but termios IUTF8 flag not set. Setting IUTF8 flag.\n" );
    child_termios.c_iflag |= IUTF8;
  }

  /* Fork child process */
  pid_t child = forkpty( &master, NULL, &child_termios, &window_size );

  if ( child == -1 ) {
    perror( "forkpty" );
    exit( 1 );
  }

  if ( child == 0 ) {
    /* child */

    /* unblock signals */
    sigset_t signals_to_block;
    assert( sigemptyset( &signals_to_block ) == 0 );
    assert( sigprocmask( SIG_SETMASK, &signals_to_block, NULL ) == 0 );

    /* set TERM */
    if ( setenv( "TERM", "xterm", true ) < 0 ) {
      perror( "setenv" );
      exit( 1 );
    }

    /* ask ncurses to send UTF-8 instead of ISO 2022 for line-drawing chars */
    if ( setenv( "NCURSES_NO_UTF8_ACS", "1", true ) < 0 ) {
      perror( "setenv" );
      exit( 1 );
    }

    /* clear STY environment variable so GNU screen regards us as top level */
    if ( unsetenv( "STY" ) < 0 ) {
      perror( "unsetenv" );
      exit( 1 );
    }

    /* get shell name */
    struct passwd *pw = getpwuid( geteuid() );
    if ( pw == NULL ) {
      perror( "getpwuid" );
      exit( 1 );
    }

    char *my_argv[ 2 ];
    my_argv[ 0 ] = strdup( pw->pw_shell );
    assert( my_argv[ 0 ] );
    my_argv[ 1 ] = NULL;
    
    if ( execve( pw->pw_shell, my_argv, environ ) < 0 ) {
      perror( "execve" );
      exit( 1 );
    }
    exit( 0 );
  } else {
    /* parent */

    #ifdef HAVE_UTEMPTER
    /* make utmp entry */
    char tmp[ 64 ];
    snprintf( tmp, 64, "mosh [%d]", getpid() );
    utempter_add_record( master, tmp );
    #endif

    try {
      serve( master, terminal, network );
    } catch ( Network::NetworkException e ) {
      fprintf( stderr, "Network exception: %s: %s\n",
	       e.function.c_str(), strerror( e.the_errno ) );
    } catch ( Crypto::CryptoException e ) {
      fprintf( stderr, "Crypto exception: %s\n",
	       e.text.c_str() );
    }

    if ( close( master ) < 0 ) {
      perror( "close" );
      exit( 1 );
    }

    #ifdef HAVE_UTEMPTER
    utempter_remove_added_record();
    #endif
  }

  printf( "\n[mosh-server is exiting.]\n" );

  return 0;
}

void serve( int host_fd, Terminal::Complete &terminal, ServerConnection &network )
{
  /* establish fd for shutdown signals */
  int signal_fd = selfpipe_init();
  if ( signal_fd < 0 ) {
    perror( "selfpipe" );
    return;
  }

  assert( selfpipe_trap( SIGTERM ) == 0 );
  assert( selfpipe_trap( SIGINT ) == 0 );

  /* prepare to poll for events */
  struct pollfd pollfds[ 3 ];

  pollfds[ 0 ].fd = network.fd();
  pollfds[ 0 ].events = POLLIN;

  pollfds[ 1 ].fd = host_fd;
  pollfds[ 1 ].events = POLLIN;

  pollfds[ 2 ].fd = signal_fd;
  pollfds[ 2 ].events = POLLIN;

  uint64_t last_remote_num = network.get_remote_state_num();

  #ifdef HAVE_UTEMPTER
  bool connected_utmp = false;
  #endif

  struct in_addr saved_addr;
  saved_addr.s_addr = 0;

  while ( 1 ) {
    try {
      uint64_t now = Network::timestamp();

      int active_fds = poll( pollfds, 3, min( network.wait_time(), terminal.wait_time( now ) ) );
      if ( active_fds < 0 && errno == EINTR ) {
	continue;
      } else if ( active_fds < 0 ) {
	perror( "poll" );
	break;
      }

      now = Network::timestamp();

      if ( pollfds[ 0 ].revents & POLLIN ) {
	/* packet received from the network */
	network.recv();
	
	/* is new user input available for the terminal? */
	if ( network.get_remote_state_num() != last_remote_num ) {
	  last_remote_num = network.get_remote_state_num();

	  string terminal_to_host;
	  
	  Network::UserStream us;
	  us.apply_string( network.get_remote_diff() );
	  /* apply userstream to terminal */
	  for ( size_t i = 0; i < us.size(); i++ ) {
	    terminal_to_host += terminal.act( us.get_action( i ) );
	    if ( typeid( *us.get_action( i ) ) == typeid( Parser::Resize ) ) {
	      /* tell child process of resize */
	      const Parser::Resize *res = static_cast<const Parser::Resize *>( us.get_action( i ) );
	      struct winsize window_size;
	      window_size.ws_col = res->width;
	      window_size.ws_row = res->height;
	      if ( ioctl( host_fd, TIOCSWINSZ, &window_size ) < 0 ) {
		perror( "ioctl TIOCSWINSZ" );
		return;
	      }
	    }
	  }

	  if ( !us.empty() ) {
	    /* register input frame number for future echo ack */
	    terminal.register_input_frame( last_remote_num, now );
	  }

	  /* update client with new state of terminal */
	  if ( !network.shutdown_in_progress() ) {
	    network.set_current_state( terminal );
	  }
	  
	  /* write any writeback octets back to the host */
	  if ( swrite( host_fd, terminal_to_host.c_str(), terminal_to_host.length() ) < 0 ) {
	    break;
	  }

	  #ifdef HAVE_UTEMPTER
	  /* update utmp entry if we have become "connected" */
	  if ( (!connected_utmp)
	       || ( saved_addr.s_addr != network.get_remote_ip().s_addr ) ) {
	    utempter_remove_added_record();

	    saved_addr = network.get_remote_ip();

	    char tmp[ 64 ];
	    snprintf( tmp, 64, "%s via mosh [%d]", inet_ntoa( saved_addr ), getpid() );
	    utempter_add_record( host_fd, tmp );

	    connected_utmp = true;
	  }
	  #endif
	}
      }
      
      if ( pollfds[ 1 ].revents & POLLIN ) {
	/* input from the host needs to be fed to the terminal */
	const int buf_size = 16384;
	char buf[ buf_size ];
	
	/* fill buffer if possible */
	ssize_t bytes_read = read( pollfds[ 1 ].fd, buf, buf_size );
	if ( bytes_read == 0 ) { /* EOF */
	  return;
	} else if ( bytes_read < 0 ) {
	  perror( "read" );
	  return;
	}
	
	string terminal_to_host = terminal.act( string( buf, bytes_read ) );
	
	/* update client with new state of terminal */
	if ( !network.shutdown_in_progress() ) {
	  network.set_current_state( terminal );
	}

	/* write any writeback octets back to the host */
	if ( swrite( host_fd, terminal_to_host.c_str(), terminal_to_host.length() ) < 0 ) {
	  break;
	}
      }

      if ( pollfds[ 2 ].revents & POLLIN ) {
	/* shutdown signal */
	int signo = selfpipe_read();
	if ( signo == 0 ) {
	  break;
	} else if ( signo < 0 ) {
	  perror( "selfpipe_read" );
	  break;
	}

	if ( network.attached() && (!network.shutdown_in_progress()) ) {
	  network.start_shutdown();
	} else {
	  break;
	}
      }
      
      if ( (pollfds[ 0 ].revents)
	   & (POLLERR | POLLHUP | POLLNVAL) ) {
	/* network problem */
	break;
      }

      if ( (pollfds[ 1 ].revents)
	   & (POLLERR | POLLHUP | POLLNVAL) ) {
	/* host problem */
	if ( network.attached() ) {
	  network.start_shutdown();
	} else {
	  break;
	}
      }

      /* quit if our shutdown has been acknowledged */
      if ( network.shutdown_in_progress() && network.shutdown_acknowledged() ) {
	break;
      }

      /* quit after shutdown acknowledgement timeout */
      if ( network.shutdown_in_progress() && network.shutdown_ack_timed_out() ) {
	break;
      }

      /* quit if we received and acknowledged a shutdown request */
      if ( network.counterparty_shutdown_ack_sent() ) {
	break;
      }

      #ifdef HAVE_UTEMPTER
      /* update utmp if has been more than 10 seconds since heard from client */
      if ( connected_utmp ) {
	if ( network.get_latest_remote_state().timestamp < now - 10000 ) {
	  utempter_remove_added_record();

	  char tmp[ 64 ];
	  snprintf( tmp, 64, "mosh [%d]", getpid() );
	  utempter_add_record( host_fd, tmp );

	  connected_utmp = false;
	}
      }
      #endif

      if ( terminal.set_echo_ack( now ) ) {
	/* update client with new echo ack */
	if ( !network.shutdown_in_progress() ) {
	  network.set_current_state( terminal );
	}
      }

      network.tick();
    } catch ( Network::NetworkException e ) {
      fprintf( stderr, "%s: %s\n", e.function.c_str(), strerror( e.the_errno ) );
      sleep( 1 );
    } catch ( Crypto::CryptoException e ) {
      fprintf( stderr, "Crypto exception: %s\n", e.text.c_str() );
    }
  }
}
