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

#define QLEN 32
#define BUFSIZE 4096
#define MAX_QUESTIONS 100
#define MAX_QTEXT 2048
#define MAX_CLIENTS 64
#define TIMEOUT_SECS 10

int passivesock(char *service, char *protocol, int qlen, int *rport);

/* ================================================================== */
/*  questions                                                         */
/* ================================================================== */

typedef struct
{
    char text[MAX_QTEXT];
    char correct[16];
} Question;

static Question questions[MAX_QUESTIONS];
static int num_questions = 0;

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
        case 0:
            if (blank)
                continue;
            qi = num_questions;
            if (qi >= MAX_QUESTIONS)
                goto done;
            memset(&questions[qi], 0, sizeof(questions[qi]));
            strncat(questions[qi].text, line,
                    MAX_QTEXT - 1 - (int)strlen(questions[qi].text));
            state = 1;
            break;
        case 1:
            if (blank)
                state = 2;
            else
                strncat(questions[qi].text, line,
                        MAX_QTEXT - 1 - (int)strlen(questions[qi].text));
            break;
        case 2:
            if (blank)
                continue;
            sscanf(line, "%15s", questions[qi].correct);
            state = 3;
            break;
        case 3:
            if (blank)
            {
                num_questions++;
                state = 0;
            }
            break;
        }
    }
    if (state == 3)
        num_questions++;
done:
    fclose(f);
    return num_questions;
}

/* ================================================================== */
/*  I/O helpers                                                       */
/* ================================================================== */

static void send_all(int sock, const char *buf, int len)
{
    int sent = 0;
    while (sent < len)
    {
        int n = write(sock, buf + sent, len - sent);
        if (n <= 0)
            return;
        sent += n;
    }
}

/*
 * read one CRLF-terminated line from sock.
 * returns: chars stored (>=0), -1 on error/disconnect, -2 on timeout.
 * timeout_secs == 0  ->  wait forever.
 */
static int read_crlf_line(int sock, char *buf, int maxlen, int timeout_secs)
{
    int i = 0;
    while (i < maxlen - 1)
    {
        if (timeout_secs > 0)
        {
            fd_set fds;
            struct timeval tv;
            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            tv.tv_sec = timeout_secs;
            tv.tv_usec = 0;
            int r = select(sock + 1, &fds, NULL, NULL, &tv);
            if (r == 0)
            {
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
            buf[i] = '\0';
            return -1;
        }
        if (c == '\r')
            continue;
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

/* ================================================================== */
/*  per-client state                                                  */
/* ================================================================== */

typedef struct
{
    int sock;
    char name[256];
    int score;

    /*
     * answer slot - shared between reader_thread (writer) and quiz driver
     *
     *
     * lifecycle per question:
     *   1. quiz driver sets ans_ready = 0, signals cv -> wakes reader_thread.
     *   2. reader reads from socket (with timeout), stores answer,
     *      sets ans_ready = 1, signals cv.
     *   3. quiz driver waits (timedwait) for ans_ready == 1.
     */
    pthread_mutex_t ans_mu;
    pthread_cond_t ans_cv;
    int ans_ready;          // 0 = pending, 1 = answer stored
    char ans_buf[64];       // the answer string (default "NOANS")
    struct timeval ans_time; // wall time when answer arrived

    int aborted; // 1 = socket gone
} Client;

/* ================================================================== */
/*  global group / server state                                       */
/* ================================================================== */

typedef enum
{
    SRV_IDLE,
    SRV_ASSEMBLING,
    SRV_QUIZZING
} SrvState;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static SrvState g_state = SRV_IDLE;
static int g_group_size = 0;
static int g_joined = 0;
static Client *g_clients[MAX_CLIENTS];
static int g_quiz_over = 0; // set by run_quiz() when done

/* ================================================================== */
/*  reader thread - one per client                                    */
/* ================================================================== */

static void *reader_thread(void *arg)
{
    Client *cl = (Client *)arg;
    char buf[BUFSIZE];

    for (;;)
    {
        /*
         * wait until quiz driver arms a new question by setting
         * ans_ready = 0, start with ans_ready = 1 so we block here
         * until first question is sent
         */
        pthread_mutex_lock(&cl->ans_mu);
        while (cl->ans_ready)
        {
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

        // Re-check quiz_over now that we're awake
        pthread_mutex_lock(&g_mu);
        int over = g_quiz_over;
        pthread_mutex_unlock(&g_mu);
        if (over)
            return NULL;

        if (cl->aborted)
            return NULL;

        /*
         * read ANS from socket, extra headroom over client-side timeout
         * so we don't declare NOANS before client sends
         */
        int r = read_crlf_line(cl->sock, buf, sizeof(buf), TIMEOUT_SECS + 3);

        char ans[64];
        strcpy(ans, "NOANS");
        if (r > 0 && strncmp(buf, "ANS|", 4) == 0)
        {
            strncpy(ans, buf + 4, sizeof(ans) - 1);
            ans[sizeof(ans) - 1] = '\0';
            int al = strlen(ans);
            while (al > 0 && (ans[al - 1] == ' ' || ans[al - 1] == '\t'))
                ans[--al] = '\0';
        }
        else if (r == -1)
        {
            cl->aborted = 1;
        }
        // r == -2 -> timed out, ans stays "NOANS"

        pthread_mutex_lock(&cl->ans_mu);
        strncpy(cl->ans_buf, ans, sizeof(cl->ans_buf) - 1);
        cl->ans_buf[sizeof(cl->ans_buf) - 1] = '\0';
        gettimeofday(&cl->ans_time, NULL); // record when answer arrived
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

    pthread_mutex_lock(&g_mu);
    int n = g_joined;
    pthread_mutex_unlock(&g_mu);

    for (int qi = 0; qi < num_questions; qi++)
    {

        // arm all answer slots: set ans_ready = 0, wake readers
        for (int c = 0; c < n; c++)
        {
            Client *cl = g_clients[c];
            pthread_mutex_lock(&cl->ans_mu);
            cl->ans_ready = 0;
            strcpy(cl->ans_buf, "NOANS");
            pthread_cond_signal(&cl->ans_cv);
            pthread_mutex_unlock(&cl->ans_mu);
        }

        // broadcast QUES|size|text
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

        // collect answers within a wall-clock deadline.
        // wait on ALL clients in parallel (each has its own reader thread),
        // then score by timestamp so whoever answered first actually wins.
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        struct timespec deadline;
        deadline.tv_sec = tv_now.tv_sec + TIMEOUT_SECS + 4;
        deadline.tv_nsec = tv_now.tv_usec * 1000;

        // wait for every client to post an answer (or deadline to pass)
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
                    break;
            }
            pthread_mutex_unlock(&cl->ans_mu);
        }

        // score: find who answered correctly AND earliest by timestamp
        int first_correct = -1;
        struct timeval best_time = {0, 0};

        for (int c = 0; c < n; c++)
        {
            Client *cl = g_clients[c];
            char answer[64];
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
                // is this the earliest correct answer so far?
                if (first_correct < 0 ||
                    at.tv_sec < best_time.tv_sec ||
                    (at.tv_sec == best_time.tv_sec &&
                     at.tv_usec < best_time.tv_usec))
                {
                    // undo previous winner's +2 if there was one
                    if (first_correct >= 0)
                        g_clients[first_correct]->score -= 2;
                    first_correct = c;
                    best_time = at;
                    cl->score += 2;
                }
                else
                    cl->score += 1;
            }
            else if (is_wrong)
            {
                cl->score -= 1;
            }
        }

        // broadcast WIN|name or WIN|
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

    // build RESULT|name|score|name|score... in descending score order
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

    // wake reader threads (set ans_ready so they unblock)
    for (int c = 0; c < n; c++)
    {
        Client *cl = g_clients[c];
        pthread_mutex_lock(&cl->ans_mu);
        cl->ans_ready = 1;
        pthread_cond_signal(&cl->ans_cv);
        pthread_mutex_unlock(&cl->ans_mu);
    }

    // signal quiz over to member handler threads
    pthread_mutex_lock(&g_mu);
    g_quiz_over = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

/* ================================================================== */
/*  client_handler thread                                             */
/* ================================================================== */

typedef struct
{
    int sock;
} AcceptArg;

static void *client_handler(void *arg)
{
    AcceptArg *aa = (AcceptArg *)arg;
    int sock = aa->sock;
    free(aa);

    char buf[BUFSIZE];

    // quick reject without lock first
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

    // determine role under lock
    pthread_mutex_lock(&g_mu);
    int is_leader = (g_state == SRV_IDLE);

    if (is_leader)
    {
        g_state = SRV_ASSEMBLING;
        g_group_size = 0;
        g_joined = 0;
        g_quiz_over = 0;
    }
    else if (g_state == SRV_ASSEMBLING)
    {
        // recheck space
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
        pthread_mutex_unlock(&g_mu);
        send_all(sock, "QS|FULL\r\n", 9);
        close(sock);
        return NULL;
    }
    pthread_mutex_unlock(&g_mu);

    // allocate client
    Client *cl = calloc(1, sizeof(Client));
    if (!cl)
    {
        close(sock);
        return NULL;
    }
    cl->sock = sock;
    cl->ans_ready = 1; // "already answered", reader blocks until first Q
    pthread_mutex_init(&cl->ans_mu, NULL);
    pthread_cond_init(&cl->ans_cv, NULL);
    strcpy(cl->name, "Unknown");

    /* ----------------------------------------------------------------
     * LEADER PATH
     * ---------------------------------------------------------------- */
    if (is_leader)
    {
        send_all(sock, "QS|ADMIN\r\n", 10);

        if (read_crlf_line(sock, buf, sizeof(buf), 30) <= 0)
        {
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
            pthread_mutex_lock(&g_mu);
            g_state = SRV_IDLE;
            pthread_mutex_unlock(&g_mu);
            pthread_mutex_destroy(&cl->ans_mu);
            pthread_cond_destroy(&cl->ans_cv);
            free(cl);
            close(sock);
            return NULL;
        }

        // parse GROUP|name|size
        char *body = buf + 6;
        char *pipe = strrchr(body, '|');
        int sz = 1;
        if (pipe)
        {
            *pipe = '\0';
            sz = atoi(pipe + 1);
            if (sz < 1 || sz > MAX_CLIENTS)
                sz = 1;
        }
        strncpy(cl->name, body, sizeof(cl->name) - 1);
        cl->name[sizeof(cl->name) - 1] = '\0';

        pthread_mutex_lock(&g_mu);
        g_group_size = sz;
        g_clients[0] = cl;
        g_joined = 1;
        pthread_mutex_unlock(&g_mu);

        fprintf(stderr, "server: leader '%s' wants group of %d\n",
                cl->name, sz);

        send_all(sock, "WAIT\r\n", 6);

        // wait for all members to join
        pthread_mutex_lock(&g_mu);
        while (g_joined < g_group_size)
            pthread_cond_wait(&g_cv, &g_mu);
        g_state = SRV_QUIZZING;
        pthread_mutex_unlock(&g_mu);

        fprintf(stderr, "server: group complete (%d), starting quiz\n",
                g_joined);

        // launch reader thread for leader's own socket
        pthread_t rtid;
        pthread_create(&rtid, NULL, reader_thread, cl);
        pthread_detach(rtid);

        // run quiz (blocks until RESULT sent)
        run_quiz();

        // give reader threads time to see quiz_over and exit cleanly
        usleep(300000);

        // close all sockets
        pthread_mutex_lock(&g_mu);
        int n = g_joined;
        pthread_mutex_unlock(&g_mu);
        for (int c = 0; c < n; c++)
            close(g_clients[c]->sock);

        usleep(100000); // let readers finish after socket close

        // free all client structs and reset state
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
     * ---------------------------------------------------------------- */

    pthread_mutex_lock(&g_mu);
    // final space check
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
    int group_full = (g_joined >= g_group_size);
    pthread_mutex_unlock(&g_mu);

    send_all(sock, "QS|JOIN\r\n", 9);

    if (read_crlf_line(sock, buf, sizeof(buf), 30) <= 0)
    {
        cl->aborted = 1;
        if (group_full)
        {
            pthread_mutex_lock(&g_mu);
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mu);
        }
        // cl lives in g_clients[]; leader's cleanup will free it
        return NULL;
    }
    if (strncmp(buf, "JOIN|", 5) == 0)
        strncpy(cl->name, buf + 5, sizeof(cl->name) - 1);
    cl->name[sizeof(cl->name) - 1] = '\0';

    fprintf(stderr, "server: member '%s' joined (%d/%d)\n",
            cl->name, idx + 1, g_group_size);

    send_all(sock, "WAIT\r\n", 6);

    // wake leader if group is now full
    if (group_full)
    {
        pthread_mutex_lock(&g_mu);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }

    // launch reader thread for this member
    pthread_t rtid;
    pthread_create(&rtid, NULL, reader_thread, cl);
    pthread_detach(rtid);

    // wait until quiz is done; leader handles all cleanup
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
    int rport = 0;

    switch (argc)
    {
    case 2:
        quizfile = argv[1];
        rport = 1;
        break;
    case 3:
        quizfile = argv[1];
        service = argv[2];
        break;
    default:
        fprintf(stderr, "usage: server2 quizfile [port]\n");
        exit(1);
    }

    if (load_questions(quizfile) <= 0)
    {
        fprintf(stderr, "server2: no questions loaded from '%s'\n", quizfile);
        exit(1);
    }
    fprintf(stderr, "server2: loaded %d question(s)\n", num_questions);

    int msock = passivesock(service, "tcp", QLEN, &rport);
    if (rport)
    {
        printf("server: port %d\n", rport);
        fflush(stdout);
    }

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