#define _POSIX_C_SOURCE 200112L
#include <linux/input-event-codes.h>
#include <assert.h>
#include <GLES2/gl2.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>
#include <wlr/render/egl.h>
#include <wlr/util/log.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *xdg_wm_base;
static struct zwlr_layer_shell_v1 *layer_shell;

struct wlr_egl egl;
struct wl_callback *frame_callback;

static const uint32_t regular_height = 40;
static const double alpha = 0.9;
static uint32_t height = regular_height;
static bool run_display = true;
static bool extended = false;
float regular_color[3] = { 0.5, 0.5, 0.5 };
float extended_color[3] = { 0.2, 0.2, 0.2 };
float color[3];

struct bar_output {
	struct wl_egl_window *egl_window;
	struct wlr_egl_surface *egl_surface;
	struct wl_output *output;
	struct wl_surface *surface;
	struct wl_list link;
	struct zwlr_layer_surface_v1 *layer_surface;

	uint32_t width, height;
	int32_t scale;
};

struct touch_slot {
	int32_t id;
	uint32_t time;
	struct bar_output *output;
	double start_x, start_y;
	double x, y;
};

struct bar_touch {
	struct wl_touch *touch;
	struct touch_slot slots[16];
};

struct bar_pointer {
	struct wl_pointer *pointer;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor_image *cursor_image;
	struct wl_surface *cursor_surface;
	struct bar_output *current;
	int x, y;
	uint32_t serial;
};

struct bar_seat {
	uint32_t wl_name;
	struct wl_seat *wl_seat;
	struct bar_pointer pointer;
	struct bar_touch touch;
	struct wl_list link; // bar_seat:link
};

static struct wl_list bar_outputs;
static struct wl_list bar_seats;
static void draw(struct bar_output* output);

static void surface_frame_callback(
		void *data, struct wl_callback *cb, uint32_t time) {
	wl_callback_destroy(cb);
	frame_callback = NULL;
	draw((struct bar_output*) data);
}

static struct wl_callback_listener frame_listener = {
	.done = surface_frame_callback
};

static void draw(struct bar_output *output) {
	eglMakeCurrent(egl.display, output->egl_surface, output->egl_surface, egl.context);

	glViewport(0, 0, output->width, output->height);
	glClearColor(color[0], color[1], color[2], alpha);
	glClear(GL_COLOR_BUFFER_BIT);

	frame_callback = wl_surface_frame(output->surface);
	wl_callback_add_listener(frame_callback, &frame_listener, output);

	eglSwapBuffers(egl.display, output->egl_surface);
}

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t w, uint32_t h) {
	struct bar_output *output = data;
	output->width = w;
	output->height = h;
	wlr_log(WLR_DEBUG, "layer_surface_configure: %u, %u", output->width, output->height);
	if (output->egl_window) {
		wlr_log(WLR_DEBUG, "resizing egl");
		wl_egl_window_resize(output->egl_window, output->width, output->height, 0, 0);
	}

	if (extended) {
		color[0] = extended_color[0];
		color[1] = extended_color[1];
		color[2] = extended_color[2];
	} else {
		color[0] = regular_color[0];
		color[1] = regular_color[1];
		color[2] = regular_color[2];
	}
	zwlr_layer_surface_v1_ack_configure(output->layer_surface, serial);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	struct bar_output *output = data;
	wlr_egl_destroy_surface(&egl, output->egl_surface);
	wl_egl_window_destroy(output->egl_window);
	zwlr_layer_surface_v1_destroy(output->layer_surface);
	wl_surface_destroy(output->surface);
	run_display = false;
}

struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

/*
static void handle_click(void) {
	extended = !extended;
	if (extended) {
		zwlr_layer_surface_v1_set_size(layer_surface, width, extended_height);
	} else {
		zwlr_layer_surface_v1_set_size(layer_surface, width, regular_height);
	}
	wl_surface_commit(wl_surface);
}
*/

static void update_cursor(struct bar_seat *seat) {
	struct bar_pointer *pointer = &seat->pointer;
	if (!pointer || !pointer->cursor_surface) {
		return;
	}
	if (pointer->cursor_theme) {
		wl_cursor_theme_destroy(pointer->cursor_theme);
	}
	//const char *cursor_theme = getenv("XCURSOR_THEME");
	unsigned cursor_size = 24;
	const char *env_cursor_size = getenv("XCURSOR_SIZE");
	if (env_cursor_size && strlen(env_cursor_size) > 0) {
		errno = 0;
		char *end;
		unsigned size = strtoul(env_cursor_size, &end, 10);
		if (!*end && errno == 0) {
			cursor_size = size;
		}
	}
	int scale = pointer->current ? pointer->current->scale : 1;
	wlr_log(WLR_DEBUG, "scale: %u", scale);
	pointer->cursor_theme = wl_cursor_theme_load(
		NULL, cursor_size * scale, shm);
	assert(pointer->cursor_theme);
	struct wl_cursor *cursor;
	cursor = wl_cursor_theme_get_cursor(pointer->cursor_theme, "left_ptr");
	pointer->cursor_image = cursor->images[0];
	wl_surface_set_buffer_scale(pointer->cursor_surface, scale);
	wl_surface_attach(pointer->cursor_surface,
			wl_cursor_image_get_buffer(pointer->cursor_image), 0, 0);
	wl_pointer_set_cursor(pointer->pointer, pointer->serial,
			pointer->cursor_surface,
			pointer->cursor_image->hotspot_x / scale,
			pointer->cursor_image->hotspot_y / scale);
	wl_surface_damage_buffer(pointer->cursor_surface, 0, 0,
			INT32_MAX, INT32_MAX);
	wl_surface_commit(pointer->cursor_surface);
}

static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct bar_seat *seat = data;
	struct bar_pointer *pointer = &seat->pointer;
	pointer->serial = serial;
	struct bar_output *output;
	wl_list_for_each(output, &bar_outputs, link) {
		if (output->surface == surface) {
			pointer->current = output;
			break;
		}
	}
	update_cursor(seat);
}

static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface) {
	struct bar_seat *seat = data;
	seat->pointer.current = NULL;
}

static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct bar_seat *seat = data;
	seat->pointer.x = wl_fixed_to_int(surface_x);
	seat->pointer.y = wl_fixed_to_int(surface_y);
}

static void process_hotspots(struct bar_output *output,
		double x, double y, uint32_t button) {
	x *= output->scale;
	y *= output->scale;

	/* click */
	wlr_log(WLR_DEBUG, "Click at (%f, %f) with button %u", x, y, button);
}

static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	struct bar_seat *seat = data;
	struct bar_pointer *pointer = &seat->pointer;
	struct bar_output *output = pointer->current;
	assert(output && "button with no active output");

	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		return;
	}

	process_hotspots(output, pointer->x, pointer->y, button);
}

static void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
	// Who cares
}

static void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
	// Who cares
}

static void wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis_source) {
	// Who cares
}

static void wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis) {
	// Who cares
}

static void wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis, int32_t discrete) {
	// Who cares
}

struct wl_pointer_listener pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
	.frame = wl_pointer_frame,
	.axis_source = wl_pointer_axis_source,
	.axis_stop = wl_pointer_axis_stop,
	.axis_discrete = wl_pointer_axis_discrete,
};

static struct touch_slot *get_touch_slot(struct bar_touch *touch, int32_t id) {
	ssize_t next = -1;
	for (size_t i = 0; i < sizeof(touch->slots) / sizeof(*touch->slots); ++i) {
		if (touch->slots[i].id == id) {
			return &touch->slots[i];
		}
		if (next == -1 && !touch->slots[i].output) {
			next = i;
		}
	}
	if (next == -1) {
		wlr_log(WLR_DEBUG, "Ran out of touch slots");
		return NULL;
	}
	return &touch->slots[next];
}

static void wl_touch_down(void *data, struct wl_touch *wl_touch,
		  uint32_t serial, uint32_t time, struct wl_surface *surface,
		  int32_t id, wl_fixed_t x, wl_fixed_t y) {
	struct bar_seat *seat = data;
	struct bar_output *_output = NULL, *output = NULL;
	wl_list_for_each(_output, &bar_outputs, link) {
		if (_output->surface == surface) {
			output = _output;
			break;
		}
	}
	if (!output) {
		wlr_log(WLR_DEBUG, "Got touch event for unknown surface");
		return;
	}
	struct touch_slot *slot = get_touch_slot(&seat->touch, id);
	if (!slot) {
		return;
	}
	slot->id = id;
	slot->output = output;
	slot->x = slot->start_x = wl_fixed_to_double(x);
	slot->y = slot->start_y = wl_fixed_to_double(y);
	slot->time = time;
}

static void wl_touch_up(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, int32_t id) {
	struct bar_seat *seat = data;
	struct touch_slot *slot = get_touch_slot(&seat->touch, id);
	if (!slot) {
		return;
	}
	if (time - slot->time < 500) {
		// Tap, treat it like a pointer click
		process_hotspots(slot->output, slot->x, slot->y, BTN_LEFT);
	}
	slot->output = NULL;

}

static void wl_touch_motion(void *data, struct wl_touch *wl_touch,
		    uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	struct bar_seat *seat = data;
	struct touch_slot *slot = get_touch_slot(&seat->touch, id);
	if (!slot) {
		return;
	}
	int prev_progress = (int)((slot->x - slot->start_x)
			/ slot->output->width * 100);
	slot->x = wl_fixed_to_double(x);
	slot->y = wl_fixed_to_double(y);
	// "progress" is a measure from 0..100 representing the fraction of the
	// output the touch gesture has travelled, positive when moving to the right
	// and negative when moving to the left.
	int progress = (int)((slot->x - slot->start_x)
			/ slot->output->width * 100);
	if (abs(progress) / 20 != abs(prev_progress) / 20) {
		// workspace_next(seat->bar, slot->output, progress - prev_progress < 0);
	}

}

static void wl_touch_frame(void *data, struct wl_touch *wl_touch) {

}

static void wl_touch_cancel(void *data, struct wl_touch *wl_touch) {
	struct bar_seat *seat = data;
	struct bar_touch *touch = &seat->touch;
	for (size_t i = 0; i < sizeof(touch->slots) / sizeof(*touch->slots); ++i) {
		touch->slots[i].output = NULL;
	}
}

struct wl_touch_listener touch_listener = {
	.down = wl_touch_down,
	.up = wl_touch_up,
	.motion = wl_touch_motion,
	.frame = wl_touch_frame,
	.cancel = wl_touch_cancel,
};

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
		enum wl_seat_capability caps) {
	struct bar_seat *seat = data;

	bool have_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
	bool have_touch = caps & WL_SEAT_CAPABILITY_TOUCH;

	if (!have_pointer && seat->pointer.pointer != NULL) {
		wl_pointer_release(seat->pointer.pointer);
		seat->pointer.pointer = NULL;
	} else if (have_pointer && seat->pointer.pointer == NULL) {
		seat->pointer.pointer = wl_seat_get_pointer(wl_seat);
		if (run_display && !seat->pointer.cursor_surface) {
			seat->pointer.cursor_surface =
				wl_compositor_create_surface(compositor);
			assert(seat->pointer.cursor_surface);
		}
		wl_pointer_add_listener(seat->pointer.pointer, &pointer_listener, seat);
	}
	if (!have_touch && seat->touch.touch != NULL) {
		wl_touch_release(seat->touch.touch);
		seat->touch.touch = NULL;
	} else if (have_touch && seat->touch.touch == NULL) {
		seat->touch.touch = wl_seat_get_touch(wl_seat);
		wl_touch_add_listener(seat->touch.touch, &touch_listener, seat);
	}
}

static void seat_handle_name(void *data, struct wl_seat *wl_seat,
		const char *name) {
	// Who cares
}

const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name,
};

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x,
		int32_t y, int32_t width_mm, int32_t height_mm, int32_t subpixel,
		const char *make, const char *model, int32_t transform) {
	// Who cares
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
	// Who cares
}

static void output_done(void *data, struct wl_output *wl_output) {
	// Who cares
}

static void output_scale(void *data, struct wl_output *wl_output,
		int32_t factor) {
	struct bar_output *output = data;
	wlr_log(WLR_DEBUG, "output scale %d", factor);
	output->scale = factor;
}
struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name,
				&wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct bar_output *output = calloc(1, sizeof(struct bar_output));
		output->output = wl_registry_bind(registry, name,
			&wl_output_interface, 3);
		wl_list_insert(&bar_outputs, &output->link);
		wl_output_add_listener(output->output, &output_listener, output);
		wlr_log(WLR_DEBUG, "output listener");
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct bar_seat *seat = calloc(1, sizeof(struct bar_seat));
		if (!seat) {
			wlr_log(WLR_DEBUG, "Failed to allocate bar_seat");
			return;
		}
		seat->wl_name = name;
		// NOTE this used version 3 in swaybar
		seat->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 3);
		wl_seat_add_listener(seat->wl_seat, &seat_listener, seat);
		wl_list_insert(&bar_seats, &seat->link);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(
				registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(
				registry, name, &xdg_wm_base_interface, 2);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	// who cares
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

int main(int argc, char **argv) {
	wlr_log_init(WLR_DEBUG, NULL);
	wl_list_init(&bar_outputs);
	wl_list_init(&bar_seats);
	char *namespace = "wlroots";
	int c;
	while ((c = getopt(argc, argv, "h:")) != -1) {
		switch (c) {
		case 'h':
			printf("Help text here\n");
			return 0;
			break;
		default:
			break;
		}
	}

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "Failed to create display\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (compositor == NULL) {
		fprintf(stderr, "wl_compositor not available\n");
		return 1;
	}
	if (shm == NULL) {
		fprintf(stderr, "wl_shm not available\n");
		return 1;
	}
	if (layer_shell == NULL) {
		fprintf(stderr, "layer_shell not available\n");
		return 1;
	}

	wlr_log(WLR_DEBUG, "Starting");
	color[0] = regular_color[0];
	color[1] = regular_color[1];
	color[2] = regular_color[2];


	EGLint attribs[] = { EGL_ALPHA_SIZE, 8, EGL_NONE };
	wlr_egl_init(&egl, EGL_PLATFORM_WAYLAND_EXT, display,
			attribs, WL_SHM_FORMAT_ARGB8888);


	struct bar_output *output;
	wl_list_for_each(output, &bar_outputs, link) {
		output->surface = wl_compositor_create_surface(compositor);
		assert(output->surface);

		output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			layer_shell,
			output->surface,
			output->output,
			ZWLR_LAYER_SHELL_V1_LAYER_TOP,
			namespace
		);
		assert(output->layer_surface);

		zwlr_layer_surface_v1_set_size(
			output->layer_surface,
			output->width,
			height
		);
		zwlr_layer_surface_v1_set_anchor(
			output->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | \
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | \
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
		);
		zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, height);
		zwlr_layer_surface_v1_set_keyboard_interactivity(output->layer_surface, false);
		zwlr_layer_surface_v1_add_listener(
			output->layer_surface,
			&layer_surface_listener,
			output
		);
		wl_surface_commit(output->surface);
		wl_display_roundtrip(display);

		output->egl_window = wl_egl_window_create(output->surface, output->width, height);
		assert(output->egl_window);
		output->egl_surface = wlr_egl_create_surface(&egl, output->egl_window);
		assert(output->egl_surface);
		wl_display_roundtrip(display);
		draw(output);
	}

	while (wl_display_dispatch(display) != -1 && run_display) {
		// This space intentionally left blank
	}

	return 0;
}
