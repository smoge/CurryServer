/*
    SuperCollider Qt IDE
    Copyright (c) 2012 Jakob Leben & Tim Blechmann
    http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "main.hpp"
#include "settings/manager.hpp"
#include "session_manager.hpp"
#include "util/standard_dirs.hpp"
#include "../widgets/main_window.hpp"
#include "../widgets/lookup_dialog.hpp"
#include "../widgets/code_editor/highlighter.hpp"
#include "../widgets/style/style.hpp"
#include "../../../QtCollider/hacks/hacks_mac.hpp"
#include "../primitives/localsocket_utils.hpp"

#include <yaml-cpp/node/node.h>
#include <yaml-cpp/parser.h>

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFileOpenEvent>
#include <QLibraryInfo>
#include <QTranslator>
#include <QDebug>
#include <QStyleFactory>

#include "util/HelpBrowserWebSocketServices.hpp"

using namespace ScIDE;


bool SingleInstanceGuard::tryConnect(QStringList const& arguments) {
    const int maxNumberOfInstances = 128;
    if (!arguments.empty()) {
        for (int socketID = 0; socketID != maxNumberOfInstances; ++socketID) {
            QString serverName = QStringLiteral("SuperColliderIDE_Singleton_%1").arg(socketID);
            QSharedPointer<QLocalSocket> socket(new QLocalSocket(this));
            socket->connectToServer(serverName);

            QStringList canonicalArguments;
            foreach (QString path, arguments) {
                QFileInfo info(path);
                canonicalArguments << info.canonicalFilePath();
            }

            if (socket->waitForConnected(200)) {
                sendSelectorAndData(socket.data(), QStringLiteral("open"), canonicalArguments);
                if (!socket->waitForBytesWritten(300))
                    qWarning("SingleInstanceGuard: writing data to another IDE instance timed out");

                return true;
            }
        }
    }

    mIpcServer = new QLocalServer(this);
    for (int socketID = 0; socketID != maxNumberOfInstances; ++socketID) {
        QString serverName = QStringLiteral("SuperColliderIDE_Singleton_%1").arg(socketID);

        bool listening = mIpcServer->listen(serverName);
        if (listening) {
            connect(mIpcServer, SIGNAL(newConnection()), this, SLOT(onNewIpcConnection()));
            return false;
        }
    }
    return false;
}

void SingleInstanceGuard::onIpcData() {
    mIpcData.append(mIpcSocket->readAll());

    // After we have put the data in the buffer, process it
    int avail = mIpcData.length();
    do {
        if (mReadSize == 0 && avail > 4) {
            mReadSize = ArrayToInt(mIpcData.left(4));
            mIpcData.remove(0, 4);
            avail -= 4;
        }

        if (mReadSize > 0 && avail >= mReadSize) {
            QByteArray baReceived(mIpcData.left(mReadSize));
            mIpcData.remove(0, mReadSize);
            mReadSize = 0;

            QDataStream in(baReceived);
            in.setVersion(QDataStream::Qt_4_6);
            QString selector;
            in >> selector;
            if (in.status() != QDataStream::Ok)
                return;

            QStringList message;
            in >> message;
            if (in.status() != QDataStream::Ok)
                return;

            if (selector == QStringLiteral("open")) {
                foreach (QString path, message)
                    Main::documentManager()->open(path);
            }
        }
    } while ((mReadSize == 0 && avail > 4) || (mReadSize > 0 && avail > mReadSize));
}


static inline QString getSettingsFile() { return standardDirectory(ScConfigUserDir) + "/sc_ide_conf.yaml"; }

// NOTE: mSettings must be the first to initialize,
// because other members use it!

Main::Main(void):
    mSettings(new Settings::Manager(getSettingsFile(), this)),
    mScProcess(new ScProcess(mSettings, this)),
    mScServer(new ScServer(mScProcess, mSettings, this)),
    mDocManager(new DocumentManager(this, mSettings)),
    mSessionManager(new SessionManager(mDocManager, this)) {
    new SyntaxHighlighterGlobals(this, mSettings);

#ifdef Q_OS_MAC
    QtCollider::Mac::DisableAutomaticWindowTabbing();
#endif

    connect(mScProcess, SIGNAL(response(QString, QString)), mDocManager, SLOT(handleScLangMessage(QString, QString)));

    qApp->installEventFilter(this);
    qApp->installNativeEventFilter(this);
}

void Main::quit() {
    mSessionManager->saveSession();
    storeSettings();
    mScProcess->stopLanguage();
    QApplication::quit();
}

bool Main::eventFilter(QObject* object, QEvent* event) {
    switch (event->type()) {
    case QEvent::FileOpen: {
        // open the file dragged onto the application icon on Mac
        QFileOpenEvent* openEvent = static_cast<QFileOpenEvent*>(event);
        mDocManager->open(openEvent->file());
        return true;
    }

    case QEvent::MouseMove:
        QApplication::restoreOverrideCursor();
        break;

    default:
        break;
    }

    return QObject::eventFilter(object, event);
}

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
bool Main::nativeEventFilter(const QByteArray&, void* message, long*) {
#else
bool Main::nativeEventFilter(const QByteArray&, void* message, qintptr*) {
#endif
    bool result = false;

#ifdef Q_OS_MAC
    if (QtCollider::Mac::IsCmdPeriodKeyDown(reinterpret_cast<void*>(message))) {
        //        QKeyEvent event(QEvent::KeyPress, Qt::Key_Period, Qt::ControlModifier, ".");
        //        QApplication::sendEvent(this, &event);
        mScProcess->stopMain(); // we completely bypass the shortcut handling
        result = true;
    } else if (QtCollider::Mac::IsCmdPeriodKeyUp(reinterpret_cast<void*>(message))) {
        result = true;
    }
#endif

    return result;
}

bool Main::openDocumentation(const QString& string) {
#ifdef SC_USE_QTWEBENGINE
    QString symbol = string.trimmed();
    if (symbol.isEmpty())
        return false;

    HelpBrowserDocklet* helpDock = MainWindow::instance()->helpBrowserDocklet();
    helpDock->browser()->gotoHelpFor(symbol);
    helpDock->focus();
    return true;
#else // SC_USE_QTWEBENGINE
    return false;
#endif // SC_USE_QTWEBENGINE
}

bool Main::openDocumentationForMethod(const QString& className, const QString& methodName) {
#ifdef SC_USE_QTWEBENGINE
    HelpBrowserDocklet* helpDock = MainWindow::instance()->helpBrowserDocklet();
    helpDock->browser()->gotoHelpForMethod(className, methodName);
    helpDock->focus();
    return true;
#else // SC_USE_QTWEBENGINE
    return false;
#endif // SC_USE_QTWEBENGINE
}

void Main::openDefinition(const QString& string, QWidget* parent) {
    QString definitionString = string.trimmed();

    LookupDialog dialog(parent);
    if (!definitionString.isEmpty())
        dialog.query(definitionString);
    dialog.exec();
}

void Main::openCommandLine(const QString& string) { MainWindow::instance()->showCmdLine(string); }

void Main::findReferences(const QString& string, QWidget* parent) {
    QString definitionString = string.trimmed();

    ReferencesDialog dialog(parent);
    if (!definitionString.isEmpty())
        dialog.query(definitionString);
    dialog.exec();
}
