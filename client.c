/*
 * client2.c - pa2 part 2
 *
 * usage:
 *   client2 [host] port <- connects as group leader
 *   client2 [host] port member <- connects as group member
 *
 * leader flow: QS|ADMIN -> GROUP|name|size ->  WAIT -> quiz
 * member flow: QS|JOIN -> JOIN|name -> WAIT ->  quiz
 * rejected: QS|FULL -> (server closes)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define BUFSIZE 4096        /* general-purpose line/message buffer size      */
#define MAX_QTEXT 2048      /* upper bound on a single question's text       */
#define TIMEOUT_SECS 8      /* per-question answer deadline (client-side)    */

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif

/* Declared here rather than via a header; defined in connectsock.c  */
/* and linked in through libsocklib.a.                               */
int connectsock(char *host, char *service, char *protocol);

/* Global TCP fd to the server. Set once by connectsock() in main(), */
/* used by every helper below so they don't each take a sock param.  */
static int csock;

/* write() may return short, loop until all `len` bytes are sent.    */
static void send_all(const char *buf, int len)
{
	int sent = 0;
	while (sent < len)
	{
		int n = write(csock, buf + sent, len - sent);
		if (n <= 0)
			return;          /* server closed / error, bail silently  */
		sent += n;
	}
}

/* Read exactly n bytes (used for fixed-length question payloads).   */
/* Returns -1 if the socket closes before n bytes arrive.            */
static int read_exact(char *buf, int n)
{
	int total = 0;
	while (total < n)
	{
		int r = read(csock, buf + total, n - total);
		if (r <= 0)
			return -1;
		total += r;
	}
	return total;
}

/* Read one CRLF-terminated line from the server.                    */
/* Byte-at-a-time so we don't over-read into the next message.       */
/* Returns length, 0 on clean EOF, -1 on error.                      */
static int read_line_server(char *buf, int maxlen)
{
	int i = 0;
	while (i < maxlen - 1)
	{
		char c;
		int r = read(csock, &c, 1);
		if (r <= 0)
		{
			buf[i] = '\0';
			return r == 0 ? 0 : -1;
		}
		if (c == '\r')
			continue;        /* swallow CR, wait for LF to terminate  */
		if (c == '\n')
		{
			buf[i] = '\0';
			return i;
		}
		buf[i++] = c;
	}
	buf[i] = '\0';
	return i;
}

/* Read one line from stdin, but give up after `timeout_secs`.       */
/* Used so an idle user auto-loses the question instead of hanging   */
/* the whole group. Returns 1 if the user typed in time, else 0.     */
static int read_stdin_timed(char *buf, int maxlen, int timeout_secs)
{
	fd_set fds;
	struct timeval tv;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	tv.tv_sec = timeout_secs;
	tv.tv_usec = 0;
	/* select() blocks until stdin is readable or the timeout fires  */
	int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
	if (r <= 0)
		return 0;            /* timeout or error, caller sends NOANS  */
	if (fgets(buf, maxlen, stdin) == NULL)
		return 0;
	/* Strip trailing CR/LF so the answer matches cleanly server-side */
	int len = (int)strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';
	return 1;
}

/* ------------------------------------------------------------------ */
/*  quiz loop, shared by leader and member after handshake            */
/* ------------------------------------------------------------------ */

static void run_quiz_client(void)
{
	char buf[BUFSIZE];
	int question_num = 0;
	int announced = 0; /* print "All players joined!" once on first QUES */

	/* Dispatch loop: server sends QUES..., WIN..., RESULT... in order */
	for (;;)
	{
		/* Parse the command word (everything up to the first '|')     */
		char cmd[32];
		int ci = 0;
		char c = 0;
		while (ci < (int)sizeof(cmd) - 1)
		{
			if (read(csock, &c, 1) <= 0)
			{
				/* Socket closed mid-message: server crashed or quit   */
				printf("\nclient: server closed connection\n");
				return;
			}
			if (c == '|' || c == '\r')
				break;
			cmd[ci++] = c;
		}
		cmd[ci] = '\0';

		/* ---- QUES|<textlen>|<text...> ---- */
		/* Length-prefixed so the text itself may contain newlines.    */
		if (strcmp(cmd, "QUES") == 0 && c == '|')
		{
			if (!announced)
			{
				/* First QUES implicitly tells us the group filled up  */
				printf("All players joined! Quiz starting...\n");
				fflush(stdout);
				announced = 1;
			}
			question_num++;

			/* Read the ASCII size field up to the next '|'            */
			char sizestr[32];
			int si = 0;
			while (si < (int)sizeof(sizestr) - 1)
			{
				if (read(csock, &c, 1) <= 0)
					return;
				if (c == '|')
					break;
				sizestr[si++] = c;
			}
			sizestr[si] = '\0';
			int textlen = atoi(sizestr);
			if (textlen <= 0 || textlen > MAX_QTEXT)
			{
				/* Defensive: bad size means we can't re-sync the      */
				/* stream, so we bail instead of guessing.             */
				fprintf(stderr, "client: bad QUES size %d\n", textlen);
				return;
			}

			/* Pull exactly `textlen` bytes, no CRLF terminator        */
			char qtext[MAX_QTEXT + 1];
			if (read_exact(qtext, textlen) != textlen)
				return;
			qtext[textlen] = '\0';

			printf("\n=== Question %d ===\n%s\n", question_num, qtext);
			printf("Your answer [%d sec]: ", TIMEOUT_SECS);
			fflush(stdout);

			/* Block for user input, but only until the local deadline */
			char answer[64];
			if (read_stdin_timed(answer, sizeof(answer), TIMEOUT_SECS) &&
				strlen(answer) > 0)
			{
				/* user typed, `answer` already populated */
			}
			else
			{
				/* Sentinel recognized by server as "no answer given"  */
				strcpy(answer, "NOANS");
				printf("\nTime's up!\n");
				fflush(stdout);
			}

			/* Reply with ANS|<answer>\r\n. Server's reader_thread     */
			/* for this client is already blocked reading this.        */
			snprintf(buf, sizeof(buf), "ANS|%s\r\n", answer);
			send_all(buf, (int)strlen(buf));
		}
		/* ---- WIN|<name>  or  WIN|   (nobody got it right) ---- */
		else if (strcmp(cmd, "WIN") == 0 && c == '|')
		{
			if (read_line_server(buf, sizeof(buf)) < 0)
				return;
			if (strlen(buf) > 0)
				printf("=== Correct! First right answer: %s ===\n", buf);
			else
				printf("=== No one got it right. ===\n");
			fflush(stdout);
		}
		/* ---- RESULT|name|score|name|score...   (end of quiz) ---- */
		else if (strcmp(cmd, "RESULT") == 0 && c == '|')
		{
			if (read_line_server(buf, sizeof(buf)) < 0)
				return;

			printf("\n=== Quiz Over! Final Standings ===\n");
			/* Walk the buffer pulling name|score pairs in order.      */
			/* Server already sorted by score descending.              */
			int rank = 1;
			char *p = buf;
			while (*p)
			{
				char *pipe1 = strchr(p, '|');
				if (!pipe1)
				{
					/* No '|' left: last field is a bare name (no score)*/
					printf("%d. %s\n", rank, p);
					break;
				}
				*pipe1 = '\0';
				char *name = p;
				char *rest = pipe1 + 1;
				char *pipe2 = strchr(rest, '|');
				char *score;
				if (pipe2)
				{
					/* Another pair follows, cut `score` out of `rest` */
					*pipe2 = '\0';
					score = rest;
					p = pipe2 + 1;
				}
				else
				{
					/* Last pair, `rest` IS the score                  */
					score = rest;
					p = rest + strlen(rest);
				}
				printf("%d. %-20s %s pts\n", rank++, name, score);
			}
			fflush(stdout);
			break;          /* RESULT terminates the quiz loop         */
		}
		else
		{
			/* Unknown command, try to resync by draining to next LF   */
			fprintf(stderr, "client: unexpected command '%s', skipping\n", cmd);
			if (c != '\r')
			{
				while (read(csock, &c, 1) > 0 && c != '\n')
					;
			}
			else
			{
				/* We already consumed '\r', swallow the trailing '\n' */
				read(csock, &c, 1);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
	char buf[BUFSIZE];
	char *host = "localhost";          /* default if user didn't pass one */
	char *service;
	int is_member = 0;

	/*
	 *  accept:
	 *      client2 port
	 *      client2 host port
	 *      client2 port member
	 *      client2 host port member
	 */
	if (argc == 2)
	{
		service = argv[1];
	}
	else if (argc == 3)
	{
		/* argv[2] is either the literal "member" flag or a port       */
		if (strcmp(argv[2], "member") == 0)
		{
			service = argv[1];
			is_member = 1;
		}
		else
		{
			host = argv[1];
			service = argv[2];
		}
	}
	else if (argc == 4)
	{
		host = argv[1];
		service = argv[2];
		is_member = (strcmp(argv[3], "member") == 0);
	}
	else
	{
		fprintf(stderr, "usage: client2 [host] port [member]\n");
		exit(1);
	}

	/* Hand off socket setup to connectsock(). On return, csock is a  */
	/* fully connected TCP fd, the matching fd on the server side is  */
	/* owned by a freshly spawned client_handler thread in server.c.  */
	if ((csock = connectsock(host, service, "tcp")) == 0)
	{
		fprintf(stderr, "client2: cannot connect to %s:%s\n", host, service);
		exit(1);
	}

	/* The server speaks first. Its greeting tells us our role:       */
	/*   QS|ADMIN  -> we were accepted as the group leader            */
	/*   QS|JOIN   -> we joined as a member of an in-progress group   */
	/*   QS|FULL   -> group is full / quiz running, go away           */
	if (read_line_server(buf, sizeof(buf)) <= 0)
	{
		fprintf(stderr, "client2: server disconnected during handshake\n");
		close(csock);
		exit(1);
	}

	/* ---- QS|FULL ---- */
	if (strcmp(buf, "QS|FULL") == 0)
	{
		printf("Server is busy (group full or quiz in progress). Try again later.\n");
		close(csock);
		exit(0);
	}

	/* ---- QS|ADMIN (leader) ---- */
	if (strcmp(buf, "QS|ADMIN") == 0)
	{
		printf("=== Welcome to da quiz (Leader) ===\n");
		printf("Your name: ");
		fflush(stdout);

		char name[256];
		if (fgets(name, sizeof(name), stdin) == NULL)
		{
			close(csock);
			exit(1);
		}
		/* Strip trailing newline chars from fgets input               */
		int nlen = strlen(name);
		while (nlen > 0 && (name[nlen - 1] == '\n' || name[nlen - 1] == '\r'))
			name[--nlen] = '\0';

		/* Leader alone decides how many players the group will have  */
		int group_size = 1;
		printf("Group size (how many players total, including you): ");
		fflush(stdout);
		char gs_buf[32];
		if (fgets(gs_buf, sizeof(gs_buf), stdin) != NULL)
			group_size = atoi(gs_buf);
		if (group_size < 1)
			group_size = 1;

		/* Send GROUP|<name>|<size>. Server parses this and starts    */
		/* turning away over-capacity connections with QS|FULL.       */
		snprintf(buf, sizeof(buf), "GROUP|%s|%d\r\n", name, group_size);
		send_all(buf, strlen(buf));

		printf("Waiting for %d player(s) to join...\n", group_size - 1);
		fflush(stdout);

		/* Server replies WAIT to acknowledge, then parks us until    */
		/* all members have joined and the quiz is ready to start.    */
		if (read_line_server(buf, sizeof(buf)) < 0)
		{
			close(csock);
			exit(1);
		}
		if (strcmp(buf, "WAIT") != 0)
		{
			fprintf(stderr, "client2: expected WAIT, got '%s'\n", buf);
			close(csock);
			exit(1);
		}

		printf("Waiting for all players to join...\n");
		fflush(stdout);

		run_quiz_client();          /* blocks until RESULT received   */

	}
	/* ---- QS|JOIN (member) ---- */
	else if (strcmp(buf, "QS|JOIN") == 0)
	{
		printf("=== Welcome to da quiz (Member) ===\n");
		printf("Your name: ");
		fflush(stdout);

		char name[256];
		if (fgets(name, sizeof(name), stdin) == NULL)
		{
			close(csock);
			exit(1);
		}
		int nlen = strlen(name);
		while (nlen > 0 && (name[nlen - 1] == '\n' || name[nlen - 1] == '\r'))
			name[--nlen] = '\0';

		/* Members only announce their name, size was already fixed   */
		/* by the leader's GROUP message.                             */
		snprintf(buf, sizeof(buf), "JOIN|%s\r\n", name);
		send_all(buf, strlen(buf));

		printf("Waiting for quiz to start...\n");
		fflush(stdout);

		/* Same WAIT handshake as the leader path                     */
		if (read_line_server(buf, sizeof(buf)) < 0)
		{
			close(csock);
			exit(1);
		}
		if (strcmp(buf, "WAIT") != 0)
		{
			fprintf(stderr, "client2: expected WAIT, got '%s'\n", buf);
			close(csock);
			exit(1);
		}

		printf("Quiz starting!\n");
		fflush(stdout);

		run_quiz_client();
	}
	else
	{
		/* Protocol violation, unknown greeting = something is wrong  */
		fprintf(stderr, "client2: unexpected greeting '%s'\n", buf);
		close(csock);
		exit(1);
	}

	/* is_member is only set from argv, server's greeting is the real */
	/* source of truth. The cast silences "unused variable" warnings. */
	(void)is_member;
	close(csock);
	return 0;
}