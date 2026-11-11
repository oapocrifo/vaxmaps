#pragma once
// Interface language. English is the source: T("text") returns the text itself, or its Portuguese version when
// that language is on. Qt's own dialogs and menus (colour dialog, Save/Discard, Copy/Paste) use the same table.
#include <QStringList>

QString T(const char *english);
QString T(const QString &english);
bool portuguese();
void setPortuguese(bool on);   // every widget gets a LanguageChange event
QStringList untranslated();    // texts asked for in Portuguese that the table lacks (self-test)
