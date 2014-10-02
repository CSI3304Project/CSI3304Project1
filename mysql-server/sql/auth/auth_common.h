#ifndef AUTH_COMMON_INCLUDED
#define AUTH_COMMON_INCLUDED

/* Copyright (c) 2000, 2014, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include "my_global.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_class.h"                          /* LEX_COLUMN */
#include "auth_acls.h"                          /* ACL information */

/* Forward Declarations */
class THD;

/* Classes */

enum ACL_internal_access_result
{
  /**
    Access granted for all the requested privileges,
    do not use the grant tables.
    This flag is used only for the INFORMATION_SCHEMA privileges,
    for compatibility reasons.
  */
  ACL_INTERNAL_ACCESS_GRANTED,
  /** Access denied, do not use the grant tables. */
  ACL_INTERNAL_ACCESS_DENIED,
  /** No decision yet, use the grant tables. */
  ACL_INTERNAL_ACCESS_CHECK_GRANT
};

/**
  Per internal table ACL access rules.
  This class is an interface.
  Per table(s) specific access rule should be implemented in a subclass.
  @sa ACL_internal_schema_access
*/
class ACL_internal_table_access
{
public:
  ACL_internal_table_access()
  {}

  virtual ~ACL_internal_table_access()
  {}

  /**
    Check access to an internal table.
    When a privilege is granted, this method add the requested privilege
    to save_priv.
    @param want_access the privileges requested
    @param [in, out] save_priv the privileges granted
    @return
      @retval ACL_INTERNAL_ACCESS_GRANTED All the requested privileges
      are granted, and saved in save_priv.
      @retval ACL_INTERNAL_ACCESS_DENIED At least one of the requested
      privileges was denied.
      @retval ACL_INTERNAL_ACCESS_CHECK_GRANT No requested privilege
      was denied, and grant should be checked for at least one
      privilege. Requested privileges that are granted, if any, are saved
      in save_priv.
  */
  virtual ACL_internal_access_result check(ulong want_access,
                                           ulong *save_priv) const= 0;
};

/**
  Per internal schema ACL access rules.
  This class is an interface.
  Each per schema specific access rule should be implemented
  in a different subclass, and registered.
  Per schema access rules can control:
  - every schema privileges on schema.*
  - every table privileges on schema.table
  @sa ACL_internal_schema_registry
*/
class ACL_internal_schema_access
{
public:
  ACL_internal_schema_access()
  {}

  virtual ~ACL_internal_schema_access()
  {}

  /**
    Check access to an internal schema.
    @param want_access the privileges requested
    @param [in, out] save_priv the privileges granted
    @return
      @retval ACL_INTERNAL_ACCESS_GRANTED All the requested privileges
      are granted, and saved in save_priv.
      @retval ACL_INTERNAL_ACCESS_DENIED At least one of the requested
      privileges was denied.
      @retval ACL_INTERNAL_ACCESS_CHECK_GRANT No requested privilege
      was denied, and grant should be checked for at least one
      privilege. Requested privileges that are granted, if any, are saved
      in save_priv.
  */
  virtual ACL_internal_access_result check(ulong want_access,
                                           ulong *save_priv) const= 0;

  /**
    Search for per table ACL access rules by table name.
    @param name the table name
    @return per table access rules, or NULL
  */
  virtual const ACL_internal_table_access *lookup(const char *name) const= 0;
};

/**
  A registry for per internal schema ACL.
  An 'internal schema' is a database schema maintained by the
  server implementation, such as 'performance_schema' and 'INFORMATION_SCHEMA'.
*/
class ACL_internal_schema_registry
{
public:
  static void register_schema(const LEX_STRING &name,
                              const ACL_internal_schema_access *access);
  static const ACL_internal_schema_access *lookup(const char *name);
};

/**
  Extension of ACL_internal_schema_access for Information Schema
*/
class IS_internal_schema_access : public ACL_internal_schema_access
{
public:
  IS_internal_schema_access()
  {}

  ~IS_internal_schema_access()
  {}

  ACL_internal_access_result check(ulong want_access,
                                   ulong *save_priv) const;

  const ACL_internal_table_access *lookup(const char *name) const;
};

/* Data Structures */

extern const char *command_array[];
extern uint        command_lengths[];

enum mysql_db_table_field
{
  MYSQL_DB_FIELD_HOST = 0,
  MYSQL_DB_FIELD_DB,
  MYSQL_DB_FIELD_USER,
  MYSQL_DB_FIELD_SELECT_PRIV,
  MYSQL_DB_FIELD_INSERT_PRIV,
  MYSQL_DB_FIELD_UPDATE_PRIV,
  MYSQL_DB_FIELD_DELETE_PRIV,
  MYSQL_DB_FIELD_CREATE_PRIV,
  MYSQL_DB_FIELD_DROP_PRIV,
  MYSQL_DB_FIELD_GRANT_PRIV,
  MYSQL_DB_FIELD_REFERENCES_PRIV,
  MYSQL_DB_FIELD_INDEX_PRIV,
  MYSQL_DB_FIELD_ALTER_PRIV,
  MYSQL_DB_FIELD_CREATE_TMP_TABLE_PRIV,
  MYSQL_DB_FIELD_LOCK_TABLES_PRIV,
  MYSQL_DB_FIELD_CREATE_VIEW_PRIV,
  MYSQL_DB_FIELD_SHOW_VIEW_PRIV,
  MYSQL_DB_FIELD_CREATE_ROUTINE_PRIV,
  MYSQL_DB_FIELD_ALTER_ROUTINE_PRIV,
  MYSQL_DB_FIELD_EXECUTE_PRIV,
  MYSQL_DB_FIELD_EVENT_PRIV,
  MYSQL_DB_FIELD_TRIGGER_PRIV,
  MYSQL_DB_FIELD_COUNT
};

enum mysql_user_table_field
{
  MYSQL_USER_FIELD_HOST= 0,
  MYSQL_USER_FIELD_USER,
  MYSQL_USER_FIELD_PASSWORD,
  MYSQL_USER_FIELD_SELECT_PRIV,
  MYSQL_USER_FIELD_INSERT_PRIV,
  MYSQL_USER_FIELD_UPDATE_PRIV,
  MYSQL_USER_FIELD_DELETE_PRIV,
  MYSQL_USER_FIELD_CREATE_PRIV,
  MYSQL_USER_FIELD_DROP_PRIV,
  MYSQL_USER_FIELD_RELOAD_PRIV,
  MYSQL_USER_FIELD_SHUTDOWN_PRIV,
  MYSQL_USER_FIELD_PROCESS_PRIV,
  MYSQL_USER_FIELD_FILE_PRIV,
  MYSQL_USER_FIELD_GRANT_PRIV,
  MYSQL_USER_FIELD_REFERENCES_PRIV,
  MYSQL_USER_FIELD_INDEX_PRIV,
  MYSQL_USER_FIELD_ALTER_PRIV,
  MYSQL_USER_FIELD_SHOW_DB_PRIV,
  MYSQL_USER_FIELD_SUPER_PRIV,
  MYSQL_USER_FIELD_CREATE_TMP_TABLE_PRIV,
  MYSQL_USER_FIELD_LOCK_TABLES_PRIV,
  MYSQL_USER_FIELD_EXECUTE_PRIV,
  MYSQL_USER_FIELD_REPL_SLAVE_PRIV,
  MYSQL_USER_FIELD_REPL_CLIENT_PRIV,
  MYSQL_USER_FIELD_CREATE_VIEW_PRIV,
  MYSQL_USER_FIELD_SHOW_VIEW_PRIV,
  MYSQL_USER_FIELD_CREATE_ROUTINE_PRIV,
  MYSQL_USER_FIELD_ALTER_ROUTINE_PRIV,
  MYSQL_USER_FIELD_CREATE_USER_PRIV,
  MYSQL_USER_FIELD_EVENT_PRIV,
  MYSQL_USER_FIELD_TRIGGER_PRIV,
  MYSQL_USER_FIELD_CREATE_TABLESPACE_PRIV,
  MYSQL_USER_FIELD_SSL_TYPE,
  MYSQL_USER_FIELD_SSL_CIPHER,
  MYSQL_USER_FIELD_X509_ISSUER,
  MYSQL_USER_FIELD_X509_SUBJECT,
  MYSQL_USER_FIELD_MAX_QUESTIONS,
  MYSQL_USER_FIELD_MAX_UPDATES,
  MYSQL_USER_FIELD_MAX_CONNECTIONS,
  MYSQL_USER_FIELD_MAX_USER_CONNECTIONS,
  MYSQL_USER_FIELD_PLUGIN,
  MYSQL_USER_FIELD_AUTHENTICATION_STRING,
  MYSQL_USER_FIELD_PASSWORD_EXPIRED,
  MYSQL_USER_FIELD_PASSWORD_LAST_CHANGED,
  MYSQL_USER_FIELD_PASSWORD_LIFETIME,
  MYSQL_USER_FIELD_COUNT
};

extern const TABLE_FIELD_DEF mysql_db_table_def;
extern bool mysql_user_table_is_in_short_password_format;
extern my_bool disconnect_on_expired_password;
extern const char *any_db;	// Special symbol for check_access
/** controls the extra checks on plugin availability for mysql.user records */

#ifndef NO_EMBEDDED_ACCESS_CHECKS
extern my_bool validate_user_plugins;
#endif /* NO_EMBEDDED_ACCESS_CHECKS */

/* Function Declarations */

/* sql_authentication */

int set_default_auth_plugin(char *plugin_name, size_t plugin_name_length);
int acl_authenticate(THD *thd, size_t com_change_user_pkt_len);
int check_password_strength(String *password);
int check_password_policy(String *password);
bool acl_check_host(const char *host, const char *ip);

/* sql_user */
void append_user(THD *thd, String *str, LEX_USER *user,
                 bool comma, bool ident);
int check_change_password(THD *thd, const char *host, const char *user,
                          const char *password, size_t password_len);
bool change_password(THD *thd, const char *host, const char *user,
                     char *password);
bool mysql_create_user(THD *thd, List <LEX_USER> &list);
bool mysql_drop_user(THD *thd, List <LEX_USER> &list);
bool mysql_rename_user(THD *thd, List <LEX_USER> &list);
bool mysql_user_password_expire(THD *thd, List <LEX_USER> &list);
int digest_password(THD *thd, LEX_USER *user_record);


/* sql_auth_cache */
int wild_case_compare(CHARSET_INFO *cs, const char *str,const char *wildstr);
bool hostname_requires_resolving(const char *hostname);
my_bool acl_init(bool dont_read_acl_tables);
void acl_free(bool end=0);
my_bool acl_reload(THD *thd); 
my_bool grant_init();
void grant_free(void);
my_bool grant_reload(THD *thd);
ulong acl_get(const char *host, const char *ip,
              const char *user, const char *db, my_bool db_is_pattern);
bool is_acl_user(const char *host, const char *user);
bool acl_getroot(Security_context *sctx, char *user,
                 char *host, char *ip, const char *db);

/* sql_authorization */
bool mysql_grant(THD *thd, const char *db, List <LEX_USER> &user_list,
                 ulong rights, bool revoke, bool is_proxy);
bool mysql_routine_grant(THD *thd, TABLE_LIST *table, bool is_proc,
                         List <LEX_USER> &user_list, ulong rights,
                         bool revoke, bool write_to_binlog);
int mysql_table_grant(THD *thd, TABLE_LIST *table, List <LEX_USER> &user_list,
                       List <LEX_COLUMN> &column_list, ulong rights,
                       bool revoke);
bool check_grant(THD *thd, ulong want_access, TABLE_LIST *tables,
                 bool any_combination_will_do, uint number, bool no_errors);
bool check_grant_column (THD *thd, GRANT_INFO *grant,
                         const char *db_name, const char *table_name,
                         const char *name, size_t length, Security_context *sctx);
bool check_column_grant_in_table_ref(THD *thd, TABLE_LIST * table_ref,
                                     const char *name, size_t length);
bool check_grant_all_columns(THD *thd, ulong want_access, 
                             Field_iterator_table_ref *fields);
bool check_grant_routine(THD *thd, ulong want_access,
                         TABLE_LIST *procs, bool is_proc, bool no_error);
bool check_routine_level_acl(THD *thd, const char *db, const char *name,
                             bool is_proc);
bool check_grant_db(THD *thd,const char *db);
bool acl_check_proxy_grant_access(THD *thd, const char *host, const char *user,
                                  bool with_grant);
void get_privilege_desc(char *to, uint max_length, ulong access);
void get_mqh(const char *user, const char *host, USER_CONN *uc);
ulong get_table_grant(THD *thd, TABLE_LIST *table);
ulong get_column_grant(THD *thd, GRANT_INFO *grant,
                       const char *db_name, const char *table_name,
                       const char *field_name);
bool mysql_show_grants(THD *thd, LEX_USER *user);
bool mysql_revoke_all(THD *thd, List <LEX_USER> &list);
bool sp_revoke_privileges(THD *thd, const char *sp_db, const char *sp_name,
                          bool is_proc);
bool sp_grant_privileges(THD *thd, const char *sp_db, const char *sp_name,
                         bool is_proc);
void fill_effective_table_privileges(THD *thd, GRANT_INFO *grant,
                                     const char *db, const char *table);
int fill_schema_user_privileges(THD *thd, TABLE_LIST *tables, Item *cond);
int fill_schema_schema_privileges(THD *thd, TABLE_LIST *tables, Item *cond);
int fill_schema_table_privileges(THD *thd, TABLE_LIST *tables, Item *cond);
int fill_schema_column_privileges(THD *thd, TABLE_LIST *tables, Item *cond);
const ACL_internal_schema_access *
get_cached_schema_access(GRANT_INTERNAL_INFO *grant_internal_info,
                         const char *schema_name);

bool select_precheck(THD *thd, LEX *lex, TABLE_LIST *tables,
                     TABLE_LIST *first_table);
bool multi_update_precheck(THD *thd, TABLE_LIST *tables);
bool multi_delete_precheck(THD *thd, TABLE_LIST *tables);
bool update_precheck(THD *thd, TABLE_LIST *tables);
bool delete_precheck(THD *thd, TABLE_LIST *tables);
bool insert_precheck(THD *thd, TABLE_LIST *tables);
bool lock_tables_precheck(THD *thd, TABLE_LIST *tables);
bool create_table_precheck(THD *thd, TABLE_LIST *tables,
                           TABLE_LIST *create_table);

#ifndef NO_EMBEDDED_ACCESS_CHECKS

bool check_one_table_access(THD *thd, ulong privilege, TABLE_LIST *tables);
bool check_single_table_access(THD *thd, ulong privilege,
			   TABLE_LIST *tables, bool no_errors);
bool check_routine_access(THD *thd, ulong want_access, const char *db,
                          char *name, bool is_proc, bool no_errors);
bool check_some_access(THD *thd, ulong want_access, TABLE_LIST *table);
bool check_some_routine_access(THD *thd, const char *db, const char *name, bool is_proc);
bool check_access(THD *thd, ulong want_access, const char *db, ulong *save_priv,
                  GRANT_INTERNAL_INFO *grant_internal_info,
                  bool dont_check_global_grants, bool no_errors);
bool check_table_access(THD *thd, ulong requirements,TABLE_LIST *tables,
                        bool any_combination_of_privileges_will_do,
                        uint number,
                        bool no_errors);
#else
inline bool check_one_table_access(THD *thd, ulong privilege, TABLE_LIST *tables)
{ return false; }
inline bool check_single_table_access(THD *thd, ulong privilege,
			   TABLE_LIST *tables, bool no_errors)
{ return false; }
inline bool check_routine_access(THD *thd,ulong want_access,const char *db,
                                 char *name, bool is_proc, bool no_errors)
{ return false; }
inline bool check_some_access(THD *thd, ulong want_access, TABLE_LIST *table)
{
  table->grant.privilege= want_access;
  return false;
}
inline bool check_some_routine_access(THD *thd, const char *db,
                                      const char *name, bool is_proc)
{ return false; }
inline bool check_access(THD *, ulong, const char *, ulong *save_priv,
                         GRANT_INTERNAL_INFO *, bool, bool)
{
  if (save_priv)
    *save_priv= GLOBAL_ACLS;
  return false;
}
inline bool
check_table_access(THD *thd, ulong requirements,TABLE_LIST *tables,
                   bool any_combination_of_privileges_will_do,
                   uint number,
                   bool no_errors)
{ return false; }
#endif /*NO_EMBEDDED_ACCESS_CHECKS*/

/* These was under the INNODB_COMPATIBILITY_HOOKS */

bool check_global_access(THD *thd, ulong want_access);

#ifdef NO_EMBEDDED_ACCESS_CHECKS
#define check_grant(A,B,C,D,E,F) 0
#define check_grant_db(A,B) 0
#endif

/* sql_user_table */
void close_acl_tables(THD *thd);

#if defined(HAVE_OPENSSL) && !defined(HAVE_YASSL)
extern my_bool opt_auto_generate_certs;
bool do_auto_cert_generation();
#endif /* HAVE_OPENSSL && !HAVE_YASSL */
#endif /* AUTH_COMMON_INCLUDED */
