/*
 * chatdlg.cpp - dialog for handling chats
 * Copyright (C) 2001-2007  Justin Karneges, Michail Pishchagin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "chatdlg.h"

#include "accountlabel.h"
#include "avatars.h"
#include "chatview.h"
#include "eventdb.h"
#include "fancylabel.h"
#include "filesharedlg.h"
#include "filesharingmanager.h"
#include "iconaction.h"
#include "iconlabel.h"
#include "iconselect.h"
#include "iconwidget.h"
#include "jidutil.h"
#include "msgmle.h"
#include "pgputil.h"
#ifdef PSI_PLUGINS
#include "pluginmanager.h"
#endif
#include "psiaccount.h"
#include "psichatdlg.h"
#include "psicon.h"
#include "psicontactlist.h"
#include "psiiconset.h"
#include "psioptions.h"
#include "psirichtext.h"
#include "psitooltip.h"
#include "shortcutmanager.h"
#include "stretchwidget.h"
#include "tabdlg.h"
#include "textutil.h"
#include "userlist.h"
#include "xmpp_caps.h"
#include "xmpp_htmlelement.h"
#include "xmpp_message.h"

#include <QCloseEvent>
#include <QColor>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSplitter>
#include <QTextDocument> // for TextUtil::escape()
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <Qt>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

static const QString geometryOption = "options.ui.chat.size";

ChatDlg *ChatDlg::create(const Jid &jid, PsiAccount *account, TabManager *tabManager)
{
    ChatDlg *chat = new PsiChatDlg(jid, account, tabManager);
    chat->init();
    return chat;
}

ChatDlg::ChatDlg(const Jid &jid, PsiAccount *pa, TabManager *tabManager) :
    TabbableWidget(jid, pa, tabManager), highlightersInstalled_(false), delayedMessages(nullptr)
{
    pending_          = 0;
    keepOpen_         = false;
    warnSend_         = false;
    transid_          = -1;
    key_              = "";
    lastWasEncrypted_ = false;
    trackBar_         = false;

    status_ = -1;

    if (!pa->findGCContact(jid) || ((pa->edb()->features() & EDB::PrivateContacts) != 0)) {
        historyState = false;
        preloadHistory();
    } else
        historyState = true;

    autoSelectContact_ = false;
    if (PsiOptions::instance()->getOption("options.ui.chat.default-jid-mode").toString() == "auto") {
        UserListItem *uli = account()->findFirstRelevant(jid);
        if (!uli || (!uli->isPrivate() && (jid.resource().isEmpty() || uli->userResourceList().count() <= 1))) {
            autoSelectContact_ = true;
        }
    }

    // Message events
    contactChatState_ = XMPP::StateNone;
    if ((PsiOptions::instance()->getOption("options.messages.send-composing-events-at-start").toBool())
        && (account()->client()->capsManager()->features(jid).hasChatState())) {
        contactChatState_ = XMPP::StateActive;
    }
    lastChatState_       = XMPP::StateNone;
    sendComposingEvents_ = false;
    isComposing_         = false;
    composingTimer_      = nullptr;
    updateRealJid();
}

void ChatDlg::init()
{
    initUi();
    initActions();
    setShortcuts();

    chatEdit()->installEventFilter(this);
    chatView()->setDialog(this);
    bool isPrivate = account()->groupchats().contains(jid().bare());
    chatView()->setSessionData(false, isPrivate, jid(), jid().full()); // FIXME fix nick updating
#ifdef WEBKIT
    chatView()->setAccount(account());
#else
    chatView()->setMediaOpener(account()->fileSharingDeviceOpener());
#endif
    chatView()->init();

    // seems its useless hack
    // connect(chatView(), SIGNAL(selectionChanged()), SLOT(logSelectionChanged())); //

    // SyntaxHighlighters modify the QTextEdit in a QTimer::singleShot(0, ...) call
    // so we need to install our hooks after it fired for the first time
    QTimer::singleShot(10, this, SLOT(initComposing()));
    connect(this, SIGNAL(composing(bool)), SLOT(updateIsComposing(bool)));

    setAcceptDrops(true);
    updateContact(jid(), true);

    X11WM_CLASS("chat");
    setLooks();

    updatePgp();

    connect(account(), SIGNAL(pgpKeyChanged()), SLOT(updatePgp()));
    connect(account(), SIGNAL(encryptedMessageSent(int, bool, int, const QString &)),
            SLOT(encryptedMessageSent(int, bool, int, const QString &)));
    account()->dialogRegister(this, jid());

    chatView()->setFocusPolicy(Qt::NoFocus);
    chatEdit()->setFocus();
}

ChatDlg::~ChatDlg()
{
    delete delayedMessages;
    account()->dialogUnregister(this);
}

void ChatDlg::initComposing()
{
    highlightersInstalled_ = true;
    initHighlighters();
}

void ChatDlg::doTrackBar()
{
    trackBar_ = false;
    chatView()->doTrackBar();
}

void ChatDlg::initActions()
{
    act_send_ = new QAction(this);
    act_send_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(act_send_);
    connect(act_send_, SIGNAL(triggered()), SLOT(doSend()));

    act_close_ = new QAction(this);
    addAction(act_close_);
    connect(act_close_, SIGNAL(triggered()), SLOT(close()));

    act_hide_ = new QAction(this);
    addAction(act_hide_);
    connect(act_hide_, SIGNAL(triggered()), SLOT(hideTab()));

    act_scrollup_ = new QAction(this);
    addAction(act_scrollup_);
    connect(act_scrollup_, SIGNAL(triggered()), chatView(), SLOT(scrollUp()));

    act_scrolldown_ = new QAction(this);
    addAction(act_scrolldown_);
    connect(act_scrolldown_, SIGNAL(triggered()), chatView(), SLOT(scrollDown()));
}

void ChatDlg::setShortcuts()
{
    act_send_->setShortcuts(ShortcutManager::instance()->shortcuts("chat.send"));
    act_scrollup_->setShortcuts(ShortcutManager::instance()->shortcuts("common.scroll-up"));
    act_scrolldown_->setShortcuts(ShortcutManager::instance()->shortcuts("common.scroll-down"));
    act_hide_->setShortcuts(ShortcutManager::instance()->shortcuts("common.hide"));

    if (!isTabbed()) {
        act_close_->setShortcuts(ShortcutManager::instance()->shortcuts("common.close"));
    } else {
        act_close_->QAction::setShortcuts(QList<QKeySequence>());
    }
}

void ChatDlg::closeEvent(QCloseEvent *e)
{
    if (readyToHide()) {
        e->accept();
    } else {
        e->ignore();
    }
}

/**
 * Runs all the gumph necessary before hiding a chat.
 * (checking new messages, setting the autodelete, cancelling composing etc)
 * \return ChatDlg is ready to be hidden.
 */
bool ChatDlg::readyToHide()
{
    // really lame way of checking if we are encrypting
    if (!chatEdit()->isEnabled()) {
        return false;
    }

    if (keepOpen_) {
        QMessageBox mb(QMessageBox::Information, tr("Warning"),
                       tr("A new chat message was just received.\nDo you still want to close the window?"),
                       QMessageBox::Cancel, this);
        mb.addButton(tr("Close"), QMessageBox::AcceptRole);
        if (mb.exec() == QMessageBox::Cancel) {
            return false;
        }
    }
    keepOpen_ = false; // tabdlg calls readyToHide twice on tabdlg close, only display message once.

    setAttribute(Qt::WA_DeleteOnClose);

    // Reset 'contact is composing' & cancel own composing event
    resetComposing();
    setChatState(XMPP::StateGone);
    if (contactChatState_ == XMPP::StateComposing || contactChatState_ == XMPP::StateInactive) {
        setContactChatState(XMPP::StatePaused);
    }

    if (pending_ > 0) {
        pending_ = 0;
        emit messagesRead(jid());
        invalidateTab();
    }
    doFlash(false);

    chatEdit()->setFocus();
    return true;
}

void ChatDlg::capsChanged(const Jid &j)
{
    if (jid().compare(j, false)) {
        capsChanged();
    }
}

void ChatDlg::capsChanged() { }

void ChatDlg::hideEvent(QHideEvent *e)
{
    if (isMinimized()) {
        resetComposing();
        setChatState(XMPP::StateInactive);
    }
    TabbableWidget::hideEvent(e);
}

void ChatDlg::showEvent(QShowEvent *)
{
    //
}

void ChatDlg::logSelectionChanged()
{
#ifdef Q_OS_MAC
    // A hack to only give the message log focus when text is selected
// seems its already useless. at least copy works w/o this hack
//    if (chatView()->textCursor().hasSelection()) {
//        chatView()->setFocus();
//    }
//    else {
//        chatEdit()->setFocus();
//    }
#endif
}

void ChatDlg::deactivated()
{
    TabbableWidget::deactivated();

    trackBar_ = true;

    setChatState(XMPP::StateInactive);
}

void ChatDlg::activated()
{
    TabbableWidget::activated();

    if (pending_ > 0) {
        pending_ = 0;
        emit messagesRead(jid());
        invalidateTab();
    }
    doFlash(false);

    chatEdit()->setFocus();

    trackBar_ = false;

    setChatState(XMPP::StateActive);
}

void ChatDlg::dropEvent(QDropEvent *event)
{
    FileShareDlg::shareFiles(
        account(), jid(), event->mimeData(),
        [this](const QList<XMPP::Reference> &&rl, const QString &desc) { doFileShare(std::move(rl), desc); }, this);
}

void ChatDlg::dragEnterEvent(QDragEnterEvent *event)
{
    Q_ASSERT(event);
    // bool accept = false;
    if (account()->loggedIn() && event->mimeData()->hasUrls()) {
        const auto &urls = event->mimeData()->urls();
        for (const QUrl &url : urls) {
            if (!url.toLocalFile().isEmpty()) {
                event->accept();
                break;
            }
        }
    }
}

void ChatDlg::setJid(const Jid &j)
{
    if (!j.compare(jid())) {
        account()->dialogUnregister(this);
        TabbableWidget::setJid(j);
        updateRealJid();
        account()->dialogRegister(this, jid());
        updateContact(jid(), false);
    }
}

const QString &ChatDlg::getDisplayName() const { return dispNick_; }

UserStatus ChatDlg::userStatusFor(const Jid &jid, QList<UserListItem *> ul, bool forceEmptyResource)
{
    if (ul.isEmpty())
        return UserStatus();

    UserStatus u;

    u.userListItem = ul.first();
    if (jid.resource().isEmpty() || forceEmptyResource) {
        // use priority
        if (u.userListItem->isAvailable()) {
            const UserResource &r = *u.userListItem->userResourceList().priority();
            u.statusType          = r.status().type();
            u.status              = r.status().status();
            u.priority            = r.status().priority();
            u.publicKeyID         = r.publicKeyID();
        }
    } else {
        // use specific
        UserResourceList::ConstIterator rit = u.userListItem->userResourceList().find(jid.resource());
        if (rit != u.userListItem->userResourceList().constEnd()) {
            u.statusType  = (*rit).status().type();
            u.status      = (*rit).status().status();
            u.priority    = (*rit).status().priority();
            u.publicKeyID = (*rit).publicKeyID();
        }
    }

    if (u.statusType == XMPP::Status::Offline) {
        u.status   = u.userListItem->lastUnavailableStatus().status();
        u.priority = 0;
    }

    return u;
}

void ChatDlg::preloadHistory()
{
    int cnt = PsiOptions::instance()->getOption("options.ui.chat.history.preload-history-size").toInt();
    if (cnt > 0) {
        holdMessages(true);
        if (cnt > 100) // This is limit, just in case.
            cnt = 100;
        EDBHandle *h = new EDBHandle(account()->edb());
        connect(h, SIGNAL(finished()), this, SLOT(getHistory()));
        Jid j = jid();
        if (!account()->findGCContact(j))
            j = jid().bare();
        int start = account()->eventQueue()->count(jid(), false);
        h->get(account()->id(), j, QDateTime(), EDB::Backward, start, cnt);
    }
}

void ChatDlg::getHistory()
{
    EDBHandle *h = qobject_cast<EDBHandle *>(sender());
    if (!h)
        return;

    historyState       = true;
    const EDBResult &r = h->result();
    for (int i = r.count() - 1; i >= 0; --i) {
        const EDBItemPtr &item = r.at(i);
        PsiEvent::Ptr     e    = item->event();
        if (e->type() == PsiEvent::Message) {
            MessageEvent::Ptr me = e.staticCast<MessageEvent>();
            appendMessage(me->message(), me->originLocal());
        }
    }
    delete h;
    holdMessages(false);
}

void ChatDlg::ensureTabbedCorrectly()
{
    TabbableWidget::ensureTabbedCorrectly();
    setShortcuts();
    QList<UserListItem *> ul         = account()->findRelevant(jid());
    UserStatus            userStatus = userStatusFor(jid(), ul, false);
    setTabIcon(PsiIconset::instance()->statusPtr(jid(), userStatus.statusType)->icon());
    if (!isTabbed() && geometryOptionPath().isEmpty()) {
        setGeometryOptionPath(geometryOption);
    }
}

void ChatDlg::updateContact(const Jid &j, bool fromPresence)
{
    if ((j.resource().isEmpty() || j.resource() == QLatin1String("Muc")) && account()->groupchats().contains(j.bare()))
        return;
    // if groupchat, only update if the resource matches
    if (account()->findGCContact(j) && !jid().compare(j)) {
        return;
    }

    if (jid().compare(j, false)) {
        QList<UserListItem *> ul = account()->findRelevant(j);
        if (ul.isEmpty()) {
            qWarning("Trying to update not existing contact");
            return;
        }
        UserStatus userStatus = userStatusFor(jid(), ul, false);

        Jid oldJid = jid();
        updateJidWidget(ul, userStatus.statusType, fromPresence);
        bool jidSwitched = !oldJid.compare(jid(), true);
        if (jidSwitched) {
            userStatus = userStatusFor(jid(), ul, false);
        }

        if (userStatus.statusType == XMPP::Status::Offline)
            contactChatState_ = XMPP::StateNone;

        bool statusWithPriority = PsiOptions::instance()->getOption("options.ui.chat.status-with-priority").toBool();
        bool statusChanged      = false;
        if (jidSwitched || status_ != userStatus.statusType || statusString_ != userStatus.status
            || (statusWithPriority && priority_ != userStatus.priority)) {
            statusChanged = true;
            status_       = userStatus.statusType;
            statusString_ = userStatus.status;
            priority_     = userStatus.priority;
        }

        contactUpdated(userStatus.userListItem, userStatus.statusType, userStatus.status);

        if (userStatus.userListItem) {
            dispNick_ = JIDUtil::nickOrJid(userStatus.userListItem->name(), userStatus.userListItem->jid().full());
            nicksChanged();
            invalidateTab();

            key_ = userStatus.publicKeyID;
            updatePgp();

            if (PsiOptions::instance()->getOption("options.ui.chat.show-status-changes").toBool() && fromPresence
                && statusChanged) {
                dispatchMessage(MessageView::statusMessage(dispNick_, status_, statusString_, priority_));
            }
        }

        // Update capabilities
        capsChanged(jid());

        // Reset 'is composing' event if the status changed
        if (statusChanged && contactChatState_ != XMPP::StateNone) {
            if (contactChatState_ == XMPP::StateComposing || contactChatState_ == XMPP::StateInactive) {
                setContactChatState(XMPP::StatePaused);
            }
        }
    }
}

void ChatDlg::updateJidWidget(const QList<UserListItem *> &ul, int status, bool fromPresence)
{
    Q_UNUSED(ul);
    Q_UNUSED(status);
    Q_UNUSED(fromPresence);
}

void ChatDlg::contactUpdated(UserListItem *u, int status, const QString &statusString)
{
    Q_UNUSED(u);
    Q_UNUSED(status);
    Q_UNUSED(statusString);
}

void ChatDlg::doVoice() { emit aVoice(jid()); }

void ChatDlg::updateAvatar(const Jid &j)
{
    if (j.compare(jid(), false)) {
        updateAvatar();
        chatView()->updateAvatar(j, ChatViewCommon::RemoteParty);
    } else if (j.compare(account()->jid(), false)) {
        chatView()->updateAvatar(j, ChatViewCommon::LocalParty);
    }
}

void ChatDlg::setLooks()
{
    // update the font
    QFont f;
    f.fromString(PsiOptions::instance()->getOption("options.ui.look.font.chat").toString());
    chatView()->setFont(f);
    chatEdit()->setFont(f);

    // update contact info
    status_ = -2; // sick way of making it redraw the status
    updateContact(jid(), false);

    // update the widget icon
#ifndef Q_OS_MAC
    setWindowIcon(IconsetFactory::icon("psi/start-chat").icon());
#endif

    /*QBrush brush;
    brush.setPixmap( QPixmap( LEGOPTS.chatBgImage ) );
    chatView()->setPaper(brush);
    chatView()->setStaticBackground(true);*/

    setWindowOpacity(double(qMax(MINIMUM_OPACITY, PsiOptions::instance()->getOption("options.ui.chat.opacity").toInt()))
                     / 100);
}

void ChatDlg::optionsUpdate()
{
    setLooks();
    setShortcuts();

    if (!isTabbed() && isHidden()) {
        deleteLater();
        return;
    }
}

void ChatDlg::updatePgp() { }

void ChatDlg::doInfo() { emit aInfo(jid()); }

void ChatDlg::doHistory() { emit aHistory(jid()); }

void ChatDlg::doFile() { emit aFile(jid()); }

void ChatDlg::doClear() { chatView()->clear(); }

QString ChatDlg::desiredCaption() const
{
    QString cap = "";

    if (pending_ > 0) {
        cap += "* ";
        if (pending_ > 1) {
            cap += QString("[%1] ").arg(pending_);
        }
    }
    cap += dispNick_;

    if (contactChatState_ == XMPP::StateComposing) {
        cap = tr("%1 (Composing ...)").arg(cap);
    } else if (contactChatState_ == XMPP::StateInactive || contactChatState_ == XMPP::StateGone) {
        cap = tr("%1 (Inactive)").arg(cap);
    }

    return cap;
}

void ChatDlg::invalidateTab() { TabbableWidget::invalidateTab(); }

void ChatDlg::updateRealJid() { realJid_ = account()->realJid(jid()); }

Jid ChatDlg::realJid() const { return realJid_; }

bool ChatDlg::isPgpEncryptionEnabled() const { return false; }

void ChatDlg::doFileShare(const QList<Reference> &&references, const QString &desc)
{
    fileShareReferences_ = std::move(references);
    fileShareDesc_       = desc;
    ChatDlg::doSend(); // FIXME PsiChatDlg does some weird things, so direct call
    fileShareReferences_.clear();
    fileShareDesc_ = QString();
}

void ChatDlg::doSend()
{
    if (!chatEdit()->isEnabled()) {
        return;
    }

    if (chatEdit()->toPlainText().isEmpty() && !fileShareReferences_.count()) {
        return;
    }

    if (chatEdit()->toPlainText() == "/clear") {
        chatEdit()->clear();
        doClear();
        QString line1, line2;
        MiniCommand_Depreciation_Message("/clear", "clear", line1, line2);
        appendSysMsg(line1);
        appendSysMsg(line2);
        return;
    }

    if (!account()->loggedIn()) {
        return;
    }

    if (warnSend_) {
        warnSend_ = false;
        int n     = QMessageBox::information(this, tr("Warning"),
                                         tr("<p>Encryption was recently disabled by the remote contact.  "
                                            "Are you sure you want to send this message without encryption?</p>"),
                                         tr("&Yes"), tr("&No"));
        if (n != 0) {
            return;
        }
    }

    Message m(jid());
    m.setType("chat");
    m.setTimeStamp(QDateTime::currentDateTime());
    if (isPgpEncryptionEnabled()) {
        m.setWasEncrypted(true);
        m.setEncryptionProtocol("Legacy OpenPGP");
    }

    if (fileShareReferences_.count()) {
        m.setReferences(fileShareReferences_);
        m.setBody(fileShareDesc_);
    } else {
        m.setBody(chatEdit()->toPlainText());
        HTMLElement html = chatEdit()->toHTMLElement();
        if (!html.body().isNull())
            m.setHTML(html);
    }

    QString id = account()->client()->genUniqueId();
    m.setId(id); // we need id early for message manipulations in chatview
    if (chatEdit()->isCorrection()) {
        m.setReplaceId(chatEdit()->lastMessageId());
    }
    chatEdit()->setLastMessageId(id);
    chatEdit()->resetCorrection();
    // xep-0184 Message Receipts
    if (PsiOptions::instance()->getOption("options.ui.notifications.request-receipts").toBool()) {
        QStringList sl;
        sl << "urn:xmpp:receipts";
        // FIXME uncomment next lines when all bugs will be fixed
        // if (!jid().resource().isEmpty() || (account()->capsManager()->isEnabled() &&
        //    account()->capsManager()->features(jid()).test(sl))) {
        // if (true) {
        m.setMessageReceipt(ReceiptRequest);
        // rememberPosition(id);
        //}
    }

    m_ = m;
    chatEdit()->appendMessageHistory(m.body());

    // Request events
    if (PsiOptions::instance()->getOption("options.messages.send-composing-events").toBool()) {
        // Only request more events when really necessary
        if (sendComposingEvents_) {
            m.addEvent(ComposingEvent);
        }
        m.setChatState(XMPP::StateActive);
    }

    // Update current state
    setChatState(XMPP::StateActive);

    if (isPgpEncryptionEnabled()) {
        chatEdit()->setEnabled(false);
        transid_ = account()->sendPgpEncryptedMessage(m);
        if (transid_ == -1) {
            chatEdit()->setEnabled(true);
            chatEdit()->setFocus();
            return;
        }
    } else {
        emit aSend(m_);
        doneSend();
    }

    chatEdit()->setFocus();
}

void ChatDlg::doneSend()
{
    historyState = false;
    appendMessage(m_, true);
    disconnect(chatEdit(), SIGNAL(textChanged()), this, SLOT(setComposing()));
    chatEdit()->clear();

    // Reset composing timer
    connect(chatEdit(), SIGNAL(textChanged()), this, SLOT(setComposing()));
    // Reset composing timer
    resetComposing();
}

void ChatDlg::encryptedMessageSent(int x, bool b, int e, const QString &dtext)
{
    Q_UNUSED(e);
    if (transid_ == -1 || transid_ != x) {
        return;
    }
    transid_ = -1;
    if (b) {
        doneSend();
    } else {
        PGPUtil::showDiagnosticText(dtext, dtext);
    }
    chatEdit()->setEnabled(true);
    chatEdit()->setFocus();
}

void ChatDlg::incomingMessage(const Message &m)
{
    historyState = false;
    if (m.body().isEmpty() && m.subject().isEmpty() && m.urlList().isEmpty()) {
        // Event message
        if (m.containsEvent(CancelEvent)) {
            setContactChatState(XMPP::StatePaused);
        } else if (m.containsEvent(ComposingEvent)) {
            setContactChatState(XMPP::StateComposing);
        }

        if (m.chatState() != XMPP::StateNone) {
            setContactChatState(m.chatState());
        }
        if (m.messageReceipt() == ReceiptReceived) {
            chatView()->markReceived(m.messageReceiptId());
        }
    } else {
        // Normal message
        // Check if user requests event messages
        sendComposingEvents_ = m.containsEvent(ComposingEvent);
        if (!m.eventId().isEmpty()) {
            eventId_ = m.eventId();
        }
        if (m.containsEvents() || m.chatState() != XMPP::StateNone) {
            setContactChatState(XMPP::StateActive);
        } else {
            setContactChatState(XMPP::StateNone);
        }
        appendMessage(m, m.carbonDirection() == Message::Sent);
    }
}

void ChatDlg::setPgpEnabled(bool enabled) { Q_UNUSED(enabled); }

QString ChatDlg::whoNick(bool local) const
{
    QString result;

    if (local) {
        result = account()->nick();
    } else {
        result = dispNick_;
    }

    return result;
}

void ChatDlg::appendMessage(const Message &m, bool local)
{
    if (trackBar_)
        doTrackBar();

    // figure out the encryption state
    bool encChanged = false;
    bool encEnabled = false;
    if (!historyState) {
        if (lastWasEncrypted_ != m.wasEncrypted()) {
            encChanged = true;
        }
        lastWasEncrypted_ = m.wasEncrypted();
        encEnabled        = lastWasEncrypted_;
    }

    if (encChanged) {
        chatView()->setEncryptionEnabled(encEnabled);
        QString msg = QString("<icon name=\"psi/cryptoNo\"> ") + tr("Encryption is disabled");
        if (encEnabled) {
            if (!m.encryptionProtocol().isEmpty()) {
                msg = QString("<icon name=\"psi/cryptoYes\"> ")
                    + tr("%1 encryption is enabled").arg(m.encryptionProtocol());
            } else {
                msg = QString("<icon name=\"psi/cryptoYes\"> ") + tr("Encryption is enabled");
            }
        }
        dispatchMessage(MessageView::fromHtml(msg, MessageView::System));
        if (!local) {
            if (!encEnabled) {
                // enable warning
                warnSend_ = true;
                QTimer::singleShot(3000, this, [this]() { warnSend_ = false; });
            }
        }
    }

    if (!m.subject().isEmpty()) {
        MessageView smv = MessageView::subjectMessage(m.subject());
        smv.setSpooled(historyState);
        dispatchMessage(smv);
    }

    MessageView mv(MessageView::Message);

    QString     body = m.body();
    HTMLElement htmlElem;
    if (m.containsHTML())
        htmlElem = m.html();

#ifdef PSI_PLUGINS
    QDomElement html = htmlElem.body();

    PluginManager::instance()->appendingChatMessage(account(), jid().full(), body, html, local);

    if (!html.isNull())
        htmlElem.setBody(html);
#endif

    if (PsiOptions::instance()->getOption("options.html.chat.render").toBool() && !htmlElem.body().isNull()
        && !htmlElem.body().firstChild().isNull()) {
        mv.setHtml(htmlElem.toString("span"));
    } else {
        mv.setPlainText(body);
    }
    mv.setMessageId(m.id());
    mv.setLocal(local);
    mv.setNick(whoNick(local));
    mv.setUserId(local ? account()->jid().full()
                       : jid().full()); // theoretically, this can be inferred from the chat dialog properties
    mv.setDateTime(m.timeStamp());
    mv.setSpooled(historyState);
    mv.setAwaitingReceipt(local && m.messageReceipt() == ReceiptRequest);
    mv.setReplaceId(m.replaceId());
    mv.setCarbonDirection(m.carbonDirection());
    account()->psi()->fileSharingManager()->fillMessageView(mv, m, account());

    dispatchMessage(mv);

    if (!m.urlList().isEmpty()) {
        UrlList                urls = m.urlList();
        QMap<QString, QString> urlsMap;
        for (const Url &u : urls) {
            urlsMap.insert(u.url(), u.desc());
        }
        // Some XMPP clients send links to HTTP uploaded files both in body and in jabber:x:oob.
        // It's convenient to show only body if OOB data brings no additional information.
        if (!(urlsMap.size() == 1 && urlsMap.contains(body) && urlsMap.value(body).isEmpty())) {
            MessageView umv = MessageView::urlsMessage(urlsMap);
            umv.setSpooled(historyState);
            dispatchMessage(umv);
        }
    }
    emit messageAppended(body, chatView()->textWidget());
}

void ChatDlg::holdMessages(bool hold)
{
    if (hold) {
        if (!delayedMessages)
            delayedMessages = new QList<MessageView>();
    } else if (delayedMessages) {
        for (const MessageView &mv : qAsConst(*delayedMessages)) {
            if (mv.isSpooled())
                displayMessage(mv);
        }
        for (const MessageView &mv : qAsConst(*delayedMessages)) {
            if (!mv.isSpooled())
                displayMessage(mv);
        }
        delete delayedMessages;
        delayedMessages = nullptr;
    }
}

void ChatDlg::dispatchMessage(const MessageView &mv)
{
    if (delayedMessages)
        delayedMessages->append(mv);
    else
        displayMessage(mv);
}

void ChatDlg::displayMessage(const MessageView &mv)
{
    chatView()->dispatchMessage(mv);

    // if we're not active, notify the user by changing the title
    MessageView::Type type = mv.type();
    if (type != MessageView::System && type != MessageView::Status && !mv.isSpooled() && !isActiveTab()
        && mv.carbonDirection() != Message::Sent) {
        ++pending_;
        invalidateTab();
        if (PsiOptions::instance()->getOption("options.ui.flash-windows").toBool()) {
            doFlash(true);
        }
        if (PsiOptions::instance()->getOption("options.ui.chat.raise-chat-windows-on-new-messages").toBool()) {
            if (isTabbed()) {
                TabDlg *tabSet = getManagingTabDlg();
                if (PsiOptions::instance()->getOption("options.ui.chat.switch-tab-on-new-messages").toBool()
                    || !tabSet->isActiveWindow())
                    tabSet->selectTab(this);
                ::bringToFront(tabSet, false);
            } else {
                ::bringToFront(this, false);
            }
        }
    }
    // else {
    //    messagesRead(jid());
    //}

    if (!mv.isLocal()) {
        keepOpen_ = true;
        QTimer::singleShot(1000, this, [this]() { keepOpen_ = false; });
    }
}

void ChatDlg::updateIsComposing(bool b) { setChatState(b ? XMPP::StateComposing : XMPP::StatePaused); }

void ChatDlg::setChatState(ChatState state)
{
    if (PsiOptions::instance()->getOption("options.messages.send-composing-events").toBool()
        && (sendComposingEvents_ || (contactChatState_ != XMPP::StateNone))) {
        // Don't send to offline resource
        QList<UserListItem *> ul = account()->findRelevant(jid());
        if (ul.isEmpty()) {
            sendComposingEvents_ = false;
            lastChatState_       = XMPP::StateNone;
            return;
        }

        UserListItem *u = ul.first();
        if (!u->isAvailable()) {
            sendComposingEvents_ = false;
            lastChatState_       = XMPP::StateNone;
            return;
        }

        // Transform to more privacy-enabled chat states if necessary
        if (!PsiOptions::instance()->getOption("options.messages.send-inactivity-events").toBool()
            && (state == XMPP::StateGone || state == XMPP::StateInactive)) {
            state = XMPP::StatePaused;
        }

        if (lastChatState_ == XMPP::StateNone
            && (state != XMPP::StateActive && state != XMPP::StateComposing && state != XMPP::StateGone)) {
            // this isn't a valid transition, so don't send it, and don't update laststate
            return;
        }

        if (lastChatState_ == XMPP::StateGone && state == XMPP::StateInactive) {
            // same here
            return;
        }

        // Check if we should send a message
        if (state == lastChatState_ || (lastChatState_ == XMPP::StateActive && state == XMPP::StatePaused)) {
            lastChatState_ = state;
            return;
        }

        // Build event message
        if (!PsiOptions::instance()->getOption("options.messages.dont-send-composing-events").toBool()) {
            Message m(jid());
            if (sendComposingEvents_) {
                m.setEventId(eventId_);
                if (state == XMPP::StateComposing) {
                    m.addEvent(ComposingEvent);
                } else if (lastChatState_ == XMPP::StateComposing) {
                    m.addEvent(CancelEvent);
                }
            }
            if (contactChatState_ != XMPP::StateNone) {
                if ((state == XMPP::StateInactive && lastChatState_ == XMPP::StateComposing)
                    || (state == XMPP::StateComposing && lastChatState_ == XMPP::StateInactive)) {
                    // First go to the paused state
                    Message tm(jid());
                    m.setType("chat");
                    m.setChatState(XMPP::StatePaused);
                    if (account()->isAvailable()) {
                        account()->dj_sendMessage(m, false);
                    }
                }
                m.setChatState(state);
            }

            // Send event message
            if (m.containsEvents() || m.chatState() != XMPP::StateNone) {
                m.setType("chat");
                if (account()->isAvailable()) {
                    account()->dj_sendMessage(m, false);
                }
            }
        }

        // Save last state
        if (lastChatState_ != XMPP::StateGone || state == XMPP::StateActive)
            lastChatState_ = state;
    }
}

void ChatDlg::setContactChatState(ChatState state)
{
    contactChatState_ = state;
    if (state == XMPP::StateGone) {
        appendSysMsg(tr("%1 ended the conversation").arg(TextUtil::escape(dispNick_)));
    }

    invalidateTab();
}

bool ChatDlg::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        keyPressEvent(static_cast<QKeyEvent *>(event));
        if (event->isAccepted())
            return true;
    }

    if (chatView()->handleCopyEvent(obj, event, chatEdit()))
        return true;

    return QWidget::eventFilter(obj, event);
}

void ChatDlg::addEmoticon(QString text)
{
    if (!isActiveTab())
        return;

    PsiRichText::addEmoticon(chatEdit(), text);
}

/**
 * Records that the user is composing
 */
void ChatDlg::setComposing()
{
    if (!composingTimer_) {
        /* User (re)starts composing */
        composingTimer_ = new QTimer(this);
        connect(composingTimer_, SIGNAL(timeout()), SLOT(checkComposing()));
        composingTimer_->start(2000); // FIXME: magic number
        emit composing(true);
    }
    isComposing_ = true;
}

/**
 * Checks if the user is still composing
 */
void ChatDlg::checkComposing()
{
    if (!isComposing_) {
        // User stopped composing
        composingTimer_->deleteLater();
        composingTimer_ = nullptr;
        emit composing(false);
    }
    isComposing_ = false; // Reset composing
}

void ChatDlg::resetComposing()
{
    if (composingTimer_) {
        delete composingTimer_;
        composingTimer_ = nullptr;
        isComposing_    = false;
    }
}

PsiAccount *ChatDlg::account() const { return TabbableWidget::account(); }

void ChatDlg::setInputText(const QString &text)
{
    // chatEdit()->setPlainText(text); because undo/redo history is reset when the function been called.
    chatEdit()->selectAll();
    chatEdit()->document()->blockSignals(true); // to disable things like auto-capitalizer
    chatEdit()->insertPlainText(text);
    chatEdit()->document()->blockSignals(false);

    chatEdit()->moveCursor(QTextCursor::End);
}

void ChatDlg::nicksChanged()
{
    // this function is intended to be reimplemented in subclasses
}

void ChatDlg::initHighlighters()
{
    if (highlightersInstalled_) {
        connect(chatEdit(), SIGNAL(textChanged()), this, SLOT(setComposing()));
    }
}

void ChatDlg::chatEditCreated()
{
    chatEdit()->setDialog(this);
    initHighlighters();
}

TabbableWidget::State ChatDlg::state() const
{
    if (unreadMessageCount())
        return TabbableWidget::State::Highlighted;
    switch (contactChatState_) {
    case XMPP::StateComposing:
        return TabbableWidget::State::Composing;
    case XMPP::StateInactive:
    case XMPP::StateGone:
        return TabbableWidget::State::Inactive;
    default:
        return TabbableWidget::State::None;
    }
}

int ChatDlg::unreadMessageCount() const { return pending_; }
