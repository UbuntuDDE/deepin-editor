/* -*- Mode: C++; indent-tabs-mode: nil; tab-width: 4 -*-
 * -*- coding: utf-8 -*-
 *
 * Copyright (C) 2011 ~ 2018 Deepin, Inc.
 *
 * Author:     Wang Yong <wangyong@deepin.com>
 * Maintainer: Rekols    <rekols@foxmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "../common/utils.h"
#include "../widgets/window.h"
#include "../widgets/bottombar.h"
#include "dtextedit.h"
#include "leftareaoftextedit.h"
#include "editwrapper.h"
#include "showflodcodewidget.h"
#include "deletebackcommond.h"
#include "replaceallcommond.h"
#include "insertblockbytextcommond.h"

#include <KF5/KSyntaxHighlighting/definition.h>
#include <KF5/KSyntaxHighlighting/syntaxhighlighter.h>
#include <KF5/KSyntaxHighlighting/theme.h>

#include <QAbstractTextDocumentLayout>
#include <QTextDocumentFragment>
#include <QInputMethodEvent>
#include <DDesktopServices>
#include <QApplication>
#include <DSettingsGroup>
#include <DSettingsOption>
#include <DSettings>
#include <QClipboard>
#include <QFileInfo>
#include <QDebug>
#include <QPainter>
#include <QScroller>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTextBlock>
#include <QMimeData>
#include <QTimer>
#include <QGesture>
#include <QStyleHints>
#include <DSysInfo>

#include <private/qguiapplication_p.h>
#include <qpa/qplatformtheme.h>
#include <QtSvg/qsvgrenderer.h>


TextEdit::TextEdit(QWidget *parent)
    : DPlainTextEdit(parent),
      m_wrapper(nullptr)
{
    setUndoRedoEnabled(false);
    //???????????????
    m_pUndoStack = new QUndoStack(this);

    m_nLines = 0;
    m_nBookMarkHoverLine = -1;
    m_bIsFileOpen = false;
    m_qstrCommitString = "";
    m_bIsShortCut = false;
    m_bIsInputMethod = false;
    m_isSelectAll = false;
    m_touchTapDistance = QGuiApplicationPrivate::platformTheme()->themeHint(QPlatformTheme::TouchDoubleTapDistance).toInt();
    m_fontLineNumberArea.setFamily("SourceHanSansSC-Normal");

    m_pLeftAreaWidget = new LeftAreaTextEdit(this);
    m_pLeftAreaWidget->m_pFlodArea->setAttribute(Qt::WA_Hover, true); //??????????????????
    m_pLeftAreaWidget->m_pBookMarkArea->setAttribute(Qt::WA_Hover, true);
    m_pLeftAreaWidget->m_pFlodArea->installEventFilter(this);
    m_pLeftAreaWidget->m_pBookMarkArea->installEventFilter(this);
    m_foldCodeShow = new ShowFlodCodeWidget(this);
    m_foldCodeShow->setVisible(false);

    viewport()->installEventFilter(this);
    viewport()->setCursor(Qt::IBeamCursor);

    // Don't draw frame around editor widget.
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setContentsMargins(0, 0, 0, 0);
    setEditPalette(palette().foreground().color().name(), palette().foreground().color().name());

    grabGesture(Qt::TapGesture);
    grabGesture(Qt::TapAndHoldGesture);
    grabGesture(Qt::SwipeGesture);
    grabGesture(Qt::PanGesture);
    grabGesture(Qt::PinchGesture);

    // Init widgets.
    //????????????????????????????????????????????? ????????????
    connect(this->verticalScrollBar(), &QScrollBar::valueChanged, this, [&](){
        if(m_isSelectAll)
            this->selectTextInView();

        this->updateLeftAreaWidget();
    });
    connect(this, &QPlainTextEdit::textChanged, this, &TextEdit::updateLeftAreaWidget);
    connect(this, &QPlainTextEdit::textChanged, this, [this]() {
        this->m_wrapper->UpdateBottomBarWordCnt(this->characterCount());
    });

    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &TextEdit::cursorPositionChanged);
    connect(this, &QPlainTextEdit::selectionChanged, this, &TextEdit::onSelectionArea);

    connect(document(), &QTextDocument::contentsChange, this, &TextEdit::updateMark);
    connect(document(), &QTextDocument::contentsChange, this, &TextEdit::checkBookmarkLineMove);

    connect(m_pUndoStack, &QUndoStack::canRedoChanged, this, [this](bool) {
        bool isModified = this->m_wrapper->isTemFile() | (m_pUndoStack->canUndo() || m_pUndoStack->index() != m_lastSaveIndex);
        this->m_wrapper->window()->updateModifyStatus(m_sFilePath, isModified);
        this->m_wrapper->OnUpdateHighlighter();
    });

    connect(m_pUndoStack, &QUndoStack::canUndoChanged, this, [this](bool canUndo) {
        bool isModified = this->m_wrapper->isTemFile() | (canUndo || m_pUndoStack->index() != m_lastSaveIndex);
        this->m_wrapper->window()->updateModifyStatus(m_sFilePath, isModified);
        this->m_wrapper->OnUpdateHighlighter();
    });

    QDBusConnection dbus = QDBusConnection::sessionBus();
    dbus.systemBus().connect("com.deepin.daemon.Gesture",
                             "/com/deepin/daemon/Gesture", "com.deepin.daemon.Gesture",
                             "Event",
                             this, SLOT(fingerZoom(QString, QString, int)));


    //?????????????????????
    initRightClickedMenu();

    // Init scroll animation.
    m_scrollAnimation = new QPropertyAnimation(verticalScrollBar(), "value");
    m_scrollAnimation->setEasingCurve(QEasingCurve::InOutExpo);
    m_scrollAnimation->setDuration(300);

    m_cursorMode = Insert;

    connect(m_scrollAnimation, &QPropertyAnimation::finished, this, &TextEdit::handleScrollFinish, Qt::QueuedConnection);

    // Monitor cursor mark status to update in line number area.
    connect(this, &TextEdit::cursorMarkChanged, this, &TextEdit::handleCursorMarkChanged);

    // configure content area
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, &TextEdit::adjustScrollbarMargins, Qt::QueuedConnection);
    // Don't blink the cursor when selecting text
    // Recover blink when not selecting text.
    connect(this, &TextEdit::selectionChanged, this, [ = ] {
        if (textCursor().hasSelection())
        {
            hideCursorBlink();
        } else
        {
            showCursorBlink();
        }
    });
}

TextEdit::~TextEdit()
{
    writeHistoryRecord();
    if (m_scrollAnimation != nullptr) {
        if (m_scrollAnimation->state() != QAbstractAnimation::Stopped) {
            m_scrollAnimation->stop();
        }
        delete m_scrollAnimation;
        m_scrollAnimation = nullptr;
    }
    if (m_colorMarkMenu != nullptr) {
        delete m_colorMarkMenu;
        m_colorMarkMenu = nullptr;
    }
    if (m_convertCaseMenu != nullptr) {
        delete m_convertCaseMenu;
        m_convertCaseMenu = nullptr;
    }
    if (m_rightMenu != nullptr) {
        delete m_rightMenu;
        m_rightMenu = nullptr;
    }
}

void TextEdit::insertTextEx(QTextCursor cursor, QString text)
{
    QUndoCommand *pInsertStack = new InsertTextUndoCommand(cursor, text);
    m_pUndoStack->push(pInsertStack);
    ensureCursorVisible();
}

void TextEdit::deleteSelectTextEx(QTextCursor cursor)
{
    if (cursor.hasSelection()) {
        QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(cursor);
        m_pUndoStack->push(pDeleteStack);
    }
}

void TextEdit::deleteSelectTextEx(QTextCursor cursor,QString text,bool currLine)
{
    QUndoCommand *pDeleteStack = new DeleteTextUndoCommand2(cursor,text,this,currLine);
    m_pUndoStack->push(pDeleteStack);
}

void TextEdit::deleteTextEx(QTextCursor cursor)
{
    QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(cursor);
    m_pUndoStack->push(pDeleteStack);
}

void TextEdit::insertSelectTextEx(QTextCursor cursor, QString text)
{
//    if (cursor.hasSelection()) {
//        deleteTextEx(cursor);
//    }
    QUndoCommand *pInsertStack = new InsertTextUndoCommand(cursor, text);
    m_pUndoStack->push(pInsertStack);
    ensureCursorVisible();
}

void TextEdit::insertColumnEditTextEx(QString text)
{
    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    for (int i = 0; i < m_altModSelections.size(); i++) {
        if (m_altModSelections[i].cursor.hasSelection()) deleteTextEx(m_altModSelections[i].cursor);
    }
    QUndoCommand *pInsertStack = new InsertTextUndoCommand(m_altModSelections, text);
    m_pUndoStack->push(pInsertStack);
    ensureCursorVisible();
}

void TextEdit::initRightClickedMenu()
{
    // Init menu.
    m_rightMenu = new DMenu();
    m_undoAction = new QAction(tr("Undo"), this);
    m_redoAction = new QAction(tr("Redo"), this);
    m_cutAction = new QAction(tr("Cut"), this);
    m_copyAction = new QAction(tr("Copy"), this);
    m_pasteAction = new QAction(tr("Paste"), this);
    m_deleteAction = new QAction(tr("Delete"), this);
    m_selectAllAction = new QAction(tr("Select All"), this);
    m_findAction = new QAction(tr("Find"), this);
    m_replaceAction = new QAction(tr("Replace"), this);
    m_jumpLineAction = new QAction(tr("Go to Line"), this);
    m_enableReadOnlyModeAction = new QAction(tr("Turn on Read-Only mode"), this);
    m_disableReadOnlyModeAction = new QAction(tr("Turn off Read-Only mode"), this);
    m_fullscreenAction = new QAction(tr("Fullscreen"), this);
    m_exitFullscreenAction = new QAction(tr("Exit fullscreen"), this);
    m_openInFileManagerAction = new QAction(tr("Display in file manager"), this);
    m_toggleCommentAction = new QAction(tr("Add Comment"), this);
    m_voiceReadingAction = new QAction(tr("Text to Speech"), this);
    m_stopReadingAction = new QAction(tr("Stop reading"), this);
    m_dictationAction = new QAction(tr("Speech to Text"), this);
    m_translateAction = new QAction(tr("Translate"), this);
    m_columnEditAction = new QAction(tr("Column Mode"), this);
    m_addBookMarkAction = new QAction(tr("Add bookmark"), this);
    m_cancelBookMarkAction = new QAction(tr("Remove Bookmark"), this);
    m_preBookMarkAction = new QAction(tr("Previous bookmark"), this);
    m_nextBookMarkAction = new QAction(tr("Next bookmark"), this);
    m_clearBookMarkAction = new QAction(tr("Remove All Bookmarks"), this);
    m_flodAllLevel = new QAction(tr("Fold All"), this);
    m_flodCurrentLevel = new QAction(tr("Fold Current Level"), this);
    m_unflodAllLevel = new QAction(tr("Unfold All"), this);
    m_unflodCurrentLevel = new QAction(tr("Unfold Current Level"), this);

    //yanyuhan
    //?????????????????????/?????????????????????????????????????????????????????????;
    //??????????????????????????????????????????????????????????????????????????????????????????????????????????????????;
    m_colorMarkMenu = new DMenu(tr("Color Mark"));

    // ???????????????Menu?????????????????????
    m_colorMarkMenu->installEventFilter(this);

    m_cancleMarkAllLine = new QAction(tr("Clear All Marks"), this);
    m_cancleLastMark = new QAction(tr("Clear Last Mark"), this);


    //??????????????????????????????????????????
    ColorSelectWdg *pColorsSelectWdg = new ColorSelectWdg(QString(), this);
    connect(pColorsSelectWdg, &ColorSelectWdg::sigColorSelected, this, [this](bool bSelected, QColor color) {
        isMarkCurrentLine(bSelected, color.name());
        renderAllSelections();
        m_colorMarkMenu->close();
        m_rightMenu->close();//????????????????????????????????????????????????????????????????????????????????????????????????
    });

    m_actionColorStyles = new QWidgetAction(this);
    m_actionColorStyles->setDefaultWidget(pColorsSelectWdg);

    m_markCurrentAct = new QAction(tr("Mark"), this);
    connect(m_markCurrentAct, &QAction::triggered, this, [this, pColorsSelectWdg]() {
        isMarkCurrentLine(true, pColorsSelectWdg->getDefaultColor().name());
        renderAllSelections();
    });

    //??????????????????????????????????????????
    ColorSelectWdg *pColorsAllSelectWdg = new ColorSelectWdg(QString(), this);
    connect(pColorsAllSelectWdg, &ColorSelectWdg::sigColorSelected, this, [this](bool bSelected, QColor color) {
        isMarkAllLine(bSelected, color.name());
        renderAllSelections();
        m_colorMarkMenu->close();
        m_rightMenu->close();//????????????????????????????????????????????????????????????????????????????????????????????????
    });
    m_actionAllColorStyles = new QWidgetAction(this);
    m_actionAllColorStyles->setDefaultWidget(pColorsAllSelectWdg);

    m_markAllAct = new QAction(tr("Mark All"), this);
    connect(m_markAllAct, &QAction::triggered, this, [this, pColorsAllSelectWdg]() {
        m_strMarkAllLineColorName = pColorsAllSelectWdg->getDefaultColor().name();
        isMarkAllLine(true, pColorsAllSelectWdg->getDefaultColor().name());
        renderAllSelections();
    });

    m_addComment = new QAction(tr("Add Comment"), this);
    m_cancelComment = new QAction(tr("Remove Comment"), this);

    connect(m_rightMenu, &DMenu::aboutToHide, this, &TextEdit::removeHighlightWordUnderCursor);
    connect(m_undoAction, &QAction::triggered, m_pUndoStack, &QUndoStack::undo);
    connect(m_redoAction, &QAction::triggered, m_pUndoStack, &QUndoStack::redo);

    #if 0
    connect(m_cutAction, &QAction::triggered, this, [this]() {
        //???????????????????????????
        if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
            QString data;
            for (auto sel : m_altModSelections) data.append(sel.cursor.selectedText());
            //???????????????
            for (int i = 0; i < m_altModSelections.size(); i++) {
                if (m_altModSelections[i].cursor.hasSelection()) {
                    QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(m_altModSelections[i].cursor);
                    m_pUndoStack->push(pDeleteStack);
                }
            }
            //??????????????????
            QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
            clipboard->setText(data);
        } else {
            QTextCursor cursor = textCursor();
            //????????????????????????
            if (cursor.hasSelection()) {
                QString data = cursor.selectedText();
                QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(cursor);
                m_pUndoStack->push(pDeleteStack);
                QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
                clipboard->setText(data);
            }
        }
        unsetMark();
    });

    connect(m_copyAction, &QAction::triggered, this, [this]() {
        if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
            QString data;
            for (auto sel : m_altModSelections) {
                data.append(sel.cursor.selectedText());
            }
            QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
            clipboard->setText(data);
        } else {
            QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
            if (textCursor().hasSelection()) {
                clipboard->setText(textCursor().selection().toPlainText());
                tryUnsetMark();
            } else {
                clipboard->setText(m_highlightWordCacheCursor.selectedText());
            }
        }
    });


    connect(m_pasteAction, &QAction::triggered, this, [this]() {
        //???????????????????????????????????????
        const QClipboard *clipboard = QApplication::clipboard(); //?????????????????????

        if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
            insertColumnEditTextEx(clipboard->text());
        } else {
            QTextCursor cursor = textCursor();
            insertSelectTextEx(textCursor(), clipboard->text());
        }

        unsetMark();
    });

#endif

    connect(m_cutAction, &QAction::triggered, this, [this]() {
        this->cut();
    });
    connect(m_copyAction, &QAction::triggered, this, [this]() {
       this->copy();
    });

    connect(m_pasteAction, &QAction::triggered, this, [this]() {
        this->paste();
    });



    connect(m_deleteAction, &QAction::triggered, this, [this]() {
        if(m_isSelectAll)
            QPlainTextEdit::selectAll();

        if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
            for (int i = 0; i < m_altModSelections.size(); i++) {
                if (m_altModSelections[i].cursor.hasSelection()) {
                    QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(m_altModSelections[i].cursor);
                    m_pUndoStack->push(pDeleteStack);
                }
            }
        } else {
            if (textCursor().hasSelection()) {
                QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(textCursor());
                m_pUndoStack->push(pDeleteStack);
            } else {
                setTextCursor(m_highlightWordCacheCursor);
            }
        }
    });


    connect(m_selectAllAction, &QAction::triggered, this, [ = ] {
#if 0
        //2021-5-25:setSelectAll()??????
        if (m_wrapper->getFileLoading())
        {
            return;
        }
        m_bIsAltMod = false;
        selectAll();
#endif
        setSelectAll();
    });

    connect(m_findAction, &QAction::triggered, this, &TextEdit::clickFindAction);
    connect(m_replaceAction, &QAction::triggered, this, &TextEdit::clickReplaceAction);
    connect(m_jumpLineAction, &QAction::triggered, this, &TextEdit::clickJumpLineAction);
    connect(m_fullscreenAction, &QAction::triggered, this, &TextEdit::clickFullscreenAction);
    connect(m_exitFullscreenAction, &QAction::triggered, this, &TextEdit::clickFullscreenAction);
    connect(m_enableReadOnlyModeAction, &QAction::triggered, this, &TextEdit::toggleReadOnlyMode);
    connect(m_disableReadOnlyModeAction, &QAction::triggered, this, &TextEdit::toggleReadOnlyMode);

    connect(m_openInFileManagerAction, &QAction::triggered, this, [this]() {
        DDesktopServices::showFileItem(this->getTruePath());
    });

    connect(m_addComment, &QAction::triggered, this, [ = ] {
        toggleComment(true);
    });
    connect(m_cancelComment, &QAction::triggered, this, [ = ] {
        toggleComment(false);
    });
    connect(m_voiceReadingAction, &QAction::triggered, this, [this]() {
        QProcess::startDetached("dbus-send  --print-reply --dest=com.iflytek.aiassistant /aiassistant/deepinmain com.iflytek.aiassistant.mainWindow.TextToSpeech");
        emit signal_readingPath();
    });
    connect(m_stopReadingAction, &QAction::triggered, this, []() {
        QProcess::startDetached("dbus-send  --print-reply --dest=com.iflytek.aiassistant /aiassistant/tts com.iflytek.aiassistant.tts.stopTTSDirectly");
    });
    connect(m_dictationAction, &QAction::triggered, this, []() {
        QProcess::startDetached("dbus-send  --print-reply --dest=com.iflytek.aiassistant /aiassistant/deepinmain com.iflytek.aiassistant.mainWindow.SpeechToText");
    });
    connect(m_translateAction, &QAction::triggered, this, &TextEdit::slot_translate);
    connect(m_columnEditAction, &QAction::triggered, this, [this] {
        //toggleComment(true); todo
        DMessageManager::instance()->sendMessage(this, QIcon(":/images/ok.svg"), tr("Press ALT and click lines to edit in column mode"));
    });

    connect(m_addBookMarkAction, &QAction::triggered, this, &TextEdit::addOrDeleteBookMark);

    connect(m_cancelBookMarkAction, &QAction::triggered, this, &TextEdit::addOrDeleteBookMark);

    connect(m_preBookMarkAction, &QAction::triggered, this, [this]() {

        int line = getLineFromPoint(m_mouseClickPos);
        int index = m_listBookmark.indexOf(line);

        if (index == 0) {
            jumpToLine(m_listBookmark.last(), true);
        } else {
            jumpToLine(m_listBookmark.value(index - 1), true);
        }

    });
    connect(m_nextBookMarkAction, &QAction::triggered, this, [this]() {
        int line = getLineFromPoint(m_mouseClickPos);
        int index = m_listBookmark.indexOf(line);

        if (index == -1 && !m_listBookmark.isEmpty()) {
            jumpToLine(m_listBookmark.last(), false);
        }

        if (index == m_listBookmark.count() - 1) {
            jumpToLine(m_listBookmark.first(), false);
        } else {
            jumpToLine(m_listBookmark.value(index + 1), false);
        }
    });

    connect(m_clearBookMarkAction, &QAction::triggered, this, [this]() {
        m_listBookmark.clear();
        qDebug() << "ClearBookMark:" << m_listBookmark;
        m_pLeftAreaWidget->m_pBookMarkArea->update();
    });

    connect(m_flodAllLevel, &QAction::triggered, this, [this] {
        flodOrUnflodAllLevel(true);
    });
    connect(m_unflodAllLevel, &QAction::triggered, this, [this] {
        flodOrUnflodAllLevel(false);
    });
    connect(m_flodCurrentLevel, &QAction::triggered, this, [this] {
        flodOrUnflodCurrentLevel(true);
    });
    connect(m_unflodCurrentLevel, &QAction::triggered, this, [this] {
        flodOrUnflodCurrentLevel(false);
    });

    connect(m_cancleMarkAllLine, &QAction::triggered, this, [this] {
        isMarkAllLine(false);
    });
    connect(m_cancleLastMark, &QAction::triggered, this, [this] {
        cancelLastMark();
    });


    // Init convert case sub menu.
    m_haveWordUnderCursor = false;
    m_convertCaseMenu = new DMenu(tr("Change Case"));
    m_upcaseAction = new QAction(tr("Upper Case"), this);
    m_downcaseAction = new QAction(tr("Lower Case"), this);
    m_capitalizeAction = new QAction(tr("Capitalize"), this);

    m_convertCaseMenu->addAction(m_upcaseAction);
    m_convertCaseMenu->addAction(m_downcaseAction);
    m_convertCaseMenu->addAction(m_capitalizeAction);

    connect(m_upcaseAction, &QAction::triggered, this, &TextEdit::upcaseWord);
    connect(m_downcaseAction, &QAction::triggered, this, &TextEdit::downcaseWord);
    connect(m_capitalizeAction, &QAction::triggered, this, &TextEdit::capitalizeWord);

    m_canUndo = false;
    m_canRedo = false;

    connect(this, &TextEdit::undoAvailable, this, [ = ](bool undoIsAvailable) {
        m_canUndo = undoIsAvailable;
    });
    connect(this, &TextEdit::redoAvailable, this, [ = ](bool redoIsAvailable) {
        m_canRedo = redoIsAvailable;
    });

}

void TextEdit::popRightMenu(QPoint pos)
{
    //???????????????????????????
    if (m_rightMenu != nullptr) {
        delete m_rightMenu;
        m_rightMenu = nullptr;
    }
    m_rightMenu = new DMenu;

    m_rightMenu->clear();
    QTextCursor selectionCursor = textCursor();
    selectionCursor.movePosition(QTextCursor::StartOfBlock);
    selectionCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    QString text = selectionCursor.selectedText();

    // init base.
    bool isBlankLine = text.trimmed().isEmpty();

    bool isAddUndoRedo = false;
    if (m_pUndoStack->canUndo() && m_bReadOnlyPermission == false && m_readOnlyMode == false) {
        m_rightMenu->addAction(m_undoAction);
        isAddUndoRedo = true;
    }

    if (m_pUndoStack->canRedo() && m_bReadOnlyPermission == false && m_readOnlyMode == false) {
        m_rightMenu->addAction(m_redoAction);
        isAddUndoRedo = true;
    }

    if (isAddUndoRedo) m_rightMenu->addSeparator();

    if (textCursor().hasSelection() || m_hasColumnSelection) {
        if (m_bReadOnlyPermission == false && m_readOnlyMode == false) {
            m_rightMenu->addAction(m_cutAction);
        }
        m_rightMenu->addAction(m_copyAction);
    }


    if (canPaste()) {
        if (m_bReadOnlyPermission == false && m_readOnlyMode == false) {
            m_rightMenu->addAction(m_pasteAction);
        }
    }

    if (textCursor().hasSelection() || m_hasColumnSelection) {
        if (m_bReadOnlyPermission == false && m_readOnlyMode == false) {
            m_rightMenu->addAction(m_deleteAction);
        }

    }

    if (!document()->isEmpty()) {
        m_rightMenu->addAction(m_selectAllAction);
    }

    m_rightMenu->addSeparator();

    if (!document()->isEmpty()) {
        m_rightMenu->addAction(m_findAction);
        if (m_bReadOnlyPermission == false && m_readOnlyMode == false) {
            m_rightMenu->addAction(m_replaceAction);
        }
        m_rightMenu->addAction(m_jumpLineAction);
        m_rightMenu->addSeparator();
    }

    if (textCursor().hasSelection()) {
        if (m_bReadOnlyPermission == false && m_readOnlyMode == false) {
            m_rightMenu->addMenu(m_convertCaseMenu);
        }
    } else {
        m_convertCaseMenu->hide();
    }

    // intelligent judge whether to support comments.
    const auto def = m_repository.definitionForFileName(QFileInfo(m_sFilePath).fileName());
    if (characterCount() &&
            (textCursor().hasSelection() || !isBlankLine) &&
            !def.filePath().isEmpty()) {

        // ??????vCard????????????(.vcf)??????????????????????????????????????????
        if (def.name() != "Markdown" && def.name().contains("vCard") == false) {
            m_rightMenu->addAction(m_addComment);
            m_rightMenu->addAction(m_cancelComment);
        }
    }

    if (m_bReadOnlyPermission || m_readOnlyMode) {
        m_addComment->setEnabled(false);
        m_cancelComment->setEnabled(false);
    } else {
        m_addComment->setEnabled(true);
        m_cancelComment->setEnabled(true);
    }

    m_rightMenu->addSeparator();
    if (m_bReadOnlyPermission == false) {
        if (m_readOnlyMode) {
            m_rightMenu->addAction(m_disableReadOnlyModeAction);
        } else {
            m_rightMenu->addAction(m_enableReadOnlyModeAction);
        }
    }

    m_rightMenu->addAction(m_openInFileManagerAction);
    m_rightMenu->addSeparator();
    if (static_cast<Window *>(this->window())->isFullScreen()) {
        m_rightMenu->addAction(m_exitFullscreenAction);
    } else {
        m_rightMenu->addAction(m_fullscreenAction);
    }

    /* ?????????/?????????/????????????????????????????????????????????? */
    /* ????????????????????????com.iflytek.aiassistant???????????????????????????????????????????????????????????? */
    /*if ((DSysInfo::uosEditionType() == DSysInfo::UosEdition::UosProfessional) ||
        (DSysInfo::uosEditionType() == DSysInfo::UosEdition::UosHome) ||
        (DSysInfo::uosEditionType() == DSysInfo::UosEdition::UosEducation)) {*/

    if (m_wrapper->window()->isRegisteredFflytekAiassistant()) {
        bool stopReadingState = false;
        QDBusMessage stopReadingMsg = QDBusMessage::createMethodCall("com.iflytek.aiassistant",
                                                                     "/aiassistant/tts",
                                                                     "com.iflytek.aiassistant.tts",
                                                                     "isTTSInWorking");

        QDBusReply<bool> stopReadingStateRet = QDBusConnection::sessionBus().call(stopReadingMsg, QDBus::BlockWithGui);
        if (stopReadingStateRet.isValid()) {
            stopReadingState = stopReadingStateRet.value();
        }
        if (!stopReadingState) {
            m_rightMenu->addAction(m_voiceReadingAction);
            m_voiceReadingAction->setEnabled(false);
        } else {
            m_rightMenu->removeAction(m_voiceReadingAction);
            m_rightMenu->addAction(m_stopReadingAction);
        }
        bool voiceReadingState = false;
        QDBusMessage voiceReadingMsg = QDBusMessage::createMethodCall("com.iflytek.aiassistant",
                                                                      "/aiassistant/tts",
                                                                      "com.iflytek.aiassistant.tts",
                                                                      "getTTSEnable");

        QDBusReply<bool> voiceReadingStateRet = QDBusConnection::sessionBus().call(voiceReadingMsg, QDBus::BlockWithGui);
        if (voiceReadingStateRet.isValid()) {
            voiceReadingState = voiceReadingStateRet.value();
        }
        if ((textCursor().hasSelection() && voiceReadingState) || m_hasColumnSelection) {
            m_voiceReadingAction->setEnabled(true);
        }

        m_rightMenu->addAction(m_dictationAction);
        bool dictationState = false;
        QDBusMessage dictationMsg = QDBusMessage::createMethodCall("com.iflytek.aiassistant",
                                                                   "/aiassistant/iat",
                                                                   "com.iflytek.aiassistant.iat",
                                                                   "getIatEnable");

        QDBusReply<bool> dictationStateRet = QDBusConnection::sessionBus().call(dictationMsg, QDBus::BlockWithGui);
        if (dictationStateRet.isValid()) {
            dictationState = dictationStateRet.value();
        }
        m_dictationAction->setEnabled(dictationState);
        if (m_bReadOnlyPermission || m_readOnlyMode) {
            m_dictationAction->setEnabled(false);
        }

        m_rightMenu->addAction(m_translateAction);
        m_translateAction->setEnabled(false);
        bool translateState = false;
        QDBusMessage translateReadingMsg = QDBusMessage::createMethodCall("com.iflytek.aiassistant",
                                                                          "/aiassistant/trans",
                                                                          "com.iflytek.aiassistant.trans",
                                                                          "getTransEnable");

        QDBusReply<bool> translateStateRet = QDBusConnection::sessionBus().call(translateReadingMsg, QDBus::BlockWithGui);
        if (translateStateRet.isValid()) {
            translateState = translateStateRet.value();
        }
        if ((textCursor().hasSelection() && translateState) || m_hasColumnSelection) {
            m_translateAction->setEnabled(translateState);
        }
    }


    if (!this->document()->isEmpty()) {

        m_colorMarkMenu->clear();
        // ?????? tab order list
        m_MarkColorMenuTabOrder.clear();

        // ?????? Mark Color Menu Item
        m_colorMarkMenu->addAction(m_markCurrentAct);
        m_colorMarkMenu->addAction(m_actionColorStyles);
        // ????????? Mark Color Menu Item ?????? Tab Order
        // QPair<QAction*, bool> , bool??????tab???????????????????????????focus
        m_MarkColorMenuTabOrder.append(QPair<QAction*,bool>(m_markCurrentAct, true));
        m_MarkColorMenuTabOrder.append(QPair<QAction*,bool>(m_actionColorStyles, false));

        // ?????? Mark Color Menu Item
        m_colorMarkMenu->addSeparator();
        m_colorMarkMenu->addAction(m_markAllAct);
        m_colorMarkMenu->addAction(m_actionAllColorStyles);
        m_colorMarkMenu->addSeparator();
        // ????????? Mark Color Menu Item ?????? Tab Order
        m_MarkColorMenuTabOrder.append(QPair<QAction*,bool>(m_markAllAct, true));
        m_MarkColorMenuTabOrder.append(QPair<QAction*,bool>(m_actionAllColorStyles, false));

        if (m_markOperations.size() > 0) {
             // ?????? Mark Color Menu Item
            m_colorMarkMenu->addAction(m_cancleLastMark);
            m_colorMarkMenu->addSeparator();
            m_colorMarkMenu->addAction(m_cancleMarkAllLine);
            // ????????? Mark Color Menu Item ?????? Tab Order
            m_MarkColorMenuTabOrder.append(QPair<QAction*,bool>(m_cancleLastMark, true));
            m_MarkColorMenuTabOrder.append(QPair<QAction*,bool>(m_cancleMarkAllLine, true));
        }

        m_rightMenu->addSeparator();
        if (m_bReadOnlyPermission == false && m_readOnlyMode == false) {
            m_rightMenu->addAction(m_columnEditAction);
        }
        m_rightMenu->addMenu(m_colorMarkMenu);
    }

    QPoint pos1 = cursorRect().bottomRight();
    //?????????????????? ????????????????????????????????? ?????????????????????????????????????????????????????????????????????
    if (pos1.y() > this->rect().height()) {
        pos1.setY((this->rect().height()) / 2);
        pos1.setX((this->rect().width()) / 2);
    }
    if (pos.isNull()) {
        m_rightMenu->exec(mapToGlobal(pos1));
    } else {
        m_rightMenu->exec(pos);
    }
}

void TextEdit::setWrapper(EditWrapper *w)
{
    m_wrapper = w;
}

EditWrapper *TextEdit::getWrapper()
{
    return m_wrapper;
}


int TextEdit::getCurrentLine()
{
    return textCursor().blockNumber() + 1;
}

int TextEdit::getCurrentColumn()
{
    return textCursor().columnNumber();
}

int TextEdit::getPosition()
{
    return textCursor().position();
}

int TextEdit::getScrollOffset()
{
    return verticalScrollBar()->value();
}

void TextEdit::forwardChar()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::NextCharacter);
    }
}

void TextEdit::backwardChar()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::PreviousCharacter);
    }
}

void TextEdit::forwardWord()
{
    QTextCursor cursor = textCursor();

    if (m_cursorMark) {
        cursor.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
    } else {
        cursor.movePosition(QTextCursor::NextWord, QTextCursor::MoveAnchor);
    }

    setTextCursor(cursor);
}

void TextEdit::backwardWord()
{
    QTextCursor cursor = textCursor();

    if (m_cursorMark) {
        // cursor.setPosition(getPrevWordPosition(cursor, QTextCursor::KeepAnchor), QTextCursor::KeepAnchor);
        cursor.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor);
    } else {
        // cursor.setPosition(getPrevWordPosition(cursor, QTextCursor::MoveAnchor), QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::PreviousWord, QTextCursor::MoveAnchor);
    }

    setTextCursor(cursor);
}

void TextEdit::forwardPair()
{
    // Record cursor and seleciton position before move cursor.
    int actionStartPos = textCursor().position();
    int selectionStartPos = textCursor().selectionStart();
    int selectionEndPos = textCursor().selectionEnd();

    // Because find always search start from selection end position.
    // So we need clear selection to make search start from cursor.
    QTextCursor removeSelectionCursor = textCursor();
    removeSelectionCursor.clearSelection();
    setTextCursor(removeSelectionCursor);

    // Start search.
    if (find(QRegExp("[\"'>)}]"))) {
        int findPos = textCursor().position();

        QTextCursor cursor = textCursor();
        auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

        if (actionStartPos == selectionStartPos) {
            cursor.setPosition(selectionEndPos, QTextCursor::MoveAnchor);
            cursor.setPosition(findPos, moveMode);
        } else {
            cursor.setPosition(selectionStartPos, QTextCursor::MoveAnchor);
            cursor.setPosition(findPos, moveMode);
        }

        setTextCursor(cursor);
    }
}

void TextEdit::backwardPair()
{
    // Record cursor and seleciton position before move cursor.
    int actionStartPos = textCursor().position();
    int selectionStartPos = textCursor().selectionStart();
    int selectionEndPos = textCursor().selectionEnd();

    // Because find always search start from selection end position.
    // So we need clear selection to make search start from cursor.
    QTextCursor removeSelectionCursor = textCursor();
    removeSelectionCursor.clearSelection();
    setTextCursor(removeSelectionCursor);

    QTextDocument::FindFlags options;
    options |= QTextDocument::FindBackward;

    if (find(QRegExp("[\"'<({]"), options)) {
        QTextCursor cursor = textCursor();
        auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

        cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor);

        int findPos = cursor.position();

        if (actionStartPos == selectionStartPos) {
            cursor.setPosition(selectionEndPos, QTextCursor::MoveAnchor);
            cursor.setPosition(findPos, moveMode);
            setTextCursor(cursor);
        } else {
            cursor.setPosition(selectionStartPos, QTextCursor::MoveAnchor);
            cursor.setPosition(findPos, moveMode);
            setTextCursor(cursor);
        }
    }
}

int TextEdit::blockCount() const
{
    return document()->blockCount();
}

int TextEdit::characterCount() const
{
    return document()->characterCount();
}

QTextBlock TextEdit::firstVisibleBlock()
{
    return document()->findBlockByLineNumber(getFirstVisibleBlockId());
}

void TextEdit::moveToStart()
{
    verticalScrollBar()->setValue(0);
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Start, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::Start);
    }
}

void TextEdit::moveToEnd()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::End);
    }
}

void TextEdit::moveToStartOfLine()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::StartOfBlock);
    }
}

void TextEdit::moveToEndOfLine()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::EndOfBlock);
    }
}

void TextEdit::moveToLineIndentation()
{
    // Init cursor and move type.
    QTextCursor cursor = textCursor();
    auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

    // Get line start position.
    cursor.movePosition(QTextCursor::StartOfBlock, moveMode);
    int startColumn = cursor.columnNumber();

    // Get line end position.
    cursor.movePosition(QTextCursor::EndOfBlock, moveMode);
    int endColumn = cursor.columnNumber();

    // Move to line start first.
    cursor.movePosition(QTextCursor::StartOfBlock, moveMode);
    int nStartPos = cursor.position();

    if (nStartPos - 1 < 0) {
        nStartPos = 0;
        //cursor.setPosition(nStartPos, QTextCursor::MoveAnchor);
        cursor.setPosition(nStartPos + 1, moveMode);
    } else {
        cursor.setPosition(nStartPos - 1, moveMode);
        cursor.setPosition(nStartPos, QTextCursor::KeepAnchor);
    }

//    cursor.movePosition(QTextCursor::PreviousCharacter,QTextCursor::KeepAnchor);

    //qDebug () << "currentChar" << *cursor.selection().toPlainText().data() << "llll" << toPlainText().at(std::max(cursor.position() - 1, 0));
    // Move to first non-blank char of line.
    int column = startColumn;
    while (column < endColumn) {
        QChar currentChar = *cursor.selection().toPlainText().data();
        //QChar currentChar = toPlainText().at(std::max(cursor.position() - 1, 0));

        if (!currentChar.isSpace()) {
            //cursor.setPosition(cursor.position(), QTextCursor::MoveAnchor);
            cursor.setPosition(cursor.position() - 1, moveMode);
            break;
        } else {
            //cursor.setPosition(cursor.position(), QTextCursor::MoveAnchor);
            cursor.setPosition(cursor.position() + 1, moveMode);
        }

        column++;
    }

    cursor.clearSelection();
    setTextCursor(cursor);
}

void TextEdit::nextLine()
{
    if (!characterCount())
        return;

    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::Down);
    }

    if (m_wrapper != nullptr) {
        m_wrapper->OnUpdateHighlighter();
        if ((m_wrapper->window()->findBarIsVisiable() || m_wrapper->window()->replaceBarIsVisiable()) &&
                (QString::compare(m_wrapper->window()->getKeywordForSearchAll(), m_wrapper->window()->getKeywordForSearch(), Qt::CaseInsensitive) == 0)) {
            highlightKeywordInView(m_wrapper->window()->getKeywordForSearchAll());
        }

        markAllKeywordInView();
    }
}

void TextEdit::prevLine()
{
    if (!characterCount())
        return;

    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Up, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    } else {
        moveCursorNoBlink(QTextCursor::Up);
    }

    if (m_wrapper != nullptr) {
        m_wrapper->OnUpdateHighlighter();
        if ((m_wrapper->window()->findBarIsVisiable() || m_wrapper->window()->replaceBarIsVisiable()) &&
                (QString::compare(m_wrapper->window()->getKeywordForSearchAll(), m_wrapper->window()->getKeywordForSearch(), Qt::CaseInsensitive) == 0)) {
            highlightKeywordInView(m_wrapper->window()->getKeywordForSearchAll());
        }

        markAllKeywordInView();
    }
}

void TextEdit::moveCursorNoBlink(QTextCursor::MoveOperation operation, QTextCursor::MoveMode mode)
{
    // Function moveCursorNoBlink will blink cursor when move cursor.
    // But function movePosition won't, so we use movePosition to avoid that cursor link when moving cursor.
    QTextCursor cursor = textCursor();
    cursor.movePosition(operation, mode);
    setTextCursor(cursor);
}

void TextEdit::jumpToLine(int line, bool keepLineAtCenter)
{
    QTextCursor cursor(document()->findBlockByNumber(line - 1)); // line - 1 because line number starts from 0
    //verticalScrollBar()->setValue(fontMetrics().height() * line - height());
    // Update cursor.
    setTextCursor(cursor);

    if (keepLineAtCenter) {
        keepCurrentLineAtCenter();
    }
    m_pLeftAreaWidget->m_pLineNumberArea->update();
}

void TextEdit::newline()
{
    // Stop mark if mark is set.
    tryUnsetMark();

    QTextCursor cursor = textCursor();
    cursor.insertText("\n");
    setTextCursor(cursor);
}

void TextEdit::openNewlineAbove()
{
    // Stop mark if mark is set.
    tryUnsetMark();

    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
    cursor.insertText("\n");
    cursor.movePosition(QTextCursor::Up, QTextCursor::MoveAnchor);

    setTextCursor(cursor);
}

void TextEdit::openNewlineBelow()
{
    // Stop mark if mark is set.
    tryUnsetMark();

    moveCursorNoBlink(QTextCursor::EndOfBlock);
    textCursor().insertText("\n");
}

void TextEdit::moveLineDownUp(bool up)
{
    QTextCursor cursor = textCursor();
    QTextCursor move = cursor;
    bool hasSelection = cursor.hasSelection();

    // this opens folded items instead of destroying them
    move.setVisualNavigation(false);
    move.beginEditBlock();

    if (hasSelection) {
        move.setPosition(cursor.selectionStart());
        move.movePosition(QTextCursor::StartOfBlock);
        move.setPosition(cursor.selectionEnd(), QTextCursor::KeepAnchor);
        move.movePosition(move.atBlockStart() ? QTextCursor::Left : QTextCursor::EndOfBlock,
                          QTextCursor::KeepAnchor);
    } else {
        move.movePosition(QTextCursor::StartOfBlock);
        move.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    }
    const QString text = move.selectedText();

    move.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
    move.removeSelectedText();

    if (up) {
        move.movePosition(QTextCursor::PreviousBlock);
        move.insertBlock();
        move.movePosition(QTextCursor::Left);
    } else {
        move.movePosition(QTextCursor::EndOfBlock);
        if (move.atBlockStart()) { // empty block
            move.movePosition(QTextCursor::NextBlock);
            move.insertBlock();
            move.movePosition(QTextCursor::Left);
        } else {
            move.insertBlock();
        }
    }

    int start = move.position();
    move.clearSelection();
    move.insertText(text);
    int end = move.position();

    if (hasSelection) {
        move.setPosition(end);
        move.setPosition(start, QTextCursor::KeepAnchor);
    } else {
        move.setPosition(start);
    }

    move.endEditBlock();
    setTextCursor(move);
}

void TextEdit::scrollLineUp()
{
    QScrollBar *scrollbar = verticalScrollBar();

    scrollbar->setValue(scrollbar->value() - 1);

    if (cursorRect().y() > rect().height() - fontMetrics().height()) {
        auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Up, moveMode);
        setTextCursor(cursor);
    }
}

void TextEdit::scrollLineDown()
{
    QScrollBar *scrollbar = verticalScrollBar();

    scrollbar->setValue(scrollbar->value() + 1);

    if (cursorRect().y() < 0) {
        auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Down, moveMode);
        setTextCursor(cursor);
    }
}

void TextEdit::scrollUp()
{
    QScrollBar *scrollbar = verticalScrollBar();
    scrollbar->triggerAction(QAbstractSlider::SliderPageStepSub);

    m_pLeftAreaWidget->m_pLineNumberArea->update();
    //m_pLeftAreaWidget->m_pFlodArea->update();
    //m_pLeftAreaWidget->m_pBookMarkArea->update();

    if (verticalScrollBar()->maximum() > 0) {
        auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;
        QPoint startPoint = QPointF(0, fontMetrics().height()).toPoint();
        QTextCursor cur = cursorForPosition(startPoint);
        QTextCursor cursor = textCursor();
        cursor.setPosition(cur.position(), moveMode);
        setTextCursor(cursor);
    }

    if (m_wrapper != nullptr) {
        m_wrapper->OnUpdateHighlighter();
        if ((m_wrapper->window()->findBarIsVisiable() || m_wrapper->window()->replaceBarIsVisiable()) &&
                (QString::compare(m_wrapper->window()->getKeywordForSearchAll(), m_wrapper->window()->getKeywordForSearch(), Qt::CaseInsensitive) == 0)) {
            highlightKeywordInView(m_wrapper->window()->getKeywordForSearchAll());
        }

        markAllKeywordInView();
    }
}

void TextEdit::scrollDown()
{
    QScrollBar *scrollbar = verticalScrollBar();
    scrollbar->triggerAction(QAbstractSlider::SliderPageStepAdd);

    m_pLeftAreaWidget->m_pLineNumberArea->update();
    //m_pLeftAreaWidget->m_pFlodArea->update();
    //m_pLeftAreaWidget->m_pBookMarkArea->update();

    if (verticalScrollBar()->maximum() > 0) {
        auto moveMode = m_cursorMark ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;
        QPoint endPoint = QPointF(0, height() - fontMetrics().height()).toPoint();
        QTextCursor cur = cursorForPosition(endPoint);
        QTextCursor cursor = textCursor();
        cursor.setPosition(cur.position(), moveMode);
        setTextCursor(cursor);
    }

    if (m_wrapper != nullptr) {
        m_wrapper->OnUpdateHighlighter();
        if ((m_wrapper->window()->findBarIsVisiable() || m_wrapper->window()->replaceBarIsVisiable()) &&
                (QString::compare(m_wrapper->window()->getKeywordForSearchAll(), m_wrapper->window()->getKeywordForSearch(), Qt::CaseInsensitive) == 0)) {
            highlightKeywordInView(m_wrapper->window()->getKeywordForSearchAll());
        }

        markAllKeywordInView();
    }
}

void TextEdit::duplicateLine()
{
    if (textCursor().hasSelection()) {
        bool cursorAtSelectionStart = (textCursor().position() == textCursor().selectionStart());

        // Rember current line's column number.
        int column = textCursor().columnNumber();

        int startPos = textCursor().selectionStart();
        int endPos = textCursor().selectionEnd();

        // Expand selection to lines bound.
        QTextCursor startCursor = textCursor();
        startCursor.setPosition(startPos, QTextCursor::MoveAnchor);
        startCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);

        QTextCursor endCursor = textCursor();
        endCursor.setPosition(endPos, QTextCursor::MoveAnchor);
        endCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        QTextCursor cursor = textCursor();
        cursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
        cursor.setPosition(endCursor.position(), QTextCursor::KeepAnchor);

        setTextCursor(cursor);

        // Get selection lines content.
        QString selectionLines = cursor.selectedText();

        // Duplicate copy lines.
        if (cursorAtSelectionStart) {
            // Insert duplicate lines.
            cursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
            cursor.insertText("\n");
            cursor.movePosition(QTextCursor::Up, QTextCursor::MoveAnchor);
            cursor.insertText(selectionLines);

            // Restore cursor's column.
            cursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column);
        } else {
            // Insert duplicate lines.
            cursor.setPosition(endCursor.position(), QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor);
            cursor.insertText(selectionLines);
            cursor.insertText("\n");

            // Restore cursor's column.
            cursor.movePosition(QTextCursor::Up, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column);
        }

        // Update cursor.
        setTextCursor(cursor);

        // Clean mark flag anyway.
        unsetMark();
    } else {
        // Rember current line's column number.
        int column = textCursor().columnNumber();

        // Get current line's content.
        QTextCursor cursor(textCursor().block());
        cursor.movePosition(QTextCursor::StartOfBlock);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QString text = cursor.selectedText();

        // Copy current line.
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);
        cursor.insertText("\n");
        cursor.insertText(text);

        // Restore cursor's column.
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column);

        // Update cursor.
        setTextCursor(cursor);
    }
}

void TextEdit::copyLines()
{
    // Record current cursor and build copy cursor.
    QTextCursor currentCursor = textCursor();
    QTextCursor copyCursor = textCursor();

    if (textCursor().hasSelection()) {
        // Sort selection bound cursors.
        int startPos = textCursor().anchor();
        int endPos = textCursor().position();

        if (startPos > endPos) {
            std::swap(startPos, endPos);
        }

        // Selectoin multi-lines.
        QTextCursor startCursor = textCursor();
        startCursor.setPosition(startPos, QTextCursor::MoveAnchor);
        startCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);

        QTextCursor endCursor = textCursor();
        endCursor.setPosition(endPos, QTextCursor::MoveAnchor);
        endCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        copyCursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
        copyCursor.setPosition(endCursor.position(), QTextCursor::KeepAnchor);

        popupNotify(tr("Selected line(s) copied"));
    } else {
        // Selection current line.
        copyCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        copyCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);

        popupNotify(tr("Current line copied"));
    }

    // Copy lines to system clipboard.
    setTextCursor(copyCursor);
    copySelectedText();

    // Reset cursor before copy lines.
    copyCursor.setPosition(currentCursor.position(), QTextCursor::MoveAnchor);
    setTextCursor(copyCursor);
}

#if 0
//2021-05-19??????????????????????????????????????????
void TextEdit::cutlines()
{
    // Record current cursor and build copy cursor.
    QTextCursor currentCursor = textCursor();
    QTextCursor copyCursor = textCursor();

    if (textCursor().hasSelection()) {
        // Sort selection bound cursors.
        int startPos = textCursor().anchor();
        int endPos = textCursor().position();

        if (startPos > endPos) {
            std::swap(startPos, endPos);
        }

        // Selectoin multi-lines.
        QTextCursor startCursor = textCursor();
        startCursor.setPosition(startPos, QTextCursor::MoveAnchor);
        startCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);

        QTextCursor endCursor = textCursor();
        endCursor.setPosition(endPos, QTextCursor::MoveAnchor);
        endCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        copyCursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
        copyCursor.setPosition(endCursor.position(), QTextCursor::KeepAnchor);

        popupNotify(tr("Selected line(s) clipped"));
    } else {
        // Selection current line.
        copyCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        copyCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);

        popupNotify(tr("Current line clipped"));
    }
    // Add the cut operation to the undo stack
    QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(copyCursor);
    m_pUndoStack->push(pDeleteStack);
    // Copy lines to system clipboard.
    setTextCursor(copyCursor);
    cutSelectedText();

    // Reset cursor before copy lines.
    copyCursor.setPosition(currentCursor.position(), QTextCursor::MoveAnchor);
    setTextCursor(copyCursor);
    // Setting text has been modified
    this->m_wrapper->window()->updateModifyStatus(m_sFilePath, true);
}
#endif

void TextEdit::cutlines()
{
    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    if (textCursor().hasSelection() || m_bIsAltMod){
        this->cut();
        popupNotify(tr("Selected line(s) clipped"));
    }
    else { 
        auto cursor = textCursor();
        cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        if (cursor.hasSelection()) {
            QString data = cursor.selectedText();
            QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(cursor);
            m_pUndoStack->push(pDeleteStack);
            QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
            clipboard->setText(data);

            popupNotify(tr("Current line clipped"));
        }

    }
}

void TextEdit::joinLines()
{
    if (blockCount() == getCurrentLine())
    {return ;}
    if (textCursor().hasSelection()) {
        // Get selection bound.
        int startPos = textCursor().anchor();
        int endPos = textCursor().position();

        if (startPos > endPos) {
            std::swap(startPos, endPos);
        }

        // Expand selection to multi-lines bound.
        QTextCursor startCursor = textCursor();
        startCursor.setPosition(startPos, QTextCursor::MoveAnchor);
        startCursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);

        QTextCursor endCursor = textCursor();
        endCursor.setPosition(endPos, QTextCursor::MoveAnchor);
        endCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        // Select multi-lines.
        QTextCursor cursor = textCursor();
        cursor.setPosition(startCursor.position(), QTextCursor::MoveAnchor);
        cursor.setPosition(endCursor.position(), QTextCursor::KeepAnchor);

        // Remove selected lines.
        QString selectedLines = cursor.selectedText();
        cursor.removeSelectedText();

        // Insert line with join actoin.
        // Because function `selectedText' will use Unicode char U+2029 instead \n,
        // so we need replace Unicode char U+2029, not replace char '\n'.
        cursor.insertText(selectedLines.replace(QChar(0x2029), " "));

        setTextCursor(cursor);
    } else {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);
        cursor.insertText(" ");
        cursor.deleteChar();
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

        setTextCursor(cursor);
    }

    tryUnsetMark();
}

void TextEdit::killLine()
{
    if (tryUnsetMark()) {
        return;
    }

    // Remove selection content if has selection.
    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    if (textCursor().hasSelection()) {
        // textCursor().removeSelectedText();
	//deleteSelectTextEx(textCursor());
        deleteSelectTextEx(textCursor(),textCursor().selectedText(),false);
    } else {
        // Get current line content.
        QTextCursor selectionCursor = textCursor();
        selectionCursor.movePosition(QTextCursor::StartOfBlock);
        selectionCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QString text = selectionCursor.selectedText();
        QTextCursor cursor = textCursor();
        deleteSelectTextEx(cursor,text,false);

        #if 0
        bool isEmptyLine = (text.size() == 0);
        bool isBlankLine = (text.trimmed().size() == 0);
        // Join next line if current line is empty or cursor at end of line.
        if (isEmptyLine || textCursor().atBlockEnd()) {
            //fixed bug 80435  ut002764  ????????????????????????????????????????????????
            //cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            deleteSelectTextEx(cursor);
        }
        // Kill whole line if current line is blank line.
        else if (isBlankLine && textCursor().atBlockStart()) {
            cursor.movePosition(QTextCursor::StartOfBlock);
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            //cursor.removeSelectedText();
            //cursor.deleteChar();
            deleteSelectTextEx(cursor);
        }
        // Otherwise kill rest content of line.
        else {
            cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            //cursor.removeSelectedText();
            deleteSelectTextEx(cursor);
        }
        // Update cursor.
        // setTextCursor(cursor);
        #endif
    }
}

void TextEdit::killCurrentLine()
{
    if (tryUnsetMark()) {
        return;
    }

    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    QTextCursor cursor = textCursor();
    //cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
    //cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    //cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    //??????????????????????????????????????????????????????????????????????????????????????????????????????
    //QString text = cursor.selectedText();
    //bool isBlankLine = text.trimmed().size() == 0;
    //cursor.removeSelectedText();
    //deleteSelectTextEx(cursor);
    deleteSelectTextEx(cursor,cursor.selectedText(),true);
}

void TextEdit::killBackwardWord()
{
    tryUnsetMark();

    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    if (textCursor().hasSelection()) {
        //textCursor().removeSelectedText();
    } else {
        //  QTextCursor cursor = textCursor();
        //  cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        //  cursor.setPosition(getPrevWordPosition(cursor, QTextCursor::KeepAnchor), QTextCursor::KeepAnchor);
        //  cursor.removeSelectedText();
        //  setTextCursor(cursor);

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor);
        //cursor.removeSelectedText();
        //setTextCursor(cursor);
        deleteSelectTextEx(cursor);
    }
}

void TextEdit::killForwardWord()
{
    tryUnsetMark();

    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    if (textCursor().hasSelection()) {
        //textCursor().removeSelectedText();
    } else {
        //  QTextCursor cursor = textCursor();
        //  cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        //  cursor.setPosition(getNextWordPosition(cursor, QTextCursor::KeepAnchor), QTextCursor::KeepAnchor);
        //  cursor.removeSelectedText();
        //  setTextCursor(cursor);

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
        //cursor.removeSelectedText();
        //setTextCursor(cursor);
        deleteSelectTextEx(cursor);
    }
}

void TextEdit::indentText()
{
    // Stop mark if mark is set.
    tryUnsetMark();
    hideCursorBlink();

    QTextCursor cursor = textCursor();

    if (cursor.hasSelection()) {
        QTextBlock block = document()->findBlock(cursor.selectionStart());
        QTextBlock end = document()->findBlock(cursor.selectionEnd()).next();

        cursor.beginEditBlock();

        while (block != end) {
            QString speaces(m_tabSpaceNumber, ' ');
            cursor.setPosition(block.position());
            cursor.insertText(speaces);
            block = block.next();
        }

        cursor.endEditBlock();
    } else {
        cursor.beginEditBlock();

        int indent = m_tabSpaceNumber - (cursor.positionInBlock() % m_tabSpaceNumber);
        QString spaces(indent, ' ');
        cursor.insertText(spaces);

        cursor.endEditBlock();
    }

    showCursorBlink();
}

void TextEdit::unindentText()
{
    // Stop mark if mark is set.
    tryUnsetMark();
    hideCursorBlink();

    QTextCursor cursor = textCursor();
    QTextBlock block;
    QTextBlock end;

    if (cursor.hasSelection()) {
        block = document()->findBlock(cursor.selectionStart());
        end = document()->findBlock(cursor.selectionEnd()).next();
    } else {
        block = cursor.block();
        end = block.next();
    }

    cursor.beginEditBlock();

    while (block != end) {
        cursor.setPosition(block.position());

        if (document()->characterAt(cursor.position()) == '\t') {
            cursor.deleteChar();
        } else {
            int pos = 0;

            while (document()->characterAt(cursor.position()) == ' ' &&
                    pos < m_tabSpaceNumber) {
                pos++;
                cursor.deleteChar();
            }
        }

        block = block.next();
    }

    cursor.endEditBlock();
    showCursorBlink();
}

void TextEdit::setTabSpaceNumber(int number)
{
    m_tabSpaceNumber = number;
    updateFont();
    //updateLineNumber();
    updateLeftAreaWidget();
}

void TextEdit::upcaseWord()
{
    tryUnsetMark();
    convertWordCase(UPPER);
}

void TextEdit::downcaseWord()
{
    tryUnsetMark();
    convertWordCase(LOWER);
}

void TextEdit::capitalizeWord()
{
    tryUnsetMark();

    convertWordCase(CAPITALIZE);
}

void TextEdit::transposeChar()
{
    tryUnsetMark();

    QTextCursor cursor = textCursor();
    cursor.clearSelection();

    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QString nextChar = cursor.selectedText();
    cursor.removeSelectedText();

    cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
    QString prevChar = cursor.selectedText();
    cursor.removeSelectedText();

    cursor.insertText(nextChar);
    cursor.insertText(prevChar);

    if (!nextChar.isEmpty()) {
        cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::MoveAnchor);
    }

    setTextCursor(cursor);
}

void TextEdit::handleCursorMarkChanged(bool mark, QTextCursor cursor)
{
    if (mark) {
        m_markStartLine = cursor.blockNumber() + 1;
    } else {
        m_markStartLine = -1;
    }

    m_pLeftAreaWidget->m_pLineNumberArea->update();
    m_pLeftAreaWidget->m_pBookMarkArea->update();
}

void TextEdit::convertWordCase(ConvertCase convertCase)
{
#if 0
    if (textCursor().hasSelection()) {
        QString text = textCursor().selectedText();

        if (convertCase == UPPER) {
            textCursor().insertText(text.toUpper());
        } else if (convertCase == LOWER) {
            textCursor().insertText(text.toLower());
        } else {
            textCursor().insertText(capitalizeText(text));
        }
    } else {
        QTextCursor cursor;

        // Move cursor to mouse position first. if have word under mouse pointer.
        if (m_haveWordUnderCursor) {
            setTextCursor(m_wordUnderPointerCursor);
        }

        cursor = textCursor();
        cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        cursor.setPosition(getNextWordPosition(cursor, QTextCursor::KeepAnchor), QTextCursor::KeepAnchor);

        QString text = cursor.selectedText();
        if (convertCase == UPPER) {
            cursor.insertText(text.toUpper());
        } else if (convertCase == LOWER) {
            cursor.insertText(text.toLower());
        } else {
            cursor.insertText(capitalizeText(text));
        }

        setTextCursor(cursor);

        m_haveWordUnderCursor = false;
    }
#endif

    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    if (textCursor().hasSelection()) {
        QString text = textCursor().selectedText();
        if (convertCase == UPPER) {
            text = text.toUpper();
        } else if (convertCase == LOWER) {
            text = text.toLower();
        } else {
            text = capitalizeText(text);
        }

        // ??????????????????????????????????????????????????????????????????
        if(text != textCursor().selectedText()) {
            InsertTextUndoCommand* insertCommond = new InsertTextUndoCommand(textCursor(),text);
            m_pUndoStack->push(insertCommond);
        }
    }
    else{
        QTextCursor cursor;

        // Move cursor to mouse position first. if have word under mouse pointer.
        if (m_haveWordUnderCursor) {
            setTextCursor(m_wordUnderPointerCursor);
        }

        cursor = textCursor();
        cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        cursor.setPosition(getNextWordPosition(cursor, QTextCursor::KeepAnchor), QTextCursor::KeepAnchor);

        QString text = cursor.selectedText();
        if (convertCase == UPPER) {
            text = text.toUpper();
        } else if (convertCase == LOWER) {
            text = text.toLower();
        } else {
            text = capitalizeText(text);
        }

        InsertTextUndoCommand* insertCommond = new InsertTextUndoCommand(cursor,text);
        m_pUndoStack->push(insertCommond);

        setTextCursor(cursor);

        m_haveWordUnderCursor = false;
    }

}

QString TextEdit::capitalizeText(QString text)
{
    QString newText = text.toLower();
    QChar currentChar;
    QChar nextChar;
    if (!newText.at(0).isSpace()) {
        newText.replace(0, 1, newText.at(0).toUpper());
    }

    for (int i = 0; i < newText.size(); i++) {
        currentChar = newText.at(i);
        if (i + 1 < newText.size())
            nextChar = newText.at(i + 1);
//        if (!currentChar.isSpace() && !m_wordSepartors.contains(QString(currentChar))&&nextChar.isSpace()) {
//            newText.replace(i, 1, currentChar.toUpper());
//            //break;
//        }
        if (currentChar.isSpace() && !nextChar.isSpace()) {
            newText.replace(i + 1, 1, nextChar.toUpper());
        }
    }

    return newText;
}

void TextEdit::keepCurrentLineAtCenter()
{
    QScrollBar *scrollbar = verticalScrollBar();

    int currentLine = cursorRect().top() / cursorRect().height();
    int halfEditorLines = rect().height() / 2 / cursorRect().height();
    scrollbar->setValue(scrollbar->value() + currentLine - halfEditorLines);
}

void TextEdit::scrollToLine(int scrollOffset, int row, int column)
{
    // Save cursor postion.
    m_restoreRow = row;
    m_restoreColumn = column;

    // Start scroll animation.
    m_scrollAnimation->setStartValue(verticalScrollBar()->value());
    m_scrollAnimation->setEndValue(scrollOffset);
    m_scrollAnimation->start();
}

void TextEdit::setLineWrapMode(bool enable)
{
    QTextCursor cursor = textCursor();
    int nJumpLine = textCursor().blockNumber() + 1;
    this->setWordWrapMode(QTextOption::WrapAnywhere);
    QPlainTextEdit::setLineWrapMode(enable ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    m_pLeftAreaWidget->m_pLineNumberArea->update();
    m_pLeftAreaWidget->m_pFlodArea->update();
    m_pLeftAreaWidget->m_pBookMarkArea->update();

    jumpToLine(nJumpLine, false);
    setTextCursor(cursor);
}

void TextEdit::setFontFamily(QString name)
{
    // Update font.
    m_fontName = name;
    updateFont();
    updateLeftAreaWidget();
}

void TextEdit::setFontSize(int size)
{
    // Update font.
    m_fontSize = size;
    updateFont();

    // Update line number after adjust font size.
    updateLeftAreaWidget();
}

void TextEdit::updateFont()
{
    QFont font = document()->defaultFont();
    font.setFixedPitch(true);
    font.setPointSize(m_fontSize);
    font.setFamily(m_fontName);
    setFont(font);
    setTabStopWidth(m_tabSpaceNumber * QFontMetrics(font).width(QChar(0x2192)));
}

void TextEdit::replaceAll(const QString &replaceText, const QString &withText)
{
    if (m_readOnlyMode) {
        return;
    }

    if (replaceText.isEmpty()) {
        return;
    }

    QTextDocument::FindFlags flags;
    flags &= QTextDocument::FindCaseSensitively;

    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::Start);

    QTextCursor startCursor = textCursor();
    startCursor.beginEditBlock();


    QString oldText = this->toPlainText();
    QString newText = oldText;

    newText.replace(replaceText,withText);


    ReplaceAllCommond* commond = new ReplaceAllCommond(oldText,newText,cursor);
    m_pUndoStack->push(commond);

    this->m_wrapper->window()->updateModifyStatus(m_sFilePath, true);

    startCursor.endEditBlock();
    setTextCursor(startCursor);
}

void TextEdit::replaceNext(const QString &replaceText, const QString &withText)
{
    if (m_readOnlyMode) {
        return;
    }

    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    if (replaceText.isEmpty() ||
            ! m_findHighlightSelection.cursor.hasSelection()) {
        // ?????????????????????
        return;
    }
    QTextCursor cursor = textCursor();
    //cursor.setPosition(m_findHighlightSelection.cursor.position() - replaceText.size());
    if (m_cursorStart != -1) {
        cursor.setPosition(m_cursorStart);
        m_cursorStart = -1;
    } else {
        cursor.setPosition(m_findHighlightSelection.cursor.selectionStart());
    }
    cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, replaceText.size());
    //cursor.insertText(withText);
    insertSelectTextEx(cursor, withText);

    // Update cursor.
    setTextCursor(cursor);
    highlightKeyword(replaceText, getPosition());
}

void TextEdit::replaceRest(const QString &replaceText, const QString &withText)
{
    if (m_readOnlyMode) {
        return;
    }

    // If replace text is nothing, don't do replace action.
    if (replaceText.isEmpty()) {
        return;
    }

    QTextDocument::FindFlags flags;
    flags &= QTextDocument::FindCaseSensitively;

    QTextCursor cursor = textCursor();

    QTextCursor startCursor = textCursor();
    startCursor.beginEditBlock();


    int pos = cursor.position();
    QString oldText = this->toPlainText();
    QString newText = oldText.left(pos);
    QString right = oldText.right(oldText.size() - pos);
    right.replace(replaceText,withText);
    newText += right;


    ReplaceAllCommond* commond = new ReplaceAllCommond(oldText,newText,cursor);
    m_pUndoStack->push(commond);

    m_wrapper->window()->updateModifyStatus(m_sFilePath, true);

    startCursor.endEditBlock();
    setTextCursor(startCursor);
}

void TextEdit::beforeReplace(QString _)
{
    if (_.isEmpty() ||
            !m_findHighlightSelection.cursor.hasSelection()) {
        highlightKeyword(_, getPosition());
    }
}

bool TextEdit::findKeywordForward(const QString &keyword)
{
    if (textCursor().hasSelection()) {
        // Get selection bound.
        int startPos = textCursor().anchor();
        int endPos = textCursor().position();

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
        //setTextCursor(cursor);

        QTextDocument::FindFlags options;
        options &= QTextDocument::FindCaseSensitively;

        bool foundOne = find(keyword, options);

        cursor.setPosition(endPos, QTextCursor::MoveAnchor);
        cursor.setPosition(startPos, QTextCursor::KeepAnchor);
        //setTextCursor(cursor);

        return foundOne;
    } else {
        QTextCursor recordCursor = textCursor();

        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
        //setTextCursor(cursor);

        QTextDocument::FindFlags options;
        options &= QTextDocument::FindCaseSensitively;

        bool foundOne = find(keyword, options);

        //setTextCursor(recordCursor);

        return foundOne;
    }
}

void TextEdit::removeKeywords()
{
    m_findHighlightSelection.cursor = textCursor();
    m_findHighlightSelection.cursor.clearSelection();

    m_findMatchSelections.clear();

    updateHighlightLineSelection();

    renderAllSelections();

    //setFocus();
}

bool TextEdit::highlightKeyword(QString keyword, int position)
{
    Q_UNUSED(position)
    m_findMatchSelections.clear();
    updateHighlightLineSelection();
    updateCursorKeywordSelection(keyword, true);
    bool bRet = updateKeywordSelectionsInView(keyword, m_findMatchFormat, &m_findMatchSelections);
    renderAllSelections();

    return bRet;
}

bool TextEdit::highlightKeywordInView(QString keyword)
{
    m_findMatchSelections.clear();
    bool yes = updateKeywordSelectionsInView(keyword, m_findMatchFormat, &m_findMatchSelections);
    // ???????????? setExtraSelections ?????????????????????????????????????????? renderAllSelections ??????????????????
    // setExtraSelections(m_findMatchSelections);
    renderAllSelections();

    return yes;
}

void TextEdit::clearFindMatchSelections()
{
    m_findMatchSelections.clear();
}

void TextEdit::updateCursorKeywordSelection(QString keyword, bool findNext)
{
    bool findOne = searchKeywordSeletion(keyword, textCursor(), findNext);

    if (!findOne) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(findNext ? QTextCursor::Start : QTextCursor::End, QTextCursor::MoveAnchor);
        if (!searchKeywordSeletion(keyword, cursor, findNext)) {
            m_findHighlightSelection.cursor = textCursor();
            m_findMatchSelections.clear();
            renderAllSelections();
        }
    }
}

void TextEdit::updateHighlightLineSelection()
{
    if (m_gestureAction == GA_slide) {
        QTextCursor textCursor = QPlainTextEdit::textCursor();
        return;
    }

    QTextEdit::ExtraSelection selection;

    selection.format.setBackground(m_currentLineColor);
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    selection.cursor = textCursor();
    selection.cursor.clearSelection();
    m_currentLineSelection = selection;
}

bool TextEdit::updateKeywordSelections(QString keyword, QTextCharFormat charFormat, QList<QTextEdit::ExtraSelection> *listSelection)
{
    // Clear keyword selections first.
    listSelection->clear();

    // Update selections with keyword.
    if (!keyword.isEmpty()) {
        QTextCursor cursor(document());
        QTextDocument::FindFlags flags;
        //flags &= QTextDocument::FindCaseSensitively;
        QTextEdit::ExtraSelection extra;
        extra.format = charFormat;
        cursor = document()->find(keyword, cursor, flags);

        if (cursor.isNull()) {
            return false;
        }

        while (!cursor.isNull()) {
            extra.cursor = cursor;
            listSelection->append(extra);
            cursor = document()->find(keyword, cursor, flags);
        }

        return true;
    }
    return false;
}

bool TextEdit::updateKeywordSelectionsInView(QString keyword, QTextCharFormat charFormat, QList<QTextEdit::ExtraSelection> *listSelection)
{
    // Clear keyword selections first.
    listSelection->clear();

    // Update selections with keyword.
    if (!keyword.isEmpty()) {
        QTextCursor cursor(document());
        QTextEdit::ExtraSelection extra;
        extra.format = charFormat;

        QScrollBar *pScrollBar = verticalScrollBar();
        QPoint startPoint = QPointF(0, 0).toPoint();
        QTextBlock beginBlock = cursorForPosition(startPoint).block();
        int beginPos = beginBlock.position();
        QTextBlock endBlock;

        if (pScrollBar->maximum() > 0) {
            QPoint endPoint = QPointF(0, 1.5 * height()).toPoint();
            endBlock = cursorForPosition(endPoint).block();
        } else {
            endBlock = document()->lastBlock();
        }
        int endPos = endBlock.position() + endBlock.length() - 1;

        cursor = document()->find(keyword, beginPos);
        if (cursor.isNull()) {
            return false;
        }

        while (!cursor.isNull()) {
            extra.cursor = cursor;
            listSelection->append(extra);
            cursor = document()->find(keyword, cursor);

            if (cursor.position() > endPos) {
                break;
            }
        }

        return true;
    }

    return false;
}

bool TextEdit::searchKeywordSeletion(QString keyword, QTextCursor cursor, bool findNext)
{
    if (keyword.isEmpty()) {
        return false;
    }

    bool ret = false;
    int offsetLines = 3;

    if (findNext) {
        QTextCursor next = document()->find(keyword, cursor);
        if (!next.isNull()) {
            m_findHighlightSelection.cursor = next;
            jumpToLine(next.blockNumber() + offsetLines, false);
            setTextCursor(next);
            ret = true;
        }

    } else {
        QTextCursor prev = document()->find(keyword, cursor, QTextDocument::FindBackward);
        if (!prev.isNull()) {
            m_findHighlightSelection.cursor = prev;
            jumpToLine(prev.blockNumber() + offsetLines, false);
            setTextCursor(prev);
            ret = true;
        }
    }

    return ret;
}

void TextEdit::renderAllSelections()
{
    QList<QTextEdit::ExtraSelection> finalSelections;
    QList<QPair<QTextEdit::ExtraSelection, qint64>> selectionsSortList;

    // ???????????????????????????
    if (m_HightlightYes)
        finalSelections.append(m_currentLineSelection);
    // ??????????????????????????????
    // else {
    //     selections.clear();
    // }

    // ????????????????????????m_markAllSelection ???????????????????????????????????????, ???????????????
    // ?????????????????????????????????????????????????????????????????????
    // finalSelections.append(m_markAllSelection);

    // ??????????????????????????????????????? selectionsSortList
    // ?????????????????????????????????????????? finalSelections ???
    selectionsSortList.append(m_wordMarkSelections);

    // Find ??? Replace ??????????????????????????????????????? finalSelections ???
    // ???????????????????????????????????????????????????????????????
    // selections.append(m_findMatchSelections);
    // selections.append(m_findHighlightSelection);

    // ????????????????????????
    // selections.append(m_wordUnderCursorSelection);

    // ????????????????????????????????????????????????????????? finalSelections ???
    // selections.append(m_beginBracketSelection);
    // selections.append(m_endBracketSelection);

    // ????????????????????????????????????????????????????????????????????????{}????????????
    // ????????????????????????????????????????????? finalSelections ????????????????????????
    // selections.append(m_markFoldHighLightSelections);

    // Alt?????????????????????, ???????????????????????????
    // selections.append(m_altModSelections);

    // ????????????????????????????????? selections ????????? selectionsSortList ?????? ?????????????????????
    QMap<QString, QList<QPair<QTextEdit::ExtraSelection, qint64>>>::Iterator it;
    for (it = m_mapKeywordMarkSelections.begin(); it != m_mapKeywordMarkSelections.end(); ++it) {
        selectionsSortList.append(it.value());
    }

    // ???????????????????????????????????????????????? selections
    qSort(selectionsSortList.begin(), selectionsSortList.end(), [](const QPair<QTextEdit::ExtraSelection, qint64> &A,const QPair<QTextEdit::ExtraSelection, qint64> &B) {
        return (A.second < B.second);
    });

    // ??????????????????????????? selections ????????? finalSelections ???
    for(int i = 0; i < selectionsSortList.size(); i++){
        finalSelections.append(selectionsSortList.at(i).first);
    }

    // ????????????
    finalSelections.append(m_beginBracketSelection);
    finalSelections.append(m_endBracketSelection);

    // ???????????????
    finalSelections.append(m_markFoldHighLightSelections);

    // Alt?????????????????????
    finalSelections.append(m_altModSelections);

    // ???????????????????????????????????????
    // Find ??????
    finalSelections.append(m_findMatchSelections);
    // Replace ??????
    finalSelections.append(m_findHighlightSelection);

    // ????????? QPlainText ???????????????
    setExtraSelections(finalSelections);
}

void TextEdit::updateMarkAllSelectColor()
{
    isMarkAllLine(m_bIsMarkAllLine, m_strMarkAllLineColorName);
    renderAllSelections();
}

DMenu *TextEdit::getHighlightMenu()
{
    return m_hlGroupMenu;
}

void TextEdit::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(m_pLeftAreaWidget->m_pLineNumberArea);
    QColor lineNumberAreaBackgroundColor;
    if (QColor(m_backgroundColor).lightness() < 128) {
        lineNumberAreaBackgroundColor = palette().brightText().color();
        lineNumberAreaBackgroundColor.setAlphaF(0.06);

        m_lineNumbersColor.setAlphaF(0.2);
    } else {
        lineNumberAreaBackgroundColor = palette().brightText().color();
        lineNumberAreaBackgroundColor.setAlphaF(0.03);
        m_lineNumbersColor.setAlphaF(0.3);
    }
    //painter.fillRect(event->rect(), lineNumberAreaBackgroundColor);

    int blockNumber = getFirstVisibleBlockId();
    QTextBlock block = document()->findBlockByNumber(blockNumber);


    int top = this->viewport()->geometry().top() + verticalScrollBar()->value();
    int bottom = top + static_cast<int>(document()->documentLayout()->blockBoundingRect(block).height());

    Utils::setFontSize(painter, document()->defaultFont().pointSize() - 2);
    QPoint endPoint;

    if (verticalScrollBar()->maximum() > 0) {
        endPoint = QPointF(0, height() + height() / verticalScrollBar()->maximum() * verticalScrollBar()->value()).toPoint();
    }

    QTextCursor cur = cursorForPosition(endPoint);
    QTextBlock endBlock = cur.block();
    int nPageLine = endBlock.blockNumber();
    int nStartLine = block.blockNumber();

    if (verticalScrollBar()->maximum() == 0) {
        nPageLine = blockCount() - 1;
    }

    cur = textCursor();
    for (int i = nStartLine; i <= nPageLine; i++) {
        if (i + 1 == m_markStartLine) {
            painter.setPen(m_regionMarkerColor);
        } else {
            painter.setPen(m_lineNumbersColor);
        }

        m_fontLineNumberArea.setPointSize(font().pointSize() - 1);
        painter.setFont(m_fontLineNumberArea);

        cur.setPosition(block.position(), QTextCursor::MoveAnchor);

        if (block.isVisible()) {
            int height = cursorRect(cur).height();
            updateLeftWidgetWidth(height);
            painter.drawText(0, cursorRect(cur).y(),
                             m_pLeftAreaWidget->m_pLineNumberArea->width(), cursorRect(cur).height() - static_cast<int>(document()->documentMargin()),
                             Qt::AlignVCenter | Qt::AlignHCenter, QString::number(block.blockNumber() + 1));
        }

        block = block.next();
        top = bottom/* + document()->documentMargin()*/;
        bottom = top + static_cast<int>(document()->documentLayout()->blockBoundingRect(block).height());
    }
}

void TextEdit::codeFLodAreaPaintEvent(QPaintEvent *event)
{
    m_listFlodIconPos.clear();
    QPainter painter(m_pLeftAreaWidget->m_pFlodArea);

    QColor codeFlodAreaBackgroundColor;
    if (QColor(m_backgroundColor).lightness() < 128) {
        codeFlodAreaBackgroundColor = palette().brightText().color();
        codeFlodAreaBackgroundColor.setAlphaF(0.06);

        m_lineNumbersColor.setAlphaF(0.2);
    } else {
        codeFlodAreaBackgroundColor = palette().brightText().color();
        codeFlodAreaBackgroundColor.setAlphaF(0.03);
        m_lineNumbersColor.setAlphaF(0.3);
    }

    //painter.fillRect(event->rect(), codeFlodAreaBackgroundColor);
    int blockNumber = getFirstVisibleBlockId();
    QTextBlock block = document()->findBlockByNumber(blockNumber);

    DGuiApplicationHelper *guiAppHelp = DGuiApplicationHelper::instance();
    QString theme  = "";

    if (guiAppHelp->themeType() == DGuiApplicationHelper::ColorType::DarkType) {  //????????????
        theme = "d";
    } else {  //????????????
        theme = "l";
    }

    QString flodImagePath = QString(":/images/d-%1.svg").arg(theme);
    QString unflodImagePath = QString(":/images/u-%1.svg").arg(theme);
    QImage Unfoldimage(unflodImagePath);
    QImage foldimage(flodImagePath);
    QPixmap scaleFoldPixmap;
    QPixmap scaleunFoldPixmap;
    int imageTop = 0;//??????????????????
    int fontHeight = fontMetrics().height();
    double nfoldImageHeight = fontHeight;

    QPoint endPoint;

    if (verticalScrollBar()->maximum() > 0) {
        endPoint = QPointF(0, height() + height() / verticalScrollBar()->maximum() * verticalScrollBar()->value()).toPoint();
    }

    QTextCursor cur = cursorForPosition(endPoint);
    QTextBlock endBlock = cur.block();
    int nPageLine = endBlock.blockNumber();

    if (verticalScrollBar()->maximum() == 0) {
        nPageLine = blockCount() - 1;
    }

    cur = textCursor();

    for (int iBlockCount = blockNumber ; iBlockCount <= nPageLine; ++iBlockCount) {
        if (block.isVisible()) {
            //??????????????????????????????????????????????????????????????????isNeedShowFoldIcon?????????????????????????????????????????????????????????????????????????????????????????????????????????

            //????????????????????? ??????????????????????????????????????????????????????????????????????????????????????????
            QString text = block.text();
            QRegExp regExp("^\"*\"&");
            QString curText = text.remove(regExp);

            //???????????????????????????????????? ?????????????????????????????????????????????????????????????????????
            bool bHasCommnent = false;
            QString multiLineCommentMark;
            QString singleLineCommentMark;

            if (m_commentDefinition.isValid()) {
                multiLineCommentMark = m_commentDefinition.multiLineStart.trimmed();
                singleLineCommentMark = m_commentDefinition.singleLine.trimmed();
                //???????????????????????????????????????
                if (!multiLineCommentMark.isEmpty()) bHasCommnent = block.text().trimmed().startsWith(multiLineCommentMark);
                if (!singleLineCommentMark.isEmpty()) bHasCommnent = block.text().trimmed().startsWith(singleLineCommentMark);
            } else {
                bHasCommnent = false;
            }

            //?????????????????? ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
            if (curText.contains("{") && isNeedShowFoldIcon(block) && !bHasCommnent) {

                cur.setPosition(block.position(), QTextCursor::MoveAnchor);

                auto rate = devicePixelRatioF();
                if(rate <=1)
                    rate = 1.25;
                foldimage = foldimage.scaled(foldimage.width()*rate, foldimage.height()*rate);
                scaleFoldPixmap = Utils::renderSVG(flodImagePath, QSize(foldimage.height(), foldimage.width()), false);
                scaleFoldPixmap.setDevicePixelRatio(devicePixelRatioF());
                scaleunFoldPixmap = Utils::renderSVG(unflodImagePath, QSize(foldimage.height(), foldimage.width()), false);
                scaleunFoldPixmap.setDevicePixelRatio(devicePixelRatioF());

        #if 0
                if (fontHeight > foldimage.height()) {
                    scaleFoldPixmap = Utils::renderSVG(flodImagePath, QSize(foldimage.height(), foldimage.width()), false);
                    scaleFoldPixmap.setDevicePixelRatio(devicePixelRatioF());
                    scaleunFoldPixmap = Utils::renderSVG(unflodImagePath, QSize(foldimage.height(), foldimage.width()), false);
                    scaleunFoldPixmap.setDevicePixelRatio(devicePixelRatioF());
                } else {
                    double scale = nfoldImageHeight / foldimage.height();
                    double nScaleWidth = scale * foldimage.height() * foldimage.height() / foldimage.width();
                    scaleFoldPixmap = Utils::renderSVG(flodImagePath, QSize(static_cast<int>(foldimage.height() * scale), static_cast<int>(nScaleWidth)), false);
                    scaleFoldPixmap.setDevicePixelRatio(devicePixelRatioF());
                    scaleunFoldPixmap = Utils::renderSVG(unflodImagePath, QSize(static_cast<int>(foldimage.height() * scale), static_cast<int>(nScaleWidth)), false);
                    scaleunFoldPixmap.setDevicePixelRatio(devicePixelRatioF());
                }

                int nOffset = (m_pLeftAreaWidget->m_pBookMarkArea->width() - scaleFoldPixmap.width()) / 2;
                if (block.next().isVisible()) {
                    if (block.isVisible()) {
                        imageTop = cursorRect(cur).y() + (cursorRect(cur).height() - scaleFoldPixmap.height()) / 2;
                        painter.drawPixmap(nOffset, imageTop/* - static_cast<int>(document()->documentMargin())*/, scaleFoldPixmap);
                    }
                } else {
                    if (block.isVisible()) {
                        imageTop = cursorRect(cur).y() + (cursorRect(cur).height() - scaleunFoldPixmap.height()) / 2;
                        painter.drawPixmap(nOffset, imageTop/* - static_cast<int>(document()->documentMargin())*/, scaleunFoldPixmap);
                    }
                }
        #endif
                int nOffset = -8;
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setRenderHints(QPainter::SmoothPixmapTransform);

                QRect rt = cursorRect(cur);

                if (block.next().isVisible()) {
                     if (block.isVisible()) {
                         //imageTop = rt.y() ;
                         //painter.drawPixmap(nOffset, imageTop, rt.height(),rt.height(),scaleFoldPixmap);
                         int height = cursorRect(cur).height();
                         updateLeftWidgetWidth(height);
                         QRect rect(0,cursorRect(cur).y(),height,height);
                         paintCodeFlod(&painter,rect);
                      }
                  } else {
                     if (block.isVisible()) {
                         //imageTop = rt.y() ;
                        // painter.drawPixmap(nOffset, imageTop,rt.height(),rt.height(), scaleunFoldPixmap);
                         int height = cursorRect(cur).height();
                         updateLeftWidgetWidth(height);
                         QRect rect(0,cursorRect(cur).y(),height,height);
                         paintCodeFlod(&painter,rect,true);
                      }
                  }
                m_listFlodIconPos.append(block.blockNumber());
            }
        }

        block = block.next();
    }
}

void TextEdit::setBookmarkFlagVisable(bool isVisable, bool bIsFirstOpen)
{
    int leftAreaWidth = m_pLeftAreaWidget->width();
    int bookmarkAreaWidth = m_pLeftAreaWidget->m_pBookMarkArea->width();

    if (!bIsFirstOpen) {
        if (isVisable) {
            //m_pLeftAreaWidget->setFixedWidth(leftAreaWidth + bookmarkAreaWidth);
        } else {
           // m_pLeftAreaWidget->setFixedWidth(leftAreaWidth - bookmarkAreaWidth);
        }
    }

    m_pIsShowBookmarkArea = isVisable;
    m_pLeftAreaWidget->m_pBookMarkArea->setVisible(isVisable);
}

void TextEdit::setCodeFlodFlagVisable(bool isVisable, bool bIsFirstOpen)
{
    int leftAreaWidth = m_pLeftAreaWidget->width();
    int flodAreaWidth = m_pLeftAreaWidget->m_pFlodArea->width();
    if (!bIsFirstOpen) {
        if (isVisable) {
            //m_pLeftAreaWidget->setFixedWidth(leftAreaWidth + flodAreaWidth);
        } else {
            //m_pLeftAreaWidget->setFixedWidth(leftAreaWidth - flodAreaWidth);
        }
    }
    m_pIsShowCodeFoldArea = isVisable;
    m_pLeftAreaWidget->m_pFlodArea->setVisible(isVisable);
}

void TextEdit::setHighLineCurrentLine(bool ok)
{
    m_HightlightYes = ok;
    renderAllSelections();
}

void TextEdit::updateLeftAreaWidget()
{
    int blockSize = QString::number(blockCount()).size();
    int leftAreaWidth = 0;

    //?????????????????????
    if (m_pIsShowBookmarkArea) {
        leftAreaWidth += m_pLeftAreaWidget->m_pBookMarkArea->width();
    }
    if (m_pIsShowCodeFoldArea) {
        leftAreaWidth += m_pLeftAreaWidget->m_pFlodArea->width();
    }

    if (bIsSetLineNumberWidth) {
        leftAreaWidth += blockSize * fontMetrics().width('9') + 5;
    }
   // m_pLeftAreaWidget->setFixedWidth(leftAreaWidth);
    m_pLeftAreaWidget->updateAll();
}


void TextEdit::handleScrollFinish()
{
    // Restore cursor postion.
    jumpToLine(m_restoreRow, false);

    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
    cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, m_restoreColumn);

    // Update cursor.
    setTextCursor(cursor);
}

void TextEdit::setSyntaxDefinition(KSyntaxHighlighting::Definition def)
{
    m_commentDefinition.setComments(def.singleLineCommentMarker(), def.multiLineCommentMarker().first,  def.multiLineCommentMarker().second);
}

bool TextEdit::setCursorKeywordSeletoin(int position, bool findNext)
{
    int offsetLines = 3;

    if (findNext) {
        for (int i = 0; i < m_findMatchSelections.size(); i++) {
            if (m_findMatchSelections[i].cursor.position() > position) {
                m_findHighlightSelection.cursor = m_findMatchSelections[i].cursor;

                jumpToLine(m_findMatchSelections[i].cursor.blockNumber() + offsetLines, false);

                QTextCursor cursor = textCursor();
                cursor.setPosition(m_findMatchSelections[i].cursor.position());

                // Update cursor.
                setTextCursor(cursor);

                return true;
            }
        }
    } else {
        for (int i = m_findMatchSelections.size() - 1; i >= 0; i--) {
            if (m_findMatchSelections[i].cursor.position() < position) {
                m_findHighlightSelection.cursor = m_findMatchSelections[i].cursor;

                jumpToLine(m_findMatchSelections[i].cursor.blockNumber() + offsetLines, false);

                QTextCursor cursor = textCursor();
                cursor.setPosition(m_findMatchSelections[i].cursor.position());

                // Update cursor.
                setTextCursor(cursor);
                return true;
            }
        }
    }

    return false;
}

void TextEdit::cursorPositionChanged()
{
    // ???????????????????????? Bracket ?????????selection
    // m_beginBracketSelection ??? m_endBracketSelection ?????? updateHighlightBrackets ????????????
    m_beginBracketSelection = QTextEdit::ExtraSelection();
    m_endBracketSelection = QTextEdit::ExtraSelection();

    updateHighlightLineSelection();
    updateHighlightBrackets('(', ')');
    updateHighlightBrackets('{', '}');
    updateHighlightBrackets('[', ']');
    renderAllSelections();

    QTextCursor cursor = textCursor();
    if (m_wrapper) {
        m_wrapper->bottomBar()->updatePosition(cursor.blockNumber() + 1,
                                               cursor.positionInBlock() + 1);
    }

    m_pLeftAreaWidget->m_pLineNumberArea->update();
    m_pLeftAreaWidget->m_pBookMarkArea->update();
    m_pLeftAreaWidget->m_pFlodArea->update();
}

void TextEdit::cut()
{

    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    //???????????????????????????
    if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
        QString data;
        for(auto it = m_altModSelections.begin();it!=m_altModSelections.end();it++)
        {
            auto text = (*it).cursor.selectedText();
            data += text ;
            if(it != m_altModSelections.end() - 1)
                data += "\n";
        }
        //???????????????
        for (int i = 0; i < m_altModSelections.size(); i++) {
            if (m_altModSelections[i].cursor.hasSelection()) {
                QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(m_altModSelections[i].cursor);
                m_pUndoStack->push(pDeleteStack);
            }
        }
        //??????????????????
        QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
        clipboard->setText(data);
    } else {
        QTextCursor cursor = textCursor();
        //????????????????????????
        if (cursor.hasSelection()) {
            QString data = cursor.selectedText();
            QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(cursor);
            m_pUndoStack->push(pDeleteStack);
            QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
            clipboard->setText(data);
        }
    }
    unsetMark();
}

void TextEdit::copy()
{
    if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
        QString data;
        for(auto it = m_altModSelections.begin();it!=m_altModSelections.end();it++)
        {
            auto text = (*it).cursor.selectedText();
            data += text ;
            if(it != m_altModSelections.end() - 1)
                data += "\n";
        }
        QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
        clipboard->setText(data);
        tryUnsetMark();
    } else {

        if(!m_isSelectAll){
            QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
            if (textCursor().hasSelection()) {
                clipboard->setText(textCursor().selection().toPlainText());
                tryUnsetMark();
            } else {
                clipboard->setText(m_highlightWordCacheCursor.selectedText());
            }
        } else{

            QClipboard *clipboard = QApplication::clipboard();
            QString text = this->toPlainText();
            clipboard->setText(text);
        }

    }
}

void TextEdit::paste()
{
#if 0
    //2021-05-25:?????????????????????????????????????????????
    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    const QClipboard *clipboard = QApplication::clipboard(); //?????????????????????
    auto text = clipboard->text();
    if(text.isEmpty())
        return;
    if (!m_bIsAltMod){
        QTextCursor cursor = textCursor();
        insertSelectTextEx(cursor, text);
        unsetMark();
    }
    else {
        insertColumnEditTextEx(text);
    }

#endif


    //???????????????-??????????????????
    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    const QClipboard *clipboard = QApplication::clipboard(); //?????????????????????
    auto text = clipboard->text();
    if(text.isEmpty())
        return;
    if (!m_bIsAltMod){

        int block = 1 * 1024 * 1024 ;
        int size = text.size();
        if(size > block){
            InsertBlockByTextCommond* commond = new InsertBlockByTextCommond(text,this,m_wrapper);
            m_pUndoStack->push(commond);
        }else{
            QTextCursor cursor = textCursor();
            insertSelectTextEx(cursor, text);
            unsetMark();
        }
    }
    else {
        insertColumnEditTextEx(text);
    }





#if 0
    //???????????????-??????????????????
    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    const QClipboard *clipboard = QApplication::clipboard(); //?????????????????????
    auto text = clipboard->text();
    if(text.isEmpty())
        return;

    if (!m_bIsAltMod){
        QString oldText = this->toPlainText();
        QString newText(oldText);
        QTextCursor cursor = textCursor();
        if(cursor.hasSelection()){
            int start = cursor.position();
            int end = cursor.anchor();
            if(start>end)
                std::swap(start,end);
            newText.remove(start,end-start);

            newText.insert(start,text);
            ReplaceAllCommond* commond = new ReplaceAllCommond(oldText,newText,cursor);
            m_pUndoStack->push(commond);

        }else{
            int pos = cursor.position();
            newText.insert(pos,text);
            ReplaceAllCommond* commond = new ReplaceAllCommond(oldText,newText,cursor);
            m_pUndoStack->push(commond);
        }
    }
    else {
        insertColumnEditTextEx(text);
    }
#endif

}

void TextEdit::highlight()
{
    QTimer::singleShot(0,this,[&]()
    {
        if(nullptr != m_wrapper)
            m_wrapper->OnUpdateHighlighter();
    });
}

void TextEdit::selectTextInView()
{

    QPoint bottom = QPoint(this->viewport()->width(),this->viewport()->height());
    int endPos = cursorForPosition(bottom).position();
    int startPos = cursorForPosition(QPoint(0,0)).position();

    auto cursor = this->textCursor();
    cursor.setPosition(endPos);
    cursor.setPosition(startPos,QTextCursor::KeepAnchor);
    this->setTextCursor(cursor);
    this->horizontalScrollBar()->setValue(0);

}

void TextEdit::setSelectAll()
{
    if(m_wrapper->getFileLoading())
            return;

     m_bIsAltMod = false;
     m_isSelectAll = true;
     selectTextInView();
}

void TextEdit::updateHighlightBrackets(const QChar &openChar, const QChar &closeChar)
{
    QTextDocument *doc = document();
    QTextCursor cursor = textCursor();
    int position = cursor.position();

    QTextCursor bracketBeginCursor;
    QTextCursor bracketEndCursor;
    cursor.clearSelection();

    if (!bracketBeginCursor.isNull() || !bracketEndCursor.isNull()) {
        bracketBeginCursor.setCharFormat(QTextCharFormat());
        bracketEndCursor.setCharFormat(QTextCharFormat());
        bracketBeginCursor = bracketEndCursor = QTextCursor();
    }

    QChar begin, end;

    if (doc->characterAt(position) == openChar ||
            doc->characterAt(position) == closeChar ||
            doc->characterAt(position - 1) == openChar ||
            doc->characterAt(position - 1) == closeChar) {
        bool forward = doc->characterAt(position) == openChar ||
                       doc->characterAt(position - 1) == openChar;

        if (forward) {
            if (doc->characterAt(position) == openChar) {
                position++;
            } else {
                cursor.setPosition(position - 1);
            }

            begin = openChar;
            end = closeChar;
        } else {
            if (doc->characterAt(position) == closeChar) {
                cursor.setPosition(position + 1);
                position -= 1;
            } else {
                position -= 2;
            }

            begin = closeChar;
            end = openChar;
        }

        bracketBeginCursor = cursor;
        bracketBeginCursor.movePosition(forward ? QTextCursor::NextCharacter : QTextCursor::PreviousCharacter,
                                        QTextCursor::KeepAnchor);

        int braceDepth = 1;
        QChar c;

        while (!(c = doc->characterAt(position)).isNull()) {
            if (c == begin) {
                braceDepth++;
            } else if (c == end) {
                braceDepth--;

                if (!braceDepth) {
                    bracketEndCursor = QTextCursor(doc);
                    bracketEndCursor.setPosition(position);
                    bracketEndCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                    break;
                }
            }

            forward ? position++ : position--;
        }

        // cannot find the end bracket to not need to highlight.
        if (!bracketEndCursor.isNull()) {
            m_beginBracketSelection.cursor = bracketBeginCursor;
            m_beginBracketSelection.format = m_bracketMatchFormat;

            m_endBracketSelection.cursor = bracketEndCursor;
            m_endBracketSelection.format = m_bracketMatchFormat;
        }
    }
}

int TextEdit::getFirstVisibleBlockId() const
{
    QTextCursor cur = QTextCursor(this->document());
    if (cur.isNull()) {
        return 0;
    }
    cur.movePosition(QTextCursor::Start);

    QPoint startPoint;
    QTextBlock startBlock, endBlock;

    if (verticalScrollBar()->maximum() > height()) {
        startPoint = QPointF(0, height() / verticalScrollBar()->maximum() * verticalScrollBar()->value()).toPoint();
        //endPoint = QPointF(0,height() + height()/verticalScrollBar()->maximum()*verticalScrollBar()->value()).toPoint();
    } else if (verticalScrollBar()->maximum() > 0 && verticalScrollBar()->maximum() <= height()) {

        startPoint = QPointF(0, verticalScrollBar()->value() / verticalScrollBar()->maximum()).toPoint();
    }

    cur = cursorForPosition(startPoint);
    startBlock = document()->findBlock(cur.position());
    cur.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    if (startBlock.text() != cur.selection().toPlainText()) {
        return startBlock.blockNumber() + 1;
    }

    return startBlock.blockNumber();
}

//line ?????????????????????  isvisable????????????  iInitnum????????????????????????????????????  isFirstLine???????????????????????????????????????????????????
bool TextEdit::getNeedControlLine(int line, bool isVisable)
{
    QTextDocument *doc = document();

    //?????????????????????
    QTextBlock curBlock = doc->findBlockByNumber(line);

    //?????????????????? ??????????????????
    QTextBlock beginBlock = curBlock.next(), endBlock = curBlock.next();

    //????????????????????????????????????"{"
    if (line == 0 && !curBlock.text().contains("{")) {
        curBlock = curBlock.next();
    }

    //???????????????????????????
    int position = curBlock.position();
    int offset = curBlock.text().lastIndexOf('{');
    position += offset;


    //??????????????????????????????????????????
    QChar begin = '{', end = '}';
    QTextCursor cursor = textCursor();
    cursor.setPosition(position, QTextCursor::MoveAnchor);
    cursor.clearSelection();

    //??????????????? ???????????????
    QTextCursor bracketBeginCursor = cursor;
    QTextCursor bracketEndCursor = cursor;
    bracketBeginCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);

    //???????????????????????????
    int braceDepth = 0;
    QChar c;
    while (!(c = doc->characterAt(position)).isNull()) {
        if (c == begin) {
            braceDepth++;
        } else if (c == end) {
            braceDepth--;

            if (0 == braceDepth) {
                bracketEndCursor = QTextCursor(doc);
                bracketEndCursor.setPosition(position);
                bracketEndCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                //??????????????????????????????
                endBlock = bracketEndCursor.block();
                break;
            }
        }
        position++;
    }

    //???????????????????????????????????????????????????
    if (braceDepth != 0) {
        //?????????????????????????????? ????????????????????????
        while (beginBlock.isValid()) {
            beginBlock.setVisible(isVisable);
            viewport()->adjustSize();
            beginBlock = beginBlock.next();
        }
        return true;
        //?????????????????????????????? //????????????"{" "}"?????????????????????
    } else if (braceDepth != 0 || endBlock == curBlock) {
        return false;
    } else {
        //?????????????????????????????? ????????????????????????
        while (beginBlock != endBlock && beginBlock.isValid()) {
            if (beginBlock.isValid()) {
                beginBlock.setVisible(isVisable);
            }
            viewport()->adjustSize();
            beginBlock = beginBlock.next();
        }

        //???????????????????????????,?????????????????????"}"
        if (beginBlock.isValid() && beginBlock == endBlock && endBlock.text().simplified() == "}") {
            endBlock.setVisible(isVisable);
            viewport()->adjustSize();
        }

        return true;
    }
}

bool TextEdit::event(QEvent *event)
{
    if (event->type() == QEvent::Gesture)
        gestureEvent(static_cast<QGestureEvent *>(event));
    return DPlainTextEdit::event(event);
}

bool TextEdit::gestureEvent(QGestureEvent *event)
{
    if (QGesture *tap = event->gesture(Qt::TapGesture))
        tapGestureTriggered(static_cast<QTapGesture *>(tap));
    if (QGesture *tapAndHold = event->gesture(Qt::TapAndHoldGesture))
        tapAndHoldGestureTriggered(static_cast<QTapAndHoldGesture *>(tapAndHold));
    if (QGesture *pan = event->gesture(Qt::PanGesture))
        panTriggered(static_cast<QPanGesture *>(pan));
    if (QGesture *pinch = event->gesture(Qt::PinchGesture))
        pinchTriggered(static_cast<QPinchGesture *>(pinch));
    if (QGesture *swipe = event->gesture(Qt::SwipeGesture))
        swipeTriggered(static_cast<QSwipeGesture *>(swipe));

    return true;
}

void TextEdit::tapGestureTriggered(QTapGesture *tap)
{
    this->clearFocus();
    //??????????????????
    switch (tap->state()) {
    case Qt::GestureStarted: {
        m_gestureAction = GA_tap;
        m_tapBeginTime = QDateTime::currentDateTime().toMSecsSinceEpoch();
        break;
    }
    case Qt::GestureUpdated: {
        m_gestureAction = GA_slide;
        break;
    }
    case Qt::GestureCanceled: {
        //????????????????????????????????????
        qint64 timeSpace = QDateTime::currentDateTime().toMSecsSinceEpoch() - m_tapBeginTime;
        if (timeSpace < TAP_MOVE_DELAY || m_slideContinueX || m_slideContinueY) {
            m_slideContinueX = false;
            m_slideContinueY = false;
            m_gestureAction = GA_slide;
            qDebug() << "slide start" << timeSpace;
        } else {
            qDebug() << "null start" << timeSpace;
            m_gestureAction = GA_null;
        }
        break;
    }
    case Qt::GestureFinished: {
        m_gestureAction = GA_null;
        break;
    }
    default: {
        Q_ASSERT(false);
        break;
    }
    }
}

void TextEdit::tapAndHoldGestureTriggered(QTapAndHoldGesture *tapAndHold)
{
    //????????????
    switch (tapAndHold->state()) {
    case Qt::GestureStarted:
        m_gestureAction = GA_hold;
        break;
    case Qt::GestureUpdated:
        Q_ASSERT(false);
        break;
    case Qt::GestureCanceled:
        Q_ASSERT(false);
        break;
    case Qt::GestureFinished:
        m_gestureAction = GA_null;
        break;
    default:
        Q_ASSERT(false);
        break;
    }
}

void TextEdit::panTriggered(QPanGesture *pan)
{
    //????????????
    switch (pan->state()) {
    case Qt::GestureStarted:
        m_gestureAction = GA_pan;
        break;
    case Qt::GestureUpdated:
        break;
    case Qt::GestureCanceled:
        break;
    case Qt::GestureFinished:
        m_gestureAction = GA_null;
        break;
    default:
        Q_ASSERT(false);
        break;
    }
}

void TextEdit::pinchTriggered(QPinchGesture *pinch)
{
    //????????????   -----??????or??????
    switch (pinch->state()) {
    case Qt::GestureStarted: {
        m_gestureAction = GA_pinch;
        if (static_cast<int>(m_scaleFactor) != m_fontSize) {
            m_scaleFactor = m_fontSize;
        }
        break;
    }
    case Qt::GestureUpdated: {
        QPinchGesture::ChangeFlags changeFlags = pinch->changeFlags();
        if (changeFlags & QPinchGesture::ScaleFactorChanged) {
            m_currentStepScaleFactor = pinch->totalScaleFactor();
        }
        break;
    }
    case Qt::GestureCanceled: {
        Q_ASSERT(false);
        break;
    }
    case Qt::GestureFinished: {
        m_gestureAction = GA_null;
        m_scaleFactor *= m_currentStepScaleFactor;
        m_currentStepScaleFactor = 1;
        break;
    }
    default: {
        Q_ASSERT(false);
        break;
    }
    }//switch

    //QFont font = getVTFont();
    int size = static_cast<int>(m_scaleFactor * m_currentStepScaleFactor);
    if (size < 8)
        size = 8;
    if (size > 50)
        size = 50;
    //????????????????????????????????????
    setFontSize(size);
    //?????????????????????????????????
    qobject_cast<Window *>(this->window())->changeSettingDialogComboxFontNumber(size);
}

void TextEdit::swipeTriggered(QSwipeGesture *swipe)
{
    //????????????
//    switch (swipe->state()) {
//    case Qt::GestureStarted:
//        m_gestureAction = GA_swipe;
//        break;
//    case Qt::GestureUpdated:
//        break;
//    case Qt::GestureCanceled:
//        m_gestureAction = GA_null;
//        break;
//    case Qt::GestureFinished:
//        Q_ASSERT(false);
//        break;
//    default:
//        Q_ASSERT(false);
//        break;
//    }

}

void TextEdit::slideGestureY(qreal diff)
{
    static qreal delta = 0.0;
    int step = static_cast<int>(diff + delta);
    delta = diff + delta - step;

    verticalScrollBar()->setValue(verticalScrollBar()->value() + step);
}

void TextEdit::slideGestureX(qreal diff)
{
    static qreal delta = 0.0;
    int step = static_cast<int>(diff + delta);
    delta = diff + delta - step;

    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + step * 30);
}

void TextEdit::setTheme(const QString &path)
{
    QVariantMap jsonMap = Utils::getThemeMapFromPath(path);
    QVariantMap textStylesMap = jsonMap["text-styles"].toMap();
    QString themeCurrentLineColor = jsonMap["editor-colors"].toMap()["current-line"].toString();
    QString textColor = textStylesMap["Normal"].toMap()["text-color"].toString();

    m_backgroundColor = QColor(jsonMap["editor-colors"].toMap()["background-color"].toString());
    m_currentLineColor = QColor(themeCurrentLineColor);
    m_currentLineNumberColor = QColor(jsonMap["editor-colors"].toMap()["current-line-number"].toString());
    m_lineNumbersColor = QColor(jsonMap["editor-colors"].toMap()["line-numbers"].toString());
    m_regionMarkerColor = QColor(textStylesMap["RegionMarker"].toMap()["selected-text-color"].toString());
    m_selectionColor = QColor(textStylesMap["Normal"].toMap()["selected-text-color"].toString());
    m_selectionBgColor = QColor(textStylesMap["Normal"].toMap()["selected-bg-color"].toString());
    m_bracketMatchFormat = currentCharFormat();
    m_bracketMatchFormat.setForeground(QColor(jsonMap["editor-colors"].toMap()["bracket-match-fg"].toString()));
    m_bracketMatchFormat.setBackground(QColor(jsonMap["editor-colors"].toMap()["bracket-match-bg"].toString()));

    QString findMatchBgColor = jsonMap["editor-colors"].toMap()["find-match-background"].toString();
    QString findMatchFgColor = jsonMap["editor-colors"].toMap()["find-match-foreground"].toString();
    m_findMatchFormat = currentCharFormat();
    m_findMatchFormat.setBackground(QColor(findMatchBgColor));
    m_findMatchFormat.setForeground(QColor(findMatchFgColor));

    QString findHighlightBgColor = jsonMap["editor-colors"].toMap()["find-highlight-background"].toString();
    QString findHighlightFgColor = jsonMap["editor-colors"].toMap()["find-highlight-foreground"].toString();
    m_findHighlightSelection.format.setProperty(QTextFormat::BackgroundBrush, QBrush(QColor(findHighlightBgColor)));
    m_findHighlightSelection.format.setProperty(QTextFormat::ForegroundBrush, QBrush(QColor(findHighlightFgColor)));

    m_beginBracketSelection.format = m_bracketMatchFormat;
    m_endBracketSelection.format = m_bracketMatchFormat;

    int iVerticalScrollValue = getScrollOffset();
    int iHorizontalScrollVaule = horizontalScrollBar()->value();
    getScrollOffset();
    verticalScrollBar()->setSliderPosition(iVerticalScrollValue);
    horizontalScrollBar()->setSliderPosition(iHorizontalScrollVaule);

    m_pLeftAreaWidget->m_pLineNumberArea->update();
    m_pLeftAreaWidget->m_pBookMarkArea->update();

    updateHighlightLineSelection();
    renderAllSelections();
}

bool TextEdit::highlightWordUnderMouse(QPoint pos)
{
    // Get cursor match mouse pointer coordinate, but cursor maybe not under mouse pointer.
    QTextCursor cursor(cursorForPosition(pos));

    // Get cursor rectangle.
    auto rect = cursorRect(cursor);
    int widthOffset = 10;
    rect.setX(std::max(rect.x() - widthOffset / 2, 0));
    rect.setWidth(rect.width() + widthOffset);

    // Just highlight word under pointer when cursor rectangle contain moue pointer coordinate.
    if ((rect.x() <= pos.x()) &&
            (pos.x() <= rect.x() + rect.width()) &&
            (rect.y() <= pos.y()) &&
            (pos.y() <= rect.y() + rect.height())) {
        // Move back to word bound start postion, and save cursor for convert case.
        m_wordUnderPointerCursor = cursor;
        m_wordUnderPointerCursor.select(QTextCursor::WordUnderCursor);
        m_wordUnderPointerCursor.setPosition(m_wordUnderPointerCursor.anchor(), QTextCursor::MoveAnchor);

        // Update highlight cursor.
        QTextEdit::ExtraSelection selection;

        selection.format.setBackground(m_selectionBgColor);
        selection.format.setForeground(m_selectionColor);
        selection.cursor = cursor;
        selection.cursor.select(QTextCursor::WordUnderCursor);

        //m_wordUnderCursorSelection = selection;

        renderAllSelections();

        return true;
    } else {
        return false;
    }
}

void TextEdit::removeHighlightWordUnderCursor()
{
    //m_highlightWordCacheCursor = m_wordUnderCursorSelection.cursor;

    QTextEdit::ExtraSelection selection;
    //m_wordUnderCursorSelection = selection;

    renderAllSelections();
    m_nBookMarkHoverLine = -1;
    m_pLeftAreaWidget->m_pBookMarkArea->update();
}

void TextEdit::setSettings(Settings *keySettings)
{
    m_settings = keySettings;
}


void TextEdit::copySelectedText()
{
    if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
        QString data;
        for (auto sel : m_altModSelections) {
            data.append(sel.cursor.selectedText());
        }
        QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
        clipboard->setText(data);
    } else {
        QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
        if (textCursor().hasSelection()) {
            clipboard->setText(textCursor().selection().toPlainText());
            tryUnsetMark();
        } else {
            clipboard->setText(m_highlightWordCacheCursor.selectedText());
        }
    }
    tryUnsetMark();
}

void TextEdit::cutSelectedText()
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(textCursor().selection().toPlainText());

    QTextCursor cursor = textCursor();
    cursor.removeSelectedText();
    setTextCursor(cursor);

    unsetMark();
}

void TextEdit::pasteText()
{
    QPlainTextEdit::paste();

    unsetMark();
}

void TextEdit::setMark()
{
    bool currentMark = m_cursorMark;
    bool markCursorChanged = false;

    if (m_cursorMark) {
        if (textCursor().hasSelection()) {
            markCursorChanged = true;

            QTextCursor cursor = textCursor();
            cursor.clearSelection();
            setTextCursor(cursor);
        } else {
            m_cursorMark = false;
        }
    } else {
        m_cursorMark = true;
    }

    if (m_cursorMark != currentMark || markCursorChanged) {
        cursorMarkChanged(m_cursorMark, textCursor());
    }
}

void TextEdit::unsetMark()
{
    bool currentMark = m_cursorMark;

    m_cursorMark = false;

    if (m_cursorMark != currentMark) {
        cursorMarkChanged(m_cursorMark, textCursor());
    }
}

bool TextEdit::tryUnsetMark()
{
    if (m_cursorMark) {
        QTextCursor cursor = textCursor();
        cursor.clearSelection();
        setTextCursor(cursor);

        unsetMark();

        return true;
    } else {
        return false;
    }
}

void TextEdit::exchangeMark()
{
    unsetMark();

    if (textCursor().hasSelection()) {
        // Record cursor and seleciton position before move cursor.
        int actionStartPos = textCursor().position();
        int selectionStartPos = textCursor().selectionStart();
        int selectionEndPos = textCursor().selectionEnd();

        QTextCursor cursor = textCursor();
        if (actionStartPos == selectionStartPos) {
            cursor.setPosition(selectionStartPos, QTextCursor::MoveAnchor);
            cursor.setPosition(selectionEndPos, QTextCursor::KeepAnchor);
        } else {
            cursor.setPosition(selectionEndPos, QTextCursor::MoveAnchor);
            cursor.setPosition(selectionStartPos, QTextCursor::KeepAnchor);
        }

        setTextCursor(cursor);
    }
}

void TextEdit::saveMarkStatus()
{
    m_cursorMarkStatus = m_cursorMark;
    m_cursorMarkPosition = textCursor().anchor();
}

void TextEdit::restoreMarkStatus()
{
    if (m_cursorMarkStatus) {
        QTextCursor currentCursor = textCursor();

        QTextCursor cursor = textCursor();
        cursor.setPosition(m_cursorMarkPosition, QTextCursor::MoveAnchor);
        cursor.setPosition(currentCursor.position(), QTextCursor::KeepAnchor);

        setTextCursor(cursor);
    }
}


void TextEdit::slot_translate()
{
    QProcess::startDetached("dbus-send  --print-reply --dest=com.iflytek.aiassistant /aiassistant/deepinmain com.iflytek.aiassistant.mainWindow.TextToTranslate");
}

QString TextEdit::getWordAtCursor()
{
    if (!characterCount()) {
        return "";
    } else {
        QTextCursor cursor = textCursor();
        QChar currentChar = toPlainText().at(std::max(cursor.position() - 1, 0));

        cursor.movePosition(QTextCursor::NoMove, QTextCursor::MoveAnchor);
        while (!currentChar.isSpace() && cursor.position() != 0) {
            // while (!currentChar.isSpace() && cursor.position() != 0) {
            cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
            currentChar = toPlainText().at(std::max(cursor.position() - 1, 0));

            if (currentChar == '-') {
                break;
            }
        }

        return cursor.selectedText();
    }
}

QString TextEdit::getWordAtMouse()
{
    if (!characterCount()) {
        return "";
    } else {
        auto pos = mapFromGlobal(QCursor::pos());
        QTextCursor cursor(cursorForPosition(pos));

        // Get cursor rectangle.
        auto rect = cursorRect(cursor);
        int widthOffset = 10;
        rect.setX(std::max(rect.x() - widthOffset / 2, 0));
        rect.setWidth(rect.width() + widthOffset);

        // Just highlight word under pointer when cursor rectangle contain mouse pointer coordinate.
        if ((rect.x() <= pos.x()) &&
                (pos.x() <= rect.x() + rect.width()) &&
                (rect.y() <= pos.y()) &&
                (pos.y() <= rect.y() + rect.height())) {
            cursor.select(QTextCursor::WordUnderCursor);

            return cursor.selectedText();
        } else {
            return "";
        }
    }
}

void TextEdit::toggleReadOnlyMode()
{
    if (m_readOnlyMode) {
        if (m_cursorMode == Overwrite) {
            emit cursorModeChanged(Overwrite);
        } else {
            emit cursorModeChanged(Insert);
        }
        setReadOnly(false);
        m_readOnlyMode = false;
        setCursorWidth(1);
        updateHighlightLineSelection();
        popupNotify(tr("Read-Only mode is off"));
    } else {
        m_readOnlyMode = true;
        setReadOnly(true);
        setCursorWidth(0); //????????????
        document()->clearUndoRedoStacks();
        updateHighlightLineSelection();
        popupNotify(tr("Read-Only mode is on"));
        emit cursorModeChanged(Readonly);
    }
}

void TextEdit::toggleComment(bool sister)
{
    if (m_readOnlyMode) {
        popupNotify(tr("Read-Only mode is on"));
        return;
    }

    //???????????????????????????????????? ?????????????????????????????????????????????????????????????????????
    bool bHasCommnent = false;
    if (m_commentDefinition.isValid()) {
        QString  multiLineCommentMark = m_commentDefinition.multiLineStart.simplified();
        QString  singleLineCommentMark = m_commentDefinition.singleLine.simplified();
        if (multiLineCommentMark.isEmpty() && singleLineCommentMark.isEmpty()) bHasCommnent = false;
        else bHasCommnent = true;
    }
    if (!bHasCommnent) return;

    const auto def = m_repository.definitionForFileName(QFileInfo(m_sFilePath).fileName());  //Java ,C++,HTML,
    QString name = def.name();
    if (name == "Markdown")
        return;

    if (!def.filePath().isEmpty()) {
        if (sister) {
            setComment();
        } else {
            removeComment();
            //unCommentSelection();
        }
    } else {
        // do not need to prompt the user.
        // popupNotify(tr("File does not support syntax comments"));
    }
}

int TextEdit::getNextWordPosition(QTextCursor &cursor, QTextCursor::MoveMode moveMode)
{
    // FIXME(rekols): if is empty text, it will crash.
    if (!characterCount()) {
        return 0;
    }

    // Move next char first.
    QTextCursor copyCursor = cursor;
    copyCursor.movePosition(QTextCursor::NextCharacter, moveMode);
    QChar *currentChar = copyCursor.selection().toPlainText().data();

    //cursor.movePosition(QTextCursor::NextCharacter, moveMode);
    //QChar currentChar = toPlainText().at(cursor.position() - 1);
    // Just to next non-space char if current char is space.
    if (currentChar->isSpace()) {
        while (copyCursor.position() < characterCount() - 1 && currentChar->isSpace()) {
            copyCursor.movePosition(QTextCursor::NextCharacter, moveMode);
            currentChar = copyCursor.selection().toPlainText().data();

            //cursor.movePosition(QTextCursor::NextCharacter, moveMode);
            //currentChar = toPlainText().at(cursor.position() - 1);
        }
        while (copyCursor.position() < characterCount() - 1 && !atWordSeparator(copyCursor.position())) {
            copyCursor.movePosition(QTextCursor::NextCharacter, moveMode);

            //cursor.movePosition(QTextCursor::NextCharacter, moveMode);
        }
    }
    // Just to next word-separator char.
    else {
        while (copyCursor.position() < characterCount() - 1 && !atWordSeparator(copyCursor.position())) {
            copyCursor.movePosition(QTextCursor::NextCharacter, moveMode);
            //cursor.movePosition(QTextCursor::NextCharacter, moveMode);
        }
    }

    return copyCursor.position();
}

int TextEdit::getPrevWordPosition(QTextCursor cursor, QTextCursor::MoveMode moveMode)
{
    if (!characterCount()) {
        return 0;
    }

    // Move prev char first.
    QTextCursor copyCursor = cursor;
    copyCursor.movePosition(QTextCursor::PreviousCharacter, moveMode);
    QChar *currentChar = copyCursor.selection().toPlainText().data();

//    cursor.movePosition(QTextCursor::PreviousCharacter, moveMode);
//    QChar currentChar = toPlainText().at(cursor.position());

    // Just to next non-space char if current char is space.
    if (currentChar->isSpace()) {
        while (copyCursor.position() > 0 && currentChar->isSpace()) {
            copyCursor.movePosition(QTextCursor::PreviousCharacter, moveMode);
            currentChar = copyCursor.selection().toPlainText().data();

//            cursor.movePosition(QTextCursor::PreviousCharacter, moveMode);
//            currentChar = toPlainText().at(cursor.position());
        }
    }
    // Just to next word-separator char.
    else {
        while (copyCursor.position() > 0 && !atWordSeparator(copyCursor.position())) {
            copyCursor.movePosition(QTextCursor::PreviousCharacter, moveMode);
            //cursor.movePosition(QTextCursor::PreviousCharacter, moveMode);
        }
    }

    return copyCursor.position();
}

bool TextEdit::atWordSeparator(int position)
{
    QTextCursor copyCursor = textCursor();
    copyCursor.setPosition(position, QTextCursor::MoveAnchor);
    copyCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QString currentChar = copyCursor.selection().toPlainText();
    return m_wordSepartors.contains(currentChar);
}

void TextEdit::showCursorBlink()
{
    // -1 ????????????Qt????????????
    QApplication::setCursorFlashTime(-1);
}

void TextEdit::hideCursorBlink()
{
    QApplication::setCursorFlashTime(0);
}

void TextEdit::setReadOnlyPermission(bool permission)
{
    m_bReadOnlyPermission = permission; //true????????????
    if (permission) {
        m_Permission2 = true;
        setReadOnly(true);
        emit cursorModeChanged(Readonly);
    } else {
        m_Permission = false;
        if (!m_readOnlyMode) {
            setReadOnly(false);
            emit cursorModeChanged(Insert);
        } else {
            setReadOnly(true);
            emit cursorModeChanged(Readonly);
        }
    }
    SendtoggleReadOnlyMode();
}

void TextEdit::SendtoggleReadmessage()
{
    if (!m_bReadOnlyPermission) {
        if (m_cursorMode == Overwrite) {
            emit cursorModeChanged(Overwrite);
        } else {
            emit cursorModeChanged(Insert);
        }
        setReadOnly(false);
        setCursorWidth(1);
        updateHighlightLineSelection();
    } else {
        setReadOnly(true);
        setCursorWidth(0); //????????????
        document()->clearUndoRedoStacks();
        updateHighlightLineSelection();
        emit cursorModeChanged(Readonly);
    }
}

void TextEdit::SendtoggleReadOnlyMode() {
    if(m_bReadOnlyPermission && !m_Permission) {
        m_Permission = m_bReadOnlyPermission;
        SendtoggleReadmessage();
    }
    else if(m_Permission2 && !m_bReadOnlyPermission){
        m_Permission2 = m_bReadOnlyPermission;
        SendtoggleReadmessage();
    }
}

bool TextEdit::getReadOnlyPermission()
{
    return m_bReadOnlyPermission;
}

bool TextEdit::getReadOnlyMode()
{
    return m_readOnlyMode;
}

void TextEdit::hideRightMenu()
{
    //arm????????????????????????????????????????????????????????????????????????????????????
    if (m_rightMenu)
        m_rightMenu->hide();
}

void TextEdit::bookMarkAreaPaintEvent(QPaintEvent *event)
{
    BookMarkWidget *bookMarkArea = m_pLeftAreaWidget->m_pBookMarkArea;
    QPainter painter(bookMarkArea);
    QColor lineNumberAreaBackgroundColor;
    if (QColor(m_backgroundColor).lightness() < 128) {
        lineNumberAreaBackgroundColor = palette().brightText().color();
        lineNumberAreaBackgroundColor.setAlphaF(0.06);

        m_lineNumbersColor.setAlphaF(0.2);
    } else {
        lineNumberAreaBackgroundColor = palette().brightText().color();
        lineNumberAreaBackgroundColor.setAlphaF(0.03);
        m_lineNumbersColor.setAlphaF(0.3);
    }
    //painter.fillRect(event->rect(), lineNumberAreaBackgroundColor);

//    int top = this->viewport()->geometry().top() + verticalScrollBar()->value();

    QTextBlock lineBlock;//??????????????????
    QImage image;
//    QImage scaleImage;
    QPixmap scalePixmap;
    QString pixmapPath;
//    int startPoint = 0;//??????????????????????????????
    int imageTop = 0;//??????????????????
    int fontHeight = fontMetrics().height();
    double nBookmarkLineHeight = fontHeight;
    QList<int> list = m_listBookmark;
    bool bIsContains = false;

    if (!m_listBookmark.contains(m_nBookMarkHoverLine) && m_nBookMarkHoverLine != -1 && m_nBookMarkHoverLine <= blockCount()) {
        list << m_nBookMarkHoverLine;
    } else {
        bIsContains = true;
    }

    foreach (auto line, list) {
        lineBlock = document()->findBlockByNumber(line - 1);
        QTextCursor cur = textCursor();
        cur.setPosition(lineBlock.position(), QTextCursor::MoveAnchor);
        if (line == m_nBookMarkHoverLine && !bIsContains) {
            if (DGuiApplicationHelper::instance()->themeType() == DGuiApplicationHelper::ColorType::DarkType) {
                pixmapPath = ":/images/like_hover_dark.svg";
                image = QImage(":/images/like_hover_dark.svg");
            } else {
                image = QImage(":/images/like_hover_light.svg");
                pixmapPath = ":/images/like_hover_light.svg";
            }
        } else {
            image = QImage(":/images/bookmark.svg");
            pixmapPath = ":/images/bookmark.svg";
        }

        if (line > 0) {
            lineBlock = document()->findBlockByNumber(line - 1);
            if (!lineBlock.isVisible()) {
                continue;
            }

            auto rate = devicePixelRatioF();
            image = image.scaled(image.width()*rate,image.height()*rate);

            scalePixmap = Utils::renderSVG(pixmapPath, QSize(image.height(), image.width()), false);
            scalePixmap.setDevicePixelRatio(devicePixelRatioF());
			#if 0
            if (fontHeight > image.height())
            {
                scalePixmap = Utils::renderSVG(pixmapPath, QSize(image.height(), image.width()), false);
                scalePixmap.setDevicePixelRatio(devicePixelRatioF());
            } else {
                double scale = nBookmarkLineHeight / image.height();
                int nScaleWidth = static_cast<int>(scale * image.width());
                scalePixmap = Utils::renderSVG(pixmapPath, QSize(static_cast<int>(scale * image.height()), nScaleWidth), false);
                scalePixmap.setDevicePixelRatio(devicePixelRatioF());
            }

            imageTop = cursorRect(cur).y() + (cursorRect(cur).height() - scalePixmap.height()) / 2;
            int nOffset = (m_pLeftAreaWidget->m_pBookMarkArea->width()  - scalePixmap.width()) / 2;
            painter.drawPixmap(nOffset, imageTop, scalePixmap);
			#endif

            //imageTop = cursorRect(cur).y() ;
           // int nOffset = (m_pLeftAreaWidget->m_pBookMarkArea->width()  - scalePixmap.width()) / 2;
           // painter.drawPixmap(0 , imageTop, scalePixmap);

            int height = cursorRect(cur).height();
            updateLeftWidgetWidth(height);
            QRect rect(0,cursorRect(cur).y(),height,height);
            rect = rect.adjusted(height/6,height/6,-1*height/6,-1*height/6);

            QSvgRenderer render;
            render.load(pixmapPath);
            render.render(&painter,rect);
        }
    }
}

int TextEdit::getLineFromPoint(const QPoint &point)
{
    QTextCursor cursor = cursorForPosition(point);
    QTextBlock block = cursor.block();
//    qDebug() << "block.blockNumber()" << block.blockNumber();
    return block.blockNumber() + 1;
}

void TextEdit::addOrDeleteBookMark()
{
    int line = 0;
    if (m_bIsShortCut) {
        line = getCurrentLine();
        m_bIsShortCut = false;
    } else {
        line = getLineFromPoint(m_mouseClickPos);
    }

    if (line > blockCount()) {
        return;
    }

    if (m_listBookmark.contains(line)) {
        m_listBookmark.removeOne(line);
        m_nBookMarkHoverLine = -1;
        qDebug() << "DeleteBookMark:" << line << m_listBookmark;
    } else {
        m_listBookmark.push_back(line);
        qDebug() << "AddBookMark:" << line << m_listBookmark;
    }

    m_pLeftAreaWidget->m_pBookMarkArea->update();
}

void TextEdit::moveToPreviousBookMark()
{
    int line = getCurrentLine();
    int index = m_listBookmark.indexOf(line);

    if (index == -1 && !m_listBookmark.isEmpty()) {
        jumpToLine(m_listBookmark.last(), false);
        return;
    }

    if (index == 0) {
        jumpToLine(m_listBookmark.last(), false);
    } else {
        jumpToLine(m_listBookmark.value(index - 1), false);
    }
}

void TextEdit::moveToNextBookMark()
{
    int line = getCurrentLine();
    int index = m_listBookmark.indexOf(line);

    if (index == -1 && !m_listBookmark.isEmpty()) {
        jumpToLine(m_listBookmark.first(), false);
        return;
    }

    if (index == m_listBookmark.count() - 1) {
        jumpToLine(m_listBookmark.first(), false);
    } else {
        jumpToLine(m_listBookmark.value(index + 1), false);
    }
}

void TextEdit::checkBookmarkLineMove(int from, int charsRemoved, int charsAdded)
{
    Q_UNUSED(charsRemoved);
    Q_UNUSED(charsAdded);

    if (m_bIsFileOpen) {
        return;
    }

    if (m_nLines != blockCount()) {
        QTextCursor cursor = textCursor();
        int nAddorDeleteLine = document()->findBlock(from).blockNumber() + 1;
        int currLine = textCursor().blockNumber() + 1;

        if (m_nLines > blockCount()) {
            foreach (const auto line, m_listBookmark) {
                if (m_nSelectEndLine != -1) {
                    if (nAddorDeleteLine < line && line <= m_nSelectEndLine) {
                        m_listBookmark.removeOne(line);
                    } else if (line > m_nSelectEndLine) {
                        m_listBookmark.replace(m_listBookmark.indexOf(line), line - m_nSelectEndLine + nAddorDeleteLine);
                    }
                } else {
                    if (line == currLine + 1) {
                        m_listBookmark.removeOne(currLine + 1);
                    } else if (line > currLine + 1) {
                        m_listBookmark.replace(m_listBookmark.indexOf(line), line  - m_nLines + blockCount());
                    }
                }
            }
        } else {
            foreach (const auto line, m_listBookmark) {

                if (nAddorDeleteLine < line) {
                    m_listBookmark.replace(m_listBookmark.indexOf(line), line + blockCount() - m_nLines);
                }
            }
        }
    }
    m_nLines = blockCount();
}

void TextEdit::flodOrUnflodAllLevel(bool isFlod)
{
    m_listMainFlodAllPos.clear();
    //??????
    if (isFlod) {
        for (int line = 0; line < document()->blockCount(); line++) {
            if (blockContainStrBrackets(line)
                && document()->findBlockByNumber(line).isVisible()
                && !document()->findBlockByNumber(line).text().trimmed().startsWith("//")) {
                if (getNeedControlLine(line, false)) {
                    m_listMainFlodAllPos.append(line);
                }
            }
        }
    //??????
    } else {
        for (int line = 0; line < document()->blockCount(); line++) {
            if (blockContainStrBrackets(line)
                && !document()->findBlockByNumber(line + 1).isVisible()
                && !document()->findBlockByNumber(line).text().trimmed().startsWith("//")) {
                if (getNeedControlLine(line, true)) {
                    m_listMainFlodAllPos.append(line);
                }
            }
        }
    }

    //??????????????????????????????????????????
    QPlainTextEdit::LineWrapMode curMode = this->lineWrapMode();
    QPlainTextEdit::LineWrapMode WrapMode = curMode ==  QPlainTextEdit::NoWrap ?  QPlainTextEdit::WidgetWidth :  QPlainTextEdit::NoWrap;
    this->setWordWrapMode(QTextOption::WrapAnywhere);
    this->setLineWrapMode(WrapMode);

    m_pLeftAreaWidget->m_pFlodArea->update();
    m_pLeftAreaWidget->m_pLineNumberArea->update();
    m_pLeftAreaWidget->m_pBookMarkArea->update();

    viewport()->update();
    document()->adjustSize();

    this->setLineWrapMode(curMode);
    viewport()->update();
}

void TextEdit::flodOrUnflodCurrentLevel(bool isFlod)
{
    int line = getLineFromPoint(m_mouseClickPos);
    getNeedControlLine(line - 1, !isFlod);
    m_pLeftAreaWidget->m_pFlodArea->update();
    m_pLeftAreaWidget->m_pLineNumberArea->update();
    m_pLeftAreaWidget->m_pBookMarkArea->update();
    viewport()->update();
    document()->adjustSize();

}

void TextEdit::getHideRowContent(int iLine)
{
    //???????????? ?????????????????? ????????????"{""}"????????????
    QTextDocument *doc = document();
    //?????????????????????
    QTextBlock curBlock = doc->findBlockByNumber(iLine);

    //?????????????????? ??????????????????
    QTextBlock beginBlock = curBlock.next(), endBlock = curBlock.next();

    //????????????????????????????????????"{"
    if (iLine == 0 && !curBlock.text().contains("{")) {
        curBlock = curBlock.next();
    }

    //???????????????????????????
    int position = curBlock.position();
    int offset = curBlock.text().lastIndexOf('{');
    position += offset;

    //??????????????????????????????????????????
    QChar begin = '{', end = '}';
    QTextCursor cursor = textCursor();
    cursor.setPosition(position, QTextCursor::MoveAnchor);
    cursor.clearSelection();

    //??????????????? ???????????????
    QTextCursor bracketBeginCursor = cursor;
    QTextCursor bracketEndCursor = cursor;
    bracketBeginCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);

    //???????????????????????????
    int braceDepth = 0;
    QChar c;
    while (!(c = doc->characterAt(position)).isNull()) {
        if (c == begin) {
            braceDepth++;
        } else if (c == end) {
            braceDepth--;

            if (0 == braceDepth) {
                bracketEndCursor = QTextCursor(doc);
                bracketEndCursor.setPosition(position);
                bracketEndCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                //??????????????????????????????
                endBlock = bracketEndCursor.block();
                break;
            }
        }
        position++;
    }

    if (QColor(m_backgroundColor).lightness() < 128) {
        m_foldCodeShow->initHighLight(m_sFilePath, false);
    } else {
        m_foldCodeShow->initHighLight(m_sFilePath, true);
    }

    m_foldCodeShow->appendText("{", width());
    //???????????????????????????
    if (braceDepth != 0) {
        //?????????????????????????????? ????????????????????????
        while (beginBlock.isValid()) {
            m_foldCodeShow->appendText(beginBlock.text(), width());
            beginBlock = beginBlock.next();
        }

        //????????????"{" "}"?????????????????????
    } else if (endBlock == curBlock) {

        return;
    } else {
        //?????????????????????????????? ????????????????????????
        while (beginBlock != endBlock && beginBlock.isValid()) {
            if (beginBlock.isValid()) {
                m_foldCodeShow->appendText(beginBlock.text(), width());
            }

            beginBlock = beginBlock.next();
        }

        if (endBlock.text().simplified() == "}") {
            m_foldCodeShow->appendText("}", width());
        }

        m_foldCodeShow->appendText("}", width());
    }
}

bool TextEdit::isNeedShowFoldIcon(QTextBlock block)
{
    QString blockText = block.text();
    bool hasFindLeft = false; // ?????????????????????????????????????????????
    int rightNum = 0, leftNum = 0;//?????????????????????????????????
    for (int i = 0 ; i < blockText.count(); ++i) {
        if (blockText.at(i) == "}" && hasFindLeft) {
            rightNum++;
        } else if (blockText.at(i) == "{") {
            if (!hasFindLeft)
                hasFindLeft = true;
            leftNum++;
        }
    }

    if (rightNum == leftNum) {
        return  false;
    } else {
        return  true;
    }
}

int TextEdit::getHighLightRowContentLineNum(int iLine)
{

    bool isFirstLine = true;
    if (iLine == 0) isFirstLine = true;
    else isFirstLine = false;

    QTextDocument *doc = document();
    //?????????????????????
    QTextBlock curBlock = doc->findBlockByNumber(iLine);

    //??????????????? ???????????????
    QTextBlock beginBlock = curBlock.next(), endBlock = curBlock.next();

    //????????????????????????????????????"{"
    if (iLine == 0 && !curBlock.text().contains("{")) {
        curBlock = curBlock.next();
    }

    //??????????????????????????????????????????
    int position = curBlock.position();
    int offset = curBlock.text().lastIndexOf('{');
    position += offset;


    //??????????????????????????????????????????
    QChar begin = '{', end = '}';
    QTextCursor cursor = textCursor();
    cursor.setPosition(position, QTextCursor::MoveAnchor);
    cursor.clearSelection();

    //??????????????? ???????????????
    QTextCursor bracketBeginCursor = cursor;
    QTextCursor bracketEndCursor = cursor;
    bracketBeginCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);

    //???????????????????????????
    int braceDepth = 0;

    QChar c;
    while (!(c = doc->characterAt(position)).isNull()) {

        if (c == begin) {
            braceDepth++;
        } else if (c == end) {
            braceDepth--;

            if (0 == braceDepth) {
                bracketEndCursor = QTextCursor(doc);
                bracketEndCursor.setPosition(position);
                bracketEndCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                endBlock = bracketEndCursor.block();
                break;
            }
        }
        position++;
    }

    //???????????????????????????
    if (braceDepth != 0) {
        //?????????????????????????????? ????????????????????????
        while (beginBlock.isValid()) {
            iLine++;
            beginBlock = beginBlock.next();
        }
        return iLine;
        //????????????"{" "}"?????????????????????
    } else if (0 != braceDepth || endBlock == curBlock) {
        return iLine;
    } else {
        while (beginBlock != endBlock && beginBlock.isValid()) {
            iLine++;
            beginBlock = beginBlock.next();
        }

        iLine++;
        return iLine;
    }
}

void TextEdit::paintCodeFlod(QPainter *painter, QRect rect, bool flod)
{
    painter->save();

    painter->translate(rect.width()/2.0,rect.height()/2.0);
    painter->translate(0,rect.y());
    QPainterPath path;
    //QPointF p1(rect.x()+rect.width() * (1.0/3),rect.y()+rect.height()*(1.0/3));
    //QPointF p2(rect.x()+rect.width() * (2.0/3),rect.y()+rect.height()*(1.0/3));
    //QPointF p3(rect.x()+rect.width() * (1.0/2),rect.y()+rect.height()*(1.5/3));
    QPointF p1(-1*rect.width() * (1.0/6),-1*rect.height()*(1.0/6));
    QPointF p2(rect.width() * (1.0/6),-1*rect.height()*(1.0/6));
    QPointF p3(0,0);
    path.moveTo(p1);
    path.lineTo(p3);
    path.lineTo(p2);

    if(flod)
        painter->rotate(-90);

    QPen pen(this->palette().foreground(),2);
    painter->setPen(pen);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->drawPath(path);

    painter->restore();
}

QColor TextEdit::getBackColor()
{
    return m_backgroundColor;
}

int TextEdit::lineNumberAreaWidth()
{
    int digits = 1;
    int max = qMax(1, this->document()->blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }

    return  fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits ;
}

void TextEdit::updateLeftWidgetWidth(int width)
{
    m_pLeftAreaWidget->m_pFlodArea->setFixedWidth(width);
    m_pLeftAreaWidget->m_pLineNumberArea->setFixedWidth(lineNumberAreaWidth());
    m_pLeftAreaWidget->m_pBookMarkArea->setFixedWidth(width);
}

int TextEdit::getLinePosYByLineNum(int iLine)
{
    QTextBlock block = document()->findBlockByNumber(iLine);
    QTextCursor cur = textCursor();

    while (!block.isVisible()) {
        block = block.next();
    }

    cur.setPosition(block.position(), QTextCursor::MoveAnchor);
    return cursorRect(cur).y();
}

bool TextEdit::ifHasHighlight()
{
    if (!m_findHighlightSelection.cursor.isNull())
        return m_findHighlightSelection.cursor.hasSelection();
    else {
        return  false;
    }
}

void TextEdit::setIsFileOpen()
{
    m_bIsFileOpen = true;
}

void TextEdit::setTextFinished()
{
    m_bIsFileOpen = false;
    m_nLines = blockCount();

    if (!m_listBookmark.isEmpty()) {
        return;
    }

    QStringList bookmarkList = readHistoryRecordofBookmark();
    QStringList filePathList = readHistoryRecordofFilePath("advance.editor.browsing_history_file");
    QList<int> linesList;

    QString qstrPath = m_sFilePath;

    if (filePathList.contains(qstrPath)) {
        int index = 2;
        QString qstrLines = bookmarkList.value(filePathList.indexOf(qstrPath));
        QString sign;

        for (int i = 0; i < qstrLines.count() - 1; i++) {
            sign = qstrLines.at(i);
            sign.append(qstrLines.at(i + 1));

            if (sign == ",*" || sign == ")*") {
                linesList << qstrLines.mid(index, i - index).toInt();
                index = i + 2;
            }
        }
    }

    foreach (const auto line, linesList) {
        if (line <= document()->blockCount()) {
            if (!m_listBookmark.contains(line)) {
                m_listBookmark << line;
            }
        }
    }
//    qDebug() << m_listBookmark << document()->blockCount();
}

QStringList TextEdit::readHistoryRecord(QString key)
{
    QString history = m_settings->settings->option(key)->value().toString();
    QStringList historyList;
    int nLeftPosition = history.indexOf("*{");
    int nRightPosition = history.indexOf("}*");

    while (nLeftPosition != -1) {
        historyList << history.mid(nLeftPosition, nRightPosition + 2 - nLeftPosition);
        nLeftPosition = history.indexOf("*{", nLeftPosition + 2);
        nRightPosition = history.indexOf("}*", nRightPosition + 2);
    }

    return historyList;
}

QStringList TextEdit::readHistoryRecordofBookmark()
{
    QString history = m_settings->settings->option("advance.editor.browsing_history_file")->value().toString();
    QStringList bookmarkList;
    int nLeftPosition = history.indexOf("*(");
    int nRightPosition = history.indexOf(")*");

    while (nLeftPosition != -1) {
        bookmarkList << history.mid(nLeftPosition, nRightPosition + 2 - nLeftPosition);
        nLeftPosition = history.indexOf("*(", nLeftPosition + 2);
        nRightPosition = history.indexOf(")*", nRightPosition + 2);
    }

    return bookmarkList;
}

QStringList TextEdit::readHistoryRecordofFilePath(QString key)
{
    QString history = m_settings->settings->option(key)->value().toString();
    QStringList filePathList;
    int nLeftPosition = history.indexOf("*[");
    int nRightPosition = history.indexOf("]*");

    while (nLeftPosition != -1) {
        filePathList << history.mid(nLeftPosition + 2, nRightPosition - 2 - nLeftPosition);
        nLeftPosition = history.indexOf("*[", nLeftPosition + 2);
        nRightPosition = history.indexOf("]*", nRightPosition + 2);
    }

    return filePathList;
}

void TextEdit::writeHistoryRecord()
{
    QString history = m_settings->settings->option("advance.editor.browsing_history_file")->value().toString();
    QStringList historyList = readHistoryRecord("advance.editor.browsing_history_file");

    int nLeftPosition = history.indexOf(m_sFilePath);
    int nRightPosition = history.indexOf("}*", nLeftPosition);
    if (history.contains(m_sFilePath)) {
        history.remove(nLeftPosition - 4, nRightPosition + 6 - nLeftPosition);
    }

    if (!m_listBookmark.isEmpty()) {

        QString filePathandBookmarkLine = "*{*[" + m_sFilePath + "]**(";

        foreach (auto line, m_listBookmark) {
            if (line <= 0) {
                line = 1;
            }

            filePathandBookmarkLine += QString::number(line) + ",*";
        }

        filePathandBookmarkLine.remove(filePathandBookmarkLine.count() - 2, 2);

        if (historyList.count() <= 5) {
//            qDebug() << "writeHistoryRecord()" <<  filePathandBookmarkLine + ")}" + history;
            m_settings->settings->option("advance.editor.browsing_history_file")->setValue(filePathandBookmarkLine + ")*}*" + history);
        } else {
            history.remove(historyList.last());
            m_settings->settings->option("advance.editor.browsing_history_file")->setValue(filePathandBookmarkLine + ")*}*" + history);
        }
    } else {
        m_settings->settings->option("advance.editor.browsing_history_file")->setValue(history);
    }
}

void TextEdit::writeEncodeHistoryRecord()
{
    qDebug() << "writeHistoryRecord";
    QString history = m_settings->settings->option("advance.editor.browsing_encode_history")->value().toString();

    QStringList pathList = readHistoryRecordofFilePath("advance.editor.browsing_encode_history");

    foreach (auto path, pathList) {
        QFileInfo f(path);
        if (!f.isFile()) {
            int nLeftPosition = history.indexOf(path);
            int nRightPosition = history.indexOf("}*", nLeftPosition);
            history.remove(nLeftPosition - 4, nRightPosition + 6 - nLeftPosition);
        }
    }

    int nLeftPosition = history.indexOf(m_sFilePath);
    int nRightPosition = history.indexOf("}*", nLeftPosition);

    if (history.contains(m_sFilePath)) {
        history.remove(nLeftPosition - 4, nRightPosition + 6 - nLeftPosition);
    }

    QString encodeHistory = history + "*{*[" + m_sFilePath + "]*" + m_textEncode + "}*";
    m_settings->settings->option("advance.editor.browsing_encode_history")->setValue(encodeHistory);
}

QStringList TextEdit::readEncodeHistoryRecord()
{
    QString history = m_settings->settings->option("advance.editor.browsing_encode_history")->value().toString();
    QStringList filePathList;
    int nLeftPosition = history.indexOf("]*");
    int nRightPosition = history.indexOf("}*");

    while (nLeftPosition != -1) {
        filePathList << history.mid(nLeftPosition + 2, nRightPosition - 2 - nLeftPosition);
        nLeftPosition = history.indexOf("]*", nLeftPosition + 2);
        nRightPosition = history.indexOf("}*", nRightPosition + 2);
    }

    return filePathList;
}



void TextEdit::tellFindBarClose()
{
    m_bIsFindClose = true;
}

void TextEdit::setEditPalette(const QString &activeColor, const QString &inactiveColor)
{
    QPalette pa = this->palette();
    pa.setColor(QPalette::Inactive, QPalette::Text, QColor(inactiveColor));
    pa.setColor(QPalette::Active, QPalette::Text, QColor(activeColor));
    setPalette(pa);
}

void TextEdit::setCodeFoldWidgetHide(bool isHidden)
{
    if (m_foldCodeShow) {
        m_foldCodeShow->setHidden(isHidden);
    }
}


void TextEdit::setTruePath(QString qstrTruePath)
{
    m_qstrTruePath = qstrTruePath;
}

QString TextEdit::getTruePath()
{
    if (m_qstrTruePath.isEmpty()) {
        return m_sFilePath;
    }

    return  m_qstrTruePath;
}

QList<int> TextEdit::getBookmarkInfo()
{
    return m_listBookmark;
}

void TextEdit::setBookMarkList(QList<int> bookMarkList)
{
    m_listBookmark = bookMarkList;
}

void TextEdit::updateSaveIndex()
{
    m_lastSaveIndex = m_pUndoStack->index();
}

void TextEdit::isMarkCurrentLine(bool isMark, QString strColor,  qint64 timeStamp)
{
    qint64 operationTimeStamp = timeStamp;
    if(operationTimeStamp < 0) {
        operationTimeStamp = QDateTime::currentDateTime().toMSecsSinceEpoch();
    }

    if (isMark) {
        QTextEdit::ExtraSelection selection;
        selection.cursor = textCursor();
        selection.format.setBackground(QColor(strColor));

        TextEdit::MarkOperation markOperation;
        markOperation.color = strColor;

        if (textCursor().hasSelection()) {
            markOperation.type = MarkOnce;
        } else {
            markOperation.type = MarkLine;
            int beginPos = textCursor().block().position();
            int endPos = beginPos + textCursor().block().length() - 1;
            selection.cursor.setPosition(beginPos, QTextCursor::MoveAnchor);
            selection.cursor.setPosition(endPos, QTextCursor::KeepAnchor);
        }
        markOperation.cursor = selection.cursor;

        m_markOperations.append(QPair<TextEdit::MarkOperation, qint64>(markOperation, operationTimeStamp));
        m_wordMarkSelections.append(
                    QPair<QTextEdit::ExtraSelection, qint64>
                    (selection, operationTimeStamp));
    } else {
        clearMarksForTextCursor();
    }
}

void TextEdit::markAllKeywordInView()
{
    if (m_markOperations.isEmpty()) {
        return;
    }

    QList<QPair<TextEdit::MarkOperation, qint64>>::iterator it;

    for (it = m_markOperations.begin(); it != m_markOperations.end(); ++it) {
        if (MarkAllMatch == it->first.type) {
            QString keyword = it->first.cursor.selectedText();
            markKeywordInView(keyword, it->first.color, it->second);
        } else if (MarkAll == it->first.type) {
            markAllInView(it->first.color, it->second);
        }
    }

    renderAllSelections();
}

bool TextEdit::markKeywordInView(QString keyword, QString color, qint64 timeStamp)
{
    qint64 operationTimeStamp = timeStamp;
    if(operationTimeStamp < 0) {
        operationTimeStamp = QDateTime::currentDateTime().toMSecsSinceEpoch();
    }

    if (keyword.isEmpty()) {
        return false;
    }

    QList<QTextEdit::ExtraSelection> listExtraSelection;
    QTextCharFormat format;
    bool ret = false;

    format.setBackground(QColor(color));
    ret = updateKeywordSelectionsInView(keyword, format, &listExtraSelection);

    // ???????????????????????? listExtraSelectionWithTimeStamp
    QList<QPair<QTextEdit::ExtraSelection, qint64>> listExtraSelectionWithTimeStamp;
    for(int i = 0; i < listExtraSelection.size(); i++) {
        listExtraSelectionWithTimeStamp.append(QPair<QTextEdit::ExtraSelection, qint64>
                                               (listExtraSelection.at(i), operationTimeStamp));
    }

    if (ret) {
        m_mapKeywordMarkSelections[keyword] = listExtraSelectionWithTimeStamp;
    }

    return ret;
}

void TextEdit::markAllInView(QString color, qint64 timeStamp)
{
    // ???????????????
    qint64 operationTimeStamp = timeStamp;
    if(operationTimeStamp < 0) {
        operationTimeStamp = QDateTime::currentDateTime().toMSecsSinceEpoch();
    }

    QTextEdit::ExtraSelection selection;
    QList<QPair<QTextEdit::ExtraSelection, qint64>> listSelections;

    // ??????????????????????????????
    // QScrollBar *pScrollBar = verticalScrollBar();
    // QPoint startPoint = QPointF(0, 0).toPoint();
    // QTextBlock beginBlock = cursorForPosition(startPoint).block();
    // QTextBlock endBlock;
    //
    // if (pScrollBar->maximum() > 0) {
    //     QPoint endPoint = QPointF(0, 1.5 * height()).toPoint();
    //     endBlock = cursorForPosition(endPoint).block();
    // } else {
    //     endBlock = document()->lastBlock();
    // }

    // selection.cursor = textCursor();
    // selection.cursor.setPosition(beginBlock.position(), QTextCursor::MoveAnchor);
    // selection.cursor.setPosition(endBlock.position() + endBlock.length() - 1, QTextCursor::KeepAnchor);

    // ?????? QTextCursor ????????????
    QTextCursor textCursor(document());
    textCursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
    textCursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);

    selection.format.setBackground(QColor(color));
    selection.cursor = textCursor;

    listSelections.append(QPair<QTextEdit::ExtraSelection, qint64>
                          (selection, operationTimeStamp));

    m_mapKeywordMarkSelections[TEXT_EIDT_MARK_ALL] = listSelections;
}

void TextEdit::isMarkAllLine(bool isMark, QString strColor)
{
    // ???????????????
    qint64 timeStamp = QDateTime::currentDateTime().toMSecsSinceEpoch();

    if (isMark) {
        QString selectionText = textCursor().selectedText();
        if (selectionText.length() != 0 && selectionText.length() < (document()->characterCount() - 1)) {
            // ??????????????????????????????
            // QList<QTextEdit::ExtraSelection> wordMarkSelections = m_wordMarkSelections;
            QList<QTextEdit::ExtraSelection> listExtraSelection;
            // ??????????????????????????????
            // QList<QTextEdit::ExtraSelection> listSelections;
            QTextEdit::ExtraSelection  extraSelection;
            QTextCharFormat format;
            format.setBackground(QColor(strColor));
            extraSelection.cursor = textCursor();
            extraSelection.format = format;

            TextEdit::MarkOperation markOperation;
            markOperation.type = MarkAllMatch;
            markOperation.cursor = textCursor();
            markOperation.color = strColor;
            m_markOperations.append(QPair<TextEdit::MarkOperation, qint64>(markOperation, timeStamp));

            if (updateKeywordSelectionsInView(selectionText, format, &listExtraSelection)) {

                QList<QPair<QTextEdit::ExtraSelection, qint64>> listExtraSelectionWithTimeStamp;
                for(int i = 0; i < listExtraSelection.size(); i++) {
                    listExtraSelectionWithTimeStamp.append(QPair<QTextEdit::ExtraSelection, qint64>
                                                           (listExtraSelection.at(i), timeStamp));
                }

                m_mapKeywordMarkSelections[selectionText] = listExtraSelectionWithTimeStamp;
            } else {
                // ?????????????????????????????????????????????????????????????????????????????????
                QTextEdit::ExtraSelection extraSelect;
                listExtraSelection.append(extraSelection);
                QList<QPair<QTextEdit::ExtraSelection, qint64>> listExtraSelectionWithTimeStamp;
                for(int i = 0; i < listExtraSelection.size(); i++) {
                    listExtraSelectionWithTimeStamp.append(QPair<QTextEdit::ExtraSelection, qint64>
                                                           (listExtraSelection.at(i), timeStamp));
                }

                m_mapKeywordMarkSelections.insert(selectionText, listExtraSelectionWithTimeStamp);
            }

        } else if (!textCursor().hasSelection() || selectionText.length() == (document()->characterCount() - 1)) {
            TextEdit::MarkOperation markOperation;
            markOperation.type = MarkAll;
            markOperation.color = strColor;
            m_markOperations.append(QPair<TextEdit::MarkOperation, qint64>(markOperation, timeStamp));
            markAllInView(strColor, timeStamp);
        }
    } else {
        m_markOperations.clear();
        m_wordMarkSelections.clear();
        m_mapKeywordMarkSelections.clear();

        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(QColor(strColor));
        selection.cursor = textCursor();
        selection.cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        selection.cursor.clearSelection();
        m_markAllSelection = selection;
    }

    renderAllSelections();
}

void TextEdit::cancelLastMark()
{
    if (m_markOperations.isEmpty()) {
        return;
    }

    switch (m_markOperations.last().first.type) {
        case MarkOnce:
        case MarkLine: {
            if (!m_wordMarkSelections.isEmpty()) {
                // m_wordMarkSelections.removeLast();
				// ?????????????????????????????????????????????????????????????????????selection
                const qint64 operationTimeStamp = m_markOperations.last().second;
                for(int i = 0; i < m_wordMarkSelections.size(); i++) {
                    if(operationTimeStamp == m_wordMarkSelections.at(i).second) {
                        m_wordMarkSelections.removeAt(i);
                        i--;
                    }
                }
            }
            break;
        }

        case MarkAllMatch: {
            // QString keyword = m_markOperations.last().first.cursor.selectedText();
            QString keyword;
            qint64 timeStamp = m_markOperations.last().second;
            // ????????????????????? keyword
            QMap<QString, QList<QPair<QTextEdit::ExtraSelection, qint64>>>::Iterator it;
            for (it = m_mapKeywordMarkSelections.begin(); it != m_mapKeywordMarkSelections.end(); ++it) {
                if(it.value().size() > 0){
                    qint64 itsTimeStamp = it.value().first().second;
                    if(itsTimeStamp == timeStamp) {
                        keyword = it.key();
                        break;
                    }
                }
            }

            if (m_mapKeywordMarkSelections.contains(keyword)) {
                m_mapKeywordMarkSelections.remove(keyword);
            }
            break;
        }

        case MarkAll: {
            if (m_mapKeywordMarkSelections.contains(TEXT_EIDT_MARK_ALL)) {
                m_mapKeywordMarkSelections.remove(TEXT_EIDT_MARK_ALL);
            }
            break;
        }

    }

    m_markOperations.removeLast();

    // ???????????????????????????????????????????????????????????????????????????????????????????????????
    if(m_markOperations.isEmpty() &&
            (m_wordMarkSelections.size() > 0 || m_mapKeywordMarkSelections.size() > 0)) {
        qWarning() << __FUNCTION__ << __LINE__ << " cancle mark color operation,"
                   << "find exist remain selections, will clear!";
        m_wordMarkSelections.clear();
        m_mapKeywordMarkSelections.clear();
    }

    renderAllSelections();
}

bool TextEdit::clearMarkOperationForCursor(QTextCursor cursor)
{
    bool bRet = false;
    for (int i = m_markOperations.size() - 1; i >= 0; --i) {
        if (m_markOperations.at(i).first.cursor == cursor) {
            m_markOperations.removeAt(i);
            bRet = true;
            break;
        }
    }

    return bRet;
}

bool TextEdit::clearMarksForTextCursor()
{
    bool bFind = false;
    QTextCursor cursor;
    QTextCursor textcursor = textCursor();

    for (int i = m_wordMarkSelections.size() - 1; i >= 0; --i) {
        cursor = m_wordMarkSelections.at(i).first.cursor;
        if (textcursor.hasSelection()) {
            if (textcursor == cursor) {
                bFind = true;
                clearMarkOperationForCursor(cursor);
                m_wordMarkSelections.removeAt(i);
                break;
            }

        } else {
            if (textcursor.position() >= cursor.selectionStart() && textcursor.position() <= cursor.selectionEnd()) {
                bFind = true;
                clearMarkOperationForCursor(cursor);
                m_wordMarkSelections.removeAt(i);
            }
        }
    }

    return bFind;
}

void TextEdit::toggleMarkSelections()
{
    if (!clearMarksForTextCursor()) {
        ColorSelectWdg *pColorSelectWdg = static_cast<ColorSelectWdg *>(m_actionColorStyles->defaultWidget());
        isMarkCurrentLine(true, pColorSelectWdg->getDefaultColor().name());
    }

    renderAllSelections();
}

void TextEdit::markSelectWord()
{
    bool isFind  = false;
    for (int i = 0 ; i < m_wordMarkSelections.size(); ++i) {
        QTextCursor curson = m_wordMarkSelections.at(i).first.cursor;
        curson.movePosition(QTextCursor::EndOfLine);
        QTextCursor currentCurson = textCursor();
        currentCurson.movePosition(QTextCursor::EndOfLine);
        //if (m_wordMarkSelections.at(i).cursor == textCursor()) {
        if (curson == currentCurson) {
            isFind = true;
            m_wordMarkSelections.removeAt(i);
            renderAllSelections();
            break;
        }
    }
    if (!isFind) {
        //???????????????????????????
        ColorSelectWdg *pColorSelectWdg = static_cast<ColorSelectWdg *>(m_actionColorStyles->defaultWidget());
        isMarkCurrentLine(true, pColorSelectWdg->getDefaultColor().name());
        renderAllSelections();
    }
}

void TextEdit::updateMark(int from, int charsRemoved, int charsAdded)
{
    //????????????????????????????????????????????????
    if (m_readOnlyMode) {
        //undo();
        return;
    }

    //??????????????????????????????????????????
    if (m_bIsFileOpen) {
        return;
    }

    //????????????
    int nStartPos = 0,///< ?????????????????????
        nEndPos = 0,///< ?????????????????????
        nCurrentPos = 0;///< ??????????????????
    QTextEdit::ExtraSelection selection;///< ??????????????????
    QList<QTextEdit::ExtraSelection> listSelections;///< ????????????????????????
    QList<QPair<QTextEdit::ExtraSelection, qint64>> wordMarkSelections = m_wordMarkSelections;///< ????????????
    QColor strColor;///< ????????????????????????
    nCurrentPos = textCursor().position();

    //?????????????????????
    if (charsRemoved > 0) {
        QList<int> listRemoveItem;///< ??????????????????indexs

        //????????????????????????index
        for (int i = 0; i < wordMarkSelections.count(); i++) {

            nEndPos = wordMarkSelections.value(i).first.cursor.selectionEnd();
            nStartPos = wordMarkSelections.value(i).first.cursor.selectionStart();
            strColor = wordMarkSelections.value(i).first.format.background().color();

            //????????????????????????
            if (m_nSelectEndLine != -1) {

                //????????????????????????????????????????????????
                if (m_nSelectStart <= nStartPos && m_nSelectEnd >= nEndPos) {
                    listRemoveItem.append(i);
                }
            } else {

                //?????????????????????????????????
                if (wordMarkSelections.value(i).first.cursor.selection().toPlainText().isEmpty()) {
                    // ?????? remove m_wordMarkSelections????????????
                    // ????????? m_wordMarkSelections ??? wordMarkSelections??????????????????
                    // m_wordMarkSelections.removeAt(i);
                    listRemoveItem.append(i);
                }
            }
        }

        //??????????????????????????????
        for (int j = 0; j < listRemoveItem.count(); j++) {
            // ?????????-j
            // m_wordMarkSelections.removeAt(listRemoveItem.value(j) - j);
            m_wordMarkSelections.removeAt(listRemoveItem.value(j));
        }
    }

    //?????????????????????
    if (charsAdded > 0) {
        for (int i = 0; i < wordMarkSelections.count(); i++) {
            nEndPos = wordMarkSelections.value(i).first.cursor.selectionEnd();
            nStartPos = wordMarkSelections.value(i).first.cursor.selectionStart();
            strColor = wordMarkSelections.value(i).first.format.background().color();
            qint64 timeStamp = wordMarkSelections.value(i).second;

            //??????????????????????????????
            if (nCurrentPos > nStartPos && nCurrentPos < nEndPos) {

                m_wordMarkSelections.removeAt(i);
                selection.format.setBackground(strColor);
                selection.cursor = textCursor();

                QTextEdit::ExtraSelection preSelection;

                //????????????????????????
                if (m_bIsInputMethod) {

                    //?????????????????????
                    selection.cursor.setPosition(nStartPos, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nCurrentPos - m_qstrCommitString.count(), QTextCursor::KeepAnchor);
                    m_wordMarkSelections.insert(i, QPair<QTextEdit::ExtraSelection, qint64>
                                                (selection, timeStamp));

                    preSelection.cursor = selection.cursor;
                    preSelection.format = selection.format;

                    //?????????????????????
                    selection.cursor.setPosition(nCurrentPos, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nEndPos, QTextCursor::KeepAnchor);
                    m_wordMarkSelections.insert(i + 1,  QPair<QTextEdit::ExtraSelection, qint64>
                                                (selection, timeStamp));

                    m_bIsInputMethod = false;
                } else {

                    //?????????????????????
                    selection.cursor.setPosition(nStartPos, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(from, QTextCursor::KeepAnchor);
                    m_wordMarkSelections.insert(i, QPair<QTextEdit::ExtraSelection, qint64>
                                                (selection, timeStamp));

                    preSelection.cursor = selection.cursor;
                    preSelection.format = selection.format;

                    //?????????????????????
                    selection.cursor.setPosition(nCurrentPos, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nEndPos, QTextCursor::KeepAnchor);
                    m_wordMarkSelections.insert(i + 1, QPair<QTextEdit::ExtraSelection, qint64>
                                                (selection, timeStamp));
                }

                bool bIsFind = false;

                //?????????????????????????????????????????????????????????
                for (int j = 0; j < m_mapWordMarkSelections.count(); j++) {
                    auto list = m_mapWordMarkSelections.value(j);
                    for (int k = 0; k < list.count(); k++) {
                        if (list.value(k).cursor == wordMarkSelections.value(i).first.cursor
                                && list.value(k).format == wordMarkSelections.value(i).first.format) {
                            list.removeAt(k);
                            listSelections = list;
                            listSelections.insert(k, preSelection);
                            listSelections.insert(k + 1, selection);
                            bIsFind = true;
                            break;
                        }
                    }

                    if (bIsFind) {
                        m_mapWordMarkSelections.remove(j);
                        m_mapWordMarkSelections.insert(j, listSelections);
                        break;
                    }
                }
                break;

            } else if (nCurrentPos == nEndPos) { //??????????????????????????????
                m_wordMarkSelections.removeAt(i);
                selection.format.setBackground(strColor);
                selection.cursor = textCursor();

                if (m_bIsInputMethod) {
                    selection.cursor.setPosition(nStartPos, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nEndPos - m_qstrCommitString.count(), QTextCursor::KeepAnchor);
                    m_bIsInputMethod = false;
                } else {
                    selection.cursor.setPosition(nStartPos, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(from, QTextCursor::KeepAnchor);
                }

                m_wordMarkSelections.insert(i, QPair<QTextEdit::ExtraSelection, qint64>
                                            (selection, timeStamp));

                bool bIsFind = false;
                for (int j = 0; j < m_mapWordMarkSelections.count(); j++) {
                    auto list = m_mapWordMarkSelections.value(j);
                    for (int k = 0; k < list.count(); k++) {
                        if (list.value(k).cursor == wordMarkSelections.value(i).first.cursor
                                && list.value(k).format == wordMarkSelections.value(i).first.format) {
                            list.removeAt(k);
                            listSelections = list;
                            listSelections.insert(k, selection);
                            bIsFind = true;
                            break;
                        }
                    }

                    if (bIsFind) {
                        m_mapWordMarkSelections.remove(j);
                        m_mapWordMarkSelections.insert(j, listSelections);
                        break;
                    }
                }
                break;
            }
        }
    }

    //?????????????????????????????????
    renderAllSelections();

    highlight();
}

void TextEdit::setCursorStart(int pos)
{
    m_cursorStart = pos;
}


void TextEdit::completionWord(QString word)
{
    QString wordAtCursor = getWordAtCursor();
    QTextCursor cursor = textCursor();

    QString completionString = word.remove(0, wordAtCursor.size());
    if (completionString.size() > 0) {
        cursor = textCursor();
        cursor.insertText(completionString);
        setTextCursor(cursor);
    }
}

bool TextEdit::eventFilter(QObject *object, QEvent *event)
{
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        QTouchEvent *touchEvent = static_cast<QTouchEvent *>(event);
        QList<QTouchEvent::TouchPoint> touchPoints = touchEvent->touchPoints();
        QMouseEvent *event2 = static_cast<QMouseEvent *>(event);
        DPlainTextEdit::mousePressEvent(event2);
        if (touchPoints.count() == 3) {
            slot_translate();
        }
        break;
    }
    default: break;
    }
    if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
        m_mouseClickPos = mouseEvent->pos();

        if (object == m_pLeftAreaWidget->m_pBookMarkArea) {
            m_mouseClickPos = mouseEvent->pos();
            if (mouseEvent->button() == Qt::RightButton) {
                m_rightMenu->clear();
                m_rightMenu->deleteLater();
                m_rightMenu = nullptr;
                m_rightMenu = new DMenu(this);
                connect(m_rightMenu, &DMenu::destroyed, this, &TextEdit::removeHighlightWordUnderCursor);
                int line = getLineFromPoint(mouseEvent->pos());
                if (m_listBookmark.contains(line)) {
                    m_rightMenu->addAction(m_cancelBookMarkAction);
                    if (m_listBookmark.count() > 1) {
                        m_rightMenu->addAction(m_preBookMarkAction);
                        m_rightMenu->addAction(m_nextBookMarkAction);
                    }
                } else {
                    m_rightMenu->addAction(m_addBookMarkAction);
                }

                if (!m_listBookmark.isEmpty()) {
                    m_rightMenu->addAction(m_clearBookMarkAction);
                }

                m_rightMenu->exec(mouseEvent->globalPos());
            } else {
                addOrDeleteBookMark();
            }
            return true;
        } else if (object == m_pLeftAreaWidget->m_pFlodArea) {
            m_foldCodeShow->hide();
            if (mouseEvent->button() == Qt::LeftButton) {
                int line = getLineFromPoint(mouseEvent->pos());
                m_markFoldHighLightSelections.clear();
                renderAllSelections();

                //???????????????????????????????????? ?????????????????????????????????????????????????????????????????????
                bool bHasCommnent = false;
                QString multiLineCommentMark;
                QString singleLineCommentMark;

                if (m_commentDefinition.isValid()) {
                    multiLineCommentMark = m_commentDefinition.multiLineStart.trimmed();
                    singleLineCommentMark = m_commentDefinition.singleLine.trimmed();
                    //qDebug()<<"CommentMark:"<<multiLineCommentMark<<singleLineCommentMark;
                    //???????????????????????????????????????
                    if (!multiLineCommentMark.isEmpty()) bHasCommnent = document()->findBlockByNumber(line - 1).text().trimmed().startsWith(multiLineCommentMark);
                    if (!singleLineCommentMark.isEmpty()) bHasCommnent = document()->findBlockByNumber(line - 1).text().trimmed().startsWith(singleLineCommentMark);
                } else {
                    bHasCommnent = false;
                }

                // ?????????line-1 ????????????line????????????
                if (document()->findBlockByNumber(line).isVisible() && document()->findBlockByNumber(line - 1).text().contains("{") && !bHasCommnent) {
                    getNeedControlLine(line - 1, false);
                    document()->adjustSize();

                    //??????????????????????????????????????????
                    QPlainTextEdit::LineWrapMode curMode = this->lineWrapMode();
                    QPlainTextEdit::LineWrapMode WrapMode = curMode ==  QPlainTextEdit::NoWrap ?  QPlainTextEdit::WidgetWidth :  QPlainTextEdit::NoWrap;          this->setWordWrapMode(QTextOption::WrapAnywhere);
                    this->setLineWrapMode(WrapMode);
                    viewport()->update();
                    m_pLeftAreaWidget->updateBookMark();
                    m_pLeftAreaWidget->updateCodeFlod();
                    m_pLeftAreaWidget->updateLineNumber();
                    this->setLineWrapMode(curMode);
                    viewport()->update();

                } else if (!document()->findBlockByNumber(line).isVisible() && document()->findBlockByNumber(line - 1).text().contains("{") && !bHasCommnent) {
                    getNeedControlLine(line - 1, true);
                    document()->adjustSize();

                    //??????????????????????????????????????????
                    QPlainTextEdit::LineWrapMode curMode = this->lineWrapMode();
                    QPlainTextEdit::LineWrapMode WrapMode = curMode ==  QPlainTextEdit::NoWrap ?  QPlainTextEdit::WidgetWidth :  QPlainTextEdit::NoWrap;          this->setWordWrapMode(QTextOption::WrapAnywhere);
                    this->setLineWrapMode(WrapMode);
                    viewport()->update();
                    m_pLeftAreaWidget->updateBookMark();
                    m_pLeftAreaWidget->updateCodeFlod();
                    m_pLeftAreaWidget->updateLineNumber();
                    this->setLineWrapMode(curMode);
                    viewport()->update();
                } else {
                    //??????????????????
                }

            } else {
                m_mouseClickPos = mouseEvent->pos();
                m_rightMenu->clear();
                m_rightMenu->deleteLater();
                m_rightMenu = nullptr;
                m_rightMenu = new DMenu(this);
                connect(m_rightMenu, &DMenu::destroyed, this, &TextEdit::removeHighlightWordUnderCursor);
                int line = getLineFromPoint(mouseEvent->pos());

                if (m_listFlodIconPos.contains(line - 1)) {
                    m_rightMenu->addAction(m_flodAllLevel);
                    m_rightMenu->addAction(m_unflodAllLevel);
                    m_rightMenu->addAction(m_flodCurrentLevel);
                    m_rightMenu->addAction(m_unflodCurrentLevel);
                }
                if (document()->findBlockByNumber(line).isVisible()) {
                    m_unflodCurrentLevel->setEnabled(false);
                    m_flodCurrentLevel->setEnabled(true);
                } else {
                    m_unflodCurrentLevel->setEnabled(true);
                    m_flodCurrentLevel->setEnabled(false);
                }
				
		//??????/????????????????????????????????????????????????
                if (m_listMainFlodAllPos.count() == 0) {
                    m_unflodAllLevel->setEnabled(false);
                } else {
                    bool bExistVisible = false;
                    bool bExistInVisble = false;
                    for (int iLine : m_listMainFlodAllPos) {
                        if (document()->findBlockByNumber(iLine+1).isVisible()) {
                            m_flodAllLevel->setEnabled(true);
                            bExistVisible = true;
                        } else {
                            m_unflodAllLevel->setEnabled(true);
                            bExistInVisble = true;
                        }

                        if (bExistVisible && bExistInVisble) {
                            break;
                        }
                    }
                    if (!bExistVisible) {
                        m_flodAllLevel->setEnabled(false);
                        m_unflodAllLevel->setEnabled(true);
                    }
                    if (!bExistInVisble) {
                        m_unflodAllLevel->setEnabled(false);
                        m_flodAllLevel->setEnabled(true);
                    }
                }

                m_rightMenu->exec(mouseEvent->globalPos());
            }

            return true;
        }

    } else if (event->type() == QEvent::HoverMove) {
        QHoverEvent *hoverEvent = static_cast<QHoverEvent *>(event);
        int line = getLineFromPoint(hoverEvent->pos());

        if (object == m_pLeftAreaWidget->m_pBookMarkArea) {
            m_nBookMarkHoverLine = line;
            m_pLeftAreaWidget->m_pBookMarkArea->update();
        } else if (object == m_pLeftAreaWidget->m_pFlodArea) {
            m_markFoldHighLightSelections.clear();
            renderAllSelections();

            if (m_listFlodIconPos.contains(line - 1)) {
                if (!document()->findBlockByNumber(line).isVisible()) {
                    m_foldCodeShow->clear();
                    m_foldCodeShow->setStyle(lineWrapMode());//enum LineWrapMode {NoWrap,WidgetWidth};
                    getHideRowContent(line - 1);
                    m_foldCodeShow->show();
                    m_foldCodeShow->hideFirstBlock();
                    m_foldCodeShow->move(5, getLinePosYByLineNum(line));
                } else {
                    QTextCursor previousCursor = textCursor();
                    int ivalue = this->verticalScrollBar()->value();
                    int iLine = getHighLightRowContentLineNum(line - 1);

                    if (line - 1 == iLine)   return false;

                    QTextEdit::ExtraSelection selection;
                    selection.format.setProperty(QTextFormat::FullWidthSelection, true);

                    QTextBlock startblock;
                    QTextBlock endblock = document()->findBlockByNumber(iLine);
//                    if (line == 1 && document()->findBlockByNumber(0).text().trimmed().startsWith("{")) {
//                        startblock = document()->findBlockByNumber(line - 1);
//                    } else {
                    startblock = document()->findBlockByNumber(line - 1);
//                    }
                    QTextCursor beginCursor(startblock);
                    beginCursor.setPosition(startblock.position() + startblock.text().indexOf("{"), QTextCursor::MoveAnchor);

//                    if (line == 1 && document()->findBlockByNumber(0).text().trimmed().startsWith("{")) {
//                        beginCursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor, iLine - line + 2);
//                    } else {
                    beginCursor.setPosition(endblock.position() + endblock.text().indexOf("}") + 1, QTextCursor::KeepAnchor);
//                    }
                    if (iLine == document()->blockCount() - 1)
                        beginCursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);

                    setTextCursor(beginCursor);
                    selection.cursor = textCursor();

                    QColor highlightBackground = DGuiApplicationHelper::instance()->applicationPalette().color(QPalette::Highlight);
                    highlightBackground.setAlphaF(0.4);
                    selection.format.setBackground(highlightBackground);
                    m_markFoldHighLightSelections.push_back(selection);

                    renderAllSelections();
                    setTextCursor(QTextCursor(document()->findBlockByNumber(line - 1)));
                    this->verticalScrollBar()->setValue(ivalue);
                }
            } else {
                m_foldCodeShow->hide();
            }

            return true;
        }

    } else if (event->type() == QEvent::HoverLeave) {
        if (object == m_pLeftAreaWidget->m_pBookMarkArea) {
            m_nBookMarkHoverLine = -1;
            m_pLeftAreaWidget->m_pBookMarkArea->update();
            return true;
        } else if (object == m_pLeftAreaWidget->m_pFlodArea) {
            m_markFoldHighLightSelections.clear();
            m_foldCodeShow->hide();
            renderAllSelections();
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonDblClick) {
        m_bIsDoubleClick = true;
        m_bBeforeIsDoubleClick = true;
    } else if (object == m_colorMarkMenu) {
        // ???????????? color mark menu ?????????????????????
        if(event->type() == QEvent::KeyRelease) {
            QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);

            // ??????????????? Tab ??? ??? ?????????????????????????????????
            if(keyEvent->key() == Qt::Key_Tab){
                // handleKey ??????????????????????????????????????????
                int handleKey = keyEvent->key();

                // ??????Timer??????????????????????????????????????????????????????????????????????????????
                QTimer::singleShot(0, this, [=] () {
                    // ?????? menu ???????????????????????????????????????Tab???
                    if(m_colorMarkMenu->isVisible() && handleKey == Qt::Key_Tab) {
                        QAction* currentAction = m_colorMarkMenu->activeAction();
                        QAction* nextAction = nullptr;
                        int currentIndex = -1;

                        // ?????????????????? focus item ??? ?????????????????????
                        if(currentAction == nullptr) {
                            return;
                        }

                        // ?????? Order List??? ??????????????? order list ???????????????focus item
                        for(int i = 0; i < m_MarkColorMenuTabOrder.size(); i++) {
                            QPair<QAction*,bool> orderItem = m_MarkColorMenuTabOrder.at(i);

                            if(orderItem.first == currentAction) {
                                // ???????????? item ??????
                                currentIndex = i;

                                if(orderItem.second == true) {
                                    // ?????? focus??? ????????????????????????
                                    return;
                                } else {
                                    // ?????????????????????????????????????????????
                                    break;
                                }
                            }
                        }

                        if(currentIndex == -1) {
                            qWarning() << " can not find current item in tab order list , need check 'Mark Color Menu' tab order setting!";
                            return;
                        }

                        // ??? order list?????????????????????item
                        for(int j = currentIndex + 1; j < m_MarkColorMenuTabOrder.size(); j++) {
                            QPair<QAction*,bool> newOrderItem = m_MarkColorMenuTabOrder.at(j);

                            if(newOrderItem.second == true) {
                                nextAction = newOrderItem.first;
                                break;
                            }
                        }

                        // ??? order list ???????????????????????????item??? ???????????????????????????
                        if(nextAction == nullptr) {
                            // ???????????????????????????i???????????????????????????i?????????
                            for(int j = 0; j < currentIndex; j++) {
                                QPair<QAction*,bool> newOrderItem = m_MarkColorMenuTabOrder.at(j);

                                if(newOrderItem.second == true) {
                                    nextAction = newOrderItem.first;
                                    break;
                                }
                            }
                        }

                        // ???????????????item??????focus
                        if(nextAction != nullptr) {
                            m_colorMarkMenu->setActiveAction(nextAction);
                        } else {
                            qWarning() << " can not find valid item , need check 'Mark Color Menu' tab order setting!";
                        }
                    }
                });
            }
        }
    }

    return QPlainTextEdit::eventFilter(object, event);
}

void TextEdit::adjustScrollbarMargins()
{
    QEvent event(QEvent::LayoutRequest);
    QApplication::sendEvent(this, &event);

    QMargins margins = viewportMargins();
    setViewportMargins(0, 0, 5, 0);
    setViewportMargins(margins);
    if (!verticalScrollBar()->visibleRegion().isEmpty()) {
        setViewportMargins(0, 0, 5, 0);        //-verticalScrollBar()->sizeHint().width()  ????????????????????????
        //setViewportMargins(0, 0, -verticalScrollBar()->sizeHint().width(), 0);
    } else {
        setViewportMargins(0, 0, 5, 0);
    }
}

bool TextEdit::containsExtraSelection(QList<QTextEdit::ExtraSelection> listSelections, QTextEdit::ExtraSelection selection)
{
    for (int i = 0; i < listSelections.count(); i++) {
        if (listSelections.value(i).cursor == selection.cursor
                && listSelections.value(i).format == selection.format) {
            return true;
        }
    }
    return false;
}

void TextEdit::appendExtraSelection(QList<QTextEdit::ExtraSelection> wordMarkSelections
                                    , QTextEdit::ExtraSelection selection, QString strColor
                                    , QList<QTextEdit::ExtraSelection> *listSelections)
{
// ?????????????????????????????????????????????????????????
// ??????????????????????????????????????????????????????????????????????????????????????????????????????
#if 1
    // ???????????????????????????
    Q_UNUSED(wordMarkSelections)
    Q_UNUSED(selection)
    Q_UNUSED(strColor)
    Q_UNUSED(listSelections)
#else
    //????????????????????????
    if (wordMarkSelections.count() > 0) {
        bool bIsContains = false;///< ????????????????????????
        int nWordMarkSelectionStart = 0,///< ????????????????????????
            nSelectionStart = 0,///< ??????????????????
            nWordMarkSelectionEnd = 0,///< ????????????????????????
            nSelectionEnd = 0;///< ??????????????????

        //?????????????????????????????????????????????
        if (selection.cursor.selectionStart() > selection.cursor.selectionEnd()) {
            nSelectionStart = selection.cursor.selectionEnd();
            nSelectionEnd = selection.cursor.selectionStart();
        } else {
            nSelectionStart = selection.cursor.selectionStart();
            nSelectionEnd = selection.cursor.selectionEnd();
        }

        for (int i = 0; i < wordMarkSelections.count(); i++) {

            //???????????????????????????????????????????????????
            if (wordMarkSelections.value(i).cursor.selectionStart() > wordMarkSelections.value(i).cursor.selectionEnd()) {
                nWordMarkSelectionStart = wordMarkSelections.value(i).cursor.selectionEnd();
                nWordMarkSelectionEnd = wordMarkSelections.value(i).cursor.selectionStart();
            } else {
                nWordMarkSelectionStart = wordMarkSelections.value(i).cursor.selectionStart();
                nWordMarkSelectionEnd = wordMarkSelections.value(i).cursor.selectionEnd();
            }

            //???????????????????????????
            if ((nWordMarkSelectionStart <= nSelectionStart && nWordMarkSelectionEnd > nSelectionEnd)
                    || (nWordMarkSelectionStart < nSelectionStart && nWordMarkSelectionEnd >= nSelectionEnd)) {

                bIsContains = true;
                selection.format.setBackground(QColor(strColor));

                //???????????????????????????
                if (wordMarkSelections.value(i).format != selection.format) {
                    int nRemPos = 0;///< ??????????????????

                    //??????????????????
                    for (int j = 0; j < wordMarkSelections.count(); j++) {
                        if (m_wordMarkSelections.value(j).cursor == wordMarkSelections.value(i).cursor
                                && m_wordMarkSelections.value(j).format == wordMarkSelections.value(i).format) {

                            m_wordMarkSelections.removeAt(j);
                            nRemPos = j;
                            break;
                        }
                    }

                    //??????????????????
                    selection.cursor.setPosition(nWordMarkSelectionStart, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nSelectionStart, QTextCursor::KeepAnchor);
                    selection.format.setBackground(wordMarkSelections.value(i).format.background());

                    bool bIsInsert = false;///< ???????????????????????????????????????

                    //?????????????????????
                    if (selection.cursor.selectedText() != "") {
                        bIsInsert = true;
                        m_wordMarkSelections.insert(nRemPos, selection);
                    }

                    QTextEdit::ExtraSelection preSelection;
                    preSelection.format = selection.format;
                    preSelection.cursor = selection.cursor;

                    selection.cursor.setPosition(nSelectionEnd, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nWordMarkSelectionEnd, QTextCursor::KeepAnchor);

                    //?????????????????????
                    if (selection.cursor.selectedText() != "") {
                        if (bIsInsert) {
                            m_wordMarkSelections.insert(nRemPos + 1, selection);
                        } else {
                            m_wordMarkSelections.insert(nRemPos, selection);
                        }
                    }

                    //?????????????????????????????????????????????????????????????????????
                    QList<QTextEdit::ExtraSelection> selecList;
                    bool bIsFind = false;///< ???????????????????????????????????????

                    for (int j = 0; j < m_mapWordMarkSelections.count(); j++) {
                        auto list = m_mapWordMarkSelections.value(j);

                        for (int k = 0; k < list.count(); k++) {
                            if (list.value(k).cursor == wordMarkSelections.value(i).cursor
                                    && list.value(k).format == wordMarkSelections.value(i).format) {

                                list.removeAt(k);
                                selecList = list;
                                bIsInsert = false;

                                if (preSelection.cursor.selectedText() != "") {
                                    bIsInsert = true;
                                    selecList.insert(k, preSelection);
                                }

                                if (selection.cursor.selectedText() != "") {
                                    if (bIsInsert) {
                                        selecList.insert(k + 1, selection);
                                    } else {
                                        selecList.insert(k, selection);
                                    }
                                }

                                bIsFind = true;
                                break;
                            }
                        }

                        if (bIsFind) {
                            m_mapWordMarkSelections.remove(j);
                            m_mapWordMarkSelections.insert(j, selecList);
                            break;
                        }
                    }

                    //????????????????????????
                    selection.cursor.setPosition(nSelectionStart, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nSelectionEnd, QTextCursor::KeepAnchor);
                    selection.format.setBackground(QColor(strColor));
                    m_wordMarkSelections.append(selection);
                    listSelections->append(selection);
                }
            } else if (nWordMarkSelectionStart >= nSelectionStart && nWordMarkSelectionEnd <= nSelectionEnd) { //??????????????????????????????
                bIsContains = true;
                selection.format.setBackground(QColor(strColor));

                //??????????????????
                for (int j = 0; j < wordMarkSelections.count(); j++) {
                    if (m_wordMarkSelections.value(j).cursor == wordMarkSelections.value(i).cursor
                            && m_wordMarkSelections.value(j).format == wordMarkSelections.value(i).format) {
                        m_wordMarkSelections.removeAt(j);
                        break;
                    }
                }

                //????????????????????????
                m_wordMarkSelections.append(selection);

                //???????????????????????????
                if (wordMarkSelections.value(i).format != selection.format) {

                    QList<QTextEdit::ExtraSelection> selecList;
                    bool bIsFind = false;///< ???????????????????????????????????????

                    //?????????????????????????????????????????????????????????????????????
                    for (int j = 0; j < m_mapWordMarkSelections.count(); j++) {
                        auto list = m_mapWordMarkSelections.value(j);
                        for (int k = 0; k < list.count(); k++) {
                            if (list.value(k).cursor == wordMarkSelections.value(i).cursor
                                    && list.value(k).format == wordMarkSelections.value(i).format) {

                                list.removeAt(k);
                                selecList = list;
                                bIsFind = true;
                                break;
                            }
                        }

                        if (bIsFind) {
                            m_mapWordMarkSelections.remove(j);
                            m_mapWordMarkSelections.insert(j, selecList);
                            break;
                        }
                    }
                }

                listSelections->append(selection);
            } else if (nWordMarkSelectionEnd < nSelectionEnd && nWordMarkSelectionStart < nSelectionStart
                       && nWordMarkSelectionEnd > nSelectionStart) { //??????????????????????????????????????????????????????

                selection.format.setBackground(QColor(strColor));
                int nRemPos = 0;///< ??????????????????

                //??????????????????
                for (int j = 0; j < wordMarkSelections.count(); j++) {
                    if (m_wordMarkSelections.value(j).cursor == wordMarkSelections.value(i).cursor
                            && m_wordMarkSelections.value(j).format == wordMarkSelections.value(i).format) {
                        m_wordMarkSelections.removeAt(j);
                        nRemPos = j;
                        break;
                    }
                }

                //???????????????????????????
                if (wordMarkSelections.value(i).format != selection.format) {

                    //????????????????????????????????????????????????????????????
                    selection.cursor.setPosition(nWordMarkSelectionStart, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nSelectionStart, QTextCursor::KeepAnchor);
                    selection.format.setBackground(wordMarkSelections.value(i).format.background());
                    m_wordMarkSelections.insert(nRemPos, selection);

                    QList<QTextEdit::ExtraSelection> selecList;
                    bool bIsFind = false;///< ???????????????????????????????????????

                    //?????????????????????????????????????????????????????????????????????
                    for (int j = 0; j < m_mapWordMarkSelections.count(); j++) {
                        auto list = m_mapWordMarkSelections.value(j);
                        for (int k = 0; k < list.count(); k++) {
                            if (list.value(k).cursor == wordMarkSelections.value(i).cursor
                                    && list.value(k).format == wordMarkSelections.value(i).format) {
                                list.removeAt(k);
                                selecList = list;
                                selecList.insert(k, selection);
                                bIsFind = true;
                                break;
                            }
                        }

                        if (bIsFind) {
                            m_mapWordMarkSelections.remove(j);
                            m_mapWordMarkSelections.insert(j, selecList);
                            break;
                        }
                    }

                    //????????????????????????
                    selection.cursor.setPosition(nSelectionStart, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nSelectionEnd, QTextCursor::KeepAnchor);
                    selection.format.setBackground(QColor(strColor));
                    m_wordMarkSelections.append(selection);
                } else { //????????????????????????
                    selection.cursor.setPosition(nWordMarkSelectionStart, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nSelectionEnd, QTextCursor::KeepAnchor);
                    m_wordMarkSelections.insert(nRemPos, selection);
                }

                if (!bIsContains) {
                    listSelections->append(selection);
                }

                bIsContains = true;
            } else if (nWordMarkSelectionEnd > nSelectionEnd && nWordMarkSelectionStart > nSelectionStart
                       && nWordMarkSelectionStart < nSelectionEnd) { //??????????????????????????????????????????????????????

                selection.format.setBackground(QColor(strColor));
                int nRemPos = 0;///< ??????????????????

                //??????????????????
                for (int j = 0; j < wordMarkSelections.count(); j++) {
                    if (m_wordMarkSelections.value(j).cursor == wordMarkSelections.value(i).cursor
                            && m_wordMarkSelections.value(j).format == wordMarkSelections.value(i).format) {
                        m_wordMarkSelections.removeAt(j);
                        nRemPos = j;
                        break;
                    }
                }

                //???????????????????????????
                if (wordMarkSelections.value(i).format != selection.format) {

                    //????????????????????????????????????????????????????????????
                    selection.cursor.setPosition(nSelectionEnd, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nWordMarkSelectionEnd, QTextCursor::KeepAnchor);
                    selection.format.setBackground(wordMarkSelections.value(i).format.background());
                    m_wordMarkSelections.insert(nRemPos, selection);

                    QList<QTextEdit::ExtraSelection> selecList;
                    bool bIsFind = false;

                    //?????????????????????????????????????????????????????????????????????
                    for (int j = 0; j < m_mapWordMarkSelections.count(); j++) {
                        auto list = m_mapWordMarkSelections.value(j);
                        for (int k = 0; k < list.count(); k++) {
                            if (list.value(k).cursor == wordMarkSelections.value(i).cursor
                                    && list.value(k).format == wordMarkSelections.value(i).format) {
                                list.removeAt(k);
                                selecList = list;
                                selecList.insert(k, selection);
                                bIsFind = true;
                                break;
                            }
                        }

                        if (bIsFind) {
                            m_mapWordMarkSelections.remove(j);
                            m_mapWordMarkSelections.insert(j, selecList);
                            break;
                        }
                    }

                    //????????????????????????
                    selection.cursor.setPosition(nSelectionStart, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nSelectionEnd, QTextCursor::KeepAnchor);
                    selection.format.setBackground(QColor(strColor));
                    m_wordMarkSelections.append(selection);
                } else { //????????????????????????
                    selection.cursor.setPosition(nSelectionStart, QTextCursor::MoveAnchor);
                    selection.cursor.setPosition(nWordMarkSelectionEnd, QTextCursor::KeepAnchor);
                    m_wordMarkSelections.insert(nRemPos, selection);
                }

                if (!bIsContains) {
                    listSelections->append(selection);
                }
                bIsContains = true;
            }
        }

        if (!bIsContains) {
            selection.format.setBackground(QColor(strColor));
            m_wordMarkSelections.append(selection);
            listSelections->append(selection);
        }

    } else { //???????????????????????????
        selection.format.setBackground(QColor(strColor));
        m_wordMarkSelections.append(selection);
        listSelections->append(selection);
    }
#endif
}

void TextEdit::onSelectionArea()
{
    if (textCursor().hasSelection()) {
        m_nSelectStart = textCursor().selectionStart();
        m_nSelectEnd = textCursor().selectionEnd();
        m_nSelectEndLine = document()->findBlock(textCursor().selectionEnd()).blockNumber() + 1;
    } else {
        m_nSelectEndLine = -1;
    }

    //????????????????????????????????????????????????
    if (m_bIsDoubleClick == true) {
        m_bIsDoubleClick = false;
        return;
    } else if (m_bBeforeIsDoubleClick == true && textCursor().selectedText() != "") {
        m_bBeforeIsDoubleClick = false;
        return;
    }

    if (m_gestureAction != GA_null) {
        QTextCursor cursor = textCursor();
        if (cursor.selectedText() != "") {
            cursor.clearSelection();
            setTextCursor(cursor);
        }
    }
}

void TextEdit::fingerZoom(QString name, QString direction, int fingers)
{
    if (name == "tap" && fingers == 3) {
        slot_translate();
    }
    // ?????????????????????,????????????
    if (hasFocus()) {
        if (name == "pinch" && fingers == 2) {
            if (direction == "in") {
                // ?????? in???????????????????????? ????????????
                qobject_cast<Window *>(this->window())->decrementFontSize();
            } else if (direction == "out") {
                // ?????? out???????????????????????? ????????????
                qobject_cast<Window *>(this->window())->incrementFontSize();
            }
        }
    }
}

void TextEdit::dragEnterEvent(QDragEnterEvent *event)
{
    QPlainTextEdit::dragEnterEvent(event);
    qobject_cast<Window *>(this->window())->requestDragEnterEvent(event);
}

void TextEdit::dragMoveEvent(QDragMoveEvent *event)
{
    if (m_readOnlyMode) {
        return;
    }

    const QMimeData *data = event->mimeData();

    if (data->hasUrls()) {
        event->acceptProposedAction();
    } else {
        QPlainTextEdit::dragMoveEvent(event);
    }
}

void TextEdit::dropEvent(QDropEvent *event)
{
    const QMimeData *data = event->mimeData();

    if (data->hasUrls() && data->urls().first().isLocalFile()) {
        qobject_cast<Window *>(this->window())->requestDropEvent(event);
    } else if (data->hasText() && !m_readOnlyMode) {
        QPlainTextEdit::dropEvent(event);
    }
}

void TextEdit::inputMethodEvent(QInputMethodEvent *e)
{
    m_bIsInputMethod = true;

    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    if (!m_readOnlyMode && !m_bReadOnlyPermission && !e->commitString().isEmpty()) {
        //???????????????????????????
        if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
            insertColumnEditTextEx(e->commitString());
        } else {
            insertSelectTextEx(textCursor(), e->commitString());
        }
    }
}

void TextEdit::mousePressEvent(QMouseEvent *e)
{
    if (m_bIsFindClose) {
        m_bIsFindClose = false;
        removeKeywords();
    }
    if(e->button() != Qt::RightButton)
        m_isSelectAll = false;

    if (Qt::MouseEventSynthesizedByQt == e->source()) {
        m_startY = e->y();
        m_startX = e->x();
    }
    if (e->source() == Qt::MouseEventSynthesizedByQt) {
        m_lastTouchBeginPos = e->pos();

        if (QScroller::hasScroller(this)) {
            QScroller::scroller(this)->deleteLater();
        }

        if (m_updateEnableSelectionByMouseTimer) {
            m_updateEnableSelectionByMouseTimer->stop();
        } else {
            m_updateEnableSelectionByMouseTimer = new QTimer(this);
            m_updateEnableSelectionByMouseTimer->setSingleShot(true);

            static QObject *theme_settings = reinterpret_cast<QObject *>(qvariant_cast<quintptr>(qApp->property("_d_theme_settings_object")));
            QVariant touchFlickBeginMoveDelay;

            if (theme_settings) {
                touchFlickBeginMoveDelay = theme_settings->property("touchFlickBeginMoveDelay");
            }

            m_updateEnableSelectionByMouseTimer->setInterval(touchFlickBeginMoveDelay.isValid() ? touchFlickBeginMoveDelay.toInt() : 300);
            connect(m_updateEnableSelectionByMouseTimer, &QTimer::timeout, m_updateEnableSelectionByMouseTimer, &QTimer::deleteLater);
        }

        m_updateEnableSelectionByMouseTimer->start();
    }

    //add for single refers to the sliding
    if (e->type() == QEvent::MouseButtonPress && e->source() == Qt::MouseEventSynthesizedByQt) {
        m_lastMouseTimeX = e->timestamp();
        m_lastMouseTimeY = e->timestamp();
        m_lastMouseYpos = e->pos().y();
        m_lastMouseXpos = e->pos().x();

        if (tweenY.activeY()) {
            m_slideContinueY = true;
            tweenY.stopY();
        }

        if (tweenX.activeX()) {
            m_slideContinueX = true;
            tweenX.stopX();
        }
    }

    if (e->modifiers() == Qt::AltModifier) {
        m_bIsAltMod = true;
        //????????????????????????????????? ???????????????????????????
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(e);
        m_altStartTextCursor = this->cursorForPosition(mouseEvent->pos());
        m_altStartTextCursor.clearSelection();
        this->setTextCursor(m_altStartTextCursor);
        m_altModSelections.clear();
    } else {
        if (e->button() != 2) { //??????,????????????????????????????????????
            m_bIsAltMod = false;
            m_altModSelections.clear();
        }
    }

    QPlainTextEdit::mousePressEvent(e);
}

void TextEdit::mouseMoveEvent(QMouseEvent *e)
{
    if (Qt::MouseEventSynthesizedByQt == e->source()) {
        m_endY = e->y();
        m_endX = e->x();
    }

    //add for single refers to the sliding
    if (e->type() == QEvent::MouseMove && e->source() == Qt::MouseEventSynthesizedByQt) {
        const ulong diffTimeX = e->timestamp() - m_lastMouseTimeX;
        const ulong diffTimeY = e->timestamp() - m_lastMouseTimeY;
        const int diffYpos = e->pos().y() - m_lastMouseYpos;
        const int diffXpos = e->pos().x() - m_lastMouseXpos;
        m_lastMouseTimeX = e->timestamp();
        m_lastMouseTimeY = e->timestamp();
        m_lastMouseYpos = e->pos().y();
        m_lastMouseXpos = e->pos().x();

        if (m_gestureAction == GA_slide) {
            QFont font = this->font();

            /*??????????????????????????????????????????*/
            qreal direction = diffYpos > 0 ? 1.0 : -1.0;
            slideGestureY(-direction * sqrt(abs(diffYpos)) / font.pointSize());
            qreal directionX = diffXpos > 0 ? 1.0 : -1.0;
            slideGestureX(-directionX * sqrt(abs(diffXpos)) / font.pointSize());

            /*????????????????????????*/
            m_stepSpeedY = static_cast<qreal>(diffYpos) / static_cast<qreal>(diffTimeY + 0.000001);
            durationY = sqrt(abs(m_stepSpeedY)) * 1000;
            m_stepSpeedX = static_cast<qreal>(diffXpos) / static_cast<qreal>(diffTimeX + 0.000001);
            durationX = sqrt(abs(m_stepSpeedX)) * 1000;

            /*????????????????????????,4.0???????????????*/
            m_stepSpeedY /= sqrt(font.pointSize() * 4.0);
            changeY = m_stepSpeedY * sqrt(abs(m_stepSpeedY)) * 100;
            m_stepSpeedX /= sqrt(font.pointSize() * 4.0);
            changeX = m_stepSpeedX * sqrt(abs(m_stepSpeedX)) * 100;

            //return true;
        }

        if (m_gestureAction != GA_null) {
            //return true;
        }
    }

    // other apps will override their own cursor when opened
    // so they need to be restored.
    QApplication::restoreOverrideCursor();

    if (viewport()->cursor().shape() != Qt::IBeamCursor) {
        viewport()->setCursor(Qt::IBeamCursor);
    }

    QPlainTextEdit::mouseMoveEvent(e);

    if (e->modifiers() == Qt::AltModifier && m_bIsAltMod) {
        m_altModSelections.clear();
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(e);

        QPoint curPos = mouseEvent->pos();
        m_altEndTextCursor = this->cursorForPosition(curPos);

        int column = m_altEndTextCursor.positionInBlock();
        int row = m_altEndTextCursor.blockNumber();

        int startColumn = m_altStartTextCursor.positionInBlock();
        int startRow = m_altStartTextCursor.blockNumber();

        int minColumn = startColumn < column ? startColumn : column;
        int maxColumn = startColumn > column ? startColumn : column;
        int minRow = startRow < row ? startRow : row;
        int maxRow = startRow > row ? startRow : row;
//        qDebug()<<"min========================:"<<minRow<<minColumn;
//        qDebug()<<"max========================:"<<maxRow<<maxColumn;
//        qDebug()<<mouseEvent->pos();
//        qDebug()<<"begin pos:row="<<startRow<<" column="<<startColumn;
//        qDebug()<<"Move  pos:row="<<row<<" column="<<column;

        QTextCharFormat format;
        QPalette palette;
        format.setBackground(QColor(SELECT_HIGHLIGHT_COLOR));
        format.setForeground(palette.highlightedText());

        for (int iRow = minRow; iRow <= maxRow; iRow++) {
            QTextBlock block = document()->findBlockByNumber(iRow);
            QTextCursor cursor(block);
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::MoveAnchor);

            QPoint blockTailPos = this->cursorRect(cursor).bottomRight();

            //?????????0??????
            int length = block.text().length();

            //??????x?????????????????????????????????????????????????????????????????????
            if (curPos.x() >= blockTailPos.x()  && length > maxColumn) {
                maxColumn = length;
            }
        }


        for (int iRow = minRow; iRow <= maxRow; iRow++) {
            QTextBlock block = document()->findBlockByNumber(iRow);
            int length = block.text().size();

            if (length < minColumn) continue;

            QTextEdit::ExtraSelection selection;
            QTextCursor cursor = this->textCursor();
            cursor.clearSelection();
            setTextCursor(cursor);
            cursor.setPosition(block.position() + minColumn, QTextCursor::MoveAnchor);
            if (length < maxColumn) {
                cursor.setPosition(block.position() + length, QTextCursor::KeepAnchor);
            } else {
                cursor.setPosition(block.position() + maxColumn, QTextCursor::KeepAnchor);
            }

            selection.cursor = cursor;
            selection.format = format;
            m_altModSelections << selection;
        }
        //?????????????????????
        m_pUndoStack->clear();
        renderAllSelections();
        update();
    }
}

void TextEdit::mouseReleaseEvent(QMouseEvent *e)
{
    //add for single refers to the sliding
    if (e->type() == QEvent::MouseButtonRelease && e->source() == Qt::MouseEventSynthesizedByQt) {
        qDebug() << "action is over" << m_gestureAction;

        if (m_gestureAction == GA_slide) {

            tweenX.startX(0, 0, changeX, durationX, std::bind(&TextEdit::slideGestureX, this, std::placeholders::_1));
            tweenY.startY(0, 0, changeY, durationY, std::bind(&TextEdit::slideGestureY, this, std::placeholders::_1));
        }

        m_gestureAction = GA_null;
    }

    int i = m_endY - m_startY;
    int k = m_endX - m_startX;
    if (Qt::MouseEventSynthesizedByQt == e->source()
            && (i > 10 && this->verticalScrollBar()->value() != 0)
            && (k > 10 && this->horizontalScrollBar()->value() != 0)) {
        e->accept();
        return;
    }
    return QPlainTextEdit::mouseReleaseEvent(e);
}

void TextEdit::keyPressEvent(QKeyEvent *e)
{
    Qt::KeyboardModifiers modifiers = e->modifiers();
    QString key = Utils::getKeyshortcut(e);

    //??????????????????????????????
    //??????esc?????????,??????????????????????????????????????????
    if (modifiers == Qt::NoModifier && e->key() == Qt::Key_Escape) {
        emit signal_setTitleFocus();
        return;
    }

    if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "copy")) {
        copy();
        return;
    } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "selectall")) {
        #if 0
        //2021-2-25:setSelectAll()??????
        if (m_wrapper->getFileLoading()) {
            return;
        }
        m_bIsAltMod = false;
        selectAll();
        #endif
        setSelectAll();
        return;
    }

    if (m_readOnlyMode || m_bReadOnlyPermission) {
        if (key == "J") {
            nextLine();
            return;
        } else if (key == "K") {
            prevLine();
            return;
        } else if (key == ",") {
            moveToEnd();
            return;
        } else if (key == ".") {
            moveToStart();
            return;
        } else if (key == "H") {
            backwardChar();
            return;
        } else if (key == "L") {
            forwardChar();
            return;
        } else if (key == "Space") {
            scrollUp();
            return;
        } else if (key == "V") {
            scrollDown();
            return;
        } else if (key == "F") {
            forwardWord();
            return;
        } else if (key == "B") {
            backwardWord();
            return;
        } else if (key == "A") {
            moveToStartOfLine();
            return;
        } else if (key == "E") {
            moveToEndOfLine();
            return;
        } else if (key == "M") {
            moveToLineIndentation();
            return;
        } else if (key == "Q" && m_bReadOnlyPermission == false) {
//            setReadOnly(false);
            toggleReadOnlyMode();
            return;
        } else if (key == "Shfit+J") {
            scrollLineUp();
            return;
        } else if (key == "Shift+K") {
            scrollLineDown();
            return;
        } else if (key == "P") {
            forwardPair();
            return;
        } else if (key == "N") {
            backwardPair();
            return;
        } else if (key == "Shift+:") {
            copyLines();
            return;
        } else if ((key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "togglereadonlymode")/* || key=="Alt+Meta+L"*/)
                   && m_bReadOnlyPermission == false) {
//            setReadOnly(false);
            toggleReadOnlyMode();
            return;
        } else if (key == "Shift+/" && e->modifiers() == Qt::ControlModifier) {
            e->ignore();
        } else if (e->key() == Qt::Key_Control || e->key() == Qt::Key_Shift) {
            e->ignore();
        } else if (e->key() == Qt::Key_F11 || e->key() == Qt::Key_F5) {
            e->ignore();
            return;
        } else if (e->modifiers() == Qt::NoModifier || e->modifiers() == Qt::KeypadModifier) {
            popupNotify(tr("Read-Only mode is on"));
            return;
        } else {
            // If press another key
            // the main window does not receive
            e->ignore();
            return;
        }
    } else {
        if (isReadOnly()) { //????????????setReadOnly?????????,?????????????????????ReadOnly
            return;
        }

        //???????????????????????????
        if (modifiers == Qt::NoModifier && (e->key() <= Qt::Key_ydiaeresis && e->key() >= Qt::Key_Space) && !e->text().isEmpty()) {

            if(m_isSelectAll)
                QPlainTextEdit::selectAll();

            //???????????????????????????????????????
            if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
                insertColumnEditTextEx(e->text());
            } else {
                insertSelectTextEx(textCursor(), e->text());
            }

            m_isSelectAll = false;
            return;
        }

        //?????????????????????
        if (modifiers == Qt::KeypadModifier && (e->key() <= Qt::Key_9 && e->key() >= Qt::Key_Asterisk) && !e->text().isEmpty()) {

            if(m_isSelectAll)
                QPlainTextEdit::selectAll();

            //???????????????????????????????????????
            if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
                insertColumnEditTextEx(e->text());
            } else {
                insertSelectTextEx(textCursor(), e->text());
            }
            return;
        }

        //??????????????????
        if (modifiers == Qt::NoModifier && (e->key() == Qt::Key_Tab || e->key() == Qt::Key_Return) ||
            modifiers == Qt::KeypadModifier && (e->key() == Qt::Key_Enter)) {

            if(m_isSelectAll)
                QPlainTextEdit::selectAll();

            //???????????????????????????
            if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
                insertColumnEditTextEx(e->text());
            } else {
                insertSelectTextEx(textCursor(), e->text());
            }
            return;
        }

        //????????? ??????????????????
        if (modifiers == Qt::NoModifier && (e->key() == Qt::Key_Backspace)) {
            if(m_isSelectAll)
                QPlainTextEdit::selectAll();

            if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
                QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(m_altModSelections);
                m_pUndoStack->push(pDeleteStack);
            } else {
                //??????backspace?????????????????????????????????backspace,???????????????*????????????
                QTextCursor cursor = textCursor();
                if(!cursor.hasSelection())
                {
                    cursor.movePosition(QTextCursor::PreviousCharacter,QTextCursor::KeepAnchor);
                }
                QString m_delText = cursor.selectedText();
                if(m_delText.size()<=0) return;
                QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(cursor);
                m_pUndoStack->push(pDeleteStack);
            }
            return;
        }

        //????????? ????????????????????????
        if (modifiers == Qt::NoModifier && (e->key() == Qt::Key_Delete)) {
            if(m_isSelectAll)
                QPlainTextEdit::selectAll();

            if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
                DeleteBackAltCommond *commond = new DeleteBackAltCommond(m_altModSelections,this);
                m_pUndoStack->push(commond);
            } else {
                //??????delete?????????????????????????????????delete,???????????????*????????????
                QTextCursor cursor = textCursor();
                if(!cursor.hasSelection())
                {
                    cursor.movePosition(QTextCursor::NextCharacter,QTextCursor::KeepAnchor);
                }
                QString m_delText = cursor.selectedText();
                if(m_delText.size()<=0) return;

                DeleteBackCommond *commond = new DeleteBackCommond(cursor, this);
                m_pUndoStack->push(commond);
            }
            return;
        }

        //fix 66710 ???????????????????????????????????????????????????????????????????????????
        //fix 75313  lxp 2021.4.22
        if ((modifiers == Qt::ShiftModifier || e->key() == Qt::Key_Shift) && !e->text().isEmpty()) {

            if(m_isSelectAll)
                QPlainTextEdit::selectAll();

            if (m_bIsAltMod) {
               insertColumnEditTextEx(e->text());
            } else {
               insertSelectTextEx(textCursor(), e->text());
            }
            return;
        }

        // alt+m ???????????????????????????
        if (e->modifiers() == Qt::AltModifier && !e->text().compare(QString("m"), Qt::CaseInsensitive)) {
            popRightMenu();
            return;
        }

        //???????????????
        if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "undo")) {
            m_pUndoStack->undo();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "redo")) {
            m_pUndoStack->redo();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "cut")) {
	    #if 0
            //???????????????????????????
            if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
                QString data;
                for (auto sel : m_altModSelections) data.append(sel.cursor.selectedText());
                //???????????????
                for (int i = 0; i < m_altModSelections.size(); i++) {
                    if (m_altModSelections[i].cursor.hasSelection()) {
                        QUndoCommand *pDeleteStack = new DeleteTextUndoCommand(m_altModSelections[i].cursor);
                        m_pUndoStack->push(pDeleteStack);
                    }
                }
                //??????????????????
                QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
                clipboard->setText(data);
            } else {
                QTextCursor cursor = textCursor();
                //????????????????????????
                if (cursor.hasSelection()) {
                    QString data = cursor.selectedText();
                    deleteTextEx(cursor);
                    QClipboard *clipboard = QApplication::clipboard();   //???????????????????????????
                    clipboard->setText(data);
                }
            }
            #endif
            this->cut();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "paste")) {
            #if 0
            //???????????????????????????????????????
            const QClipboard *clipboard = QApplication::clipboard(); //?????????????????????

            if (m_bIsAltMod && !m_altModSelections.isEmpty()) {
                insertColumnEditTextEx(clipboard->text());
            } else {
                insertSelectTextEx(textCursor(), clipboard->text());
            }
            #endif
            this->paste();
            return;
        }  else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "scrollup")) {
            //????????????
            scrollUp();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "scrolldown")) {
            //????????????
            scrollDown();
            return;
        }  else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "copylines")) {
            copyLines();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "cutlines")) {
            cutlines();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "indentline")) {
            QPlainTextEdit::keyPressEvent(e);
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "backindentline")) {
            unindentText();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "forwardchar")) {
            forwardChar();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "backwardchar")) {
            backwardChar();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "forwardword")) {
            forwardWord();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "backwardword")) {
            backwardWord();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "nextline")) {
            nextLine();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "prevline")) {
            prevLine();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "newline")/* || key == "Return"*/) {
            newline();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "opennewlineabove")) {
            openNewlineAbove();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "opennewlinebelow")) {
            openNewlineBelow();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "duplicateline")) {
            duplicateLine();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "killline")) {
            killLine();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "killcurrentline")) {
            killCurrentLine();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "swaplineup")) {
            moveLineDownUp(true);
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "swaplinedown")) {
            moveLineDownUp(false);
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "scrolllineup")) {
            scrollLineUp();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "scrolllinedown")) {
            scrollLineDown();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "scrollup")) {
            scrollUp();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "scrolldown")) {
            scrollDown();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetoendofline")) {
            moveToEndOfLine();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetostartofline")) {
            moveToStartOfLine();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetostart")) {
            moveToStart();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetoend")) {
            moveToEnd();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetolineindentation")) {
            moveToLineIndentation();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "upcaseword")) {
            upcaseWord();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "downcaseword")) {
            downcaseWord();
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "capitalizeword")) {
            capitalizeWord();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "killbackwardword")) {
            killBackwardWord();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "killforwardword")) {
            killForwardWord();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "forwardpair")) {
            forwardPair();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "backwardpair")) {
            backwardPair();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "transposechar")) {
            transposeChar();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "setmark")) {
            setMark();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "exchangemark")) {
            exchangeMark();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "joinlines")) {
            joinLines();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "togglereadonlymode")/*|| key=="Alt+Meta+L"*/) {
//            setReadOnly(false);
            toggleReadOnlyMode();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "togglecomment")) {
            toggleComment(true);
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "removecomment")) {
            toggleComment(false);
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "switchbookmark")) {
            m_bIsShortCut = true;
            addOrDeleteBookMark();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetoprebookmark")) {
            moveToPreviousBookMark();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "movetonextbookmark")) {
            moveToNextBookMark();
            return;
        } else if (key == Utils::getKeyshortcutFromKeymap(m_settings, "editor", "mark")) {
            toggleMarkSelections();
            return;
        } else if (e->key() == Qt::Key_Insert && key != "Shift+Ins") {
            if (e->modifiers() == Qt::NoModifier) {
                setOverwriteMode(!overwriteMode());
                //update();
                if(!overwriteMode()){
                    auto cursor = this->textCursor();
                    cursor.clearSelection();
                    cursor.movePosition(QTextCursor::Right);
                    this->setTextCursor(cursor);

                    cursor = this->textCursor();
                    cursor.movePosition(QTextCursor::Left);
                    this->setTextCursor(cursor);
                }

                m_cursorMode = overwriteMode() ? Overwrite : Insert;
                emit cursorModeChanged(m_cursorMode);
                e->accept();
            }
        } else {
            // Post event to window widget if key match window key list.
            for (auto option : m_settings->settings->group("shortcuts.window")->options()) {
                if (key == m_settings->settings->option(option->key())->value().toString()) {
                    e->ignore();
                    return;
                }
            }

            // Post event to window widget if match Alt+0 ~ Alt+9
            QRegularExpression re("^Alt\\+\\d");
            QRegularExpressionMatch match = re.match(key);
            if (match.hasMatch()) {
                e->ignore();
                return;
            }

            // Text editor handle key self.
            QPlainTextEdit::keyPressEvent(e);
        }

        //return QPlainTextEdit::keyPressEvent(e);
    }
}

void TextEdit::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        const int deltaY = e->angleDelta().y();

        if (deltaY < 0) {
            qobject_cast<Window *>(this->window())->decrementFontSize();
        } else {
            qobject_cast<Window *>(this->window())->incrementFontSize();
        }

        return;
    }

    QPlainTextEdit::wheelEvent(e);
}

void TextEdit::contextMenuEvent(QContextMenuEvent *event)
{
    popRightMenu(event->globalPos());
}

void TextEdit::paintEvent(QPaintEvent *e)
{
    DPlainTextEdit::paintEvent(e);

    if (m_altModSelections.length() > 0) {
        for (auto sel : m_altModSelections) {
            if (sel.cursor.hasSelection()) {
                m_hasColumnSelection = true;
                break;
            } else {
                m_hasColumnSelection = false;
            }
        }
    }

    QColor lineColor = palette().text().color();

    if (m_bIsAltMod && !m_altModSelections.isEmpty()) {

        QTextCursor textCursor = this->textCursor();
        int cursorWidth = this->cursorWidth();
        //int cursoColumn = textCursor.positionInBlock();
        QPainter painter(viewport());
        QPen pen;
        pen.setColor(lineColor);
        pen.setWidth(cursorWidth);
        painter.setPen(pen);

        QList<int> rowList;
        for (int i = 0 ; i < m_altModSelections.size(); i++) {
            //if(m_altModSelections[i].cursor.positionInBlock() == cursoColumn){
            int row = m_altModSelections[i].cursor.blockNumber();
            if (!rowList.contains(row)) {
                rowList << row;
                QRect textCursorRect = this->cursorRect(m_altModSelections[i].cursor);
                painter.drawRect(textCursorRect);
            }
            // }
        }
    }
}

void TextEdit::resizeEvent(QResizeEvent *e)
{
    if(m_isSelectAll)
        selectTextInView();

    QPlainTextEdit::resizeEvent(e);
}

static bool isComment(const QString &text, int index, const QString &commentType)
{
    int length = commentType.length();

    Q_ASSERT(text.length() - index >= length);

    int i = 0;
    while (i < length) {
        if (text.at(index + i) != commentType.at(i))
            return false;
        ++i;
    }
    return true;
}


void TextEdit::unCommentSelection()
{
    if (!m_commentDefinition.isValid())
        return;

    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    QTextCursor cursor = textCursor();
    QTextDocument *doc = cursor.document();

    int pos = cursor.position();
    int anchor = cursor.anchor();
    int start = qMin(anchor, pos);
    int end = qMax(anchor, pos);
    bool anchorIsStart = (anchor == start);

    QTextBlock startBlock = doc->findBlock(start);
    QTextBlock endBlock = doc->findBlock(end);

    if (end > start && endBlock.position() == end) {
        --end;
        endBlock = endBlock.previous();
    }

    bool doMultiLineStyleUncomment = false;
    bool doMultiLineStyleComment = false;
    bool doSingleLineStyleUncomment = false;

    bool hasSelection = cursor.hasSelection();

    if (hasSelection && m_commentDefinition.hasMultiLineStyle()) {

        QString startText = startBlock.text();
        int startPos = start - startBlock.position();
        const int multiLineStartLength = m_commentDefinition.multiLineStart.length();
        bool hasLeadingCharacters = !startText.left(startPos).trimmed().isEmpty();

        if (startPos >= multiLineStartLength
                && isComment(startText,
                             startPos - multiLineStartLength,
                             m_commentDefinition.multiLineStart)) {
            startPos -= multiLineStartLength;
            start -= multiLineStartLength;
        }

        bool hasSelStart = startPos <= startText.length() - multiLineStartLength
                           && isComment(startText, startPos, m_commentDefinition.multiLineStart);

        QString endText = endBlock.text();
        int endPos = end - endBlock.position();
        const int multiLineEndLength = m_commentDefinition.multiLineEnd.length();
        bool hasTrailingCharacters =
            !endText.left(endPos).remove(m_commentDefinition.singleLine).trimmed().isEmpty()
            && !endText.mid(endPos).trimmed().isEmpty();

        if (endPos <= endText.length() - multiLineEndLength
                && isComment(endText, endPos, m_commentDefinition.multiLineEnd)) {
            endPos += multiLineEndLength;
            end += multiLineEndLength;
        }

        bool hasSelEnd = endPos >= multiLineEndLength
                         && isComment(endText, endPos - multiLineEndLength, m_commentDefinition.multiLineEnd);

        doMultiLineStyleUncomment = hasSelStart && hasSelEnd;
        doMultiLineStyleComment = !doMultiLineStyleUncomment
                                  && (hasLeadingCharacters
                                      || hasTrailingCharacters
                                      || !m_commentDefinition.hasSingleLineStyle());
    } else if (!hasSelection && !m_commentDefinition.hasSingleLineStyle()) {

        QString text = startBlock.text().trimmed();
        doMultiLineStyleUncomment = text.startsWith(m_commentDefinition.multiLineStart)
                                    && text.endsWith(m_commentDefinition.multiLineEnd);
        doMultiLineStyleComment = !doMultiLineStyleUncomment && !text.isEmpty();

        start = startBlock.position();
        end = endBlock.position() + endBlock.length() - 1;

        if (doMultiLineStyleUncomment) {
            int offset = 0;
            text = startBlock.text();
            const int length = text.length();
            while (offset < length && text.at(offset).isSpace())
                ++offset;
            start += offset;
        }
    }

    if (doMultiLineStyleUncomment) {
        cursor.setPosition(end);
        cursor.movePosition(QTextCursor::PreviousCharacter,
                            QTextCursor::KeepAnchor,
                            m_commentDefinition.multiLineEnd.length());
        //cursor.removeSelectedText();
        deleteTextEx(cursor);
        cursor.setPosition(start);
        cursor.movePosition(QTextCursor::NextCharacter,
                            QTextCursor::KeepAnchor,
                            m_commentDefinition.multiLineStart.length());
        //cursor.removeSelectedText();
        deleteTextEx(cursor);
    } else if (doMultiLineStyleComment) {
        cursor.setPosition(end);
        insertTextEx(cursor, m_commentDefinition.multiLineEnd);
        //cursor.insertText(m_commentDefinition.multiLineEnd);
        cursor.setPosition(start);
        insertTextEx(cursor, m_commentDefinition.multiLineStart);
        //cursor.insertText(m_commentDefinition.multiLineStart);
    } else {
        endBlock = endBlock.next();
        doSingleLineStyleUncomment = true;
        for (QTextBlock block = startBlock; block != endBlock; block = block.next()) {
            QString text = block.text().trimmed();
            if (!text.isEmpty() && !text.startsWith(m_commentDefinition.singleLine)) {
                doSingleLineStyleUncomment = false;
                break;
            }
        }

        const int singleLineLength = m_commentDefinition.singleLine.length();
        for (QTextBlock block = startBlock; block != endBlock; block = block.next()) {
            if (doSingleLineStyleUncomment) {
                QString text = block.text();
                int i = 0;
                while (i <= text.size() - singleLineLength) {
                    if (isComment(text, i, m_commentDefinition.singleLine)) {
                        cursor.setPosition(block.position() + i);
                        cursor.movePosition(QTextCursor::NextCharacter,
                                            QTextCursor::KeepAnchor,
                                            singleLineLength);
                        //cursor.removeSelectedText();
                        deleteTextEx(cursor);
                        break;
                    }
                    if (i < text.size()) {
                        if (!text.at(i).isSpace())
                            break;
                    }

                    ++i;
                }
            } else {
                const QString text = block.text();
                foreach (QChar c, text) {
                    if (!c.isSpace()) {
                        if (m_commentDefinition.isAfterWhiteSpaces)
                            cursor.setPosition(block.position() + text.indexOf(c));
                        else
                            cursor.setPosition(block.position());
                        insertTextEx(cursor, m_commentDefinition.singleLine);
                        break;
                    }
                }
            }
        }
    }

    // adjust selection when commenting out
    if (hasSelection && !doMultiLineStyleUncomment && !doSingleLineStyleUncomment) {
        cursor = textCursor();
        if (!doMultiLineStyleComment)
            start = startBlock.position(); // move the comment into the selection
        int lastSelPos = anchorIsStart ? cursor.position() : cursor.anchor();
        if (anchorIsStart) {
            cursor.setPosition(start);
            cursor.setPosition(lastSelPos, QTextCursor::KeepAnchor);
        } else {
            cursor.setPosition(lastSelPos);
            cursor.setPosition(start, QTextCursor::KeepAnchor);
        }
        setTextCursor(cursor);
    }
}

void TextEdit::setComment()
{
    //?????????????????????unCommentSelection()???if-else???uncomment???????????????
    if (!m_commentDefinition.isValid())
        return;
    if(m_isSelectAll)
        QPlainTextEdit::selectAll();

    QTextCursor cursor = textCursor();
    if (cursor.isNull()) return;

    QTextDocument *doc = cursor.document();

    int pos = cursor.position();
    int anchor = cursor.anchor();
    int start = qMin(anchor, pos);
    int end = qMax(anchor, pos);
    bool anchorIsStart = (anchor == start);

    QTextBlock startBlock = doc->findBlock(start);
    QTextBlock endBlock = doc->findBlock(end);
    if (end > start && endBlock.position() == end) {
        --end;
        endBlock = endBlock.previous();
    }
    bool doMultiLineStyleUncomment = false;
    bool doMultiLineStyleComment = false;
    bool doSingleLineStyleUncomment = false;

    bool hasSelection = cursor.hasSelection();

    if (hasSelection && m_commentDefinition.hasMultiLineStyle()) {

        QString startText = startBlock.text();
        int startPos = start - startBlock.position();
        const int multiLineStartLength = m_commentDefinition.multiLineStart.length();
        bool hasLeadingCharacters = !startText.left(startPos).trimmed().isEmpty();

        if (startPos >= multiLineStartLength
                && isComment(startText,
                             startPos - multiLineStartLength,
                             m_commentDefinition.multiLineStart)) {
            startPos -= multiLineStartLength;
            start -= multiLineStartLength;
        }

        bool hasSelStart = startPos <= startText.length() - multiLineStartLength
                           && isComment(startText, startPos, m_commentDefinition.multiLineStart);

        QString endText = endBlock.text();
        int endPos = end - endBlock.position();
        const int multiLineEndLength = m_commentDefinition.multiLineEnd.length();
        bool hasTrailingCharacters =
            !endText.left(endPos).remove(m_commentDefinition.singleLine).trimmed().isEmpty()
            && !endText.mid(endPos).trimmed().isEmpty();

        if (endPos <= endText.length() - multiLineEndLength
                && isComment(endText, endPos, m_commentDefinition.multiLineEnd)) {
            endPos += multiLineEndLength;
            end += multiLineEndLength;
        }

        bool hasSelEnd = endPos >= multiLineEndLength
                         && isComment(endText, endPos - multiLineEndLength, m_commentDefinition.multiLineEnd);

        doMultiLineStyleUncomment = hasSelStart && hasSelEnd;
        doMultiLineStyleComment = !doMultiLineStyleUncomment
                                  && (hasLeadingCharacters
                                      || hasTrailingCharacters
                                     // || !m_commentDefinition.hasSingleLineStyle());
                                         || m_commentDefinition.hasMultiLineStyle());
    } else if (!hasSelection && !m_commentDefinition.hasSingleLineStyle()) {

        QString text = startBlock.text().trimmed();
        doMultiLineStyleUncomment = text.startsWith(m_commentDefinition.multiLineStart)
                                    && text.endsWith(m_commentDefinition.multiLineEnd);
        qDebug() << m_commentDefinition.multiLineStart;
        doMultiLineStyleComment = !doMultiLineStyleUncomment && !text.isEmpty();

        qDebug() << "setComment:" << !text.isEmpty() << text << end << start;

        start = startBlock.position();
        end = endBlock.position() + endBlock.length() - 1;

        if (doMultiLineStyleUncomment) {
            int offset = 0;
            text = startBlock.text();
            const int length = text.length();
            while (offset < length && text.at(offset).isSpace())
                ++offset;
            start += offset;
        }
    }

    if (doMultiLineStyleComment) {
        cursor.setPosition(end);
        insertTextEx(cursor, m_commentDefinition.multiLineEnd);
        cursor.setPosition(start);
        insertTextEx(cursor, m_commentDefinition.multiLineStart);
    } else {
        endBlock = endBlock.next();
        doSingleLineStyleUncomment = true;
        for (QTextBlock block = startBlock; block != endBlock; block = block.next()) {
            QString text = block.text().trimmed();
            if (!text.isEmpty() && !text.startsWith(m_commentDefinition.singleLine)) {
                doSingleLineStyleUncomment = false;
                break;
            }
        }
        //qDebug()<<startBlock.text()<<startBlock.position() <<endBlock.text()<<endBlock.position() ;
        for (QTextBlock block = startBlock; block != endBlock; block = block.next()) {
            cursor.setPosition(block.position());
            insertTextEx(cursor, m_commentDefinition.singleLine);
        }
    }


    // adjust selection when commenting out
    if (hasSelection && !doMultiLineStyleUncomment && !doSingleLineStyleUncomment) {
        cursor = textCursor();
        if (!doMultiLineStyleComment)
            start = startBlock.position(); // move the comment into the selection
        int lastSelPos = anchorIsStart ? cursor.position() : cursor.anchor();
        if (anchorIsStart) {
            cursor.setPosition(start);
            cursor.setPosition(lastSelPos, QTextCursor::KeepAnchor);
        } else {
            cursor.setPosition(lastSelPos);
            cursor.setPosition(start, QTextCursor::KeepAnchor);
        }
        setTextCursor(cursor);
    }
}

void TextEdit::removeComment()
{
    //?????????????????????unCommentSelection()???if-else???comment???????????????
    if (!m_commentDefinition.isValid())
        return;

    qDebug() << m_commentDefinition.multiLineStart << m_commentDefinition.multiLineEnd << m_commentDefinition.singleLine;
    QString tep = m_commentDefinition.singleLine;
    QString abb = tep.remove(QRegExp("\\s"));

    if(m_isSelectAll)
        QPlainTextEdit::selectAll();


    QTextCursor cursor = textCursor();
    QTextDocument *doc = cursor.document();

    int pos = cursor.position();
    int anchor = cursor.anchor();
    int start = qMin(anchor, pos);
    int end = qMax(anchor, pos);
    bool anchorIsStart = (anchor == start);

    QTextBlock startBlock = doc->findBlock(start);
    QTextBlock endBlock = doc->findBlock(end);

    if (end > start && endBlock.position() == end) {
        --end;
        endBlock = endBlock.previous();
    }

    bool doMultiLineStyleUncomment = false;
    bool doMultiLineStyleComment = false;
    bool doSingleLineStyleUncomment = false;

    bool hasSelection = cursor.hasSelection();

    if (hasSelection && m_commentDefinition.hasMultiLineStyle()) {
        QString startText = startBlock.text();
        int startPos = start - startBlock.position();
        const int multiLineStartLength = m_commentDefinition.multiLineStart.length();
        bool hasLeadingCharacters = !startText.left(startPos).trimmed().isEmpty();

        if (startPos >= multiLineStartLength
                && isComment(startText,
                             startPos - multiLineStartLength,
                             m_commentDefinition.multiLineStart)) {
            startPos -= multiLineStartLength;
            start -= multiLineStartLength;
        }

        bool hasSelStart = startPos <= startText.length() - multiLineStartLength
                           && isComment(startText, startPos, m_commentDefinition.multiLineStart);

        QString endText = endBlock.text();
        int endPos = end - endBlock.position();
        const int multiLineEndLength = m_commentDefinition.multiLineEnd.length();
        bool hasTrailingCharacters =
            !endText.left(endPos).remove(m_commentDefinition.singleLine).trimmed().isEmpty()
            && !endText.mid(endPos).trimmed().isEmpty();

        if (endPos <= endText.length() - multiLineEndLength
                && isComment(endText, endPos, m_commentDefinition.multiLineEnd)) {
            endPos += multiLineEndLength;
            end += multiLineEndLength;
        }

        bool hasSelEnd = endPos >= multiLineEndLength
                         && isComment(endText, endPos - multiLineEndLength, m_commentDefinition.multiLineEnd);

        doMultiLineStyleUncomment = hasSelStart && hasSelEnd;
        doMultiLineStyleComment = !doMultiLineStyleUncomment
                                  && (hasLeadingCharacters
                                      || hasTrailingCharacters
                                      || !m_commentDefinition.hasSingleLineStyle());
    } else if (!hasSelection && !m_commentDefinition.hasSingleLineStyle()) {

        QString text = startBlock.text().trimmed();
        doMultiLineStyleUncomment = text.startsWith(m_commentDefinition.multiLineStart)
                                    && text.endsWith(m_commentDefinition.multiLineEnd);
        doMultiLineStyleComment = !doMultiLineStyleUncomment && !text.isEmpty();
        start = startBlock.position();
        end = endBlock.position() + endBlock.length() - 1;

        if (doMultiLineStyleUncomment) {
            int offset = 0;
            text = startBlock.text();
            const int length = text.length();
            while (offset < length && text.at(offset).isSpace())
                ++offset;
            start += offset;
        }
    }


    if (doMultiLineStyleUncomment) {
        cursor.setPosition(end);
        cursor.movePosition(QTextCursor::PreviousCharacter,
                            QTextCursor::KeepAnchor,
                            m_commentDefinition.multiLineEnd.length());
        //cursor.removeSelectedText();
        deleteTextEx(cursor);
        cursor.setPosition(start);
        cursor.movePosition(QTextCursor::NextCharacter,
                            QTextCursor::KeepAnchor,
                            m_commentDefinition.multiLineStart.length());
        //cursor.removeSelectedText();
        deleteTextEx(cursor);
    } else {
        int tmp = 0;//?????????????????????????????????????????????????????????
        endBlock = endBlock.next();
        doSingleLineStyleUncomment = true;
        for (QTextBlock block = startBlock; block != endBlock; block = block.next()) {
            QString text = block.text().trimmed();
            if (!text.isEmpty() && (!text.startsWith(m_commentDefinition.singleLine))) {
                if (!text.startsWith(abb)) {
                    doSingleLineStyleUncomment = false;
                    break;
                }
            }
        }

        int singleLineLength = m_commentDefinition.singleLine.length();
        QString text = startBlock.text().trimmed();

        if (text.startsWith(m_commentDefinition.singleLine)) {
            tmp = 0;
        } else if (text.startsWith(abb)) {
            tmp = 1;
        }
        QString check = "";
        for (QTextBlock block = startBlock; block != endBlock; block = block.next()) {
            if (doSingleLineStyleUncomment) {
                text = block.text();
                int i = 0;
                if (tmp == 1)
                    check = abb;
                else {
                    check = m_commentDefinition.singleLine;
                }
                while (i <= text.size() - singleLineLength) {
                    if (isComment(text, i, check)) {
                        cursor.setPosition(block.position() + i);
                        cursor.movePosition(QTextCursor::NextCharacter,
                                            QTextCursor::KeepAnchor,
                                            singleLineLength - tmp);
                        //cursor.removeSelectedText();
                        deleteTextEx(cursor);
                        break;
                    }
                    if (i < text.size()) {
                        if (!text.at(i).isSpace())
                            break;
                    }

                    ++i;
                }
            }
        }
    }


    // adjust selection when commenting out
    if (hasSelection && !doMultiLineStyleUncomment && !doSingleLineStyleUncomment) {
        cursor = textCursor();
        if (!doMultiLineStyleComment)
            start = startBlock.position(); // move the comment into the selection
        int lastSelPos = anchorIsStart ? cursor.position() : cursor.anchor();
        if (anchorIsStart) {
            cursor.setPosition(start);
            cursor.setPosition(lastSelPos, QTextCursor::KeepAnchor);
        } else {
            cursor.setPosition(lastSelPos);
            cursor.setPosition(start, QTextCursor::KeepAnchor);
        }
        setTextCursor(cursor);
    }
}

bool TextEdit::blockContainStrBrackets(int line)
{
    //?????????????????????
    QTextBlock curBlock = document()->findBlockByNumber(line);
    QString text = curBlock.text();
    QRegExp regExp("^\"*\"&");
    if (text.contains(regExp)) {
        QString curText = text.remove(regExp);
        return curText.contains("{");
    } else {
        return text.contains("{");
    }
}
