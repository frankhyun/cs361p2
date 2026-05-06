/*
 *  server2.c - pa2 part 2
 *  multi-client quiz server: 1 thread per client, 1 group at a time
 *
 *  Architecture:
 *  accept loop - main thread, spins forever accepting connections
 *  client_handler thread - one per connection; handles join protocol then:
 *      leader: waits for group to fill, launches reader thread, drives quiz
 *      member: completes join, launches reader thread, waits for quiz_over
 *  reader thread - one per client; reads ANS from socket, posts to
 *  per-client answer slot so quiz driver can collect
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>

#define QLEN 32             /* listen() backlog, pending-connect queue depth */
#define BUFSIZE 4096        /* general-purpose line/message buffer size      */
#define MAX_QUESTIONS 100   /* upper bound on quiz file size                 */
#define MAX_QTEXT 2048      /* upper bound on one question's text            */
#define MAX_CLIENTS 64      /* hard cap on group size (g_clients[] length)   */
#define TIMEOUT_SECS 10     /* per-question read timeout, server-side        */

/* Declared here rather than via a header; defined in passivesock.c  */
/* and linked in through libsocklib.a (same archive client.c uses).  */
int passivesock(char *service, char *protocol, int qlen, int *rport);

/* ================================================================== */
/*  questions                                                         */
/* ================================================================== */

typedef struct
{
    char text[MAX_QTEXT];   /* the prompt (may contain newlines)             */
    char correct[16];       /* short answer key, compared verbatim           */
} Question;

/* Loaded once at startup from the quiz file, then read-only by all threads  */
static Question questions[MAX_QUESTIONS];
static int num_questions = 0;

/*
 * Parse the quiz file into `questions[]`.
 *
 * File format: each question is
 *      <prompt line(s)>
 *      <blank line>
 *      <correct answer>
 *      <blank line>
 * Use a tiny state machine so multi-line prompts work:
 *      0 = expecting first line of a new prompt
 *      1 = inside a prompt, accumulating lines until blank
 *      2 = expecting the answer line
 *      3 = expecting the trailing blank that closes the question
 */
static int load_questions(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f)
    {
        perror(filename);
        return -1;
    }

    char line[512];
    int state = 0, qi = -1;

    while (fgets(line, sizeof(line), f))
    {
        int len = strlen(line);
        int blank = (len == 0 || line[0] == '\n' || line[0] == '\r');
        switch (state)
        {
        case 0:             /* waiting for a non-blank line to start a Q     */
            if (blank)
                continue;
            qi = num_questions;
            if (qi >= MAX_QUESTIONS)
                goto done;  /* file has more questions than we can hold     */
            memset(&questions[qi], 0, sizeof(questions[qi]));
            strncat(questions[qi].text, line,
                    MAX_QTEXT - 1 - (int)strlen(questions[qi].text));
            state = 1;
            break;
        case 1:             /* inside prompt, blank line ends it             */
            if (blank)
                state = 2;
            else
                strncat(questions[qi].text, line,
                        MAX_QTEXT - 1 - (int)strlen(questions[qi].text));
            break;
        case 2:             /* next non-blank line is the answer key         */
            if (blank)
                continue;
            sscanf(line, "%15s", questions[qi].correct);
            state = 3;
            break;
        case 3:             /* trailing blank commits this question          */
            if (blank)
            {
                num_questions++;
                state = 0;
            }
            break;
        }
    }
    /* File may end without a trailing blank, commit the last one anyway     */
    if (state == 3)
        num_questions++;
done:
    fclose(f);
    return num_questions;
}

/* ================================================================== */
/*  I/O helpers                                                       */
/* ================================================================== */

/* write() may return short, loop until all `len` bytes are sent.   */
/* Takes `sock` explicitly, unlike the client version, because the  */
/* server writes to many different sockets (one per client).        */
static void send_all(int sock, const char *buf, int len)
{
    int sent = 0;
    while (sent < len)
    {
        int n = write(sock, buf + sent, len - sent);
        if (n <= 0)
            return;         /* client disconnected mid-send, drop it */
        sent += n;
    }
}

/*
 * read one CRLF-terminated line from sock.
 * returns: chars stored (>=0), -1 on error/disconnect, -2 on timeout.
 * timeout_secs == 0  ->  wait forever.
 *
 * Byte-at-a-time is intentional, it prevents us from over-reading
 * into the next message while also letting us enforce a per-read
 * timeout via select() before each byte.
 */
static int read_crlf_line(int sock, char *buf, int maxlen, int timeout_secs)
{
    int i = 0;
    while (i < maxlen - 1)
    {
        if (timeout_secs > 0)
        {
            /* Before every single-byte read, wait up to the timeout */
            /* with select(). This turns each read into a bounded op */
            /* instead of a potentially-blocking one.                */
            fd_set fds;
            struct timeval tv;
            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            tv.tv_sec = timeout_secs;
            tv.tv_usec = 0;
            int r = select(sock + 1, &fds, NULL, NULL, &tv);
            if (r == 0)
            {
                /* select returned 0 -> no data in time = timeout    */
                buf[i] = '\0';
                return -2;
            }
            if (r < 0)
            {
                buf[i] = '\0';
                return -1;
            }
        }
        char c;
        int n = read(sock, &c, 1);
        if (n <= 0)
        {
            /* 0 = peer closed, <0 = error. Either way, disconnect.  */
            buf[i] = '\0';
            return -1;
        }
        if (c == '\r')
            continue;       /* swallow CR, wait for LF to terminate  */
        if (c == '\n')
        {
            buf[i] = '\0';
            return i;
        }
        buf[i++] = c;
    }
    /* Line longer than buffer, return what we have (truncated)      */
    buf[i] = '\0';
    return i;
}

/* ================================================================== */
/*  per-client state                                                  */
/* ================================================================== */

typedef struct
{
    int sock;               /* fd returned by accept() for this client      */
    char name[256];         /* display name, from GROUP|.. or JOIN|..       */
    int score;               /* running score over the whole quiz            */

    /*
     * Answer slot: shared between this client's reader_thread (writer)
     * and the quiz driver (reader). ans_mu guards the whole slot.
     *
     * Lifecycle per question:
     *   1. quiz driver sets ans_ready = 0, signals cv -> wakes reader.
     *   2. reader reads ANS from socket (with timeout), stores answer,
     *      sets ans_ready = 1, signals cv.
     *   3. quiz driver waits (timedwait) for ans_ready == 1, then reads.
     */
    pthread_mutex_t ans_mu;
    pthread_cond_t ans_cv;
    int ans_ready;          /* 0 = pending, 1 = answer stored               */
    char ans_buf[64];       /* the answer string (default "NOANS")          */
    struct timeval ans_time; /* time stamp for tie-break*/

    int aborted;            /* 1 = socket gone, skip further I/O on this cl */
} Client;

/* ================================================================== */
/*  global group / server state                                       */
/* ================================================================== */

/*
 * The server has a single global "current group". Extra connections
 * that arrive while SRV_ASSEMBLING is full, or while SRV_QUIZZING,
 * get QS|FULL'd and dropped immediately.
 */
typedef enum
{
    SRV_IDLE,               /* no group yet, first connection becomes leader*/
    SRV_ASSEMBLING,         /* leader has set size, waiting for members     */
    SRV_QUIZZING            /* quiz in progress, reject new connections     */
} SrvState;

/* g_mu protects every global below; g_cv wakes handlers on state changes  */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static SrvState g_state = SRV_IDLE;
static int g_group_size = 0;           /* target size set by leader         */
static int g_joined = 0;               /* current member count incl. leader */
static Client *g_clients[MAX_CLIENTS]; /* parallel array: index = join order*/
static int g_quiz_over = 0; /* set by run_quiz() when done, wakes members  */

/* ================================================================== */
/*  reader thread - one per client                                    */
/* ================================================================== */

static void *reader_thread(void *arg)
{
    Client *cl = (Client *)arg;
    char buf[BUFSIZE];

    /*
     * One iteration per question. The handler thread created us and
     * then went off to either drive the quiz (leader) or wait for it
     * to end (member). We are the only thing reading this socket.
     */
    for (;;)
    {
        /*
         * Park until the quiz driver "arms" us for a new question by
         * setting ans_ready = 0. Client structs are created with
         * ans_ready = 1 precisely so the first trip through this loop
         * blocks here until the first QUES has been broadcast.
         */
        pthread_mutex_lock(&cl->ans_mu);
        while (cl->ans_ready)
        {
            /* Check shutdown flag before sleeping so we don't miss it*/
            pthread_mutex_lock(&g_mu);
            int over = g_quiz_over;
            pthread_mutex_unlock(&g_mu);
            if (over)
            {
                pthread_mutex_unlock(&cl->ans_mu);
                return NULL;
            }
            pthread_cond_wait(&cl->ans_cv, &cl->ans_mu);
        }
        pthread_mutex_unlock(&cl->ans_mu);

        /* Re-check quiz_over now that we're awake (might have been  */
        /* woken by run_quiz()'s final broadcast, not a new question)*/
        pthread_mutex_lock(&g_mu);
        int over = g_quiz_over;
        pthread_mutex_unlock(&g_mu);
        if (over)
            return NULL;

        if (cl->aborted)
            return NULL;

        /*
         * Read one ANS| line with a slightly longer timeout than the
         * client's own deadline so we don't declare NOANS before the
         * client has had a chance to send its answer over the wire.
         */
        // Reader Timeout
        // Returns -2 if the reader times out, answer should stay "NOANS".
        int r = read_crlf_line(cl->sock, buf, sizeof(buf), TIMEOUT_SECS + 3);

        char ans[64];
        strcpy(ans, "NOANS"); /* default if anything goes wrong       */
        // Nothing went wrong.
        if (r > 0 && strncmp(buf, "ANS|", 4) == 0)
        {
            /* Strip "ANS|" prefix, trim trailing whitespace          */
            strncpy(ans, buf + 4, sizeof(ans) - 1);
            ans[sizeof(ans) - 1] = '\0';
            int al = strlen(ans);
            while (al > 0 && (ans[al - 1] == ' ' || ans[al - 1] == '\t'))
                ans[--al] = '\0';
        }
        // Error
        else if (r == -1)
        {
            /* Socket closed or errored, mark client dead so the quiz*/
            /* driver skips it for the rest of the session.          */
            cl->aborted = 1;
        }
        /* r == -2 -> read timed out, ans stays "NOANS"              */

        /* Publish the answer to the quiz driver and wake it         */

        // RACE CONDITION 3: Reader thread.
        // Writes "CORRECT\0"
        pthread_mutex_lock(&cl->ans_mu);
        strncpy(cl->ans_buf, ans, sizeof(cl->ans_buf) - 1);
        cl->ans_buf[sizeof(cl->ans_buf) - 1] = '\0';
        gettimeofday(&cl->ans_time, NULL); /* for first-correct tiebreak*/
        cl->ans_ready = 1;
        pthread_cond_signal(&cl->ans_cv);
        pthread_mutex_unlock(&cl->ans_mu);

        if (cl->aborted)
            return NULL;
    }
}

/* ================================================================== */
/*  quiz driver, called by the leader's handler thread after group    */
/*  is formed, blocks until all questions are done and results sent   */
/* ================================================================== */

static void run_quiz(void)
{
    char buf[BUFSIZE];

    /* Snapshot player count once, group_size is frozen at this point */
    pthread_mutex_lock(&g_mu);
    int n = g_joined;
    pthread_mutex_unlock(&g_mu);

    /* ---- outer loop: one iteration per quiz question ---- */
    for (int qi = 0; qi < num_questions; qi++)
    {
        /* Arm every answer slot so the reader threads know a new    */
        /* question is coming. They each wake up, block on read()    */
        /* of their socket, and will post an ANS back into the slot. */
        // Phase 1: Server Waits for all answers before next question
        for (int c = 0; c < n; c++)
        {
            Client *cl = g_clients[c];
            pthread_mutex_lock(&cl->ans_mu);
            cl->ans_ready = 0; // slot empty waiting for answer
            strcpy(cl->ans_buf, "NOANS");
            pthread_cond_signal(&cl->ans_cv); // wait reader, new question ready
            pthread_mutex_unlock(&cl->ans_mu);
        }

        /* Broadcast QUES|size|text to every live client. Size is    */
        /* sent explicitly so the client can read the exact payload  */
        /* even if the question text itself contains newlines.       */
        // Broadcast question
        int textlen = (int)strlen(questions[qi].text);
        int hlen = snprintf(buf, sizeof(buf), "QUES|%d|", textlen);
        for (int c = 0; c < n; c++)
        {
            Client *cl = g_clients[c];
            if (!cl->aborted)
            {
                send_all(cl->sock, buf, hlen);
                send_all(cl->sock, questions[qi].text, textlen);
            }
        }

        /* Collect answers within a single wall-clock deadline.      */
        /* Each client has its own reader_thread doing the actual    */
        /* read, so all sockets are drained in parallel. We then     */
        /* score by ans_time timestamps, so the first correct answer */
        /* on the wire wins even if we visit clients in index order. */
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        struct timespec deadline;
        deadline.tv_sec = tv_now.tv_sec + TIMEOUT_SECS + 4;
        deadline.tv_nsec = tv_now.tv_usec * 1000;

        /* Wait for each client to post, or for the shared deadline  */
        /* to fire. Because the deadline is absolute, clients we     */
        /* wait on later don't get extra time.                       */
        // Collect answers from broadcast
        for (int c = 0; c < n; c++)
        {
            Client *cl = g_clients[c];
            if (cl->aborted)
                continue;
            pthread_mutex_lock(&cl->ans_mu);
            while (!cl->ans_ready && !cl->aborted)
            {
                int rc = pthread_cond_timedwait(&cl->ans_cv, &cl->ans_mu,
                                                &deadline);
                if (rc == ETIMEDOUT)
                    break;      /* out of time, ans stays NOANS     */
            }
            pthread_mutex_unlock(&cl->ans_mu);
        }

        /* Scoring pass: find earliest correct answer by timestamp.  */
        /* first_correct winner gets +2, later correct get +1, wrong */
        /* gets -1, NOANS gets 0.                                    */
        int first_correct = -1;
        struct timeval best_time = {0, 0};

        for (int c = 0; c < n; c++)
        {
            Client *cl = g_clients[c];
            char answer[64];
            /* Copy out under lock to avoid racing with reader_thread*/

            // RACE CONDITION 3: Torn Read/ Write on Answer Buffer
            // Reads answer
            pthread_mutex_lock(&cl->ans_mu);
            strncpy(answer, cl->ans_buf, sizeof(answer) - 1);
            answer[sizeof(answer) - 1] = '\0';
            struct timeval at = cl->ans_time;
            pthread_mutex_unlock(&cl->ans_mu);

            int is_noans = (strcmp(answer, "NOANS") == 0);
            int is_correct = !is_noans &&
                             (strcmp(answer, questions[qi].correct) == 0);
            int is_wrong = !is_noans && !is_correct;

            fprintf(stderr,
                    "server: Q%d  %-16s answer='%-6s' correct='%s' -> %s\n",
                    qi + 1, cl->name, answer, questions[qi].correct,
                    is_correct ? "CORRECT" : (is_noans ? "NOANS" : "WRONG"));

            if (is_correct)
            {
                /* Is this the earliest correct answer so far?       */
                if (first_correct < 0 ||
                    at.tv_sec < best_time.tv_sec ||
                    (at.tv_sec == best_time.tv_sec &&
                     at.tv_usec < best_time.tv_usec))
                {
                    /* Replace previous winner: they get the +1      */
                    /* consolation, new winner gets the +2.          */
                    if (first_correct >= 0)
                        g_clients[first_correct]->score -= 2;
                    first_correct = c;
                    best_time = at;
                    cl->score += 2;
                }
                else
                    cl->score += 1;   /* correct but not first       */
            }
            else if (is_wrong)
            {
                cl->score -= 1;       /* NOANS is 0, wrong costs 1   */
            }
        }

        /* Tell everyone who won this round (empty name = nobody).   */
        if (first_correct >= 0)
            snprintf(buf, sizeof(buf), "WIN|%s\r\n",
                     g_clients[first_correct]->name);
        else
            snprintf(buf, sizeof(buf), "WIN|\r\n");

        for (int c = 0; c < n; c++)
        {
            Client *cl = g_clients[c];
            if (!cl->aborted)
                send_all(cl->sock, buf, strlen(buf));
        }
    }

    /* ---- end of quiz: build final scoreboard ---- */
    /* Insertion sort into `sorted` by score descending. Small N     */
    /* (<= MAX_CLIENTS = 64) so O(n^2) is fine.                      */
    Client *sorted[MAX_CLIENTS];
    for (int i = 0; i < n; i++)
        sorted[i] = g_clients[i];
    for (int i = 1; i < n; i++)
    {
        Client *key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j]->score < key->score)
        {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    /* Serialize as RESULT|name1|score1|name2|score2|...\r\n         */
    /* Client parses this into the final standings display.          */
    char result[BUFSIZE];
    int rlen = snprintf(result, sizeof(result), "RESULT");
    for (int i = 0; i < n; i++)
        rlen += snprintf(result + rlen, sizeof(result) - rlen,
                         "|%s|%d", sorted[i]->name, sorted[i]->score);
    rlen += snprintf(result + rlen, sizeof(result) - rlen, "\r\n");

    for (int c = 0; c < n; c++)
    {
        Client *cl = g_clients[c];
        if (!cl->aborted)
            send_all(cl->sock, result, rlen);
        fprintf(stderr, "server: final  %-16s scored %d\n",
                cl->name, cl->score);
    }

    /* Unblock reader threads from their "wait for next question"    */
    /* loop by posting ans_ready = 1. They will then observe         */
    /* g_quiz_over below and exit cleanly.                           */
    for (int c = 0; c < n; c++)
    {
        Client *cl = g_clients[c];
        pthread_mutex_lock(&cl->ans_mu);
        cl->ans_ready = 1;
        pthread_cond_signal(&cl->ans_cv);
        pthread_mutex_unlock(&cl->ans_mu);
    }

    /* Flip the global "quiz is over" flag, broadcasting on g_cv     */
    /* wakes the member handler threads that have been parked on it. */
    pthread_mutex_lock(&g_mu);
    g_quiz_over = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

/* ================================================================== */
/*  client_handler thread                                             */
/* ================================================================== */

/* Small heap wrapper so main() can pass the accepted fd to a new   */
/* thread without racing on a stack variable.                        */
typedef struct
{
    int sock;
} AcceptArg;

static void *client_handler(void *arg)
{
    AcceptArg *aa = (AcceptArg *)arg;
    int sock = aa->sock;
    free(aa);                   /* done with the wrapper immediately */

    char buf[BUFSIZE];

    /* Fast reject without heavy work: if we're full or mid-quiz,    */
    /* just say QS|FULL and drop the connection. No allocation,      */
    /* no Client struct, no reader thread.                           */
    pthread_mutex_lock(&g_mu);
    SrvState cur = g_state;
    int full = (cur == SRV_ASSEMBLING && g_group_size > 0 &&
                g_joined >= g_group_size);
    int quiz = (cur == SRV_QUIZZING);
    pthread_mutex_unlock(&g_mu);

    if (full || quiz)
    {
        send_all(sock, "QS|FULL\r\n", 9);
        close(sock);
        return NULL;
    }

    /* Role decision under lock: whoever finds SRV_IDLE becomes the  */
    /* leader and transitions the server into SRV_ASSEMBLING.        */

    // RACE CONDITION 1: Two Leaders
    // Without lock both threads think they're leaders.
    pthread_mutex_lock(&g_mu);
    int is_leader = (g_state == SRV_IDLE); // First connection is leader.

    if (is_leader)
    {
        /* Claim the idle server as our group, reset counters        */
        g_state = SRV_ASSEMBLING;
        g_group_size = 0;
        g_joined = 0;
        g_quiz_over = 0;
    }
    else if (g_state == SRV_ASSEMBLING) // Leader is set, waiting for members.
    {
        /* Recheck space, between the fast reject and now a race     */
        /* could have filled the group.                              */
        if (g_group_size == 0 ||
            g_joined >= g_group_size)
        {
            pthread_mutex_unlock(&g_mu);
            send_all(sock, "QS|FULL\r\n", 9);
            close(sock);
            return NULL;
        }
    }
    else
    {
        /* SRV_QUIZZING, turn away                                   */
        pthread_mutex_unlock(&g_mu);
        send_all(sock, "QS|FULL\r\n", 9);
        close(sock);
        return NULL;
    }
    pthread_mutex_unlock(&g_mu);

    /* Allocate per-client state. ans_ready = 1 means "already       */
    /* answered", so the reader_thread we launch later will block on */
    /* its cond-var until run_quiz() arms it with ans_ready = 0.     */
    Client *cl = calloc(1, sizeof(Client));
    if (!cl)
    {
        close(sock);
        return NULL;
    }
    cl->sock = sock;
    cl->ans_ready = 1;
    pthread_mutex_init(&cl->ans_mu, NULL);
    pthread_cond_init(&cl->ans_cv, NULL);
    strcpy(cl->name, "Unknown");

    /* ----------------------------------------------------------------
     * LEADER PATH
     * This handler thread drives the entire quiz lifecycle for the
     * group: waits for members, runs the quiz, cleans everyone up.
     * ---------------------------------------------------------------- */
    if (is_leader)
    {
        /* Greet: "you are the admin, send me a GROUP message"       */
        send_all(sock, "QS|ADMIN\r\n", 10);

        /* Wait up to 30s for the client's GROUP|name|size reply     */
        if (read_crlf_line(sock, buf, sizeof(buf), 30) <= 0)
        {
            /* Client vanished during handshake, release the server  */
            pthread_mutex_lock(&g_mu);
            g_state = SRV_IDLE;
            pthread_mutex_unlock(&g_mu);
            pthread_mutex_destroy(&cl->ans_mu);
            pthread_cond_destroy(&cl->ans_cv);
            free(cl);
            close(sock);
            return NULL;
        }
        if (strncmp(buf, "GROUP|", 6) != 0)
        {
            /* Wrong message, same teardown                          */
            pthread_mutex_lock(&g_mu);
            g_state = SRV_IDLE;
            pthread_mutex_unlock(&g_mu);
            pthread_mutex_destroy(&cl->ans_mu);
            pthread_cond_destroy(&cl->ans_cv);
            free(cl);
            close(sock);
            return NULL;
        }

        /* Parse GROUP|<name>|<size>. Use strrchr so names can still */
        /* contain '|' if they must, the last '|' separates size.    */
        char *body = buf + 6;
        char *pipe = strrchr(body, '|');
        int sz = 1;
        if (pipe)
        {
            *pipe = '\0';
            sz = atoi(pipe + 1);
            if (sz < 1 || sz > MAX_CLIENTS)
                sz = 1;          /* clamp to sane range              */
        }
        strncpy(cl->name, body, sizeof(cl->name) - 1);
        cl->name[sizeof(cl->name) - 1] = '\0';

        /* Publish the group parameters so member handlers can see   */
        /* there's room to join.                                     */
        pthread_mutex_lock(&g_mu);
        g_group_size = sz;
        g_clients[0] = cl;       /* leader is always index 0         */
        g_joined = 1;
        pthread_mutex_unlock(&g_mu);

        fprintf(stderr, "server: leader '%s' wants group of %d\n",
                cl->name, sz);

        /* Tell client to sit tight while we gather members          */
        send_all(sock, "WAIT\r\n", 6);

        /* Block until every member's handler has joined and signalled*/
        // Quiz starts only when the group is full.
        // Unlock g_mu, put thread to sleep, re-lock g_mu
        pthread_mutex_lock(&g_mu);
        while (g_joined < g_group_size)
            pthread_cond_wait(&g_cv, &g_mu);
        g_state = SRV_QUIZZING;  /* hard-close the door              */
        pthread_mutex_unlock(&g_mu);

        fprintf(stderr, "server: group complete (%d), starting quiz\n",
                g_joined);

        /* Start the leader's own reader_thread. Member handlers     */
        /* already started theirs at join time.                      */
        pthread_t rtid;
        pthread_create(&rtid, NULL, reader_thread, cl);
        pthread_detach(rtid);

        /* Run the whole quiz synchronously on this thread. Returns  */
        /* only after RESULT has been broadcast and quiz_over set.   */
        run_quiz();

        /* Small pause so reader threads can wake on quiz_over and   */
        /* exit cleanly before we yank their sockets out from under. */
        usleep(300000);

        /* Close all client sockets, this also kicks any reader that */
        /* is still blocked in read().                               */
        pthread_mutex_lock(&g_mu);
        int n = g_joined;
        pthread_mutex_unlock(&g_mu);
        for (int c = 0; c < n; c++)
            close(g_clients[c]->sock);

        usleep(100000);          /* second short pause post-close    */

        /* Free every Client struct (including ours) and reset the   */
        /* global state back to SRV_IDLE. Broadcast wakes any member */
        /* handler still parked on g_cv waiting for quiz_over.       */
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < g_joined; i++)
        {
            pthread_mutex_destroy(&g_clients[i]->ans_mu);
            pthread_cond_destroy(&g_clients[i]->ans_cv);
            free(g_clients[i]);
            g_clients[i] = NULL;
        }
        g_joined = 0;
        g_group_size = 0;
        g_quiz_over = 0;
        g_state = SRV_IDLE;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);

        fprintf(stderr, "server: ready for next group\n");
        return NULL;
    }

    /* ----------------------------------------------------------------
     * MEMBER PATH
     * Register ourselves in g_clients[], answer
     * questions via our reader_thread, then wait for the leader's
     * handler to tear everything down.
     * ---------------------------------------------------------------- */

    /* Final space check under lock, then claim our slot             */

    // RACE CONDITION 2: Array Slot Collision.
    // Two members join at the same time, both think they're at index 1.
    pthread_mutex_lock(&g_mu);
    if (g_state != SRV_ASSEMBLING || g_group_size == 0 ||
        g_joined >= g_group_size)
    {
        pthread_mutex_unlock(&g_mu);
        send_all(sock, "QS|FULL\r\n", 9);
        pthread_mutex_destroy(&cl->ans_mu);
        pthread_cond_destroy(&cl->ans_cv);
        free(cl);
        close(sock);
        return NULL;
    }
    int idx = g_joined;
    g_clients[idx] = cl;
    g_joined++;
    int group_full = (g_joined >= g_group_size); /* did we complete it?*/
    pthread_mutex_unlock(&g_mu);

    /* Greet: "you are a member, send me a JOIN message"             */
    send_all(sock, "QS|JOIN\r\n", 9);

    /* Wait for JOIN|<name>                                          */
    if (read_crlf_line(sock, buf, sizeof(buf), 30) <= 0)
    {
        /* Client gave up. Mark aborted so run_quiz() skips them.    */
        /* We do NOT free cl, it's in g_clients[], leader owns it.   */
        cl->aborted = 1;
        if (group_full)
        {
            pthread_mutex_lock(&g_mu);
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mu);
        }
        return NULL;
    }
    if (strncmp(buf, "JOIN|", 5) == 0)
        strncpy(cl->name, buf + 5, sizeof(cl->name) - 1);
    cl->name[sizeof(cl->name) - 1] = '\0';

    fprintf(stderr, "server: member '%s' joined (%d/%d)\n",
            cl->name, idx + 1, g_group_size);

    send_all(sock, "WAIT\r\n", 6);

    /* If our join completed the group, wake the leader who is       */
    /* parked on g_cv waiting for g_joined >= g_group_size.          */
    if (group_full)
    {
        pthread_mutex_lock(&g_mu);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }

    /* Launch our reader_thread. It owns our socket from now on,    */
    /* we (the handler) do not read from it anymore.                 */
    pthread_t rtid;
    pthread_create(&rtid, NULL, reader_thread, cl);
    pthread_detach(rtid);

    /* Park until run_quiz() flips g_quiz_over. Leader's handler     */
    /* does all cleanup (free, close, state reset) for everyone.     */
    pthread_mutex_lock(&g_mu);
    while (!g_quiz_over)
        pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);

    return NULL;
}

/* ================================================================== */
/*  main                                                              */
/* ================================================================== */

int main(int argc, char *argv[])
{
    char *service = NULL;
    char *quizfile;
    int rport = 0;              /* 0 = use `service`, 1 = let kernel pick */

    switch (argc)
    {
    case 2:                     /* quizfile only: kernel picks a free port */
        quizfile = argv[1];
        rport = 1;
        break;
    case 3:                     /* quizfile + explicit port                */
        quizfile = argv[1];
        service = argv[2];
        break;
    default:
        fprintf(stderr, "usage: server2 quizfile [port]\n");
        exit(1);
    }

    /* Load questions before opening the socket, if the file is bad  */
    /* we want to fail fast without bothering the network layer.     */
    if (load_questions(quizfile) <= 0)
    {
        fprintf(stderr, "server2: no questions loaded from '%s'\n", quizfile);
        exit(1);
    }
    fprintf(stderr, "server2: loaded %d question(s)\n", num_questions);

    /*
     * Ask passivesock for a listening TCP socket bound to `service`
     * (or, if rport==1, any free port, which passivesock writes back
     * into rport so we can print it). msock is the "master" socket,
     * we never read/write on it directly, only accept() on it.
     */
    int msock = passivesock(service, "tcp", QLEN, &rport);
    if (rport)
    {
        printf("server: port %d\n", rport);
        fflush(stdout);
    }

    /*
     * Accept loop. Each iteration:
     *   - blocks in accept() until a client's connectsock() completes
     *     the TCP handshake,
     *   - accept() returns ssock, a fresh fd dedicated to that one
     *     client (msock stays open for future connections),
     *   - we hand ssock off to a detached client_handler thread so
     *     main() can go right back to accepting. This is what lets
     *     the server handle many clients concurrently even though
     *     the quiz itself only runs one group at a time.
     */
    for (;;)
    {
        struct sockaddr_in fsin;
        socklen_t alen = sizeof(fsin);
        int ssock = accept(msock, (struct sockaddr *)&fsin, &alen);
        if (ssock < 0)
        {
            fprintf(stderr, "accept: %s\n", strerror(errno));
            continue;
        }

        /* Heap-allocate the arg so it outlives this stack frame */
        AcceptArg *aa = malloc(sizeof(AcceptArg));
        if (!aa)
        {
            close(ssock);
            continue;
        }
        aa->sock = ssock;

        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, aa);
        pthread_detach(tid);
    }

    return 0;
}