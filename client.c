/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

static struct tmuxproc	*client_proc;
static struct tmuxpeer	*client_peer;
static int		 client_flags;
static struct event	 client_stdin;
static enum {
	CLIENT_EXIT_NONE,
	CLIENT_EXIT_DETACHED,
	CLIENT_EXIT_DETACHED_HUP,
	CLIENT_EXIT_LOST_TTY,
	CLIENT_EXIT_TERMINATED,
	CLIENT_EXIT_LOST_SERVER,
	CLIENT_EXIT_EXITED,
	CLIENT_EXIT_SERVER_EXITED,
} client_exitreason = CLIENT_EXIT_NONE;
static int		 client_exitval;
static enum msgtype	 client_exittype;
static const char	*client_exitsession;
static const char	*client_execstr;
static const char	*client_execshell;
static int		 client_attached;

static __dead void	 client_exec(const char *,const char *);
static int		 client_get_lock(char *);
static int		 client_connect(struct event_base *, const char *, int);
static void		 client_send_identify(const char *, const char *);
static void		 client_stdin_callback(int, short, void *);
static void		 client_write(int, const char *, size_t);
static void		 client_signal(int);
static void		 client_dispatch(struct imsg *, void *);
static void		 client_dispatch_attached(struct imsg *);
static void		 client_dispatch_wait(struct imsg *, const char *);
static const char	*client_exit_message(void);

/*
 * Get server create lock. If already held then server start is happening in
 * another client, so block until the lock is released and return -2 to
 * retry. Return -1 on failure to continue and start the server anyway.
 */
static int
client_get_lock(char *lockfile)
{
	int lockfd;

	log_debug("lock file is %s", lockfile);

	if ((lockfd = open(lockfile, O_WRONLY|O_CREAT, 0600)) == -1) {
		log_debug("open failed: %s", strerror(errno));
		return (-1);
	}

	if (flock(lockfd, LOCK_EX|LOCK_NB) == -1) {
		log_debug("flock failed: %s", strerror(errno));
		if (errno != EAGAIN)
			return (lockfd);
		while (flock(lockfd, LOCK_EX) == -1 && errno == EINTR)
			/* nothing */;
		close(lockfd);
		return (-2);
	}
	log_debug("flock succeeded");

	return (lockfd);
}

/* Connect client to server. */
static int
client_connect(struct event_base *base, const char *path, int start_server)
{
	struct sockaddr_un	sa;
	size_t			size;
	int			fd, lockfd = -1, locked = 0;
	char		       *lockfile = NULL;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	size = strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	if (size >= sizeof sa.sun_path) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	log_debug("socket is %s", path);

retry:
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return (-1);

	log_debug("trying connect");
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == -1) {
		log_debug("connect failed: %s", strerror(errno));
		if (errno != ECONNREFUSED && errno != ENOENT)
			goto failed;
		if (!start_server)
			goto failed;
		close(fd);

		if (!locked) {
			xasprintf(&lockfile, "%s.lock", path);
			if ((lockfd = client_get_lock(lockfile)) < 0) {
				log_debug("didn't get lock (%d)", lockfd);

				free(lockfile);
				lockfile = NULL;

				if (lockfd == -2)
					goto retry;
			}
			log_debug("got lock (%d)", lockfd);

			/*
			 * Always retry at least once, even if we got the lock,
			 * because another client could have taken the lock,
			 * started the server and released the lock between our
			 * connect() and flock().
			 */
			locked = 1;
			goto retry;
		}

		if (lockfd >= 0 && unlink(path) != 0 && errno != ENOENT) {
			free(lockfile);
			close(lockfd);
			return (-1);
		}
		fd = server_start(base, lockfd, lockfile);
	}

	if (locked && lockfd >= 0) {
		free(lockfile);
		close(lockfd);
	}
	setblocking(fd, 0);
	return (fd);

failed:
	if (locked) {
		free(lockfile);
		close(lockfd);
	}
	close(fd);
	return (-1);
}

/* Get exit string from reason number. */
const char *
client_exit_message(void)
{
	static char msg[256];

	switch (client_exitreason) {
	case CLIENT_EXIT_NONE:
		break;
	case CLIENT_EXIT_DETACHED:
		if (client_exitsession != NULL) {
			xsnprintf(msg, sizeof msg, "detached "
			    "(from session %s)", client_exitsession);
			return (msg);
		}
		return ("detached");
	case CLIENT_EXIT_DETACHED_HUP:
		if (client_exitsession != NULL) {
			xsnprintf(msg, sizeof msg, "detached and SIGHUP "
			    "(from session %s)", client_exitsession);
			return (msg);
		}
		return ("detached and SIGHUP");
	case CLIENT_EXIT_LOST_TTY:
		return ("lost tty");
	case CLIENT_EXIT_TERMINATED:
		return ("terminated");
	case CLIENT_EXIT_LOST_SERVER:
		return ("lost server");
	case CLIENT_EXIT_EXITED:
		return ("exited");
	case CLIENT_EXIT_SERVER_EXITED:
		return ("server exited");
	}
	return ("unknown reason");
}

/* Client main loop. */
int
client_main(struct event_base *base, int argc, char **argv, int flags,
    const char *shellcmd)
{
	struct cmd		*cmd;
	struct cmd_list		*cmdlist;
	struct msg_command_data	*data;
	int			 cmdflags, fd, i;
	const char		*ttynam, *cwd;
	pid_t			 ppid;
	enum msgtype		 msg;
	char			*cause, path[PATH_MAX];
	struct termios		 tio, saved_tio;
	size_t			 size;

	/* Ignore SIGCHLD now or daemon() in the server will leave a zombie. */
	signal(SIGCHLD, SIG_IGN);

	/* Save the flags. */
	client_flags = flags;

	/* Set up the initial command. */
	cmdflags = 0;
	if (shellcmd != NULL) {
		msg = MSG_SHELL;
		cmdflags = CMD_STARTSERVER;
	} else if (argc == 0) {
		msg = MSG_COMMAND;
		cmdflags = CMD_STARTSERVER;
	} else {
		msg = MSG_COMMAND;

		/*
		 * It sucks parsing the command string twice (in client and
		 * later in server) but it is necessary to get the start server
		 * flag.
		 */
		cmdlist = cmd_list_parse(argc, argv, NULL, 0, &cause);
		if (cmdlist == NULL) {
			fprintf(stderr, "%s\n", cause);
			return (1);
		}
		cmdflags &= ~CMD_STARTSERVER;
		TAILQ_FOREACH(cmd, &cmdlist->list, qentry) {
			if (cmd->entry->flags & CMD_STARTSERVER)
				cmdflags |= CMD_STARTSERVER;
		}
		cmd_list_free(cmdlist);
	}

	/* Create client process structure (starts logging). */
	client_proc = proc_start("client", base, 0, client_signal);

	/* Initialize the client socket and start the server. */
	fd = client_connect(base, socket_path, cmdflags & CMD_STARTSERVER);
	if (fd == -1) {
		if (errno == ECONNREFUSED) {
			fprintf(stderr, "no server running on %s\n",
			    socket_path);
		} else {
			fprintf(stderr, "error connecting to %s (%s)\n",
			    socket_path, strerror(errno));
		}
		return (1);
	}
	client_peer = proc_add_peer(client_proc, fd, client_dispatch,
	    (void *)shellcmd);

	/* Save these before pledge(). */
	if ((cwd = getcwd(path, sizeof path)) == NULL) {
		if ((cwd = find_home()) == NULL)
			cwd = "/";
	}
	if ((ttynam = ttyname(STDIN_FILENO)) == NULL)
		ttynam = "";

#ifdef __OpenBSD__
	/*
	 * Drop privileges for client. "proc exec" is needed for -c and for
	 * locking (which uses system(3)).
	 *
	 * "tty" is needed to restore termios(4) and also for some reason -CC
	 * does not work properly without it (input is not recognised).
	 *
	 * "sendfd" is dropped later in client_dispatch_wait().
	 */
	if (pledge("stdio unix sendfd proc exec tty", NULL) != 0)
		fatal("pledge failed");
#endif

	/* Free stuff that is not used in the client. */
	options_free(global_options);
	options_free(global_s_options);
	options_free(global_w_options);
	environ_free(global_environ);

	/* Create stdin handler. */
	setblocking(STDIN_FILENO, 0);
	event_set(&client_stdin, STDIN_FILENO, EV_READ|EV_PERSIST,
	    client_stdin_callback, NULL);
	if (client_flags & CLIENT_CONTROLCONTROL) {
		if (tcgetattr(STDIN_FILENO, &saved_tio) != 0)
			fatal("tcgetattr failed");
		cfmakeraw(&tio);
		tio.c_iflag = ICRNL|IXANY;
		tio.c_oflag = OPOST|ONLCR;
#ifdef NOKERNINFO
		tio.c_lflag = NOKERNINFO;
#endif
		tio.c_cflag = CREAD|CS8|HUPCL;
		tio.c_cc[VMIN] = 1;
		tio.c_cc[VTIME] = 0;
		cfsetispeed(&tio, cfgetispeed(&saved_tio));
		cfsetospeed(&tio, cfgetospeed(&saved_tio));
		tcsetattr(STDIN_FILENO, TCSANOW, &tio);
	}

	/* Send identify messages. */
	client_send_identify(ttynam, cwd);

	/* Send first command. */
	if (msg == MSG_COMMAND) {
		/* How big is the command? */
		size = 0;
		for (i = 0; i < argc; i++)
			size += strlen(argv[i]) + 1;
		data = xmalloc((sizeof *data) + size);

		/* Prepare command for server. */
		data->argc = argc;
		if (cmd_pack_argv(argc, argv, (char *)(data + 1), size) != 0) {
			fprintf(stderr, "command too long\n");
			free(data);
			return (1);
		}
		size += sizeof *data;

		/* Send the command. */
		if (proc_send(client_peer, msg, -1, data, size) != 0) {
			fprintf(stderr, "failed to send command\n");
			free(data);
			return (1);
		}
		free(data);
	} else if (msg == MSG_SHELL)
		proc_send(client_peer, msg, -1, NULL, 0);

	/* Start main loop. */
	proc_loop(client_proc, NULL);

	/* If user requested exec, exec instead of exiting */
	if (client_exittype == MSG_EXEC) {
		client_exec(client_execshell, client_execstr);
	}

	/* Print the exit message, if any, and exit. */
	if (client_attached) {
		if (client_exitreason != CLIENT_EXIT_NONE)
			printf("[%s]\n", client_exit_message());

		ppid = getppid();
		if (client_exittype == MSG_DETACHKILL && ppid > 1)
			kill(ppid, SIGHUP);
	} else if (client_flags & CLIENT_CONTROLCONTROL) {
		if (client_exitreason != CLIENT_EXIT_NONE)
			printf("%%exit %s\n", client_exit_message());
		else
			printf("%%exit\n");
		printf("\033\\");
		tcsetattr(STDOUT_FILENO, TCSAFLUSH, &saved_tio);
	} else if (client_exitreason != CLIENT_EXIT_NONE)
		fprintf(stderr, "%s\n", client_exit_message());
	setblocking(STDIN_FILENO, 1);
	return (client_exitval);
}

/* Send identify messages to server. */
static void
client_send_identify(const char *ttynam, const char *cwd)
{
	const char	 *s;
	char		**ss;
	size_t		  sslen;
	int		  fd, flags = client_flags;
	pid_t		  pid;

	proc_send(client_peer, MSG_IDENTIFY_FLAGS, -1, &flags, sizeof flags);

	if ((s = getenv("TERM")) == NULL)
		s = "";
	proc_send(client_peer, MSG_IDENTIFY_TERM, -1, s, strlen(s) + 1);

	proc_send(client_peer, MSG_IDENTIFY_TTYNAME, -1, ttynam,
	    strlen(ttynam) + 1);
	proc_send(client_peer, MSG_IDENTIFY_CWD, -1, cwd, strlen(cwd) + 1);

	if ((fd = dup(STDIN_FILENO)) == -1)
		fatal("dup failed");
	proc_send(client_peer, MSG_IDENTIFY_STDIN, fd, NULL, 0);

	pid = getpid();
	proc_send(client_peer, MSG_IDENTIFY_CLIENTPID, -1, &pid, sizeof pid);

	for (ss = environ; *ss != NULL; ss++) {
		sslen = strlen(*ss) + 1;
		if (sslen > MAX_IMSGSIZE - IMSG_HEADER_SIZE)
			continue;
		proc_send(client_peer, MSG_IDENTIFY_ENVIRON, -1, *ss, sslen);
	}

	proc_send(client_peer, MSG_IDENTIFY_DONE, -1, NULL, 0);
}

/* Callback for client stdin read events. */
static void
client_stdin_callback(__unused int fd, __unused short events,
    __unused void *arg)
{
	struct msg_stdin_data	data;

	data.size = read(STDIN_FILENO, data.data, sizeof data.data);
	if (data.size < 0 && (errno == EINTR || errno == EAGAIN))
		return;

	proc_send(client_peer, MSG_STDIN, -1, &data, sizeof data);
	if (data.size <= 0)
		event_del(&client_stdin);
}

/* Force write to file descriptor. */
static void
client_write(int fd, const char *data, size_t size)
{
	ssize_t	used;

	while (size != 0) {
		used = write(fd, data, size);
		if (used == -1) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			break;
		}
		data += used;
		size -= used;
	}
}

/* Run command in shell; used for -c. */
static __dead void
client_exec(const char *shell, const char *shellcmd)
{
	const char	*name, *ptr;
	char		*argv0;

	log_debug("shell %s, command %s", shell, shellcmd);

	ptr = strrchr(shell, '/');
	if (ptr != NULL && *(ptr + 1) != '\0')
		name = ptr + 1;
	else
		name = shell;
	if (client_flags & CLIENT_LOGIN)
		xasprintf(&argv0, "-%s", name);
	else
		xasprintf(&argv0, "%s", name);
	setenv("SHELL", shell, 1);

	setblocking(STDIN_FILENO, 1);
	setblocking(STDOUT_FILENO, 1);
	setblocking(STDERR_FILENO, 1);
	closefrom(STDERR_FILENO + 1);

	execl(shell, argv0, "-c", shellcmd, (char *) NULL);
	fatal("execl failed");
}

/* Callback to handle signals in the client. */
static void
client_signal(int sig)
{
	struct sigaction sigact;
	int		 status;

	if (sig == SIGCHLD)
		waitpid(WAIT_ANY, &status, WNOHANG);
	else if (!client_attached) {
		if (sig == SIGTERM)
			proc_exit(client_proc);
	} else {
		switch (sig) {
		case SIGHUP:
			client_exitreason = CLIENT_EXIT_LOST_TTY;
			client_exitval = 1;
			proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
			break;
		case SIGTERM:
			client_exitreason = CLIENT_EXIT_TERMINATED;
			client_exitval = 1;
			proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
			break;
		case SIGWINCH:
			proc_send(client_peer, MSG_RESIZE, -1, NULL, 0);
			break;
		case SIGCONT:
			memset(&sigact, 0, sizeof sigact);
			sigemptyset(&sigact.sa_mask);
			sigact.sa_flags = SA_RESTART;
			sigact.sa_handler = SIG_IGN;
			if (sigaction(SIGTSTP, &sigact, NULL) != 0)
				fatal("sigaction failed");
			proc_send(client_peer, MSG_WAKEUP, -1, NULL, 0);
			break;
		}
	}
}

/* Callback for client read events. */
static void
client_dispatch(struct imsg *imsg, void *arg)
{
	if (imsg == NULL) {
		client_exitreason = CLIENT_EXIT_LOST_SERVER;
		client_exitval = 1;
		proc_exit(client_proc);
		return;
	}

	if (client_attached)
		client_dispatch_attached(imsg);
	else
		client_dispatch_wait(imsg, arg);
}

/* Dispatch imsgs when in wait state (before MSG_READY). */
static void
client_dispatch_wait(struct imsg *imsg, const char *shellcmd)
{
	char			*data;
	ssize_t			 datalen;
	struct msg_stdout_data	 stdoutdata;
	struct msg_stderr_data	 stderrdata;
	int			 retval;
#ifdef __OpenBSD__
	static int		 pledge_applied;

	/*
	 * "sendfd" is no longer required once all of the identify messages
	 * have been sent. We know the server won't send us anything until that
	 * point (because we don't ask it to), so we can drop "sendfd" once we
	 * get the first message from the server.
	 */
	if (!pledge_applied) {
		if (pledge("stdio unix proc exec tty", NULL) != 0)
			fatal("pledge failed");
		pledge_applied = 1;
	};
#endif

	data = imsg->data;
	datalen = imsg->hdr.len - IMSG_HEADER_SIZE;

	switch (imsg->hdr.type) {
	case MSG_EXIT:
	case MSG_SHUTDOWN:
		if (datalen != sizeof retval && datalen != 0)
			fatalx("bad MSG_EXIT size");
		if (datalen == sizeof retval) {
			memcpy(&retval, data, sizeof retval);
			client_exitval = retval;
		}
		proc_exit(client_proc);
		break;
	case MSG_READY:
		if (datalen != 0)
			fatalx("bad MSG_READY size");

		event_del(&client_stdin);
		client_attached = 1;
		proc_send(client_peer, MSG_RESIZE, -1, NULL, 0);
		break;
	case MSG_STDIN:
		if (datalen != 0)
			fatalx("bad MSG_STDIN size");

		event_add(&client_stdin, NULL);
		break;
	case MSG_STDOUT:
		if (datalen != sizeof stdoutdata)
			fatalx("bad MSG_STDOUT size");
		memcpy(&stdoutdata, data, sizeof stdoutdata);

		client_write(STDOUT_FILENO, stdoutdata.data,
		    stdoutdata.size);
		break;
	case MSG_STDERR:
		if (datalen != sizeof stderrdata)
			fatalx("bad MSG_STDERR size");
		memcpy(&stderrdata, data, sizeof stderrdata);

		client_write(STDERR_FILENO, stderrdata.data,
		    stderrdata.size);
		break;
	case MSG_VERSION:
		if (datalen != 0)
			fatalx("bad MSG_VERSION size");

		fprintf(stderr, "protocol version mismatch "
		    "(client %d, server %u)\n", PROTOCOL_VERSION,
		    imsg->hdr.peerid & 0xff);
		client_exitval = 1;
		proc_exit(client_proc);
		break;
	case MSG_SHELL:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_SHELL string");

		clear_signals(0);
		client_exec(data, shellcmd);
		/* NOTREACHED */
	case MSG_DETACH:
	case MSG_DETACHKILL:
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXITED:
		proc_exit(client_proc);
		break;
	}
}

/* Dispatch imsgs in attached state (after MSG_READY). */
static void
client_dispatch_attached(struct imsg *imsg)
{
	struct sigaction	 sigact;
	char			*data;
	ssize_t			 datalen;

	data = imsg->data;
	datalen = imsg->hdr.len - IMSG_HEADER_SIZE;

	switch (imsg->hdr.type) {
	case MSG_DETACH:
	case MSG_DETACHKILL:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_DETACH string");

		client_exitsession = xstrdup(data);
		client_exittype = imsg->hdr.type;
		if (imsg->hdr.type == MSG_DETACHKILL)
			client_exitreason = CLIENT_EXIT_DETACHED_HUP;
		else
			client_exitreason = CLIENT_EXIT_DETACHED;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXEC:
		if (datalen == 0 || data[datalen - 1] != '\0' || strlen(data) == datalen - 1)
			fatalx("bad MSG_EXEC string");
		client_execstr = xstrdup(data);
		client_execshell = xstrdup(data + strlen(data) + 1);
		client_exittype = imsg->hdr.type;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXIT:
		if (datalen != 0 && datalen != sizeof (int))
			fatalx("bad MSG_EXIT size");

		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		client_exitreason = CLIENT_EXIT_EXITED;
		break;
	case MSG_EXITED:
		if (datalen != 0)
			fatalx("bad MSG_EXITED size");

		proc_exit(client_proc);
		break;
	case MSG_SHUTDOWN:
		if (datalen != 0)
			fatalx("bad MSG_SHUTDOWN size");

		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		client_exitreason = CLIENT_EXIT_SERVER_EXITED;
		client_exitval = 1;
		break;
	case MSG_SUSPEND:
		if (datalen != 0)
			fatalx("bad MSG_SUSPEND size");

		memset(&sigact, 0, sizeof sigact);
		sigemptyset(&sigact.sa_mask);
		sigact.sa_flags = SA_RESTART;
		sigact.sa_handler = SIG_DFL;
		if (sigaction(SIGTSTP, &sigact, NULL) != 0)
			fatal("sigaction failed");
		kill(getpid(), SIGTSTP);
		break;
	case MSG_LOCK:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_LOCK string");

		system(data);
		proc_send(client_peer, MSG_UNLOCK, -1, NULL, 0);
		break;
	}
}
