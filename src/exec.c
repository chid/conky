/* -*- mode: c; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: t -*-
 * vim: ts=4 sw=4 noet ai cindent syntax=c
 *
 * Conky, a system monitor, based on torsmo
 *
 * Any original torsmo code is licensed under the BSD license
 *
 * All code written since the fork of torsmo is licensed under the GPL
 *
 * Please see COPYING for details
 *
 * Copyright (c) 2004, Hannu Saransaari and Lauri Hakkarainen
 * Copyright (c) 2005-2009 Brenden Matthews, Philip Kovacs, et. al.
 *	(see AUTHORS)
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "conky.h"
#include "core.h"
#include "logging.h"
#include "specials.h"
#include "text_object.h"
#include "timed_thread.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>

struct execi_data {
	double last_update;
	float interval;
	char *cmd;
	char *buffer;
	double data;
	timed_thread *p_timed_thread;
};

/* FIXME: this will probably not work, since the variable is being reused
 * between different text objects. So when a process really hangs, it's PID
 * will be overwritten at the next iteration. */
pid_t childpid = 0;

//our own implementation of popen, the difference : the value of 'childpid' will be filled with
//the pid of the running 'command'. This is useful if want to kill it when it hangs while reading
//or writing to it. We have to kill it because pclose will wait until the process dies by itself
static FILE* pid_popen(const char *command, const char *mode, pid_t *child) {
	int ends[2];
	int parentend, childend;

	//by running pipe after the strcmp's we make sure that we don't have to create a pipe
	//and close the ends if mode is something illegal
	if(strcmp(mode, "r") == 0) {
		if(pipe(ends) != 0) {
			return NULL;
		}
		parentend = ends[0];
		childend = ends[1];
	} else if(strcmp(mode, "w") == 0) {
		if(pipe(ends) != 0) {
			return NULL;
		}
		parentend = ends[1];
		childend = ends[0];
	} else {
		return NULL;
	}
	*child = fork();
	if(*child == -1) {
		close(parentend);
		close(childend);
		return NULL;
	} else if(*child > 0) {
		close(childend);
		waitpid(*child, NULL, 0);
	} else {
		//don't read from both stdin and pipe or write to both stdout and pipe
		if(childend == ends[0]) {
			close(0);
		} else {
			close(1);
		}
		dup(childend);	//by dupping childend, the returned fd will have close-on-exec turned off
		execl("/bin/sh", "sh", "-c", command, (char *) NULL);
		_exit(EXIT_FAILURE); //child should die here, (normally execl will take care of this but it can fail)
	}
	return fdopen(parentend, mode);
}

//remove backspaced chars, example: "dog^H^H^Hcat" becomes "cat"
//string has to end with \0 and it's length should fit in a int
#define BACKSPACE 8
static void remove_deleted_chars(char *string){
	int i = 0;
	while(string[i] != 0){
		if(string[i] == BACKSPACE){
			if(i != 0){
				strcpy( &(string[i-1]), &(string[i+1]) );
				i--;
			}else strcpy( &(string[i]), &(string[i+1]) ); //necessary for ^H's at the start of a string
		}else i++;
	}
}

static inline double get_barnum(char *buf)
{
	char *c = buf;
	double barnum;

	while (*c) {
		if (*c == '\001') {
			*c = ' ';
		}
		c++;
	}

	if (sscanf(buf, "%lf", &barnum) == 0) {
		NORM_ERR("reading exec value failed (perhaps it's not the "
				"correct format?)");
		return -1;
	}
	if (barnum > 100.0 || barnum < 0.0) {
		NORM_ERR("your exec value is not between 0 and 100, "
				"therefore it will be ignored");
		return -1;
	}
	return barnum;
}

static inline void read_exec(const char *data, char *buf, const int size)
{
	FILE *fp;

	memset(buf, 0, size);

	if (!data)
		return;

	alarm(update_interval);
	fp = pid_popen(data, "r", &childpid);
	if(fp) {
		int length;

		length = fread(buf, 1, size, fp);
		pclose(fp);
		buf[length] = '\0';
		if (length > 0 && buf[length - 1] == '\n') {
			buf[length - 1] = '\0';
		}
	} else {
		buf[0] = '\0';
	}
	alarm(0);
}

static void *threaded_exec(void *) __attribute__((noreturn));

static void *threaded_exec(void *arg)
{
	char *buff, *p2;
	struct text_object *obj = arg;
	struct execi_data *ed = obj->data.opaque;

	while (1) {
		buff = malloc(text_buffer_size);
		read_exec(ed->cmd, buff, text_buffer_size);
		p2 = buff;
		while (*p2) {
			if (*p2 == '\001') {
				*p2 = ' ';
			}
			p2++;
		}
		timed_thread_lock(ed->p_timed_thread);
		if (ed->buffer)
			free(ed->buffer);
		ed->buffer = buff;
		timed_thread_unlock(ed->p_timed_thread);
		if (timed_thread_test(ed->p_timed_thread, 0)) {
			timed_thread_exit(ed->p_timed_thread);
		}
	}
	/* never reached */
}

/* check the execi fields and return true if the given interval has passed */
static int time_to_update(struct execi_data *ed)
{
	if (!ed->interval)
		return 0;
	if (current_update_time - ed->last_update >= ed->interval)
		return 1;
	return 0;
}

void scan_exec_arg(struct text_object *obj, const char *arg)
{
	obj->data.s = strndup(arg ? arg : "", text_buffer_size);
}

void scan_pre_exec_arg(struct text_object *obj, const char *arg)
{
	char buf[2048];

	obj->type = OBJ_text;
	read_exec(arg, buf, sizeof(buf));
	obj->data.s = strndup(buf, text_buffer_size);
}

void scan_execi_arg(struct text_object *obj, const char *arg)
{
	struct execi_data *ed;
	int n;

	ed = malloc(sizeof(struct execi_data));
	memset(ed, 0, sizeof(struct execi_data));

	if (sscanf(arg, "%f %n", &ed->interval, &n) <= 0) {
		NORM_ERR("${execi* <interval> command}");
		free(ed);
		return;
	}
	ed->cmd = strndup(arg + n, text_buffer_size);
	obj->data.opaque = ed;
}

void print_exec(struct text_object *obj, char *p, int p_max_size)
{
	read_exec(obj->data.s, p, p_max_size);
	remove_deleted_chars(p);
}

void print_execp(struct text_object *obj, char *p, int p_max_size)
{
	struct information *tmp_info;
	struct text_object subroot;

	read_exec(obj->data.s, p, p_max_size);

	tmp_info = malloc(sizeof(struct information));
	memcpy(tmp_info, &info, sizeof(struct information));
	parse_conky_vars(&subroot, p, p, tmp_info);

	free_text_objects(&subroot, 1);
	free(tmp_info);
}

void print_execi(struct text_object *obj, char *p, int p_max_size)
{
	struct execi_data *ed = obj->data.opaque;

	if (!ed)
		return;

	if (time_to_update(ed)) {
		if (!ed->buffer)
			ed->buffer = malloc(text_buffer_size);
		read_exec(ed->cmd, ed->buffer, text_buffer_size);
		ed->last_update = current_update_time;
	}
	snprintf(p, p_max_size, "%s", ed->buffer);
}

void print_execpi(struct text_object *obj, char *p)
{
	struct execi_data *ed = obj->data.opaque;
	struct text_object subroot;
	struct information *tmp_info;

	if (!ed)
		return;

	tmp_info = malloc(sizeof(struct information));
	memcpy(tmp_info, &info, sizeof(struct information));

	if (!time_to_update(ed)) {
		parse_conky_vars(&subroot, ed->buffer, p, tmp_info);
	} else {
		char *output;
		int length;
		FILE *fp = pid_popen(ed->cmd, "r", &childpid);

		if (!ed->buffer)
			ed->buffer = malloc(text_buffer_size);

		length = fread(ed->buffer, 1, text_buffer_size, fp);
		pclose(fp);

		output = ed->buffer;
		output[length] = '\0';
		if (length > 0 && output[length - 1] == '\n') {
			output[length - 1] = '\0';
		}

		parse_conky_vars(&subroot, ed->buffer, p, tmp_info);
		ed->last_update = current_update_time;
	}
	free_text_objects(&subroot, 1);
	free(tmp_info);
}

void print_texeci(struct text_object *obj, char *p, int p_max_size)
{
	struct execi_data *ed = obj->data.opaque;

	if (!ed)
		return;

	if (!ed->p_timed_thread) {
		ed->p_timed_thread = timed_thread_create(&threaded_exec,
					(void *) obj, ed->interval * 1000000);
		if (!ed->p_timed_thread) {
			NORM_ERR("Error creating texeci timed thread");
		}
		/*
		 * note that we don't register this thread with the
		 * timed_thread list, because we destroy it manually
		 */
		if (timed_thread_run(ed->p_timed_thread)) {
			NORM_ERR("Error running texeci timed thread");
		}
	} else {
		timed_thread_lock(ed->p_timed_thread);
		snprintf(p, p_max_size, "%s", ed->buffer);
		timed_thread_unlock(ed->p_timed_thread);
	}
}

#ifdef X11
void print_execgauge(struct text_object *obj, char *p, int p_max_size)
{
	double barnum;

	read_exec(obj->data.s, p, p_max_size);
	barnum = get_barnum(p); /*using the same function*/

	if (barnum >= 0.0) {
		barnum /= 100;
		new_gauge(p, obj->a, obj->b, round_to_int(barnum * 255.0));
	}
}

void print_execgraph(struct text_object *obj, char *p, int p_max_size)
{
	char showaslog = FALSE;
	char tempgrad = FALSE;
	double barnum;
	struct execi_data *ed = obj->data.opaque;
	char *cmd;

	if (!ed)
		return;

	cmd = ed->cmd;

	if (strstr(cmd, " "TEMPGRAD) && strlen(cmd) > strlen(" "TEMPGRAD)) {
		tempgrad = TRUE;
		cmd += strlen(" "TEMPGRAD);
	}
	if (strstr(cmd, " "LOGGRAPH) && strlen(cmd) > strlen(" "LOGGRAPH)) {
		showaslog = TRUE;
		cmd += strlen(" "LOGGRAPH);
	}
	read_exec(cmd, p, p_max_size);
	barnum = get_barnum(p);

	if (barnum > 0) {
		new_graph(p, obj->a, obj->b, obj->c, obj->d, round_to_int(barnum),
				100, 1, showaslog, tempgrad);
	}
}

void print_execigraph(struct text_object *obj, char *p, int p_max_size)
{
	struct execi_data *ed = obj->data.opaque;

	if (!ed)
		return;

	if (time_to_update(ed)) {
		double barnum;
		char showaslog = FALSE;
		char tempgrad = FALSE;
		char *cmd = ed->cmd;

		if (strstr(cmd, " "TEMPGRAD) && strlen(cmd) > strlen(" "TEMPGRAD)) {
			tempgrad = TRUE;
			cmd += strlen(" "TEMPGRAD);
		}
		if (strstr(cmd, " "LOGGRAPH) && strlen(cmd) > strlen(" "LOGGRAPH)) {
			showaslog = TRUE;
			cmd += strlen(" "LOGGRAPH);
		}
		obj->char_a = showaslog;
		obj->char_b = tempgrad;
		read_exec(cmd, p, p_max_size);
		barnum = get_barnum(p);

		if (barnum >= 0.0) {
			obj->f = barnum;
		}
		ed->last_update = current_update_time;
	}
	new_graph(p, obj->a, obj->b, obj->c, obj->d, (int) (obj->f), 100, 1, obj->char_a, obj->char_b);
}

void print_execigauge(struct text_object *obj, char *p, int p_max_size)
{
	struct execi_data *ed = obj->data.opaque;

	if (!ed)
		return;

	if (time_to_update(ed)) {
		double barnum;

		read_exec(ed->cmd, p, p_max_size);
		barnum = get_barnum(p);

		if (barnum >= 0.0) {
			obj->f = 255 * barnum / 100.0;
		}
		ed->last_update = current_update_time;
	}
	new_gauge(p, obj->a, obj->b, round_to_int(obj->f));
}
#endif /* X11 */

void print_execbar(struct text_object *obj, char *p, int p_max_size)
{
	double barnum;
	read_exec(obj->data.s, p, p_max_size);
	barnum = get_barnum(p);

	if (barnum >= 0.0) {
#ifdef X11
		if(output_methods & TO_X) {
			barnum /= 100;
			new_bar(obj, p, round_to_int(barnum * 255.0));
		}else
#endif /* X11 */
			new_bar_in_shell(obj, p, p_max_size, barnum);
	}
}

void print_execibar(struct text_object *obj, char *p, int p_max_size)
{
	struct execi_data *ed = obj->data.opaque;
	double barnum;

	if (!ed)
		return;

	if (time_to_update(ed)) {
		read_exec(ed->cmd, p, p_max_size);
		barnum = get_barnum(p);

		if (barnum >= 0.0) {
			obj->f = barnum;
		}
		ed->last_update = current_update_time;
	}
#ifdef X11
	if(output_methods & TO_X) {
		new_bar(obj, p, round_to_int(obj->f * 2.55));
	} else
#endif /* X11 */
		new_bar_in_shell(obj, p, p_max_size, round_to_int(obj->f));
}

void free_exec(struct text_object *obj)
{
	if (obj->data.s) {
		free(obj->data.s);
		obj->data.s = NULL;
	}
}

void free_execi(struct text_object *obj)
{
	struct execi_data *ed = obj->data.opaque;

	if (!ed)
		return;

	if (ed->p_timed_thread)
		timed_thread_destroy(ed->p_timed_thread, &ed->p_timed_thread);
	if (ed->cmd)
		free(ed->cmd);
	if (ed->buffer)
		free(ed->buffer);
	free(obj->data.opaque);
	obj->data.opaque = NULL;
}