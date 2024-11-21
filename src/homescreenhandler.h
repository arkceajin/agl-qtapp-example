// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (c) 2017 TOYOTA MOTOR CORPORATION
 * Copyright (c) 2022 Konsulko Group
 */

#ifndef HOMESCREENHANDLER_H
#define HOMESCREENHANDLER_H

#include <QObject>
#include <string>

#include "applicationlauncher.h"
#include "AppLauncherClient.h"
#include "AglShellGrpcClient.h"

using namespace std;

class HomescreenHandler : public QObject
{
	Q_OBJECT
public:
	explicit HomescreenHandler(ApplicationLauncher *launcher = 0, GrpcClient *_client = nullptr, QObject *parent = 0);
	~HomescreenHandler();

	Q_INVOKABLE void tapShortcut(QString application_id);

	void addAppToStack(const QString& application_id);
	void activateApp(const QString& app_id);
	void deactivateApp(const QString& app_id);
	void setGrpcClient(GrpcClient *_client) { m_grpc_client = _client; }
	GrpcClient *getGrpcClient(void) { return m_grpc_client; }

	QStringList apps_stack;
	std::list<std::pair<const QString, const QString>> pending_app_list;
signals:
	void showNotification(QString application_id, QString icon_path, QString text);
	void showInformation(QString info);

public slots:
	void processAppStatusEvent(const QString &id, const QString &status);

private:
	ApplicationLauncher *mp_launcher;
	AppLauncherClient *mp_applauncher_client;
	GrpcClient *m_grpc_client;
};

#endif // HOMESCREENHANDLER_H
