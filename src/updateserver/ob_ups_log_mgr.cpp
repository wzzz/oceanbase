/**
 * (C) 2007-2010 Taobao Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Version: $Id$
 *
 * Authors:
 *   yanran <yanran.hfs@taobao.com>
 *     - some work details if you want
 */

#include "ob_ups_log_mgr.h"

#include "common/file_utils.h"
#include "common/file_directory_utils.h"
#include "common/ob_log_dir_scanner.h"
#include "common/ob_log_reader.h"
#include "common/ob_direct_log_reader.h"
#include "common/utility.h"
#include "common/ob_delay_guard.h"
#include "ob_update_server_main.h"

using namespace oceanbase::common;
using namespace oceanbase::updateserver;

namespace oceanbase
{
  namespace updateserver
  {
    // 获得sstable指示的最大日志文件ID
    uint64_t get_max_file_id_by_sst()
    {
      uint64_t file_id = OB_INVALID_ID;
      ObUpdateServerMain *ups = ObUpdateServerMain::get_instance();
      if (NULL == ups)
      {
        file_id = OB_INVALID_ID;
      }
      else
      {
        file_id = ups->get_update_server().get_sstable_mgr().get_max_clog_id();
      }
      return file_id;
    }

    // 备UPS向主机询问日志的起始点时，主机应该返回上一次major frozen的点
    int64_t get_last_major_frozen_log_file_id(const char* log_dir)
    {
      int err = OB_SUCCESS;
      uint64_t file_id = 0;
      ObUpdateServerMain *ups = ObUpdateServerMain::get_instance();
      ObLogDirScanner scanner;
      if (NULL != ups)
      {
        file_id = ups->get_update_server().get_sstable_mgr().get_last_major_frozen_clog_file_id();
      }
      if (0 < file_id)
      {}
      else if (OB_SUCCESS != (err = scanner.init(log_dir))
               && OB_DISCONTINUOUS_LOG != err)
      {
        TBSYS_LOG(ERROR, "scanner.init(log_dir=%s)=>%d", log_dir, err);
      }
      else if (OB_SUCCESS != (err = scanner.get_min_log_id((uint64_t&)file_id)) && OB_ENTRY_NOT_EXIST != err)
      {
        TBSYS_LOG(ERROR, "scanner.get_min_log_file_id()=>%d", err);
      }
      return file_id;
    }

    int parse_log_buffer(const char* log_data, int64_t data_len, const ObLogCursor& start_cursor, ObLogCursor& end_cursor)
    {
      int err = OB_SUCCESS;
      int64_t pos = 0;
      int64_t tmp_pos = 0;
      int64_t file_id = 0;
      ObLogEntry log_entry;
      end_cursor = start_cursor;
      if (NULL == log_data || data_len <= 0 || !start_cursor.is_valid())
      {
        err = OB_INVALID_ARGUMENT;
        TBSYS_LOG(ERROR, "invalid argument, log_data=%p, data_len=%ld, start_cursor=%s",
                  log_data, data_len, start_cursor.to_str());
      }

      while (OB_SUCCESS == err && pos < data_len)
      {
        if (OB_SUCCESS != (err = log_entry.deserialize(log_data, data_len, pos)))
        {
          TBSYS_LOG(ERROR, "log_entry.deserialize(log_data=%p, data_len=%ld, pos=%ld)=>%d", log_data, data_len, pos, err);
        }
        else if (OB_SUCCESS != (err = log_entry.check_data_integrity(log_data + pos)))
        {
          TBSYS_LOG(ERROR, "log_entry.check_data_integrity()=>%d", err);
        }
        else
        {
          tmp_pos = pos;
        }

        if (OB_SUCCESS != err)
        {}
        else if (OB_LOG_SWITCH_LOG == log_entry.cmd_
                 && !(OB_SUCCESS == (err = serialization::decode_i64(log_data, data_len, tmp_pos, (int64_t*)&file_id)
                                     && start_cursor.file_id_ + 1 == file_id)))
        {
          TBSYS_LOG(ERROR, "decode switch_log failed(log_data=%p, data_len=%ld, pos=%ld)=>%d", log_data, data_len, tmp_pos, err);
        }
        else
        {
          pos += log_entry.get_log_data_len();
          if (OB_SUCCESS != (err = end_cursor.advance(log_entry)))
          {
            TBSYS_LOG(ERROR, "end_cursor[%ld].advance(%ld)=>%d", end_cursor.log_id_, log_entry.seq_, err);
          }
        }
      }
      if (OB_SUCCESS == err && pos != data_len)
      {
        err = OB_ERR_UNEXPECTED;
        TBSYS_LOG(ERROR, "pos[%ld] != data_len[%ld]", pos, data_len);
      }

      if (OB_SUCCESS != err)
      {
        //hex_dump(log_data, static_cast<int32_t>(data_len), TBSYS_LOG_LEVEL_WARN);
      }
      return err;
    }
  }; // end namespace updateserver
}; // end namespace oceanbase


int64_t ObUpsLogMgr::FileIdBeforeLastMajorFrozen::get()
{
  uint64_t file_id = 0;
  ObUpdateServerMain *ups = ObUpdateServerMain::get_instance();
  if (NULL != ups)
  {
    file_id = ups->get_update_server().get_sstable_mgr().get_last_major_frozen_clog_file_id();
  }
  return (0 < file_id) ? (file_id - 1) : file_id;
}

ObUpsLogMgr::ObUpsLogMgr(): log_buffer_for_fetch_(LOG_BUFFER_SIZE), log_buffer_for_replay_(LOG_BUFFER_SIZE)
{
  table_mgr_ = NULL;
  role_mgr_ = NULL;
  stop_ = false;
  last_receive_log_time_ = 0;
  master_getter_ = NULL;
  master_log_id_ = 0;
  max_log_id_ = 0;
  local_max_log_id_when_start_ = -1;
  is_initialized_ = false;
  log_dir_[0] = '\0';
  is_started_ = false;
}

ObUpsLogMgr::~ObUpsLogMgr()
{
}

bool ObUpsLogMgr::is_inited() const
{
  return is_initialized_;
}

int ObUpsLogMgr::init(const char* log_dir, const int64_t log_file_max_size,
                      ObLogReplayWorker* replay_worker, ObReplayLogSrc* replay_log_src, ObUpsTableMgr* table_mgr,
                      ObUpsSlaveMgr *slave_mgr, ObiRole* obi_role, ObUpsRoleMgr *role_mgr, int64_t log_sync_type)
{
  int ret = OB_SUCCESS;
  int64_t len = 0;

  if (is_initialized_)
  {
    TBSYS_LOG(ERROR, "ObUpsLogMgr has been initialized");
    ret = OB_INIT_TWICE;
  }
  else if (NULL == log_dir || NULL == replay_log_src || NULL == table_mgr
           || NULL == slave_mgr || NULL == obi_role || NULL == role_mgr)
  {
    TBSYS_LOG(ERROR, "Invalid argument[log_dir=%p replay_log_src=%p, table_mgr=%p]"
              "[slave_mgr=%p obi_role=%p, role_mgr=%p]",
              log_dir, replay_log_src, table_mgr, slave_mgr, obi_role, role_mgr);
    ret = OB_INVALID_ARGUMENT;
  }
  else if (0 >= (len = snprintf(log_dir_, sizeof(log_dir_), "%s", log_dir)) || len >= (int64_t)sizeof(log_dir_))
  {
    TBSYS_LOG(ERROR, "Argument is invalid[log_dir_len=%ld log_dir=%s]", len, log_dir);
    ret = OB_INVALID_ARGUMENT;
  }
  else if (OB_SUCCESS != (ret = recent_log_cache_.init()))
  {
    TBSYS_LOG(ERROR, "recent_log_cache.init()=>%d", ret);
  }
  else if (OB_SUCCESS != (ret = pos_log_reader_.init(log_dir)))
  {
    TBSYS_LOG(ERROR, "pos_log_reader.init(log_dir=%s)=>%d", log_dir, ret);
  }
  else if (OB_SUCCESS != (ret = cached_pos_log_reader_.init(&pos_log_reader_, &recent_log_cache_)))
  {
    TBSYS_LOG(ERROR, "cached_pos_log_reader_.init(pos_log_reader=%p, log_buf=%p)=>%d",
              &pos_log_reader_, &recent_log_cache_, ret);
  }
  else if (OB_SUCCESS != (ret = replay_local_log_task_.init(this)))
  {
    TBSYS_LOG(ERROR, "reaplay_local_log_task.init()=>%d", ret);
  }
  else if (OB_SUCCESS != (ret = replay_point_.init(log_dir_)))
  {
    TBSYS_LOG(ERROR, "replay_point_file.init(log_dir=%s)=>%d", log_dir_, ret);
  }

  if (OB_SUCCESS == ret)
  {
    ObSlaveMgr *slave = reinterpret_cast<ObSlaveMgr*>(slave_mgr);
    ret = ObLogWriter::init(log_dir_, log_file_max_size, slave, log_sync_type, &last_fid_before_frozen_);
    if (OB_SUCCESS != ret)
    {
      TBSYS_LOG(ERROR, "ObLogWriter init failed[ret=%d]", ret);
    }
    else
    {
      table_mgr_ = table_mgr;
      replay_worker_ = replay_worker;
      replay_log_src_ = replay_log_src;
      obi_role_ = obi_role;
      role_mgr_ = role_mgr;
      is_initialized_ = true;
      replay_worker_->start();
      TBSYS_LOG(INFO, "ObUpsLogMgr[this=%p] init succ", this);
    }
  }

  return ret;
}

int64_t ObUpsLogMgr::to_string(char* buf, const int64_t len) const
{
  int64_t pos = 0;
  databuff_printf(buf, len, pos, "LogMgr(master_id=%ld, worker=%s, rlog_cache=%s)", master_log_id_, to_cstring(*replay_worker_), to_cstring(recent_log_cache_));
  return pos;
}

int ObUpsLogMgr::write_replay_point(uint64_t replay_point)
{
  int ret = 0;

  if (OB_SUCCESS != (ret = check_inner_stat()))
  {
    TBSYS_LOG(ERROR, "check_inner_stat()=>%d", ret);
  }
  else if (OB_SUCCESS != (ret = replay_point_.write(replay_point)))
  {
    TBSYS_LOG(ERROR, "replay_point_file.write(%lu)=>%d", replay_point, ret);
  }
  return ret;
}

int ObUpsLogMgr::add_slave(const ObServer& server, uint64_t &new_log_file_id, const bool switch_log)
{
  int ret = OB_SUCCESS;

  ObSlaveMgr* slave_mgr = get_slave_mgr();
  if (NULL == slave_mgr)
  {
    TBSYS_LOG(ERROR, "slave_mgr is NULL");
    ret = OB_ERROR;
  }
  else
  {
    TBSYS_LOG(INFO, "log mgr add slave: %s", to_cstring(server));
    ret = slave_mgr->add_server(server);
    if (OB_SUCCESS != ret)
    {
      TBSYS_LOG(ERROR, "ObSlaveMgreadd_server error[ret=%d]", ret);
    }
    else
    {
      if (true) // 现在的日志同步方案不用切日志了
      {}
      else
      if (switch_log)
      {
        ret = switch_log_file(new_log_file_id);
        if (OB_SUCCESS != ret)
        {
          TBSYS_LOG(ERROR, "switch_log_file error[ret=%d]", ret);
        }
        else
        {
          TBSYS_LOG(INFO, "log mgr switch log file succ");
        }
      }
      else
      {
        //take new_log_file_id as slave_slave_ups's send_log_point
        ret = slave_mgr->set_send_log_point(server, new_log_file_id);
        if (OB_SUCCESS != ret)
        {
          TBSYS_LOG(ERROR, "fail to set_send_log_point to slave. slave=%s", server.to_cstring());
        }
        else
        {
          TBSYS_LOG(INFO, "slave [%s] send log point [%ld]", server.to_cstring(), new_log_file_id);
        }
      }
    }
  }

  return ret;
}

int ObUpsLogMgr::add_log_replay_event(const int64_t seq, const int64_t mutation_time)
{
  return delay_stat_.add_log_replay_event(seq, mutation_time, master_log_id_);
}

// 一定从一个文件的开始重放，只能调用一次，
// 可能没有日志或日志不连续，这时重放本地日志实际上什么都不用做，
// 重放完之后replayed_cursor_可能依然无效
int ObUpsLogMgr::replay_local_log()
{
  int err = OB_SUCCESS;
  ObLogCursor end_cursor;
  uint64_t log_file_id_by_sst = get_max_file_id_by_sst();
  if (!is_inited())
  {
    err = OB_NOT_INIT;
  }
  else if (replay_worker_->get_replayed_log_id() > 0)
  {
    TBSYS_LOG(WARN, "local log is already replayed: cur_cursor=%ld", replay_worker_->get_replayed_log_id());
    err = OB_ALREADY_DONE; // 已经重放过了
  }
  else if (OB_INVALID_ID != log_file_id_by_sst)
  {
    start_cursor_.file_id_ = (int64_t)log_file_id_by_sst;
  }
  else if (OB_SUCCESS != (err = replay_point_.get(start_cursor_.file_id_)))
  {
    TBSYS_LOG(ERROR, "get_replay_point_func(log_dir=%s)=>%d", log_dir_, err);
  }
  TBSYS_LOG(INFO, "get_replay_point(file_id=%ld)", start_cursor_.file_id_);

  // 可能会有单个空文件存在
  if (OB_SUCCESS != err || start_cursor_.file_id_ <= 0) 
  {}
  else if (OB_SUCCESS != (err = replay_local_log_func(stop_, log_dir_, start_cursor_, end_cursor, *replay_worker_))
           && OB_ENTRY_NOT_EXIST != err)
  {
    TBSYS_LOG(ERROR, "replay_log_func(log_dir=%s, start_cursor=%s)=>%d", log_dir_, start_cursor_.to_str(), err);
  }
  else if (end_cursor.log_id_ <= 0
           && OB_SUCCESS != (err = get_local_max_log_cursor_func(log_dir_, get_max_file_id_by_sst(), end_cursor)))
  {
    TBSYS_LOG(ERROR, "get_local_max_log_cursor(log_dir=%s)=>%d", log_dir_, err);
  }
  else if (end_cursor.log_id_ <= 0)
  {
    TBSYS_LOG(WARN, "replayed_cursor.log_id[%ld] <= 0 after replay local log", end_cursor.log_id_);
  }
  else if (OB_SUCCESS != (err = start_log(end_cursor)))
  {
    TBSYS_LOG(ERROR, "start_log(cursor=%s)=>%d", end_cursor.to_str(), err);
  }
  else
  {
    TBSYS_LOG(INFO, "start_log_after_replay_local_log(replay_cursor=%s): OK.", end_cursor.to_str());
  }

  // 在UPS主循环中调用start_log_for_master_write()并设置状态为ACTIVE
  // if (OB_SUCCESS != err || !is_master_master())
  // {}
  // else if (OB_SUCCESS != (err = start_log_for_master_write()))
  // {
  //   TBSYS_LOG(ERROR, "start_log_for_master_write()=>%d", err);
  // }
  // else
  // {
  //   set_state_as_active();
  // }
  if (OB_ALREADY_DONE == err)
  {
    err = OB_SUCCESS;
  }
  if (OB_SUCCESS != err)
  {
    role_mgr_->set_state(ObUpsRoleMgr::FATAL);
  }
  return err;
}

// 备切换为主时需要调用一次
int ObUpsLogMgr::start_log_for_master_write()
{
  int err = OB_SUCCESS;
  const bool allow_missing_log_file = true;
  ObLogCursor start_cursor;
  set_cursor(start_cursor, (start_cursor_.file_id_ > 0)? start_cursor_.file_id_: 1, 1, 0);
  if (start_cursor_.log_id_ > 0)
  {
    TBSYS_LOG(INFO, "start_log_for_master(replay_cursor=%s): ALREADY STARTED", start_cursor_.to_str());
  }
  else if (!allow_missing_log_file && start_cursor_.file_id_ > 0)
  {
    err = OB_LOG_MISSING;
    TBSYS_LOG(ERROR, "missing log_file[file_id=%ld]", start_cursor_.file_id_);
  }
  else if (OB_SUCCESS != (err = start_log(start_cursor)))
  {
    TBSYS_LOG(ERROR, "start_log()=>%d", err);
  }
  else
  {
    TBSYS_LOG(INFO, "start_log_for_master(replay_cursor=%s): STARTE OK.", start_cursor_.to_str());
  }
  return err;
}

bool ObUpsLogMgr::is_slave_master() const
{
  return NULL != obi_role_ && NULL != role_mgr_
    && ObiRole::SLAVE == obi_role_->get_role() &&  ObUpsRoleMgr::MASTER == role_mgr_->get_role();
}

bool ObUpsLogMgr::is_master_master() const
{
  return NULL != obi_role_ && NULL != role_mgr_
    && ObiRole::MASTER == obi_role_->get_role() &&  ObUpsRoleMgr::MASTER == role_mgr_->get_role();
}

int ObUpsLogMgr::get_replayed_cursor(ObLogCursor& cursor) const
{
  return replay_worker_->get_replay_cursor(cursor);
}

ObLogCursor& ObUpsLogMgr::get_replayed_cursor_(ObLogCursor& cursor) const
{
  replay_worker_->get_replay_cursor(cursor);
  return cursor;
}

int ObUpsLogMgr::set_state_as_active()
{
  int err = OB_SUCCESS;
  ObLogCursor replay_cursor;
  if (NULL == role_mgr_)
  {
    err = OB_NOT_INIT;
  }
  else if (ObUpsRoleMgr::ACTIVE != role_mgr_->get_state())
  {
    TBSYS_LOG(INFO, "set ups state to be ACTIVE. master_log_id=%ld, log_cursor=%s",
              master_log_id_, to_cstring(get_replayed_cursor_(replay_cursor)));
    role_mgr_->set_state(ObUpsRoleMgr::ACTIVE);
  }
  return err;
}

int ObUpsLogMgr::write_log_as_slave(const char* buf, const int64_t len)
{
  int err = OB_SUCCESS;
  bool need_send_log = is_slave_master();
  if (!is_inited())
  {
    err = OB_NOT_INIT;
  }
  else if (OB_SUCCESS != (err = store_log(buf, len, need_send_log)))
  {
    TBSYS_LOG(ERROR, "store_log(%p[%ld], sync_to_slave=%s)=>%d", buf, len, STR_BOOL(need_send_log), err);
  }
  return err;
}

int ObUpsLogMgr::replay_and_write_log(const int64_t start_id, const int64_t end_id, const char* buf, int64_t len)
{
  int err = OB_SUCCESS;
  ObLogCursor start_cursor;
  ObLogCursor end_cursor;
  int64_t real_end_id = 0;
  UNUSED(start_id);
  UNUSED(end_id);
  if (!is_inited())
  {
    err = OB_NOT_INIT;
  }
  else if (NULL == buf || 0 > len)
  {
    err = OB_INVALID_ARGUMENT;
    TBSYS_LOG(ERROR, "replay_and_write_log(buf=%p[%ld])", buf, len);
  }
  else if (0 == len)
  {}
  else if (len & (OB_DIRECT_IO_ALIGN-1))
  {
    err = OB_LOG_NOT_ALIGN;
    TBSYS_LOG(ERROR, "len[%ld] is not aligned, start_id=%ld", len, start_id);
  }
  else if (OB_SUCCESS != (err = replay_worker_->get_replay_cursor(start_cursor)))
  {
    TBSYS_LOG(ERROR, "get_replay_cursor()=>%d", err);
  }
  else if (OB_SUCCESS != (err = parse_log_buffer(buf, len, start_cursor, end_cursor)))
  {
    TBSYS_LOG(ERROR, "parse_log_buffer(log_data=%p, data_len=%ld, log_cursor=%s)=>%d",
              buf, len, start_cursor.to_str(), err);
  }
  else if (OB_SUCCESS != (err = replay_worker_->submit_batch(real_end_id, buf, len, RT_APPLY)))
  {
    TBSYS_LOG(ERROR, "replay_worker.submit_batch(buf=%p[%ld])=>%d", buf, len, err);
  }
  return err;
}

int ObUpsLogMgr::set_master_log_id(const int64_t master_log_id)
{
  int err = OB_SUCCESS;
  if (master_log_id < master_log_id_)
  {
    TBSYS_LOG(WARN, "master_log_id[%ld] < master_log_id_[%ld]", master_log_id, master_log_id_);
  }
  set_counter(master_log_id_cond_, master_log_id_, master_log_id);
  return err;
}

int ObUpsLogMgr::slave_receive_log(const char* buf, int64_t len, const int64_t wait_sync_time_us,
                                   const WAIT_SYNC_TYPE wait_event_type)
{
  int err = OB_SUCCESS;
  int64_t start_id = 0;
  int64_t end_id = 0;
  if (!is_inited())
  {
    err = OB_NOT_INIT;
  }
  else if (NULL == buf || 0 > len)
  {
    err = OB_INVALID_ARGUMENT;
    TBSYS_LOG(ERROR, "slave_receive_log(buf=%p[%ld]): invalid argument.", buf, len);
  }
  else if (OB_SUCCESS != (err = parse_log_buffer(buf, len, start_id, end_id)))
  {
    TBSYS_LOG(ERROR, "parse_log_buffer(log_data=%p[%ld])=>%d", buf, len, err);
  }
  else if (OB_SUCCESS != (err = append_to_log_buffer(&recent_log_cache_, start_id, end_id, buf, len)))
  {
    TBSYS_LOG(ERROR, "append_to_log_buffer(log_id=[%ld,%ld))=>%d", start_id, end_id, err);
  }
  else
  {
    int64_t next_flush_log_id = 0;
    int64_t next_commit_log_id = 0;
    set_master_log_id(end_id);
    last_receive_log_time_ = tbsys::CTimeUtil::getTime();
    if (wait_sync_time_us <= 0 || WAIT_NONE == wait_event_type)
    {}
    else if (ObUpsRoleMgr::ACTIVE != role_mgr_->get_state())
    {
      TBSYS_LOG(WARN, "wait_slave_sync(log=[%ld,%ld)) state not ACTIVE", start_id, end_id);
    }
    else if (WAIT_COMMIT == wait_event_type)
    {
      if (end_id > (next_commit_log_id = replay_worker_->wait_next_commit_log_id(end_id, wait_sync_time_us)))
      {
        TBSYS_LOG(WARN, "wait_flush_log_id(end_id=%ld, next_flush_log_id=%ld, timeout=%ld) Fail.",
                  end_id, next_commit_log_id, wait_sync_time_us);
      }
    }
    else if (WAIT_FLUSH == wait_event_type)
    {
      if (end_id > (next_flush_log_id = replay_worker_->wait_next_flush_log_id(end_id, wait_sync_time_us)))
      {
        TBSYS_LOG(WARN, "wait_flush_log_id(end_id=%ld, next_flush_log_id=%ld, timeout=%ld) Fail.",
                  end_id, next_flush_log_id, wait_sync_time_us);
      }
    }
    else
    {
      TBSYS_LOG(WARN, "unknown wait_event_type=%d", wait_event_type);
    }
  }
  return err;
}

int ObUpsLogMgr::get_log_for_slave_fetch(ObFetchLogReq& req, ObFetchedLog& result)
{
  int err = OB_SUCCESS;
  ThreadSpecificBuffer::Buffer* my_buffer = log_buffer_for_fetch_.get_buffer();
  if (NULL == my_buffer)
  {
    err = OB_ALLOCATE_MEMORY_FAILED;
    TBSYS_LOG(ERROR, "get_thread_buffer fail.");
  }
  else
  {
    my_buffer->reset();
  }
  if (OB_SUCCESS != err)
  {}
  else if (OB_SUCCESS != (err = result.set_buf(my_buffer->current(), my_buffer->remain())))
  {
    TBSYS_LOG(ERROR, "result.set_buf(buf=%p[%d]): INVALID_ARGUMENT", my_buffer->current(), my_buffer->remain());
  }
  else if (!is_inited())
  {
    err = OB_NOT_INIT;
  }
  else if (0 >= req.start_id_ || req.start_id_ >= replay_worker_->get_replayed_log_id())
  {
    TBSYS_LOG(DEBUG, "get_log_for_slave_fetch data not server. req.start_id_=%ld, replayed_cursor_=%ld",
              req.start_id_, replay_worker_->get_replayed_log_id());
    err = OB_DATA_NOT_SERVE;
  }
  if (OB_SUCCESS == err && OB_SUCCESS != (err = cached_pos_log_reader_.get_log(req, result))
      && OB_DATA_NOT_SERVE != err)
  {
    TBSYS_LOG(ERROR, "cached_pos_log_reader.get_log(log_id=%ld)=>%d", req.start_id_, err);
  }
  if (OB_SUCCESS == err && result.data_len_ & (OB_DIRECT_IO_ALIGN-1))
  {
    err = OB_LOG_NOT_ALIGN;
    TBSYS_LOG(ERROR, "result.data_len[%ld] is not aligned.", result.data_len_);
  }
  TBSYS_LOG(DEBUG, "get_log_for_slave_fetch(start_id=%ld)=>%d", req.start_id_, err);
  return err;
}

int ObUpsLogMgr::fill_log_cursor(ObLogCursor& log_cursor)
{
  int err = OB_SUCCESS;
  if (log_cursor.log_id_ == 0 && log_cursor.file_id_ == 0)
  {
    log_cursor.file_id_ =  max(get_last_major_frozen_log_file_id(log_dir_), 1);
  }
  if (log_cursor.log_id_ == 0 && log_cursor.file_id_ > 0
      && OB_SUCCESS != (err = get_first_log_id_func(log_dir_, log_cursor.file_id_, log_cursor.log_id_))
      && OB_ENTRY_NOT_EXIST != err)
  {
    TBSYS_LOG(ERROR, "get_first_log_id_func()=>%d", err);
  }
  else if (OB_ENTRY_NOT_EXIST == err)
  {
    err = OB_SUCCESS;
  }
  TBSYS_LOG(TRACE, "fill_log_cursor[for slave](log_cursor=%s)=>%d", log_cursor.to_str(), err);
  return err;
}

int ObUpsLogMgr::start_log(const ObLogCursor& start_cursor)
{
  int err = OB_SUCCESS;
  if (OB_SUCCESS != (err = ObLogWriter::start_log(start_cursor)))
  {
    TBSYS_LOG(ERROR, "start_log(start_cursor=%s)=>%d", start_cursor.to_str(), err);
  }
  else if (OB_SUCCESS != (err = replay_worker_->start_log(start_cursor)))
  {
    TBSYS_LOG(ERROR, "replay_worker_.start_log(start_cursor=%s)=>%d", start_cursor.to_str(), err);
  }
  else
  {
    start_cursor_ = start_cursor;
  }
  return err;
}

int ObUpsLogMgr::start_log_for_replay()
{
  int err = OB_SUCCESS;
  if (start_cursor_.log_id_ > 0)
  {
    //TBSYS_LOG(INFO, "start_log_for_replay(replayed_cursor=%s): ALREADY STARTED.", replayed_cursor_.to_str() );
  }
  else if (OB_SUCCESS != (err = replay_log_src_->get_remote_log_src().fill_start_cursor(start_cursor_))
      && OB_NEED_RETRY != err)
  {
    TBSYS_LOG(ERROR, "fill_start_cursor(replayed_cursor=%s)=>%d", start_cursor_.to_str(), err);
  }
  else if (OB_SUCCESS == err && 0 >= start_cursor_.log_id_)
  {
    err = OB_NEED_RETRY;
  }
  else if (OB_NEED_RETRY == err)
  {}
  else if (OB_SUCCESS != (err = start_log(start_cursor_)))
  {
    TBSYS_LOG(ERROR, "start_log(start_cursor=%s)=>%d", start_cursor_.to_str(), err);
  }
  else
  {
    TBSYS_LOG(INFO, "start_log_for_replay(replayed_cursor=%s): STARTED OK.", start_cursor_.to_str());
  }
  return err;
}

bool ObUpsLogMgr::is_sync_with_master() const
{
  return 0 >= master_log_id_ || replay_worker_->get_replayed_log_id() >= master_log_id_;
}

static bool in_range(const int64_t x, const int64_t lower, const int64_t upper)
{
  return x >= lower && x < upper;
}

bool ObUpsLogMgr::has_nothing_in_buf_to_replay() const
{
  return !in_range(replay_worker_->get_replayed_log_id(), recent_log_cache_.get_start_id(), recent_log_cache_.get_end_id())
    && replay_worker_->is_all_task_finished();
}

bool ObUpsLogMgr::has_log_to_replay() const
{
  return replay_log_src_->get_remote_log_src().is_using_lsync() || master_log_id_ > replay_worker_->get_replayed_log_id();
}

int64_t ObUpsLogMgr::wait_new_log_to_replay(const int64_t timeout_us)
{
  return replay_log_src_->get_remote_log_src().is_using_lsync()?
    -1: wait_counter(master_log_id_cond_, master_log_id_, replay_worker_->get_replayed_log_id() + 1, timeout_us);
}
// 可能返回OB_NEED_RETRY;
int ObUpsLogMgr::replay_log()
{
  int err = OB_SUCCESS;
  int64_t end_id = 0;
  ObServer server;
  ObLogCursor replay_cursor;
  ThreadSpecificBuffer::Buffer* my_buffer = log_buffer_for_replay_.get_buffer();
  char* buf = NULL;
  int64_t len = 0;
  int64_t read_count = 0;

  if (NULL == my_buffer)
  {
    TBSYS_LOG(ERROR, "get thread specific buffer fail");
    err = OB_ALLOCATE_MEMORY_FAILED;
  }
  else
  {
    my_buffer->reset();
    buf = my_buffer->current();
    len = my_buffer->remain();
  }

  if (OB_SUCCESS != err)
  {
    // do nothing
  }
  else if (!is_inited())
  {
    err = OB_NOT_INIT;
  }
  else if (ObUpsRoleMgr::STOP == role_mgr_->get_state())
  {
    err = OB_CANCELED;
  }
  else if (ObUpsRoleMgr::FATAL == role_mgr_->get_state())
  {
    err = OB_NEED_RETRY;
  }
  else if (!is_log_replay_finished())
  {
    err = OB_NEED_RETRY;
  }
  else if (replay_worker_->get_replayed_log_id() < 0)
  {
    err = OB_ERR_UNEXPECTED;
  }
  else if (OB_SUCCESS != (err = start_log_for_replay())
           && OB_NEED_RETRY != err)
  {
    TBSYS_LOG(ERROR, "start_log_for_replay()=>%d", err);
  }
  else if (OB_NEED_RETRY == err)
  {
    err = OB_NEED_WAIT;
  }
  else if (!replay_log_src_->get_remote_log_src().is_using_lsync() &&  master_log_id_ <= replay_worker_->get_next_submit_log_id())
  {
    err = OB_NEED_RETRY;
  }
  else if (OB_SUCCESS != (err = replay_worker_->get_replay_cursor(replay_cursor)))
  {
    TBSYS_LOG(ERROR, "get_replay_cursor()=>%d", err);
  }
  else if (OB_SUCCESS != (err = replay_log_src_->get_log(replay_cursor, end_id, buf, len, read_count))
           && OB_DATA_NOT_SERVE != err)
  {
    TBSYS_LOG(ERROR, "get_log(replayed_cursor=%s)=>%d", replay_cursor.to_str(), err);
  }
  else if (OB_DATA_NOT_SERVE == err)
  {
    err = OB_NEED_WAIT;
  }
  else if (master_log_id_ > 0 && end_id >= master_log_id_ && OB_SUCCESS != (err = set_state_as_active()))
  {
    TBSYS_LOG(ERROR, "set_state_as_active()=>%d", err);
  }
  else if (0 == read_count)
  {
    err = OB_NEED_RETRY;
  }
  else if (OB_SUCCESS != (err = replay_and_write_log(replay_worker_->get_replayed_log_id(), end_id, buf, read_count)))
  {
    TBSYS_LOG(ERROR, "replay_and_write_log(buf=%p[%ld])=>%d", buf, read_count, err);
  }
  if (OB_SUCCESS != err && OB_NEED_RETRY != err && OB_NEED_WAIT != err && OB_CANCELED != err)
  {
    role_mgr_->set_state(ObUpsRoleMgr::FATAL);
    TBSYS_LOG(ERROR, "replay failed, ret=%d, will kill self", err);
    kill(getpid(), SIGTERM);
  }
  return err;
}

// sstable_mgr必须初始化完成
int  ObUpsLogMgr::get_max_log_seq_in_file(int64_t& log_seq) const
{
  int err = OB_SUCCESS;
  ObLogCursor log_cursor;
  log_seq = 0;
  if (!is_inited())
  {
    err = OB_NOT_INIT;
  }
  else if (is_log_replay_finished())
  {
    log_seq = replay_worker_->get_replayed_log_id();
  }
  else if (local_max_log_id_when_start_ >= 0)
  {
    log_seq = local_max_log_id_when_start_;
  }
  else if (OB_SUCCESS != (err = get_local_max_log_cursor_func(log_dir_, get_max_file_id_by_sst(), log_cursor)))
  {
    TBSYS_LOG(ERROR, "get_local_max_log_cursor(log_dir=%s)=>%d", log_dir_, err);
  }
  else
  {
    log_seq = log_cursor.log_id_;
    const_cast<int64_t&>(local_max_log_id_when_start_) = log_seq;
    if (log_cursor.log_id_ <= 0)
    {
      TBSYS_LOG(INFO, "local log_dir has no log or log is not continuous.");
    }
  }
  return err;
}

int ObUpsLogMgr::get_max_log_seq_in_buffer(int64_t& log_seq) const
{
  int err = OB_SUCCESS;
  log_seq = replay_worker_->get_replayed_log_id();
  if (!is_inited())
  {
    err = OB_NOT_INIT;
  }
  else if (!is_log_replay_finished())
  {}
  else if (log_seq > recent_log_cache_.get_start_id())
  {
    log_seq = max(log_seq, recent_log_cache_.get_end_id());
  }
  return err;
}

int ObUpsLogMgr::get_max_log_seq_replayable(int64_t& max_log_seq) const
{
  int err = OB_SUCCESS;
  int64_t max_log_seq_in_file = 0;
  int64_t max_log_seq_in_buffer = 0;
  if (!is_inited())
  {
    err = OB_NOT_INIT;
  }
  else if (OB_SUCCESS != (err = get_max_log_seq_in_file(max_log_seq_in_file)))
  {
    TBSYS_LOG(ERROR, "get_max_log_seq_in_file()=>%d", err);
  }
  else if (OB_SUCCESS != (err = get_max_log_seq_in_buffer(max_log_seq_in_buffer)))
  {
    TBSYS_LOG(ERROR, "get_max_log_seq_in_buffer()=>%d", err);
  }
  else
  {
    max_log_seq = max(max_log_seq_in_file, max_log_seq_in_buffer);
  }
  return err;
}

int ObUpsLogMgr::write_log_hook(const bool is_master,
                                const ObLogCursor start_cursor, const ObLogCursor end_cursor,
                                const char* log_data, const int64_t data_len)
{
  int err = OB_SUCCESS;
  int64_t start_id = start_cursor.log_id_;
  int64_t end_id = end_cursor.log_id_;
  int64_t log_size = (end_cursor.file_id_ - 1) * get_file_size() + end_cursor.offset_;
  clog_stat_.add_disk_us(start_id, end_id, get_last_disk_elapse());
  clog_stat_.add_net_us(start_id, end_id, get_last_net_elapse());
  OB_STAT_SET(UPDATESERVER, UPS_STAT_COMMIT_LOG_SIZE, log_size);
  OB_STAT_SET(UPDATESERVER, UPS_STAT_COMMIT_LOG_ID, end_cursor.log_id_);
  if (is_master)
  {
    if (OB_SUCCESS != (err = append_to_log_buffer(&recent_log_cache_, start_id, end_id, log_data, data_len)))
    {
      TBSYS_LOG(ERROR, "append_to_log_buffer([%s,%s], data=%p[%ld])=>%d",
                to_cstring(start_cursor), to_cstring(end_cursor), log_data, data_len, err);
    }
    else if (OB_SUCCESS != (err = replay_worker_->update_replay_cursor(end_cursor)))
    {
      TBSYS_LOG(ERROR, "update_replay_cursor(log_cursor=%s)=>%d", end_cursor.to_str(), err);
    }
    else
    {
      set_counter(master_log_id_cond_, master_log_id_, end_cursor.log_id_);
    }
  }
  return err;
}

ObLogBuffer& ObUpsLogMgr::get_log_buffer()
{
  return recent_log_cache_;
}
