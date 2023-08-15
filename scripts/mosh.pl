#!/usr/bin/env perl

#   Mosh: the mobile shell
#   Copyright 2012 Keith Winstein
#
#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
#   In addition, as a special exception, the copyright holders give
#   permission to link the code of portions of this program with the
#   OpenSSL library under certain conditions as described in each
#   individual source file, and distribute linked combinations including
#   the two.
#
#   You must obey the GNU General Public License in all respects for all
#   of the code used other than OpenSSL. If you modify file(s) with this
#   exception, you may extend this exception to your version of the
#   file(s), but you are not obligated to do so. If you do not wish to do
#   so, delete this exception statement from your version. If you delete
#   this exception statement from all source files in the program, then
#   also delete it here.

use 5.8.8;

use warnings;
use strict;
use Getopt::Long;
use IO::Socket;
use Text::ParseWords;
use Socket qw(IPPROTO_TCP);
use Errno qw(EINTR);
use POSIX qw(_exit);

BEGIN {
  my @gai_reqs = qw( getaddrinfo getnameinfo AI_CANONNAME AI_NUMERICHOST NI_NUMERICHOST );
  eval { Socket->import( @gai_reqs ); 1; }
    || (eval { require Socket::GetAddrInfo; 1; }
        && (eval { Socket::GetAddrInfo->import( ':newapi', @gai_reqs ); 1; }
            || eval { Socket::GetAddrInfo->import( '0.22', @gai_reqs ); 1; }))
    || die "$0 error: requires Perl 5.14 or Socket::GetAddrInfo.\n";
}

my $have_ipv6 = eval {
  require IO::Socket::IP;
  IO::Socket::IP->import('-register');
  1;
} || eval {
  require IO::Socket::INET6;
  1;
};

$|=1;

my $client = 'mosh-client';
my $server = 'mosh-server';

my $predict = undef;

my $overwrite = 0;

my $bind_ip = undef;

my $use_remote_ip = 'proxy';

my $family = 'prefer-inet';
my $port_request = undef;

my @ssh = ('ssh');

my $term_init = 1;

my $localhost = undef;

my $ssh_pty = 1;

my $help = undef;
my $version = undef;

my @cmdline = @ARGV;

my $usage =
qq{Usage: $0 [options] [--] [user@]host [command...]
        --client=PATH        mosh client on local machine
                                (default: "mosh-client")
        --server=COMMAND     mosh server on remote machine
                                (default: "mosh-server")

        --predict=adaptive      local echo for slower links [default]
-a      --predict=always        use local echo even on fast links
-n      --predict=never         never use local echo
        --predict=experimental  aggressively echo even when incorrect

-o      --predict-overwrite     prediction overwrites instead of inserting

-4      --family=inet        use IPv4 only
-6      --family=inet6       use IPv6 only
        --family=auto        autodetect network type for single-family hosts only
        --family=all         try all network types
        --family=prefer-inet use all network types, but try IPv4 first [default]
        --family=prefer-inet6 use all network types, but try IPv6 first
-p PORT[:PORT2]
        --port=PORT[:PORT2]  server-side UDP port or range
                                (No effect on server-side SSH port)
        --bind-server={ssh|any|IP}  ask the server to reply from an IP address
                                       (default: "ssh")

        --ssh=COMMAND        ssh command to run when setting up session
                                (example: "ssh -p 2222")
                                (default: "ssh")

        --no-ssh-pty         do not allocate a pseudo tty on ssh connection

        --no-init            do not send terminal initialization string

        --local              run mosh-server locally without using ssh

        --experimental-remote-ip=(local|remote|proxy)  select the method for
                             discovering the remote IP address to use for mosh
                             (default: "proxy")

        --help               this message
        --version            version and copyright information

Please report bugs to mosh-devel\@mit.edu.
Mosh home page: https://mosh.org\n};

my $version_message = '@PACKAGE_STRING@ [build @VERSION@]' . qq{
Copyright 2012 Keith Winstein <mosh-devel\@mit.edu>
License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.
This is free software: you are free to change and redistribute it.
There is NO WARRANTY, to the extent permitted by law.\n};

sub predict_check {
  my ( $predict, $env_set ) = @_;

  if ( not exists { adaptive => 0, always => 0,
		    never => 0, experimental => 0 }->{ $predict } ) {
    my $explanation = $env_set ? " (MOSH_PREDICTION_DISPLAY in environment)" : "";
    print STDERR qq{$0: Unknown mode \"$predict\"$explanation.\n\n};

    die $usage;
  }
}

GetOptions( 'client=s' => \$client,
	    'server=s' => \$server,
	    'predict=s' => \$predict,
	    'predict-overwrite|o!' => \$overwrite,
	    'port=s' => \$port_request,
	    'a' => sub { $predict = 'always' },
	    'n' => sub { $predict = 'never' },
	    'family=s' => \$family,
	    '4' => sub { $family = 'inet' },
	    '6' => sub { $family = 'inet6' },
	    'p=s' => \$port_request,
	    'ssh=s' => sub { @ssh = shellwords($_[1]); },
	    'ssh-pty!' => \$ssh_pty,
	    'init!' => \$term_init,
	    'local' => \$localhost,
	    'help' => \$help,
	    'version' => \$version,
	    'fake-proxy!' => \my $fake_proxy,
	    'bind-server=s' => \$bind_ip,
	    'experimental-remote-ip=s' => \$use_remote_ip) or die $usage;

if ( defined $help ) {
    print $usage;
    exit;
}
if ( defined $version ) {
    print $version_message;
    exit;
}

if ( defined $predict ) {
  predict_check( $predict, 0 );
} elsif ( defined $ENV{ 'MOSH_PREDICTION_DISPLAY' } ) {
  $predict = $ENV{ 'MOSH_PREDICTION_DISPLAY' };
  predict_check( $predict, 1 );
} else {
  $predict = 'adaptive';
  predict_check( $predict, 0 );
}

if ( not grep { $_ eq $use_remote_ip } qw { local remote proxy } ) {
  die "Unknown parameter $use_remote_ip";
}

$family = lc( $family );
# Handle IPv4-only Perl installs.
if (!$have_ipv6) {
  # Report failure if IPv6 needed and not available.
  if (defined($family) && $family eq "inet6") {
    die "$0: IPv6 sockets not available in this Perl install\n";
  }
  # Force IPv4.
  $family = "inet";
}
if ( $overwrite ) {
    $ENV{ "MOSH_PREDICTION_OVERWRITE" } = "yes";
}

if ( defined $port_request ) {
  if ( $port_request =~ m{^(\d+)(:(\d+))?$} ) {
    my ( $low, $clause, $high ) = ( $1, $2, $3 );
    # good port or port-range
    if ( $low < 0 or $low > 65535 ) {
      die "$0: Server-side (low) port ($low) must be within valid range [0..65535].\n";
    }
    if ( defined $high ) {
      if ( $high <= 0 or $high > 65535 ) {
	die "$0: Server-side high port ($high) must be within valid range [1..65535].\n";
      }
      if ( $low == 0 ) {
	die "$0: Server-side port ranges may not be used with starting port 0 ($port_request).\n";
      }
      if ( $low > $high ) {
	die "$0: Server-side port range ($port_request): low port greater than high port.\n";
      }
    }
  } else {
    die "$0: Server-side port or range ($port_request) is not valid.\n";
  }
}

delete $ENV{ 'MOSH_PREDICTION_DISPLAY' };

my $userhost;
my @command;
my @bind_arguments;

if ( ! defined $fake_proxy ) {
  if ( scalar @ARGV < 1 ) {
    die $usage;
  }
  $userhost = shift;
  @command = @ARGV;
  if ( not defined $bind_ip or $bind_ip =~ m{^ssh$}i ) {
    if ( not defined $localhost ) {
      push @bind_arguments, '-s';
    } else {
      push @bind_arguments, ('-i', "$userhost");
    }
  } elsif ( $bind_ip =~ m{^any$}i ) {
    # do nothing
  } else {
    push @bind_arguments, ('-i', "$bind_ip");
  }
} else {
  my ( $host, $port ) = @ARGV;

  my @res = resolvename( $host, $port, $family );

  # Now try and connect to something.
  my $err;
  my $sock;
  my $addr_string;
  my $service;
  for my $ai ( @res ) {
    ( $err, $addr_string, $service ) = getnameinfo( $ai->{addr}, NI_NUMERICHOST );
    next if $err;
    if ( $sock = IO::Socket->new( Domain => $ai->{family},
				  Family => $ai->{family},
				  PeerHost => $addr_string,
				  PeerPort => $port,
				  Proto => 'tcp' )) {
      print STDERR 'MOSH IP ', $addr_string, "\n";
      last;
    } else {
      $err = $@;
    }
  }
  die "$0: Could not connect to ${host}, last tried ${addr_string}: ${err}\n" if !$sock;

  # Act like netcat
  binmode($sock);
  binmode(STDIN);
  binmode(STDOUT);

  sub cat {
    my ( $from, $to ) = @_;
    while ( my $n = $from->sysread( my $buf, 4096 ) ) {
      next if ( $n == -1 && $! == EINTR );
      $n >= 0 or last;
      $to->write( $buf ) or last;
    }
  }

  defined( my $pid = fork ) or die "$0: fork: $!\n";
  if ( $pid == 0 ) {
    close STDIN;
    cat $sock, \*STDOUT; $sock->shutdown( 0 );
    _exit 0;
  }
  $SIG{ 'HUP' } = 'IGNORE';
  close STDOUT;
  cat \*STDIN, $sock; $sock->shutdown( 1 );
  close STDIN;
  waitpid $pid, 0;
  exit;
}

# Count colors
open COLORCOUNT, '-|', $client, ('-c') or die "Can't count colors: $!\n";
my $colors = "";
{
  local $/ = undef;
  $colors = <COLORCOUNT>;
}
close COLORCOUNT or die;

chomp $colors;

if ( (not defined $colors)
    or $colors !~ m{^[0-9]+$}
    or $colors < 0 ) {
  $colors = 0;
}

$ENV{ 'MOSH_CLIENT_PID' } = $$; # We don't support this, but it's useful for test and debug.

# If we are using a locally-resolved address, we have to get it before we fork,
# so both parent and child get it.
my $ip;
if ( $use_remote_ip eq 'local' ) {
  # "parse" the host from what the user gave us
  my ($user, $host) = $userhost =~ /^((?:.*@)?)(.*)$/;
  # get list of addresses
  my @res = resolvename( $host, 22, $family );
  # Use only the first address as the Mosh IP
  my $hostaddr = $res[0];
  if ( !defined $hostaddr ) {
    die( "could not find address for $host" );
  }
  my ( $err, $addr_string, $service ) = getnameinfo( $hostaddr->{addr}, NI_NUMERICHOST );
  if ( $err ) {
    die( "could not use address for $host" );
  }
  $ip = $addr_string;
  $userhost = "$user$ip";
}

# Construct server exec arguments.
my @sshopts = ( '-n' );
if ($ssh_pty) {
  push @sshopts, '-tt';
}

my $ssh_connection = "";
if ( $use_remote_ip eq 'remote' ) {
  # Ask the server for its IP.  The user's shell may not be
  # Posix-compatible so invoke sh explicitly.
  $ssh_connection = "sh -c " .
    shell_quote ( '[ -n "$SSH_CONNECTION" ] && printf "\nMOSH SSH_CONNECTION %s\n" "$SSH_CONNECTION"' ) .
    " ; ";
  # Only with 'remote', we may need to tell SSH which protocol to use.
  if ( $family eq 'inet' ) {
    push @sshopts, '-4';
  } elsif ( $family eq 'inet6' ) {
    push @sshopts, '-6';
  }
}
my @server = ( 'new' );

push @server, ( '-c', $colors );

push @server, @bind_arguments;

if ( defined $port_request ) {
  push @server, ( '-p', $port_request );
}

for ( &locale_vars ) {
  push @server, ( '-l', $_ );
}

if ( scalar @command > 0 ) {
  push @server, '--', @command;
}

if ( $use_remote_ip eq 'proxy' ) {
  my $quoted_proxy_command = shell_quote( $0, "--family=$family" );
  push @sshopts, ( '-S', 'none', '-o', "ProxyCommand=$quoted_proxy_command --fake-proxy -- %h %p" );
}
my @exec_argv = ( @ssh, @sshopts, $userhost, '--', $ssh_connection . "$server " . shell_quote( @server ) );

# Override command line for local execution.
if ( defined( $localhost )) {
  @exec_argv = ( "$server " . shell_quote( @server ) );
}

my $pid = open(my $pipe, "-|");
die "$0: fork: $!\n" unless ( defined $pid );
if ( $pid == 0 ) { # child
  open(STDERR, ">&STDOUT") or die;

  if ( !defined( $localhost) && $use_remote_ip eq 'proxy' ) {
    # Non-standard shells and broken shrc files cause the ssh
    # proxy to break mysteriously.
    $ENV{ 'SHELL' } = '/bin/sh';
  }

  if ( defined( $localhost )) {
    delete $ENV{ 'SSH_CONNECTION' };
    chdir; # $HOME
    print "MOSH IP ${userhost}\n";
  }

  exec @exec_argv;
  die "Cannot exec child: $!\n";
}
# parent
my ( $sship, $port, $key );
my $bad_udp_port_warning = 0;
LINE: while ( <$pipe> ) {
  chomp;
  if ( m{^MOSH IP } ) {
    if ( defined $ip ) {
      die "$0 error: detected attempt to redefine MOSH IP.\n";
    }
    ( $ip ) = m{^MOSH IP (\S+)\s*$} or die "Bad MOSH IP string: $_\n";
  } elsif ( m{^MOSH SSH_CONNECTION } ) {
    my @words = split;
    if ( scalar @words == 6 ) {
      $sship = $words[4];
    } else {
      die "Bad MOSH SSH_CONNECTION string: $_\n";
    }
  } elsif ( m{^MOSH CONNECT } ) {
    if ( ( $port, $key ) = m{^MOSH CONNECT (\d+?) ([A-Za-z0-9/+]{22})\s*$} ) {
      last LINE;
    } else {
      die "Bad MOSH CONNECT string: $_\n";
    }
  } else {
    if ( defined $port_request and $port_request =~ m{:} and m{Bad UDP port} ) {
      $bad_udp_port_warning = 1;
    }
    print "$_\n";
  }
}
if ( not close $pipe ) {
  if ( $! == 0 ) {
    die_on_exitstatus($?, "server command", shell_quote(@exec_argv));
  } else {
    die("$0: error closing server pipe: $!\n")
  }
}

if ( not defined $ip ) {
  if ( defined $sship ) {
    warn "$0: Using remote IP address ${sship} from \$SSH_CONNECTION for hostname ${userhost}\n";
    $ip = $sship;
  } else {
    die "$0: Did not find remote IP address (is SSH ProxyCommand disabled?).\n";
  }
}

if ( not defined $key or not defined $port ) {
  if ( $bad_udp_port_warning ) {
    die "$0: Server does not support UDP port range option.\n";
  }
  die "$0: Did not find mosh server startup message. (Have you installed mosh on your server?)\n";
}

# Now start real mosh client
$ENV{ 'MOSH_KEY' } = $key;
$ENV{ 'MOSH_PREDICTION_DISPLAY' } = $predict;
$ENV{ 'MOSH_NO_TERM_INIT' } = '1' if !$term_init;
exec {$client} ("$client", "-# @cmdline |", $ip, $port);

sub shell_quote { join ' ', map {(my $a = $_) =~ s/'/'\\''/g; "'$a'"} @_ }

sub locale_vars {
  my @names = qw[LANG LANGUAGE LC_CTYPE LC_NUMERIC LC_TIME LC_COLLATE LC_MONETARY LC_MESSAGES LC_PAPER LC_NAME LC_ADDRESS LC_TELEPHONE LC_MEASUREMENT LC_IDENTIFICATION LC_ALL];

  my @assignments;

  for ( @names ) {
    if ( defined $ENV{ $_ } ) {
      push @assignments, $_ . q{=} . $ENV{ $_ };
    }
  }

  return @assignments;
}

sub resolvename {
  my ( $host, $port, $family ) = @_;
  my $err;
  my @res;
  my $af;

  # If the user selected a specific family, parse it.
  if ( defined( $family ) && ( $family eq 'inet' || $family eq 'inet6' )) {
      # Choose an address family, or cause pain.
      my $afstr = 'AF_' . uc( $family );
      $af = eval { IO::Socket->$afstr } or die "$0: Invalid family $family\n";
  }

  # First try the address as a numeric.
  my %hints = ( flags => AI_NUMERICHOST,
		socktype => SOCK_STREAM,
		protocol => IPPROTO_TCP );
  if ( defined( $af )) {
    $hints{family} = $af;
  }
  ( $err, @res ) = getaddrinfo( $host, $port, \%hints );
  if ( $err ) {
    # Get canonical name for this host.
    $hints{flags} = AI_CANONNAME;
    ( $err, @res ) = getaddrinfo( $host, $port, \%hints );
    die "$0: could not get canonical name for $host: ${err}\n" if $err;
    # Then get final resolution of the canonical name.
    delete $hints{flags};
    my @newres;
    ( $err, @newres ) = getaddrinfo( $res[0]{canonname}, $port, \%hints );
    die "$0: could not resolve canonical name ${res[0]{canonname}} for ${host}: ${err}\n" if $err;
    @res = @newres;
  }

  if ( defined( $af )) {
    # If v4 or v6 was specified, reduce the host list.
    @res = grep {$_->{family} == $af} @res;
  } elsif ( $family =~ /^prefer-/ ) {
    # If prefer-* was specified, reorder the host list to put that family first.
    my $prefer_afstr = 'AF_' . uc( ($family =~ /prefer-(.*)/)[0] );
    my $prefer_af = eval { IO::Socket->$prefer_afstr } or die "$0: Invalid preferred family $family\n";
    @res = (grep({$_->{family} == $prefer_af} @res), grep({$_->{family} != $prefer_af} @res));
  } elsif ( $family ne 'all' ) {
    # If v4/v6/all were not specified, verify that this host only has one address family available.
    for my $ai ( @res ) {
      if ( !defined( $af )) {
	$af = $ai->{family};
      } else {
	die "$0: host has both IPv4 and IPv6 addresses, use --family to specify behavior\n"
	    if $af != $ai->{family};
      }
    }
  }
  return @res;
}

sub die_on_exitstatus {
  my ($exitstatus, $what_command, $exec_string) = @_;

  if (POSIX::WIFSIGNALED($exitstatus)) {
    my $termsig = POSIX::WTERMSIG($exitstatus);
    die("$0: $what_command exited on signal $termsig: $exec_string\n" );
  }
  if (!POSIX::WIFEXITED($exitstatus)) {
    die("$0: $what_command unexpectedly terminated with exit status $exitstatus: $exec_string\n");
  }
  my $exitcode = POSIX::WEXITSTATUS($exitstatus);
  if ($exitcode != 0) {
    die("$0: $what_command failed with exitstatus $exitcode: $exec_string\n");
  }
  return;
}
