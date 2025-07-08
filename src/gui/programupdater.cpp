/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2021  Mike Tzou (Chocobo1)
 * Copyright (C) 2010  Christophe Dumez <chris@qbittorrent.org>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "programupdater.h"

#include <libtorrent/version.hpp>

#include <QtCore/qconfig.h>
#include <QtSystemDetection>
#include <QDebug>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include "base/global.h"
#include "base/logger.h"
#include "base/net/downloadmanager.h"
#include "base/preferences.h"
#include "base/utils/version.h"
#include "base/version.h"

namespace
{
    bool isVersionMoreRecent(const ProgramUpdater::Version &remoteVersion)
    {
        if (!remoteVersion.isValid())
            return false;

        const ProgramUpdater::Version currentVersion {QBT_VERSION_MAJOR, QBT_VERSION_MINOR, QBT_VERSION_BUGFIX, QBT_VERSION_BUILD};
        if (remoteVersion == currentVersion)
        {
            const bool isDevVersion = QStringLiteral(QBT_VERSION_STATUS).contains(
                QRegularExpression(u"(alpha|beta|rc)"_s));
            if (isDevVersion)
                return true;
        }
        return (remoteVersion > currentVersion);
    }
}

void ProgramUpdater::checkForUpdates() const
{
    const auto RSS_URL = u"https://husky.moe/feedqBittorent.xml"_s;
    const auto FALLBACK_URL = u"https://www.qbittorrent.org/versions.json"_s;

    // Don't change this User-Agent. In case our updater goes haywire,
    // the filehost can identify it and contact us.
    Net::DownloadManager::instance()->download(Net::DownloadRequest(RSS_URL).userAgent(USER_AGENT)
            , Preferences::instance()->useProxyForGeneralPurposes(), this, &ProgramUpdater::rssDownloadFinished);
    Net::DownloadManager::instance()->download(Net::DownloadRequest(FALLBACK_URL).userAgent(USER_AGENT)
            , Preferences::instance()->useProxyForGeneralPurposes(), this, &ProgramUpdater::fallbackDownloadFinished);

    m_hasCompletedOneReq = false;
}

ProgramUpdater::Version ProgramUpdater::getNewVersion() const
{
    return shouldUseFallback() ? m_fallbackRemoteVersion : m_remoteVersion;
}

QString ProgramUpdater::getNewContent() const
{
  return m_content;
}

QString ProgramUpdater::getNextUpdate() const
{
  return m_nextUpdate;
}

void ProgramUpdater::rssDownloadFinished(const Net::DownloadResult &result)
{
    if (result.status != Net::DownloadStatus::Success)
    {
        LogMsg(tr("Failed to download the update info. URL: %1. Error: %2").arg(result.url, result.errorString) , Log::WARNING);
        handleFinishedRequest();
        return;
    }

    const auto getStringValue = [](QXmlStreamReader &xml) -> QString
    {
        xml.readNext();
        return (xml.isCharacters() && !xml.isWhitespace())
            ? xml.text().toString()
            : QString {};
    };

#ifdef Q_OS_MACOS
    const QString OS_TYPE = u"Mac OS X"_s;
#elif defined(Q_OS_WIN)
    const QString OS_TYPE = u"Windows x64"_s;
#endif

    bool inItem = false;
    QString version;
    QString content;
    QString nextUpdate;
    QString updateLink;
    QString type;
    QXmlStreamReader xml(result.data);

    while (!xml.atEnd())
    {
        xml.readNext();

        if (xml.isStartElement())
        {
            if (xml.name() == u"item")
                inItem = true;
            else if (inItem && (xml.name() == u"link"))
                updateLink = getStringValue(xml);
            else if (inItem && (xml.name() == u"type"))
                type = getStringValue(xml);
            else if (inItem && (xml.name() == u"version"))
                version = getStringValue(xml);
            else if (inItem && (xml.name() == u"content"))
                content = getStringValue(xml);
            else if (inItem && (xml.name() == u"update"))
                nextUpdate = getStringValue(xml);
        }
        else if (xml.isEndElement())
        {
            if (inItem && (xml.name() == u"item"))
            {
                if (type.compare(OS_TYPE, Qt::CaseInsensitive) == 0)
                {
                    qDebug("The last update available is %s", qUtf8Printable(version));
                    if (!version.isEmpty())
                    {
                        qDebug("Detected version is %s", qUtf8Printable(version));
                        const ProgramUpdater::Version tmpVer {version};
                        if (isVersionMoreRecent(tmpVer))
                        {
                            m_remoteVersion = tmpVer;
                            m_updateURL = updateLink;
                            m_content = content;
                        }
                        m_nextUpdate = nextUpdate;
                    }
                    break;
                }

                inItem = false;
                updateLink.clear();
                type.clear();
                version.clear();
                content.clear();
                nextUpdate.clear();
            }
        }
    }

    handleFinishedRequest();
}

void ProgramUpdater::fallbackDownloadFinished(const Net::DownloadResult &result)
{
    if (result.status != Net::DownloadStatus::Success)
    {
        LogMsg(tr("Failed to download the update info. URL: %1. Error: %2").arg(result.url, result.errorString) , Log::WARNING);
        handleFinishedRequest();
        return;
    }

    const auto json = QJsonDocument::fromJson(result.data);

#if defined(Q_OS_MACOS)
    const QString platformKey = u"macos"_s;
#elif defined(Q_OS_WIN)
    const QString platformKey = u"win"_s;
#endif

    if (const QJsonValue verJSON = json[platformKey][u"version"_s]; verJSON.isString())
    {
        const ProgramUpdater::Version tmpVer {verJSON.toString()};
        if (isVersionMoreRecent(tmpVer))
            m_fallbackRemoteVersion = tmpVer;
    }

    handleFinishedRequest();
}

bool ProgramUpdater::updateProgram() const
{
    return QDesktopServices::openUrl(shouldUseFallback() ? u"https://www.qbittorrent.org/download"_s : m_updateURL);
}

void ProgramUpdater::handleFinishedRequest()
{
    if (m_hasCompletedOneReq)
        emit updateCheckFinished();
    else
        m_hasCompletedOneReq = true;
}

bool ProgramUpdater::shouldUseFallback() const
{
    return m_fallbackRemoteVersion > m_remoteVersion;
}
