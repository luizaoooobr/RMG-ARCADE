/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "RomSearcherThread.hpp"

#include <RMG-Core/CachedRomHeaderAndSettings.hpp>
#include <RMG-Core/Emulation.hpp>

#include <QElapsedTimer>
#include <QDirIterator>

using namespace Thread;

RomSearcherThread::RomSearcherThread(QObject *parent) : QThread(parent)
{
    qRegisterMetaType<CoreRomType>("CoreRomType");
    qRegisterMetaType<CoreRomHeader>("CoreRomHeader");
    qRegisterMetaType<CoreRomSettings>("CoreRomSettings");
    qRegisterMetaType<RomSearcherThreadData>("RomSearcherThreadData");
}

RomSearcherThread::~RomSearcherThread(void)
{
    this->Stop();
}

void RomSearcherThread::SetDirectory(QString directory)
{
    this->directory = directory;
}

void RomSearcherThread::SetRecursive(bool value)
{
    this->recursive = value;
}

void RomSearcherThread::SetMaximumFiles(int value)
{
    this->maxItems = value;
}

void RomSearcherThread::Stop(void)
{
    this->stop = true;
    while (this->isRunning())
    {
        this->wait();
    }
}

void RomSearcherThread::run(void)
{
    this->stop = false;
    this->searchDirectory(this->directory);
}

void RomSearcherThread::searchDirectory(QString directory)
{
    QStringList filter;
    filter << "*.N64";
    filter << "*.Z64";
    filter << "*.V64";
    filter << "*.NDD";
    filter << "*.D64";
    filter << "*.ZIP";
    filter << "*.7Z";

    QDirIterator::IteratorFlag flag = this->recursive ? 
        QDirIterator::Subdirectories : 
        QDirIterator::NoIteratorFlags;
    QDirIterator romDirIt(directory, filter, QDir::Files, flag);

    CoreRomType     type;
    CoreRomHeader   header;
    CoreRomSettings defaultSettings;
    CoreRomSettings settings;
    bool            ret;

    QList<QString> roms;
    while (romDirIt.hasNext())
    {
        roms.push_back(romDirIt.next());
    }

    const int romAmount = std::min(this->maxItems, (int)roms.size());

    // our ROM data
    QList<RomSearcherThreadData> data;

    // keep track of the time
    QElapsedTimer timer;
    timer.start();

    for (int i = 0; i < romAmount; i++)
    {
        QString file = roms.at(i);

        if (CoreGetCachedRomHeaderAndSettings(file.toStdU32String(), &type, &header, nullptr, &settings))
        {
            data.push_back(
            {
                file,
                type,
                header,
                settings
            });
        }
        else
        {
            // Not an N64 rom: it may be an FBNeo arcade set (zip/7z whose stem
            // matches a driver). Those have no N64 header, so synthesize one for
            // the browser and tag it as an Arcade rom.
            std::string arcadeShort;
            std::string arcadeFull;
            std::string arcadeId;
            if (CoreGetFbneoRomInfo(file.toStdU32String(), arcadeShort, arcadeFull, arcadeId))
            {
                CoreRomHeader   arcadeHeader   = {};
                CoreRomSettings arcadeSettings = {};
                arcadeHeader.Name         = arcadeShort;
                arcadeHeader.GameID       = arcadeShort;
                arcadeSettings.GoodName   = arcadeFull;
                // The lobby keys rooms and local-rom lookups on this field.
                arcadeSettings.MD5        = arcadeId;

                data.push_back(
                {
                    file,
                    CoreRomType::Arcade,
                    arcadeHeader,
                    arcadeSettings
                });
            }
        }

        // we need to give the UI some breathing room,
        // so when 10ms have passed,
        // send our data to the UI and
        // clear our data and restart
        // the timer
        if (timer.elapsed() >= 10)
        {
            emit this->RomsFound(data, (i + 1), romAmount);
            data.clear();
            timer.start();
        }

        if (this->stop)
        {
            break;
        }
    }

    // when we're done and
    // we still have data left,
    // send it to the UI before we finish
    if (!data.isEmpty())
    {
        emit this->RomsFound(data, romAmount, romAmount);
    }

    emit this->Finished(this->stop);
}

