/*
 * Copyright (C) 2017 ~ 2018 Deepin Technology Co., Ltd.
 *
 * Author:     rekols <rekols@foxmail.com>
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
#include "../widgets/window.h"
#include "../encodes/detectcode.h"
#include "../common/fileloadthread.h"
#include "editwrapper.h"
#include "../common/utils.h"
#include "leftareaoftextedit.h"
#include "drecentmanager.h"
#include "../common/settings.h"
#include <DSettingsOption>
#include <DSettings>
#include <unistd.h>
#include <QCoreApplication>
#include <QApplication>
#include <QSaveFile>
#include <QScrollBar>
#include <QScroller>
#include <QDebug>
#include <QTimer>
#include <QDir>
#include <DSettingsOption>
#include <DMenuBar>
#include <QFileInfo>

DCORE_USE_NAMESPACE

EditWrapper::EditWrapper(Window *window, QWidget *parent)
    : QWidget(parent),
      m_pWindow(window),
      m_pTextEdit(new TextEdit(this)),
      m_pBottomBar(new BottomBar(this)),
      m_pWaringNotices(new WarningNotices(WarningNotices::ResidentType, this))

{
    m_bQuit = false;
    m_pWaringNotices->hide();
    // Init layout and widgets.
    QHBoxLayout *m_layout = new QHBoxLayout;
    m_pLeftAreaTextEdit = m_pTextEdit->getLeftAreaWidget();
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_layout->addWidget(m_pLeftAreaTextEdit);
    m_layout->addWidget(m_pTextEdit);
    m_pTextEdit->setWrapper(this);

    QVBoxLayout *mainLayout = new QVBoxLayout;
    mainLayout->addLayout(m_layout);
    mainLayout->addWidget(m_pBottomBar);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    setLayout(mainLayout);

    connect(m_pTextEdit, &TextEdit::cursorModeChanged, this, &EditWrapper::handleCursorModeChanged);
    connect(m_pWaringNotices, &WarningNotices::reloadBtnClicked, this, &EditWrapper::reloadModifyFile);
    connect(m_pWaringNotices, &WarningNotices::saveAsBtnClicked, m_pWindow, &Window::saveAsFile);
    connect(m_pTextEdit->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        OnUpdateHighlighter();
        if ((m_pWindow->findBarIsVisiable() || m_pWindow->replaceBarIsVisiable()) &&
                (QString::compare(m_pWindow->getKeywordForSearchAll(), m_pWindow->getKeywordForSearch(), Qt::CaseInsensitive) == 0)) {
            m_pTextEdit->highlightKeywordInView(m_pWindow->getKeywordForSearchAll());
        }

        m_pTextEdit->markAllKeywordInView();
    });
}

EditWrapper::~EditWrapper()
{
    if (m_pTextEdit != nullptr) {
    disconnect(m_pTextEdit);
        delete m_pTextEdit;
        m_pTextEdit = nullptr;
    }
    if (m_pBottomBar != nullptr) {
        disconnect(m_pBottomBar);
        delete m_pBottomBar;
        m_pBottomBar = nullptr;
    }
    //delete ???????????????????????????????????????????????????????????????????????????????????????????????????????????????78042 ut002764
//    if (m_pWaringNotices != nullptr) {
//    disconnect(m_pWaringNotices);
//        delete m_pWaringNotices;
//        m_pWaringNotices = nullptr;
//    }
}

void EditWrapper::setQuitFlag()
{
    m_bQuit = true;
}

bool EditWrapper::isQuit()
{
    return m_bQuit;
}

bool EditWrapper::getFileLoading()
{
    return (m_bQuit || m_bFileLoading);
}

void EditWrapper::openFile(const QString &filepath, QString qstrTruePath, bool bIsTemFile)
{

    m_bIsTemFile = bIsTemFile;
    // update file path.
    updatePath(filepath, qstrTruePath);
    m_pTextEdit->setIsFileOpen();

    FileLoadThread *thread = new FileLoadThread(filepath);
    // begin to load the file.
    connect(thread, &FileLoadThread::sigLoadFinished, this, &EditWrapper::handleFileLoadFinished);
    connect(thread, &FileLoadThread::finished, thread, &FileLoadThread::deleteLater);
    thread->start();
}

bool EditWrapper::readFile(QByteArray encode)
{
    QByteArray newEncode = encode;
    if (newEncode.isEmpty()) {
        newEncode = DetectCode::GetFileEncodingFormat(m_pTextEdit->getFilePath());
        m_sFirstEncode = newEncode;
    }

	//QFile file(m_pTextEdit->getFilePath());
    QFile file2(m_pTextEdit->getTruePath());

    if (file2.open(QIODevice::ReadOnly)) {
        QByteArray fileContent = file2.readAll();
        QByteArray Outdata;
        DetectCode::ChangeFileEncodingFormat(fileContent, Outdata, newEncode, QString("UTF-8"));
        loadContent(Outdata);
        file2.close();
        m_sCurEncode = newEncode;
        updateModifyStatus(false);
        return true;
    }
    return false;
}

bool EditWrapper::saveAsFile(const QString &newFilePath, QByteArray encodeName)
{
    // use QSaveFile for safely save files.
    QSaveFile saveFile(newFilePath);
    saveFile.setDirectWriteFallback(true);

    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    QFile file(newFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    //auto append new line char to end of file when file's format is Linux/MacOS
    QByteArray fileContent = m_pTextEdit->toPlainText().toUtf8();

    QByteArray Outdata;
    DetectCode::ChangeFileEncodingFormat(fileContent, Outdata, QString("UTF-8"), encodeName);
    file.write(Outdata);
    // close and delete file.
    QFileDevice::FileError error = file.error();
    file.close();

    // flush file.
    if (!saveFile.flush()) {
        return false;
    }

    // ensure that the file is written to disk
    fsync(saveFile.handle());
    QFileInfo fi(filePath());
    m_tModifiedDateTime = fi.lastModified();

    // did save work?
    // only finalize if stream status == OK
    bool ok = (error == QFileDevice::NoError);

    return ok;
}

bool EditWrapper::saveAsFile()
{
    DFileDialog dialog(this, tr("Save"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.addComboBox(QObject::tr("Encoding"),  QStringList() << m_sFirstEncode);
    dialog.setDirectory(QDir::homePath());
    dialog.setNameFilter("*.txt");

    this->setUpdatesEnabled(false);
    int mode =  dialog.exec();
    this->setUpdatesEnabled(true);
    hideWarningNotices();

    if (QDialog::Accepted == mode) {
        const QString newFilePath = dialog.selectedFiles().value(0);
        if (newFilePath.isEmpty())
            return false;

        QFile qfile(newFilePath);

        if (!qfile.open(QFile::WriteOnly | QIODevice::Truncate)) {
            return false;
        }

        // ????????????????????????????????????
        QByteArray inputData = m_pTextEdit->toPlainText().toUtf8();
        QByteArray outData;
        DetectCode::ChangeFileEncodingFormat(inputData, outData, QString("UTF-8"), m_sFirstEncode);
        qfile.write(outData);
        qfile.close();

        return true;
    }

    return false;
}

bool EditWrapper::reloadFileEncode(QByteArray encode)
{
    //?????????????????????????????????
    if (m_sCurEncode == encode) return false;

    //???????????? ?????????????????????
    if (Utils::isDraftFile(m_pTextEdit->getFilePath()) &&  m_pTextEdit->toPlainText().isEmpty()) {
        m_sCurEncode = encode;
        m_sFirstEncode = encode;
        return true;
    }


    //1.????????????????????????????????????????????????,?????????????????????????????????.2.???????????????????????????
    if (m_pTextEdit->getModified()) {
        DDialog *dialog = new DDialog(tr("Encoding changed. Do you want to save the file now?"), "", this);
        //dialog->setWindowFlags(dialog->windowFlags() | Qt::WindowStaysOnBottomHint);
        dialog->setIcon(QIcon::fromTheme("deepin-editor"));
        dialog->addButton(QString(tr("Cancel")), false, DDialog::ButtonNormal);   //??????
        //dialog->addButton(QString(tr("Discard")), false, DDialog::ButtonNormal);//?????????
        dialog->addButton(QString(tr("Save")), true, DDialog::ButtonRecommend);   //??????
        int res = dialog->exec();//0  1

        //???????????????
        if (res == 0) return false;

        //?????????,????????????
	#if 0
        if (res == 1) {
            bool ok = readFile(encode);
            //if(ok && m_sCurEncode != m_sFirstEncode) m_pTextEdit->setTabbarModified(true);
            return ok;
        }
	#endif

        //??????
        if (res == 1) {
            //????????????
            if (Utils::isDraftFile(m_pTextEdit->getFilePath())) {
                if (saveDraftFile()) return readFile(encode);
                else return false;
            } else {
                return (saveFile() && readFile(encode));
            }
        }

        return false;
    } else {
        return readFile(encode);
    }
}

void EditWrapper::reloadModifyFile()
{
    hideWarningNotices();

    int curPos = m_pTextEdit->textCursor().position();
    int yoffset = m_pTextEdit->verticalScrollBar()->value();
    int xoffset = m_pTextEdit->horizontalScrollBar()->value();

    bool IsModified = m_pTextEdit->getModified();

    if (m_pWindow->getTabbar()->textAt(m_pWindow->getTabbar()->currentIndex()).front() == "*") {
        IsModified = true;
    }
    //??????????????????????????????????????????  ???????????????????????????????????????
    if (IsModified) {
        DDialog *dialog = new DDialog(tr("Do you want to save this file?"), "", this);
        dialog->setWindowFlags(dialog->windowFlags() | Qt::WindowStaysOnBottomHint);
        dialog->setIcon(QIcon::fromTheme("deepin-editor"));
        dialog->addButton(QString(tr("Cancel")), false, DDialog::ButtonNormal);//?????????
        dialog->addButton(QString(tr("Discard")), false, DDialog::ButtonNormal);//??????
        dialog->addButton(QString(tr("Save")), true, DDialog::ButtonRecommend);//??????
        dialog->setCloseButtonVisible(false);
        int res = dialog->exec();//0  1

        //????????????
        if (res == 0) return;

        //?????????
        if (res == 1) {
            //??????????????????
            readFile();
        }
        //??????
        if (res == 2) {
            //??????????????????????????? ?????????????????????????????????
            if (Utils::isDraftFile(m_pTextEdit->getFilePath())) {
                if (!saveDraftFile()) return;
            } else {
                if (!saveAsFile()) return;
            }
            //??????????????????
            readFile();
        }

    } else {
        //??????????????????
        readFile();
    }

    QFileInfo fi(m_pTextEdit->getTruePath());
    m_tModifiedDateTime = fi.lastModified();

    QTextCursor textcur = m_pTextEdit->textCursor();
    textcur.setPosition(curPos);
    m_pTextEdit->setTextCursor(textcur);
    m_pTextEdit->verticalScrollBar()->setValue(yoffset);
    m_pTextEdit->horizontalScrollBar()->setValue(xoffset);
}

QString EditWrapper::getTextEncode()
{
    return m_sCurEncode;
}

bool EditWrapper::saveFile()
{
    QString qstrFilePath = m_pTextEdit->getTruePath();
    QFile file(qstrFilePath);
    hideWarningNotices();

    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QByteArray fileContent = m_pTextEdit->toPlainText().toLocal8Bit();
        if (!fileContent.isEmpty()) {
            QByteArray Outdata;
            DetectCode::ChangeFileEncodingFormat(fileContent, Outdata, QString("UTF-8"), m_sCurEncode);
            // ?????? iconv ????????????
            if(Outdata.isEmpty()) {
                qWarning() << QString("iconv Encode Transformat from '%1' to '%2' Fail!")
                              .arg(QString("UTF-8")).arg(m_sCurEncode)
                           << ", start QTextCodec Encode Transformat.";
                // ?????? QTextCodec ??????????????????
                QTextCodec *codec = QTextCodec::codecForName(m_sCurEncode.toUtf8());
                QByteArray encodedString = codec->fromUnicode(fileContent);

                if (encodedString.isEmpty()) {
                    qWarning() << "Both iconv and QTextCodec Encode Transformat Fail!";
                } else {
                    qWarning() << QString("QTextCodec Encode Transformat from '%1' to '%2' Success!")
                                  .arg(QString("UTF-8")).arg(m_sCurEncode);
                    Outdata = encodedString;
                }
            }

            if(Outdata.isEmpty() == false) {
                // ???????????????????????????????????????????????????????????????????????????
                // ?????????????????????????????????????????????
                file.write(Outdata);
            }

            QFileDevice::FileError error = file.error();
            file.close();
            m_sFirstEncode = m_sCurEncode;

            QFileInfo fi(qstrFilePath);
            m_tModifiedDateTime = fi.lastModified();

            // did save work?
            // only finalize if stream status == OK
            // ??????????????????????????????????????????????????????ok??????false
            bool ok = (Outdata.isEmpty() == false && error == QFileDevice::NoError);

            // update status.
            if (ok)  updateModifyStatus(false);
            m_bIsTemFile = false;
            return ok;

        } else {
            file.write(fileContent);
            QFileDevice::FileError error = file.error();
            file.close();
            m_sFirstEncode = m_sCurEncode;

            QFileInfo fi(qstrFilePath);
            m_tModifiedDateTime = fi.lastModified();

            // did save work?
            // only finalize if stream status == OK
            bool ok = (error == QFileDevice::NoError);

            // update status.
            if (ok)  updateModifyStatus(false);
            m_bIsTemFile = false;
            return ok;
        }
    } else {
        DMessageManager::instance()->sendMessage(this->window()->getStackedWgt()->currentWidget(), QIcon(":/images/warning.svg")
                                                 , QString(tr("You do not have permission to save %1")).arg(file.fileName()));
        return false;
    }

}

bool EditWrapper::saveTemFile(QString qstrDir)
{
    QFile file(qstrDir);

    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QByteArray fileContent = m_pTextEdit->toPlainText().toLocal8Bit();
//        if(!fileContent.isEmpty())
//        {
        QByteArray Outdata;
        DetectCode::ChangeFileEncodingFormat(fileContent, Outdata, QString("UTF-8"), m_sCurEncode);
        file.write(Outdata);
        QFileDevice::FileError error = file.error();
        file.close();
        m_sFirstEncode = m_sCurEncode;

        // did save work?
        // only finalize if stream status == OK
        bool ok = (error == QFileDevice::NoError);

        // update status.
        if (ok)  updateModifyStatus(isModified());
        return ok;

//        }else {
//            file.write(fileContent);
//            QFileDevice::FileError error = file.error();
//            file.close();
//            m_sFirstEncode = m_sCurEncode;

              // did save work?
              // only finalize if stream status == OK
//            bool ok = (error == QFileDevice::NoError);

//            // update status.
//            if (ok)  updateModifyStatus(true);
//            return ok;
//        }
    } else {
        return false;
    }
}

void EditWrapper::updatePath(const QString &file, QString qstrTruePath)
{
    if (qstrTruePath.isEmpty()) {
        qstrTruePath = file;
    }

    QFileInfo fi(qstrTruePath);
    m_tModifiedDateTime = fi.lastModified();

    m_pTextEdit->setFilePath(file);
    m_pTextEdit->setTruePath(qstrTruePath);
}

bool EditWrapper::isModified()
{
    //???????????????????????????????????????????????????
    // bool modified = (m_sFirstEncode != m_sCurEncode || m_pTextEdit->document()->isModified());
    bool modified =  m_pTextEdit->getModified();

    return  modified | m_bIsTemFile;
}

bool EditWrapper::isDraftFile()
{
    return Utils::isDraftFile(m_pTextEdit->getFilePath());
}

bool EditWrapper::isPlainTextEmpty()
{
    return m_pTextEdit->document()->isEmpty();
}

bool EditWrapper::isTemFile()
{
    return m_bIsTemFile;
}

bool EditWrapper::saveDraftFile()
{
    DFileDialog dialog(this, tr("Save"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.addComboBox(QObject::tr("Encoding"),  QStringList() << m_sCurEncode);
    dialog.setDirectory(QDir::homePath());
    dialog.setNameFilter("*.txt");

    if (m_pWindow) {
        QRegularExpression reg("[^*](.+)");
        QRegularExpressionMatch match = reg.match(m_pWindow->getTabbar()->currentName());
        dialog.selectFile(match.captured(0) + ".txt");
    }



    this->setUpdatesEnabled(false);
    int mode =  dialog.exec(); // 0???????????? 1??????
    this->setUpdatesEnabled(true);
    hideWarningNotices();

    if (mode == 1) {
        const QString newFilePath = dialog.selectedFiles().value(0);
        if (newFilePath.isEmpty())
            return false;

        QFile qfile(newFilePath);

        if (!qfile.open(QFile::WriteOnly)) {
            return false;
        }

        // ????????????????????????????????????
        QByteArray inputData = m_pTextEdit->toPlainText().toUtf8();
        QByteArray outData;
        DetectCode::ChangeFileEncodingFormat(inputData, outData, QString("UTF-8"), m_sCurEncode);
        qfile.write(outData);
        qfile.close();

        //?????????????????? ?????????????????????
        m_sFirstEncode = m_sCurEncode;
        QFile(m_pTextEdit->getFilePath()).remove();
        updateSaveAsFileName(m_pTextEdit->getFilePath(), newFilePath);
        m_pTextEdit->document()->setModified(false);
        return true;
    }

    return false;
}

void EditWrapper::hideWarningNotices()
{
    if (m_pWaringNotices->isVisible()) {
        m_pWaringNotices->hide();
    }
}

//??????????????? ???????????????????????????,???????????????
void EditWrapper::checkForReload()
{
    if (Utils::isDraftFile(m_pTextEdit->getTruePath())) {
        return;
    }

    QFileInfo fi(m_pTextEdit->getTruePath());

    QTimer::singleShot(50, [=]() {
        if (fi.lastModified() == m_tModifiedDateTime || m_pWaringNotices->isVisible()) {
            return;
        }

    QFileInfo fi2(m_pTextEdit->getTruePath());

    if (!fi2.exists()) {
        m_pWaringNotices->setMessage(tr("File removed on the disk. Save it now?"));
        m_pWaringNotices->setSaveAsBtn();
        m_pWaringNotices->show();
        DMessageManager::instance()->sendMessage(m_pTextEdit, m_pWaringNotices);
    } else if (fi2.lastModified() != m_tModifiedDateTime) {
        m_pWaringNotices->setMessage(tr("File has changed on disk. Reload?"));
        m_pWaringNotices->setReloadBtn();
        m_pWaringNotices->show();
        DMessageManager::instance()->sendMessage(m_pTextEdit, m_pWaringNotices);
    }

    });
}

void EditWrapper::showNotify(const QString &message)
{
    if (m_pTextEdit->getReadOnlyPermission() || m_pTextEdit->getReadOnlyMode()) {
        DMessageManager::instance()->sendMessage(m_pTextEdit, QIcon(":/images/warning.svg"), message);
    } else {
        DMessageManager::instance()->sendMessage(m_pTextEdit, QIcon(":/images/ok.svg"), message);
    }
}


void EditWrapper::handleCursorModeChanged(TextEdit::CursorMode mode)
{
    switch (mode) {
    case TextEdit::Insert:
        m_pBottomBar->setCursorStatus(tr("INSERT"));
        break;
    case TextEdit::Overwrite:
        m_pBottomBar->setCursorStatus(tr("OVERWRITE"));
        break;
    case TextEdit::Readonly:
        m_pBottomBar->setCursorStatus(tr("R/O"));
        break;
    }
}


void EditWrapper::handleFileLoadFinished(const QByteArray &encode, const QByteArray &content)
{

    qint64 time1 = QDateTime::currentMSecsSinceEpoch();
    m_Definition = m_Repository.definitionForFileName(m_pTextEdit->getFilePath());
    qDebug() << "===========begin load file:" << time1;
    qDebug() << m_Definition.isValid() << m_Definition.filePath() << m_Definition.translatedName();
    if (m_Definition.isValid() && !m_Definition.filePath().isEmpty()) {
        if (!m_pSyntaxHighlighter) m_pSyntaxHighlighter = new CSyntaxHighlighter(m_pTextEdit->document());
        QString m_themePath = Settings::instance()->settings->option("advance.editor.theme")->value().toString();
        if (m_themePath.contains("dark")) {
            m_pSyntaxHighlighter->setTheme(m_Repository.defaultTheme(KSyntaxHighlighting::Repository::DarkTheme));
        } else {
            m_pSyntaxHighlighter->setTheme(m_Repository.defaultTheme(KSyntaxHighlighting::Repository::LightTheme));
        }

        if (m_pSyntaxHighlighter) m_pSyntaxHighlighter->setDefinition(m_Definition);;
        m_pTextEdit->setSyntaxDefinition(m_Definition);
        m_pBottomBar->getHighlightMenu()->setCurrentTextOnly(m_Definition.translatedName());

    }

    qint64 time2 = QDateTime::currentMSecsSinceEpoch();
    qDebug() << "===========load SyntaxHighter:" << time2 - time1;



    if (!Utils::isDraftFile(m_pTextEdit->getFilePath())) {
        DRecentData data;
        data.appName = "Deepin Editor";
        data.appExec = "deepin-editor";
        DRecentManager::addItem(m_pTextEdit->getFilePath(), data);
    }

    bool flag = m_pTextEdit->getReadOnlyPermission();
    if (flag == true) m_pTextEdit->setReadOnlyPermission(false);

    m_bFileLoading = true;
    m_sCurEncode = encode;
    m_sFirstEncode = encode;

    //????????????????????????
    if (m_bIsTemFile) {
        // m_bIsTemFile = false;
        updateModifyStatus(true);
    }

    loadContent(content);
    //??????????????????????????????????????????????????????????????????
    //clearDoubleCharaterEncode();

    qint64 time3 = QDateTime::currentMSecsSinceEpoch();
    qDebug() << "===========end load file:" << time3 - time1;

    PerformanceMonitor::openFileFinish(filePath(), QFileInfo(filePath()).size());

    m_bFileLoading = false;
    if (flag == true) m_pTextEdit->setReadOnlyPermission(true);
    if (m_bQuit) return;
    m_pTextEdit->setTextFinished();

    QStringList temFileList = Settings::instance()->settings->option("advance.editor.browsing_history_temfile")->value().toStringList();

    for (int var = 0; var < temFileList.count(); ++var) {
        QJsonParseError jsonError;
        // ????????? JSON ??????
        QJsonDocument doucment = QJsonDocument::fromJson(temFileList.value(var).toUtf8(), &jsonError);
        // ?????????????????????
        if (!doucment.isNull() && (jsonError.error == QJsonParseError::NoError)) {
            if (doucment.isObject()) {
                QString temFilePath;
                QString localPath;
                // JSON ???????????????
                QJsonObject object = doucment.object();  // ???????????????

                if (object.contains("localPath") || object.contains("temFilePath")) {
                    // ??????????????? key
                    QJsonValue localPathValue = object.value("localPath");  // ???????????? key ????????? value
                    QJsonValue temFilePathValue = object.value("temFilePath");  // ???????????? key ????????? value

                    if (localPathValue.toString() == m_pTextEdit->getFilePath()) {
                        QJsonValue value = object.value("cursorPosition");  // ???????????? key ????????? value

                        if (value.isString()) {
                            QTextCursor cursor = m_pTextEdit->textCursor();
                            cursor.setPosition(value.toString().toInt());
                            m_pTextEdit->setTextCursor(cursor);
                            OnUpdateHighlighter();
                            break;
                        }
                    } else if (temFilePathValue.toString() == m_pTextEdit->getFilePath()) {
                        QJsonValue value = object.value("cursorPosition");  // ???????????? key ????????? value

                        if (value.isString()) {
                            QTextCursor cursor = m_pTextEdit->textCursor();
                            cursor.setPosition(value.toString().toInt());
                            m_pTextEdit->setTextCursor(cursor);
                            OnUpdateHighlighter();
                            break;
                        }
                    }
                }
            }
        }
    }
    //????????????????????????
    if (m_bIsTemFile) {
        //m_bIsTemFile = false;
        updateModifyStatus(true);
    }

    if (m_pSyntaxHighlighter) {
        m_pSyntaxHighlighter->setEnableHighlight(true);
        OnUpdateHighlighter();
    }

    m_pBottomBar->setEncodeName(m_sCurEncode);
}


void EditWrapper::OnThemeChangeSlot(QString theme)
{
    QVariantMap jsonMap = Utils::getThemeMapFromPath(theme);
    QString backgroundColor = jsonMap["editor-colors"].toMap()["background-color"].toString();
    QString textColor = jsonMap["Normal"].toMap()["text-color"].toString();

    //???????????????
    QPalette palette = m_pBottomBar->palette();
    palette.setColor(QPalette::Background, backgroundColor);
    palette.setColor(QPalette::Text, textColor);
    m_pBottomBar->setPalette(palette);

    //???????????????
    if (m_pSyntaxHighlighter) {
        if (QColor(backgroundColor).lightness() < 128) {
            m_pSyntaxHighlighter->setTheme(m_Repository.defaultTheme(KSyntaxHighlighting::Repository::DarkTheme));
        } else {
            m_pSyntaxHighlighter->setTheme(m_Repository.defaultTheme(KSyntaxHighlighting::Repository::LightTheme));
        }
        m_pSyntaxHighlighter->rehighlight();
    }

    m_pTextEdit->setTheme(theme);
}

void EditWrapper::UpdateBottomBarWordCnt(int cnt)
{
    m_pBottomBar->updateWordCount(cnt);
}

void EditWrapper::OnUpdateHighlighter()
{
    if (m_pSyntaxHighlighter  && !m_bQuit && !m_bHighlighterAll) {
        QScrollBar *pScrollBar = m_pTextEdit->verticalScrollBar();
        QPoint startPoint = QPointF(0, 0).toPoint();
        QTextBlock beginBlock = m_pTextEdit->cursorForPosition(startPoint).block();
        QTextBlock endBlock;

        if (pScrollBar->maximum() > 0) {
            QPoint endPoint = QPointF(0, 1.5 * m_pTextEdit->height()).toPoint();
            endBlock = m_pTextEdit->cursorForPosition(endPoint).block();
        } else {
            endBlock = m_pTextEdit->document()->lastBlock();
        }

        if (!beginBlock.isValid() || !endBlock.isValid()) {
            return;
        }

        for (QTextBlock var = beginBlock; var != endBlock; var = var.next()) {
            m_pSyntaxHighlighter->setEnableHighlight(true);
            m_pSyntaxHighlighter->rehighlightBlock(var);
            m_pSyntaxHighlighter->setEnableHighlight(false);
        }

        m_pSyntaxHighlighter->setEnableHighlight(true);
        m_pSyntaxHighlighter->rehighlightBlock(endBlock);
        m_pSyntaxHighlighter->setEnableHighlight(false);
    }
}

void EditWrapper::updateHighlighterAll()
{
    if (m_pSyntaxHighlighter  && !m_bQuit && !m_bHighlighterAll) {
        QTextBlock beginBlock = m_pTextEdit->document()->firstBlock();
        QTextBlock endBlock = m_pTextEdit->document()->lastBlock();

        if (!beginBlock.isValid() || !endBlock.isValid()) {
            return;
        }

        for (QTextBlock var = beginBlock; var != endBlock; var = var.next()) {
            m_pSyntaxHighlighter->setEnableHighlight(true);
            m_pSyntaxHighlighter->rehighlightBlock(var);
            m_pSyntaxHighlighter->setEnableHighlight(false);
        }

        m_pSyntaxHighlighter->setEnableHighlight(true);
        m_pSyntaxHighlighter->rehighlightBlock(endBlock);
        m_pSyntaxHighlighter->setEnableHighlight(false);

        m_bHighlighterAll = true;
    }
}

void EditWrapper::updateModifyStatus(bool bModified)
{
    if (getFileLoading()) return;
    if (!bModified)
        m_pTextEdit->updateSaveIndex();
    m_pTextEdit->document()->setModified(bModified);
    Window *pWindow = static_cast<Window *>(QWidget::window());
    pWindow->updateModifyStatus(m_pTextEdit->getFilePath(), bModified);
}

void EditWrapper::updateSaveAsFileName(QString strOldFilePath, QString strNewFilePath)
{
    m_pWindow->updateSaveAsFileName(strOldFilePath, strNewFilePath);
}

//yanyuhan
void EditWrapper::setLineNumberShow(bool bIsShow, bool bIsFirstShow)
{
    if (bIsShow && !bIsFirstShow) {
        int lineNumberAreaWidth = m_pTextEdit->getLeftAreaWidget()->m_pLineNumberArea->width();
        int leftAreaWidth = m_pTextEdit->getLeftAreaWidget()->width();
        m_pTextEdit->getLeftAreaWidget()->m_pLineNumberArea->show();
        //m_pTextEdit->getLeftAreaWidget()->setFixedWidth(leftAreaWidth + lineNumberAreaWidth);

    } else if (!bIsShow) {
        int lineNumberAreaWidth = m_pTextEdit->getLeftAreaWidget()->m_pLineNumberArea->width();
        int leftAreaWidth = m_pTextEdit->getLeftAreaWidget()->width();
        m_pTextEdit->getLeftAreaWidget()->m_pLineNumberArea->hide();
        //m_pTextEdit->getLeftAreaWidget()->setFixedWidth(leftAreaWidth - lineNumberAreaWidth);
    }
    m_pTextEdit->bIsSetLineNumberWidth = bIsShow;
    m_pTextEdit->updateLeftAreaWidget();
}

//???????????????
void EditWrapper::setShowBlankCharacter(bool ok)
{
    if (ok) {
        QTextOption opts = m_pTextEdit->document()->defaultTextOption();
        QTextOption::Flags flag = opts.flags();
        flag |= QTextOption::ShowTabsAndSpaces;
        // flag |= QTextOption::ShowLineAndParagraphSeparators;
        opts.setFlags(flag);
        m_pTextEdit->document()->setDefaultTextOption(opts);
    } else {
        QTextOption opts = m_pTextEdit->document()->defaultTextOption();
        QTextOption::Flags flag = opts.flags();
        flag &= ~QTextOption::ShowTabsAndSpaces;
        // flag &= ~QTextOption::ShowLineAndParagraphSeparators;
        opts.setFlags(flag);
        m_pTextEdit->document()->setDefaultTextOption(opts);
    }
}

BottomBar *EditWrapper::bottomBar()
{
    return m_pBottomBar;
}

QString EditWrapper::filePath()
{
    return  m_pTextEdit->getFilePath();
}

TextEdit *EditWrapper::textEditor()
{
    return m_pTextEdit;
}

Window *EditWrapper::window()
{
    Window *window = static_cast<Window *>(QWidget::window());

    if (m_pWindow != window) {
        m_pWindow = window;
    }

    return m_pWindow;
}

//????????????????????? ??????????????? ?????????
void EditWrapper::loadContent(const QByteArray &content)
{
    m_pBottomBar->setChildEnabled(false);
    m_pWindow->setPrintEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_pTextEdit->clear();
    m_bQuit = false;
    //QTextDocument *doc = m_pTextEdit->document();
    QTextCursor cursor = m_pTextEdit->textCursor();

    QString strContent = content.data();

    int len = strContent.length();
    //???????????????????????????
    int InitContentPos = 5 * 1024;
    //????????????????????????
    int step = 1 * 1024 * 1024;
    //??????????????????
    int cnt = len / step;
    //??????????????????
    int mod = len % step;

    int max = 40 * 1024 * 1024;

    QString data;

    if (len > max) {
        for (int i = 0; i < cnt; i++) {
            //???????????????
            if (i == 0 && !m_bQuit) {
                data = strContent.mid(i * step, step);
                cursor.insertText(data);
                QTextCursor firstLineCursor = m_pTextEdit->textCursor();
                firstLineCursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
                m_pTextEdit->setTextCursor(firstLineCursor);
                //????????????????????????
                OnUpdateHighlighter();
                QApplication::processEvents();
                continue;
            }
            if (!m_bQuit) {
                data = strContent.mid(i * step, step);
                cursor.insertText(data);
                QApplication::processEvents();
                if (!m_bQuit && i == cnt - 1 && mod > 0) {
                    data = strContent.mid(cnt * step, mod);
                    cursor.insertText(data);
                    QApplication::processEvents();
                }
            }
        }

    } else {
        //???????????????
        if (!m_bQuit && len > InitContentPos) {
            data = strContent.mid(0, InitContentPos);
            cursor.insertText(data);
            QTextCursor firstLineCursor = m_pTextEdit->textCursor();
            firstLineCursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
            m_pTextEdit->setTextCursor(firstLineCursor);
            //????????????????????????
            OnUpdateHighlighter();
            QApplication::processEvents();
            if (!m_bQuit) {
                data = strContent.mid(InitContentPos, len - InitContentPos);
                cursor.insertText(data);
            }
        } else {
            if (!m_bQuit) {
                cursor.insertText(strContent);
                QTextCursor firstLineCursor = m_pTextEdit->textCursor();
                firstLineCursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
                m_pTextEdit->setTextCursor(firstLineCursor);
                //????????????????????????
                OnUpdateHighlighter();
            }
        }
    }
    m_pWindow->setPrintEnabled(true);
    m_pBottomBar->setChildEnabled(true);
    QApplication::restoreOverrideCursor();
}

void EditWrapper::clearDoubleCharaterEncode()
{
    if (QFileInfo(filePath()).baseName().contains("double")
            || QFileInfo(filePath()).baseName().contains("user")
            || QFileInfo(filePath()).baseName().contains("four")) {
        if (QFileInfo(filePath()).size() > 500 * 1024) {
            return;
        }
        emit sigClearDoubleCharaterEncode();
    }
}
