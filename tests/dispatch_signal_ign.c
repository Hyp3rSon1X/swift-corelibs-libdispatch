/*
 * Copyright (c) 2026 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

// Signal sources must fire when the signal was set to SIG_IGN before the
// source was registered, as recommended for signal sources, even if an
// earlier source for the same signal already existed in the process. kqueue's
// EVFILT_SIGNAL behaves that way; on epoll the signal handler that forwards
// the signal to the signalfd has to be reinstalled.

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <dispatch/dispatch.h>

#include <bsdtests.h>
#include "dispatch_test.h"

// Registration is asynchronous, so the signal is sent repeatedly until the
// source fires (sends before that are discarded since the signal is ignored).
#define SIGNAL_INTERVAL (10 * NSEC_PER_MSEC)
#define SIGNAL_ATTEMPTS 500

static dispatch_queue_t q;

static dispatch_source_t
signal_source(int signo, dispatch_semaphore_t fired)
{
	dispatch_source_t ds = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
			(uintptr_t)signo, 0, q);
	test_ptr_notnull("dispatch_source_create", ds);
	dispatch_source_set_event_handler(ds, ^{
		dispatch_semaphore_signal(fired);
	});
	dispatch_resume(ds);
	return ds;
}

static long
send_until_fired(int signo, dispatch_semaphore_t fired)
{
	for (int i = 0; i < SIGNAL_ATTEMPTS; i++) {
		kill(getpid(), signo);
		if (!dispatch_semaphore_wait(fired,
				dispatch_time(DISPATCH_TIME_NOW, SIGNAL_INTERVAL))) {
			return 0;
		}
	}
	return -1;
}

// Swallow events from signals sent before the source fired the first time.
static void
drain(dispatch_semaphore_t fired)
{
	usleep(100000);
	while (!dispatch_semaphore_wait(fired, DISPATCH_TIME_NOW)) {
	}
}

static void
cancel_and_wait(dispatch_source_t ds)
{
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	dispatch_source_set_cancel_handler(ds, ^{
		dispatch_semaphore_signal(done);
	});
	dispatch_source_cancel(ds);
	// the cancel handler runs once the source is unregistered
	dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
	dispatch_release(done);
	dispatch_release(ds);
}

// A new source after the previous one was cancelled and the signal was
// ignored again (e.g. by another component following the same pattern).
static void
test_reregister(int signo)
{
	dispatch_semaphore_t fired = dispatch_semaphore_create(0);
	dispatch_source_t ds;

	signal(signo, SIG_IGN);
	ds = signal_source(signo, fired);
	test_long("reregister: first source fired",
			send_until_fired(signo, fired), 0);
	cancel_and_wait(ds);
	drain(fired);

	signal(signo, SIG_IGN);
	ds = signal_source(signo, fired);
	test_long("reregister: source after SIG_IGN again fired",
			send_until_fired(signo, fired), 0);
	cancel_and_wait(ds);
	dispatch_release(fired);
}

// A second source registered after the signal was ignored again while the
// first source still exists.
static void
test_second_source(int signo)
{
	dispatch_semaphore_t fired1 = dispatch_semaphore_create(0);
	dispatch_semaphore_t fired2 = dispatch_semaphore_create(0);
	dispatch_source_t ds1, ds2;

	signal(signo, SIG_IGN);
	ds1 = signal_source(signo, fired1);
	test_long("second source: first source fired",
			send_until_fired(signo, fired1), 0);
	drain(fired1);

	signal(signo, SIG_IGN);
	ds2 = signal_source(signo, fired2);
	test_long("second source: second source fired",
			send_until_fired(signo, fired2), 0);
	test_long("second source: first source fired again",
			dispatch_semaphore_wait(fired1,
					dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);

	cancel_and_wait(ds2);
	cancel_and_wait(ds1);
	dispatch_release(fired2);
	dispatch_release(fired1);
}

int
main(void)
{
	dispatch_test_start("Dispatch signal sources for ignored signals");

	// The kernel discards an ignored signal unless the thread it is sent to
	// blocks it, so keep the signals deliverable to this thread, like on the
	// main thread of an application.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGUSR2);
	pthread_sigmask(SIG_UNBLOCK, &mask, NULL);

	q = dispatch_queue_create("signal_ign", NULL);
	test_reregister(SIGUSR1);
	test_second_source(SIGUSR2);
	dispatch_release(q);

	test_stop();
}
