/* -*- Mode: C++; indent-tabs-mode: nil; tab-width: 4 -*-
 * -*- coding: utf-8 -*-
 *
 * Copyright (C) 2011 ~ 2018 Deepin, Inc.
 *               2011 ~ 2018 Wang Yong
 *
 * Author:     Wang Yong <wangyong@deepin.com>
 * Maintainer: Wang Yong <wangyong@deepin.com>
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

#include "policykithelper.h"
#include "dbus.h"
#include "utils.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QtCore/QFile>

DBus::DBus(QObject *parent) : QObject(parent)
{
}

bool DBus::saveFile(const QByteArray &path, const QByteArray &text, const QByteArray &encoding)
{
    const QString filepath = QString::fromUtf8(path);

    if (PolicyKitHelper::instance()->checkAuthorization("com.deepin.editor.saveFile", getpid())) {
        // Create file if filepath is not exists.
        if (!Utils::fileExists(filepath)) {
            QString directory = QFileInfo(filepath).dir().absolutePath();

            QDir().mkpath(directory);
            if (QFile(filepath).open(QIODevice::ReadWrite)) {
                qDebug() << QString("File %1 not exists, create one.").arg(filepath);
            }
        }

        // Save content to file.
        QFile file(filepath);
        if(!file.open(QIODevice::WriteOnly | QIODevice::Text)){
            qDebug() << "Can't write file: " << filepath;

            return false;
        }

        QTextStream out(&file);
        out.setCodec(encoding);
        out << text;
        file.close();

        return true;
    } else{
        return false;
    }
}
