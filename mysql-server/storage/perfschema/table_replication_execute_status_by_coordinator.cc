/*
      Copyright (c) 2013, 2014, Oracle and/or its affiliates. All rights reserved.

      This program is free software; you can redistribute it and/or modify
      it under the terms of the GNU General Public License as published by
      the Free Software Foundation; version 2 of the License.

      This program is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
      GNU General Public License for more details.

      You should have received a copy of the GNU General Public License
      along with this program; if not, write to the Free Software
      Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file storage/perfschema/table_replication_execute_status_by_cordinator.cc
  Table replication_execute_status_by_coordinator (implementation).
*/

#define HAVE_REPLICATION

#include "my_global.h"
#include "sql_priv.h"
#include "table_replication_execute_status_by_coordinator.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "rpl_slave.h"
#include "rpl_info.h"
#include  "rpl_rli.h"
#include "rpl_mi.h"
#include "sql_parse.h"

THR_LOCK table_replication_execute_status_by_coordinator::m_table_lock;

/*
  numbers in varchar count utf8 characters.
*/
static const TABLE_FIELD_TYPE field_types[]=
{
  {
    {C_STRING_WITH_LEN("THREAD_ID")},
    {C_STRING_WITH_LEN("bigint")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("SERVICE_STATE")},
    {C_STRING_WITH_LEN("enum('ON','OFF')")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("LAST_ERROR_NUMBER")},
    {C_STRING_WITH_LEN("int(11)")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("LAST_ERROR_MESSAGE")},
    {C_STRING_WITH_LEN("varchar(1024)")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("LAST_ERROR_TIMESTAMP")},
    { C_STRING_WITH_LEN("timestamp") },
    { NULL, 0}
  },
};

TABLE_FIELD_DEF
table_replication_execute_status_by_coordinator::m_field_def=
{ 5, field_types };

PFS_engine_table_share
table_replication_execute_status_by_coordinator::m_share=
{
  { C_STRING_WITH_LEN("replication_execute_status_by_coordinator") },
  &pfs_readonly_acl,
  table_replication_execute_status_by_coordinator::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_replication_execute_status_by_coordinator::get_row_count,
  sizeof(PFS_simple_index), /* ref length */
  &m_table_lock,
  &m_field_def,
  false /* checked */
};

PFS_engine_table* table_replication_execute_status_by_coordinator::create(void)
{
  return new table_replication_execute_status_by_coordinator();
}

table_replication_execute_status_by_coordinator
  ::table_replication_execute_status_by_coordinator()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(0), m_next_pos(0)
{}

table_replication_execute_status_by_coordinator
  ::~table_replication_execute_status_by_coordinator()
{}

void table_replication_execute_status_by_coordinator::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

ha_rows table_replication_execute_status_by_coordinator::get_row_count()
{
  uint row_count= 0;

  mysql_mutex_lock(&LOCK_active_mi);

  if (active_mi && active_mi->host[0])
    row_count= 1;

  mysql_mutex_unlock(&LOCK_active_mi);

  return row_count;
}

int table_replication_execute_status_by_coordinator::rnd_next(void)
{
  if(get_row_count() == 0)
    return HA_ERR_END_OF_FILE;

  m_pos.set_at(&m_next_pos);

  if (m_pos.m_index == 0)
  {
    make_row();
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_replication_execute_status_by_coordinator::rnd_pos(const void *pos)
{
  if(get_row_count() == 0)
    return HA_ERR_END_OF_FILE;

  set_position(pos);

  DBUG_ASSERT(m_pos.m_index < 1);

  make_row();

  return 0;
}

void table_replication_execute_status_by_coordinator::make_row()
{
  m_row_exists= false;

  mysql_mutex_lock(&LOCK_active_mi);

  DBUG_ASSERT(active_mi != NULL);
  DBUG_ASSERT(active_mi->rli != NULL);

  mysql_mutex_lock(&active_mi->rli->data_lock);

  if (active_mi->rli->slave_running)
  {
    PSI_thread *psi= thd_get_psi(active_mi->rli->info_thd);
    PFS_thread *pfs= reinterpret_cast<PFS_thread *> (psi);
    if(pfs)
    {
      m_row.thread_id= pfs->m_thread_internal_id;
      m_row.thread_id_is_null= false;
    }
    else
      m_row.thread_id_is_null= true;
  }
  else
    m_row.thread_id_is_null= true;

  if (active_mi->rli->slave_running)
    m_row.service_state= PS_RPL_YES;
  else
    m_row.service_state= PS_RPL_NO;

  mysql_mutex_lock(&active_mi->rli->err_lock);

  m_row.last_error_number= (long int) active_mi->rli->last_error().number;
  m_row.last_error_message_length= 0;
  m_row.last_error_timestamp= 0;

  /** if error, set error message and timestamp */
  if (m_row.last_error_number)
  {
    char *temp_store= (char*) active_mi->rli->last_error().message;
    m_row.last_error_message_length= strlen(temp_store);
    memcpy(m_row.last_error_message, temp_store,
           m_row.last_error_message_length);

    /** time in millisecond since epoch */
    m_row.last_error_timestamp= (ulonglong)active_mi->rli->last_error().skr*1000000;
  }

  mysql_mutex_unlock(&active_mi->rli->err_lock);
  mysql_mutex_unlock(&active_mi->rli->data_lock);
  mysql_mutex_unlock(&LOCK_active_mi);

  m_row_exists= true;
}

int table_replication_execute_status_by_coordinator
  ::read_row_values(TABLE *table, unsigned char *buf,
                    Field **fields, bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  DBUG_ASSERT(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /*thread_id*/
        if (!m_row.thread_id_is_null)
          set_field_ulonglong(f, m_row.thread_id);
        else
          f->set_null();
        break;
      case 1: /*service_state*/
        set_field_enum(f, m_row.service_state);
        break;
      case 2: /*last_error_number*/
        set_field_ulong(f, m_row.last_error_number);
        break;
      case 3: /*last_error_message*/
        set_field_varchar_utf8(f, m_row.last_error_message,
                               m_row.last_error_message_length);
        break;
      case 4: /*last_error_timestamp*/
        set_field_timestamp(f, m_row.last_error_timestamp);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }
  return 0;
}
