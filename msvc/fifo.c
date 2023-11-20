/***************************************************************************
 *   Copyright (C) 2007, Gilles Casse <gcasse@oralux.org>                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#include "speech.h"

#ifdef USE_ASYNC
// This source file is only used for asynchronious modes

#include <windows.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <errno.h>
#include <time.h>
#include "fifo.h"
#include "wave.h"
#include "debug.h"


static int my_command_is_running = 0;
static int fifo_start_req_val = 0;
static int fifo_stop_req_val = 0;

static CONDITION_VARIABLE fifo_start_req;
static CONDITION_VARIABLE fifo_stop_req;
static CONDITION_VARIABLE fifo_stop_ack;
static CRITICAL_SECTION fifo_lock;

static DWORD say_thread(LPVOID);

static HANDLE fifo_thread = NULL;

static espeak_ERROR push(t_espeak_command *the_command);

static t_espeak_command *pop();

static void init(int process_parameters);

static int node_counter = 0;
enum {
    MAX_NODE_COUNTER = 400,
    INACTIVITY_TIMEOUT = 50, // in ms, check that the stream is inactive
    MAX_INACTIVITY_CHECK = 2
};

void fifo_init() {
    ENTER("fifo_init");

    // security
    InitializeConditionVariable(&fifo_start_req);
    InitializeConditionVariable(&fifo_stop_req);
    InitializeConditionVariable(&fifo_stop_ack);
    InitializeCriticalSection(&fifo_lock);

    init(0);

    fifo_thread = CreateThread(
            NULL, // default security attributes
            0,    // default stack size
            (LPTHREAD_START_ROUTINE) say_thread,
            NULL, // no thread function arguments
            0,    // default creation flags
            NULL);// receive thread identifier
    assert(fifo_thread != NULL);

    // when the thread starts,
    // a stop ack request will be sent
    EnterCriticalSection(&fifo_lock);
    while (fifo_stop_req_val == 0) {
        SleepConditionVariableCS(&fifo_stop_ack, &fifo_lock, INFINITE);
    }
    fifo_stop_req_val++;
    LeaveCriticalSection(&fifo_lock);
}

espeak_ERROR fifo_add_command(t_espeak_command *the_command) {
    espeak_ERROR a_error = EE_OK;

    ENTER("fifo_add_command");

    EnterCriticalSection(&fifo_lock);
    a_error = push(the_command);
    if (!my_command_is_running && (a_error == EE_OK)) {
        // quit when command is actually started
        // (for possible forthcoming 'end of command' checks)
        fifo_start_req_val++;
        LeaveCriticalSection(&fifo_lock);
        WakeConditionVariable(&fifo_start_req);
        while (fifo_start_req_val > 0) {
            espeakSleep(50); // TBD: event?
        }
    } else {
        LeaveCriticalSection(&fifo_lock);
    }
    return a_error;
}

espeak_ERROR fifo_add_commands(
        t_espeak_command *command1,
        t_espeak_command *command2) {
    espeak_ERROR a_error = EE_OK;
    ENTER("fifo_add_command");
    EnterCriticalSection(&fifo_lock);
    if (node_counter + 1 >= MAX_NODE_COUNTER) {
        a_error = EE_BUFFER_FULL;
    } else {
        a_error = push(command1);
        if (a_error == EE_OK) {
            a_error = push(command2);
        }
    }
    if (!my_command_is_running && (a_error == EE_OK)) {
        // quit when one command is actually started
        // (for possible forthcoming 'end of command' checks)
        fifo_start_req_val++;
        LeaveCriticalSection(&fifo_lock);
        WakeConditionVariable(&fifo_start_req);
        while (fifo_start_req_val > 0) {
            espeakSleep(50); // TBD: event?
        }
    } else {
        LeaveCriticalSection(&fifo_lock);
    }
    return a_error;
}

espeak_ERROR fifo_stop() {
    int running_flag = 0;
    ENTER("fifo_stop");
    EnterCriticalSection(&fifo_lock);
    if (my_command_is_running) {
        running_flag = 1;
        fifo_stop_req_val++;
    }
    LeaveCriticalSection(&fifo_lock);
    if (running_flag) {
        EnterCriticalSection(&fifo_lock);
        while (fifo_stop_req_val > 0) {
            SleepConditionVariableCS(&fifo_stop_ack, &fifo_lock, INFINITE);
        }
        LeaveCriticalSection(&fifo_lock);
    }
    return EE_OK;
}

int fifo_is_busy() {
    return my_command_is_running;
}

// Wait for the start request (my_sem_start_is_required).
// Besides this, if the audio stream is still busy,
// check from time to time its end.
// The end of the stream is confirmed by several checks
// for filtering underflow.
//
static int sleep_until_start_request_or_inactivity() {
    int idx = 0;
    int start_request = 0;
    SHOW_TIME("fifo > sleep_until_start_request_or_inactivity > ENTER");
    while (start_request <= 0) {
        if (wave_is_busy(NULL)) {
            idx = 0;
        } else {
            idx++;
        }
        if (idx > MAX_INACTIVITY_CHECK) {
            break;
        }
        EnterCriticalSection(&fifo_lock);
        while (fifo_start_req_val <= 0) {
            SleepConditionVariableCS(
                    &fifo_start_req, &fifo_lock,
                    INACTIVITY_TIMEOUT);
        }
        start_request = fifo_start_req_val;
        LeaveCriticalSection(&fifo_lock);
    }
    return start_request;
}

// Warning: a wave_close can be already required by
// an external command (espeak_Cancel + fifo_stop), if so:
// my_stop_is_required = 1;
static void close_stream() {
    int stop_flag = 0;
    SHOW_TIME("fifo > close_stream > ENTER\n");
    EnterCriticalSection(&fifo_lock);
    stop_flag = fifo_stop_req_val;
    if (fifo_stop_req_val <= 0) {
        my_command_is_running = 1;
    }
    LeaveCriticalSection(&fifo_lock);

    if (stop_flag <= 0) {
        wave_close(NULL);
        EnterCriticalSection(&fifo_lock);
        my_command_is_running = 0;
        stop_flag = fifo_stop_req_val;
        LeaveCriticalSection(&fifo_lock);
        if (stop_flag > 0) {
            WakeConditionVariable(&fifo_stop_ack);
        }
    }
}


static DWORD say_thread(LPVOID arg) {
    int look_for_inactivity = 0;
    (void) arg;
    ENTER("say_thread");

    // announce that thread is started
    EnterCriticalSection(&fifo_lock);
    fifo_stop_req_val--;
    WakeConditionVariable(&fifo_stop_ack);
    LeaveCriticalSection(&fifo_lock);

    while (1) {
        int stop_request = 0;
        int start_request = 0;
        if (look_for_inactivity) {
            start_request = sleep_until_start_request_or_inactivity();
            if (start_request <= 0) {
                close_stream();
            }
        }
        look_for_inactivity = 1;

        EnterCriticalSection(&fifo_lock);
        while (fifo_start_req_val <= 0) {
            SleepConditionVariableCS(&fifo_start_req, &fifo_lock, INFINITE);
        }
        fifo_start_req_val--;
        LeaveCriticalSection(&fifo_lock);


        my_command_is_running = 1;
        while (my_command_is_running) {
            t_espeak_command *a_command = NULL;
            EnterCriticalSection(&fifo_lock);
            a_command = (t_espeak_command *) pop();
            if (a_command == NULL) {
                LeaveCriticalSection(&fifo_lock);
                my_command_is_running = 0;
            } else {
                // purge start semaphore
                fifo_start_req_val = 0;
                if (fifo_stop_req_val > 0) {
                    my_command_is_running = 0;
                }
                stop_request = fifo_stop_req_val;
                LeaveCriticalSection(&fifo_lock);
                if (my_command_is_running) {
                    process_espeak_command(a_command);
                }
                delete_espeak_command(a_command);
            }
        }

        // stop_request
        if (stop_request > 0) {
            // no mutex required since the stop command is synchronous
            // and waiting for my_sem_stop_is_acknowledged
            init(1);

            EnterCriticalSection(&fifo_lock);
            // purge start semaphore
            fifo_start_req_val = 0;
            // acknowledge the stop request
            fifo_stop_req_val--;
            stop_request = fifo_stop_req_val;
            LeaveCriticalSection(&fifo_lock);
            WakeConditionVariable(&fifo_stop_req);
        }
    }
}

int fifo_is_command_enabled(void) {
    return fifo_stop_req_val <= 0;
}

typedef struct t_node {
    t_espeak_command *data;
    struct t_node *next;
} node;

static node *head = NULL;
static node *tail = NULL;

// return 1 if ok, 0 otherwise
static espeak_ERROR push(t_espeak_command *the_command) {
    node *n = NULL;

    ENTER("fifo > push");

    assert((!head && !tail) || (head && tail));

    if (the_command == NULL) {
        SHOW("push > command=0x%x\n", NULL);
        return EE_INTERNAL_ERROR;
    }

    if (node_counter >= MAX_NODE_COUNTER) {
        SHOW("push > %s\n", "EE_BUFFER_FULL");
        return EE_BUFFER_FULL;
    }

    n = (node *) malloc(sizeof(node));
    if (n == NULL) {
        return EE_INTERNAL_ERROR;
    }

    if (head == NULL) {
        head = n;
        tail = n;
    } else {
        tail->next = n;
        tail = n;
    }

    tail->next = NULL;
    tail->data = the_command;

    node_counter++;
    SHOW("push > counter=%d\n", node_counter);

    the_command->state = CS_PENDING;
    display_espeak_command(the_command);

    return EE_OK;
}

static t_espeak_command *pop() {
    t_espeak_command *the_command = NULL;

    ENTER("fifo > pop");

    assert((!head && !tail) || (head && tail));

    if (head != NULL) {
        node *n = head;
        the_command = n->data;
        head = n->next;
        free(n);
        node_counter--;
        SHOW("pop > command=0x%x (counter=%d)\n",
             the_command, node_counter);
    }

    if (head == NULL) {
        tail = NULL;
    }

    display_espeak_command(the_command);

    return the_command;
}

static void init(int process_parameters) {
    // Changed by Tyler Spivey 30.Nov.2011
    t_espeak_command *c = NULL;
    ENTER("fifo > init");
    c = pop();
    while (c != NULL) {
        if (process_parameters &&
            (c->type == ET_PARAMETER ||
             c->type == ET_VOICE_NAME ||
             c->type == ET_VOICE_SPEC)) {
            process_espeak_command(c);
        }
        delete_espeak_command(c);
        c = pop();
    }
    node_counter = 0;
}

void fifo_terminate() {
    ENTER("fifo_terminate");
    WakeAllConditionVariable(&fifo_start_req);
    WakeAllConditionVariable(&fifo_stop_req);
    TerminateThread(fifo_thread, 0);
    CloseHandle(fifo_thread);
    init(0); // purge fifo
}

#endif

