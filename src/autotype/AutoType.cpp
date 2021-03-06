/*
 *  Copyright (C) 2012 Felix Geyer <debfx@fobos.de>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "AutoType.h"

#include <QApplication>
#include <QPluginLoader>

#include "config-keepassx.h"

#include "autotype/AutoTypePlatformPlugin.h"
#include "autotype/AutoTypeSelectDialog.h"
#include "autotype/WildcardMatcher.h"
#include "core/Config.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/FilePath.h"
#include "core/Group.h"
#include "core/ListDeleter.h"
#include "core/Tools.h"
#include "gui/MessageBox.h"

AutoType* AutoType::m_instance = nullptr;

AutoType::AutoType(QObject* parent, bool test)
    : QObject(parent)
    , m_inAutoType(false)
    , m_autoTypeDelay(0)
    , m_currentGlobalKey(static_cast<Qt::Key>(0))
    , m_currentGlobalModifiers(0)
    , m_pluginLoader(new QPluginLoader(this))
    , m_plugin(nullptr)
    , m_executor(nullptr)
    , m_windowFromGlobal(0)
{
    // prevent crash when the plugin has unresolved symbols
    m_pluginLoader->setLoadHints(QLibrary::ResolveAllSymbolsHint);

    QString pluginName = "keepassx-autotype-";
    if (!test) {
        pluginName += QApplication::platformName();
    }
    else {
        pluginName += "test";
    }

    QString pluginPath = filePath()->pluginPath(pluginName);

    if (!pluginPath.isEmpty()) {
        #ifdef WITH_XC_AUTOTYPE
        loadPlugin(pluginPath);
        #endif
    }

    connect(qApp, SIGNAL(aboutToQuit()), SLOT(unloadPlugin()));
}

AutoType::~AutoType()
{
    if (m_executor) {
        delete m_executor;
        m_executor = nullptr;
    }
}

void AutoType::loadPlugin(const QString& pluginPath)
{
    m_pluginLoader->setFileName(pluginPath);

    QObject* pluginInstance = m_pluginLoader->instance();
    if (pluginInstance) {
        m_plugin = qobject_cast<AutoTypePlatformInterface*>(pluginInstance);
        m_executor = nullptr;

        if (m_plugin) {
            if (m_plugin->isAvailable()) {
                m_executor = m_plugin->createExecutor();
                connect(pluginInstance, SIGNAL(globalShortcutTriggered()), SIGNAL(globalShortcutTriggered()));
            }
            else {
                unloadPlugin();
            }
        }
    }

    if (!m_plugin) {
        qWarning("Unable to load auto-type plugin:\n%s", qPrintable(m_pluginLoader->errorString()));
    }
}

AutoType* AutoType::instance()
{
    if (!m_instance) {
        m_instance = new AutoType(qApp);
    }

    return m_instance;
}

void AutoType::createTestInstance()
{
    Q_ASSERT(!m_instance);

    m_instance = new AutoType(qApp, true);
}

QStringList AutoType::windowTitles()
{
    if (!m_plugin) {
        return QStringList();
    }

    return m_plugin->windowTitles();
}

void AutoType::performAutoType(const Entry* entry, QWidget* hideWindow, const QString& customSequence, WId window)
{
    if (m_inAutoType || !m_plugin) {
        return;
    }
    m_inAutoType = true;

    QString sequence;
    if (customSequence.isEmpty()) {
        sequence = autoTypeSequence(entry);
    }
    else {
        sequence = customSequence;
    }

    QList<AutoTypeAction*> actions;
    ListDeleter<AutoTypeAction*> actionsDeleter(&actions);

    if (!parseActions(sequence, entry, actions)) {
        m_inAutoType = false; // TODO: make this automatic
        return;
    }

    if (hideWindow) {
#if defined(Q_OS_MAC)
        m_plugin->raiseLastActiveWindow();
#else
        hideWindow->showMinimized();
#endif
    }

    Tools::wait(m_plugin->initialTimeout());

    if (!window) {
        window = m_plugin->activeWindow();
    }

    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    for (AutoTypeAction* action : asConst(actions)) {
        if (m_plugin->activeWindow() != window) {
            qWarning("Active window changed, interrupting auto-type.");
            break;
        }

        action->accept(m_executor);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    m_inAutoType = false;
}

void AutoType::performGlobalAutoType(const QList<Database*>& dbList)
{
    if (m_inAutoType || !m_plugin) {
        return;
    }

    QString windowTitle = m_plugin->activeWindowTitle();

    if (windowTitle.isEmpty()) {
        return;
    }

    m_inAutoType = true;

    QList<Entry*> entryList;
    QHash<Entry*, QString> sequenceHash;

    for (Database* db : dbList) {
        const QList<Entry*> dbEntries = db->rootGroup()->entriesRecursive();
        for (Entry* entry : dbEntries) {
            QString sequence = autoTypeSequence(entry, windowTitle);
            if (!sequence.isEmpty()) {
                entryList << entry;
                sequenceHash.insert(entry, sequence);
            }
        }
    }

    if (entryList.isEmpty()) {
        m_inAutoType = false;
        QString message = tr("Couldn't find an entry that matches the window title:");
        message.append("\n\n");
        message.append(windowTitle);
        MessageBox::information(nullptr, tr("Auto-Type - KeePassXC"), message);
    }
    else if ((entryList.size() == 1) && !config()->get("security/autotypeask").toBool()) {
        m_inAutoType = false;
        performAutoType(entryList.first(), nullptr, sequenceHash[entryList.first()]);
    }
    else {
        m_windowFromGlobal = m_plugin->activeWindow();
        AutoTypeSelectDialog* selectDialog = new AutoTypeSelectDialog();
        connect(selectDialog, SIGNAL(entryActivated(Entry*,QString)),
                SLOT(performAutoTypeFromGlobal(Entry*,QString)));
        connect(selectDialog, SIGNAL(rejected()), SLOT(resetInAutoType()));
        selectDialog->setEntries(entryList, sequenceHash);
#if defined(Q_OS_MAC)
        m_plugin->raiseOwnWindow();
        Tools::wait(500);
#endif
        selectDialog->show();
        // necessary when the main window is minimized
        selectDialog->activateWindow();
    }
}

void AutoType::performAutoTypeFromGlobal(Entry* entry, const QString& sequence)
{
    Q_ASSERT(m_inAutoType);

    m_plugin->raiseWindow(m_windowFromGlobal);

    m_inAutoType = false;
    performAutoType(entry, nullptr, sequence, m_windowFromGlobal);
}

void AutoType::resetInAutoType()
{
    Q_ASSERT(m_inAutoType);

    m_inAutoType = false;
}

void AutoType::unloadPlugin()
{
    if (m_executor) {
        delete m_executor;
        m_executor = nullptr;
    }

    if (m_plugin) {
        m_plugin->unload();
        m_plugin = nullptr;
    }
}

bool AutoType::registerGlobalShortcut(Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    Q_ASSERT(key);
    Q_ASSERT(modifiers);

    if (!m_plugin) {
        return false;
    }

    if (key != m_currentGlobalKey || modifiers != m_currentGlobalModifiers) {
        if (m_currentGlobalKey && m_currentGlobalModifiers) {
            m_plugin->unregisterGlobalShortcut(m_currentGlobalKey, m_currentGlobalModifiers);
        }

        if (m_plugin->registerGlobalShortcut(key, modifiers)) {
            m_currentGlobalKey = key;
            m_currentGlobalModifiers = modifiers;
            return true;
        }
        else {
            return false;
        }
    }
    else {
        return true;
    }
}

void AutoType::unregisterGlobalShortcut()
{
    if (m_plugin && m_currentGlobalKey && m_currentGlobalModifiers) {
        m_plugin->unregisterGlobalShortcut(m_currentGlobalKey, m_currentGlobalModifiers);
    }
}

int AutoType::callEventFilter(void* event)
{
    if (!m_plugin) {
        return -1;
    }

    return m_plugin->platformEventFilter(event);
}

bool AutoType::parseActions(const QString& sequence, const Entry* entry, QList<AutoTypeAction*>& actions)
{
    QString tmpl;
    bool inTmpl = false;
    m_autoTypeDelay = 0;


    for (const QChar& ch : sequence) {
        // TODO: implement support for {{}, {}}

        if (inTmpl) {
            if (ch == '{') {
                qWarning("Syntax error in auto-type sequence.");
                return false;
            }
            else if (ch == '}') {
                actions.append(createActionFromTemplate(tmpl, entry));
                inTmpl = false;
                tmpl.clear();
            }
            else {
                tmpl += ch;
            }
        }
        else if (ch == '{') {
            inTmpl = true;
        }
        else if (ch == '}') {
            qWarning("Syntax error in auto-type sequence.");
            return false;
        }
        else {
            actions.append(new AutoTypeChar(ch));
        }
    }
    if (m_autoTypeDelay > 0) {
        QList<AutoTypeAction*>::iterator i;
        i = actions.begin();
        while (i != actions.end()) {
            ++i;
            if (i != actions.end()) {
                i = actions.insert(i, new AutoTypeDelay(m_autoTypeDelay));
                ++i;
            }
        }
    }
    return true;
}

QList<AutoTypeAction*> AutoType::createActionFromTemplate(const QString& tmpl, const Entry* entry)
{
    QString tmplName = tmpl;
    int num = -1;
    QList<AutoTypeAction*> list;

    QRegExp delayRegEx("delay=(\\d+)", Qt::CaseInsensitive, QRegExp::RegExp2);
    if (delayRegEx.exactMatch(tmplName)) {
        num = delayRegEx.cap(1).toInt();
        m_autoTypeDelay = std::max(0, std::min(num, 10000));
        return list;
    }

    QRegExp repeatRegEx("(.+) (\\d+)", Qt::CaseInsensitive, QRegExp::RegExp2);
    if (repeatRegEx.exactMatch(tmplName)) {
        tmplName = repeatRegEx.cap(1);
        num = repeatRegEx.cap(2).toInt();

        if (num == 0) {
            return list;
        }
        // some safety checks
        else if (tmplName.compare("delay",Qt::CaseInsensitive)==0) {
            if (num > 10000) {
                return list;
            }
        }
        else if (num > 100) {
            return list;
        }
    }

    if (tmplName.compare("tab",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Tab));
    }
    else if (tmplName.compare("enter",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Enter));
    }
    else if (tmplName.compare("space",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Space));
    }
    else if (tmplName.compare("up",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Up));
    }
    else if (tmplName.compare("down",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Down));
    }
    else if (tmplName.compare("left",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Left));
    }
    else if (tmplName.compare("right",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Right));
    }
    else if (tmplName.compare("insert",Qt::CaseInsensitive)==0 ||
             tmplName.compare("ins",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Insert));
    }
    else if (tmplName.compare("delete",Qt::CaseInsensitive)==0 ||
             tmplName.compare("del",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Delete));
    }
    else if (tmplName.compare("home",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Home));
    }
    else if (tmplName.compare("end",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_End));
    }
    else if (tmplName.compare("pgup",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_PageUp));
    }
    else if (tmplName.compare("pgdown",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_PageDown));
    }
    else if (tmplName.compare("backspace",Qt::CaseInsensitive)==0 ||
             tmplName.compare("bs",Qt::CaseInsensitive)==0 ||
             tmplName.compare("bksp",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Backspace));
    }
    else if (tmplName.compare("break",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Pause));
    }
    else if (tmplName.compare("capslock",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_CapsLock));
    }
    else if (tmplName.compare("esc",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Escape));
    }
    else if (tmplName.compare("help",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Help));
    }
    else if (tmplName.compare("numlock",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_NumLock));
    }
    else if (tmplName.compare("ptrsc",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_Print));
    }
    else if (tmplName.compare("scrolllock",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeKey(Qt::Key_ScrollLock));
    }
    // Qt doesn't know about keypad keys so use the normal ones instead
    else if (tmplName.compare("add",Qt::CaseInsensitive)==0 ||
             tmplName.compare("+",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeChar('+'));
    }
    else if (tmplName.compare("subtract",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeChar('-'));
    }
    else if (tmplName.compare("multiply",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeChar('*'));
    }
    else if (tmplName.compare("divide",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeChar('/'));
    }
    else if (tmplName.compare("^",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeChar('^'));
    }
    else if (tmplName.compare("%",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeChar('%'));
    }
    else if (tmplName.compare("~",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeChar('~'));
    }
    else if (tmplName.compare("(",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeChar('('));
    }
    else if (tmplName.compare(")",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeChar(')'));
    }
    else if (tmplName.compare("{",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeChar('{'));
    }
    else if (tmplName.compare("}",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeChar('}'));
    }
    else {
        QRegExp fnRegexp("f(\\d+)", Qt::CaseInsensitive, QRegExp::RegExp2);
        if (fnRegexp.exactMatch(tmplName)) {
            int fnNo = fnRegexp.cap(1).toInt();
            if (fnNo >= 1 && fnNo <= 16) {
                list.append(new AutoTypeKey(static_cast<Qt::Key>(Qt::Key_F1 - 1 + fnNo)));
            }
        }
    }

    if (!list.isEmpty()) {
        for (int i = 1; i < num; i++) {
            list.append(list.at(0)->clone());
        }

        return list;
    }


    if (tmplName.compare("delay",Qt::CaseInsensitive)==0 && num > 0) {
        list.append(new AutoTypeDelay(num));
    }
    else if (tmplName.compare("clearfield",Qt::CaseInsensitive)==0) {
        list.append(new AutoTypeClearField());
    }

    if (!list.isEmpty()) {
        return list;
    }

    const QString placeholder = QString("{%1}").arg(tmplName);
    const QString resolved = entry->resolvePlaceholder(placeholder);
    if (placeholder != resolved) {
        for (const QChar& ch : resolved) {
            if (ch == '\n') {
                list.append(new AutoTypeKey(Qt::Key_Enter));
            }
            else if (ch == '\t') {
                list.append(new AutoTypeKey(Qt::Key_Tab));
            }
            else {
                list.append(new AutoTypeChar(ch));
            }
        }
    }

    return list;
}

QString AutoType::autoTypeSequence(const Entry* entry, const QString& windowTitle)
{
    if (!entry->autoTypeEnabled()) {
        return QString();
    }

    bool enableSet = false;
    QString sequence;
    if (!windowTitle.isEmpty()) {
        bool match = false;
        const QList<AutoTypeAssociations::Association> assocList = entry->autoTypeAssociations()->getAll();
        for (const AutoTypeAssociations::Association& assoc : assocList) {
            if (windowMatches(windowTitle, assoc.window)) {
                if (!assoc.sequence.isEmpty()) {
                    sequence = assoc.sequence;
                }
                else {
                    sequence = entry->defaultAutoTypeSequence();
                }
                match = true;
                break;
            }
        }

        if (!match && config()->get("AutoTypeEntryTitleMatch").toBool() && !entry->title().isEmpty()
                && windowTitle.contains(entry->title(), Qt::CaseInsensitive)) {
            sequence = entry->defaultAutoTypeSequence();
            match = true;
        }

        if (!match) {
            return QString();
        }
    }
    else {
        sequence = entry->defaultAutoTypeSequence();
    }

    const Group* group = entry->group();
    do {
        if (!enableSet) {
            if (group->autoTypeEnabled() == Group::Disable) {
                return QString();
            }
            else if (group->autoTypeEnabled() == Group::Enable) {
                enableSet = true;
            }
        }

        if (sequence.isEmpty()) {
            sequence = group->defaultAutoTypeSequence();
        }

        group = group->parentGroup();
    } while (group && (!enableSet || sequence.isEmpty()));

    if (sequence.isEmpty() && (!entry->username().isEmpty() || !entry->password().isEmpty())) {
        if (entry->username().isEmpty()) {
            sequence = "{PASSWORD}{ENTER}";
        }
        else if (entry->password().isEmpty()) {
            sequence = "{USERNAME}{ENTER}";
        }
        else {
            sequence = "{USERNAME}{TAB}{PASSWORD}{ENTER}";
        }
    }

    return sequence;
}

bool AutoType::windowMatches(const QString& windowTitle, const QString& windowPattern)
{
    if (windowPattern.startsWith("//") && windowPattern.endsWith("//") && windowPattern.size() >= 4) {
        QRegExp regExp(windowPattern.mid(2, windowPattern.size() - 4), Qt::CaseInsensitive, QRegExp::RegExp2);
        return (regExp.indexIn(windowTitle) != -1);
    }
    else {
        return WildcardMatcher(windowTitle).match(windowPattern);
    }
}
