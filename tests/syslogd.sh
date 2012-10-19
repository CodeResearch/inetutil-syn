#!/bin/sh

# Copyright (C) 2011, 2012 Free Software Foundation, Inc.
#
# This file is part of GNU Inetutils.
#
# GNU Inetutils is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or (at
# your option) any later version.
#
# GNU Inetutils is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see `http://www.gnu.org/licenses/'.

# Tests to establish functionality of SYSLOG daemon.
#
# Written by Mats Erik Andersson.

# Prerequisites:
#
#  * Shell: SVR4 Bourne shell, or newer.
#
#  * id(1), kill(1), mktemp(1), netstat(8), uname(1).


# Is usage explanation in demand?
#
if test "$1" = "-h" || test "$1" = "--help" || test "$1" = "--usage"; then
    cat <<HERE
Test utility for syslogd and logger.

The following environment variables are used:

NOCLEAN		No clean up of testing directory, if set.
VERBOSE		Be verbose, if set.
OPTIONS		Base options to build upon.
IU_TESTDIR	If set, use this as testing dir.  Unless
		NOCLEAN is also set, any created \$IU_TESTDIR
		will be erased after the test.  A non-existing
		directory must be named as a template for mktemp(1).
REMOTE_LOGHOST	Add this host as a receiving loghost.
TARGET		Receiving IPv4 address.
TARGET6		Receiving IPv6 address.

HERE
    exit 0
fi

# Step into `tests/', should the invokation
# have been made outside of it.
#
[ -d src ] && [ -f tests/syslogd.sh ] && cd tests/

. ./tools.sh

if test -z "${VERBOSE+set}"; then
    silence=:
fi

# Portability fix for SVR4
PWD="${PWD:-`pwd`}"
USER="${USER:-`id -u -n`}"

$need_mktemp || exit_no_mktemp
$need_netstat || exit_no_netstat

# Execution control.  Initialise early!
#
do_cleandir=false
do_socket_length=true
do_unix_socket=true
do_inet_socket=true
do_standard_port=true

# The UNIX socket name length is preset by the system
# and is also system dependent.
#
# A long name consists of 103 or 107 non-NUL characters,
# whereas the excessive string contains 104 or 108 characters.
# BSD allocates only 104, while Glibc and Solaris admit 108
# characters in "sun_path", a count which includes the final NUL.

iu_socklen_max=104	# BSD flavour!

IU_OS=`uname -s`
if test "$IU_OS" = "Linux" || test "$IU_OS" = "GNU/kFreeBSD" ||
	test "$IU_OS" = "SunOS"; then
    # Aim at the boundary of 108 characters.
    iu_socklen_max=108
fi

# The executables under test.
#
SYSLOGD=${SYSLOGD:-../src/syslogd$EXEEXT}
LOGGER=${LOGGER:-../src/logger$EXEEXT}

if [ $VERBOSE ]; then
    set -x
    $SYSLOGD --version | $SED '1q'
    $LOGGER --version | $SED '1q'
fi

if [ ! -x $SYSLOGD ]; then
    echo "Missing executable '$SYSLOGD'.  Failing." >&2
    exit 77
fi

if [ ! -x $LOGGER ]; then
    echo "Missing executable '$LOGGER'.  Failing." >&2
    exit 77
fi

# For file creation below IU_TESTDIR.
umask 0077

# Keep any external assignment of testing directory.
# Otherwise a randomisation is included.
#
: ${IU_TESTDIR:=$PWD/iu_syslog.XXXXXX}

if [ ! -d "$IU_TESTDIR" ]; then
    do_cleandir=true
    IU_TESTDIR="`$MKTEMP -d "$IU_TESTDIR" 2>/dev/null`" ||
	{
	    echo 'Failed at creating test directory.  Aborting.' >&2
	    exit 77
	}
elif expr X"$IU_TESTDIR" : X"\.\{1,2\}/\{0,1\}$" >/dev/null; then
    # Eliminating directories: . ./ .. ../
    echo 'Dangerous input for test directory.  Aborting.' >&2
    exit 77
fi

# The SYSLOG daemon uses four files.
#
CONF="$IU_TESTDIR"/syslog.conf
PID="$IU_TESTDIR"/syslogd.pid
OUT="$IU_TESTDIR"/messages
: ${SOCKET:=$IU_TESTDIR/log}

# Are we able to write in IU_TESTDIR?
# This could happen with preset IU_TESTDIR.
#
touch "$OUT" || {
    echo 'No write access in test directory.  Aborting.' >&2
    exit 1
}

# Some automated build environments dig deep chroots, i.e.,
# make the paths to this working directory disturbingly long.
# Check SOCKET for this calamity.
#
if test `expr X"$SOCKET" : X".*"` -gt $iu_socklen_max; then
    do_unix_socket=false
    cat <<-EOT >&2
	WARNING! The working directory uses a disturbingly long path.
	We are not able to construct a UNIX socket on top of it.
	Therefore disabling socket messaging in this test run.
	EOT
fi

# Erase the testing directory.
#
clean_testdir () {
    if test -f "$PID" && kill -0 "`cat "$PID"`" >/dev/null 2>&1; then
	kill "`cat "$PID"`" || kill -9 "`cat "$PID"`"
    fi
    if test -z "${NOCLEAN+no}" && $do_cleandir; then
	rm -r -f "$IU_TESTDIR"
    fi
    if $do_socket_length && test -d "$IU_TMPDIR"; then
	rmdir "$IU_TMPDIR"	# Should be empty.
    fi
}

# Clean artifacts as execution stops.
#
trap clean_testdir EXIT HUP INT QUIT TERM

# Test at this port.
# Standard is syslog at 514/udp.
PROTO=udp
PORT=${PORT:-514}

# locate_port proto port
#
locate_port () {
    if [ "`uname -s`" = "SunOS" ]; then
	$NETSTAT -na -finet -finet6 -P$1 |
	$GREP "\.$2[^0-9]" >/dev/null 2>&1
    else
	$NETSTAT -na |
	$GREP "^$1[46]\{0,2\}.*[^0-9]$2[^0-9]" >/dev/null 2>&1
    fi
}

# Receivers for INET sockets.
: ${TARGET:=127.0.0.1}
: ${TARGET6:=[::1]}

# For testing of critical lengths for UNIX socket names,
# we need a well defined base directory; choose $TMPDIR.
IU_TMPDIR=${TMPDIR:=/tmp}

if test ! -d "$IU_TMPDIR"; then
    do_socket_length=false
    cat <<-EOT >&2
	WARNING!  Disabling socket length test since the directory
	"$IU_TMPDIR", for temporary storage, does not exist.
	EOT
else
    # Append a slash if it is missing.
    expr X"$IU_TMPDIR" : X".*/$" >/dev/null || IU_TMPDIR="$IU_TMPDIR/"

    IU_TMPDIR="`$MKTEMP -d "${IU_TMPDIR}iu.XXXXXX" 2>/dev/null`" ||
	{   # Directory creation failed.  Disable test.
	    cat <<-EOT >&2
		WARNING!  Unable to create temporary directory below
		"${TMPDIR:-/tmp}" for socket length test.
		Now disabling this subtest.
		EOT
	    do_socket_length=false
	}
fi

iu_eighty=0123456789
iu_eighty=${iu_eighty}${iu_eighty}
iu_eighty=${iu_eighty}${iu_eighty}
iu_eighty=${iu_eighty}${iu_eighty}

# This good name base will be expanded.
IU_GOOD_BASE=${IU_TMPDIR}/_iu

# Add a single character to violate the size condition.
IU_BAD_BASE=${IU_TMPDIR}/X_iu

if test `expr X"$IU_GOOD_BASE" : X".*"` -gt $iu_socklen_max; then
    # Maximum socket length is already less than prefix.
    echo 'WARNING! Disabling socket length test.  Too long base name' >&2
    do_socket_length=false
fi

if $do_socket_length; then
    # Compute any patching needed to get socket names
    # touching the limit of allowed length.
    iu_patch=''
    iu_pt="$IU_GOOD_BASE"	# Computational helper.

    if test `expr X"$iu_pt$iu_eighty" : X".*"` -le $iu_socklen_max; then
	iu_patch="$iu_patch$iu_eighty" && iu_pt="$iu_pt$iu_eighty"
    fi

    count=`expr X"$iu_pt" : X".*"`
    count=`expr $iu_socklen_max - $count`

    # $count gives the number, and $iu_eighty the characters.
    if test $count -gt 0; then
	iu_patch="$iu_patch`expr X"$iu_eighty" : X"\(.\{1,$count\}\)"`"
    fi

    IU_LONG_SOCKET="$IU_GOOD_BASE$iu_patch"
    IU_EXCESSIVE_SOCKET="$IU_BAD_BASE$iu_patch"
fi

# All messages intended for post-detection are
# to be uniformly tagged.
TAG="syslogd-test"

# Remove old files in use by daemon.
rm -f "$OUT" "$PID" "$CONF"

# Full testing at the standard port needs a superuser.
# Randomise if necessary to get an underprivileged port.
test `func_id_uid` = 0 || do_standard_port=false

if test `func_id_uid` != 0 && test $PORT -le 1023; then
    $silence cat <<-EOT >&2
	WARNING!!  The preset port $PORT/$PROTO is not usable,
	since you are underprivileged.  Now attempting
	a randomised higher port.
	EOT
    PORT=`expr $PORT + 3917 + ${RANDOM:-$$} % 2711`
fi

# Is the INET port already in use? If so,
# randomise somewhat.
if locate_port $PROTO $PORT; then
    echo "Port $PORT/$PROTO is in use. Randomising port somewhat." >&2
    PORT=`expr $PORT + 2711 + ${RANDOM:-$$} % 917`
fi

# Test a final time.
if locate_port $PROTO $PORT; then
    cat <<-EOT >&2
	The INET port $PORT/$PROTO is already in use.
	Skipping test of INET socket this time.
	EOT
    do_inet_socket=false
fi

# A minimal, catch-all configuration.
#
cat > "$CONF" <<-EOT
	*.*	$OUT
	# Test incorrect forwarding.
	*.*	@not.in.existence
	# Recover from missing action field and short selector.
	12345
	*.*
	*.	/dev/null
EOT

# Add a user recipient in verbose mode.
$silence false || echo "*.*	$USER" >> "$CONF"

# Set REMOTE_LOGHOST to activate forwarding
#
if [ -n "$REMOTE_LOGHOST" ]; then
    # Append a forwarding stanza.
    cat >> "$CONF" <<-EOT
	# Forwarding remotely
	*.*	@$REMOTE_LOGHOST
	EOT
fi

# Attempt to start the server after first
# building the desired option list.
#
## Base configuration.
IU_OPTIONS="--rcfile='$CONF' --pidfile='$PID'"
if $do_unix_socket; then
    IU_OPTIONS="$IU_OPTIONS --socket='$SOCKET'"
else
    # The empty string will disable the standard socket.
    IU_OPTIONS="$IU_OPTIONS --socket=''"
fi
if $do_socket_length; then
    IU_OPTIONS="$IU_OPTIONS -a '$IU_LONG_SOCKET' -a '$IU_EXCESSIVE_SOCKET'"
fi

## Enable INET service when possible.
if $do_inet_socket; then
    IU_OPTIONS="$IU_OPTIONS --ipany --inet -B$PORT --hop"
fi
## Bring in additional options from command line.
## Disable kernel messages otherwise.
if [ -c /dev/klog ]; then
    : OPTIONS=${OPTIONS:=--no-klog}
fi
IU_OPTIONS="$IU_OPTIONS $OPTIONS"

# The eval-construct allows white space in file names,
# based on the use of single quotes in IU_OPTIONS.
eval $SYSLOGD $IU_OPTIONS

# Wait a moment in order to avoid an obvious
# race condition with the server daemon on
# slow systems.
#
sleep 1

# Test to see whether the service got started.
#
if [ ! -r "$PID" ]; then
    echo "The service daemon never started.  Failing." >&2
    exit 1
fi

# Declare the number of implemented tests,
# as well as an exit code.
#
TESTCASES=0
SUCCESSES=0
EXITCODE=1

# Check that the excessively long UNIX socket name was rejected.
if $do_socket_length; then
    TESTCASES=`expr $TESTCASES + 1`
    # Messages can be truncated in the message log, so make a best
    # effort to limit the length of the string we are searching for.
    # Allowing 55 characters for IU_BAD_BASE is almost aggressive.
    # A host name of length six would allow 64 characters
    pruned=`expr "UNIX socket name too long.*${IU_BAD_BASE}" : '\(.\{1,82\}\)'`
    if $GREP "$pruned" "$OUT" >/dev/null 2>&1; then
	SUCCESSES=`expr $SUCCESSES + 1`
    fi
fi

# Send messages on two sockets: IPv4 and UNIX.
#
if $do_unix_socket; then
    TESTCASES=`expr $TESTCASES + 1`
    $LOGGER -h "$SOCKET" -p user.info -t "$TAG" \
	"Sending BSD message. (pid $$)"
fi

if $do_socket_length; then
    TESTCASES=`expr $TESTCASES + 1`
    $LOGGER -h "$IU_LONG_SOCKET" -p user.info \
	-t "$TAG" "Sending via long socket name. (pid $$)"
fi

if $do_inet_socket; then
    TESTCASES=`expr $TESTCASES + 2`
    $LOGGER -4 -h "$TARGET:$PORT" -p user.info -t "$TAG" \
	"Sending IPv4 message. (pid $$)"
    $LOGGER -6 -h "$TARGET6:$PORT" -p user.info -t "$TAG" \
	"Sending IPv6 message. (pid $$)"
fi

# Generate a more elaborate message routing, aimed at confirming
# discrimination of severity and facility.  This is made active
# by sending SIGHUP to the server process.
#
OUT_USER="$IU_TESTDIR"/user.log
OUT_DEBUG="$IU_TESTDIR"/debug.log

# Create the new files to avoid false negatives.
: > "$OUT_USER"
: > "$OUT_DEBUG"

cat > "$CONF" <<-EOT
	*.*;user.none	$OUT
	user.info	$OUT_USER
	*.=debug	$OUT_DEBUG
EOT

# Use another tag for better discrimination.
TAG2="syslogd-reload-test"

# Load the new configuration
kill -HUP `cat "$PID"`

if $do_unix_socket; then
    # Two messages, but absence is also awarded credit.
    TESTCASES=`expr $TESTCASES + 4`
    $LOGGER -h "$SOCKET" -p user.info -t "$TAG2" \
	"user.info as BSD message. (pid $$)"
    $LOGGER -h "$SOCKET" -p user.debug -t "$TAG2" \
	"user.debug as BSD message. (pid $$)"
fi

if $do_inet_socket; then
    # Two messages, but absence is also awarded credit.
    TESTCASES=`expr $TESTCASES + 4`
    $LOGGER -4 -h "$TARGET:$PORT" -p user.info -t "$TAG2" \
	"user.info IPv4 message. (pid $$)"
    $LOGGER -4 -h "$TARGET:$PORT" -p user.debug -t "$TAG2" \
	"user.debug as IPv4 message. (pid $$)"
fi

# Remove previous SYSLOG daemon.
test -r "$PID" && kill -0 "`cat "$PID"`" >/dev/null 2>&1 &&
    kill "`cat "$PID"`"

# Check functionality of standard port, i.e., execution
# where neither syslogd nor logger have been instructed
# to use specific ports for inet sockets.
#
locate_port $PROTO 514 && do_standard_port=false

if $do_standard_port; then
    echo 'Checking also standard port 514/udp.' >&2
    TESTCASES=`expr $TESTCASES + 2`

    # New configuration, but continuing old message file.
    rm -f "$PID"
    cat > "$CONF" <<-EOT
	*.*	$OUT
	EOT

    # Only INET socket, no UNIX socket.
    $SYSLOGD --rcfile="$CONF" --pidfile="$PID" --socket='' \
	--inet --ipany $OPTIONS
    $LOGGER -4 -h "$TARGET" -p user.info -t "$TAG" \
	"IPv4 to standard port. (pid $$)"
    $LOGGER -6 -h "$TARGET6" -p user.info -t "$TAG" \
	"IPv6 to standard port. (pid $$)"
fi

# Delay detection due to observed race condition.
sleep 1

# Detection of registered messages.
#
COUNT=`$GREP -c "$TAG" "$OUT"`
COUNT2=`$GREP -c "$TAG2" "$OUT_USER"`
COUNT2_debug=`$GREP -c "$TAG2.*user.debug" "$OUT_USER"`
COUNT3=`$GREP -c "$TAG2" "$OUT_DEBUG"`
COUNT3_info=`$GREP -c "$TAG2.*user.info" "$OUT_DEBUG"`
SUCCESSES=`expr $SUCCESSES + $COUNT + 2 \* $COUNT2 - $COUNT2_debug \
		+ 2 \* $COUNT3 - $COUNT3_info`

if [ -n "${VERBOSE+yes}" ]; then
    cat <<-EOT
	---------- Successfully detected messages. ----------
	`$GREP "$TAG" "$OUT"`
	`$GREP -h "$TAG2" "$OUT_USER" "$OUT_DEBUG"`
	---------- Full message log for syslogd. ------------
	`cat "$OUT"`
	---------- User message log. ------------------------
	`cat "$OUT_USER"`
	---------- Debug message log. -----------------------
	`cat "$OUT_DEBUG"`
	-----------------------------------------------------
	EOT
fi

test $SUCCESSES -eq $TESTCASES && $silence false \
    || echo "Registered $SUCCESSES successes out of $TESTCASES."

# Report incomplete test setup.
$do_inet_socket ||
    echo 'NOTICE: Port specified INET socket test was not run' >&2
$do_standard_port ||
    echo 'NOTICE: Standard port test was not run.' >&1

if [ "$SUCCESSES" -eq "$TESTCASES" ]; then
    $silence echo "Successful testing."
    EXITCODE=0
else
    $silence echo "Failing some tests."
fi

# Remove the daemon process.
test -r "$PID" && kill -0 "`cat "$PID"`" >/dev/null 2>&1 &&
    kill "`cat "$PID"`"

exit $EXITCODE
