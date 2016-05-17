// Copyright (c) 2016, Baidu.com, Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <common/string_util.h>
#include <common/logging.h>
#include <common/timer.h>
#include <gflags/gflags.h>

#include "nameserver/sync.h"
#include "rpc/rpc_client.h"

DECLARE_string(slave_node);
DECLARE_string(master_slave_role);

namespace baidu {
namespace bfs {

MasterSlaveImpl::MasterSlaveImpl() : exiting_(false), master_only_(false), cond_(&mu_),
                                     log_done_(&mu_), read_log_(-1), scan_log_(-1),
                                     current_offset_(0), sync_offset_(0) {
}

void MasterSlaveImpl::Init() {
    // recover sync_offset_
    int fp = open("prog.log", O_RDONLY);
    if (fp < 0 && errno != ENOENT) {
        LOG(FATAL, "[Sync] open prog.log failed reason: %s", strerror(errno));
    }
    if (fp >= 0) {
        char buf[4];
        uint32_t ret = read(fp, buf, 4);
        if (ret == 4) {
            memcpy(&sync_offset_, buf, 4);
            LOG(INFO, "[Sync] set sync_offset_ to %d", sync_offset_);
        }
        close(fp);
    }

    log_ = open("sync.log", O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if (log_ < 0) {
        LOG(FATAL, "[Sync] open sync log failed reason: %s", strerror(errno));
    }
    current_offset_ = lseek(log_, 0, SEEK_END);
    LOG(INFO, "[Sync] set current_offset_ to %d", current_offset_);
    assert(current_offset_ >= sync_offset_);

    read_log_ = open("sync.log", O_RDONLY);
    if (read_log_ < 0)  {
        LOG(FATAL, "[Sync] open sync log for read failed reason: %s", strerror(errno));
    }
    int offset = lseek(read_log_, sync_offset_, SEEK_SET);
    assert(offset == sync_offset_);

    rpc_client_ = new RpcClient();
    rpc_client_->GetStub(FLAGS_slave_node, &slave_stub_);
    if (IsLeader()) {
        worker_.Start(boost::bind(&MasterSlaveImpl::BackgroundLog, this));
        logger_.Start(boost::bind(&MasterSlaveImpl::LogProgress, this));
    }
}

bool MasterSlaveImpl::IsLeader(std::string* leader_addr) {
    return FLAGS_master_slave_role == "master";
}

bool MasterSlaveImpl::Log(const std::string& entry, int timeout_ms) {
    mu_.Lock();
    int len = LogLocal(entry);
    int last_offset = current_offset_;
    current_offset_ += len;
    cond_.Signal();
    mu_.Unlock();
    // slave is way behind, do no wait
    if (master_only_ && sync_offset_ < last_offset) {
        LOG(WARNING, "[Sync] Sync in maset-only mode, do not wait");
        return true;
    }

    int64_t start_point = common::timer::get_micros();
    int64_t stop_point = start_point + timeout_ms * 1000;
    while (sync_offset_ != current_offset_ && common::timer::get_micros() < stop_point) {
        MutexLock lock(&mu_);
        int wait_time = (stop_point - common::timer::get_micros()) / 1000;
        if (log_done_.TimeWait(wait_time)) {
            if (sync_offset_ != current_offset_) {
                continue;
            }
            if (master_only_) {
                LOG(INFO, "[Sync] leaves master-only mode");
                master_only_ = false;
            }
            LOG(INFO, "[Sync] sync log takes %ld ms", common::timer::get_micros() - start_point);
            return true;
        } else {
            break;
        }
    }
    // log replicate time out
    LOG(WARNING, "[Sync] Sync log timeout, Sync is in master-only mode");
    master_only_ = true;
    return true;
}

void MasterSlaveImpl::Log(const std::string& entry, boost::function<void (bool)> callback) {
    LOG(INFO, "[Sync] in async log");
    mu_.Lock();
    int len = LogLocal(entry);
    LOG(INFO, "[Sync] log entry len = %d", len);
    callbacks_.insert(std::make_pair(current_offset_, callback));
    LOG(INFO, "[Sync] insert callback current_offset_ = %d", current_offset_);
    current_offset_ += len;
    cond_.Signal();
    mu_.Unlock();
    return;
}

void MasterSlaveImpl::RegisterCallback(boost::function<void (const std::string& log)> callback) {
    log_callback_ = callback;
}

void MasterSlaveImpl::AppendLog(::google::protobuf::RpcController* controller,
                                const master_slave::AppendLogRequest* request,
                                master_slave::AppendLogResponse* response,
                                ::google::protobuf::Closure* done) {
    int len = request->log_data().size();
    LOG(INFO, "[Sync] receive log len=%d", len);
    char buf[4];
    memcpy(buf, &len, 4);
    int ret = write(log_, buf, 4);
    assert(ret == 4);
    ret = write(log_, request->log_data().c_str(), len);
    assert(ret == len);
    log_callback_(request->log_data());
    response->set_success(true);
    done->Run();
}

void MasterSlaveImpl::BackgroundLog() {
    while (true) {
        MutexLock lock(&mu_);
        while (!exiting_ && sync_offset_ == current_offset_) {
            LOG(INFO, "[Sync] BackgroundLog waiting...");
            cond_.Wait();
        }
        if (exiting_) {
            return;
        }
        LOG(INFO, "[Sync] BackgroundLog logging...");
        mu_.Unlock();
        ReplicateLog();
        mu_.Lock();
    }
}

void MasterSlaveImpl::ReplicateLog() {
    while (sync_offset_ < current_offset_) {
        mu_.Lock();
        if (sync_offset_ == current_offset_) {
            mu_.Unlock();
            break;
        }
        LOG(INFO, "[Sync] ReplicateLog sync_offset_ = %d, current_offset_ = %d",
                sync_offset_, current_offset_);
        mu_.Unlock();
        if (read_log_ < 0) {
            LOG(FATAL, "[Sync] read_log_ error");
        }
        char buf[4];
        uint32_t ret = read(read_log_, buf, 4);
        if (ret < 4) {
            LOG(WARNING, "[Sync] read failed read length = %u", ret);
            assert(0);
            return;
        }
        uint32_t len;
        memcpy(&len, buf, 4);
        LOG(INFO, "[Sync] record length = %u", len);
        char* entry = new char[len];
        ret = read(read_log_, entry, len);
        if (ret < len) {
            LOG(WARNING, "[Sync] incomplete record");
            return;
        }
        master_slave::AppendLogRequest request;
        master_slave::AppendLogResponse response;
        request.set_log_data(std::string(entry, len));
        while (!rpc_client_->SendRequest(slave_stub_, &master_slave::MasterSlave_Stub::AppendLog,
               &request, &response, 15, 1)) {
            LOG(WARNING, "[Sync] Replicate log failed sync_offset_ = %d, current_offset_ = %d",
                sync_offset_, current_offset_);
            sleep(5);
        }
        boost::function<void (bool)> callback;
        mu_.Lock();
        std::map<int, boost::function<void (bool)> >::iterator it = callbacks_.find(sync_offset_);
        if (it != callbacks_.end()) {
            callback = it->second;
            callbacks_.erase(it);
            LOG(INFO, "[Sync] calling callback %d", it->first);
            mu_.Unlock();
            callback(true);
            mu_.Lock();
        } else {
            LOG(INFO, "[Sync] can not find callback sync_offset_ = %d", sync_offset_);
            if (sync_offset_ != 0) {
                for (std::map<int, boost::function<void (bool)> >::iterator t = callbacks_.begin(); t != callbacks_.end(); ++t) {
                    LOG(INFO, "[Sync] callbacks_ %d", t->first);
                }
                assert(0);
            }
        }
        sync_offset_ += 4 + len;
        LOG(INFO, "[Sync] Replicate log done. sync_offset_ = %d, current_offset_ = %d",
                sync_offset_, current_offset_);
        mu_.Unlock();
        delete[] entry;
    }
    log_done_.Signal();
}

void MasterSlaveImpl::LogProgress() {
    while (!exiting_) {
        sleep(10);
        int fp = open("prog.tmp", O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
        if (fp < 0) {
            LOG(FATAL, "[Sync] open prog.tmp failed reason: %s", strerror(errno));
        }
        char buf[4];
        memcpy(buf, &sync_offset_, 4);
        int ret = write(fp, buf, 4);
        if (ret == 4) {
            rename("prog.tmp", "prog.log");
        }
        close(fp);
    }
}

int MasterSlaveImpl::LogLocal(const std::string& entry) {
    if (!IsLeader()) {
        LOG(FATAL, "[Sync] slave does not need to log");
    }
    int len = entry.length();
    char buf[4];
    memcpy(buf, &len, 4);
    write(log_, buf, 4);
    int w = write(log_, entry.c_str(), entry.length());
    assert(w >= 0);
    return w + 4;
}
} // namespace bfs
} // namespace baidu
