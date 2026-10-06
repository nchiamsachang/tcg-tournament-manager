#include "main_window.h"

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
        QMessageBox::warning(activeWindow(), "Action not completed",
                             "The last action could not be completed.\n\n" + detail);
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
    MainWindow window;
    window.show();
    return app.exec();
}
