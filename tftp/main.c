/*
  Copyright (C) 1995, 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003,
  2004, 2005, 2006, 2007, 2008, 2009 Free Software Foundation, Inc.

  This file is part of GNU Inetutils.

  GNU Inetutils is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or (at
  your option) any later version.

  GNU Inetutils is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see `http://www.gnu.org/licenses/'. */

/*
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* Many bug fixes are from Jim Guyton <guyton@rand-unix> */

/*
 * TFTP User Program -- Command Interface.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/file.h>

#include <netinet/in.h>

#include <arpa/inet.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <argp.h>
#include <libinetutils.h>

#include "xalloc.h"
#include "extern.h"
#include "progname.h"

#define TIMEOUT		5	/* secs between rexmt's */

struct sockaddr_in peeraddr;
int f;
short port;
int trace;
int verbose;
int connected;
char mode[32];
char line[200];
int margc;
char *margv[20];
char *prompt = "tftp";
jmp_buf toplevel;
void intr ();
struct servent *sp;

void get (int, char **);
void help (int, char **);
void modecmd (int, char **);
void put (int, char **);
void quit (int, char **);
void setascii (int, char **);
void setbinary (int, char **);
void setpeer (int, char **);
void setrexmt (int, char **);
void settimeout (int, char **);
void settrace (int, char **);
void setverbose (int, char **);
void status (int, char **);

static void command (void);

static void getusage (char *);
static void makeargv (void);
static void putusage (char *);
static void settftpmode (char *);

#define HELPINDENT (sizeof("connect"))

struct cmd
{
  char *name;
  char *help;
  void (*handler) (int, char **);
};

char vhelp[] = "toggle verbose mode";
char thelp[] = "toggle packet tracing";
char chelp[] = "connect to remote tftp";
char qhelp[] = "exit tftp";
char hhelp[] = "print help information";
char shelp[] = "send file";
char rhelp[] = "receive file";
char mhelp[] = "set file transfer mode";
char sthelp[] = "show current status";
char xhelp[] = "set per-packet retransmission timeout";
char ihelp[] = "set total retransmission timeout";
char ashelp[] = "set mode to netascii";
char bnhelp[] = "set mode to octet";

struct cmd cmdtab[] = {
  {"connect", chelp, setpeer},
  {"mode", mhelp, modecmd},
  {"put", shelp, put},
  {"get", rhelp, get},
  {"quit", qhelp, quit},
  {"verbose", vhelp, setverbose},
  {"trace", thelp, settrace},
  {"status", sthelp, status},
  {"binary", bnhelp, setbinary},
  {"ascii", ashelp, setascii},
  {"rexmt", xhelp, setrexmt},
  {"timeout", ihelp, settimeout},
  {"?", hhelp, help},
  {0}
};

struct cmd *getcmd ();
char *tail ();


const char args_doc[] = "[HOST [PORT]]";
const char doc[] = "Trivial file transfer protocol client";

static struct argp_option argp_options[] = {
  {"verbose", 'v', NULL, 0, "verbose output"},
  {NULL}
};

char *hostport_argv[3] = { "connect" };
int hostport_argc = 1;

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'v':		/* Verbose.  */
      verbose++;
      break;

    case ARGP_KEY_ARG:
      if (state->arg_num >= 2 || hostport_argc >= 3)
	/* Too many arguments. */
	argp_usage (state);
      hostport_argv[hostport_argc++] = arg;
      break;

    default:
      return ARGP_ERR_UNKNOWN;
    }

  return 0;
}

static struct argp argp = {argp_options, parse_opt, args_doc, doc};

int
main (int argc, char *argv[])
{
  struct sockaddr_in sin;

  set_program_name (argv[0]);
  iu_argp_init ("tftp", default_program_authors);
  argp_parse (&argp, argc, argv, 0, NULL, NULL);

  sp = getservbyname ("tftp", "udp");
  if (sp == 0)
    {
      fprintf (stderr, "tftp: udp/tftp: unknown service\n");
      exit (1);
    }
  f = socket (AF_INET, SOCK_DGRAM, 0);
  if (f < 0)
    {
      perror ("tftp: socket");
      exit (3);
    }
  memset (&sin, 0, sizeof (sin));
  sin.sin_family = AF_INET;
  if (bind (f, (struct sockaddr *) &sin, sizeof (sin)) < 0)
    {
      perror ("tftp: bind");
      exit (1);
    }
  strcpy (mode, "netascii");
  signal (SIGINT, intr);
  if (hostport_argc > 1)
    {
      if (setjmp (toplevel) != 0)
	exit (0);
      setpeer (hostport_argc, hostport_argv);
    }
  if (setjmp (toplevel) != 0)
    putchar ('\n');
  command ();
}

char *hostname;

#define RESOLVE_OK            0
#define RESOLVE_FAIL         -1
#define RESOLVE_NOT_RESOLVED  1

/* Resolve NAME. Fill in peeraddr, hostname and set connected on success.
   Return value: RESOLVE_OK success
                 RESOLVE_FAIL error
		 RESOLVE_NOT_RESOLVED name is not resolved and ALLOW_NULL
		 is true */
static int
resolve_name (char *name, int allow_null)
{
  struct hostent *hp = gethostbyname (name);
  if (hp == NULL)
    {
      if (allow_null)
	return RESOLVE_NOT_RESOLVED;
      fprintf (stderr, "tftp: %s: ", name);
      herror ((char *) NULL);
      return RESOLVE_FAIL;
    }
  else if (hp->h_length != sizeof peeraddr.sin_addr)
    {
      fprintf (stderr, "tftp: resolving %s returns unexpected length", name);
      return RESOLVE_FAIL;
    }
  memcpy (&peeraddr.sin_addr, hp->h_addr, hp->h_length);
  peeraddr.sin_family = hp->h_addrtype;
  connected = 1;
  free (hostname);
  hostname = xstrdup (hp->h_name);
  return RESOLVE_OK;
}

/* Prompt for more arguments from the user with PROMPT, putting the results
   into ARGC & ARGV, with an initial argument of ARG0.  Global variables
   LINE, MARGC, and MARGV are changed.  */
static void
get_args (char *arg0, char *prompt, int *argc, char ***argv)
{
  size_t arg0_len = strlen (arg0);

  strcpy (line, arg0);
  strcat (line, " ");

  printf ("%s", prompt);
  fgets (line + arg0_len + 1, sizeof line - arg0_len - 1, stdin);

  makeargv ();
  *argc = margc;
  *argv = margv;
}

void
setpeer (int argc, char *argv[])
{
  if (argc < 2)
    get_args ("Connect", "(to) ", &argc, &argv);

  if (argc < 2 || argc > 3)
    {
      printf ("usage: %s host-name [port]\n", argv[0]);
      return;
    }

  switch (resolve_name (argv[1], 1))
    {
    case RESOLVE_OK:
      break;

    case RESOLVE_FAIL:
      return;

    case RESOLVE_NOT_RESOLVED:
      peeraddr.sin_family = AF_INET;
      peeraddr.sin_addr.s_addr = inet_addr (argv[1]);
      if (peeraddr.sin_addr.s_addr == -1)
	{
	  connected = 0;
	  printf ("%s: unknown host\n", argv[1]);
	  return;
	}
      hostname = xstrdup (argv[1]);
    }

  port = sp->s_port;
  if (argc == 3)
    {
      port = atoi (argv[2]);
      if (port < 0)
	{
	  printf ("%s: bad port number\n", argv[2]);
	  connected = 0;
	  return;
	}
      port = htons (port);
    }
  connected = 1;
}

struct modes
{
  char *m_name;
  char *m_mode;
} modes[] =
  {
    {"ascii", "netascii"},
    {"netascii", "netascii"},
    {"binary", "octet"},
    {"image", "octet"},
    {"octet", "octet"},
    {0, 0}
  };

void
modecmd (int argc, char *argv[])
{
  register struct modes *p;
  char *sep;

  if (argc < 2)
    {
      printf ("Using %s mode to transfer files.\n", mode);
      return;
    }
  if (argc == 2)
    {
      for (p = modes; p->m_name; p++)
	if (strcmp (argv[1], p->m_name) == 0)
	  break;
      if (p->m_name)
	{
	  settftpmode (p->m_mode);
	  return;
	}
      printf ("%s: unknown mode\n", argv[1]);
      /* drop through and print usage message */
    }

  printf ("usage: %s [", argv[0]);
  sep = " ";
  for (p = modes; p->m_name; p++)
    {
      printf ("%s%s", sep, p->m_name);
      if (*sep == ' ')
	sep = " | ";
    }
  printf (" ]\n");
  return;
}

void
setbinary (int argc, char *argv[])
{
  settftpmode ("octet");
}

void
setascii (int argc, char *argv[])
{
  settftpmode ("netascii");
}

static void
settftpmode (char *newmode)
{
  strcpy (mode, newmode);
  if (verbose)
    printf ("mode set to %s\n", mode);
}

/*
 * Send file(s).
 */
void
put (int argc, char *argv[])
{
  int fd;
  register int n;
  register char *cp, *targ;

  if (argc < 2)
    get_args ("send", "(file) ", &argc, &argv);

  if (argc < 2)
    {
      putusage (argv[0]);
      return;
    }
  targ = argv[argc - 1];
  if (strchr (argv[argc - 1], ':'))
    {
      char *cp;

      for (n = 1; n < argc - 1; n++)
	if (strchr (argv[n], ':'))
	  {
	    putusage (argv[0]);
	    return;
	  }
      cp = argv[argc - 1];
      targ = strchr (cp, ':');
      *targ++ = 0;
      if (resolve_name (cp, 0) != RESOLVE_OK)
	return;
    }
  if (!connected)
    {
      printf ("No target machine specified.\n");
      return;
    }
  if (argc < 4)
    {
      cp = argc == 2 ? tail (targ) : argv[1];
      fd = open (cp, O_RDONLY);
      if (fd < 0)
	{
	  fprintf (stderr, "tftp: ");
	  perror (cp);
	  return;
	}
      if (verbose)
	printf ("putting %s to %s:%s [%s]\n", cp, hostname, targ, mode);
      peeraddr.sin_port = port ? port : sp->s_port;
      send_file (fd, targ, mode);
      return;
    }
  /* this assumes the target is a directory */
  /* on a remote unix system.  hmmmm.  */
  cp = strchr (targ, '\0');
  *cp++ = '/';
  for (n = 1; n < argc - 1; n++)
    {
      strcpy (cp, tail (argv[n]));
      fd = open (argv[n], O_RDONLY);
      if (fd < 0)
	{
	  fprintf (stderr, "tftp: ");
	  perror (argv[n]);
	  continue;
	}
      if (verbose)
	printf ("putting %s to %s:%s [%s]\n", argv[n], hostname, targ, mode);
      peeraddr.sin_port = port ? port : sp->s_port;
      send_file (fd, targ, mode);
    }
}

static void
putusage (char *s)
{
  printf ("usage: %s file ... host:target, or\n", s);
  printf ("       %s file ... target (when already connected)\n", s);
}

/*
 * Receive file(s).
 */
void
get (int argc, char *argv[])
{
  int fd;
  register int n;
  register char *cp;
  char *src;

  if (argc < 2)
    get_args ("get", "(files) ", &argc, &argv);

  if (argc < 2)
    {
      getusage (argv[0]);
      return;
    }
  if (!connected)
    {
      for (n = 1; n < argc; n++)
	if (strchr (argv[n], ':') == 0)
	  {
	    getusage (argv[0]);
	    return;
	  }
    }
  for (n = 1; n < argc; n++)
    {
      src = strchr (argv[n], ':');
      if (src == NULL)
	src = argv[n];
      else
	{
	  *src++ = 0;
	  if (resolve_name (argv[n], 0) != RESOLVE_OK)
	    continue;
	}

      if (argc < 4)
	{
	  cp = argc == 3 ? argv[2] : tail (src);
	  fd = creat (cp, 0644);
	  if (fd < 0)
	    {
	      fprintf (stderr, "tftp: ");
	      perror (cp);
	      return;
	    }
	  if (verbose)
	    printf ("getting from %s:%s to %s [%s]\n",
		    hostname, src, cp, mode);
	  peeraddr.sin_port = port ? port : sp->s_port;
	  recvfile (fd, src, mode);
	  break;
	}
      cp = tail (src);		/* new .. jdg */
      fd = creat (cp, 0644);
      if (fd < 0)
	{
	  fprintf (stderr, "tftp: ");
	  perror (cp);
	  continue;
	}
      if (verbose)
	printf ("getting from %s:%s to %s [%s]\n", hostname, src, cp, mode);
      peeraddr.sin_port = port ? port : sp->s_port;
      recvfile (fd, src, mode);
    }
}

static void
getusage (char *s)
{
  printf ("usage: %s host:file host:file ... file, or\n", s);
  printf ("       %s file file ... file if connected\n", s);
}

int rexmtval = TIMEOUT;

void
setrexmt (int argc, char *argv[])
{
  int t;

  if (argc < 2)
    get_args ("Rexmt-timeout", "(value) ", &argc, &argv);

  if (argc != 2)
    {
      printf ("usage: %s value\n", argv[0]);
      return;
    }
  t = atoi (argv[1]);
  if (t < 0)
    printf ("%s: bad value\n", argv[1]);
  else
    rexmtval = t;
}

int maxtimeout = 5 * TIMEOUT;

void
settimeout (int argc, char *argv[])
{
  int t;

  if (argc < 2)
    get_args ("Maximum-timeout", "(value) ", &argc, &argv);

  if (argc != 2)
    {
      printf ("usage: %s value\n", argv[0]);
      return;
    }
  t = atoi (argv[1]);
  if (t < 0)
    printf ("%s: bad value\n", argv[1]);
  else
    maxtimeout = t;
}

void
status (int argc, char *argv[])
{
  if (connected)
    printf ("Connected to %s.\n", hostname);
  else
    printf ("Not connected.\n");
  printf ("Mode: %s Verbose: %s Tracing: %s\n", mode,
	  verbose ? "on" : "off", trace ? "on" : "off");
  printf ("Rexmt-interval: %d seconds, Max-timeout: %d seconds\n",
	  rexmtval, maxtimeout);
}

void
intr ()
{
  signal (SIGALRM, SIG_IGN);
  alarm (0);
  longjmp (toplevel, -1);
}

char *
tail (char *filename)
{
  register char *s;

  while (*filename)
    {
      s = strrchr (filename, '/');
      if (s == NULL)
	break;
      if (s[1])
	return (s + 1);
      *s = '\0';
    }
  return filename;
}

/*
 * Command parser.
 */
static void
command ()
{
  register struct cmd *c;

  for (;;)
    {
      printf ("%s> ", prompt);
      if (fgets (line, sizeof line, stdin) == 0)
	{
	  if (feof (stdin))
	    exit (0);
	  else
	    continue;
	}
      if (line[0] == 0)
	continue;
      makeargv ();
      if (margc == 0)
	continue;
      c = getcmd (margv[0]);
      if (c == (struct cmd *) -1)
	{
	  printf ("?Ambiguous command\n");
	  continue;
	}
      if (c == 0)
	{
	  printf ("?Invalid command\n");
	  continue;
	}
      (*c->handler) (margc, margv);
    }
}

struct cmd *
getcmd (register char *name)
{
  register char *p, *q;
  register struct cmd *c, *found;
  register int nmatches, longest;

  longest = 0;
  nmatches = 0;
  found = 0;
  for (c = cmdtab; (p = c->name) != NULL; c++)
    {
      for (q = name; *q == *p++; q++)
	if (*q == 0)		/* exact match? */
	  return (c);

      if (!*q)
	{			/* the name was a prefix */
	  if (q - name > longest)
	    {
	      longest = q - name;
	      nmatches = 1;
	      found = c;
	    }
	  else if (q - name == longest)
	    nmatches++;
	}
    }
  if (nmatches > 1)
    return (struct cmd *) -1;
  return found;
}

/*
 * Slice a string up into argc/argv.
 */
static void
makeargv ()
{
  register char *cp;
  register char **argp = margv;

  margc = 0;
  for (cp = line; *cp;)
    {
      while (isspace (*cp))
	cp++;
      if (*cp == '\0')
	break;
      *argp++ = cp;
      margc += 1;
      while (*cp != '\0' && !isspace (*cp))
	cp++;
      if (*cp == '\0')
	break;
      *cp++ = '\0';
    }
  *argp++ = 0;
}

void
quit (int argc, char *argv[])
{
  exit (0);
}

/*
 * Help command.
 */
void
help (int argc, char *argv[])
{
  register struct cmd *c;

  if (argc == 1)
    {
      printf ("Commands may be abbreviated.  Commands are:\n\n");
      for (c = cmdtab; c->name; c++)
	printf ("%-*s\t%s\n", (int) HELPINDENT, c->name, c->help);
      return;
    }

  while (--argc > 0)
    {
      register char *arg;

      arg = *++argv;
      c = getcmd (arg);
      if (c == (struct cmd *) -1)
	printf ("?Ambiguous help command %s\n", arg);
      else if (c == (struct cmd *) 0)
	printf ("?Invalid help command %s\n", arg);
      else
	printf ("%s\n", c->help);
    }
}

void
settrace (int argc, char **argv)
{
  trace = !trace;
  printf ("Packet tracing %s.\n", trace ? "on" : "off");
}

void
setverbose (int argc, char **argv)
{
  verbose = !verbose;
  printf ("Verbose mode %s.\n", verbose ? "on" : "off");
}
