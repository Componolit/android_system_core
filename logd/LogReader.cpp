/*
 * Copyright (C) 2012-2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>

#include <cutils/sockets.h>

#include "LogReader.h"
#include "FlushCommand.h"

LogReader::LogReader(LogBuffer *logbuf)
        : SocketListener(getLogSocket(), true)
        , mLogbuf(*logbuf)
{ }

// When we are notified a new log entry is available, inform
// all of our listening sockets.
void LogReader::notifyNewLog() {
    FlushCommand command(*this);
    runOnEachSocket(&command);
}

bool LogReader::onDataAvailable(SocketClient *cli) {
    prctl(PR_SET_NAME, "logd.reader");

    char buffer[255];

    int len = read(cli->getSocket(), buffer, sizeof(buffer) - 1);
    if (len <= 0) {
        doSocketDelete(cli);
        return false;
    }
    buffer[len] = '\0';

    unsigned long tail = 0;
    static const char _tail[] = " tail=";
    char *cp = strstr(buffer, _tail);
    if (cp) {
        tail = atol(cp + sizeof(_tail) - 1);
    }

    log_time start(log_time::EPOCH);
    static const char _start[] = " start=";
    cp = strstr(buffer, _start);
    if (cp) {
        // Parse errors will result in current time
        start.strptime(cp + sizeof(_start) - 1, "%s.%q");
    }

    unsigned int logMask = -1;
    static const char _logIds[] = " lids=";
    cp = strstr(buffer, _logIds);
    if (cp) {
        logMask = 0;
        cp += sizeof(_logIds) - 1;
        while (*cp && *cp != '\0') {
            int val = 0;
            while (isdigit(*cp)) {
                val = val * 10 + *cp - '0';
                ++cp;
            }
            logMask |= 1 << val;
            if (*cp != ',') {
                break;
            }
            ++cp;
        }
    }

    pid_t pid = 0;
    static const char _pid[] = " pid=";
    cp = strstr(buffer, _pid);
    if (cp) {
        pid = atol(cp + sizeof(_pid) - 1);
    }

    bool nonBlock = false;
    if (strncmp(buffer, "dumpAndClose", 12) == 0) {
        // Allow writer to get some cycles, and wait for pending notifications
        sched_yield();
        LogTimeEntry::lock();
        LogTimeEntry::unlock();
        sched_yield();
        nonBlock = true;
    }

    // Convert realtime to monotonic time
    if (start == log_time::EPOCH) {
        start = LogTimeEntry::EPOCH;
    } else {
        class LogFindStart {
            const pid_t mPid;
            const unsigned mLogMask;
            bool startTimeSet;
            log_time &start;
            log_time last;

        public:
            LogFindStart(unsigned logMask, pid_t pid, log_time &start)
                    : mPid(pid)
                    , mLogMask(logMask)
                    , startTimeSet(false)
                    , start(start)
                    , last(LogTimeEntry::EPOCH)
            { }

            static bool callback(const LogBufferElement *element, void *obj) {
                LogFindStart *me = reinterpret_cast<LogFindStart *>(obj);
                if (!me->startTimeSet
                        && (!me->mPid || (me->mPid == element->getPid()))
                        && (me->mLogMask & (1 << element->getLogId()))) {
                    if (me->start == element->getRealTime()) {
                        me->start = element->getMonotonicTime();
                        me->startTimeSet = true;
                    } else {
                        if (me->start < element->getRealTime()) {
                            me->start = me->last;
                            me->startTimeSet = true;
                        }
                        me->last = element->getMonotonicTime();
                    }
                }
                return false;
            }

            bool found() { return startTimeSet; }
        } logFindStart(logMask, pid, start);

        logbuf().flushTo(cli, LogTimeEntry::EPOCH,
                         FlushCommand::hasReadLogs(cli),
                         logFindStart.callback, &logFindStart);

        if (!logFindStart.found()) {
            if (nonBlock) {
                doSocketDelete(cli);
                return false;
            }
            log_time now(CLOCK_MONOTONIC);
            start = now;
        }
    }

    FlushCommand command(*this, nonBlock, tail, logMask, pid, start);
    command.runSocketCommand(cli);
    return true;
}

void LogReader::doSocketDelete(SocketClient *cli) {
    LastLogTimes &times = mLogbuf.mTimes;
    LogTimeEntry::lock();
    LastLogTimes::iterator it = times.begin();
    while(it != times.end()) {
        LogTimeEntry *entry = (*it);
        if (entry->mClient == cli) {
            times.erase(it);
            entry->release_Locked();
            break;
        }
        it++;
    }
    LogTimeEntry::unlock();
}

int LogReader::getLogSocket() {
    static const char socketName[] = "logdr";
    int sock = android_get_control_socket(socketName);

    if (sock < 0) {
        sock = socket_local_server(socketName,
                                   ANDROID_SOCKET_NAMESPACE_RESERVED,
                                   SOCK_SEQPACKET);
    }

    return sock;
}
