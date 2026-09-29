#pragma once
#include <QSqlDatabase>
#include <QString>

// The one SQLite store. All schema work lives here.
namespace Store {

// Invalid when the file cannot be opened.
QSqlDatabase open(const QString &connectionName);

// Once per session, by the library. Narrow on purpose: episode progress outlives the library,
// so only rows whose show is in neither library nor history go.
void prune(QSqlDatabase &db);

}
