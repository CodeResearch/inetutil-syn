/*
 * Copyright (c) 1983, 1988, 1993, 1994
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
 * 4. Neither the name of the University nor the names of its contributors
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

#ifndef lint
static char copyright[] =
"@(#) Copyright (c) 1983, 1988, 1993, 1994\n\
	The Regents of the University of California.  All rights reserved.\n";
#endif /* not lint */

#ifndef lint
static char sccsid[] = "@(#)syslogd.c	8.3 (Berkeley) 4/4/94";
#endif /* not lint */

/*
 *  syslogd -- log system messages
 *
 * This program implements a system log. It takes a series of lines.
 * Each line may have a priority, signified as "<n>" as
 * the first characters of the line.  If this is
 * not present, a default priority is used.
 *
 * To kill syslogd, send a signal 15 (terminate).  A signal 1 (hup) will
 * cause it to reread its configuration file.
 *
 * Defined Constants:
 *
 * MAXLINE -- the maximum line length that can be handled.
 * DEFUPRI -- the default priority for user messages
 * DEFSPRI -- the default priority for kernel messages
 *
 * Author: Eric Allman
 * extensive changes by Ralph Campbell
 * more extensive changes by Eric Allman (again)
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define	MAXLINE		1024		/* maximum line length */
#define	MAXSVLINE	240		/* maximum saved line length */
#define DEFUPRI		(LOG_USER|LOG_NOTICE)
#define DEFSPRI		(LOG_KERN|LOG_CRIT)
#define TIMERINTVL	30		/* interval for checking flush, mark */

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_MSGBUF_H
#include <sys/msgbuf.h>
#endif
#include <sys/uio.h>
#include <sys/un.h>
#ifdef TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#include <sys/resource.h>

#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utmp.h>
#include <getopt.h>
#define SYSLOG_NAMES
#include <syslog.h>
#ifndef HAVE_SYSLOG_INTERNAL
#include <syslog-int.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include <version.h>

char	*LogName = PATH_LOG;
char	*ConfFile = PATH_LOGCONF;
char	*PidFile = PATH_LOGPID;
char	ctty[] = PATH_CONSOLE;

static int debugging_on = 0;

#define FDMASK(fd)	(1 << (fd))

#ifndef LINE_MAX
#define LINE_MAX 2048
#endif

/*
 * Flags to logmsg().
 */

#define IGN_CONS	0x001	/* don't print on console */
#define SYNC_FILE	0x002	/* do fsync on file after printing */
#define ADDDATE		0x004	/* add a date to the message */
#define MARK		0x008	/* this message is a mark */

/*
 * This structure represents the files that will have log
 * copies printed.
 */

struct filed {
	struct	filed *f_next;		/* next in linked list */
	short	f_type;			/* entry type, see below */
	short	f_file;			/* file descriptor */
	time_t	f_time;			/* time this was last written */
	u_char	f_pmask[LOG_NFACILITIES+1];	/* priority mask */
	union {
		struct {
			int f_nusers;
			char **f_unames;
		} f_user;
		struct {
			char	*f_hname;
			struct sockaddr_in	f_addr;
		} f_forw;		/* forwarding address */
		char	*f_fname;
	} f_un;
	char	f_prevline[MAXSVLINE];		/* last message logged */
	char	f_lasttime[16];			/* time of last occurrence */
	char	*f_prevhost;			/* host from which recd. */
	int	f_prevpri;			/* pri of f_prevline */
	int	f_prevlen;			/* length of f_prevline */
	int	f_prevcount;			/* repetition cnt of prevline */
	int	f_repeatcount;			/* number of "repeated" msgs */
	int	f_flags;			/* additional flags see below */
};

/*
 * Flags in filed.f_flags.
 */

#define OMIT_SYNC	0x001	/* omit fsync after printing */

/*
 * Intervals at which we flush out "message repeated" messages,
 * in seconds after previous message is logged.  After each flush,
 * we move to the next interval until we reach the largest.
 */
int	repeatinterval[] = { 30, 120, 600 };	/* # of secs before flush */
#define	MAXREPEAT ((sizeof(repeatinterval) / sizeof(repeatinterval[0])) - 1)
#define	REPEATTIME(f)	((f)->f_time + repeatinterval[(f)->f_repeatcount])
#define	BACKOFF(f)	{ if (++(f)->f_repeatcount > MAXREPEAT) \
				 (f)->f_repeatcount = MAXREPEAT; \
			}
/*
 * Constants for F_FORW_UNKN retry feature.
 */
#define INET_SUSPEND_TIME 180		/* equal to 3 minutes */
#define INET_RETRY_MAX 10		/* maximum of retries for gethostbyname() */

#define LIST_DELIMITER ':'		/* delimiter between two hosts */

/* values for f_type */
#define F_UNUSED	0		/* unused entry */
#define F_FILE		1		/* regular file */
#define F_TTY		2		/* terminal */
#define F_CONSOLE	3		/* console terminal */
#define F_FORW		4		/* remote machine */
#define F_USERS		5		/* list of users */
#define F_WALL		6		/* everyone logged on */
#define F_FORW_SUSP	7		/* suspended host forwarding */
#define F_FORW_UNKN	8		/* unknown host forwarding */
#define F_PIPE		9		/* named pipe */

char	*TypeNames[] = {
	"UNUSED",	"FILE",		"TTY",		"CONSOLE",
	"FORW",		"USERS",	"WALL",		"FORW(SUSPENDED)",
	"FORW(UNKNOWN)","PIPE"
};

struct	filed *Files;
struct	filed consfile;

int	Debug;			/* debug flag */
char	*LocalHostName = 0;	/* our hostname */
char	*LocalDomain;		/* our local domain name */
int	InetInuse = 0;		/* non-zero if INET sockets are being used */
int	finet;			/* Internet datagram socket */
int	LogPort;		/* port number for INET connections */
int	Initialized = 0;	/* set when we have initialized ourselves */
int	MarkInterval = 20 * 60;	/* interval between marks in seconds */
int	MarkSeq = 0;		/* mark sequence number */
int     AcceptRemote = 0;       /* receive messages that come via UDP */
char    **StripDomains = NULL;  /* these domains may be stripped before writing
logs */
char    **LocalHosts = NULL;    /* these hosts are logged with their hostname */

int	NoDetach = 0;		/* Don't run in background and detach from ctty. */
int	NoHops = 1;		/* non-zero if we don't bounce syslog messages
				   for other hosts */

void	cfline __P((char *, struct filed *));
const char *cvthname __P((struct sockaddr_in *));
int	decode __P((const char *, CODE *));
void	die __P((int));
void	domark __P((int));
void	fprintlog __P((struct filed *, const char *, int, const char *));
void	init __P((int));
void	logerror __P((const char *));
void	logmsg __P((int, const char *, const char *, int));
void	printline __P((const char *, const char *));
void	printsys __P((const char *));
void	reapchild __P((int));
char   *ttymsg __P((struct iovec *, int, char *, int));
void	usage __P((void));
void	wallmsg __P((struct filed *, struct iovec *));
extern char *localhost __P ((void));
char **crunch_list __P((char *list));
void    debug_switch __P(());
#if defined(__GLIBC__)
#define dprintf mydprintf
#endif /* __GLIBC__ */
static void dprintf __P((char *, ...));

int
main(int argc, char *argv[])
{
	int ch, funix, i, inetm, fklog, klogm;
	size_t len;
	struct sockaddr_un sunx, fromunix;
	struct sockaddr_in sin, frominet;
	FILE *fp;
	char *p;
#ifdef MSG_BSIZE
	char line[MSG_BSIZE + 1];
#else
	char line[MAXLINE + 1];
#endif

	while ((ch = getopt(argc, argv, "dhf:l:m:np:rsV")) != EOF)
		switch(ch) {
		case 'd':		/* debug */
			Debug++;
			break;
		case 'f':		/* configuration file */
			ConfFile = optarg;
			break;
		case 'h':
			NoHops = 0;
			break;
		case 'm':		/* mark interval */
			MarkInterval = atoi(optarg) * 60;
			break;
		case 'n':		/* don't run in the background */
			NoDetach = 1;
		case 'p':		/* path */
			LogName = optarg;
			break;
		case 'r':		/* accept remote messages */
			AcceptRemote = 1;
			break;
		case 'V':
			printf("syslogd (%s) %s\n", inetutils_package,
			       inetutils_version);
			exit (0);
		case 'l':
			if (LocalHosts) {
				fprintf (stderr, "Only one -l argument allowed," \
					"the first one is taken.\n");
				break;
			}
			LocalHosts = crunch_list(optarg);
			break;
		case 's':
			if (StripDomains) {
				fprintf (stderr, "Only one -s argument allowed," \
					"the first one is taken.\n");
				break;
			}
			StripDomains = crunch_list(optarg);
			break; 
		case '?':
		default:
			usage();
		}
	if ((argc -= optind) != 0)
		usage();

	if (!(Debug || NoDetach))
		(void)daemon(0, 0);
	else
	{
	  debugging_on = 1;
#ifdef HAVE_SETLINEBUF
	  setlinebuf (stdout);
#else
#ifndef SETVBUF_REVERSED
	  setvbuf (stdout, (char *) 0, _IOLBF, BUFSIZ);
#else /* setvbuf not reversed.  */
 /* Some buggy systems lose if we pass 0 instead of allocating ourselves.  */
	  setvbuf (stdout, _IOLBF, xmalloc (BUFSIZ), BUFSIZ);
#endif /* setvbuf reversed.  */
#endif /* setlinebuf missing.  */
	}

	LocalHostName = localhost ();
	if (! LocalHostName) {
		perror ("Can't get local host name");
		exit (2);
	}

	if ((p = strchr(LocalHostName, '.')) != NULL) {
		*p++ = '\0';
		LocalDomain = p;
	} else {
		struct hostent *host_ent;

		LocalDomain = "";
		host_ent = gethostbyname(LocalHostName);
		if (host_ent)
			LocalHostName = strdup(host_ent->h_name);
		if ( (p = strchr(LocalHostName, '.')) )
		{
			*p++ = '\0';
			LocalDomain = p;
		}
	}

	for (p = (char *)LocalDomain; *p ; p++)
		if (isupper(*p))
			*p = tolower(*p);

	consfile.f_type = F_CONSOLE;
	consfile.f_un.f_fname = strdup (ctty);

	(void)signal(SIGTERM, die);
	(void)signal(SIGINT, Debug ? die : SIG_IGN);
	(void)signal(SIGQUIT, Debug ? die : SIG_IGN);
	(void)signal(SIGCHLD, reapchild);
	(void)signal(SIGALRM, domark);
	(void)signal(SIGUSR1, Debug ? debug_switch : SIG_IGN);
	(void)alarm(TIMERINTVL);
	(void)unlink(LogName);

#ifndef SUN_LEN
#define SUN_LEN(unp) (strlen((unp)->sun_path) + 3)
#endif
	memset(&sunx, 0, sizeof(sunx));
	sunx.sun_family = AF_UNIX;
	(void)strncpy(sunx.sun_path, LogName, sizeof(sunx.sun_path));
	funix = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (funix < 0 ||
	    bind(funix, (struct sockaddr *)&sunx, SUN_LEN(&sunx)) < 0 ||
	    chmod(LogName, 0666) < 0) {
		snprintf (line, sizeof line, "cannot create %s", LogName);
		logerror(line);
		dprintf("cannot create %s (%d)\n", LogName, errno);
		die(0);
	}
	finet = socket(AF_INET, SOCK_DGRAM, 0);
	inetm = 0;
	if (finet >= 0) {
		struct servent *sp;

		sp = getservbyname("syslog", "udp");
		if (sp == NULL) {
			errno = 0;
			logerror("syslog/udp: unknown service");
			die(0);
		}
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = LogPort = sp->s_port;
		if (bind(finet, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
			logerror("bind");
			if (!Debug)
				die(0);
		} else {
			inetm = FDMASK(finet);
			InetInuse = 1;
		}
	}

#ifdef PATH_KLOG
	if ((fklog = open(PATH_KLOG, O_RDONLY, 0)) >= 0)
		klogm = FDMASK(fklog);
	else {
		dprintf("can't open %s (%d)\n", PATH_KLOG, errno);
		klogm = 0;
	}
#else
	klogm = 0;
#endif

	/* tuck my process id away */
	fp = fopen(PidFile, "w");
	if (fp != NULL) {
		fprintf(fp, "%d\n", getpid());
		(void) fclose(fp);
	}

	dprintf("off & running....\n");

	init(0);
	(void)signal(SIGHUP, init);

	if (Debug) {
		dprintf("Debugging disabled, send SIGUSR1 to turn on debugging.\n");
		debugging_on = 0;
	}

	for (;;) {
		int nfds, readfds = FDMASK(funix) | klogm;

		if (AcceptRemote)
			readfds |= inetm;

		dprintf("readfds = %#x\n", readfds);
		nfds = select(20, (fd_set *)&readfds, (fd_set *)NULL,
		    (fd_set *)NULL, (struct timeval *)NULL);
		if (nfds == 0)
			continue;
		if (nfds < 0) {
			if (errno != EINTR)
				logerror("select");
			continue;
		}
		dprintf("got a message (%d, %#x)\n", nfds, readfds);
		if (readfds & klogm) {
			i = read(fklog, line, sizeof(line) - 1);
			if (i > 0) {
				line[i] = '\0';
				printsys(line);
			} else if (i < 0 && errno != EINTR) {
				logerror("klog");
				fklog = -1;
				klogm = 0;
			}
		}
		if (readfds & FDMASK(funix)) {
			len = sizeof(fromunix);
			i = recvfrom(funix, line, MAXLINE, 0,
			    (struct sockaddr *)&fromunix, &len);
			if (i > 0) {
				line[i] = '\0';
				printline(LocalHostName, line);
			} else if (i < 0 && errno != EINTR)
				logerror("recvfrom unix");
		}
		if (AcceptRemote && (readfds & inetm)) {
			len = sizeof(frominet);
			i = recvfrom(finet, line, MAXLINE, 0,
			    (struct sockaddr *)&frominet, &len);
			if (i > 0) {
				line[i] = '\0';
				printline(cvthname(&frominet), line);
			} else if (i < 0 && errno != EINTR)
				logerror("recvfrom inet");
		}
	}
}

void
usage()
{

	(void)fprintf(stderr,
"Usage: syslogd [OPTION]...\n\
Start system log daemon.\n\
\n\
  -f FILE        Read configuration from FILE instead from " PATH_LOGCONF ".\n\
  -h             Forward messages from remote hosts.\n\
  -l HOSTLIST    A ':'-seperated list of hosts which should be logged by\n\
                 their hostname and not the FQDN.\n\
  -m INTERVAL    Log timestamp mark every INTERVAL seconds. If INTERVAL is 0,\n\
                 timestamps are disabled.\n\
  -n             Don't detach syslogd from terminal or background it.\n\
  -p FILE        Specify the pathname of an alternate log socket instead of\n\
                 the default " PATH_LOG ".\n\
  -r             Enable to receive remote messages via internet domain socket.\n\
  -s DOMAINLIST  A ':'-seperated list of domains which should be stripped\n\
                 from FQDNs of hosts before logging them.\n\
  -V             Print version information and exit.\n\
\n\
Report bugs to %s.\n", inetutils_bugaddr);
	exit(1);
}

char **
crunch_list(char *list);
{
	int count, i;
	char *p, *q;
	char **result = NULL;

	p = list;

	/* strip off trailing delimiters */
	while (p[strlen(p)-1] == LIST_DELIMITER) {
		count--;
		p[strlen(p)-1] = '\0';
	}
	/* cut off leading delimiters */
	while (p[0] == LIST_DELIMITER) {
		count--;
		p++;
	}

	/* count delimiters to calculate elements */
	for (count=i=0; p[i]; i++)
		if (p[i] == LIST_DELIMITER) count++;

	if ((result = (char **)malloc(sizeof(char *) * count+2)) == NULL) {
		printf ("Sorry, can't get enough memory, exiting.\n");
		exit(0);
	}

	/*
	 * We now can assume that the first and last
	 * characters are different from any delimiters,
	 * so we don't have to care about this.
	 */
	count = 0;
	while ((q=strchr(p, LIST_DELIMITER))) {
		result[count] = (char *) malloc((q - p + 1) * sizeof(char));
		if (result[count] == NULL) {
			printf ("Sorry, can't get enough memory, exiting.\n");
			exit(0);
		}
		strncpy(result[count], p, q - p);
		result[count][q - p] = '\0';
		p = q; p++;
		count++;
	}
	if ((result[count] = \
		(char *)malloc(sizeof(char) * strlen(p) + 1)) == NULL) {
		printf ("Sorry, can't get enough memory, exiting.\n");
		exit(0);
	}
	strcpy(result[count],p);
	result[++count] = NULL;

#if 0
	count=0;
	while (result[count])
		dprintf ("#%d: %s\n", count, StripDomains[count++]);
#endif
	return result;
}

/*
 * Take a raw input line, decode the message, and print the message
 * on the appropriate log files.
 */
void
printline(const char *hname, const char *msg)
{
	int c, pri;
	const char *p;
	char *q, line[MAXLINE + 1];

	/* test for special codes */
	pri = DEFUPRI;
	p = msg;
	if (*p == '<') {
		pri = 0;
		while (isdigit(*++p))
			pri = 10 * pri + (*p - '0');
		if (*p == '>')
			++p;
	}
	if (pri &~ (LOG_FACMASK|LOG_PRIMASK))
		pri = DEFUPRI;

	/* don't allow users to log kernel messages */
	if (LOG_FAC(pri) == LOG_KERN)
		pri = LOG_MAKEPRI(LOG_USER, LOG_PRI(pri));

	q = line;

	while ((c = *p++) != '\0' &&
	    q < &line[sizeof(line) - 1])
		if (iscntrl(c))
			if (c == '\n')
				*q++ = ' ';
			else if (c == '\t')
				*q++ = '\t';
			else if (c >= 0177)
				*q++ = c;
			else {
				*q++ = '^';
				*q++ = c ^ 0100;
			}
		else
			*q++ = c;
	*q = '\0';

	logmsg(pri, line, hname, SYNC_FILE);
}

/*
 * Take a raw input line from /dev/klog, split and format similar to syslog().
 */
void
printsys(const char *msg)
{
	int c, pri, flags;
	char *lp, *q, line[MAXLINE + 1];
	const char *p;

	(void)strcpy(line, "vmunix: ");
	lp = line + strlen(line);
	for (p = msg; *p != '\0'; ) {
		flags = SYNC_FILE | ADDDATE;	/* fsync file after write */
		pri = DEFSPRI;
		if (*p == '<') {
			pri = 0;
			while (isdigit(*++p))
				pri = 10 * pri + (*p - '0');
			if (*p == '>')
				++p;
		} else {
			/* kernel printf's come out on console */
			flags |= IGN_CONS;
		}
		if (pri &~ (LOG_FACMASK|LOG_PRIMASK))
			pri = DEFSPRI;
		q = lp;
		while (*p != '\0' && (c = *p++) != '\n' &&
		    q < &line[MAXLINE])
			*q++ = c;
		*q = '\0';
		logmsg(pri, line, LocalHostName, flags);
	}
}

time_t	now;

/*
 * Log a message to the appropriate log files, users, etc. based on
 * the priority.
 */
void
logmsg(int pri, const char *msg, const char *from, int flags)
{
	struct filed *f;
	int fac, msglen, prilev;
#ifdef HAVE_SIGACTION
	sigset_t sigs, osigs;
#else
	int omask;
#endif

	const char *timestamp;

	dprintf("logmsg: pri %o, flags %x, from %s, msg %s\n",
	    pri, flags, from, msg);

#ifdef HAVE_SIGACTION
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGHUP);
	sigaddset(&sigs, SIGALRM);
	sigprocmask(SIG_BLOCK, &sigs, &osigs);
#else
	omask = sigblock(sigmask(SIGHUP)|sigmask(SIGALRM));
#endif

	/*
	 * Check to see if msg looks non-standard.
	 */
	msglen = strlen(msg);
	if (msglen < 16 || msg[3] != ' ' || msg[6] != ' ' ||
	    msg[9] != ':' || msg[12] != ':' || msg[15] != ' ')
		flags |= ADDDATE;

	(void)time(&now);
	if (flags & ADDDATE)
		timestamp = ctime(&now) + 4;
	else {
		timestamp = msg;
		msg += 16;
		msglen -= 16;
	}

	/* extract facility and priority level */
	if (flags & MARK)
		fac = LOG_NFACILITIES;
	else
		fac = LOG_FAC(pri);
	prilev = LOG_PRI(pri);

	/* log the message to the particular outputs */
	if (!Initialized) {
		f = &consfile;
		f->f_file = open(ctty, O_WRONLY, 0);
		f->f_prevhost = strdup (LocalHostName);
		if (f->f_file >= 0) {
			fprintlog(f, from, flags, msg);
			(void)close(f->f_file);
		}
#ifdef HAVE_SIGACTION
		sigprocmask(SIG_SETMASK, &osigs, 0);
#else
		(void)sigsetmask(omask);
#endif
		return;
	}
	for (f = Files; f; f = f->f_next) {
		/* skip messages that are incorrect priority */
		if (!(f->f_pmask[fac] & LOG_MASK(prilev)) ||
		    f->f_pmask[fac] == INTERNAL_NOPRI)
			continue;

		if (f->f_type == F_CONSOLE && (flags & IGN_CONS))
			continue;

		/* don't output marks to recently written files */
		if ((flags & MARK) && (now - f->f_time) < MarkInterval / 2)
			continue;

		/*
		 * suppress duplicate lines to this file
		 */
		if ((flags & MARK) == 0 && msglen == f->f_prevlen &&
		    f->f_prevhost &&
		    !strcmp(msg, f->f_prevline) &&
		    !strcmp(from, f->f_prevhost)) {
			(void)strncpy(f->f_lasttime, timestamp, 15);
			f->f_prevcount++;
			dprintf("msg repeated %d times, %ld sec of %d\n",
			    f->f_prevcount, now - f->f_time,
			    repeatinterval[f->f_repeatcount]);
			/*
			 * If domark would have logged this by now,
			 * flush it now (so we don't hold isolated messages),
			 * but back off so we'll flush less often
			 * in the future.
			 */
			if (now > REPEATTIME(f)) {
				fprintlog(f, from, flags, (char *)NULL);
				BACKOFF(f);
			}
		} else {
			/* new line, save it */
			if (f->f_prevcount)
				fprintlog(f, from, 0, (char *)NULL);
			f->f_repeatcount = 0;
			(void)strncpy(f->f_lasttime, timestamp, 15);
			if (f->f_prevhost)
				free (f->f_prevhost);
			f->f_prevhost = strdup (from);
			if (msglen < MAXSVLINE) {
				f->f_prevlen = msglen;
				f->f_prevpri = pri;
				(void)strcpy(f->f_prevline, msg);
				fprintlog(f, from, flags, (char *)NULL);
			} else {
				f->f_prevline[0] = 0;
				f->f_prevlen = 0;
				fprintlog(f, from, flags, msg);
			}
		}
	}
#ifdef HAVE_SIGACTION
	sigprocmask(SIG_SETMASK, &osigs, 0);
#else
	(void)sigsetmask(omask);
#endif
}

void
fprintlog(struct filed *f, const char *from, int flags, const char *msg)
{
	struct iovec iov[6];
	struct iovec *v;
	int l;
	char line[MAXLINE + 1], repbuf[80], greetings[200];
	time_t fwd_suspend;
	struct hostent *hp;

	v = iov;
	if (f->f_type == F_WALL) {
		v->iov_base = greetings;
		snprintf (greetings, sizeof greetings,
			  "\r\n\7Message from syslogd@%s at %.24s ...\r\n",
			  f->f_prevhost, ctime(&now));
		v->iov_len = strlen (greetings);
		v++;
		v->iov_base = "";
		v->iov_len = 0;
		v++;
	} else {
		v->iov_base = f->f_lasttime;
		v->iov_len = 15;
		v++;
		v->iov_base = " ";
		v->iov_len = 1;
		v++;
	}
	v->iov_base = f->f_prevhost;
	v->iov_len = strlen(v->iov_base);
	v++;
	v->iov_base = " ";
	v->iov_len = 1;
	v++;

	if (msg) {
		v->iov_base = (char *)msg;
		v->iov_len = strlen(msg);
	} else if (f->f_prevcount > 1) {
		v->iov_base = repbuf;
		snprintf(repbuf, sizeof(repbuf), "last message repeated %d times",
			 f->f_prevcount);
		v->iov_len = strlen(repbuf);
	} else {
		v->iov_base = f->f_prevline;
		v->iov_len = f->f_prevlen;
	}
	v++;

	dprintf("Logging to %s", TypeNames[f->f_type]);

	switch (f->f_type) {
	case F_UNUSED:
		f->f_time = now;
		dprintf("\n");
		break;

	case F_FORW_SUSP:
		fwd_suspend = time((time_t *) 0) - f->f_time;
		if ( fwd_suspend >= INET_SUSPEND_TIME ) {
			dprintf("\nForwarding suspension over, " \
				"retrying FORW ");
			f->f_type = F_FORW;
			goto f_forw;
		}
		else {
			dprintf(" %s\n", f->f_un.f_forw.f_hname);
			dprintf("Forwarding suspension not over, time " \
				"left: %d.\n", INET_SUSPEND_TIME - \
				fwd_suspend);
		}
		break;

	case F_FORW_UNKN:
		dprintf(" %s\n", f->f_un.f_forw.f_hname);
		fwd_suspend = time((time_t *) 0) - f->f_time;
		if (fwd_suspend >= INET_SUSPEND_TIME) {
			hp = gethostbyname(f->f_un.f_forw.f_hname);
			if (hp == NULL) {
				extern int h_errno;
#ifndef HAVE_HSTRERROR_DECL
				extern char *hstrerror __P((int));
#endif
				dprintf("Failure: %s\n",
					hstrerror(h_errno));
				dprintf("Retries: %d\n", f->f_prevcount);
				if ( --f->f_prevcount < 0 )
					f->f_type = F_UNUSED;
			} else {
				dprintf("%s found, resuming.\n",
					f->f_un.f_forw.f_hname);
				memcpy(&f->f_un.f_forw.f_addr.sin_addr,
				       hp->h_addr, hp->h_length);
				f->f_prevcount = 0;
				f->f_type = F_FORW;
				goto f_forw;
			}
		} else
			dprintf("Forwarding suspension not over, " \
				"time left: %d\n",
				INET_SUSPEND_TIME - fwd_suspend);
		break;

	case F_FORW:
	f_forw:
		dprintf(" %s\n", f->f_un.f_forw.f_hname);
		if (strcmp(from, LocalHostName) && NoHops) {
			dprintf("Not forwarding remote message.\n");
		} else {
			f->f_time = now;
			snprintf(line, sizeof(line), "<%d>%.15s %s",
				 f->f_prevpri, (char *)iov[0].iov_base,
				 (char *)iov[4].iov_base);
			l = strlen(line);
			if (l > MAXLINE)
				l = MAXLINE;
			if (sendto(finet, line, l, 0,
				   (struct sockaddr *)&f->f_un.f_forw.f_addr,
				   sizeof(f->f_un.f_forw.f_addr)) != l) {
				int e = errno;
				dprintf("INET sendto error: %d = %s.\n",
					e, strerror(e));
				f->f_type = F_FORW_SUSP;
				errno = e;
				logerror("sendto");
			}
		}
		break;

	case F_CONSOLE:
		f->f_time = now;
		if (flags & IGN_CONS) {
			dprintf(" (ignored)\n");
			break;
		}
		/* FALLTHROUGH */

	case F_TTY:
	case F_FILE:
	case F_PIPE:
		f->f_time = now;
		dprintf(" %s\n", f->f_un.f_fname);
		if (f->f_type == F_TTY || f->f_type == F_CONSOLE) {
			v->iov_base = "\r\n";
			v->iov_len = 2;
		} else {
			v->iov_base = "\n";
			v->iov_len = 1;
		}
	again:
		if (writev(f->f_file, iov, 6) < 0) {
			int e = errno;

			/* XXX: If a named pipe is full, ignore it. */
			if (f->f_type == F_PIPE && e == EAGAIN)
				break;

			(void)close(f->f_file);
			/*
			 * Check for errors on TTY's due to loss of tty
			 */
			if ((e == EIO || e == EBADF)
			    && (f->f_type == F_TTY || f->f_type == F_CONSOLE)) {
				f->f_file = open(f->f_un.f_fname,
				    O_WRONLY|O_APPEND, 0);
				if (f->f_file < 0) {
					f->f_type = F_UNUSED;
					logerror(f->f_un.f_fname);
				} else
					goto again;
			} else {
				f->f_type = F_UNUSED;
				errno = e;
				logerror(f->f_un.f_fname);
			}
		} else if ((flags & SYNC_FILE) && !(f->f_flags & OMIT_SYNC))
			(void)fsync(f->f_file);
		break;

	case F_USERS:
	case F_WALL:
		f->f_time = now;
		dprintf("\n");
		v->iov_base = "\r\n";
		v->iov_len = 2;
		wallmsg(f, iov);
		break;
	}

	if (f->f_type != F_FORW_UNKN)
		f->f_prevcount = 0;
}

/*
 *  WALLMSG -- Write a message to the world at large
 *
 *	Write the specified message to either the entire
 *	world, or a list of approved users.
 */
void
wallmsg(struct filed *f, struct iovec *iov)
{
	static int reenter;			/* avoid calling ourselves */
	FILE *uf;
	struct utmp ut;
	int i;
	char *p;
	char line[sizeof(ut.ut_line) + 1];

	if (reenter++)
		return;
	if ((uf = fopen(PATH_UTMP, "r")) == NULL) {
		logerror(PATH_UTMP);
		reenter = 0;
		return;
	}
	/* NOSTRICT */
	while (fread((char *)&ut, sizeof(ut), 1, uf) == 1) {
		if (ut.ut_name[0] == '\0')
			continue;
		strncpy(line, ut.ut_line, sizeof(ut.ut_line));
		line[sizeof(ut.ut_line)] = '\0';
		if (f->f_type == F_WALL) {
			if ((p = ttymsg(iov, 6, line, 60*5)) != NULL) {
				errno = 0;	/* already in msg */
				logerror(p);
			}
			continue;
		}
		/* should we send the message to this user? */
		for (i = 0; i < f->f_un.f_user.f_nusers; i++)
			if (!strncmp(f->f_un.f_user.f_unames[i], ut.ut_name,
				     sizeof (ut.ut_name))) {
				if ((p = ttymsg(iov, 6, line, 60*5)) != NULL) {
					errno = 0;	/* already in msg */
					logerror(p);
				}
				break;
			}
	}
	(void)fclose(uf);
	reenter = 0;
}

void
reapchild(int signo)
{
#ifdef HAVE_WAITPID
	while (waitpid(-1, 0, WNOHANG) > 0)
#else
	while (wait3(0, WNOHANG, (struct rusage *)NULL) > 0)
#endif
		;
}

/*
 * Return a printable representation of a host address.
 */
const char *
cvthname(struct sockaddr_in *f)
{
	struct hostent *hp;
	char *p;

	dprintf("cvthname(%s)\n", inet_ntoa(f->sin_addr));

	if (f->sin_family != AF_INET) {
		dprintf("Malformed from address.\n");
		return ("???");
	}
	hp = gethostbyaddr((char *)&f->sin_addr,
	    sizeof(struct in_addr), f->sin_family);
	if (hp == 0) {
		dprintf("Host name for your address (%s) unknown.\n",
			inet_ntoa(f->sin_addr));
		return (inet_ntoa(f->sin_addr));
	}
	for (p = hp->h_name; *p ; p++)
		if (isupper(*p))
			*p = tolower(*p);

	if ((p = strchr(hp->h_name, '.')) && strcmp(p + 1, LocalDomain) == 0) {
		*p = '\0';
		return (hp->h_name);
	} else {
		if (StripDomains) {
			count=0;
			while (StripDomains[count]) {
				if (strcmp(p + 1, StripDomains[count]) == 0) {
					*p = '\0';
					return (hp->h_name);
				}
				count++;
			}
			if (LocalHosts) {
				count=0;
				while (LocalHosts[count]) {
					if (!strcmp(hp->h_name, LocalHosts[count])) {
						*p = '\0';
						return (hp->h_name);
					}
					count++;
				}
			}
		}
	}
	return (hp->h_name);
}

void
domark(int signo)
{
	struct filed *f;

	now = time((time_t *)NULL);
	MarkSeq += TIMERINTVL;
	if (MarkSeq >= MarkInterval) {
		logmsg(LOG_INFO, "-- MARK --", LocalHostName, ADDDATE|MARK);
		MarkSeq = 0;
	}

	for (f = Files; f; f = f->f_next) {
		if (f->f_prevcount && now >= REPEATTIME(f)) {
			dprintf("flush %s: repeated %d times, %d sec.\n",
			    TypeNames[f->f_type], f->f_prevcount,
			    repeatinterval[f->f_repeatcount]);
			fprintlog(f, LocalHostName, 0, (char *)NULL);
			BACKOFF(f);
		}
	}
	(void)alarm(TIMERINTVL);
}

/*
 * Print syslogd errors some place.
 */
void
logerror(const char *type)
{
	char buf[100];

	if (errno)
		(void)snprintf(buf,
		    sizeof(buf), "syslogd: %s: %s", type, strerror(errno));
	else
		(void)snprintf(buf, sizeof(buf), "syslogd: %s", type);
	errno = 0;
	dprintf("%s\n", buf);
	logmsg(LOG_SYSLOG|LOG_ERR, buf, LocalHostName, ADDDATE);
}

void
die(int signo)
{
	struct filed *f;
	char buf[100];

	for (f = Files; f != NULL; f = f->f_next) {
		/* flush any pending output */
		if (f->f_prevcount)
			fprintlog(f, LocalHostName, 0, (char *)NULL);
	}
	if (signo) {
		dprintf("syslogd: exiting on signal %d\n", signo);
		snprintf(buf, sizeof(buf), "exiting on signal %d", signo);
		errno = 0;
		logerror(buf);
	}
	(void)unlink(LogName);
	exit(0);
}

/*
 *  INIT -- Initialize syslogd from configuration table
 */
void
init(int signo)
{
	int i;
	FILE *cf;
	struct filed *f, *next, **nextp;
	char *p;
	char cbuf[LINE_MAX];
	char *cline;

	dprintf("init\n");

	/*
	 *  Close all open log files.
	 */
	Initialized = 0;
	for (f = Files; f != NULL; f = next) {
		/* flush any pending output */
		if (f->f_prevcount)
			fprintlog(f, LocalHostName, 0, (char *)NULL);

		switch (f->f_type) {
		case F_FILE:
		case F_TTY:
		case F_CONSOLE:
		case F_PIPE:
			(void)close(f->f_file);
			break;
		}
		next = f->f_next;
		free((char *)f);
	}
	Files = NULL;
	nextp = &Files;

	/* open the configuration file */
	if ((cf = fopen(ConfFile, "r")) == NULL) {
		dprintf("cannot open %s\n", ConfFile);
		*nextp = (struct filed *)calloc(1, sizeof(*f));
		cfline("*.ERR\t" PATH_CONSOLE, *nextp);
		(*nextp)->f_next = (struct filed *)calloc(1, sizeof(*f));
		cfline("*.PANIC\t*", (*nextp)->f_next);
		Initialized = 1;
		return;
	}

	/*
	 *  Foreach line in the conf table, open that file.
	 */
	f = NULL;
	cline = cbuf;
	while (fgets(cline, sizeof(cbuf) - (cline - cbuf), cf) != NULL) {
		/*
		 * check for end-of-section, comments, strip off trailing
		 * spaces and newline character.
		 */
		for (p = cline; isspace(*p); ++p)
			continue;
		if (*p == '\0' || *p == '#')
			continue;
		strcpy(cline, p);
		for (p = strchr(cline, '\0'); isspace(*--p);)
			continue;
		if (*p == '\\') {
			if ((p - cbuf) > LINE_MAX - 30) {
				/* Oops the buffer is full - what now? */
				cline = cbuf;
			} else {
				*p = 0;
				cline = p;
				continue;
			}
		}  else
			cline = cbuf;
		*++p = '\0';
		f = (struct filed *)calloc(1, sizeof(*f));
		*nextp = f;
		nextp = &f->f_next;
		cfline(cbuf, f);
	}

	/* close the configuration file */
	(void)fclose(cf);

	Initialized = 1;

	if (Debug) {
		for (f = Files; f; f = f->f_next) {
			for (i = 0; i <= LOG_NFACILITIES; i++)
				if (f->f_pmask[i] == INTERNAL_NOPRI)
					printf(" X ");
				else
					printf("%2d ", f->f_pmask[i]);
			printf("%s: ", TypeNames[f->f_type]);
			switch (f->f_type) {
			case F_FILE:
			case F_TTY:
			case F_CONSOLE:
			case F_PIPE:
				printf("%s", f->f_un.f_fname);
				break;

			case F_FORW:
			case F_FORW_SUSP:
			case F_FORW_UNKN:
				printf("%s", f->f_un.f_forw.f_hname);
				break;

			case F_USERS:
				for (i = 0; i < f->f_un.f_user.f_nusers; i++)
					printf("%s, ",
					       f->f_un.f_user.f_unames[i]);
				break;
			}
			printf("\n");
		}
	}

	if (AcceptRemote)
		logmsg(LOG_SYSLOG|LOG_INFO, "syslogd (" inetutils_package \
			" " inetutils_version "): restart (remote reception)",
			LocalHostName, ADDDATE);
	else
		logmsg(LOG_SYSLOG|LOG_INFO, "syslogd (" inetutils_package \
			" " inetutils_version "): restart", LocalHostName, ADDDATE);
	dprintf("syslogd: restarted\n");
}

/*
 * Crack a configuration file line
 */
void
cfline(char *line, struct filed *f)
{
	struct hostent *hp;
	int i, pri, negate_pri, excl_pri;
	unsigned int pri_set, pri_clear;
	char *bp, *p, *q;
	char buf[MAXLINE], ebuf[200];

	dprintf("cfline(%s)\n", line);

	errno = 0;	/* keep strerror() stuff out of logerror messages */

	/* clear out file entry */
	memset(f, 0, sizeof(*f));
	for (i = 0; i <= LOG_NFACILITIES; i++) {
		f->f_pmask[i] = INTERNAL_NOPRI;
		f->f_flags = 0;
	}

	/* scan through the list of selectors */
	for (p = line; *p && *p != '\t' && *p != ' ';) {

		/* find the end of this facility name list */
		for (q = p; *q && *q != '\t' && *q++ != '.'; )
			continue;

		/* collect priority name */
		for (bp = buf; *q && !strchr("\t ,;", *q); )
			*bp++ = *q++;
		*bp = '\0';

		/* skip cruft */
		while (strchr(",;", *q))
			q++;

		bp = buf;
		negate_pri = excl_pri = 0;

		while (*bp == '!' || *bp == '=')
			switch (*bp++)
			{
			case '!':
				negate_pri = 1;
				break;
			case '=':
				excl_pri = 1;
				break;
			}

		/* Decode priority name and set up bit masks. */
		if (*bp == '*') {
			pri_clear = INTERNAL_NOPRI;
			pri_set = LOG_UPTO(LOG_PRIMASK);
		} else {
			pri = decode(bp, prioritynames);
			if (pri < 0) {
				snprintf (ebuf, sizeof ebuf,
						  "unknown priority name \"%s\"", bp);
				logerror(ebuf);
				return;
			}
			pri_clear = 0;
			pri_set = excl_pri ? LOG_MASK(pri) : LOG_UPTO(pri);
		}
		if (negate_pri) {
			unsigned int exchange = pri_set;
			pri_set = pri_clear;
			pri_clear = exchange;
		}

		/* scan facilities */
		while (*p && !strchr("\t .;", *p)) {
			for (bp = buf; *p && !strchr("\t ,;.", *p); )
				*bp++ = *p++;
			*bp = '\0';
			if (*buf == '*')
				for (i = 0; i <= LOG_NFACILITIES; i++) {
					f->f_pmask[i] &= ~pri_clear;
					f->f_pmask[i] |= pri_set;
				}
			else {
				i = decode(buf, facilitynames);
				if (i < 0) {
					snprintf (ebuf, sizeof ebuf,
							 "unknown facility name \"%s\"", buf);
					logerror(ebuf);
					return;
				}
				f->f_pmask[LOG_FAC(i)] &= ~pri_clear;
				f->f_pmask[LOG_FAC(i)] |= pri_set;
			}
			while (*p == ',' || *p == ' ')
				p++;
		}

		p = q;
	}

	/* skip to action part */
	while (*p == '\t' || *p == ' ')
		p++;

	if (*p == '-') {
		f->f_flags |= OMIT_SYNC;
		p++;
	}

	switch (*p)
	{
	case '@':
		if (!InetInuse)
			break;
		f->f_un.f_forw.f_hname = strdup (++p);
		hp = gethostbyname(p);
		if (hp == NULL) {
			f->f_type = F_FORW_UNKN;
			f->f_prevcount = INET_RETRY_MAX;
			f->f_time = time ( (time_t *)0 );
		} else
			f->f_type = F_FORW;
		memset(&f->f_un.f_forw.f_addr, 0,
		       sizeof(f->f_un.f_forw.f_addr));
		f->f_un.f_forw.f_addr.sin_family = AF_INET;
		f->f_un.f_forw.f_addr.sin_port = LogPort;
		if (f->f_type == F_FORW)
			memcpy(&f->f_un.f_forw.f_addr.sin_addr,
			       hp->h_addr, hp->h_length);
		break;

	case '|':
		f->f_un.f_fname = strdup (p);
		if (f->f_file = open(++p, O_RDWR|O_NONBLOCK) < 0) {
			f->f_type = F_UNUSED;
			logerror(p);
			break;
		}
		f->f_type = F_PIPE;
		break;

	case '/':
		f->f_un.f_fname = strdup (p);
		if ((f->f_file = open(p, O_WRONLY|O_APPEND|O_CREAT, 0644)) < 0) {
			f->f_type = F_UNUSED;
			logerror(p);
			break;
		}
		if (isatty(f->f_file))
			f->f_type = F_TTY;
		else
			f->f_type = F_FILE;
		if (strcmp(p, ctty) == 0)
			f->f_type = F_CONSOLE;
		break;

	case '*':
		f->f_type = F_WALL;
		break;

	default:
		f->f_un.f_user.f_nusers = 1;
		for (q = p; *q; q++)
			if (*q == ',')
				f->f_un.f_user.f_nusers++;
		f->f_un.f_user.f_unames =
		  (char **) malloc (f->f_un.f_user.f_nusers * sizeof (char *));
		for (i = 0; *p; i++) {
			for (q = p; *q && *q != ','; )
				q++;
			f->f_un.f_user.f_unames[i] = malloc (q - p + 1);
			if (f->f_un.f_user.f_unames[i]) {
				strncpy (f->f_un.f_user.f_unames[i], p, q - p);
				f->f_un.f_user.f_unames[i][q - p] = '\0';
			}
			while (*q == ',' || *q == ' ')
				q++;
			p = q;
		}
		f->f_type = F_USERS;
		break;
	}
}

/*
 *  Decode a symbolic name to a numeric value
 */
int
decode(const char *name, CODE *codetab)
{
	CODE *c;
	char *p, buf[80];

	if (isdigit(*name))
		return (atoi(name));

	for (p = buf; *name && p < &buf[sizeof(buf) - 1]; p++, name++) {
		if (isupper(*name))
			*p = tolower(*name);
		else
			*p = *name;
	}
	*p = '\0';
	for (c = codetab; c->c_name; c++)
		if (!strcmp(buf, c->c_name))
			return (c->c_val);

	return (-1);
}

void
debug_switch()
{
	int debugging_save = debugging_on;

	debugging_on = 1;
	dprintf("Switching debugging_on to %s.\n",
		(debugging_save == 0) ? "true" : "false");
	debugging_on = (debugging_save == 0) ? 1 : 0;
	signal(SIGUSR1, debug_switch);
}

static void
dprintf(char *fmt, ...)
{
	va_list ap;

	if (!(Debug && debugging_on))
		return;

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);

	fflush(stdout);
}
