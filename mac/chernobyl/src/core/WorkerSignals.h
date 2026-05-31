#pragma once

#include <QObject>
#include <QString>

class WorkerSignals : public QObject
{
    Q_OBJECT

public:
    explicit WorkerSignals(QObject *parent = nullptr) : QObject(parent) {}

signals:
    void log(const QString &message);
    void status(const QString &statusText);
    void progress(int current, int total);
    void finished();
    void error(const QString &message);
};
