// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2016, 2017 Mentor Graphics Development (Deutschland) GmbH
 * Copyright (c) 2017, 2018 TOYOTA MOTOR CORPORATION
 * Copyright (c) 2022 Konsulko Group
 * Copyright (c) 2023 Collabora, Ltd.
 */

#include <QGuiApplication>
#include <QCommandLineParser>
#include <QtCore/QUrlQuery>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlComponent>
#include <QtQml/qqml.h>
#include <QQuickWindow>
#include <QTimer>
#include <QScreen>

#include <weather.h>
#include <bluetooth.h>

#include "applicationlauncher.h"
#include "statusbarmodel.h"
#include "mastervolume.h"
#include "homescreenhandler.h"
#include "hmi-debug.h"

// meson will define these
#include QT_QPA_HEADER
#include <wayland-client.h>

#include "agl-shell-client-protocol.h"

#include <thread>
#include "AglShellGrpcClient.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

QScreen *
find_screen(const char *screen_name)
{
	QList<QScreen *> screens = qApp->screens();
	QScreen *found = nullptr;
	QString qstr_name = QString::fromUtf8(screen_name, -1);

	for (QScreen *xscreen : screens) {
		if (qstr_name == xscreen->name()) {
			found = xscreen;
			break;
		}
	}

	return found;
}

struct shell_data {
	struct agl_shell *shell;
	HomescreenHandler *homescreenHandler;
	bool wait_for_bound;
	bool bound_ok;
	int ver;
};

static void
agl_shell_bound_ok(void *data, struct agl_shell *agl_shell)
{
	struct shell_data *shell_data = static_cast<struct shell_data *>(data);
	shell_data->wait_for_bound = false;

	shell_data->bound_ok = true;
}

static void
agl_shell_bound_fail(void *data, struct agl_shell *agl_shell)
{
	struct shell_data *shell_data = static_cast<struct shell_data *>(data);
	shell_data->wait_for_bound = false;

	shell_data->bound_ok = false;
}

static void
agl_shell_app_state(void *data, struct agl_shell *agl_shell,
		const char *app_id, uint32_t state)
{
	/* unused */
}

static void
agl_shell_app_on_output(void *data, struct agl_shell *agl_shell,
		const char *app_id, const char *output_name)
{
	struct shell_data *shell_data = static_cast<struct shell_data *>(data);
	HomescreenHandler *homescreenHandler = shell_data->homescreenHandler;

	if (!homescreenHandler)
		return;

	// a couple of use-cases, if there is no app_id in the app_list then it
	// means this is a request to map the application, from the start to a
	// different output that the default one. We'd get an
	// AGL_SHELL_APP_STATE_STARTED which will handle activation.
	//
	// if there's an app_id then it means we might have gotten an event to
	// move the application to another output; so we'd need to process it
	// by explicitly calling processAppStatusEvent() which would ultimately
	// activate the application on other output. We'd have to pick-up the
	// last activated window and activate the default output.
	//
	// finally if the outputs are identical probably that's an user-error -
	// but the compositor won't activate it again, so we don't handle that.
	std::pair new_pending_app = std::pair(QString(app_id),
					      QString(output_name));
	homescreenHandler->pending_app_list.push_back(new_pending_app);

	if (homescreenHandler->apps_stack.contains(QString(app_id))) {
		qDebug() << "Got event to move " << app_id <<
			" to another output " << output_name;

		homescreenHandler->processAppStatusEvent(app_id, "started");
	}
}


#ifdef AGL_SHELL_BOUND_OK_SINCE_VERSION
static const struct agl_shell_listener shell_listener = {
	agl_shell_bound_ok,
	agl_shell_bound_fail,
	agl_shell_app_state,
	agl_shell_app_on_output,
};
#endif

static void
global_add(void *data, struct wl_registry *reg, uint32_t name,
	   const char *interface, uint32_t ver)
{
	struct shell_data *shell_data = static_cast<struct shell_data *>(data);

	if (!shell_data)
		return;

	if (strcmp(interface, agl_shell_interface.name) == 0) {
		if (ver >= 2) {
			shell_data->shell =
				static_cast<struct agl_shell *>(
					wl_registry_bind(reg, name, &agl_shell_interface, MIN(8, ver)));
#ifdef AGL_SHELL_BOUND_OK_SINCE_VERSION
			agl_shell_add_listener(shell_data->shell, &shell_listener, data);
#endif
		} else {
			shell_data->shell =
				static_cast<struct agl_shell *>(
					wl_registry_bind(reg, name, &agl_shell_interface, 1));
		}
		shell_data->ver = ver;

	}
}

static void
global_remove(void *data, struct wl_registry *reg, uint32_t id)
{
	/* Don't care */
	(void) data;
	(void) reg;
	(void) id;
}

static const struct wl_registry_listener registry_listener = {
	global_add,
	global_remove,
};

static struct wl_surface *
getWlSurface(QPlatformNativeInterface *native, QWindow *window)
{
	void *surf = native->nativeResourceForWindow("surface", window);
	return static_cast<struct ::wl_surface *>(surf);
}

static struct wl_output *
getWlOutput(QPlatformNativeInterface *native, QScreen *screen)
{
	void *output = native->nativeResourceForScreen("output", screen);
	return static_cast<struct ::wl_output*>(output);
}

static struct wl_display *
getWlDisplay(QPlatformNativeInterface *native)
{
       return static_cast<struct wl_display *>(
               native->nativeResourceForIntegration("display")
       );
}


static void
register_agl_shell(QPlatformNativeInterface *native, struct shell_data *shell_data)
{
	struct wl_display *wl;
	struct wl_registry *registry;

	wl = getWlDisplay(native);
	registry = wl_display_get_registry(wl);

	wl_registry_add_listener(registry, &registry_listener, shell_data);

	/* Roundtrip to get all globals advertised by the compositor */
	wl_display_roundtrip(wl);
	wl_registry_destroy(registry);
}

static struct wl_surface *
create_component(QPlatformNativeInterface *native, QQmlComponent *comp,
		 QScreen *screen, QObject **qobj)
{
	QObject *obj = comp->create();
	obj->setParent(screen);

	QWindow *win = qobject_cast<QWindow *>(obj);
	*qobj = obj;

	return getWlSurface(native, win);
}


static void
load_agl_shell(QPlatformNativeInterface *native, QQmlApplicationEngine *engine,
	       struct agl_shell *agl_shell, QScreen *screen)
{
	struct wl_surface *bg;
	struct wl_output *output;
	int32_t x, y;
	int32_t width, height;
	QObject *qobj_bg;
	QSize size = screen->size();

	// this incorporates the panels directly, but in doing so, it
	// would also need to specify an activation area the same area
	// in order to void overlapping any new activation window
	QQmlComponent bg_comp(engine, QUrl("qrc:/background_with_panels.qml"));
	qInfo() << bg_comp.errors();

	bg = create_component(native, &bg_comp, screen, &qobj_bg);

	output = getWlOutput(native, screen);

	qDebug() << "Normal mode - with single surface";
	qDebug() << "Setting homescreen to screen  " << screen->name();
	agl_shell_set_background(agl_shell, bg, output);

	// 216 is the width size of the panel
	x = 0;
	y = 216;

	width  = size.width();
	height = size.height() - (2 * y);

	qDebug() << "Using custom rectangle " << width << "x" << height
		<< "+" << x << "x" << y << " for activation";
	qDebug() << "Panels should be embedded the background surface";

#ifdef AGL_SHELL_SET_ACTIVATE_REGION_SINCE_VERSION
	agl_shell_set_activate_region(agl_shell, output,
				      x, y, width, height);
#endif
}

static void
load_agl_shell_for_ci(QPlatformNativeInterface *native,
		      QQmlApplicationEngine *engine,
		      struct agl_shell *agl_shell, QScreen *screen)
{
	struct wl_surface *bg, *top, *bottom;
	struct wl_output *output;
	QObject *qobj_bg, *qobj_top, *qobj_bottom;

	QQmlComponent bg_comp(engine, QUrl("qrc:/background_demo.qml"));
	qInfo() << bg_comp.errors();

	QQmlComponent top_comp(engine, QUrl("qrc:/toppanel_demo.qml"));
	qInfo() << top_comp.errors();

	QQmlComponent bot_comp(engine, QUrl("qrc:/bottompanel_demo.qml"));
	qInfo() << bot_comp.errors();

	top = create_component(native, &top_comp, screen, &qobj_top);
	bottom = create_component(native, &bot_comp, screen, &qobj_bottom);
	bg = create_component(native, &bg_comp, screen, &qobj_bg);

	/* engine.rootObjects() works only if we had a load() */
	StatusBarModel *statusBar = qobj_top->findChild<StatusBarModel *>("statusBar");
	if (statusBar) {
		qDebug() << "got statusBar objectname, doing init()";
		statusBar->init(engine->rootContext());
	}

	output = getWlOutput(native, screen);

	qDebug() << "Setting homescreen to screen  " << screen->name();

	agl_shell_set_background(agl_shell, bg, output);
	agl_shell_set_panel(agl_shell, top, output, AGL_SHELL_EDGE_TOP);
	agl_shell_set_panel(agl_shell, bottom, output, AGL_SHELL_EDGE_BOTTOM);

	qDebug() << "CI mode - with multiple surfaces";
}

static void
app_status_callback(::agl_shell_ipc::AppStateResponse app_response, void *data);

static void
run_in_thread(GrpcClient *client)
{
	grpc::Status status = client->Wait();
}

static void
load_agl_shell_app(QPlatformNativeInterface *native, QQmlApplicationEngine *engine,
		   struct shell_data shell_data, const char *screen_name, bool is_demo)
{
	QScreen *screen = nullptr;
	HomescreenHandler *homescreenHandler = shell_data.homescreenHandler;

	if (!screen_name)
		screen = qApp->primaryScreen();
	else
		screen = find_screen(screen_name);

	if (!screen) {
		qDebug() << "No outputs present in the system.";
		return;
	}

	if (is_demo) {
		load_agl_shell_for_ci(native, engine, shell_data.shell, screen);
	} else {
		load_agl_shell(native, engine, shell_data.shell, screen);
	}

	/* Delay the ready signal until after Qt has done all of its own setup
	 * in a.exec() */
	QTimer::singleShot(500, [shell_data](){
		qDebug() << "sending ready to compositor";
		agl_shell_ready(shell_data.shell);
	});
}

static void
app_status_callback(::agl_shell_ipc::AppStateResponse app_response, void *data)
{
	HomescreenHandler *homescreenHandler = static_cast<HomescreenHandler *>(data);

	if (!homescreenHandler) {
		return;
	}

	auto app_id = QString(app_response.app_id().c_str());
	auto state = app_response.state();

	qDebug() << "appstateresponse: app_id " << app_id << "state " << state;

	switch (state) {
	case AGL_SHELL_APP_STATE_STARTED:
		qDebug() << "Got AGL_SHELL_APP_STATE_STARTED for app_id " << app_id;
		homescreenHandler->processAppStatusEvent(app_id, "started");
		break;
	case AGL_SHELL_APP_STATE_TERMINATED:
		qDebug() << "Got AGL_SHELL_APP_STATE_TERMINATED for app_id " << app_id;
		homescreenHandler->processAppStatusEvent(app_id, "terminated");
		break;
	case AGL_SHELL_APP_STATE_ACTIVATED:
		qDebug() << "Got AGL_SHELL_APP_STATE_ACTIVATED for app_id " << app_id;
		homescreenHandler->addAppToStack(app_id);
		break;
	case AGL_SHELL_APP_STATE_DEACTIVATED:
		qDebug() << "Got AGL_SHELL_APP_STATE_DEACTIVATED for app_id " << app_id;
		homescreenHandler->processAppStatusEvent(app_id, "deactivated");
		break;
	default:
		break;
	}
}

int main(int argc, char *argv[])
{
	setenv("QT_QPA_PLATFORM", "wayland", 1);
	setenv("QT_QUICK_CONTROLS_STYLE", "AGL", 1);

	QGuiApplication app(argc, argv);
	const char *screen_name;
	bool is_demo_val = false;
	bool is_embedded_panels = false;
	int ret = 0;
	struct shell_data shell_data = { nullptr, nullptr, true, false, 0 };

	QPlatformNativeInterface *native = qApp->platformNativeInterface();
	screen_name = getenv("HOMESCREEN_START_SCREEN");

	const char *is_demo = getenv("HOMESCREEN_DEMO_CI");
	if (is_demo && strcmp(is_demo, "1") == 0)
		is_demo_val = true;

	const char *embedded_panels = getenv("HOMESCREEN_EMBEDDED_PANELS");
	if (embedded_panels && strcmp(embedded_panels, "1") == 0)
		is_embedded_panels = true;

	QCoreApplication::setOrganizationDomain("LinuxFoundation");
	QCoreApplication::setOrganizationName("AutomotiveGradeLinux");
	QCoreApplication::setApplicationName("HomeScreen");
	QCoreApplication::setApplicationVersion("0.7.0");

	// we need to have an app_id
	app.setDesktopFileName("homescreen");

	GrpcClient *client = new GrpcClient();
	// create a new thread to listner for gRPC events
	std::thread th = std::thread(run_in_thread, client);

	register_agl_shell(native, &shell_data);
	if (!shell_data.shell) {
		fprintf(stderr, "agl_shell extension is not advertised. "
			"Are you sure that agl-compositor is running?\n");
		exit(EXIT_FAILURE);
	}

	qDebug() << "agl-shell interface is at version " << shell_data.ver;
	if (shell_data.ver >= 2) {
		while (ret != -1 && shell_data.wait_for_bound) {
			ret = wl_display_dispatch(getWlDisplay(native));

			if (shell_data.wait_for_bound)
				continue;
		}

		if (!shell_data.bound_ok) {
			qInfo() << "agl_shell extension already in use by other shell client.";
			exit(EXIT_FAILURE);
		}
	}


	std::shared_ptr<struct agl_shell> agl_shell{shell_data.shell, agl_shell_destroy};

	// Import C++ class to QML
	qmlRegisterType<StatusBarModel>("HomeScreen", 1, 0, "StatusBarModel");
	qmlRegisterType<MasterVolume>("MasterVolume", 1, 0, "MasterVolume");

	ApplicationLauncher *launcher = new ApplicationLauncher();
	launcher->setCurrent(QStringLiteral("launcher"));

	HomescreenHandler* homescreenHandler = new HomescreenHandler(launcher);
	shell_data.homescreenHandler = homescreenHandler;
	shell_data.homescreenHandler->setGrpcClient(client);

	// blocks until we're sure connected with the server
	HMI_DEBUG("HomescreenHandler", "Checking if connected to the gRPC server...");
	client->WaitForConnected(500, 10);
	client->AppStatusState(app_status_callback, homescreenHandler);

	QQmlApplicationEngine engine;
	QQmlContext *context = engine.rootContext();

	context->setContextProperty("homescreenHandler", homescreenHandler);
	context->setContextProperty("launcher", launcher);
	context->setContextProperty("weather", new Weather());
	context->setContextProperty("bluetooth", new Bluetooth(false, context));

	load_agl_shell_app(native, &engine, shell_data,
			   screen_name, is_demo_val);

	return app.exec();
}
