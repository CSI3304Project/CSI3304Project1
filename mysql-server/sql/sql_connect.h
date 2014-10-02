/* Copyright (c) 2006, 2013, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef SQL_CONNECT_INCLUDED
#define SQL_CONNECT_INCLUDED

#include "my_global.h"   // uint

class THD;
typedef struct st_lex_user LEX_USER;
typedef struct user_conn USER_CONN;
typedef struct user_resources USER_RESOURCES;

void init_max_user_conn(void);
void free_max_user_conn(void);
void reset_mqh(LEX_USER *lu, bool get_them);
bool check_mqh(THD *thd, uint check_command);
void decrease_user_connections(USER_CONN *uc);
void release_user_connection(THD *thd);
bool thd_init_client_charset(THD *thd, uint cs_number);
bool thd_prepare_connection(THD *thd);
void close_connection(THD *thd, uint sql_errno= 0);
bool thd_is_connection_alive(THD *thd);
void end_connection(THD *thd);
int get_or_create_user_conn(THD *thd, const char *user,
                            const char *host, const USER_RESOURCES *mqh);
int check_for_max_user_connections(THD *thd, const USER_CONN *uc);

#endif /* SQL_CONNECT_INCLUDED */
