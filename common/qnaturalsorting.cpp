#include "qnaturalsorting.h"

#include <QCollator>
#include <QLocale>

static QCollator collatorCI = [] {
    QCollator c;
    // Qt's C/POSIX collation ignores numeric mode ("10" sorts before "2").
    if (c.locale().language() == QLocale::C)
        c.setLocale(QLocale(QLocale::English, QLocale::UnitedStates));
    c.setNumericMode(true);
    c.setIgnorePunctuation(false);
    c.setCaseSensitivity(Qt::CaseInsensitive);
    return c;
}();

static QCollator collatorCS = [] {
    QCollator c;
    if (c.locale().language() == QLocale::C)
        c.setLocale(QLocale(QLocale::English, QLocale::UnitedStates));
    c.setNumericMode(true);
    c.setIgnorePunctuation(false);
    c.setCaseSensitivity(Qt::CaseSensitive);
    return c;
}();

int naturalCompare(const QString &s1, const QString &s2, Qt::CaseSensitivity caseSensitivity)
{
    QCollator &c = (caseSensitivity == Qt::CaseSensitive) ? collatorCS : collatorCI;
    return c.compare(s1, s2);
}
bool naturalSortLessThanCS(const QString &left, const QString &right)
{
    return (naturalCompare(left, right, Qt::CaseSensitive) < 0);
}

bool naturalSortLessThanCI(const QString &left, const QString &right)
{
    return (naturalCompare(left, right, Qt::CaseInsensitive) < 0);
}

bool comicNumberLessThan(const QVariant &leftNumber, const QString &leftName,
                         const QVariant &rightNumber, const QString &rightName)
{
    if (leftNumber.isNull() && rightNumber.isNull())
        return naturalSortLessThanCI(leftName, rightName);

    if (!leftNumber.isNull() && !rightNumber.isNull())
        return naturalSortLessThanCI(leftNumber.toString(), rightNumber.toString());

    return rightNumber.isNull();
}

bool naturalSortLessThanCIFileInfo(const QFileInfo &left, const QFileInfo &right)
{
    return naturalSortLessThanCI(left.fileName(), right.fileName());
}

bool naturalSortLessThanCILibraryItem(LibraryItem *left, LibraryItem *right)
{
    return naturalSortLessThanCI(left->name, right->name);
}
