/*
 * reloader.c
 * The reloader, watcher, and utility function implementations.
 */

#include <sys/inotify.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "reloader.h"

struct reloader_t *
reloader_new()
{
	int s;
	struct reloader_t *rldr;

	rldr = (struct reloader_t *)malloc(sizeof(struct reloader_t));
	if (rldr == NULL)
		return (NULL);

	rldr->in_fd = inotify_init();
	if (rldr->in_fd == -1) {
		reloader_close(rldr);
		return (NULL);
	}

	rldr->evbase = event_base_new();
	if (rldr->evbase == NULL) {
		reloader_close(rldr);
		return (NULL);
	}

	rldr->evbuffer = new_event_buffer(rldr->evbase, rldr->in_fd, rldr);
	if (rldr->evbuffer == NULL) {
		reloader_close(rldr);
		return (NULL);
	}

	rldr->watchers = NULL;

	return (rldr);
}

void
reloader_close(struct reloader_t *rldr)
{
	struct watcher_t *current, *head;

	if (rldr->watchers != NULL) {
		head = rldr->watchers;
		for (; (current = head) != NULL;) {
			head = head->next;
			watcher_close(rldr->in_fd, current);
		}
	}

	if (rldr->evbuffer != NULL)
		bufferevent_free(rldr->evbuffer);

	if (rldr->evbase != NULL)
		event_base_free(rldr->evbase);

	close(rldr->in_fd);

	if (rldr != NULL)
		free(rldr);

	return;
}

void
reloader_readcb(struct bufferevent *bev, void *ctx)
{
	struct inotify_event	*ev;
	struct reloader_t	*rldr;
	struct watcher_t	*watch;
	char	*cmd;
	uint8_t	 buf[RLD_BUF_SZ];
	size_t	 nread, offset;

	rldr = (struct reloader_t *)ctx;
	nread = bufferevent_read(bev, buf, RLD_BUF_SZ);
	for (offset = 0; offset < nread;) {
		/*
		 * If a change that we care about happens, check if the watcher
		 * is currently waiting to run. If it isn't, start a new process
		 * thread.
		 */
		ev = (struct inotify_event *)(buf + offset);
		if (ev->mask & RLD_CHANGE) {
			watch = reloader_get_watcher(rldr, ev->wd);
			if (watch != NULL && !watcher_is_waiting(watch))
				process_in_event(ev, watch);
		}
		offset += sizeof(struct inotify_event) + ev->len;
	}

	return;
}

int
reloader_add_watch(struct reloader_t *rldr, const char *path, const char *cmd,
    uint32_t delay)
{
	struct watcher_t *watcher, *current, *prev;

	watcher = watcher_new(rldr->in_fd, path, cmd, delay);
	if (watcher == NULL)
		return (-1);

	if (rldr->watchers == NULL) {
		rldr->watchers = watcher;
	} else {
		prev = NULL;
		current = rldr->watchers;
		for (; current != NULL; current = current->next)
			prev = current;
		prev->next = watcher;
	}

	return (watcher->wd);
}

void
reloader_remove_watch(struct reloader_t *rldr, int wd)
{
	struct watcher_t *current, *prev;

	if (rldr->watchers != NULL) {
		prev = NULL;
		current = rldr->watchers;
		for (; current != NULL; current = current->next) {
			if (current->wd == wd)
				break;
			prev = current;
		}
		if (current == NULL) {
			return;
		} else if (prev == NULL) {
			rldr->watchers = current->next;
		} else {
			prev->next = current->next;
		}
		watcher_close(rldr->in_fd, current);
	}

	return;
}

struct watcher_t *
reloader_get_watcher(struct reloader_t *rldr, int wd)
{
	struct watcher_t *current;

	current = rldr->watchers;
	for (; current != NULL; current = current->next) {
		if (current->wd == wd)
			break;
	}

	return (current);
}

int
reloader_start(struct reloader_t *rldr)
{

	// 0 is success, -1 is error, 1 no longer active
	return event_base_dispatch(rldr->evbase);
}

int
reloader_stop(struct reloader_t *rldr)
{

	// 0 is success, -1 is failure
	return event_base_loopbreak(rldr->evbase);
}

struct watcher_t *
watcher_new(int in_fd, const char *path, const char *cmd, uint32_t delay)
{
	char		 *cmd_copy;
	struct watcher_t *watch;

	watch = (struct watcher_t *)malloc(sizeof(struct watcher_t));
	if (watch == NULL)
		return (NULL);

	watch->wd = inotify_add_watch(in_fd, path, IN_ALL_EVENTS);
	strncpy(watch->path, path, PATH_MAX);
	cmd_copy = strndup(cmd, PATH_MAX);
	watch->cmd = cmd_copy;
	watch->waiting = false;
	watch->delay = (delay == 0 ? RLD_DEFAULT_DELAY : delay);
	watch->next = NULL;
	watch->td = NULL;
	pthread_mutex_init(&(watch->lock), NULL);
	pthread_mutex_init(&(watch->wait_lock), NULL);
	pthread_cond_init(&(watch->cond), NULL);

	return (watch);
}

void
watcher_close(int in_fd, struct watcher_t *watch)
{
	int		s;
	pthread_t	td;

	if (watch->td != NULL) {
		td = *(watch->td);
		s = pthread_cancel(td);
		if (s == 0)
			pthread_join(td, NULL);
	}
	inotify_rm_watch(in_fd, watch->wd);
	if (watch->cmd != NULL)
		free(watch->cmd);
	if (watch != NULL)
		free(watch);

	return;
}

bool
watcher_is_waiting(struct watcher_t *watch)
{
	bool waiting;

	pthread_mutex_lock(&(watch->lock));
	waiting = watch->waiting;
	pthread_mutex_unlock(&(watch->lock));

	return (waiting);
}

void
watcher_set_waiting(struct watcher_t *watch, bool wait)
{

	pthread_mutex_lock(&(watch->lock));
	watch->waiting = wait;
	pthread_mutex_unlock(&(watch->lock));

	return;
}

void
watcher_set_td(struct watcher_t *watch, pthread_t *td)
{

	pthread_mutex_lock(&(watch->lock));
	watch->td = td;
	pthread_mutex_unlock(&(watch->lock));

	return;
}

static struct bufferevent *
new_event_buffer(struct event_base *evbase, evutil_socket_t fd, void *cbarg)
{
	int s;
	struct bufferevent *evbuffer;

	evbuffer = bufferevent_socket_new(evbase, fd, 0);
	if (evbuffer == NULL)
		return (NULL);
	bufferevent_setcb(evbuffer, reloader_readcb, NULL, NULL, cbarg);
	s = bufferevent_enable(evbuffer, EV_READ);
	if (s == -1) {
		bufferevent_free(evbuffer);
		return (NULL);
	}

	return (evbuffer);
}

static bool
process_in_event(struct inotify_event *ev, struct watcher_t *watch)
{
	struct watcher_event	*wev;
	pthread_t		 thread;

	wev = (struct watcher_event *)malloc(sizeof(struct watcher_event));
	if (wev == NULL)
		return (false);

	wev->ev = ev;
	wev->watch = watch;
	pthread_create(&thread, NULL, run_command, wev);
	watcher_set_waiting(watch, true);
	watcher_set_td(watch, &thread);

	return (true);
}

static void *
run_command(void *arg)
{
	struct watcher_event	*wev;
	struct timespec		 delay;

	wev = (struct watcher_event *)arg;
	delay.tv_sec = time(NULL) + wev->watch->delay;

	pthread_cleanup_push(cleanup_command_thread, (void *)wev);

	/*
	 * Be aware that the conditional wait will release the mutex lock and
	 * block on the condition variable. Meaning, you can't test if the mutex
	 * is locked to determine if we are waiting to run the command.
	 */
	pthread_mutex_lock(&(wev->watch->wait_lock));
	pthread_cond_timedwait(&(wev->watch->cond), &(wev->watch->wait_lock),
	    &delay);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	system(wev->watch->cmd);
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_mutex_unlock(&(wev->watch->wait_lock));

	pthread_cleanup_pop(1);

	pthread_exit(NULL);
}

static void
cleanup_command_thread(void *arg)
{
	struct watcher_event *wev;

	wev = (struct watcher_event *)arg;
	if (wev != NULL) {
		watcher_set_waiting(wev->watch, false);
		watcher_set_td(wev->watch, NULL);
		free(wev);
	}

	return;
}
