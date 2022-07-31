// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Thomas Bogendoerfer (tsbogend@alpha.franken.de
 */

#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <libgupnp/gupnp.h>
#include <gmodule.h>

#define XMLFILE		"canon-ssdp.xml"

struct camera {
	struct camera *next;
	gboolean busy;
	gchar *usn;
	gchar *path;
	gchar *cmd;
};

struct camera *cameras;

static gchar *iface;
static gchar *conffile = "canon-ssdp.conf";

typedef struct _GSSDPDiscover {
	GMainLoop *main_loop;
	GSSDPClient *client;
	GSSDPResourceBrowser *browser;
} GSSDPDiscover;

GOptionEntry entries[] =
{
	{ "interface", 'i', 0, G_OPTION_ARG_STRING, &iface,
	  "Network interface to listen on", NULL },
	{ "config", 'c', 0, G_OPTION_ARG_STRING, &conffile,
	  "Network interface to listen on", NULL },
	{ NULL , 0, 0, 0, NULL, NULL, NULL}
};

static const char *devxml_part1 =
{
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
	"<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
        "<specVersion>\n"
	"    <major>1</major>\n"
	"    <minor>0</minor>\n"
	"</specVersion>\n"
	"<device>\n"
	"    <deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>\n"
	"    <friendlyName>"
};

static const char *devxml_part2 =
{
	"</friendlyName>\n"
	"    <manufacturer>GPL</manufacturer>\n"
	"    <modelName>Canon PTP Endpoint</modelName>\n"
	"    <UDN>uuid:"
};

static const char *devxml_part3 =
{
	"</UDN>\n"
	"</device>\n"
	"</root>\n"
};

/*
 * haven't found a way to feed xml file via buffer into gupnp, so we
 * ust create a xml device file to put hostname into friendlyName it
 */
static int create_dev_xml(void)
{
	char hostname[64];
	gchar *uuid;
	int fd;

	if (gethostname(hostname, sizeof(hostname - 1)) < 0)
		return -1;

	hostname[sizeof(hostname) - 1] = 0;

	fd = open(XMLFILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;

	uuid = g_uuid_string_random();

	write(fd, devxml_part1, strlen(devxml_part1));
	write(fd, hostname, strlen(hostname));
	write(fd, devxml_part2, strlen(devxml_part2));
	write(fd, uuid, strlen(uuid));
	write(fd, devxml_part3, strlen(devxml_part3));
	close(fd);
	g_free(uuid);

	return 0;
}

static void command_done(GPid pid, gint wait_status, gpointer data)
{
	struct camera *cam = data;

	cam->busy = FALSE;
	g_spawn_close_pid(pid);
}

static void run_camera_cmd(struct camera *cam, GList *locations)
{
	gchar *hostname = NULL;
	GError *error = NULL;
	gchar *logname;
	gchar *newarg;
	gchar **argv;
	gchar **split;
	gint logfd;
	gint argc;
	GPid pid;
	int i;

	if (cam->busy)
		return;

	g_shell_parse_argv(cam->cmd, &argc, &argv, &error);
	if (error) {
		g_printerr("Error parsing command: %s\n",
			    error->message);
		g_error_free(error);
		return;
	}

	/* get hostname out of location string */
	if (!g_uri_split_network(locations->data, G_URI_FLAGS_NONE, NULL,
				 &hostname, NULL, NULL))
		return;

	/* check, if we need to replace $HOSTNAME in argv */
	for (i = 0; i < argc; i++) {
		if (strstr(argv[i], "$HOSTNAME")) {
			split = g_strsplit(argv[i], "$HOSTNAME", 2);
			newarg = g_strconcat(split[0], hostname, split[1], NULL);
			g_strfreev(split);
			g_free(argv[i]);
			argv[i] = newarg;
		}
	}

	logname = g_strdup_printf("%s/logfile", cam->path);
	if (!logname)
		return;
	logfd = open(logname, O_RDWR | O_CREAT | O_APPEND, 0644);
	g_free(logname);
	if (logfd < 0)
		return;

	cam->busy = TRUE;

	g_spawn_async_with_fds(cam->path, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
			       NULL, NULL, &pid, -1, dup(logfd), dup(logfd),
			       &error);

	if (error) {
		g_printerr("Error spawning command: %s\n",
			    error->message);
		g_error_free(error);
		cam->busy = FALSE;
		return;
	}
	g_child_watch_add(pid, command_done, cam);
}

static void create_camera(gchar *usn, gchar *path, gchar *cmd)
{
	struct camera *cam;

	cam = malloc(sizeof(*cam));
	if (!cam)
		return;

	cam->usn = g_strdup(usn);
	cam->path = g_strdup(path);
	cam->cmd = g_strdup(cmd);
	cam->next = cameras;
	cam->busy = FALSE;
	cameras = cam;
}

static void load_config(void)
{
	GError *error = NULL;
	GKeyFile *keyfile;
	gchar **groups;
	gchar **keys;
	gchar *path, *cmd;
	int i,j;

	keyfile = g_key_file_new();
	if (!keyfile)
		return;

	g_key_file_load_from_file(keyfile, conffile, G_KEY_FILE_NONE, &error);
	if (error != NULL) {
		g_printerr("Error loading config file: %s\n",
			    error->message);
		g_error_free(error);
		return;
	}

	groups = g_key_file_get_groups(keyfile, NULL);
	for (i = 0; groups[i]; i++) {
		path = NULL;
		cmd = NULL;
		keys = g_key_file_get_keys(keyfile, groups[i], NULL, NULL);
		for (j = 0; keys[j]; j++) {
			if (g_strcmp0(keys[j], "path") == 0)
				path = g_key_file_get_string(keyfile,
							     groups[i],
							     keys[j], NULL);
			else if (g_strcmp0(keys[j], "command") == 0)
				cmd = g_key_file_get_string(keyfile,
							     groups[i],
							     keys[j], NULL);
		}
		if (path && cmd)
			create_camera(groups[i], path, cmd);

		g_strfreev(keys);
	}
	g_strfreev(groups);
	g_key_file_free(keyfile);
}

static void on_resource_available(GSSDPResourceBrowser *browser,
				  const char *usn, GList *locations)
{
	struct camera *cam;

	for (cam = cameras; cam; cam = cam->next)
		if (g_strcmp0(cam->usn, usn) == 0)
			break;

	if (cam)
		run_camera_cmd(cam, locations);
}

int main(int argc, char **argv)
{
	GOptionContext *optionContext;
	GSSDPDiscover discover;
	GUPnPContext *context;
	GUPnPRootDevice *dev;
	GError *error = NULL;

	optionContext = g_option_context_new(NULL);
	g_option_context_add_main_entries(optionContext, entries, NULL);
	if (!g_option_context_parse(optionContext, &argc, &argv, &error)) {
		g_print("option parsing failed: %s\n", error->message);
		return 1;
	}
	g_option_context_free (optionContext);

	load_config();

	context = gupnp_context_new(iface, 0, &error);
	if (error) {
		g_printerr("Error creating the GUPnP context: %s\n",
			    error->message);
		g_error_free(error);
		return 1;
	}

	if (!g_file_test(XMLFILE, G_FILE_TEST_EXISTS)) {
		if (create_dev_xml() < 0) {
			g_printerr("Error creating the device xml\n");
			return 1;
		}
	}

	dev = gupnp_root_device_new(context, XMLFILE, ".", &error);
	if (error != NULL) {
		g_printerr("Error creating the GUPnP root device: %s\n",
			    error->message);
		g_error_free(error);
		return 1;
	}
	gupnp_root_device_set_available(dev, TRUE);

	/*
	 * without Windows in the server string my Powershot doesn't
	 * accept the upnp device
	 */
	gssdp_client_set_server_id (GSSDP_CLIENT(context),
		"Microsoft-Windows-NT/5.1 UPnP/1.0 UPnP-Device-Host/1.0");

	discover.client = gssdp_client_new(iface, &error);
	if (error != NULL) {
		g_printerr("Failed to create GSSDP client: %s", error->message);
                g_error_free(error);
		return 1;
	}
	discover.browser = gssdp_resource_browser_new(discover.client,
						      "ssdp:all");

	g_signal_connect(discover.browser, "resource-available",
			 G_CALLBACK(on_resource_available), &discover);

	gssdp_resource_browser_set_active (discover.browser, TRUE);
	
	discover.main_loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(discover.main_loop);

	g_main_loop_unref(discover.main_loop);
	g_object_unref(discover.browser);
	g_object_unref(discover.client);
	g_object_unref(dev);
	g_object_unref(context);

	return 0;
}
