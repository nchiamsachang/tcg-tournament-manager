#include "applog.h"
#include "db.h"
#include "main_window.h"
#include "prefs.h"

#include <QApplication>
#include <QMessageBox>
#include <exception>

// Screens catch the errors they expect and explain them.  This is the net underneath: an
// exception that escapes a button handler would otherwise end the program, so it is shown
// instead.  Saves are transactions, so a save that failed part-way has been rolled back.
class App : public QApplication {
public:
    using QApplication::QApplication;

    bool notify(QObject *receiver, QEvent *event) override
    {
        try {
            return QApplication::notify(receiver, event);
        } catch (const std::exception &e) {
            report(QString::fromUtf8(e.what()));
        }
        return false;
    }

private:
    void report(const QString &detail)
    {
        if (reporting_)                 // an error while the message is open: do not stack dialogs
            return;
        reporting_ = true;
        // written (and flushed) before the dialog is shown, with the reference the dialog carries
        QString op;
        const QString ref = applog::errorReference(&op);        // the failed action's reference, when there is one
        applog::Fields fields{{"ref", ref}};
        if (!op.isEmpty())
            fields.append({"op", op});
        applog::error("exception.unhandled", fields, applog::scrub(detail));
        QMessageBox::warning(activeWindow(), "Action not completed",
                             "The last action could not be completed.\n\n" + detail + "\n\nReference: " + ref);
        reporting_ = false;
    }
    bool reporting_ = false;
};

int main(int argc, char *argv[])
{
    // One style engine on every Windows version: the stylesheet draws the controls, and
    // popups (dropdown lists, menus) are ordinary opaque windows rather than translucent ones.
    QApplication::setStyle("Fusion");
    App app(argc, argv);
    app.setApplicationName("TCG Tournament Manager");
    app.setApplicationVersion(prefs::VERSION);
    // The log lives beside the database, in the user's own data folder.  Anything logged
    // while that folder was being worked out was held back and is written now.
    applog::captureQtMessages();
    applog::setDirectory(db::dataDir() + "/logs");
    applog::sessionStarted();
    try {
        MainWindow window;
        window.show();
        const int code = app.exec();
        applog::sessionEnded();
        return code;
    } catch (const std::exception &e) {
        // the database could not be opened, or could not be backed up before an update
        const QString ref = applog::newReference();
        applog::error("app.start_failed", {{"ref", ref}}, QString::fromUtf8(e.what()));
        QMessageBox::critical(nullptr, "TCG Tournament Manager",
                              "The program could not start.\n\n" + QString::fromUtf8(e.what()) + "\n\nReference: " + ref);
        applog::sessionEnded();
        return 1;
    }
}
