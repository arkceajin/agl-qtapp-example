#include <QGuiApplication>
#include <QQmlApplicationEngine>
// #include "applicationlauncher.h"
#include <qpa/qplatformnativeinterface.h>

#include "applicationlauncher.h"
#include "homescreenhandler.h"

#include <wayland-client.h>
#include <thread>

#include "AglShellGrpcClient.h"

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
	case 0:
		qDebug() << "Got AGL_SHELL_APP_STATE_STARTED for app_id " << app_id;
		homescreenHandler->processAppStatusEvent(app_id, "started");
		break;
	case 1:
		qDebug() << "Got AGL_SHELL_APP_STATE_TERMINATED for app_id " << app_id;
		homescreenHandler->processAppStatusEvent(app_id, "terminated");
		break;
	case 2:
		qDebug() << "Got AGL_SHELL_APP_STATE_ACTIVATED for app_id " << app_id;
		homescreenHandler->addAppToStack(app_id);
		break;
	case 3:
		qDebug() << "Got AGL_SHELL_APP_STATE_DEACTIVATED for app_id " << app_id;
		homescreenHandler->processAppStatusEvent(app_id, "deactivated");
		break;
	default:
		break;
	}
}

static void
run_in_thread(GrpcClient *client)
{
	grpc::Status status = client->Wait();
}

int main(int argc, char *argv[])
{
    setenv("QT_QPA_PLATFORM", "wayland", 1);
    setenv("QT_QUICK_CONTROLS_STYLE", "AGL", 1);

    QGuiApplication app(argc, argv);

    QPlatformNativeInterface *native = qApp->platformNativeInterface();

    QCoreApplication::setOrganizationDomain("LinuxFoundation");
    QCoreApplication::setOrganizationName("AutomotiveGradeLinux");
    QCoreApplication::setApplicationName("HomeScreen");
    QCoreApplication::setApplicationVersion("0.7.0");

    app.setDesktopFileName("homescreen");

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("hello-qml", "Main");

    auto win = QGuiApplication::allWindows()[0];
    static_cast<struct wl_surface *>(native->nativeResourceForWindow("surface", win));

	GrpcClient *client = new GrpcClient();
	// create a new thread to listner for gRPC events
	std::thread th = std::thread(run_in_thread, client);

	ApplicationLauncher *launcher = new ApplicationLauncher();
	launcher->setCurrent(QStringLiteral("launcher"));
    
	HomescreenHandler* homescreenHandler = new HomescreenHandler(launcher);
	homescreenHandler->setGrpcClient(client);

	client->WaitForConnected(500, 10);
	client->AppStatusState(app_status_callback, homescreenHandler);
    
    qDebug()<<"homescreenHandler->activateApp";
    homescreenHandler->activateApp("homescreen");

    return app.exec();
}
