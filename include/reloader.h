/*
 * reloader.h
 * Reloader header file defining the reloader, watcher, and constants.
 */

#ifndef	_RELOADER_H
#define	_RELOADER_H

#include <sys/inotify.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>

#define RLD_VERSION "1.0.0-dev"

#define RLD_DEFAULT_DELAY 30

#define RLD_MAX_BUF_EVENTS 10
/* Double max buffer events for max thread count */
#define RLD_MAX_THREAD_COUNT 20
#define RLD_BUF_SZ (RLD_MAX_BUF_EVENTS * (sizeof(struct inotify_event) + NAME_MAX + 1))

#define RLD_CREATE	(IN_CREATE | IN_MOVED_TO)
#define RLD_REMOVE	(IN_DELETE_SELF | IN_DELETE)
#define RLD_WRITE	(IN_MODIFY)
#define RLD_RENAME	(IN_MOVE_SELF | IN_MOVED_FROM)
#define RLD_CHMOD	(IN_ATTRIB)
#define RLD_CHANGE	(RLD_CREATE | RLD_REMOVE | RLD_WRITE)

/*
 * watcher_t represents a inotify event watcher context, tracking the path,
 * command, and delay (in seconds).
 */
struct watcher_t {
	int		 wd;			/* watch descriptor */
	char		 path[PATH_MAX];	/* inotify path */
	char		 *cmd;			/* command to run on change */
	bool		 waiting;		/* is the command waiting? */
	uint32_t	 delay;			/* delay before the command */
	struct watcher_t *next;			/* linked list of watchers */

	pthread_t	 *td;			/* thread descriptor */
	pthread_mutex_t	 lock;			/* watcher context lock */
	pthread_mutex_t	 wait_lock;		/* timed wait lock */
	pthread_cond_t	 cond;			/* timed wait condition var */
};

/*
 * watcher_event represents a watched inotify event and coupling the inotify
 * event instance and its watcher. Used during processing in separate threads.
 */
struct watcher_event {
	struct inotify_event	*ev;
	struct watcher_t	*watch;
};

/*
 * reloader_t represents a reloader context, tracking the inotify event buffer,
 * the inotify event watchers, and inotify's file descriptor. A reloader watches
 * a list of given paths and performs a given command (in a new thread) whenever
 * a change happens in that path. The typical use case is as hot reloader, where
 * if a directory changes, a service must be reloaded.
 *
 * A delay is used as a heurstic method to group events on a single watcher and
 * are considered corelated. Meaning, if a bunch of events happen at around the
 * same time for the same path, these events are probably related. If this
 * happens, the command only runs once for that group within the delay window.
 */
struct reloader_t {
	struct event_base	*evbase;	/* libevent base */
	struct bufferevent	*evbuffer;	/* inotify event buffer */
	struct watcher_t	*watchers;	/* inotify watchers */
	int	in_fd;				/* inotify file descriptor */
};

/*
 * ----------------------
 * | Reloader functions |
 * ----------------------
 */

/*
 * reloader_new creates a new reloader context for use in other functions.
 */
struct reloader_t *reloader_new();

/*
 * reloader_close closes the given reloader, and frees any allocated memory.
 */
void reloader_close(struct reloader_t *_rldr);

/*
 * reloader_add_watch creates and adds a new watcher to the reloader for a given
 * path, command, and delay. Returns the watch descriptor on success, otherwise
 * -1 indicating an error.
 */
int reloader_add_watch(struct reloader_t *_rldr, const char *_path,
    const char *_cmd, uint32_t _delay);

/*
 * reloader_remove_watch removes a watched path from the reloader.
 */
void reloader_remove_watch(struct reloader_t *_rldr, int _wd);

/*
 * reloader_get_watcher returns the watcher for a given watch descriptor. If a
 * watcher isn't found, NULL is returned.
 */
struct watcher_t *reloader_get_watcher(struct reloader_t *_rldr, int _wd);

/*
 * reloader_start dispatches the inotify event loop. Returns 0 on succes, -1 on
 * error, and 1 when the event loop is no longer active.
 *
 * See event_base_dispatch in the libevent library for more information. For
 * this function is mostly a wrapper.
 */
int reloader_start(struct reloader_t *_rldr);

/*
 * reloader_start dispatches the inotify event loop. Returns 0 on succes, -1 on
 * error.
 *
 * See event_base_loopbreak in the libevent library for more information. For
 * this function is mostly a wrapper.
 */
int reloader_stop(struct reloader_t *_rldr);

/*
 * reloader_readcb acts as the default buffer event read callback function.
 * Meaning, if there are events ready to be consumed this function will be
 * called.
 */
void reloader_readcb(struct bufferevent *bev, void *ctx);

/*
 * ---------------------
 * | Watcher functions |
 * ---------------------
 */

/*
 * watcher_new creates and returns a new watcher for the given path.
 */
struct watcher_t *watcher_new(int _in_fd, const char *_path, const char *_cmd,
    uint32_t _delay);

/*
 * watcher_close closes the given watcher context, freeing any allocated memory.
 */
void watcher_close(int _in_fd, struct watcher_t *_watch);

/*
 * watcher_is_waiting returns true if the watcher is currently waiting to run
 * the command. This function is thread-safe.
 */
bool watcher_is_waiting(struct watcher_t *_watch);

/*
 * watcher_set_waiting sets the waiting state of the watcher context. This
 * function is thread-safe.
 */
void watcher_set_waiting(struct watcher_t *_watch, bool _wait);

/*
 * watcher_set_td sets the thread descriptor for the watcher context. Passing
 * NULL will reset this descriptor. This function is thread-safe.
 */
void watcher_set_td(struct watcher_t *_watch, pthread_t *_td);

/*
 * -------------
 * | Utilities |
 * -------------
 */

/*
 * new_event_buffer
 */
static struct bufferevent *new_event_buffer(struct event_base *evbase,
    evutil_socket_t fd, void *cbarg);

/*
 * process_in_event starts a new processing thread for the given inotify event
 * and its watcher. The watchers command will run during this process.
 */
static bool process_in_event(struct inotify_event *_ev,
    struct watcher_t *_watch);

/*
 * run_command runs the command for the passed watcher event. This is used as
 * the pthread routine that process_in_event creates.
 */
static void *run_command(void *_arg);

/*
 * cleanup_command_thread callback function for run_command threads. Cleans up
 * allocated resources and resets watcher states.
 */
static void  cleanup_command_thread(void *_arg);

#endif	/* !_RELOADER_H */
