// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2017, 2018, 2019 TOYOTA MOTOR CORPORATION
 * Copyright (c) 2022 Konsulko Group
 */

#include <QGuiApplication>
#include <QScreen>
#include <QFileInfo>
#include <functional>

#include "homescreenhandler.h"
#include "hmi-debug.h"

QScreen *find_screen(const char *output);

// defined by meson build file
#include "qpa/qplatformnativeinterface.h"

// LAUNCHER_APP_ID shouldn't be started by applaunchd as it is started as
// a user session by systemd
#define LAUNCHER_APP_ID          "launcher"


HomescreenHandler::HomescreenHandler(ApplicationLauncher *launcher, GrpcClient *_client, QObject *parent) :
	QObject(parent)
	, m_grpc_client(_client)
{
	mp_launcher = launcher;
	// mp_applauncher_client = new AppLauncherClient();

	//
	// The "started" event is received any time a start request is made to applaunchd,
	// and the application either starts successfully or is already running. This
	// effectively acts as a "switch to app X" action.
	//
	// connect(mp_applauncher_client,
	// 	&AppLauncherClient::appStatusEvent,
	// 	this,
	// 	&HomescreenHandler::processAppStatusEvent);
}

HomescreenHandler::~HomescreenHandler()
{
	delete m_grpc_client;
	// delete mp_applauncher_client;
}

void HomescreenHandler::tapShortcut(QString app_id)
{
	HMI_DEBUG("HomeScreen","tapShortcut %s", app_id.toStdString().c_str());

	if (app_id == LAUNCHER_APP_ID) {
		activateApp(app_id);
		return;
	}

	// if (!mp_applauncher_client->startApplication(app_id)) {
	// 	HMI_ERROR("HomeScreen","Unable to start application '%s'",
	// 		  app_id.toStdString().c_str());
	// 	return;
	// }
}

/*
 * Keep track of currently running apps and the order in which
 * they were activated. That way, when an app is closed, we can
 * switch back to the previously active one.
 */
void HomescreenHandler::addAppToStack(const QString& app_id)
{
	if (app_id == "homescreen")
		return;

	if (!apps_stack.contains(app_id)) {
		apps_stack << app_id;
	} else {
		int current_pos = apps_stack.indexOf(app_id);
		int last_pos = apps_stack.size() - 1;

		if (current_pos != last_pos)
			apps_stack.move(current_pos, last_pos);
	}
}

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

void HomescreenHandler::activateApp(const QString& app_id)
{
	QScreen *default_screen = qApp->screens().first();
	std::string default_output_name;

	if (!default_screen) {
		HMI_DEBUG("HomeScreen", "No default output found to activate on!\n");
	} else {
		default_output_name = default_screen->name().toStdString();
		HMI_DEBUG("HomeScreen", "Activating app_id %s by default on output %s\n",
				app_id.toStdString().c_str(), default_output_name.c_str());
	}

	if (mp_launcher) {
		mp_launcher->setCurrent(app_id);
	}

	// search for a pending application which might have a different output
	auto iter = pending_app_list.begin();
	bool found_pending_app = false;
	while (iter != pending_app_list.end()) {
		const QString &app_to_search = iter->first;

		if (app_to_search == app_id) {
			found_pending_app = true;
			HMI_DEBUG("HomeScreen", "Found app_id %s in pending list  of applications",
					app_id.toStdString().c_str());
			break;
		}

		iter++;
	}

	if (found_pending_app) {
		const QString &output_name = iter->second;
		QScreen *screen =
			::find_screen(output_name.toStdString().c_str());

		if (!screen) {
			HMI_DEBUG("HomeScreen", "Can't activate application %s on another "
				  "output, because output %s could not be found. "
				  "Trying with remoting ones.",
				  app_id.toStdString().c_str(),
				  output_name.toStdString().c_str());

			// try with remoting-remote-X which is the streaming
			// one
			std::string new_remote_output = 
				"remoting-" + output_name.toStdString();

			screen = ::find_screen(new_remote_output.c_str());
			if (!screen) {
				HMI_DEBUG("HomeScreen", "Can't activate application %s on another "
					  "output, because output remoting-%s could not be found",
					  app_id.toStdString().c_str(),
					  output_name.toStdString().c_str());
				return;
			}

			HMI_DEBUG("HomeScreen", "Found a stream remoting output %s to activate application %s on",
				  new_remote_output.c_str(),
				  app_id.toStdString().c_str());
			default_output_name = new_remote_output;
		} else {
			default_output_name = output_name.toStdString();
		}

		pending_app_list.erase(iter);
		HMI_DEBUG("HomeScreen", "For application %s found another "
				"output to activate %s\n",
				app_id.toStdString().c_str(),
				default_output_name.c_str());
	}

	if (default_output_name.empty()) {
		HMI_DEBUG("HomeScreen", "No suitable output found for activating %s",
				app_id.toStdString().c_str());
		return;
	}

	HMI_DEBUG("HomeScreen", "Activating application %s on output %s",
			app_id.toStdString().c_str(), default_output_name.c_str());
	m_grpc_client->ActivateApp(app_id.toStdString(), default_output_name);
}

void HomescreenHandler::deactivateApp(const QString& app_id)
{
	if (apps_stack.contains(app_id)) {
		apps_stack.removeOne(app_id);
		if (!apps_stack.isEmpty())
			activateApp(apps_stack.last());
	}
}

void HomescreenHandler::processAppStatusEvent(const QString &app_id, const QString &status)
{
	HMI_DEBUG("HomeScreen", "Processing application %s, status %s",
			app_id.toStdString().c_str(), status.toStdString().c_str());

	if (status == "started") {
		activateApp(app_id);
	} else if (status == "terminated") {
		HMI_DEBUG("HomeScreen", "Application %s terminated, activating last app", app_id.toStdString().c_str());
		deactivateApp(app_id);
	} else if (status == "deactivated") {
		HMI_DEBUG("HomeScreen", "Application %s deactivated, activating last app", app_id.toStdString().c_str());
	}
}
