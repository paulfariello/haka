
#include <assert.h>
#include <stdlib.h>
#include <lua.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <errno.h>

#include <haka/log.h>
#include <haka/packet_module.h>
#include <haka/error.h>
#include <haka/thread.h>

#include "thread.h"
#include "app.h"


struct thread_state {
	int                         thread_id;
	struct packet_module_state *capture;
	lua_state                  *lua;
	int                         lua_function;
	pthread_t                   thread;
};

extern void lua_pushppacket(lua_State *L, struct packet *pkt);

static filter_result filter_wrapper(struct thread_state *state, struct packet *pkt)
{
	lua_rawgeti(state->lua, LUA_REGISTRYINDEX, state->lua_function);
	lua_pushppacket(state->lua, pkt);
	if (lua_pcall(state->lua, 1, 1, 0)) {
		print_error(state->lua, L"filter function");
		return FILTER_DROP;
	}

	if (!lua_isnumber(state->lua, -1)) {
		lua_pop(state->lua, 1);
		return FILTER_DROP;
	}

	const int ret = lua_tonumber(state->lua, -1);
	lua_remove(state->lua, -1);

	return (filter_result)ret;
}

struct thread_state *init_thread_state(struct packet_module *packet_module, int thread_id)
{
	struct thread_state *state;

	assert(packet_module);

	state = malloc(sizeof(struct thread_state));
	if (!state) {
		return NULL;
	}

	state->thread_id = thread_id;

	state->lua = init_state();
	if (!state->lua) {
		message(HAKA_LOG_FATAL, L"core", L"unable to create lua state");
		cleanup_thread_state(packet_module, state);
		return NULL;
	}

	state->capture = packet_module->init_state(thread_id);
	if (!state->capture) {
		message(HAKA_LOG_FATAL, L"core", L"unable to create packet capture state");
		cleanup_thread_state(packet_module, state);
		return NULL;
	}

	state->lua_function = do_file_as_function(state->lua, get_filter_script());
	if (state->lua_function == -1) {
		cleanup_thread_state(packet_module, state);
		return NULL;
	}

	return state;
}

void cleanup_thread_state(struct packet_module *packet_module, struct thread_state *state)
{
	assert(state);
	assert(packet_module);

	if (state->lua) {
		cleanup_state(state->lua);
		state->lua = NULL;
	}

	if (state->capture) {
		packet_module->cleanup_state(state->capture);
		state->capture = NULL;
	}

	free(state);
}

static atomic_t thread_running_count = 0;
static semaphore_t* thread_join = NULL;

void *thread_main_loop(void *_state)
{
	struct thread_state *state = (struct thread_state *)_state;
	struct packet *pkt = NULL;
	int error = 0;

	struct packet_module *packet_module = get_packet_module();
	assert(packet_module);

	thread_set_id(state->thread_id);

	while ((error = packet_module->receive(state->capture, &pkt)) == 0) {
		/* The packet can be NULL in case of failure in packet receive */
		if (pkt) {
			filter_result result = filter_wrapper(state, pkt);
			packet_module->verdict(pkt, result);
			pkt = NULL;
		}
	}

	if (thread_join && atomic_dec(&thread_running_count) == 0) {
		semaphore_post(thread_join);
	}

	return NULL;
}

void start_thread(struct thread_state *state)
{
	pthread_attr_t attr;
	int err;

	if (!thread_join) {
		thread_join = malloc(sizeof(semaphore_t));
		if (!thread_join) {
			error(L"memory error");
			return;
		}

		if (!semaphore_init(thread_join, 0)) {
			return;
		}
	}

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	err = pthread_create(&state->thread, &attr, thread_main_loop, state);
	if (err) {
		error(L"thread creation error: %s", errno_error(err));
		pthread_attr_destroy(&attr);
		return;
	}

	pthread_attr_destroy(&attr);
	atomic_inc(&thread_running_count);
}

void start_single(struct thread_state *state)
{
	atomic_inc(&thread_running_count);
	thread_main_loop(state);
}

void wait_threads()
{
	while (atomic_get(&thread_running_count) > 0) {
		semaphore_wait(thread_join);
	}

	sem_destroy(thread_join);
	thread_join = NULL;
}