
# hanzo_iam against a real server: tokens are signed with keys made here,
# presented the way pgx presents them (SASL OAUTHBEARER, "Bearer <token>"),
# and every refusal is checked for the reason the server logged, so a token
# is not refused by accident.

use strict;
use warnings FATAL => 'all';

use File::Copy ();
use IO::Select;
use IO::Socket::UNIX;
use IPC::Run;
use JSON::PP;
use MIME::Base64;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $json = JSON::PP->new->canonical;
my $dir = PostgreSQL::Test::Utils::tempdir;

# ---- keys: sql1 and sql2 are trusted, sql3 comes and goes, the others are not.
# ec, dsa and small are unfit to sign whatever file they are in: a curve key
# and a 1024-bit key fail the size rule, and a 2048-bit DSA key only the type.

sub genkey
{
	my ($name, @algorithm) = @_;
	@algorithm = ('-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048')
	  unless @algorithm;
	command_ok(
		[ 'openssl', 'genpkey', @algorithm, '-out', "$dir/$name.key" ],
		"generate $name");
	command_ok(
		[ 'openssl', 'pkey', '-in', "$dir/$name.key", '-pubout', '-out', "$dir/$name.pub" ],
		"public half of $name");
}

genkey($_) for qw(sql1 sql2 sql3 other iam);
genkey('ec', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256');
genkey('small', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:1024');
command_ok(
	[
		'openssl', 'genpkey', '-genparam', '-algorithm', 'DSA',
		'-pkeyopt', 'dsa_paramgen_bits:2048', '-pkeyopt', 'dsa_paramgen_q_bits:256',
		'-out', "$dir/dsa.param"
	],
	'DSA parameters');
genkey('dsa', '-paramfile', "$dir/dsa.param");

# hanzo_iam.dir as a ConfigMap mounts it: files that are links into ..data.
my $keys = "$dir/trust";
mkdir $keys or die;
mkdir "$keys/..v1" or die;
for my $n (qw(1 2))
{
	copy_pub("sql$n", "$keys/..v1/sql-$n.pem");
}
symlink '..v1', "$keys/..data" or die;
symlink "..data/sql-$_.pem", "$keys/sql-$_.pem" or die for qw(1 2);

sub copy_pub
{
	my ($name, $to) = @_;
	open my $in, '<', "$dir/$name.pub" or die;
	open my $out, '>', $to or die;
	print $out $_ while <$in>;
}

# ---- tokens

sub b64u
{
	my $s = encode_base64($_[0], '');
	$s =~ tr{+/}{-_};
	$s =~ s/=+\z//;
	return $s;
}

sub sign
{
	my ($keyfile, $msg) = @_;
	my ($out, $err);
	IPC::Run::run([ 'openssl', 'dgst', '-sha256', '-sign', $keyfile ],
		'<', \$msg, '>', \$out, '2>', \$err)
	  or die "openssl dgst: $err";
	return $out;
}

# The signed part of a token valid for role cloud now; header, claims (undef
# drops one) can be changed, or the JSON of either replaced whole.
sub signing_input
{
	my (%o) = @_;
	my $now = $o{now} // time;
	my %h = (alg => 'RS256', typ => 'JWT', kid => 'sql-1', %{ $o{header} // {} });
	my %c = (
		iss => 'https://hanzo.id',
		aud => ['hanzo-sql'],
		sub => 'sql:cloud',
		typ => 'sql',
		jti => 'tap',
		iat => 0 + $now,
		nbf => 0 + $now,
		exp => $now + 300,
		%{ $o{claims} // {} });
	for my $h (\%h, \%c)
	{
		delete $h->{$_} for grep { !defined $h->{$_} } keys %$h;
	}
	return b64u($o{header_json} // $json->encode(\%h)) . '.'
	  . b64u($o{claims_json} // $json->encode(\%c));
}

# The whole token, signed by sql1 unless key says another.
sub mint
{
	my (%o) = @_;
	my $signing = signing_input(%o);
	return $signing . '.' . b64u(sign($o{key} // "$dir/sql1.key", $signing));
}

# A token valid for role cloud that is exactly $size bytes long.  The padding
# goes in the claims; base64url has no length of 4n+1, so some sizes need a
# header member as well.  An RSA-2048 signature is 256 bytes, 342 characters.
sub token_of
{
	my ($size) = @_;
	for my $hpad (0 .. 3)
	{
		my %o = (header => $hpad ? { pad => 'h' x $hpad } : {});
		for my $pad (0 .. $size)
		{
			$o{claims} = { pad => 'a' x $pad };
			my $len = length(signing_input(%o)) + 1 + 342;
			last if $len > $size;
			return mint(%o) if $len == $size;
		}
	}
	die "no token of $size bytes";
}

# ---- the server

my $node = PostgreSQL::Test::Cluster->new('iam');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
oauth_validator_libraries = 'hanzo_iam'
hanzo_iam.dir = '$keys'
log_connections = 'authentication,authorization'
});
$node->start;

my $admin = $node->safe_psql('postgres', 'SELECT current_user');
$node->safe_psql('postgres', q{
	CREATE ROLE sql_iam NOLOGIN;
	CREATE ROLE cloud LOGIN IN ROLE sql_iam;
	CREATE ROLE iam LOGIN IN ROLE sql_iam;
});

# The pg_hba.conf shape of hanzo-sql: members of sql_iam come in by token only.
unlink $node->data_dir . '/pg_hba.conf';
$node->append_conf(
	'pg_hba.conf', qq{
local all $admin trust
local all +sql_iam oauth issuer="https://hanzo.id" scope="hanzo-sql" validator=hanzo_iam delegate_ident_mapping=1
});
$node->reload;

# ---- a client that sends what pgx sends, and says what the server said

# Read from $sock until $$buf holds $want bytes; false at end of file.  A
# server that stalls fails the test after the suite's timeout.
sub fill
{
	my ($sock, $buf, $want) = @_;
	while (length $$buf < $want)
	{
		IO::Select->new($sock)->can_read($PostgreSQL::Test::Utils::timeout_default)
		  or die "timed out waiting for the server";
		sysread($sock, my $more, $want - length $$buf) or return 0;
		$$buf .= $more;
	}
	return 1;
}

sub message
{
	my ($sock) = @_;
	my ($head, $body) = ('', '');
	fill($sock, \$head, 5) or return;
	my ($type, $len) = unpack('a N', $head);
	fill($sock, \$body, $len - 4) or return;
	return ($type, $body);
}

sub send_message
{
	my ($sock, $type, $body) = @_;
	syswrite($sock, $type . pack('N', 4 + length $body) . $body);
}

# Log in as $role presenting $token (undef: no token, only the discovery
# request).  Returns { user => current_user once in, error => the server's }.
sub login
{
	my ($role, $token) = @_;
	my $sock = IO::Socket::UNIX->new(
		Type => SOCK_STREAM,
		Peer => $node->host . '/.s.PGSQL.' . $node->port) or die "connect: $!";
	my %result;
	my ($authenticated, $asked) = (0, 0);

	my $startup = pack('N', 196608) . "user\0$role\0database\0postgres\0\0";
	syswrite($sock, pack('N', 4 + length $startup) . $startup);

	while (my ($type, $body) = message($sock))
	{
		if ($type eq 'R')
		{
			my $code = unpack('N', $body);
			if ($code == 10)
			{
				my $auth = defined $token ? "Bearer $token" : '';
				my $first = "n,,\x01auth=$auth\x01\x01";
				send_message($sock, 'p',
					"OAUTHBEARER\0" . pack('N', length $first) . $first);
			}
			elsif ($code == 11)
			{
				# The server's error document; the answer is one kvsep.
				send_message($sock, 'p', "\x01");
			}
			elsif ($code == 0)
			{
				$authenticated = 1;
			}
		}
		elsif ($type eq 'E')
		{
			my %field = map { /\A(.)(.*)\z/s ? ($1 => $2) : () } split /\0/, $body;
			$result{error} = $field{M};
			last;
		}
		elsif ($type eq 'Z')
		{
			last unless $authenticated;
			if ($asked)
			{
				send_message($sock, 'X', '');
				last;
			}
			$asked = 1;
			send_message($sock, 'Q', "SELECT current_user\0");
		}
		elsif ($type eq 'D')
		{
			my ($cols, $len) = unpack('n N', $body);
			$result{user} = substr($body, 6, $len);
		}
	}
	close $sock;
	return \%result;
}

my $skew = 60;

sub accepted
{
	my ($name, $role, $token) = @_;
	my $r = login($role, $token);
	is($r->{user}, $role, "$name: connects as $role");
}

sub refused
{
	my ($name, $role, $token, $reason) = @_;
	my $off = -s $node->logfile;
	my $r = login($role, $token);
	ok(!defined $r->{user}, "$name: refused");
	like($r->{error} // '', qr/authentication failed/, "$name: the client is told it failed");
	my $logged = eval { $node->wait_for_log(qr/hanzo_iam: \Q$reason\E/, $off); 1 };
	ok($logged, "$name: logged as \"$reason\"");
}

# ---- a valid token

accepted('valid token', 'cloud', mint());
accepted('valid token for another role', 'iam', mint(claims => { sub => 'sql:iam' }));
{
	my $off = -s $node->logfile;
	login('cloud', mint());
	$node->wait_for_log(qr/connection authenticated: identity="cloud" method=oauth/, $off);
	pass('the role is the authenticated identity in the log');
}
accepted('second key, reached through ConfigMap links', 'cloud',
	mint(header => { kid => 'sql-2' }, key => "$dir/sql2.key"));
accepted('aud is a string', 'cloud', mint(claims => { aud => 'hanzo-sql' }));
accepted('aud lists other audiences too', 'cloud',
	mint(claims => { aud => [ 'hanzo-cloud', 'hanzo-sql' ] }));
accepted('nbf is optional', 'cloud', mint(claims => { nbf => undef }));
accepted('claims no check names are ignored', 'cloud',
	mint(claims => { azp => 'x', cnf => { k => [ 1, { a => undef } ] } }));
accepted('exp 30 s past is within the skew', 'cloud',
	mint(claims => { iat => time - 330, exp => time - 30, nbf => undef }));
accepted('nbf 30 s ahead is within the skew', 'cloud', mint(claims => { nbf => time + 30 }));
accepted('lifetime of exactly 600 s', 'cloud',
	mint(claims => { iat => time, exp => time + 600 }));

# ---- missing

{
	my $off = -s $node->logfile;
	my $r = login('cloud', undef);
	ok(!defined $r->{user}, 'missing token: refused');
	like($r->{error} // '', qr/authentication failed/, 'missing token: the client is told it failed');
	unlike(slurp_file($node->logfile, $off), qr/hanzo_iam:/,
		'missing token: the validator is not asked');
}

# ---- signature

refused('bad signature', 'cloud', mint(key => "$dir/other.key"), 'bad signature');
refused(
	'claims changed after signing', 'cloud',
	do
	{
		my @t = split /\./, mint();
		my %c = (iss => 'https://hanzo.id', aud => ['hanzo-sql'], sub => 'sql:iam',
			typ => 'sql', iat => 0 + time, exp => time + 300);
		join '.', $t[0], b64u($json->encode(\%c)), $t[2];
	},
	'bad signature');
refused('an IAM-signed token', 'cloud',
	mint(header => { kid => 'cert-hanzo' }, key => "$dir/iam.key",
		claims => { aud => ['hanzo-cloud'], sub => 'hanzo/z', typ => undef }),
	'unknown key');
refused('an IAM key under a trusted kid', 'cloud', mint(key => "$dir/iam.key"), 'bad signature');

# ---- keys

refused('unknown kid', 'cloud', mint(header => { kid => 'sql-9' }, key => "$dir/sql1.key"),
	'unknown key');
{
	copy_pub('sql3', "$keys/sql-3.pem");
	my $token = mint(header => { kid => 'sql-3' }, key => "$dir/sql3.key");
	accepted('a key just added', 'cloud', $token);
	unlink "$keys/sql-3.pem" or die;
	refused('the same token once the kid file is removed', 'cloud', $token, 'unknown key');
}

# A key file is a PEM public key, RSA, of 2048 bits or more, in at most 16 KiB.
{
	copy_pub('ec', "$keys/sql-ec.pem");
	refused('an EC key under a trusted kid', 'cloud',
		mint(header => { kid => 'sql-ec' }, key => "$dir/ec.key"), 'unusable key');
	copy_pub('dsa', "$keys/sql-dsa.pem");
	refused('a DSA key of 2048 bits', 'cloud',
		mint(header => { kid => 'sql-dsa' }, key => "$dir/dsa.key"), 'unusable key');
	copy_pub('small', "$keys/sql-small.pem");
	refused('an RSA key of 1024 bits', 'cloud',
		mint(header => { kid => 'sql-small' }, key => "$dir/small.key"), 'unusable key');

	command_ok(
		[
			'openssl', 'req', '-x509', '-new', '-key', "$dir/sql1.key",
			'-subj', '/CN=sql', '-days', '1', '-out', "$dir/sql1.crt"
		],
		'certificate for sql1');
	File::Copy::copy("$dir/sql1.key", "$keys/sql-private.pem") or die;
	File::Copy::copy("$dir/sql1.crt", "$keys/sql-cert.pem") or die;
	for my $file ([ 'private', 'a private key file' ], [ 'cert', 'a certificate file' ])
	{
		my ($kid, $what) = @$file;
		my $off = -s $node->logfile;
		refused("$what under a kid", 'cloud', mint(header => { kid => "sql-$kid" }),
			'unknown key');
		like(slurp_file($node->logfile, $off),
			qr/hanzo_iam: "[^"]*sql-$kid\.pem" is not a PEM public key/,
			"$what: the log names sql-$kid.pem");
	}

	open my $empty, '>', "$keys/sql-empty.pem" or die;
	close $empty;
	refused('an empty key file', 'cloud', mint(header => { kid => 'sql-empty' }), 'unknown key');
	mkdir "$keys/sql-dir.pem" or die;
	refused('a directory named <kid>.pem', 'cloud', mint(header => { kid => 'sql-dir' }),
		'unknown key');

	# The public key of sql1, padded with newlines to a size.
	for my $size ([ 'sql-16k', 16384 ], [ 'sql-over', 16385 ])
	{
		open my $in, '<', "$dir/sql1.pub" or die;
		my $pem = do { local $/; <$in> };
		close $in;
		open my $out, '>', "$keys/$size->[0].pem" or die;
		print $out $pem, "\n" x ($size->[1] - length $pem);
		close $out;
		is(-s "$keys/$size->[0].pem", $size->[1], "$size->[0].pem is $size->[1] bytes");
	}
	accepted('a key file of exactly 16 KiB', 'cloud', mint(header => { kid => 'sql-16k' }));
	refused('a key file over 16 KiB', 'cloud', mint(header => { kid => 'sql-over' }),
		'unknown key');
}

refused('no kid', 'cloud', mint(header => { kid => undef }), 'bad key id');
refused('kid names a path', 'cloud', mint(header => { kid => '../trust/sql-1' }), 'bad key id');
refused('kid is absolute', 'cloud', mint(header => { kid => '/etc/passwd' }), 'bad key id');
refused('kid is hidden', 'cloud', mint(header => { kid => '.sql-1' }), 'bad key id');
refused('kid is too long', 'cloud', mint(header => { kid => 'k' x 65 }), 'bad key id');
refused('alg none', 'cloud', mint(header => { alg => 'none' }), 'unsupported algorithm');
refused('alg HS256', 'cloud', mint(header => { alg => 'HS256' }), 'unsupported algorithm');
refused('no alg', 'cloud', mint(header => { alg => undef }), 'unsupported algorithm');

# ---- claims

refused('wrong iss', 'cloud', mint(claims => { iss => 'https://iam.example' }), 'wrong issuer');
refused('no iss', 'cloud', mint(claims => { iss => undef }), 'wrong issuer');
refused('wrong aud', 'cloud', mint(claims => { aud => ['hanzo-cloud'] }), 'wrong audience');
refused('aud is a string for another service', 'cloud', mint(claims => { aud => 'hanzo-cloud' }),
	'wrong audience');
refused('no aud', 'cloud', mint(claims => { aud => undef }), 'wrong audience');
refused('wrong typ', 'cloud', mint(claims => { typ => 'JWT' }), 'wrong token type');
refused('no typ', 'cloud', mint(claims => { typ => undef }), 'wrong token type');
refused('act present', 'cloud', mint(claims => { act => { sub => 'sql:iam' } }), 'act claim present');
refused('act present as null', 'cloud',
	mint(claims_json => '{"iss":"https://hanzo.id","aud":["hanzo-sql"],"sub":"sql:cloud","typ":"sql","iat":'
		  . time . ',"exp":' . (time + 300) . ',"act":null}'),
	'act claim present');

# JSON escapes reach the server as written: these are single-quoted, so Perl
# leaves the backslashes.  A member and a value mean what they decode to.
refused('act spelled with an escape', 'cloud',
	mint(claims_json => '{"iss":"https://hanzo.id","aud":["hanzo-sql"],"sub":"sql:cloud","typ":"sql","iat":'
		  . time . ',"exp":' . (time + 300) . ',"\u0061ct":1}'),
	'act claim present');
refused('exp twice, one spelled with an escape', 'cloud',
	mint(claims_json => '{"iss":"https://hanzo.id","aud":["hanzo-sql"],"sub":"sql:cloud","typ":"sql","iat":'
		  . time . ',"exp":' . (time + 300) . ',"\u0065xp":' . (time + 5000) . '}'),
	'duplicate member');
refused('sub escaped to a role with a newline', 'cloud',
	mint(claims_json => '{"iss":"https://hanzo.id","aud":["hanzo-sql"],"sub":"sql:cloud\u000a","typ":"sql","iat":'
		  . time . ',"exp":' . (time + 300) . '}'),
	'bad subject');
accepted('a member and values spelled with escapes', 'cloud',
	mint(claims_json => '{"\u0069ss":"https:\/\/hanzo.id","aud":["hanzo-\u0073ql"],"sub":"sql:cl\u006fud","typ":"sql","iat":'
		  . time . ',"exp":' . (time + 300) . '}'));
refused('sub without the prefix', 'cloud', mint(claims => { sub => 'cloud' }), 'bad subject');
refused('sub with another prefix', 'cloud', mint(claims => { sub => 'iam:cloud' }), 'bad subject');
refused('sub with no role', 'cloud', mint(claims => { sub => 'sql:' }), 'bad subject');
refused('sub with a space', 'cloud', mint(claims => { sub => 'sql:cl oud' }), 'bad subject');
refused('sub with a newline', 'cloud', mint(claims => { sub => "sql:cloud\nFATAL" }), 'bad subject');
refused('no sub', 'cloud', mint(claims => { sub => undef }), 'bad subject');

# ---- time

{
	my $now = time;
	refused('expired', 'cloud',
		mint(claims => { iat => $now - 600, nbf => undef, exp => $now - 300 }), 'token expired');
	refused('expired past the skew', 'cloud',
		mint(claims => { iat => $now - 400, nbf => undef, exp => $now - $skew - 30 }),
		'token expired');
	refused('nbf in the future', 'cloud', mint(claims => { nbf => $now + 3600 }),
		'token not yet valid');
	refused('nbf past the skew', 'cloud', mint(claims => { nbf => $now + $skew + 30 }),
		'token not yet valid');
	refused('iat in the future', 'cloud',
		mint(claims => { nbf => undef, iat => $now + 3600, exp => $now + 3900 }),
		'token not yet valid');
	refused('lifetime over 600 s', 'cloud', mint(claims => { iat => $now, exp => $now + 601 }),
		'lifetime out of range');
	refused('lifetime of a day', 'cloud', mint(claims => { iat => $now, exp => $now + 86400 }),
		'lifetime out of range');
	refused('exp not after iat', 'cloud', mint(claims => { iat => $now, exp => $now }),
		'lifetime out of range');
	refused('no exp', 'cloud', mint(claims => { exp => undef }), 'missing exp');
	refused('no iat', 'cloud', mint(claims => { iat => undef }), 'missing iat');
}

# ---- role

{
	my $off = -s $node->logfile;
	refused('role mismatch', 'cloud', mint(claims => { sub => 'sql:iam' }), 'role mismatch');
	like(slurp_file($node->logfile, $off), qr/hanzo_iam: role mismatch$/m,
		'the reason is logged alone, with neither role');
}
{
	my $off = -s $node->logfile;
	login('cloud', mint(claims => { sub => 'sql:iam' }));
	$node->wait_for_log(qr/connection authenticated: identity="iam" method=oauth/, $off);
	pass('a token for another role still names its own identity in the log');
}

# ---- size

accepted('padding of 100 bytes', 'cloud', mint(claims => { pad => 'a' x 100 }));
refused('oversized', 'cloud', mint(claims => { pad => 'a' x 9000 }), 'token too large');
{
	# The bound is 8 KiB of the whole token, and the decoders are sized by it.
	my $at = token_of(8192);
	is(length $at, 8192, 'a token of exactly 8192 bytes');
	accepted('the largest token', 'cloud', $at);
	my $over = token_of(8193);
	is(length $over, 8193, 'a token of 8193 bytes');
	refused('one byte over', 'cloud', $over, 'token too large');
	refused('a header part that nearly fills the bound', 'cloud', ('A' x 8184) . '.AA.AA',
		'malformed header');
	refused('a header part past the bound', 'cloud', ('A' x 8796) . '.A.A', 'token too large');
}

# ---- shape

refused('not a token', 'cloud', 'abc', 'malformed token');
refused('two parts', 'cloud', 'a.b', 'malformed token');
refused('four parts', 'cloud', mint() . '.AA', 'malformed token');
refused('empty parts', 'cloud', '..', 'malformed token');
refused('empty signature', 'cloud', (join '.', (split /\./, mint())[ 0, 1 ]) . '.', 'malformed token');
refused('not base64url', 'cloud', 'a+.b.c', 'malformed token');
refused('header is not JSON', 'cloud', mint(header_json => 'not json'), 'malformed header');
refused('header is an array', 'cloud', mint(header_json => '[]'), 'malformed header');
refused('claims are not JSON', 'cloud', mint(claims_json => 'not json'), 'malformed claims');
refused('claims are a string', 'cloud', mint(claims_json => '"sql:cloud"'), 'malformed claims');
refused('exp is a string', 'cloud', mint(claims => { exp => '1' }), 'malformed claims');
refused('exp is fractional', 'cloud',
	mint(claims_json => '{"iss":"https://hanzo.id","aud":["hanzo-sql"],"sub":"sql:cloud","typ":"sql","iat":'
		  . time . ',"exp":' . (time + 300) . '.5}'),
	'malformed claims');
refused('iss is an object', 'cloud', mint(claims => { iss => {} }), 'malformed claims');
refused('aud holds a number', 'cloud', mint(claims => { aud => [ 'hanzo-sql', 5 ] }), 'malformed claims');
refused('aud is nested', 'cloud', mint(claims => { aud => [ ['hanzo-sql'] ] }), 'malformed claims');
refused('nested too deep', 'cloud', mint(claims_json => '{"x":' . ('[' x 50) . (']' x 50) . '}'),
	'malformed claims');
refused('NUL in a string', 'cloud',
	mint(claims_json => '{"iss":"https://hanzo.id","aud":["hanzo-sql"],"sub":"sql:cl\u0000oud","typ":"sql","iat":'
		  . time . ',"exp":' . (time + 300) . '}'),
	'malformed claims');
refused('exp twice', 'cloud',
	mint(claims_json => '{"iss":"https://hanzo.id","aud":["hanzo-sql"],"sub":"sql:cloud","typ":"sql","iat":'
		  . time . ',"exp":' . (time + 300) . ',"exp":' . (time + 5000) . '}'),
	'duplicate member');
refused('kid twice', 'cloud', mint(header_json => '{"alg":"RS256","kid":"sql-1","kid":"sql-1"}'),
	'duplicate member');

# ---- the directory

$node->append_conf('postgresql.conf', "hanzo_iam.dir = ''\n");
$node->reload;
refused('no directory set', 'cloud', mint(), 'hanzo_iam.dir is not set');

done_testing();
