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
static struct wl_seat *seat;
static struct wl_shm *shm;
static struct wl_pointer *pointer;
static struct wl_touch *touch;
static struct wl_keyboard *keyboard;
static struct xdg_wm_base *xdg_wm_base;
static struct zwlr_layer_shell_v1 *layer_shell;

struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_output *wl_output;

struct wl_surface *wl_surface;
struct wlr_egl egl;
struct wl_egl_window *egl_window;
struct wlr_egl_surface *egl_surface;
struct wl_callback *frame_callback;

static uint32_t output = UINT32_MAX;

static const uint32_t regular_height = 40;
static const uint32_t extended_height = 1080;
static uint32_t width = 0, height = regular_height;
static int32_t margin_top = 0;
static double alpha = 1.0;
static bool run_display = true;
static bool animate = false;
static bool keyboard_interactive = false;
static double frame = 0;
static int cur_x = -1, cur_y = -1;
static int buttons = 0;
static bool extended = false;
float regular_color[3] = { 0.5, 0.5, 0.5 };
float extended_color[3] = { 0.2, 0.2, 0.2 };

struct wl_cursor_image *cursor_image;
struct wl_surface *cursor_surface, *input_surface;

static struct {
	struct timespec last_frame;
	float color[3];
	int dec;
} demo;

static void draw(void);

static void surface_frame_callback(
		void *data, struct wl_callback *cb, uint32_t time) {
	wl_callback_destroy(cb);
	frame_callback = NULL;
	draw();
}

static struct wl_callback_listener frame_listener = {
	.done = surface_frame_callback
};

static void draw(void) {
	eglMakeCurrent(egl.display, egl_surface, egl_surface, egl.context);
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	long ms = (ts.tv_sec - demo.last_frame.tv_sec) * 1000 +
		(ts.tv_nsec - demo.last_frame.tv_nsec) / 1000000;

	if (animate) {
		frame += ms / 50.0;
		int32_t old_top = margin_top;
		margin_top = -(20 - ((int)frame % 20));
		if (old_top != margin_top) {
			zwlr_layer_surface_v1_set_margin(layer_surface,
					margin_top, 0, 0, 0);
			wl_surface_commit(wl_surface);
		}
	}

	glViewport(0, 0, width, height);
	glClearColor(demo.color[0], demo.color[1], demo.color[2], alpha);
	glClear(GL_COLOR_BUFFER_BIT);

	frame_callback = wl_surface_frame(wl_surface);
	wl_callback_add_listener(frame_callback, &frame_listener, NULL);

	eglSwapBuffers(egl.display, egl_surface);

	demo.last_frame = ts;
}

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t w, uint32_t h) {
	width = w;
	height = h;
	wlr_log(WLR_DEBUG, "layer_surface_configure: %u, %u", width, height);
	if (egl_window) {
		wlr_log(WLR_DEBUG, "resizing egl");
		wl_egl_window_resize(egl_window, width, height, 0, 0);
	}

	if (height == regular_height) {
		demo.color[0] = regular_color[0];
		demo.color[1] = regular_color[1];
		demo.color[2] = regular_color[2];
	} else {
		demo.color[0] = extended_color[0];
		demo.color[1] = extended_color[1];
		demo.color[2] = extended_color[2];
	}
	zwlr_layer_surface_v1_ack_configure(surface, serial);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	wlr_egl_destroy_surface(&egl, egl_surface);
	wl_egl_window_destroy(egl_window);
	zwlr_layer_surface_v1_destroy(surface);
	wl_surface_destroy(wl_surface);
	run_display = false;
}

struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void handle_click(void) {
	extended = !extended;
	if (extended) {
		zwlr_layer_surface_v1_set_size(layer_surface, width, extended_height);
	} else {
		zwlr_layer_surface_v1_set_size(layer_surface, width, regular_height);
	}
}

static void wl_touch_down(void *data, struct wl_touch *wl_touch,
		  uint32_t serial, uint32_t time, struct wl_surface *surface,
		  int32_t id, wl_fixed_t x_w, wl_fixed_t y_w) {
	handle_click();
}

static void wl_touch_up(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, int32_t id) {

}

static void wl_touch_motion(void *data, struct wl_touch *wl_touch,
		    uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w) {

}

static void wl_touch_frame(void *data, struct wl_touch *wl_touch) {

}

static void wl_touch_cancel(void *data, struct wl_touch *wl_touch) {

}

struct wl_touch_listener touch_listener = {
	.down = wl_touch_down,
	.up = wl_touch_up,
	.motion = wl_touch_motion,
	.frame = wl_touch_frame,
	.cancel = wl_touch_cancel,
};

static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct wl_cursor_image *image;
	image = cursor_image;
	wl_surface_attach(cursor_surface,
		wl_cursor_image_get_buffer(image), 0, 0);
	wl_surface_damage(cursor_surface, 1, 0,
		image->width, image->height);
	wl_surface_commit(cursor_surface);
	wl_pointer_set_cursor(wl_pointer, serial, cursor_surface,
		image->hotspot_x, image->hotspot_y);
	input_surface = surface;
}

static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface) {
	cur_x = cur_y = -1;
	buttons = 0;
}

static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	cur_x = wl_fixed_to_int(surface_x);
	cur_y = wl_fixed_to_int(surface_y);
}

static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	if (input_surface == wl_surface) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			if (button == BTN_RIGHT) {
				wlr_log(WLR_DEBUG, "exiting");
				run_display = false;
			} else {
				buttons++;
				handle_click();
			}
		} else {
			buttons--;
		}
	} else {
		assert(false && "Unknown surface");
	}
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

static void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	// Who cares
}

static void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	wlr_log(WLR_DEBUG, "Keyboard enter");
}

static void wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface) {
	wlr_log(WLR_DEBUG, "Keyboard leave");
}

static void wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	wlr_log(WLR_DEBUG, "Key event: %d %d", key, state);
}

static void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group) {
	// Who cares
}

static void wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay) {
	// Who cares
}

static struct wl_keyboard_listener keyboard_listener = {
	.keymap = wl_keyboard_keymap,
	.enter = wl_keyboard_enter,
	.leave = wl_keyboard_leave,
	.key = wl_keyboard_key,
	.modifiers = wl_keyboard_modifiers,
	.repeat_info = wl_keyboard_repeat_info,
};

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
		enum wl_seat_capability caps) {
	if ((caps & WL_SEAT_CAPABILITY_TOUCH)) {
		touch = wl_seat_get_touch(wl_seat);
		wl_touch_add_listener(touch, &touch_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER)) {
		pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
		keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
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

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name,
				&wl_shm_interface, 1);
	} else if (strcmp(interface, "wl_output") == 0) {
		if (output != UINT32_MAX) {
			if (!wl_output) {
				wl_output = wl_registry_bind(registry, name,
						&wl_output_interface, 1);
			} else {
				output--;
			}
		}
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name,
				&wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(
				registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(
				registry, name, &xdg_wm_base_interface, 1);
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
	char *namespace = "wlroots";
	int32_t margin_right = 0, margin_bottom = 0, margin_left = 0;
	int c;
	while ((c = getopt(argc, argv, "kn:o:m:t:")) != -1) {
		switch (c) {
		case 'o':
			output = atoi(optarg);
			break;
		case 't':
			alpha = atof(optarg);
			break;
		case 'm': {
			char *endptr = optarg;
			margin_top = strtol(endptr, &endptr, 10);
			assert(*endptr == ',');
			margin_right = strtol(endptr + 1, &endptr, 10);
			assert(*endptr == ',');
			margin_bottom = strtol(endptr + 1, &endptr, 10);
			assert(*endptr == ',');
			margin_left = strtol(endptr + 1, &endptr, 10);
			assert(!*endptr);
			break;
		}
		case 'n':
			animate = true;
			break;
		case 'k':
			keyboard_interactive = true;
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
	demo.color[0] = regular_color[0];
	demo.color[1] = regular_color[1];
	demo.color[2] = regular_color[2];

	struct wl_cursor_theme *cursor_theme =
		wl_cursor_theme_load(NULL, 16, shm);
	assert(cursor_theme);
	struct wl_cursor *cursor =
		wl_cursor_theme_get_cursor(cursor_theme, "left_ptr");
	assert(cursor);
	cursor_image = cursor->images[0];

	cursor = wl_cursor_theme_get_cursor(cursor_theme, "tcross");
	if (cursor == NULL) {
		cursor = wl_cursor_theme_get_cursor(cursor_theme, "left_ptr");
	}
	assert(cursor);

	cursor_surface = wl_compositor_create_surface(compositor);
	assert(cursor_surface);

	EGLint attribs[] = { EGL_ALPHA_SIZE, 8, EGL_NONE };
	wlr_egl_init(&egl, EGL_PLATFORM_WAYLAND_EXT, display,
			attribs, WL_SHM_FORMAT_ARGB8888);

	wl_surface = wl_compositor_create_surface(compositor);
	assert(wl_surface);

	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
				wl_surface, wl_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, namespace);
	assert(layer_surface);
	zwlr_layer_surface_v1_set_size(layer_surface, width, height);
	zwlr_layer_surface_v1_set_anchor(
		layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | \
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | \
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
	);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, height);
	zwlr_layer_surface_v1_set_margin(layer_surface,
			margin_top, margin_right, margin_bottom, margin_left);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
			layer_surface, keyboard_interactive);
	zwlr_layer_surface_v1_add_listener(layer_surface,
			&layer_surface_listener, layer_surface);
	wl_surface_commit(wl_surface);
	wl_display_roundtrip(display);

	egl_window = wl_egl_window_create(wl_surface, width, height);
	assert(egl_window);
	egl_surface = wlr_egl_create_surface(&egl, egl_window);
	assert(egl_surface);

	wl_display_roundtrip(display);
	draw();

	while (wl_display_dispatch(display) != -1 && run_display) {
		// This space intentionally left blank
	}

	wl_cursor_theme_destroy(cursor_theme);
	return 0;
}
