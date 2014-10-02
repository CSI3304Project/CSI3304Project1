# -*- cperl -*-
# Copyright (c) 2011, 2013, Oracle and/or its affiliates. All rights reserved. 
# 
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA


########## Memcache Client Library for Perl
### 
###  $mc = My::Memcache->new()          create an ascii-protocol client 
###  $mc = My::Memcache::Binary->new()  create a binary-protocol client
###
###  $mc->connect(host, port)           returns 1 on success, 0 on failure
### 
###  $mc->{error}                       holds most recent error/status message
### 
###  $mc->store(cmd, key, value, ...)   alternate API for set/add/replace/append/prepend
###  $mc->set(key, value)               returns 1 on success, 0 on failure
###  $mc->add(key, value)               set if record does not exist
###  $mc->replace(key, value)           set if record exists
###  $mc->append(key, value)            append value to existing data
###  $mc->prepend(key, value)           prepend value to existing data
###
###  $mc->get(key, [ key ...])          returns a value or undef 
###  $mc->next_result()                 Fetch results after get()
### 
###  $mc->delete(key)                   returns 1 on success, 0 on failure
###  $mc->stats(stat_key)               get stats; returns a hash
###  $mc->incr(key, amount, [initial])  returns the new value or undef
###  $mc->decr(key, amount, [initial])  like incr.  
###                                           The third argument is used in
###                                           the binary protocol ONLY. 
###  $mc->flush()                       flush_all
###
###  $mc->set_expires(sec)              Set TTL for all store operations
###  $mc->set_flags(int_flags)          Set numeric flags for store operations
###
###  $mc->note_config_version() 
###    Store the generation number of the running config in the filesystem,
###    for later use by wait_for_reconf()
### 
###  $mc->wait_for_reconf()
###    Wait for NDB/Memcache to complete online reconfiguration.  
###    Returns the generation number of the newly running configuration, 
###    or zero on timeout/error. 

################ TODO ################################
###  * Support explicit binary k/q commands with pipelining
###  * Implement TOUCH & GAT commands
###  * Support UDP
###  * Standardize APIs to take (key, value, hashref-of-options)

use strict;
use IO::Socket::INET;
use IO::File;
use Carp;
use Time::HiRes;

######## Memcache Result

package My::Memcache::Result;

sub new {
  my ($pkg, $key, $flags, $cas) = @_;
  $cas = 0 if(! defined($cas));
  bless {
    "key" => $key,
    "flags" => $flags,
    "cas" => $cas,
    "value" => undef,
  }, $pkg;
}


######## Memcache Client

package My::Memcache;

sub new {
  my $pkg = shift;
  # min/max wait refer to msec. wait during temporary errors.  Both powers of 2.
  bless { "created" => 1 , "error" => "OK" , "cf_gen" => 0,
          "req_id" => 0, "min_wait" => 4,  "max_wait" => 8192, 
          "temp_errors" => 0 , "total_wait" => 0, "has_cas" => 0,
          "flags" => 0, "exptime" => 0, "get_results" => undef,
          "get_with_cas" => 0
        }, $pkg;
}


# Common code to ASCII and BINARY protocols:

sub fail {
  my $self = shift;
  my $msg = 
      "error:       " . $self->{error} . "\n" .
      "req_id:      " . $self->{req_id} . "\n" .
      "temp_errors: " . $self->{temp_errors} . "\n".
      "total_wait:  " . $self->{total_wait} . "\n";
  while(my $extra = shift) { 
    $msg .= $extra . "\n"; 
  }
  Carp::confess($msg);
}

sub connect {
  my $self = shift;
  my $host = shift;
  my $port = shift; 
  my $conn;
  
  # Wait for memcached to be ready, up to ten seconds.
  my $retries = 100;
  do {
    $conn = IO::Socket::INET->new(PeerAddr => "$host:$port", Proto => "tcp");
    if(! $conn) { 
      Time::HiRes::usleep(100 * 1000);
      $retries--;   
    }
  } while($retries && !$conn);

  if($conn) {
    $self->{connection} = $conn;
    $self->{connected} = 1;
    $self->{server} = "$host:$port";
    return 1;
  }
  $self->{error} = "CONNECTION_FAILED";
  return 0;
}

sub DESTROY {
  my $self = shift;
  if($self->{connection}) {
    $self->{connection}->close();
  }
}

sub set_expires {
  my $self = shift;
  $self->{exptime} = shift;  
}

sub set_flags {
  my $self = shift;
  $self->{flags} = shift;
}

# Some member variables are per-request.  
# Clear them in preparation for a new request, and increment the request counter.
sub new_request {
  my $self = shift;
  $self->{error} = "OK";
  $self->{has_cas} = 0;
  $self->{req_id}++;
}

sub next_result {
  my $self = shift;
  shift @{$self->{get_results}};
}

# note_config_version and wait_for_reconf are only for use by mysql-test-run
sub note_config_version {
  my $self = shift;

  my $vardir = $ENV{MYSQLTEST_VARDIR};
  # Fetch the memcached current config generation number and save it
  my %stats = $self->stats("reconf");
  my $F = IO::File->new("$vardir/tmp/memcache_cf_gen", "w") or die;
  my $ver = $stats{"Running"};
  print $F "$ver\n";
  $F->close();

  $self->{cf_gen} = $ver;
}

sub wait_for_reconf {
  my $self = shift;

  if($self->{cf_gen} == 0) { 
    my $cfgen = 0;
    my $vardir = $ENV{MYSQLTEST_VARDIR};
    my $F = IO::File->new("$vardir/tmp/memcache_cf_gen", "r");
    if(defined $F) {
      chomp($cfgen = <$F>);
      undef $F;
    }
    $self->{cf_gen} = $cfgen;
  }
  
  print STDERR "Config generation is : " . $self->{cf_gen} . "\n";
  my $wait_for = $self->{cf_gen} + 1 ;
  print STDERR "Waiting for: $wait_for \n";
  
  my $new_gen = $self->wait_for_config_generation($wait_for);
  if($new_gen > 0) {
    $self->{cf_gen} = $new_gen;
  }
  else {
    print STDERR "Timed out.\n";
  }
  
  return $new_gen;
}
  
# wait_for_config_generation($cf_gen)
# Wait until memcached is running config generation >= to $cf_gen
# Returns 0 on error/timeout, or the actual running generation number
#
sub wait_for_config_generation {
  my $self = shift;
  my $cf_gen = shift;
  my $ready = 0;
  my $retries = 100;   # 100 retries x 100 ms = 10s
  
  while($retries && ! $ready) {
    Time::HiRes::usleep(100 * 1000);
    my %stats = $self->stats("reconf");
    if($stats{"Running"} >= $cf_gen) {
      $ready = $stats{"Running"};
    }
    else {
      $retries -= 1;
    }
  }
  return $ready;
}

#  -----------------------------------------------------------------------
#  ------------------          ASCII PROTOCOL         --------------------
#  -----------------------------------------------------------------------

sub ascii_command {
  my $self = shift;
  my $packet = shift;
  my $sock = $self->{connection};
  my $waitTime = $self->{min_wait};
  my $maxWait = $self->{max_wait};
  my $reply;
  
  do {
    $self->new_request();
    $sock->print($packet) || Carp::confess("send error: ". $packet);
    $reply = $sock->getline();
    $self->normalize_error($reply);
    if($self->{error} eq "SERVER_TEMPORARY_ERROR") {
      if($waitTime < $maxWait) {
        $self->{temp_errors} += 1;
        $self->{total_wait} += ( Time::HiRes::usleep($waitTime * 1000) / 1000);
        $waitTime *= 2;
      }
      else {
        $self->fail("Too Many Temporary Errors", $waitTime);
      }
    }
  } while($self->{error} eq "SERVER_TEMPORARY_ERROR" && $waitTime <= $maxWait);
    
  return $reply;
}

  
sub delete {
  my $self = shift;
  my $key = shift;
  
  return ($self->ascii_command("delete $key\r\n") =~ "^DELETED");
}


sub store {
  my ($self, $cmd, $key, $value, $flags, $exptime, $cas_chk) = @_;
  $flags   = $self->{flags}   unless $flags;
  $exptime = $self->{exptime} unless $exptime;
  my $packet;
  if(($cmd eq "cas" || $cmd eq "replace") && $cas_chk > 0)
  {
    $packet = sprintf("cas %s %d %d %d %d\r\n%s\r\n", 
                      $key, $flags, $exptime, $cas_chk, length($value), $value);
  }
  else 
  {
    $packet = sprintf("%s %s %d %d %d\r\n%s\r\n",
                      $cmd, $key, $flags, $exptime, length($value), $value);
  }
  $self->ascii_command($packet);
  return ($self->{error} eq "OK");
}
 
sub set {
  my ($self, $key, $value, $flags, $exptime) = @_;  
  return $self->store("set", $key, $value, $flags, $exptime);
}


sub add {
  my ($self, $key, $value, $flags, $exptime) = @_;  
  return $self->store("add", $key, $value, $flags, $exptime);
}


sub append {
  my ($self, $key, $value, $flags, $exptime) = @_;  
  return $self->store("append", $key, $value, $flags, $exptime);
}


sub prepend {    
  my ($self, $key, $value, $flags, $exptime) = @_;  
  return $self->store("prepend", $key, $value, $flags, $exptime);
}


sub replace {
  my ($self, $key, $value, $flags, $exptime, $cas) = @_;  
  return $self->store("replace", $key, $value, $flags, $exptime, $cas);
}


sub get {
  my $self = shift;
  my $keys = "";
  $keys .= shift(@_) . " " while(@_);
  my $sock = $self->{connection};
  my @results;
  my $command = $self->{get_with_cas} ? "gets" : "get";
  $self->{get_with_cas} = 0; # CHECK, THEN RESET FOR NEXT CALL
  my $response = $self->ascii_command("$command $keys\r\n");
  while ($response ne "END\r\n") 
  {
    $response =~ /^VALUE (\S+) (\d+) (\d+) ?(\d+)?/;
    my $result = My::Memcache::Result->new($1, $2, $4);
    $sock->read($result->{value}, $3);   # $3 is length of value
    $self->{has_cas} = 1 if($4);
    push @results, $result;
    $sock->getline();  # \r\n after value
    $response = $sock->getline();
  }
  $self->{get_results} = \@results;
  return $results[0]->{value} if @results;
  $self->{error} = "NOT_FOUND";
  return undef;
}


sub _txt_math {
  my ($self, $cmd, $key, $delta) = @_;
  my $response = $self->ascii_command("$cmd $key $delta \r\n");
  
  if ($response =~ "^NOT_FOUND" || $response =~ "ERROR") {
    $self->normalize_error($response);
    return undef;
  }

  $response =~ /(\d+)/;
  return $1;
}


sub incr {
  my ($self, $key, $delta) = @_;
  return $self->_txt_math("incr", $key, $delta);
}


sub decr {
  my ($self, $key, $delta) = @_;
  return $self->_txt_math("decr", $key, $delta);
}


sub stats {
  my $self = shift;
  my $key = shift || "";
  my $sock = $self->{connection};

  $self->new_request();
  $sock->print("stats $key\r\n") || Carp::confess "send error";
  
  my %response = ();
  my $line = "";
  while($line !~ "^END") {
    return %response if $line eq "ERROR\r\n";
    if(($line) && ($line =~ /^STAT(\s+)(\S+)(\s+)(\S+)/)) {
      $response{$2} = $4;
    }
    $line = $sock->getline();
  }
  
  return %response;
}

sub flush {
  my $self = shift;
  my $key = shift;
  my $result = $self->ascii_command("flush_all\r\n");  
  return ($self->{error} eq "OK");
}


# Try to provide consistent error messagees across ascii & binary protocols
sub normalize_error {
  my $self = shift;
  my $reply = shift;
  my %error_message = (
  "STORED\r\n"                         => "OK",
  "EXISTS\r\n"                         => "KEY_EXISTS",
  "NOT_FOUND\r\n"                      => "NOT_FOUND",
  "NOT_STORED\r\n"                     => "NOT_STORED",
  "CLIENT_ERROR value too big\r\n"     => "VALUE_TOO_LARGE",
  "SERVER_ERROR object too large for cache\r\n"     => "VALUE_TOO_LARGE",
  "CLIENT_ERROR invalid arguments\r\n" => "INVALID_ARGUMENTS",
  "SERVER_ERROR not my vbucket\r\n"    => "NOT_MY_VBUCKET",
  "SERVER_ERROR out of memory\r\n"     => "SERVER_OUT_OF_MEMORY",
  "SERVER_ERROR not supported\r\n"     => "NOT_SUPPORTED",
  "SERVER_ERROR internal\r\n"          => "INTERNAL_ERROR",
  "SERVER_ERROR temporary failure\r\n" => "SERVER_TEMPORARY_ERROR"
  );  
  $self->{error} = $error_message{$reply} || "OK";
  return 0;
} 

#  -----------------------------------------------------------------------
#  ------------------         BINARY PROTOCOL         --------------------
#  -----------------------------------------------------------------------

package My::Memcache::Binary;
BEGIN { @My::Memcache::Binary::ISA = qw(My::Memcache); }
use constant BINARY_HEADER_FMT  => "CCnCCnNNNN";
use constant BINARY_REQUEST     => 0x80;
use constant BINARY_RESPONSE    => 0x81;

use constant BIN_CMD_GET        => 0x00;
use constant BIN_CMD_SET        => 0x01;
use constant BIN_CMD_ADD        => 0x02;
use constant BIN_CMD_REPLACE    => 0x03;
use constant BIN_CMD_DELETE     => 0x04;
use constant BIN_CMD_INCR       => 0x05;
use constant BIN_CMD_DECR       => 0x06;
use constant BIN_CMD_QUIT       => 0x07;
use constant BIN_CMD_FLUSH      => 0x08;
use constant BIN_CMD_NOOP       => 0x0A;
use constant BIN_CMD_GETK       => 0x0C;
use constant BIN_CMD_GETKQ      => 0x0D;
use constant BIN_CMD_APPEND     => 0x0E;
use constant BIN_CMD_PREPEND    => 0x0F;
use constant BIN_CMD_STAT       => 0x10;


sub error_message {
  my ($self, $code) = @_;
  my %error_messages = (
   0x00 => "OK",
   0x01 => "NOT_FOUND",
   0x02 => "KEY_EXISTS", 
   0x03 => "VALUE_TOO_LARGE",
   0x04 => "INVALID_ARGUMENTS",
   0x05 => "NOT_STORED",
   0x06 => "NON_NUMERIC_VALUE",
   0x07 => "NOT_MY_VBUCKET",
   0x81 => "UNKNOWN_COMMAND",
   0x82 => "SERVER_OUT_OF_MEMORY",
   0x83 => "NOT_SUPPORTED",
   0x84 => "INTERNAL_ERROR",
   0x85 => "SERVER_BUSY",
   0x86 => "SERVER_TEMPORARY_ERROR"
  );
  return $error_messages{$code};
}


sub send_binary_request {
  my $self = shift;
  my ($cmd, $key, $val, $extra_header, $cas) = @_;

  $cas = 0 unless $cas;
  my $sock = $self->{connection};
  my $key_len    = length($key);
  my $val_len    = length($val);
  my $extra_len  = length($extra_header);
  my $total_len  = $key_len + $val_len + $extra_len;
  my $cas_hi     = ($cas >> 32) & 0xFFFFFFFF;
  my $cas_lo     = ($cas & 0xFFFFFFFF);

  $self->new_request();
  
  my $header = pack(BINARY_HEADER_FMT, BINARY_REQUEST, $cmd,
                    $key_len, $extra_len, 0, 0, $total_len, 
                    $self->{req_id}, $cas_hi, $cas_lo);
  my $packet = $header . $extra_header . $key . $val;

  $sock->send($packet) || Carp::confess "send failed";
}


sub get_binary_response {
  my $self = shift;
  my $sock = $self->{connection};
  my $header_len = length(pack(BINARY_HEADER_FMT));
  my $header;
  my $body="";

  $sock->recv($header, $header_len);

  my ($magic, $cmd, $key_len, $extra_len, $datatype, $status, $body_len,
      $sequence, $cas_hi, $cas_lo) = unpack(BINARY_HEADER_FMT, $header);

  ($magic == BINARY_RESPONSE) || Carp::confess "Bad magic number in response";
  
  while($body_len - length($body) > 0) {
    my $buf;
    $sock->recv($buf, $body_len - length($body));
    $body .= $buf;
  }
  $self->{error} = $self->error_message($status);

  # Packet structure is: header .. extras .. key .. value 
  my $cas = ($cas_hi * (2 ** 32)) + $cas_lo;
  my $l = $extra_len + $key_len;
  my $extras = substr $body, 0, $extra_len;
  my $key    = substr $body, $extra_len, $key_len; 
  my $value  = substr $body, $l, $body_len - $l;

  return ($status, $value, $key, $extras, $cas, $sequence);
}  


sub binary_command {
  my $self = shift;
  my ($cmd, $key, $value, $extra_header, $cas) = @_;
  my $waitTime = $self->{min_wait};
  my $maxWait = $self->{max_wait};
  my $status;
  
  do {
    $self->send_binary_request($cmd, $key, $value, $extra_header, $cas);
    ($status) = $self->get_binary_response();
    if($status == 0x86) {
      if($waitTime < $maxWait) {
        $self->{temp_errors} += 1;
        $self->{total_wait} += ( Time::HiRes::usleep($waitTime * 1000) / 1000);
        $waitTime *= 2;
      }
      else {
        $self->fail("Too Many Temporary Errors", $waitTime);
      }
    }
  } while($status == 0x86 && $waitTime <= $maxWait);

  return ($status == 0) ? 1 : undef;
}


sub bin_math {
  my $self = shift;
  my ($cmd, $key, $delta, $initial) = @_;
  my $expires = 0xffffffff;  # 0xffffffff means the create flag is NOT set
  if(defined($initial))  { $expires = $self->{exptime};   }
  else                   { $initial = 0;                  }
  my $value = undef;
  
  my $extra_header = pack "NNNNN", 
  ($delta   / (2 ** 32)),   # delta hi
  ($delta   % (2 ** 32)),   # delta lo
  ($initial / (2 ** 32)),   # initial hi
  ($initial % (2 ** 32)),   # initial lo
  $expires;
  $self->send_binary_request($cmd, $key, '', $extra_header);

  my ($status, $packed_val) = $self->get_binary_response();
  if($status == 0) {
    my ($val_hi, $val_lo) = unpack("NN", $packed_val);
    $value = ($val_hi * (2 ** 32)) + $val_lo;
  }
  return $value;
}


sub bin_store {
  my ($self, $cmd, $key, $value, $flags, $exptime, $cas) = @_;
  $flags   = $self->{flags}   unless $flags;
  $exptime = $self->{exptime} unless $exptime;
  my $extra_header = pack "NN", $flags, $exptime;
  
  return $self->binary_command($cmd, $key, $value, $extra_header, $cas);
}

## Pipelined multi-get
sub get {
  my $self = shift;
  my $idx = $#_;   # Index of the final key
  my $cmd = BIN_CMD_GETKQ;  # GET + KEY + NOREPLY
  for(my $i = 0 ; $i <= $idx ; $i++) {
    $cmd = BIN_CMD_GETK if($i == $idx);  # Final request gets replies
    $self->send_binary_request($cmd, $_[$i], '', '');
  }

  my $sequence = 0;
  my @results;  
  while($sequence < $self->{req_id}) {
    my ($status, $value, $key, $extra, $cas);
    ($status, $value, $key, $extra, $cas, $sequence) = $self->get_binary_response();
    unless($status) {
      my $result = My::Memcache::Result->new($key, unpack("N", $extra), $cas);
      $result->{value} = $value;
      push @results, $result;
    }
  }
  $self->{get_results} = \@results;
  if(@results) {
    $self->{error} = "OK";
    return $results[0]->{value};
  }
  $self->{error} = "NOT_FOUND";
  return undef;
}


sub stats {
  my $self = shift;
  my $key = shift;
  my %response, my $status, my $value, my $klen, my $tlen;

  $self->send_binary_request(BIN_CMD_STAT, $key, '', '');
  do {
    ($status, $value, $key) = $self->get_binary_response();
    if($status == 0) {
      $response{$key} = $value;
    } 
  } while($status == 0 && $key);

  return %response;
}

sub flush {
  my ($self, $key, $value) = @_;
  $self->send_binary_request(BIN_CMD_FLUSH, $key, '', ''); 
  my ($status, $result) = $self->get_binary_response();
  return ($status == 0) ? 1 : 0;
}

sub store {
  my ($self, $cmd, $key, $value, $flags, $exptime, $cas) = @_;
  my %cmd_map = (
    "set" => BIN_CMD_SET , "add" => BIN_CMD_ADD , "replace" => BIN_CMD_REPLACE ,
    "append" => BIN_CMD_APPEND , "prepend" => BIN_CMD_PREPEND
  );
  return $self->bin_store($cmd_map{$cmd}, $key, $value, $flags, $exptime, $cas);
}
  
sub set {
  my ($self, $key, $value) = @_;
  return $self->bin_store(BIN_CMD_SET, $key, $value);
}

sub add {
  my ($self, $key, $value) = @_;
  return $self->bin_store(BIN_CMD_ADD, $key, $value);
}

sub replace {
  my ($self, $key, $value) = @_;
  return $self->bin_store(BIN_CMD_REPLACE, $key, $value);
}

sub append {
  my ($self, $key, $value) = @_;
  return $self->binary_command(BIN_CMD_APPEND, $key, $value, '');
}

sub prepend {
  my ($self, $key, $value) = @_;
  return $self->binary_command(BIN_CMD_PREPEND, $key, $value, '');
}

sub delete { 
  my ($self, $key) = @_;
  return $self->binary_command(BIN_CMD_DELETE, $key, '', '');
}
  
sub incr {
  my ($self, $key, $delta, $initial) = @_;
  return $self->bin_math(BIN_CMD_INCR, $key, $delta, $initial);
}

sub decr {
  my ($self, $key, $delta, $initial) = @_;
  return $self->bin_math(BIN_CMD_DECR, $key, $delta, $initial);
}


1;
