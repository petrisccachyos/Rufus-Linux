/*
 * Linux front end for Rufus.
 *
 * The control order, labels, and resource IDs mirror src/rufus.rc and
 * src/resource.h.  The Win32 implementation remains in the original source
 * tree; this file supplies the Linux window and block-device operations.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../resource.h"

typedef struct {
	GtkWidget *window;
	GtkWidget *device;
	GtkWidget *save_button;
	GtkWidget *boot_selection;
	GtkWidget *hash_button;
	GtkWidget *image_option;
	GtkWidget *persistence_slider;
	GtkWidget *persistence_size;
	GtkWidget *persistence_units;
	GtkWidget *partition_scheme;
	GtkWidget *target_system;
	GtkWidget *advanced_device_expander;
	GtkWidget *list_usb_hdd;
	GtkWidget *old_bios_fixes;
	GtkWidget *uefi_validation;
	GtkWidget *volume_label;
	GtkWidget *file_system;
	GtkWidget *cluster_size;
	GtkWidget *advanced_format_expander;
	GtkWidget *quick_format;
	GtkWidget *extended_label;
	GtkWidget *bad_blocks;
	GtkWidget *bad_block_passes;
	GtkWidget *status;
	GtkWidget *progress;
	GtkWidget *select_button;
	GtkWidget *start_button;
	GtkWidget *close_button;
	GString *log_messages;
	GHashTable *device_sizes;
	gchar *image_path;
	guint pulse_source;
	gboolean include_usb;
	gboolean verify_after_write;
	gboolean eject_after_write;
	gboolean busy;
} RufusApp;

typedef struct {
	gchar *device_path;
	gchar *image_path;
	guint64 image_size;
	gboolean verify_after_write;
	gboolean eject_after_write;
	gboolean write_completed;
	gboolean verified;
	gboolean eject_attempted;
	gboolean eject_succeeded;
	gchar *eject_error;
} WriteJob;

typedef struct {
	gchar *image_path;
} HashJob;

static void refresh_devices(RufusApp *app);
static void update_start_button(RufusApp *app);
static void set_busy(RufusApp *app, gboolean busy);
static void on_list_usb_toggled(GtkToggleButton *button, gpointer user_data);

static void append_log(RufusApp *app, const gchar *message)
{
	g_string_append_printf(app->log_messages, "%s\n", message);
}

static gchar *preferences_file(void)
{
	return g_build_filename(g_get_user_config_dir(), "rufus-linux", "settings.ini", NULL);
}

static void load_preferences(RufusApp *app)
{
	GKeyFile *key_file = g_key_file_new();
	GError *error = NULL;
	gchar *path = preferences_file();
	if (g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
		app->include_usb = g_key_file_get_boolean(key_file, "Devices", "IncludeUsbHardDrives", NULL);
		app->verify_after_write = g_key_file_get_boolean(key_file, "Operations", "VerifyAfterWrite", NULL);
		app->eject_after_write = g_key_file_get_boolean(key_file, "Operations", "EjectAfterWrite", NULL);
	}
	g_clear_error(&error);
	g_free(path);
	g_key_file_unref(key_file);
}

static void save_preferences(RufusApp *app)
{
	GKeyFile *key_file = g_key_file_new();
	GError *error = NULL;
	gchar *path = preferences_file();
	gchar *directory = g_path_get_dirname(path);
	gchar *data;
	gsize length;

	g_key_file_set_boolean(key_file, "Devices", "IncludeUsbHardDrives", app->include_usb);
	g_key_file_set_boolean(key_file, "Operations", "VerifyAfterWrite", app->verify_after_write);
	g_key_file_set_boolean(key_file, "Operations", "EjectAfterWrite", app->eject_after_write);
	data = g_key_file_to_data(key_file, &length, &error);
	if (data != NULL && error == NULL && g_mkdir_with_parents(directory, 0700) != 0) {
		g_set_error(&error, G_FILE_ERROR, g_file_error_from_errno(errno),
			"Could not create settings folder: %s", g_strerror(errno));
	}
	if (data != NULL && error == NULL)
		g_file_set_contents(path, data, (gssize)length, &error);
	if (error != NULL && app->log_messages != NULL)
		append_log(app, error->message);
	g_clear_error(&error);
	g_free(data);
	g_free(directory);
	g_free(path);
	g_key_file_unref(key_file);
}

static void set_control_id(GtkWidget *widget, const gchar *id)
{
	gtk_widget_set_name(widget, id);
}

#define SET_ID(widget, id) G_STMT_START { \
	gchar *resource_name = g_strdup_printf("%s:%d", #id, (id)); \
	set_control_id((widget), resource_name); \
	g_free(resource_name); \
} G_STMT_END

static GtkWidget *make_label(const gchar *text, guint id)
{
	GtkWidget *label = gtk_label_new(text);
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	SET_ID(label, id);
	return label;
}

static GtkWidget *make_heading(const gchar *text, guint id)
{
	GtkWidget *label = make_label(NULL, id);
	gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
	gtk_label_set_markup(GTK_LABEL(label), markup);
	g_free(markup);
	return label;
}

static GtkWidget *make_combo(guint id, const gchar *const *items)
{
	GtkWidget *combo = gtk_combo_box_text_new();
	SET_ID(combo, id);
	for (guint i = 0; items[i] != NULL; i++)
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), items[i]);
	if (items[0] != NULL)
		gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
	return combo;
}

static gboolean spawn_capture(const gchar *const argv[], gchar **stdout_text,
		gchar **stderr_text, GError **error)
{
	GSubprocess *process;
	gboolean communicated;

	process = g_subprocess_newv(argv,
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
	if (process == NULL)
		return FALSE;
	communicated = g_subprocess_communicate_utf8(process, NULL, NULL,
		stdout_text, stderr_text, error);
	if (communicated && !g_subprocess_get_successful(process)) {
		const gchar *detail = stderr_text != NULL && *stderr_text != NULL ? *stderr_text : "";
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"Command failed (%d): %s%s%s", g_subprocess_get_exit_status(process),
			argv[0], *detail ? " — " : "", detail);
		communicated = FALSE;
	}
	g_object_unref(process);
	return communicated;
}

static const gchar *json_string(JsonObject *object, const gchar *member)
{
	JsonNode *node = json_object_get_member(object, member);
	if (node == NULL || JSON_NODE_HOLDS_NULL(node) || !JSON_NODE_HOLDS_VALUE(node))
		return "";
	if (json_node_get_value_type(node) != G_TYPE_STRING)
		return "";
	return json_node_get_string(node);
}

static guint64 json_uint64(JsonObject *object, const gchar *member)
{
	JsonNode *node = json_object_get_member(object, member);
	if (node == NULL || JSON_NODE_HOLDS_NULL(node) || !JSON_NODE_HOLDS_VALUE(node))
		return 0;
	if (json_node_get_value_type(node) == G_TYPE_INT64)
		return (guint64)json_node_get_int(node);
	if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
		return (guint64)json_node_get_double(node);
	return 0;
}

static gboolean json_bool(JsonObject *object, const gchar *member)
{
	JsonNode *node = json_object_get_member(object, member);
	if (node == NULL || JSON_NODE_HOLDS_NULL(node) || !JSON_NODE_HOLDS_VALUE(node))
		return FALSE;
	if (json_node_get_value_type(node) == G_TYPE_BOOLEAN)
		return json_node_get_boolean(node);
	if (json_node_get_value_type(node) == G_TYPE_INT64)
		return json_node_get_int(node) != 0;
	return FALSE;
}

static JsonArray *load_block_tree(JsonParser **parser_out, GError **error)
{
	static const gchar *const argv[] = {
		"lsblk", "--json", "--bytes", "--paths", "--output",
		"PATH,TYPE,RM,TRAN,SIZE,MODEL,RO,MOUNTPOINTS", NULL
	};
	gchar *output = NULL;
	gchar *errors = NULL;
	JsonParser *parser;
	JsonNode *root;
	JsonArray *devices;

	if (!spawn_capture(argv, &output, &errors, error)) {
		g_free(output);
		g_free(errors);
		return NULL;
	}
	g_free(errors);
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, output, -1, error)) {
		g_object_unref(parser);
		g_free(output);
		return NULL;
	}
	g_free(output);
	root = json_parser_get_root(parser);
	if (!JSON_NODE_HOLDS_OBJECT(root)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			"lsblk did not return a device list.");
		g_object_unref(parser);
		return NULL;
	}
	devices = json_object_get_array_member(json_node_get_object(root), "blockdevices");
	if (devices == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			"lsblk returned no block device array.");
		g_object_unref(parser);
		return NULL;
	}
	*parser_out = parser;
	return devices;
}

static void collect_disk_paths(JsonArray *nodes, GHashTable *disks)
{
	for (guint i = 0; i < json_array_get_length(nodes); i++) {
		JsonNode *node = json_array_get_element(nodes, i);
		JsonObject *object;
		JsonArray *children;
		const gchar *type;
		const gchar *path;

		if (!JSON_NODE_HOLDS_OBJECT(node))
			continue;
		object = json_node_get_object(node);
		type = json_string(object, "type");
		path = json_string(object, "path");
		if (g_strcmp0(type, "disk") == 0 && *path)
			g_hash_table_add(disks, g_strdup(path));
		children = json_object_get_array_member(object, "children");
		if (children != NULL)
			collect_disk_paths(children, disks);
	}
}

static GHashTable *root_backing_disks(GError **error)
{
	static const gchar *const findmnt_argv[] = {
		"findmnt", "--noheadings", "--evaluate", "--output", "SOURCE",
		"--target", "/", NULL
	};
	gchar *source = NULL;
	gchar *command_errors = NULL;
	GHashTable *disks = NULL;
	JsonParser *parser = NULL;
	JsonArray *tree;
	gchar *root_source = NULL;
	gchar *lsblk_output = NULL;
	gchar *bracket;
	gchar *lsblk_argv[8];

	if (!spawn_capture(findmnt_argv, &source, &command_errors, error)) {
		g_free(source);
		g_free(command_errors);
		return NULL;
	}
	g_free(command_errors);
	g_strstrip(source);
	bracket = strchr(source, '[');
	if (bracket != NULL)
		*bracket = '\0';
	if (!g_str_has_prefix(source, "/dev/")) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"Could not identify the disk containing Linux /. Drive listing is disabled for safety.");
		g_free(source);
		return NULL;
	}
	lsblk_argv[0] = "lsblk";
	lsblk_argv[1] = "--json";
	lsblk_argv[2] = "--inverse";
	lsblk_argv[3] = "--paths";
	lsblk_argv[4] = "--output";
	lsblk_argv[5] = "PATH,TYPE";
	root_source = source;
	lsblk_argv[6] = root_source;
	lsblk_argv[7] = NULL;
	if (!spawn_capture((const gchar *const *)lsblk_argv, &lsblk_output,
			&command_errors, error)) {
		g_free(root_source);
		g_free(lsblk_output);
		g_free(command_errors);
		return NULL;
	}
	g_free(root_source);
	g_free(command_errors);
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, lsblk_output, -1, error)) {
		g_free(lsblk_output);
		g_object_unref(parser);
		return NULL;
	}
	g_free(lsblk_output);
	tree = json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)),
		"blockdevices");
	disks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (tree != NULL)
		collect_disk_paths(tree, disks);
	g_object_unref(parser);
	if (g_hash_table_size(disks) == 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"Linux / has no identifiable backing disk. Drive listing is disabled for safety.");
		g_hash_table_unref(disks);
		return NULL;
	}
	return disks;
}

static gchar *capacity_text(guint64 size)
{
	return g_format_size(size);
}

static void refresh_devices(RufusApp *app)
{
	JsonParser *parser = NULL;
	JsonArray *devices;
	GHashTable *protected_disks;
	GError *error = NULL;
	const gchar *active_id;
	gchar *previous_id;
	gboolean list_usb;
	guint listed = 0;

	if (app->busy)
		return;
	devices = load_block_tree(&parser, &error);
	if (devices == NULL) {
		gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->device));
		g_hash_table_remove_all(app->device_sizes);
		update_start_button(app);
		gtk_label_set_text(GTK_LABEL(app->status), error->message);
		g_clear_error(&error);
		return;
	}
	protected_disks = root_backing_disks(&error);
	if (protected_disks == NULL) {
		gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->device));
		g_hash_table_remove_all(app->device_sizes);
		update_start_button(app);
		gtk_label_set_text(GTK_LABEL(app->status), error->message);
		g_clear_error(&error);
		g_object_unref(parser);
		return;
	}

	active_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->device));
	previous_id = active_id != NULL ? g_strdup(active_id) : NULL;
	gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->device));
	g_hash_table_remove_all(app->device_sizes);
	list_usb = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->list_usb_hdd));
	for (guint i = 0; i < json_array_get_length(devices); i++) {
		JsonNode *node = json_array_get_element(devices, i);
		JsonObject *object;
		const gchar *path, *type, *transport, *model;
		gboolean removable, readonly;
		guint64 size;
		gchar *size_label, *display;
		guint64 *stored_size;

		if (!JSON_NODE_HOLDS_OBJECT(node))
			continue;
		object = json_node_get_object(node);
		path = json_string(object, "path");
		type = json_string(object, "type");
		transport = json_string(object, "tran");
		model = json_string(object, "model");
		removable = json_bool(object, "rm");
		readonly = json_bool(object, "ro");
		size = json_uint64(object, "size");
		if (g_strcmp0(type, "disk") != 0 || !*path || readonly ||
			(!removable && !(list_usb && g_strcmp0(transport, "usb") == 0)) ||
			g_hash_table_contains(protected_disks, path))
			continue;

		size_label = capacity_text(size);
		display = g_strdup_printf("%s  —  %s  —  %s", path, size_label,
			*model ? model : "USB storage");
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->device), path, display);
		stored_size = g_new(guint64, 1);
		*stored_size = size;
		g_hash_table_insert(app->device_sizes, g_strdup(path), stored_size);
		g_free(size_label);
		g_free(display);
		listed++;
	}
	if (previous_id != NULL && gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->device), previous_id)) {
		/* Keep the user's selected drive after a refresh. */
	} else if (listed > 0) {
		gtk_combo_box_set_active(GTK_COMBO_BOX(app->device), 0);
	}
	g_free(previous_id);
	if (listed == 0)
		gtk_label_set_text(GTK_LABEL(app->status), "No USB drive detected");
	else if (!app->image_path)
		gtk_label_set_text(GTK_LABEL(app->status), "READY");
	update_start_button(app);
	g_hash_table_unref(protected_disks);
	g_object_unref(parser);
}

static void on_device_changed(GtkComboBox *combo, gpointer user_data)
{
	(void)combo;
	update_start_button((RufusApp *)user_data);
}

static void on_save_device(GtkButton *button, gpointer user_data)
{
	RufusApp *app = user_data;
	const gchar *device = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->device));
	GtkWidget *dialog;
	gchar *output = NULL, *errors = NULL, *filename = NULL;
	GError *error = NULL;
	(void)button;
	if (device == NULL)
		return;
	{
		const gchar *argv[] = {
			"lsblk", "--bytes", "--paths", "--output",
			"NAME,PATH,TYPE,SIZE,MODEL,TRAN,FSTYPE,LABEL,UUID,MOUNTPOINTS",
			device, NULL
		};
		if (!spawn_capture(argv, &output, &errors, &error)) {
			GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
				GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
				"Could not read drive information.");
			gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(error_dialog),
				"%s", error != NULL ? error->message : "Unknown error");
			gtk_dialog_run(GTK_DIALOG(error_dialog));
			gtk_widget_destroy(error_dialog);
			g_clear_error(&error);
			g_free(errors);
			g_free(output);
			return;
		}
	}
	g_free(errors);
	dialog = gtk_file_chooser_dialog_new("Save drive information",
		GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SAVE,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "rufus-drive-info.txt");
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
		filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
	gtk_widget_destroy(dialog);
	if (filename != NULL) {
		if (!g_file_set_contents(filename, output, -1, &error)) {
			GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
				GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
				"Could not save drive information.");
			gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(error_dialog),
				"%s", error != NULL ? error->message : "Unknown error");
			gtk_dialog_run(GTK_DIALOG(error_dialog));
			gtk_widget_destroy(error_dialog);
			g_clear_error(&error);
		} else {
			gchar *entry = g_strdup_printf("Saved drive information: %s", filename);
			append_log(app, entry);
			g_free(entry);
			gtk_label_set_text(GTK_LABEL(app->status), "DRIVE INFORMATION SAVED");
		}
	}
	g_free(filename);
	g_free(output);
}

static void on_about(GtkToolButton *button, gpointer user_data)
{
	RufusApp *app = user_data;
	(void)button;
	gtk_show_about_dialog(GTK_WINDOW(app->window),
		"program-name", "Rufus",
		"version", "4.16.2443 (Linux port)",
		"comments", "The Reliable USB Formatting Utility",
		"website", "https://rufus.ie",
		"license-type", GTK_LICENSE_GPL_3_0,
		NULL);
}

static void on_settings(GtkToolButton *button, gpointer user_data)
{
	RufusApp *app = user_data;
	GtkWidget *dialog, *content, *devices, *include_usb, *operations;
	GtkWidget *verify, *power_off, *note;
	(void)button;
	dialog = gtk_dialog_new_with_buttons("Settings", GTK_WINDOW(app->window),
		GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL,
		"_Save", GTK_RESPONSE_ACCEPT, NULL);
	gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
	content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_set_border_width(GTK_CONTAINER(content), 14);
	gtk_box_set_spacing(GTK_BOX(content), 8);
	devices = make_heading("Devices", 40000);
	gtk_widget_set_margin_top(devices, 6);
	gtk_box_pack_start(GTK_BOX(content), devices, FALSE, FALSE, 0);
	include_usb = gtk_check_button_new_with_label("List USB Hard Drives");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(include_usb), app->include_usb);
	gtk_box_pack_start(GTK_BOX(content), include_usb, FALSE, FALSE, 0);
	operations = make_heading("Write operations", 40001);
	gtk_widget_set_margin_top(operations, 8);
	gtk_box_pack_start(GTK_BOX(content), operations, FALSE, FALSE, 0);
	verify = gtk_check_button_new_with_label("Verify the image after writing");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(verify), app->verify_after_write);
	gtk_box_pack_start(GTK_BOX(content), verify, FALSE, FALSE, 0);
	power_off = gtk_check_button_new_with_label("Safely power off the USB drive when finished");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(power_off), app->eject_after_write);
	gtk_box_pack_start(GTK_BOX(content), power_off, FALSE, FALSE, 0);
	note = gtk_label_new("Writing an image always asks for confirmation before erasing a drive.");
	gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
	gtk_widget_set_halign(note, GTK_ALIGN_START);
	gtk_widget_set_margin_top(note, 4);
	gtk_box_pack_start(GTK_BOX(content), note, FALSE, FALSE, 0);
	gtk_widget_show_all(dialog);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		app->include_usb = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(include_usb));
		app->verify_after_write = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(verify));
		app->eject_after_write = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(power_off));
		g_signal_handlers_block_by_func(app->list_usb_hdd,
			G_CALLBACK(on_list_usb_toggled), app);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->list_usb_hdd), app->include_usb);
		g_signal_handlers_unblock_by_func(app->list_usb_hdd,
			G_CALLBACK(on_list_usb_toggled), app);
		refresh_devices(app);
		save_preferences(app);
	}
	gtk_widget_destroy(dialog);
}

static void on_log(GtkToolButton *button, gpointer user_data)
{
	RufusApp *app = user_data;
	GtkWidget *dialog;
	(void)button;
	dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
		GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "Rufus operation log");
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
		app->log_messages->str);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}

static void hash_job_free(HashJob *job)
{
	if (job == NULL)
		return;
	g_free(job->image_path);
	g_free(job);
}

static void hash_worker(GTask *task, gpointer source_object, gpointer task_data,
		GCancellable *cancellable)
{
	HashJob *job = task_data;
	GChecksumType types[] = { G_CHECKSUM_MD5, G_CHECKSUM_SHA1,
		G_CHECKSUM_SHA256, G_CHECKSUM_SHA512 };
	const gchar *names[] = { "MD5", "SHA-1", "SHA-256", "SHA-512" };
	GChecksum *checksums[G_N_ELEMENTS(types)];
	gchar **results;
	FILE *file;
	guchar buffer[1024 * 1024];
	size_t bytes;
	GError *error = NULL;
	(void)source_object;
	(void)cancellable;

	file = fopen(job->image_path, "rb");
	if (file == NULL) {
		g_set_error(&error, G_FILE_ERROR, g_file_error_from_errno(errno),
			"Could not open image: %s", g_strerror(errno));
		g_task_return_error(task, error);
		return;
	}
	for (guint i = 0; i < G_N_ELEMENTS(types); i++)
		checksums[i] = g_checksum_new(types[i]);
	while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
		for (guint i = 0; i < G_N_ELEMENTS(types); i++)
			g_checksum_update(checksums[i], buffer, (gssize)bytes);
	}
	if (ferror(file)) {
		g_set_error(&error, G_FILE_ERROR, g_file_error_from_errno(errno),
			"Could not read image: %s", g_strerror(errno));
		fclose(file);
		for (guint i = 0; i < G_N_ELEMENTS(types); i++)
			g_checksum_free(checksums[i]);
		g_task_return_error(task, error);
		return;
	}
	fclose(file);
	results = g_new0(gchar *, G_N_ELEMENTS(types) + 1);
	for (guint i = 0; i < G_N_ELEMENTS(types); i++) {
		results[i] = g_strdup_printf("%s: %s", names[i], g_checksum_get_string(checksums[i]));
		g_checksum_free(checksums[i]);
	}
	g_task_return_pointer(task, results, (GDestroyNotify)g_strfreev);
}

static void hash_complete(GObject *source_object, GAsyncResult *result,
		gpointer user_data)
{
	RufusApp *app = user_data;
	gchar **hashes;
	GError *error = NULL;
	GString *summary;
	GtkWidget *dialog;
	(void)source_object;

	hashes = g_task_propagate_pointer(G_TASK(result), &error);
	set_busy(app, FALSE);
	if (hashes == NULL) {
		gtk_label_set_text(GTK_LABEL(app->status), "ERROR");
		dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
			GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Could not compute image checksums.");
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
			error != NULL ? error->message : "Unknown error");
	} else {
		summary = g_string_new(NULL);
		for (guint i = 0; hashes[i] != NULL; i++) {
			g_string_append_printf(summary, "%s\n", hashes[i]);
			append_log(app, hashes[i]);
		}
		gtk_label_set_text(GTK_LABEL(app->status), "READY");
		dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
			GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "Image checksums");
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", summary->str);
		g_string_free(summary, TRUE);
		g_strfreev(hashes);
	}
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	g_clear_error(&error);
	update_start_button(app);
}

static void on_hash_image(GtkButton *button, gpointer user_data)
{
	RufusApp *app = user_data;
	HashJob *job;
	GTask *task;
	(void)button;
	if (app->image_path == NULL || app->busy)
		return;
	job = g_new0(HashJob, 1);
	job->image_path = g_strdup(app->image_path);
	set_busy(app, TRUE);
	gtk_label_set_text(GTK_LABEL(app->status), "COMPUTING CHECKSUMS…");
	task = g_task_new(NULL, NULL, hash_complete, app);
	g_task_set_task_data(task, job, (GDestroyNotify)hash_job_free);
	g_task_run_in_thread(task, hash_worker);
	g_object_unref(task);
}

static void on_list_usb_toggled(GtkToggleButton *button, gpointer user_data)
{
	RufusApp *app = user_data;
	app->include_usb = gtk_toggle_button_get_active(button);
	save_preferences(app);
	refresh_devices(app);
}

static void on_advanced_device_toggled(GObject *object, GParamSpec *property,
		gpointer user_data)
{
	RufusApp *app = user_data;
	gboolean expanded = gtk_expander_get_expanded(GTK_EXPANDER(object));
	gint width, height;
	(void)property;
	gtk_expander_set_label(GTK_EXPANDER(object), expanded ?
		"Hide advanced drive properties" : "Show advanced drive properties");
	gtk_window_get_size(GTK_WINDOW(app->window), &width, &height);
	gtk_window_resize(GTK_WINDOW(app->window), width, height + (expanded ? 64 : -64));
}

static void on_advanced_format_toggled(GObject *object, GParamSpec *property,
		gpointer user_data)
{
	RufusApp *app = user_data;
	gboolean expanded = gtk_expander_get_expanded(GTK_EXPANDER(object));
	gint width, height;
	(void)property;
	gtk_expander_set_label(GTK_EXPANDER(object), expanded ?
		"Hide advanced format options" : "Show advanced format options");
	gtk_window_get_size(GTK_WINDOW(app->window), &width, &height);
	gtk_window_resize(GTK_WINDOW(app->window), width, height + (expanded ? 64 : -64));
}

static void update_start_button(RufusApp *app)
{
	const gchar *device_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->device));
	gtk_widget_set_sensitive(app->start_button,
		!app->busy && device_id != NULL && app->image_path != NULL);
	gtk_widget_set_sensitive(app->save_button, !app->busy && device_id != NULL);
	gtk_widget_set_sensitive(app->hash_button, !app->busy && app->image_path != NULL);
}

static void on_select_image(GtkButton *button, gpointer user_data)
{
	RufusApp *app = user_data;
	GtkWidget *dialog;
	GtkFileFilter *filter;
	gchar *path;
	struct stat info;
	(void)button;

	dialog = gtk_file_chooser_dialog_new("Select ISO or disk image",
		GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
	filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, "Disk images (*.iso, *.img, *.raw)");
	gtk_file_filter_add_pattern(filter, "*.iso");
	gtk_file_filter_add_pattern(filter, "*.ISO");
	gtk_file_filter_add_pattern(filter, "*.img");
	gtk_file_filter_add_pattern(filter, "*.IMG");
	gtk_file_filter_add_pattern(filter, "*.raw");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
		gtk_widget_destroy(dialog);
		return;
	}
	path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
	gtk_widget_destroy(dialog);
	if (path == NULL)
		return;
	if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
		GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
			GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
			"The selected image is not a readable, non-empty file.");
		gtk_dialog_run(GTK_DIALOG(error_dialog));
		gtk_widget_destroy(error_dialog);
		g_free(path);
		return;
	}
	g_free(app->image_path);
	app->image_path = path;
	{
		gchar *entry = g_strdup_printf("Selected image: %s", path);
		append_log(app, entry);
		g_free(entry);
	}
	gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->boot_selection));
	{
		gchar *basename = g_path_get_basename(path);
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->boot_selection), path, basename);
		g_free(basename);
	}
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->boot_selection), path);
	gtk_widget_show(app->hash_button);
	gtk_label_set_text(GTK_LABEL(app->status), "READY");
	update_start_button(app);
}

static void collect_mounts(JsonArray *nodes, GPtrArray *mounts)
{
	for (guint i = 0; i < json_array_get_length(nodes); i++) {
		JsonNode *node = json_array_get_element(nodes, i);
		JsonObject *object;
		JsonNode *mount_node;
		JsonArray *children;
		const gchar *path;

		if (!JSON_NODE_HOLDS_OBJECT(node))
			continue;
		object = json_node_get_object(node);
		path = json_string(object, "path");
		mount_node = json_object_get_member(object, "mountpoints");
		if (mount_node != NULL && JSON_NODE_HOLDS_ARRAY(mount_node)) {
			JsonArray *points = json_node_get_array(mount_node);
			for (guint j = 0; j < json_array_get_length(points); j++) {
				JsonNode *point = json_array_get_element(points, j);
				const gchar *mountpoint;
				if (point == NULL || JSON_NODE_HOLDS_NULL(point) || !JSON_NODE_HOLDS_VALUE(point))
					continue;
				mountpoint = json_node_get_string(point);
				if (mountpoint != NULL && *mountpoint && *path)
					g_ptr_array_add(mounts, g_strdup(path));
			}
		}
		children = json_object_get_array_member(object, "children");
		if (children != NULL)
			collect_mounts(children, mounts);
	}
}

static gboolean write_image_worker(WriteJob *job, GError **error)
{
	JsonParser *parser = NULL;
	JsonArray *devices;
	JsonObject *target = NULL;
	GHashTable *protected_disks = NULL;
	GPtrArray *mounts = NULL;
	gchar *output = NULL, *errors = NULL;
	gchar *dd_path = NULL;
	gchar *pkexec_path = NULL;
	gchar *cmp_path = NULL;
	gboolean success = FALSE;

	devices = load_block_tree(&parser, error);
	if (devices == NULL)
		goto cleanup;
	protected_disks = root_backing_disks(error);
	if (protected_disks == NULL)
		goto cleanup;
	for (guint i = 0; i < json_array_get_length(devices); i++) {
		JsonNode *node = json_array_get_element(devices, i);
		JsonObject *object;
		if (!JSON_NODE_HOLDS_OBJECT(node))
			continue;
		object = json_node_get_object(node);
		if (g_strcmp0(json_string(object, "path"), job->device_path) == 0 &&
			g_strcmp0(json_string(object, "type"), "disk") == 0) {
			target = object;
			break;
		}
	}
	if (target == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"The selected USB drive is no longer connected.");
		goto cleanup;
	}
	if (g_hash_table_contains(protected_disks, job->device_path)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			"The selected drive contains the running Linux system and is protected.");
		goto cleanup;
	}
	if (job->image_size > json_uint64(target, "size")) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
			"The image is larger than the selected USB drive.");
		goto cleanup;
	}

	mounts = g_ptr_array_new_with_free_func(g_free);
	{
		JsonArray *one_target = json_array_new();
		JsonNode *target_node = json_node_new(JSON_NODE_OBJECT);
		json_node_set_object(target_node, target);
		json_array_add_element(one_target, target_node);
		collect_mounts(one_target, mounts);
		json_array_unref(one_target);
	}
	for (guint i = 0; i < mounts->len; i++) {
		const gchar *partition = g_ptr_array_index(mounts, i);
		const gchar *unmount_argv[] = {
			"udisksctl", "unmount", "--block-device", partition, NULL
		};
		if (!spawn_capture(unmount_argv, &output, &errors, error)) {
			if (error != NULL && *error != NULL) {
				gchar *detail = g_strdup((*error)->message);
				g_clear_error(error);
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
					"Could not unmount %s: %s", partition, detail);
				g_free(detail);
			}
			goto cleanup;
		}
		g_clear_pointer(&output, g_free);
		g_clear_pointer(&errors, g_free);
	}

	dd_path = g_find_program_in_path("dd");
	pkexec_path = g_find_program_in_path("pkexec");
	if (dd_path == NULL || pkexec_path == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"This Linux system needs dd and pkexec to write a USB image.");
		goto cleanup;
	}
	{
		gchar *if_arg = g_strdup_printf("if=%s", job->image_path);
		gchar *of_arg = g_strdup_printf("of=%s", job->device_path);
		const gchar *write_argv[] = {
			pkexec_path, dd_path, if_arg, of_arg, "bs=4M", "status=progress",
			"conv=fsync", NULL
		};
		if (!spawn_capture(write_argv, &output, &errors, error)) {
			if (error != NULL && *error != NULL) {
				gchar *detail = (errors != NULL && *errors != '\0') ?
					g_strdup(errors) : g_strdup((*error)->message);
				g_clear_error(error);
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
					"The image write did not finish: %s", detail);
				g_free(detail);
			}
			g_free(if_arg);
			g_free(of_arg);
			goto cleanup;
		}
		g_free(if_arg);
		g_free(of_arg);
	}
	job->write_completed = TRUE;
	g_clear_pointer(&output, g_free);
	g_clear_pointer(&errors, g_free);
	if (job->verify_after_write) {
		GError *verify_error = NULL;
		gchar *byte_limit = g_strdup_printf("--bytes=%" G_GUINT64_FORMAT,
			job->image_size);
		cmp_path = g_find_program_in_path("cmp");
		if (cmp_path == NULL) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
				"The image was written, but verification needs the cmp utility.");
			g_free(byte_limit);
			goto cleanup;
		}
		{
			const gchar *verify_argv[] = {
				pkexec_path, cmp_path, byte_limit, job->image_path,
				job->device_path, NULL
			};
			if (!spawn_capture(verify_argv, &output, &errors, &verify_error)) {
				const gchar *detail = errors != NULL && *errors != '\0' ?
					errors : (verify_error != NULL ? verify_error->message : "Images differ");
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
					"The image was written, but verification failed: %s", detail);
				g_clear_error(&verify_error);
				g_free(byte_limit);
				goto cleanup;
			}
		}
		g_free(byte_limit);
		g_clear_error(&verify_error);
		job->verified = TRUE;
		g_clear_pointer(&output, g_free);
		g_clear_pointer(&errors, g_free);
	}
	if (job->eject_after_write) {
		GError *eject_error = NULL;
		const gchar *eject_argv[] = {
			"udisksctl", "power-off", "--block-device", job->device_path, NULL
		};
		job->eject_attempted = TRUE;
		if (spawn_capture(eject_argv, &output, &errors, &eject_error)) {
			job->eject_succeeded = TRUE;
		} else {
			job->eject_error = g_strdup(eject_error != NULL ?
				eject_error->message : "The drive could not be powered off.");
		}
		g_clear_error(&eject_error);
	}
	success = TRUE;

cleanup:
	g_free(output);
	g_free(errors);
	g_free(dd_path);
	g_free(pkexec_path);
	g_free(cmp_path);
	if (mounts != NULL)
		g_ptr_array_unref(mounts);
	if (protected_disks != NULL)
		g_hash_table_unref(protected_disks);
	if (parser != NULL)
		g_object_unref(parser);
	return success;
}

static void write_job_free(WriteJob *job)
{
	if (job == NULL)
		return;
	g_free(job->device_path);
	g_free(job->image_path);
	g_free(job->eject_error);
	g_free(job);
}

static void write_worker(GTask *task, gpointer source_object, gpointer task_data,
		GCancellable *cancellable)
{
	WriteJob *job = task_data;
	GError *error = NULL;
	(void)source_object;
	(void)cancellable;
	if (write_image_worker(job, &error))
		g_task_return_boolean(task, TRUE);
	else
		g_task_return_error(task, error);
}

static gboolean pulse_progress(gpointer user_data)
{
	RufusApp *app = user_data;
	if (!app->busy)
		return G_SOURCE_REMOVE;
	gtk_progress_bar_pulse(GTK_PROGRESS_BAR(app->progress));
	return G_SOURCE_CONTINUE;
}

static void on_bad_blocks_toggled(GtkToggleButton *button, gpointer user_data)
{
	RufusApp *app = user_data;
	gtk_widget_set_sensitive(app->bad_block_passes,
		gtk_toggle_button_get_active(button) && !app->busy);
}

static void set_busy(RufusApp *app, gboolean busy)
{
	GtkWidget *widgets[] = {
		app->device, app->save_button, app->boot_selection, app->hash_button,
		app->image_option,
		app->advanced_device_expander, app->advanced_format_expander,
		app->list_usb_hdd, app->select_button, app->close_button, NULL
	};
	app->busy = busy;
	for (guint i = 0; widgets[i] != NULL; i++)
		gtk_widget_set_sensitive(widgets[i], !busy);
	update_start_button(app);
	if (busy) {
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
		app->pulse_source = g_timeout_add(120, pulse_progress, app);
	} else if (app->pulse_source != 0) {
		g_source_remove(app->pulse_source);
		app->pulse_source = 0;
	}
}

static void write_complete(GObject *source_object, GAsyncResult *result,
		gpointer user_data)
{
	RufusApp *app = user_data;
	WriteJob *job = g_task_get_task_data(G_TASK(result));
	GError *error = NULL;
	gboolean success;
	GtkWidget *dialog;
	(void)source_object;

	success = g_task_propagate_boolean(G_TASK(result), &error);
	set_busy(app, FALSE);
	if (success) {
		append_log(app, "Image write completed successfully.");
		if (job->verified)
			append_log(app, "Image verification completed successfully.");
		if (job->eject_attempted)
			append_log(app, job->eject_succeeded ?
				"USB drive powered off safely." : job->eject_error);
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 1.0);
		gtk_label_set_text(GTK_LABEL(app->status), "DONE");
		dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
			GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "Image written successfully.");
		if (job->eject_succeeded) {
			gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
				"%sThe USB drive has been safely powered off.",
				job->verified ? "The written image was verified.\n\n" : "");
		} else if (job->eject_attempted) {
			gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
				"%sThe image was written, but the drive could not be powered off: %s\n\nUse your desktop's eject option before unplugging it.",
				job->verified ? "The written image was verified.\n\n" : "",
				job->eject_error != NULL ? job->eject_error : "Unknown error");
		} else {
			gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
				"%sUse your desktop's eject option before unplugging the USB drive.",
				job->verified ? "The written image was verified.\n\n" : "");
		}
	} else {
		gchar *entry = g_strdup_printf("Image operation failed: %s",
			error != NULL ? error->message : "Unknown error");
		append_log(app, entry);
		g_free(entry);
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
		gtk_label_set_text(GTK_LABEL(app->status), job->write_completed ?
			"VERIFY ERROR" : "ERROR");
		dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
			GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, job->write_completed ?
			"The image was written, but could not be verified." :
			"Could not write the image.");
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
			error != NULL ? error->message : "Unknown error");
	}
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	g_clear_error(&error);
	refresh_devices(app);
}

static void on_start(GtkButton *button, gpointer user_data)
{
	RufusApp *app = user_data;
	const gchar *device_id;
	guint64 *device_size;
	struct stat image_info;
	gchar *display_name;
	GtkWidget *dialog;
	WriteJob *job;
	GTask *task;
	(void)button;

	device_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->device));
	if (device_id == NULL || app->image_path == NULL)
		return;
	device_size = g_hash_table_lookup(app->device_sizes, device_id);
	if (device_size == NULL || stat(app->image_path, &image_info) != 0 ||
		!S_ISREG(image_info.st_mode) || image_info.st_size <= 0) {
		gtk_label_set_text(GTK_LABEL(app->status), "Image or drive is unavailable");
		update_start_button(app);
		return;
	}
	if ((guint64)image_info.st_size > *device_size) {
		dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
			GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "The image is too large for this USB drive.");
		gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);
		return;
	}
	display_name = g_path_get_basename(app->image_path);
	dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
		GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
		"This will erase all data on %s.", device_id);
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
		"Image: %s\n\nCheck that this is the correct USB drive before continuing.",
		display_name);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_NO);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_YES) {
		gtk_widget_destroy(dialog);
		g_free(display_name);
		return;
	}
	gtk_widget_destroy(dialog);
	g_free(display_name);

	job = g_new0(WriteJob, 1);
	job->device_path = g_strdup(device_id);
	job->image_path = g_strdup(app->image_path);
	job->image_size = (guint64)image_info.st_size;
	job->verify_after_write = app->verify_after_write;
	job->eject_after_write = app->eject_after_write;
	{
		gchar *entry = g_strdup_printf("Writing %s to %s", app->image_path, device_id);
		append_log(app, entry);
		g_free(entry);
	}
	set_busy(app, TRUE);
	gtk_label_set_text(GTK_LABEL(app->status), "WRITING IMAGE…");
	task = g_task_new(NULL, NULL, write_complete, app);
	g_task_set_task_data(task, job, (GDestroyNotify)write_job_free);
	g_task_run_in_thread(task, write_worker);
	g_object_unref(task);
}

static void on_close(GtkButton *button, gpointer user_data)
{
	RufusApp *app = user_data;
	(void)button;
	if (app->busy)
		return;
	gtk_main_quit();
}

static gboolean on_delete_window(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	RufusApp *app = user_data;
	(void)widget;
	(void)event;
	if (app->busy)
		return TRUE;
	gtk_main_quit();
	return TRUE;
}

static GtkWidget *add_check(GtkWidget *box, const gchar *text, guint id,
		gboolean active)
{
	GtkWidget *check = gtk_check_button_new_with_label(text);
	SET_ID(check, id);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), active);
	gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 1);
	return check;
}

static void add_spaced(GtkWidget *box, GtkWidget *widget, guint top, guint bottom)
{
	gtk_widget_set_margin_top(widget, top);
	gtk_widget_set_margin_bottom(widget, bottom);
	gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
}

static GtkToolItem *make_toolbar_button(const gchar *icon_name,
		const gchar *tooltip, guint id)
{
	GtkToolItem *item = gtk_tool_button_new(NULL, NULL);
	SET_ID(GTK_WIDGET(item), id);
	gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(item), icon_name);
	gtk_widget_set_tooltip_text(GTK_WIDGET(item), tooltip);
	return item;
}

static GtkWidget *build_window(RufusApp *app)
{
	static const gchar *const image_options[] = {
		"Standard Windows installation", "Windows To Go", NULL
	};
	static const gchar *const partition_options[] = { "MBR", "GPT", NULL };
	static const gchar *const target_options[] = {
		"BIOS (or UEFI-CSM)", "UEFI (non CSM)", NULL
	};
	static const gchar *const fs_options[] = { "FAT32", "NTFS", "exFAT", NULL };
	static const gchar *const cluster_options[] = {
		"Default", "512 bytes", "1024 bytes", "2048 bytes", "4096 bytes", NULL
	};	
	GtkWidget *outer, *row, *box, *labels, *combos, *status_row;
	GtkWidget *device_label, *boot_label, *image_label, *partition_label;
	GtkWidget *target_label, *volume_label, *filesystem_label, *cluster_label;
	GtkWidget *status_title, *button_row, *toolbar, *spacer;
	GtkToolItem *language_tool, *about_tool, *settings_tool, *log_tool, *separator;
	GError *icon_error = NULL;

	app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(app->window), "Rufus 4.16.2443");
	gtk_window_set_default_size(GTK_WINDOW(app->window), 478, 590);
	gtk_window_set_resizable(GTK_WINDOW(app->window), TRUE);
	gtk_container_set_border_width(GTK_CONTAINER(app->window), 12);
	gtk_window_set_position(GTK_WINDOW(app->window), GTK_WIN_POS_CENTER);
	gtk_window_set_icon_from_file(GTK_WINDOW(app->window), "res/rufus.ico", &icon_error);
	g_clear_error(&icon_error);
	SET_ID(app->window, IDD_DIALOG);
	g_signal_connect(app->window, "delete-event", G_CALLBACK(on_delete_window), app);

	outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_container_add(GTK_CONTAINER(app->window), outer);

	add_spaced(outer, make_heading("Drive Properties", IDS_DRIVE_PROPERTIES_TXT), 0, 1);
	device_label = make_label("Device", IDS_DEVICE_TXT);
	add_spaced(outer, device_label, 0, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	app->device = gtk_combo_box_text_new();
	SET_ID(app->device, IDC_DEVICE);
	gtk_widget_set_hexpand(app->device, TRUE);
	gtk_box_pack_start(GTK_BOX(row), app->device, TRUE, TRUE, 0);
	app->save_button = gtk_button_new_from_icon_name("document-save-symbolic", GTK_ICON_SIZE_MENU);
	SET_ID(app->save_button, IDC_SAVE);
	gtk_widget_set_size_request(app->save_button, 28, -1);
	gtk_widget_set_tooltip_text(app->save_button, "Save drive information");
	gtk_widget_set_sensitive(app->save_button, FALSE);
	gtk_box_pack_start(GTK_BOX(row), app->save_button, FALSE, FALSE, 0);
	add_spaced(outer, row, 0, 4);

	boot_label = make_label("Boot selection", IDS_BOOT_SELECTION_TXT);
	add_spaced(outer, boot_label, 0, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	app->boot_selection = gtk_combo_box_text_new();
	SET_ID(app->boot_selection, IDC_BOOT_SELECTION);
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->boot_selection),
		"Disk or ISO image (Please select)");
	gtk_combo_box_set_active(GTK_COMBO_BOX(app->boot_selection), 0);
	gtk_widget_set_hexpand(app->boot_selection, TRUE);
	gtk_box_pack_start(GTK_BOX(row), app->boot_selection, TRUE, TRUE, 0);
	app->hash_button = gtk_button_new_from_icon_name("security-high-symbolic", GTK_ICON_SIZE_MENU);
	SET_ID(app->hash_button, IDC_HASH);
	gtk_widget_set_size_request(app->hash_button, 28, -1);
	gtk_widget_set_tooltip_text(app->hash_button, "Compute image checksums");
	gtk_widget_set_sensitive(app->hash_button, FALSE);
	gtk_widget_set_no_show_all(app->hash_button, TRUE);
	gtk_widget_hide(app->hash_button);
	gtk_box_pack_start(GTK_BOX(row), app->hash_button, FALSE, FALSE, 0);
	app->select_button = gtk_button_new_with_label("SELECT");
	SET_ID(app->select_button, IDC_SELECT);
	gtk_widget_set_size_request(app->select_button, 90, -1);
	gtk_box_pack_start(GTK_BOX(row), app->select_button, FALSE, FALSE, 0);
	g_signal_connect(app->select_button, "clicked", G_CALLBACK(on_select_image), app);
	g_signal_connect(app->save_button, "clicked", G_CALLBACK(on_save_device), app);
	g_signal_connect(app->hash_button, "clicked", G_CALLBACK(on_hash_image), app);
	add_spaced(outer, row, 0, 4);

	image_label = make_label("Image Option", IDS_IMAGE_OPTION_TXT);
	add_spaced(outer, image_label, 0, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	app->image_option = make_combo(IDC_IMAGE_OPTION, image_options);
	gtk_widget_set_hexpand(app->image_option, TRUE);
	gtk_box_pack_start(GTK_BOX(row), app->image_option, TRUE, TRUE, 0);
	app->persistence_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	SET_ID(app->persistence_slider, IDC_PERSISTENCE_SLIDER);
	gtk_scale_set_draw_value(GTK_SCALE(app->persistence_slider), FALSE);
	gtk_widget_set_size_request(app->persistence_slider, 70, -1);
	gtk_box_pack_start(GTK_BOX(row), app->persistence_slider, FALSE, FALSE, 0);
	app->persistence_size = gtk_entry_new();
	SET_ID(app->persistence_size, IDC_PERSISTENCE_SIZE);
	gtk_entry_set_width_chars(GTK_ENTRY(app->persistence_size), 4);
	gtk_box_pack_start(GTK_BOX(row), app->persistence_size, FALSE, FALSE, 0);
	app->persistence_units = gtk_combo_box_text_new();
	SET_ID(app->persistence_units, IDC_PERSISTENCE_UNITS);
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->persistence_units), "MB");
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->persistence_units), "GB");
	gtk_combo_box_set_active(GTK_COMBO_BOX(app->persistence_units), 0);
	gtk_box_pack_start(GTK_BOX(row), app->persistence_units, FALSE, FALSE, 0);
	add_spaced(outer, row, 0, 5);

	labels = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	partition_label = make_label("Partition scheme", IDS_PARTITION_TYPE_TXT);
	target_label = make_label("Target system", IDS_TARGET_SYSTEM_TXT);
	gtk_widget_set_hexpand(partition_label, TRUE);
	gtk_widget_set_hexpand(target_label, TRUE);
	gtk_box_pack_start(GTK_BOX(labels), partition_label, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(labels), target_label, TRUE, TRUE, 0);
	add_spaced(outer, labels, 0, 0);
	combos = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	app->partition_scheme = make_combo(IDC_PARTITION_TYPE, partition_options);
	app->target_system = make_combo(IDC_TARGET_SYSTEM, target_options);
	gtk_widget_set_hexpand(app->partition_scheme, TRUE);
	gtk_widget_set_hexpand(app->target_system, TRUE);
	gtk_box_pack_start(GTK_BOX(combos), app->partition_scheme, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(combos), app->target_system, TRUE, TRUE, 0);
	add_spaced(outer, combos, 0, 4);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	app->list_usb_hdd = add_check(box, "List USB Hard Drives", IDC_LIST_USB_HDD, app->include_usb);
	app->old_bios_fixes = add_check(box,
		"Add fixes for old BIOSes (extra partition, align, etc.)",
		IDC_OLD_BIOS_FIXES, FALSE);
	app->uefi_validation = add_check(box,
		"Enable runtime UEFI media validation", IDC_UEFI_MEDIA_VALIDATION, FALSE);
	app->advanced_device_expander = gtk_expander_new("Show advanced drive properties");
	SET_ID(app->advanced_device_expander, IDC_ADVANCED_DRIVE_PROPERTIES);
	gtk_container_add(GTK_CONTAINER(app->advanced_device_expander), box);
	add_spaced(outer, app->advanced_device_expander, 0, 6);
	g_signal_connect(app->advanced_device_expander, "notify::expanded",
		G_CALLBACK(on_advanced_device_toggled), app);
	g_signal_connect(app->list_usb_hdd, "toggled", G_CALLBACK(on_list_usb_toggled), app);

	add_spaced(outer, make_heading("Format Options", IDS_FORMAT_OPTIONS_TXT), 0, 1);
	volume_label = make_label("Volume label", IDS_LABEL_TXT);
	add_spaced(outer, volume_label, 0, 0);
	app->volume_label = gtk_entry_new();
	SET_ID(app->volume_label, IDC_LABEL);
	add_spaced(outer, app->volume_label, 0, 4);

	labels = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	filesystem_label = make_label("File system", IDS_FILE_SYSTEM_TXT);
	cluster_label = make_label("Cluster size", IDS_CLUSTER_SIZE_TXT);
	gtk_widget_set_hexpand(filesystem_label, TRUE);
	gtk_widget_set_hexpand(cluster_label, TRUE);
	gtk_box_pack_start(GTK_BOX(labels), filesystem_label, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(labels), cluster_label, TRUE, TRUE, 0);
	add_spaced(outer, labels, 0, 0);
	combos = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	app->file_system = make_combo(IDC_FILE_SYSTEM, fs_options);
	app->cluster_size = make_combo(IDC_CLUSTER_SIZE, cluster_options);
	gtk_widget_set_hexpand(app->file_system, TRUE);
	gtk_widget_set_hexpand(app->cluster_size, TRUE);
	gtk_box_pack_start(GTK_BOX(combos), app->file_system, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(combos), app->cluster_size, TRUE, TRUE, 0);
	add_spaced(outer, combos, 0, 4);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	app->quick_format = add_check(box, "Quick format", IDC_QUICK_FORMAT, TRUE);
	app->extended_label = add_check(box,
		"Create extended label and icon files", IDC_EXTENDED_LABEL, TRUE);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	app->bad_blocks = gtk_check_button_new_with_label("Check device for bad blocks");
	SET_ID(app->bad_blocks, IDC_BAD_BLOCKS);
	app->bad_block_passes = gtk_combo_box_text_new();
	SET_ID(app->bad_block_passes, IDC_NB_PASSES);
	for (guint i = 1; i <= 4; i++) {
		gchar *passes = g_strdup_printf("%u pass%s", i, i == 1 ? "" : "es");
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->bad_block_passes), passes);
		g_free(passes);
	}
	gtk_combo_box_set_active(GTK_COMBO_BOX(app->bad_block_passes), 0);
	gtk_widget_set_hexpand(app->bad_block_passes, TRUE);
	gtk_box_pack_start(GTK_BOX(row), app->bad_blocks, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), app->bad_block_passes, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 1);
	gtk_widget_set_sensitive(app->bad_block_passes, FALSE);
	g_signal_connect(app->bad_blocks, "toggled", G_CALLBACK(on_bad_blocks_toggled), app);
	app->advanced_format_expander = gtk_expander_new("Show advanced format options");
	SET_ID(app->advanced_format_expander, IDC_ADVANCED_FORMAT_OPTIONS);
	gtk_container_add(GTK_CONTAINER(app->advanced_format_expander), box);
	add_spaced(outer, app->advanced_format_expander, 0, 5);
	g_signal_connect(app->advanced_format_expander, "notify::expanded",
		G_CALLBACK(on_advanced_format_toggled), app);

	status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	status_title = make_label("Status", IDS_STATUS_TXT);
	app->status = make_label("READY", IDC_STATUS);
	gtk_widget_set_hexpand(app->status, TRUE);
	gtk_box_pack_start(GTK_BOX(status_row), status_title, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(status_row), app->status, TRUE, TRUE, 0);
	add_spaced(outer, status_row, 0, 2);
	app->progress = gtk_progress_bar_new();
	SET_ID(app->progress, IDC_PROGRESS);
	gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progress), FALSE);
	add_spaced(outer, app->progress, 0, 5);

	button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	toolbar = gtk_toolbar_new();
	gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_ICONS);
	gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar), GTK_ICON_SIZE_SMALL_TOOLBAR);
	language_tool = make_toolbar_button("preferences-desktop-locale",
		"Language selection is not yet available", IDC_LANG);
	gtk_widget_set_sensitive(GTK_WIDGET(language_tool), FALSE);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), language_tool, -1);
	separator = gtk_separator_tool_item_new();
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), separator, -1);
	about_tool = make_toolbar_button("help-about", "About Rufus", IDC_ABOUT);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), about_tool, -1);
	separator = gtk_separator_tool_item_new();
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), separator, -1);
	settings_tool = make_toolbar_button("preferences-system", "Settings", IDC_SETTINGS);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), settings_tool, -1);
	separator = gtk_separator_tool_item_new();
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), separator, -1);
	log_tool = make_toolbar_button("view-list-details", "View log", IDC_LOG);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar), log_tool, -1);
	gtk_box_pack_start(GTK_BOX(button_row), toolbar, FALSE, FALSE, 0);
	spacer = gtk_label_new("");
	gtk_widget_set_hexpand(spacer, TRUE);
	gtk_box_pack_start(GTK_BOX(button_row), spacer, TRUE, TRUE, 0);
	app->start_button = gtk_button_new_with_label("START");
	SET_ID(app->start_button, IDC_START);
	gtk_widget_set_size_request(app->start_button, 92, -1);
	app->close_button = gtk_button_new_with_label("CLOSE");
	SET_ID(app->close_button, 2); /* IDCANCEL in the original dialog resource. */
	gtk_widget_set_size_request(app->close_button, 92, -1);
	gtk_box_pack_start(GTK_BOX(button_row), app->start_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(button_row), app->close_button, FALSE, FALSE, 0);
	add_spaced(outer, button_row, 5, 0);
	g_signal_connect(about_tool, "clicked", G_CALLBACK(on_about), app);
	g_signal_connect(settings_tool, "clicked", G_CALLBACK(on_settings), app);
	g_signal_connect(log_tool, "clicked", G_CALLBACK(on_log), app);

	/* Raw image mode is implemented on Linux; formatting controls stay visible
	 * in their original places but are unavailable until their backends are ported. */
	gtk_widget_set_sensitive(app->image_option, FALSE);
	gtk_widget_set_tooltip_text(app->image_option,
		"The Linux build currently writes selected images directly in DD mode.");
	gtk_widget_set_sensitive(app->persistence_slider, FALSE);
	gtk_widget_set_sensitive(app->persistence_size, FALSE);
	gtk_widget_set_sensitive(app->persistence_units, FALSE);
	gtk_widget_set_sensitive(app->partition_scheme, FALSE);
	gtk_widget_set_sensitive(app->target_system, FALSE);
	gtk_widget_set_sensitive(app->old_bios_fixes, FALSE);
	gtk_widget_set_sensitive(app->uefi_validation, FALSE);
	gtk_widget_set_sensitive(app->volume_label, FALSE);
	gtk_widget_set_sensitive(app->file_system, FALSE);
	gtk_widget_set_sensitive(app->cluster_size, FALSE);
	gtk_widget_set_sensitive(app->quick_format, FALSE);
	gtk_widget_set_sensitive(app->extended_label, FALSE);
	gtk_widget_set_sensitive(app->bad_blocks, FALSE);

	g_signal_connect(app->device, "changed", G_CALLBACK(on_device_changed), app);
	g_signal_connect(app->start_button, "clicked", G_CALLBACK(on_start), app);
	g_signal_connect(app->close_button, "clicked", G_CALLBACK(on_close), app);
	return app->window;
}

static gboolean periodic_refresh(gpointer user_data)
{
	refresh_devices((RufusApp *)user_data);
	return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
	RufusApp app = { 0 };
	(void)argv;
	gtk_init(&argc, &argv);
	app.log_messages = g_string_new("Rufus 4.16.2443 Linux log\n");
	app.device_sizes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	load_preferences(&app);
	build_window(&app);
	gtk_widget_show_all(app.window);
	refresh_devices(&app);
	g_timeout_add_seconds(3, periodic_refresh, &app);
	gtk_main();
	if (app.pulse_source != 0)
		g_source_remove(app.pulse_source);
	g_free(app.image_path);
	g_string_free(app.log_messages, TRUE);
	g_hash_table_unref(app.device_sizes);
	return 0;
}
