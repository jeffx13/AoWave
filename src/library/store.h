#pragma once
#include <QSqlDatabase>
#include <QString>

namespace Store {

constexpr int kHistoryRows = 100;

QSqlDatabase open(const QString &connectionName);

void prune(QSqlDatabase &db);

}
