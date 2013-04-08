/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot.
 *
 * Copyright (C) 2013 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#include <sys/ptrace.h> /* PTRACE_*,  */
#include <sys/wait.h>   /* __WALL,  */
#include <errno.h>      /* E*, */
#include <assert.h>     /* assert(3), */
#include <stdbool.h>    /* bool, true, false, */

#include "ptrace/wait.h"
#include "ptrace/ptrace.h"
#include "syscall/sysnum.h"
#include "tracee/tracee.h"
#include "tracee/event.h"
#include "tracee/reg.h"
#include "tracee/mem.h"
#include "notice.h"

#include "attribute.h"

static const char *stringify_event(int event) UNUSED;
static const char *stringify_event(int event)
{
	if (WIFEXITED(event))
		return "exited";
	else if (WIFSIGNALED(event))
		return "signaled";
	else if (WIFCONTINUED(event))
		return "continued";
	else if (WIFSTOPPED(event)) {
		switch ((event & 0xfff00) >> 8) {
		case SIGTRAP:
			return "stopped: SIGTRAP";
		case SIGTRAP | 0x80:
			return "stopped: SIGTRAP: 0x80";
		case SIGTRAP | PTRACE_EVENT_VFORK << 8:
			return "stopped: SIGTRAP: PTRACE_EVENT_VFORK";
		case SIGTRAP | PTRACE_EVENT_FORK  << 8:
			return "stopped: SIGTRAP: PTRACE_EVENT_FORK";
		case SIGTRAP | PTRACE_EVENT_VFORK_DONE  << 8:
			return "stopped: SIGTRAP: PTRACE_EVENT_FORK_DONE";
		case SIGTRAP | PTRACE_EVENT_CLONE << 8:
			return "stopped: SIGTRAP: PTRACE_EVENT_CLONE";
		case SIGTRAP | PTRACE_EVENT_EXEC  << 8:
			return "stopped: SIGTRAP: PTRACE_EVENT_EXEC";
		case SIGTRAP | PTRACE_EVENT_EXIT  << 8:
			return "stopped: SIGTRAP: PTRACE_EVENT_EXIT";
		case SIGSTOP:
			return "stopped: SIGSTOP";
		default:
			return "stopped: unknown";
		}
	}
	return "unknown";
}

/**
 * Translate the wait syscall made by @ptracer into a "void" syscall
 * if the expected pid is one of its ptracees, in order to emulate the
 * ptrace mechanism within PRoot.  This function returns -errno if an
 * error occured (unsupported request), otherwise 0.
 */
int translate_wait_enter(Tracee *ptracer)
{
	static bool warned = false;
	Tracee *ptracee;
	word_t options;
	pid_t pid;

	PTRACER.waits_in = WAITS_IN_KERNEL;

	/* Don't emulate the ptrace mechanism if it's not a ptracer.  */
	if (PTRACER.nb_ptracees == 0)
		return 0;

	/* Don't emulate the ptrace mechanism if the requested pid is
	 * not a ptracee.  */
	pid = (pid_t) peek_reg(ptracer, ORIGINAL, SYSARG_1);
	if (pid != -1) {
		ptracee = get_tracee(ptracer, pid, false);
		if (ptracee == NULL || PTRACEE.ptracer != ptracer)
			return 0;
	}

	/* Only the __WALL option is supported so far.  */
	options = peek_reg(ptracer, ORIGINAL, SYSARG_3);
	if (options != __WALL && !warned) {
		notice(ptracer, INTERNAL, WARNING, "only __WALL option is supported yet");
		warned = true;
	}

	/* This syscall is canceled at the enter stage in order to be
	 * handled at the exit stage.  */
	set_sysnum(ptracer, PR_void);
	PTRACER.waits_in = WAITS_IN_PROOT;

	return 0;
}

/**
 * Emulate the wait* syscall made by @ptracer if it was in the context
 * of the ptrace mechanism. This function returns -errno if an error
 * occured, otherwise the pid of the expected tracee.
 */
int translate_wait_exit(Tracee *ptracer)
{
	Tracee *ptracee;
	word_t address;
	pid_t pid;

	assert(PTRACER.waits_in == WAITS_IN_PROOT);
	PTRACER.waits_in = DOESNT_WAIT;

	pid = (pid_t) peek_reg(ptracer, ORIGINAL, SYSARG_1);

	/* Is there such a waiting ptracee with an event not yet
	 * passed to its ptracer?  */
	ptracee = get_waiting_ptracee(ptracer, pid, true);
	if (ptracee == NULL) {
		word_t options;

		/* Is there still living ptracees?  */
		if (PTRACER.nb_ptracees == 0)
			return -ECHILD;

		/* Non blocking wait(2) ?  */
		options = peek_reg(ptracer, ORIGINAL, SYSARG_3);
		if ((options & WNOHANG) != 0)
			return 0;

		/* Otherwise put this ptracer in the "waiting for
		 * ptracee" state, it will be woken up in
		 * handle_ptracee_event() later.  */
		PTRACER.wait_pid = pid;
		return 0;
	}

	/* Update the child status of ptracer's wait(2), if any.  */
	address = peek_reg(ptracer, ORIGINAL, SYSARG_2);
	if (address != 0) {
		poke_mem(ptracer, address, PTRACEE.event4.ptracer.value);
		if (errno != 0)
			return -errno;

		PTRACEE.event4.ptracer.cleared = true;
	}

	return ptracee->pid;
}

/**
 * For the given @ptracee, pass its current @event to its ptracer if
 * this latter is waiting for it, otherwise put the @ptracee in the
 * "waiting for ptracer" state.  This function returns whether
 * @ptracee shall be kept in the stop state or not.
 */
bool handle_ptracee_event(Tracee *ptracee, int event)
{
	Tracee *ptracer = PTRACEE.ptracer;
	bool keep_stopped;

	assert(ptracer != NULL);
	assert(!PTRACER.blocked_by_vfork);

	/* Remember what the event initially was, this will be
	 * required by PRoot to handle this event later.  */
	PTRACEE.event4.proot.value   = event;
	PTRACEE.event4.proot.cleared = false;

	/* By default, this ptracee should be kept stopped until its
	 * ptracer restarts it.  */
	keep_stopped = true;

	/* Not all events are expected for this ptracee.  */
	if (WIFSTOPPED(event)) {
		switch ((event & 0xfff00) >> 8) {
		case SIGTRAP | 0x80:
			if (PTRACEE.ignore_syscall)
				return false;

			if ((PTRACEE.options & PTRACE_O_TRACESYSGOOD) == 0)
				event &= ~(0x80 << 8);
			break;

#define PTRACE_EVENT_VFORKDONE PTRACE_EVENT_VFORK_DONE
#define CASE_FILTER_EVENT(name) case SIGTRAP | PTRACE_EVENT_ ##name << 8:	\
			if ((PTRACEE.options & PTRACE_O_TRACE ##name) == 0)	\
				return false;					\
			break;

			CASE_FILTER_EVENT(FORK);
			CASE_FILTER_EVENT(VFORK);
			CASE_FILTER_EVENT(VFORKDONE);
			CASE_FILTER_EVENT(CLONE);
			CASE_FILTER_EVENT(EXEC);
			CASE_FILTER_EVENT(EXIT);

		default:
			break;
		}
	}
	/* In these cases, the ptracee isn't really alive anymore.  To
	 * ensure it will not be in limbo, PRoot restarts it whether
	 * its ptracer is waiting for it or not.  */
	else if (WIFEXITED(event) || WIFSIGNALED(event))
		keep_stopped = false;

	/* Remember what the new event is, this will be required by
	   the ptracer in translate_ptrace_exit() in order to restart
	   this ptracee.  */
	if (keep_stopped) {
		PTRACEE.event4.ptracer.value   = event;
		PTRACEE.event4.ptracer.cleared = false;
	}

	/* Note: wait_pid is set in translate_wait_exit() if no
	 * ptracee event was pending when the ptracer started to
	 * wait.  */
	if (PTRACER.wait_pid == -1 || PTRACER.wait_pid == ptracee->pid) {
		word_t address;
		bool restarted;

		/* Update pid & wait status of the ptracer's
		 * wait(2).  */
		poke_reg(ptracer, SYSARG_RESULT, ptracee->pid);
		address = peek_reg(ptracer, ORIGINAL, SYSARG_2);
		if (address != 0) {
			poke_mem(ptracer, address, PTRACEE.event4.ptracer.value);
			if (errno != 0)
				poke_reg(ptracer, SYSARG_RESULT, (word_t) -errno);

			PTRACEE.event4.ptracer.cleared = true;
		}

		/* Write ptracer's register cache back.  */
		(void) push_regs(ptracer);

		/* Restart the ptracer.  */
		PTRACER.wait_pid = 0;
		restarted = restart_tracee(ptracer, 0);
		if (!restarted)
			keep_stopped = false;

		return keep_stopped;
	}

	return keep_stopped;
}