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

#include "sql_base.h"                   /* open_normal_and_derived_tables */
#include "sql_table.h"                  /* build_table_filename */
#include "sql_show.h"                   /* append_identifier */
#include "sql_view.h"                   /* VIEW_ANY_ACL */
#include "rpl_filter.h"                 /* rpl_filter */
#include "sql_parse.h"                  /* get_current_user */
                                        /* any_db */
#include "binlog.h"                     /* mysql_bin_log */
#include "sp.h"                         /* sp_exist_routines */

#include "auth_internal.h"
#include "sql_auth_cache.h"
#include "sql_authentication.h"
#include "sql_authorization.h"

const char *command_array[]=
{
  "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "RELOAD",
  "SHUTDOWN", "PROCESS","FILE", "GRANT", "REFERENCES", "INDEX",
  "ALTER", "SHOW DATABASES", "SUPER", "CREATE TEMPORARY TABLES",
  "LOCK TABLES", "EXECUTE", "REPLICATION SLAVE", "REPLICATION CLIENT",
  "CREATE VIEW", "SHOW VIEW", "CREATE ROUTINE", "ALTER ROUTINE",
  "CREATE USER", "EVENT", "TRIGGER", "CREATE TABLESPACE"
};

uint command_lengths[]=
{
  6, 6, 6, 6, 6, 4, 6, 8, 7, 4, 5, 10, 5, 5, 14, 5, 23, 11, 7, 17, 18, 11, 9,
  14, 13, 11, 5, 7, 17
};

const char *any_db="*any*";	// Special symbol for check_access


static bool check_show_access(THD *thd, TABLE_LIST *table);

/**
  Get a cached internal schema access.
  @param grant_internal_info the cache
  @param schema_name the name of the internal schema
*/
const ACL_internal_schema_access *
get_cached_schema_access(GRANT_INTERNAL_INFO *grant_internal_info,
                         const char *schema_name)
{
  if (grant_internal_info)
  {
    if (! grant_internal_info->m_schema_lookup_done)
    {
      grant_internal_info->m_schema_access=
        ACL_internal_schema_registry::lookup(schema_name);
      grant_internal_info->m_schema_lookup_done= TRUE;
    }
    return grant_internal_info->m_schema_access;
  }
  return ACL_internal_schema_registry::lookup(schema_name);
}


/**
  Get a cached internal table access.
  @param grant_internal_info the cache
  @param schema_name the name of the internal schema
  @param table_name the name of the internal table
*/
const ACL_internal_table_access *
get_cached_table_access(GRANT_INTERNAL_INFO *grant_internal_info,
                        const char *schema_name,
                        const char *table_name)
{
  DBUG_ASSERT(grant_internal_info);
  if (! grant_internal_info->m_table_lookup_done)
  {
    const ACL_internal_schema_access *schema_access;
    schema_access= get_cached_schema_access(grant_internal_info, schema_name);
    if (schema_access)
      grant_internal_info->m_table_access= schema_access->lookup(table_name);
    grant_internal_info->m_table_lookup_done= TRUE;
  }
  return grant_internal_info->m_table_access;
}


ACL_internal_access_result
IS_internal_schema_access::check(ulong want_access,
                                 ulong *save_priv) const
{
  want_access &= ~SELECT_ACL;

  /*
    We don't allow any simple privileges but SELECT_ACL on
    the information_schema database.
  */
  if (unlikely(want_access & DB_ACLS))
    return ACL_INTERNAL_ACCESS_DENIED;

  /* Always grant SELECT for the information schema. */
  *save_priv|= SELECT_ACL;

  return want_access ? ACL_INTERNAL_ACCESS_CHECK_GRANT :
                       ACL_INTERNAL_ACCESS_GRANTED;
}

const ACL_internal_table_access *
IS_internal_schema_access::lookup(const char *name) const
{
  /* There are no per table rules for the information schema. */
  return NULL;
}

/**
  Perform first stage of privilege checking for SELECT statement.

  @param thd          Thread context.
  @param lex          LEX for SELECT statement.
  @param tables       List of tables used by statement.
  @param first_table  First table in the main SELECT of the SELECT
                      statement.

  @retval FALSE - Success (column-level privilege checks might be required).
  @retval TRUE  - Failure, privileges are insufficient.
*/

bool select_precheck(THD *thd, LEX *lex, TABLE_LIST *tables,
                     TABLE_LIST *first_table)
{
  bool res;
  /*
    lex->exchange != NULL implies SELECT .. INTO OUTFILE and this
    requires FILE_ACL access.
  */
  ulong privileges_requested= lex->exchange ? SELECT_ACL | FILE_ACL :
                                              SELECT_ACL;

  if (tables)
  {
    res= check_table_access(thd,
                            privileges_requested,
                            tables, FALSE, UINT_MAX, FALSE) ||
         (first_table && first_table->schema_table_reformed &&
          check_show_access(thd, first_table));
  }
  else
    res= check_access(thd, privileges_requested, any_db, NULL, NULL, 0, 0);

  return res;
}


/**
  Multi update query pre-check.

  @param thd		Thread handler
  @param tables	Global/local table list (have to be the same)

  @retval
    FALSE OK
  @retval
    TRUE  Error
*/

bool multi_update_precheck(THD *thd, TABLE_LIST *tables)
{
  const char *msg= 0;
  TABLE_LIST *table;
  LEX *lex= thd->lex;
  SELECT_LEX *select_lex= lex->select_lex;
  DBUG_ENTER("multi_update_precheck");

  if (select_lex->item_list.elements != lex->value_list.elements)
  {
    my_message(ER_WRONG_VALUE_COUNT, ER(ER_WRONG_VALUE_COUNT), MYF(0));
    DBUG_RETURN(TRUE);
  }
  /*
    Ensure that we have UPDATE or SELECT privilege for each table
    The exact privilege is checked in mysql_multi_update()
  */
  for (table= tables; table; table= table->next_local)
  {
    if (table->derived)
      table->grant.privilege= SELECT_ACL;
    else if ((check_access(thd, UPDATE_ACL, table->db,
                           &table->grant.privilege,
                           &table->grant.m_internal,
                           0, 1) ||
              check_grant(thd, UPDATE_ACL, table, FALSE, 1, TRUE)) &&
             (check_access(thd, SELECT_ACL, table->db,
                           &table->grant.privilege,
                           &table->grant.m_internal,
                           0, 0) ||
              check_grant(thd, SELECT_ACL, table, FALSE, 1, FALSE)))
      DBUG_RETURN(TRUE);

    table->table_in_first_from_clause= 1;
  }
  /*
    Is there tables of subqueries?
  */
  if (lex->select_lex != lex->all_selects_list)
  {
    DBUG_PRINT("info",("Checking sub query list"));
    for (table= tables; table; table= table->next_global)
    {
      if (!table->table_in_first_from_clause)
      {
	if (check_access(thd, SELECT_ACL, table->db,
                         &table->grant.privilege,
                         &table->grant.m_internal,
                         0, 0) ||
	    check_grant(thd, SELECT_ACL, table, FALSE, 1, FALSE))
	  DBUG_RETURN(TRUE);
      }
    }
  }

  if (select_lex->order_list.elements)
    msg= "ORDER BY";
  else if (select_lex->select_limit)
    msg= "LIMIT";
  if (msg)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "UPDATE", msg);
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


/**
  Multi delete query pre-check.

  @param thd			Thread handler
  @param tables		Global/local table list

  @retval
    FALSE OK
  @retval
    TRUE  error
*/

bool multi_delete_precheck(THD *thd, TABLE_LIST *tables)
{
  SELECT_LEX *select_lex= thd->lex->select_lex;
  TABLE_LIST *aux_tables= thd->lex->auxiliary_table_list.first;
  TABLE_LIST **save_query_tables_own_last= thd->lex->query_tables_own_last;
  DBUG_ENTER("multi_delete_precheck");

  /* sql_yacc guarantees that tables and aux_tables are not zero */
  DBUG_ASSERT(aux_tables != 0);
  if (check_table_access(thd, SELECT_ACL, tables, FALSE, UINT_MAX, FALSE))
    DBUG_RETURN(TRUE);

  /*
    Since aux_tables list is not part of LEX::query_tables list we
    have to juggle with LEX::query_tables_own_last value to be able
    call check_table_access() safely.
  */
  thd->lex->query_tables_own_last= 0;
  if (check_table_access(thd, DELETE_ACL, aux_tables, FALSE, UINT_MAX, FALSE))
  {
    thd->lex->query_tables_own_last= save_query_tables_own_last;
    DBUG_RETURN(TRUE);
  }
  thd->lex->query_tables_own_last= save_query_tables_own_last;

  if ((thd->variables.option_bits & OPTION_SAFE_UPDATES) &&
      !select_lex->where_cond())
  {
    my_message(ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE,
               ER(ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE), MYF(0));
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


/**
  simple UPDATE query pre-check.

  @param thd		Thread handler
  @param tables	Global table list

  @retval
    FALSE OK
  @retval
    TRUE  Error
*/

bool update_precheck(THD *thd, TABLE_LIST *tables)
{
  DBUG_ENTER("update_precheck");
  if (thd->lex->select_lex->item_list.elements != thd->lex->value_list.elements)
  {
    my_message(ER_WRONG_VALUE_COUNT, ER(ER_WRONG_VALUE_COUNT), MYF(0));
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(check_one_table_access(thd, UPDATE_ACL, tables));
}


/**
  simple DELETE query pre-check.

  @param thd		Thread handler
  @param tables	Global table list

  @retval
    FALSE  OK
  @retval
    TRUE   error
*/

bool delete_precheck(THD *thd, TABLE_LIST *tables)
{
  DBUG_ENTER("delete_precheck");
  if (check_one_table_access(thd, DELETE_ACL, tables))
    DBUG_RETURN(TRUE);
  /* Set privilege for the WHERE clause */
  tables->grant.want_privilege=(SELECT_ACL & ~tables->grant.privilege);
  DBUG_RETURN(FALSE);
}


/**
  simple INSERT query pre-check.

  @param thd		Thread handler
  @param tables	Global table list

  @retval
    FALSE  OK
  @retval
    TRUE   error
*/

bool insert_precheck(THD *thd, TABLE_LIST *tables)
{
  LEX *lex= thd->lex;
  DBUG_ENTER("insert_precheck");

  /*
    Check that we have modify privileges for the first table and
    select privileges for the rest
  */
  ulong privilege= (INSERT_ACL |
                    (lex->duplicates == DUP_REPLACE ? DELETE_ACL : 0) |
                    (lex->value_list.elements ? UPDATE_ACL : 0));

  if (check_one_table_access(thd, privilege, tables))
    DBUG_RETURN(TRUE);

  if (lex->update_list.elements != lex->value_list.elements)
  {
    my_message(ER_WRONG_VALUE_COUNT, ER(ER_WRONG_VALUE_COUNT), MYF(0));
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


/**
  Check privileges for LOCK TABLES statement.

  @param thd     Thread context.
  @param tables  List of tables to be locked.

  @retval FALSE - Success.
  @retval TRUE  - Failure.
*/

bool lock_tables_precheck(THD *thd, TABLE_LIST *tables)
{
  TABLE_LIST *first_not_own_table= thd->lex->first_not_own_table();

  for (TABLE_LIST *table= tables; table != first_not_own_table && table;
       table= table->next_global)
  {
    if (is_temporary_table(table))
      continue;

    if (check_table_access(thd, LOCK_TABLES_ACL | SELECT_ACL, table,
                           FALSE, 1, FALSE))
      return TRUE;
  }

  return FALSE;
}


/**
  CREATE TABLE query pre-check.

  @param thd			Thread handler
  @param tables		Global table list
  @param create_table	        Table which will be created

  @retval
    FALSE   OK
  @retval
    TRUE   Error
*/

bool create_table_precheck(THD *thd, TABLE_LIST *tables,
                           TABLE_LIST *create_table)
{
  LEX *lex= thd->lex;
  SELECT_LEX *select_lex= lex->select_lex;
  ulong want_priv;
  bool error= TRUE;                                 // Error message is given
  DBUG_ENTER("create_table_precheck");

  /*
    Require CREATE [TEMPORARY] privilege on new table; for
    CREATE TABLE ... SELECT, also require INSERT.
  */

  want_priv= (lex->create_info.options & HA_LEX_CREATE_TMP_TABLE) ?
             CREATE_TMP_ACL :
             (CREATE_ACL | (select_lex->item_list.elements ? INSERT_ACL : 0));

  if (check_access(thd, want_priv, create_table->db,
                   &create_table->grant.privilege,
                   &create_table->grant.m_internal,
                   0, 0))
    goto err;

  /* If it is a merge table, check privileges for merge children. */
  if (lex->create_info.merge_list.first)
  {
    /*
      The user must have (SELECT_ACL | UPDATE_ACL | DELETE_ACL) on the
      underlying base tables, even if there are temporary tables with the same
      names.

      From user's point of view, it might look as if the user must have these
      privileges on temporary tables to create a merge table over them. This is
      one of two cases when a set of privileges is required for operations on
      temporary tables (see also CREATE TABLE).

      The reason for this behavior stems from the following facts:

        - For merge tables, the underlying table privileges are checked only
          at CREATE TABLE / ALTER TABLE time.

          In other words, once a merge table is created, the privileges of
          the underlying tables can be revoked, but the user will still have
          access to the merge table (provided that the user has privileges on
          the merge table itself). 

        - Temporary tables shadow base tables.

          I.e. there might be temporary and base tables with the same name, and
          the temporary table takes the precedence in all operations.

        - For temporary MERGE tables we do not track if their child tables are
          base or temporary. As result we can't guarantee that privilege check
          which was done in presence of temporary child will stay relevant later
          as this temporary table might be removed.

      If SELECT_ACL | UPDATE_ACL | DELETE_ACL privileges were not checked for
      the underlying *base* tables, it would create a security breach as in
      Bug#12771903.
    */

    if (check_table_access(thd, SELECT_ACL | UPDATE_ACL | DELETE_ACL,
                           lex->create_info.merge_list.first,
                           FALSE, UINT_MAX, FALSE))
      goto err;
  }

  if (want_priv != CREATE_TMP_ACL &&
      check_grant(thd, want_priv, create_table, FALSE, 1, FALSE))
    goto err;

  if (select_lex->item_list.elements)
  {
    /* Check permissions for used tables in CREATE TABLE ... SELECT */
    if (tables && check_table_access(thd, SELECT_ACL, tables, FALSE,
                                     UINT_MAX, FALSE))
      goto err;
  }
  else if (lex->create_info.options & HA_LEX_CREATE_TABLE_LIKE)
  {
    if (check_table_access(thd, SELECT_ACL, tables, FALSE, UINT_MAX, FALSE))
      goto err;
  }
  error= FALSE;

err:
  DBUG_RETURN(error);
}


#ifndef NO_EMBEDDED_ACCESS_CHECKS

/**
  Check grants for commands which work only with one table and all other
  tables belonging to subselects or implicitly opened tables.

  @param thd			Thread handler
  @param privilege		requested privilege
  @param all_tables		global table list of query

  @retval
    0   OK
  @retval
    1   access denied, error is sent to client
*/

bool check_one_table_access(THD *thd, ulong privilege, TABLE_LIST *all_tables)
{
  if (check_single_table_access (thd,privilege,all_tables, FALSE))
    return 1;

  /* Check rights on tables of subselects and implicitly opened tables */
  TABLE_LIST *subselects_tables, *view= all_tables->view ? all_tables : 0;
  if ((subselects_tables= all_tables->next_global))
  {
    /*
      Access rights asked for the first table of a view should be the same
      as for the view
    */
    if (view && subselects_tables->belong_to_view == view)
    {
      if (check_single_table_access (thd, privilege, subselects_tables, FALSE))
        return 1;
      subselects_tables= subselects_tables->next_global;
    }
    if (subselects_tables &&
        (check_table_access(thd, SELECT_ACL, subselects_tables, FALSE,
                            UINT_MAX, FALSE)))
      return 1;
  }
  return 0;
}


/**
  Check grants for commands which work only with one table.

  @param thd                    Thread handler
  @param privilege              requested privilege
  @param all_tables             global table list of query
  @param no_errors              FALSE/TRUE - report/don't report error to
                            the client (using my_error() call).

  @retval
    0   OK
  @retval
    1   access denied, error is sent to client
*/

bool check_single_table_access(THD *thd, ulong privilege, 
                               TABLE_LIST *all_tables, bool no_errors)
{
  Security_context * backup_ctx= thd->security_ctx;

  /* we need to switch to the saved context (if any) */
  if (all_tables->security_ctx)
    thd->security_ctx= all_tables->security_ctx;

  const char *db_name;
  if ((all_tables->view || all_tables->field_translation) &&
      !all_tables->schema_table)
    db_name= all_tables->view_db.str;
  else
    db_name= all_tables->db;

  if (check_access(thd, privilege, db_name,
                   &all_tables->grant.privilege,
                   &all_tables->grant.m_internal,
                   0, no_errors))
    goto deny;

  /* Show only 1 table for check_grant */
  if (!(all_tables->belong_to_view &&
        (thd->lex->sql_command == SQLCOM_SHOW_FIELDS)) &&
      check_grant(thd, privilege, all_tables, FALSE, 1, no_errors))
    goto deny;

  thd->security_ctx= backup_ctx;
  return 0;

deny:
  thd->security_ctx= backup_ctx;
  return 1;
}


bool
check_routine_access(THD *thd, ulong want_access, const char *db, char *name,
		     bool is_proc, bool no_errors)
{
  TABLE_LIST tables[1];
  
  memset(tables, 0, sizeof(TABLE_LIST));
  tables->db= db;
  tables->table_name= tables->alias= name;
  
  /*
    The following test is just a shortcut for check_access() (to avoid
    calculating db_access) under the assumption that it's common to
    give persons global right to execute all stored SP (but not
    necessary to create them).
    Note that this effectively bypasses the ACL_internal_schema_access checks
    that are implemented for the INFORMATION_SCHEMA and PERFORMANCE_SCHEMA,
    which are located in check_access().
    Since the I_S and P_S do not contain routines, this bypass is ok,
    as long as this code path is not abused to create routines.
    The assert enforce that.
  */
  DBUG_ASSERT((want_access & CREATE_PROC_ACL) == 0);
  if ((thd->security_ctx->master_access & want_access) == want_access)
    tables->grant.privilege= want_access;
  else if (check_access(thd, want_access, db,
                        &tables->grant.privilege,
                        &tables->grant.m_internal,
                        0, no_errors))
    return TRUE;
  
  return check_grant_routine(thd, want_access, tables, is_proc, no_errors);
}


/**
  Check if the given table has any of the asked privileges

  @param thd		 Thread handler
  @param want_access	 Bitmap of possible privileges to check for

  @retval
    0  ok
  @retval
    1  error
*/

bool check_some_access(THD *thd, ulong want_access, TABLE_LIST *table)
{
  ulong access;
  DBUG_ENTER("check_some_access");

  /* This loop will work as long as we have less than 32 privileges */
  for (access= 1; access < want_access ; access<<= 1)
  {
    if (access & want_access)
    {
      if (!check_access(thd, access, table->db,
                        &table->grant.privilege,
                        &table->grant.m_internal,
                        0, 1) &&
           !check_grant(thd, access, table, FALSE, 1, TRUE))
        DBUG_RETURN(0);
    }
  }
  DBUG_PRINT("exit",("no matching access rights"));
  DBUG_RETURN(1);
}


/**
  Check if the routine has any of the routine privileges.

  @param thd	       Thread handler
  @param db           Database name
  @param name         Routine name

  @retval
    0            ok
  @retval
    1            error
*/

bool check_some_routine_access(THD *thd, const char *db, const char *name,
                               bool is_proc)
{
  ulong save_priv;
  /*
    The following test is just a shortcut for check_access() (to avoid
    calculating db_access)
    Note that this effectively bypasses the ACL_internal_schema_access checks
    that are implemented for the INFORMATION_SCHEMA and PERFORMANCE_SCHEMA,
    which are located in check_access().
    Since the I_S and P_S do not contain routines, this bypass is ok,
    as it only opens SHOW_PROC_ACLS.
  */
  if (thd->security_ctx->master_access & SHOW_PROC_ACLS)
    return FALSE;
  if (!check_access(thd, SHOW_PROC_ACLS, db, &save_priv, NULL, 0, 1) ||
      (save_priv & SHOW_PROC_ACLS))
    return FALSE;
  return check_routine_level_acl(thd, db, name, is_proc);
}


/**
  @brief Compare requested privileges with the privileges acquired from the
    User- and Db-tables.
  @param thd          Thread handler
  @param want_access  The requested access privileges.
  @param db           A pointer to the Db name.
  @param[out] save_priv A pointer to the granted privileges will be stored.
  @param grant_internal_info A pointer to the internal grant cache.
  @param dont_check_global_grants True if no global grants are checked.
  @param no_error     True if no errors should be sent to the client.

  'save_priv' is used to save the User-table (global) and Db-table grants for
  the supplied db name. Note that we don't store db level grants if the global
  grants is enough to satisfy the request AND the global grants contains a
  SELECT grant.

  For internal databases (INFORMATION_SCHEMA, PERFORMANCE_SCHEMA),
  additional rules apply, see ACL_internal_schema_access.

  @see check_grant

  @return Status of denial of access by exclusive ACLs.
    @retval FALSE Access can't exclusively be denied by Db- and User-table
      access unless Column- and Table-grants are checked too.
    @retval TRUE Access denied.
*/

bool
check_access(THD *thd, ulong want_access, const char *db, ulong *save_priv,
             GRANT_INTERNAL_INFO *grant_internal_info,
             bool dont_check_global_grants, bool no_errors)
{
  Security_context *sctx= thd->security_ctx;
  ulong db_access;

  /*
    GRANT command:
    In case of database level grant the database name may be a pattern,
    in case of table|column level grant the database name can not be a pattern.
    We use 'dont_check_global_grants' as a flag to determine
    if it's database level grant command
    (see SQLCOM_GRANT case, mysql_execute_command() function) and
    set db_is_pattern according to 'dont_check_global_grants' value.
  */
  bool  db_is_pattern= ((want_access & GRANT_ACL) && dont_check_global_grants);
  ulong dummy;
  DBUG_ENTER("check_access");
  DBUG_PRINT("enter",("db: %s  want_access: %lu  master_access: %lu",
                      db ? db : "", want_access, sctx->master_access));

  if (save_priv)
    *save_priv=0;
  else
  {
    save_priv= &dummy;
    dummy= 0;
  }

  THD_STAGE_INFO(thd, stage_checking_permissions);
  if ((!db || !db[0]) && !thd->db().str && !dont_check_global_grants)
  {
    DBUG_PRINT("error",("No database"));
    if (!no_errors)
      my_message(ER_NO_DB_ERROR, ER(ER_NO_DB_ERROR),
                 MYF(0));                       /* purecov: tested */
    DBUG_RETURN(TRUE);				/* purecov: tested */
  }

  if ((db != NULL) && (db != any_db))
  {
    const ACL_internal_schema_access *access;
    access= get_cached_schema_access(grant_internal_info, db);
    if (access)
    {
      switch (access->check(want_access, save_priv))
      {
      case ACL_INTERNAL_ACCESS_GRANTED:
        /*
          All the privileges requested have been granted internally.
          [out] *save_privileges= Internal privileges.
        */
        DBUG_RETURN(FALSE);
      case ACL_INTERNAL_ACCESS_DENIED:
        if (! no_errors)
        {
          my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
                   sctx->priv_user, sctx->priv_host, db);
        }
        DBUG_RETURN(TRUE);
      case ACL_INTERNAL_ACCESS_CHECK_GRANT:
        /*
          Only some of the privilege requested have been granted internally,
          proceed with the remaining bits of the request (want_access).
        */
        want_access&= ~(*save_priv);
        break;
      }
    }
  }

  if ((sctx->master_access & want_access) == want_access)
  {
    /*
      1. If we don't have a global SELECT privilege, we have to get the
      database specific access rights to be able to handle queries of type
      UPDATE t1 SET a=1 WHERE b > 0
      2. Change db access if it isn't current db which is being addressed
    */
    if (!(sctx->master_access & SELECT_ACL))
    {
      if (db && (!thd->db().str || db_is_pattern ||
                 strcmp(db, thd->db().str)))
        db_access= acl_get(sctx->get_host()->ptr(), sctx->get_ip()->ptr(),
                           sctx->priv_user, db, db_is_pattern);
      else
      {
        /* get access for current db */
        db_access= sctx->db_access;
      }
      /*
        The effective privileges are the union of the global privileges
        and the intersection of db- and host-privileges,
        plus the internal privileges.
      */
      *save_priv|= sctx->master_access | db_access;
    }
    else
      *save_priv|= sctx->master_access;
    DBUG_RETURN(FALSE);
  }
  if (((want_access & ~sctx->master_access) & ~DB_ACLS) ||
      (! db && dont_check_global_grants))
  {						// We can never grant this
    DBUG_PRINT("error",("No possible access"));
    if (!no_errors)
    {
      if (thd->password == 2)
        my_error(ER_ACCESS_DENIED_NO_PASSWORD_ERROR, MYF(0),
                 sctx->priv_user,
                 sctx->priv_host);
      else
        my_error(ER_ACCESS_DENIED_ERROR, MYF(0),
                 sctx->priv_user,
                 sctx->priv_host,
                 (thd->password ?
                  ER(ER_YES) :
                  ER(ER_NO)));                    /* purecov: tested */
    }
    DBUG_RETURN(TRUE);				/* purecov: tested */
  }

  if (db == any_db)
  {
    /*
      Access granted; Allow select on *any* db.
      [out] *save_privileges= 0
    */
    DBUG_RETURN(FALSE);
  }

  if (db && (!thd->db().str || db_is_pattern || strcmp(db,thd->db().str)))
    db_access= acl_get(sctx->get_host()->ptr(), sctx->get_ip()->ptr(),
                       sctx->priv_user, db, db_is_pattern);
  else
    db_access= sctx->db_access;
  DBUG_PRINT("info",("db_access: %lu  want_access: %lu",
                     db_access, want_access));

  /*
    Save the union of User-table and the intersection between Db-table and
    Host-table privileges, with the already saved internal privileges.
  */
  db_access= (db_access | sctx->master_access);
  *save_priv|= db_access;

  /*
    We need to investigate column- and table access if all requested privileges
    belongs to the bit set of .
  */
  bool need_table_or_column_check=
    (want_access & (TABLE_ACLS | PROC_ACLS | db_access)) == want_access;

  /*
    Grant access if the requested access is in the intersection of
    host- and db-privileges (as retrieved from the acl cache),
    also grant access if all the requested privileges are in the union of
    TABLES_ACLS and PROC_ACLS; see check_grant.
  */
  if ( (db_access & want_access) == want_access ||
      (!dont_check_global_grants &&
       need_table_or_column_check))
  {
    /*
       Ok; but need to check table- and column privileges.
       [out] *save_privileges is (User-priv | (Db-priv & Host-priv) | Internal-priv)
    */
    DBUG_RETURN(FALSE);
  }

  /*
    Access is denied;
    [out] *save_privileges is (User-priv | (Db-priv & Host-priv) | Internal-priv)
  */
  DBUG_PRINT("error",("Access denied"));
  if (!no_errors)
    my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
             sctx->priv_user, sctx->priv_host,
             (db ? db : (thd->db().str ?
                         thd->db().str :
                         "unknown")));
  DBUG_RETURN(TRUE);

}


/**
  @brief Check if the requested privileges exists in either User-, Host- or
    Db-tables.
  @param thd          Thread context
  @param want_access  Privileges requested
  @param tables       List of tables to be compared against
  @param no_errors    Don't report error to the client (using my_error() call).
  @param any_combination_of_privileges_will_do TRUE if any privileges on any
    column combination is enough.
  @param number       Only the first 'number' tables in the linked list are
                      relevant.

  The suppled table list contains cached privileges. This functions calls the
  help functions check_access and check_grant to verify the first three steps
  in the privileges check queue:
  1. Global privileges
  2. OR (db privileges AND host privileges)
  3. OR table privileges
  4. OR column privileges (not checked by this function!)
  5. OR routine privileges (not checked by this function!)

  @see check_access
  @see check_grant

  @note This functions assumes that table list used and
  thd->lex->query_tables_own_last value correspond to each other
  (the latter should be either 0 or point to next_global member
  of one of elements of this table list).

  @return
    @retval FALSE OK
    @retval TRUE  Access denied; But column or routine privileges might need to
      be checked also.
*/

bool
check_table_access(THD *thd, ulong requirements,TABLE_LIST *tables,
		   bool any_combination_of_privileges_will_do,
                   uint number, bool no_errors)
{
  TABLE_LIST *org_tables= tables;
  TABLE_LIST *first_not_own_table= thd->lex->first_not_own_table();
  uint i= 0;
  Security_context *sctx= thd->security_ctx, *backup_ctx= thd->security_ctx;
  /*
    The check that first_not_own_table is not reached is for the case when
    the given table list refers to the list for prelocking (contains tables
    of other queries). For simple queries first_not_own_table is 0.
  */
  for (; i < number && tables != first_not_own_table && tables;
       tables= tables->next_global, i++)
  {
    TABLE_LIST *const table_ref= tables->correspondent_table ?
      tables->correspondent_table : tables;
    ulong want_access= requirements;
    if (table_ref->security_ctx)
      sctx= table_ref->security_ctx;
    else
      sctx= backup_ctx;

    /*
       Register access for view underlying table.
       Remove SHOW_VIEW_ACL, because it will be checked during making view
     */
    table_ref->grant.orig_want_privilege= (want_access & ~SHOW_VIEW_ACL);

    /*
      We should not encounter table list elements for reformed SHOW
      statements unless this is first table list element in the main
      select.
      Such table list elements require additional privilege check
      (see check_show_access()). This check is carried out by caller,
      but only for the first table list element from the main select.
    */
    DBUG_ASSERT(!table_ref->schema_table_reformed ||
                table_ref == thd->lex->select_lex->table_list.first);

    DBUG_PRINT("info", ("derived: %d  view: %d", table_ref->derived != 0,
                        table_ref->view != 0));

    if (table_ref->is_anonymous_derived_table())
      continue;

    thd->security_ctx= sctx;

    if (check_access(thd, want_access, table_ref->get_db_name(),
                     &table_ref->grant.privilege,
                     &table_ref->grant.m_internal,
                     0, no_errors))
      goto deny;
  }
  thd->security_ctx= backup_ctx;
  return check_grant(thd,requirements,org_tables,
                     any_combination_of_privileges_will_do,
                     number, no_errors);
deny:
  thd->security_ctx= backup_ctx;
  return TRUE;
}


/****************************************************************************
  Handle GRANT commands
****************************************************************************/


/*
  Return 1 if we are allowed to create new users
  the logic here is: INSERT_ACL is sufficient.
  It's also a requirement in opt_safe_user_create,
  otherwise CREATE_USER_ACL is enough.
*/

static bool test_if_create_new_users(THD *thd)
{
  Security_context *sctx= thd->security_ctx;
  bool create_new_users= MY_TEST(sctx->master_access & INSERT_ACL) ||
                         (!opt_safe_user_create &&
                          MY_TEST(sctx->master_access & CREATE_USER_ACL));
  if (!create_new_users)
  {
    TABLE_LIST tl;
    ulong db_access;
    tl.init_one_table(C_STRING_WITH_LEN("mysql"),
                      C_STRING_WITH_LEN("user"), "user", TL_WRITE);
    create_new_users= 1;

    db_access=acl_get(sctx->get_host()->ptr(), sctx->get_ip()->ptr(),
                      sctx->priv_user, tl.db, 0);
    if (!(db_access & INSERT_ACL))
    {
      if (check_grant(thd, INSERT_ACL, &tl, FALSE, UINT_MAX, TRUE))
        create_new_users=0;
    }
  }
  return create_new_users;
}


/*
  Store table level and column level grants in the privilege tables

  SYNOPSIS
    mysql_table_grant()
    thd                 Thread handle
    table_list          List of tables to give grant
    user_list           List of users to give grant
    columns             List of columns to give grant
    rights              Table level grant
    revoke_grant        Set to 1 if this is a REVOKE command

  RETURN
    FALSE ok
    TRUE  error
*/

int mysql_table_grant(THD *thd, TABLE_LIST *table_list,
                      List <LEX_USER> &user_list,
                      List <LEX_COLUMN> &columns, ulong rights,
                      bool revoke_grant)
{
  ulong column_priv= 0;
  List_iterator <LEX_USER> str_list (user_list);
  LEX_USER *Str, *tmp_Str;
  TABLE_LIST tables[3];
  bool create_new_users=0;
  const char *db_name, *table_name;
  bool save_binlog_row_based;
  bool transactional_tables;
  DBUG_ENTER("mysql_table_grant");

  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0),
             "--skip-grant-tables");        /* purecov: inspected */
    DBUG_RETURN(TRUE);                      /* purecov: inspected */
  }
  if (rights & ~TABLE_ACLS)
  {
    my_message(ER_ILLEGAL_GRANT_FOR_TABLE, ER(ER_ILLEGAL_GRANT_FOR_TABLE),
               MYF(0));
    DBUG_RETURN(TRUE);
  }

  if (!revoke_grant)
  {
    if (columns.elements)
    {
      class LEX_COLUMN *column;
      List_iterator <LEX_COLUMN> column_iter(columns);

      if (open_normal_and_derived_tables(thd, table_list, 0))
        DBUG_RETURN(TRUE);

      while ((column = column_iter++))
      {
        uint unused_field_idx= NO_CACHED_FIELD_INDEX;
        TABLE_LIST *dummy;
        Field *f=find_field_in_table_ref(thd, table_list, column->column.ptr(),
                                         column->column.length(),
                                         column->column.ptr(), NULL, NULL,
                                         NULL, TRUE, FALSE,
                                         &unused_field_idx, FALSE, &dummy);
        if (f == (Field*)0)
        {
          my_error(ER_BAD_FIELD_ERROR, MYF(0),
                   column->column.c_ptr(), table_list->alias);
          DBUG_RETURN(TRUE);
        }
        if (f == (Field *)-1)
          DBUG_RETURN(TRUE);
        column_priv|= column->rights;
      }
      close_mysql_tables(thd);
    }
    else
    {
      if (!(rights & CREATE_ACL))
      {
        char buf[FN_REFLEN + 1];
        build_table_filename(buf, sizeof(buf) - 1, table_list->db,
                             table_list->table_name, reg_ext, 0);
        fn_format(buf, buf, "", "", MY_UNPACK_FILENAME  | MY_RESOLVE_SYMLINKS |
                                    MY_RETURN_REAL_PATH | MY_APPEND_EXT);
        if (access(buf,F_OK))
        {
          my_error(ER_NO_SUCH_TABLE, MYF(0), table_list->db, table_list->alias);
          DBUG_RETURN(TRUE);
        }
      }
      if (table_list->grant.want_privilege)
      {
        char command[128];
        get_privilege_desc(command, sizeof(command),
                           table_list->grant.want_privilege);
        my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
                 command, thd->security_ctx->priv_user,
                 thd->security_ctx->host_or_ip, table_list->alias);
        DBUG_RETURN(-1);
      }
    }
  }

  /* open the mysql.tables_priv and mysql.columns_priv tables */

  tables[0].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("user"), "user", TL_WRITE);
  tables[1].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("tables_priv"),
                           "tables_priv", TL_WRITE);
  tables[2].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("columns_priv"),
                           "columns_priv", TL_WRITE);
  tables[0].next_local= tables[0].next_global= tables+1;
  /* Don't open column table if we don't need it ! */
  if (column_priv || (revoke_grant && ((rights & COL_ACLS) || columns.elements)))
    tables[1].next_local= tables[1].next_global= tables+2;

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
  */
  if ((save_binlog_row_based= thd->is_current_stmt_binlog_format_row()))
    thd->clear_current_stmt_binlog_format_row();

#ifdef HAVE_REPLICATION
  /*
    GRANT and REVOKE are applied the slave in/exclusion rules as they are
    some kind of updates to the mysql.% tables.
  */
  if (thd->slave_thread && rpl_filter->is_on())
  {
    /*
      The tables must be marked "updating" so that tables_ok() takes them into
      account in tests.
    */
    tables[0].updating= tables[1].updating= tables[2].updating= 1;
    if (!(thd->sp_runtime_ctx || rpl_filter->tables_ok(0, tables)))
    {
      /* Restore the state of binlog format */
      DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
      if (save_binlog_row_based)
        thd->set_current_stmt_binlog_format_row();
      DBUG_RETURN(FALSE);
    }
  }
#endif /* HAVE_REPLICATION */

  /* 
    The lock api is depending on the thd->lex variable which needs to be
    re-initialized.
  */
  Query_tables_list backup;
  thd->lex->reset_n_backup_query_tables_list(&backup);
  /*
    Restore Query_tables_list::sql_command value, which was reset
    above, as the code writing query to the binary log assumes that
    this value corresponds to the statement being executed.
  */
  thd->lex->sql_command= backup.sql_command;
  if (open_and_lock_tables(thd, tables, FALSE, MYSQL_LOCK_IGNORE_TIMEOUT))
  {                                             // Should never happen
    /* Restore the state of binlog format */
    DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
    thd->lex->restore_backup_query_tables_list(&backup);
    if (save_binlog_row_based)
      thd->set_current_stmt_binlog_format_row();
    DBUG_RETURN(TRUE);                          /* purecov: deadcode */
  }

  transactional_tables= (tables[0].table->file->has_transactions() ||
                         tables[1].table->file->has_transactions() ||
                         (tables[2].table &&
                          tables[2].table->file->has_transactions()));

  if (!revoke_grant)
    create_new_users= test_if_create_new_users(thd);
  bool result= FALSE;
  bool is_partial_execution= false;
  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);
  MEM_ROOT *old_root= thd->mem_root;
  thd->mem_root= &memex;
  grant_version++;

  while ((tmp_Str = str_list++))
  {
    int error;
    bool is_user_applied= true;
    GRANT_TABLE *grant_table;
    if (!(Str= get_current_user(thd, tmp_Str)))
    {
      result= TRUE;
      continue;
    }

    /*
      No User, but a password?
      They did GRANT ... TO CURRENT_USER() IDENTIFIED BY ... !
      Get the current user, and shallow-copy the new password to them!
    */
    if (!tmp_Str->user.str && tmp_Str->password.str)
      Str->password= tmp_Str->password;
    
    /* Create user if needed */
    error=replace_user_table(thd, tables[0].table, Str,
                             0, revoke_grant, create_new_users,
                             MY_TEST(thd->variables.sql_mode &
                                     MODE_NO_AUTO_CREATE_USER));
    if (error)
    {
      result= TRUE;                             // Remember error
      continue;                                 // Add next user
    }

    db_name= table_list->get_db_name();
    thd->add_to_binlog_accessed_dbs(db_name); // collecting db:s for MTS
    table_name= table_list->get_table_name();

    /* Find/create cached table grant */
    grant_table= table_hash_search(Str->host.str, NullS, db_name,
                                   Str->user.str, table_name, 1);
    if (!grant_table)
    {
      if (revoke_grant)
      {
        my_error(ER_NONEXISTING_TABLE_GRANT, MYF(0),
                 Str->user.str, Str->host.str, table_list->table_name);
        result= TRUE;
        continue;
      }
      grant_table = new GRANT_TABLE (Str->host.str, db_name,
                                     Str->user.str, table_name,
                                     rights,
                                     column_priv);
      if (!grant_table ||
        my_hash_insert(&column_priv_hash,(uchar*) grant_table))
      {
        result= TRUE;                           /* purecov: deadcode */
        continue;                               /* purecov: deadcode */
      }
    }

    /* If revoke_grant, calculate the new column privilege for tables_priv */
    if (revoke_grant)
    {
      class LEX_COLUMN *column;
      List_iterator <LEX_COLUMN> column_iter(columns);
      GRANT_COLUMN *grant_column;

      /* Fix old grants */
      while ((column = column_iter++))
      {
        grant_column = column_hash_search(grant_table,
                                          column->column.ptr(),
                                          column->column.length());
        if (grant_column)
          grant_column->rights&= ~(column->rights | rights);
      }
      /* scan trough all columns to get new column grant */
      column_priv= 0;
      for (uint idx=0 ; idx < grant_table->hash_columns.records ; idx++)
      {
        grant_column= (GRANT_COLUMN*)
          my_hash_element(&grant_table->hash_columns, idx);
        grant_column->rights&= ~rights;         // Fix other columns
        column_priv|= grant_column->rights;
      }
    }
    else
    {
      column_priv|= grant_table->cols;
    }


    /* update table and columns */

    if (replace_table_table(thd, grant_table, tables[1].table, *Str,
                            db_name, table_name,
                            rights, column_priv, revoke_grant))
    {
      /* Should only happen if table is crashed */
      result= TRUE;                            /* purecov: deadcode */
      is_user_applied= false;
    }
    else if (tables[2].table)
    {
      if ((replace_column_table(grant_table, tables[2].table, *Str,
                                columns,
                                db_name, table_name,
                                rights, revoke_grant)))
      {
        result= TRUE;
        is_user_applied= false;
      }
    }
    if (is_user_applied)
      is_partial_execution= true;
  }
  thd->mem_root= old_root;
  mysql_mutex_unlock(&acl_cache->lock);

  /*
    We only log "complete" successful commands, because partially
    failed REVOKE/GRANTS that fail because of insufficient privileges
    on the master, will succeed on the slave due to SQL thread SUPER
    privilege. Even though replication will stop (the error code from
    the master will mismatch the error code on the slave), the
    operation will already be executed (thence revoking or granting
    additional privileges on the slave).
    Before ACLs are changed to execute fully or none at all, when
    some error happens, write an incident if one or more users are
    granted/revoked successfully (it has a partial execution), a
    warning if no user is granted/revoked successfully.
  */
  if (result)
  {
    if (is_partial_execution)
    {
      const char* err_msg= "REVOKE/GRANT failed while storing table level "
                           "and column level grants in the privilege tables.";
      mysql_bin_log.write_incident(thd, true /* need_lock_log=true */,
                                   err_msg);
    }
    else
      sql_print_warning("Did not write failed '%s' into binary log while "
                        "storing table level and column level grants in "
                        "the privilege tables.", thd->query().str);
  }
  else
    result= result |
            write_bin_log(thd, FALSE, thd->query().str, thd->query().length,
                          transactional_tables);

  mysql_rwlock_unlock(&LOCK_grant);

  result|= acl_trans_commit_and_close_tables(thd);

  if (!result) /* success */
  {
    acl_notify_htons(thd, thd->query().str, thd->query().length);
    my_ok(thd);
  }

  thd->lex->restore_backup_query_tables_list(&backup);
  /* Restore the state of binlog format */
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  if (save_binlog_row_based)
    thd->set_current_stmt_binlog_format_row();
  DBUG_RETURN(result);
}


/**
  Store routine level grants in the privilege tables

  @param thd Thread handle
  @param table_list List of routines to give grant
  @param is_proc Is this a list of procedures?
  @param user_list List of users to give grant
  @param rights Table level grant
  @param revoke_grant Is this is a REVOKE command?

  @return
    @retval FALSE Success.
    @retval TRUE An error occurred.
*/

bool mysql_routine_grant(THD *thd, TABLE_LIST *table_list, bool is_proc,
                         List <LEX_USER> &user_list, ulong rights,
                         bool revoke_grant, bool write_to_binlog)
{
  List_iterator <LEX_USER> str_list (user_list);
  LEX_USER *Str, *tmp_Str;
  TABLE_LIST tables[2];
  bool create_new_users=0, result=0;
  const char *db_name, *table_name;
  bool save_binlog_row_based;
  bool transactional_tables;
  DBUG_ENTER("mysql_routine_grant");

  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0),
             "--skip-grant-tables");
    DBUG_RETURN(TRUE);
  }
  if (rights & ~PROC_ACLS)
  {
    my_message(ER_ILLEGAL_GRANT_FOR_TABLE, ER(ER_ILLEGAL_GRANT_FOR_TABLE),
               MYF(0));
    DBUG_RETURN(TRUE);
  }

  if (!revoke_grant)
  {
    if (sp_exist_routines(thd, table_list, is_proc))
      DBUG_RETURN(TRUE);
  }

  /* open the mysql.user and mysql.procs_priv tables */

  tables[0].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("user"), "user", TL_WRITE);
  tables[1].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("procs_priv"), "procs_priv", TL_WRITE);
  tables[0].next_local= tables[0].next_global= tables+1;

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
  */
  if ((save_binlog_row_based= thd->is_current_stmt_binlog_format_row()))
    thd->clear_current_stmt_binlog_format_row();

#ifdef HAVE_REPLICATION
  /*
    GRANT and REVOKE are applied the slave in/exclusion rules as they are
    some kind of updates to the mysql.% tables.
  */
  if (thd->slave_thread && rpl_filter->is_on())
  {
    /*
      The tables must be marked "updating" so that tables_ok() takes them into
      account in tests.
    */
    tables[0].updating= tables[1].updating= 1;
    if (!(thd->sp_runtime_ctx || rpl_filter->tables_ok(0, tables)))
    {
      /* Restore the state of binlog format */
      DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
      if (save_binlog_row_based)
        thd->set_current_stmt_binlog_format_row();
      DBUG_RETURN(FALSE);
    }
  }
#endif /* HAVE_REPLICATION */

  if (open_and_lock_tables(thd, tables, FALSE, MYSQL_LOCK_IGNORE_TIMEOUT))
  {                                             // Should never happen
    /* Restore the state of binlog format */
    DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
    if (save_binlog_row_based)
      thd->set_current_stmt_binlog_format_row();
    DBUG_RETURN(TRUE);
  }

  transactional_tables= (tables[0].table->file->has_transactions() ||
                         tables[1].table->file->has_transactions());

  if (!revoke_grant)
    create_new_users= test_if_create_new_users(thd);
  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);
  MEM_ROOT *old_root= thd->mem_root;
  thd->mem_root= &memex;

  DBUG_PRINT("info",("now time to iterate and add users"));

  bool is_partial_execution= false;
  while ((tmp_Str= str_list++))
  {
    int error;
    GRANT_NAME *grant_name;
    if (!(Str= get_current_user(thd, tmp_Str)))
    {
      result= TRUE;
      continue;
    }
    
    /* Create user if needed */
    error=replace_user_table(thd, tables[0].table, Str,
                             0, revoke_grant, create_new_users,
                             MY_TEST(thd->variables.sql_mode &
                                     MODE_NO_AUTO_CREATE_USER));
    if (error)
    {
      result= TRUE;                             // Remember error
      continue;                                 // Add next user
    }

    db_name= table_list->db;
    if (write_to_binlog)
      thd->add_to_binlog_accessed_dbs(db_name);
    table_name= table_list->table_name;
    grant_name= routine_hash_search(Str->host.str, NullS, db_name,
                                    Str->user.str, table_name, is_proc, 1);
    if (!grant_name)
    {
      if (revoke_grant)
      {
        my_error(ER_NONEXISTING_PROC_GRANT, MYF(0),
                 Str->user.str, Str->host.str, table_name);
        result= TRUE;
        continue;
      }
      grant_name= new GRANT_NAME(Str->host.str, db_name,
                                 Str->user.str, table_name,
                                 rights, TRUE);
      if (!grant_name ||
        my_hash_insert(is_proc ?
                       &proc_priv_hash : &func_priv_hash,(uchar*) grant_name))
      {
        result= TRUE;
        continue;
      }
    }

    if (replace_routine_table(thd, grant_name, tables[1].table, *Str,
                              db_name, table_name, is_proc, rights, 
                              revoke_grant) != 0)
    {
      result= TRUE;
      continue;
    }
    is_partial_execution= true;
  }
  thd->mem_root= old_root;
  mysql_mutex_unlock(&acl_cache->lock);

  if (write_to_binlog)
  {
    /*
      Before ACLs are changed to execute fully or none at all, when
      some error happens, write an incident if one or more users are
      granted/revoked successfully (it has a partial execution), a
      warning if no user is granted/revoked successfully.
    */
    if (result)
    {
      if (is_partial_execution)
      {
        const char* err_msg= "REVOKE/GRANT failed while storing routine "
                             "level grants in the privilege tables.";
        mysql_bin_log.write_incident(thd, true /* need_lock_log=true */,
                                     err_msg);
      }
      else
        sql_print_warning("Did not write failed '%s' into binary log while "
                          "storing routine level grants in the privilege "
                          "tables.", thd->query().str);
    }
    else
    {
      /*
        For performance reasons, we don't rewrite the query if we don't have to.
        If that was the case, write the original query.
      */
      if (!thd->rewritten_query.length())
      {
        if (write_bin_log(thd, false, thd->query().str, thd->query().length,
                          transactional_tables))
          result= TRUE;
      }
      else
      {
        if (write_bin_log(thd, false,
                          thd->rewritten_query.c_ptr_safe(),
                          thd->rewritten_query.length(),
                          transactional_tables))
          result= TRUE;
      }
    }
  }

  mysql_rwlock_unlock(&LOCK_grant);

  result|= acl_trans_commit_and_close_tables(thd);

  if (write_to_binlog && !result)
    acl_notify_htons(thd, thd->query().str, thd->query().length);

  /* Restore the state of binlog format */
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  if (save_binlog_row_based)
    thd->set_current_stmt_binlog_format_row();
 
  DBUG_RETURN(result);
}


bool mysql_grant(THD *thd, const char *db, List <LEX_USER> &list,
                 ulong rights, bool revoke_grant, bool is_proxy)
{
  List_iterator <LEX_USER> str_list (list);
  LEX_USER *Str, *tmp_Str, *proxied_user= NULL;
  char tmp_db[NAME_LEN+1];
  bool create_new_users=0;
  TABLE_LIST tables[2];
  bool save_binlog_row_based;
  bool transactional_tables;
  DBUG_ENTER("mysql_grant");
  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0),
             "--skip-grant-tables");    /* purecov: tested */
    DBUG_RETURN(TRUE);                  /* purecov: tested */
  }

  if (lower_case_table_names && db)
  {
    my_stpnmov(tmp_db,db,NAME_LEN);
    tmp_db[NAME_LEN]= '\0';
    my_casedn_str(files_charset_info, tmp_db);
    db=tmp_db;
  }

  if (is_proxy)
  {
    DBUG_ASSERT(!db);
    proxied_user= str_list++;
  }

  /* open the mysql.user and mysql.db or mysql.proxies_priv tables */
  tables[0].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("user"), "user", TL_WRITE);
  if (is_proxy)

    tables[1].init_one_table(C_STRING_WITH_LEN("mysql"),
                             C_STRING_WITH_LEN("proxies_priv"),
                             "proxies_priv", 
                             TL_WRITE);
  else
    tables[1].init_one_table(C_STRING_WITH_LEN("mysql"),
                             C_STRING_WITH_LEN("db"), 
                             "db", 
                             TL_WRITE);
  tables[0].next_local= tables[0].next_global= tables+1;

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
  */
  if ((save_binlog_row_based= thd->is_current_stmt_binlog_format_row()))
    thd->clear_current_stmt_binlog_format_row();

#ifdef HAVE_REPLICATION
  /*
    GRANT and REVOKE are applied the slave in/exclusion rules as they are
    some kind of updates to the mysql.% tables.
  */
  if (thd->slave_thread && rpl_filter->is_on())
  {
    /*
      The tables must be marked "updating" so that tables_ok() takes them into
      account in tests.
    */
    tables[0].updating= tables[1].updating= 1;
    if (!(thd->sp_runtime_ctx || rpl_filter->tables_ok(0, tables)))
    {
      /* Restore the state of binlog format */
      DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
      if (save_binlog_row_based)
        thd->set_current_stmt_binlog_format_row();
      DBUG_RETURN(FALSE);
    }
  }
#endif /*HAVE_REPLICATION */

  if (open_and_lock_tables(thd, tables, FALSE, MYSQL_LOCK_IGNORE_TIMEOUT))
  {                                     // This should never happen
    /* Restore the state of binlog format */
    DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
    if (save_binlog_row_based)
      thd->set_current_stmt_binlog_format_row();
    DBUG_RETURN(TRUE);                  /* purecov: deadcode */
  }

  transactional_tables= (tables[0].table->file->has_transactions() ||
                         tables[1].table->file->has_transactions());

  if (!revoke_grant)
    create_new_users= test_if_create_new_users(thd);

  /* go through users in user_list */
  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);
  grant_version++;

  int result= 0;
  bool is_partial_execution= false;
  while ((tmp_Str = str_list++))
  {
    bool is_user_applied= true;
    if (!(Str= get_current_user(thd, tmp_Str)))
    {
      result= TRUE;
      continue;
    }

    /*
      No User, but a password?
      They did GRANT ... TO CURRENT_USER() IDENTIFIED BY ... !
      Get the current user, and shallow-copy the new password to them!
    */
    if (!tmp_Str->user.str && tmp_Str->password.str)
      Str->password= tmp_Str->password;
 
    if (replace_user_table(thd, tables[0].table, Str,
                           (!db ? rights : 0), revoke_grant, create_new_users,
                           MY_TEST(thd->variables.sql_mode &
                                   MODE_NO_AUTO_CREATE_USER)))
    {
      result= -1;
      is_user_applied= false;
    }
    else if (db)
    {
      ulong db_rights= rights & DB_ACLS;
      if (db_rights  == rights)
      {
        if (replace_db_table(tables[1].table, db, *Str, db_rights,
                             revoke_grant))
        {
          result= -1;
          is_user_applied= false;
        }
      }
      else
      {
        my_error(ER_WRONG_USAGE, MYF(0), "DB GRANT", "GLOBAL PRIVILEGES");
        result= -1;
        is_user_applied= false;
      }
      thd->add_to_binlog_accessed_dbs(db);
    }
    else if (is_proxy)
    {
      if (replace_proxies_priv_table (thd, tables[1].table, Str, proxied_user,
                                    rights & GRANT_ACL ? TRUE : FALSE, 
                                    revoke_grant))
      {
        result= -1;
        is_user_applied= false;
      }
    }
    if (is_user_applied)
      is_partial_execution= true;
  }
  mysql_mutex_unlock(&acl_cache->lock);

  /*
    Before ACLs are changed to execute fully or none at all, when
    some error happens, write an incident if one or more users are
    granted/revoked successfully (it has a partial execution), a
    warning if no user is granted/revoked successfully.
  */
  if (result)
  {
    if (is_partial_execution)
    {
      const char* err_msg= "REVOKE/GRANT failed while granting/revoking "
                           "privileges in databases.";
      mysql_bin_log.write_incident(thd, true /* need_lock_log=true */,
                                   err_msg);
    }
    else
      sql_print_warning("Did not write failed '%s' into binary log while "
                        "granting/revoking privileges in databases.",
                        thd->query().str);
  }
  else
  {
    if (thd->rewritten_query.length())
      result= result |
          write_bin_log(thd, FALSE,
                        thd->rewritten_query.c_ptr_safe(),
                        thd->rewritten_query.length(),
                        transactional_tables);
    else
      result= result |
        write_bin_log(thd, FALSE, thd->query().str, thd->query().length,
                            transactional_tables);
  }

  mysql_rwlock_unlock(&LOCK_grant);

  result|= acl_trans_commit_and_close_tables(thd);
  
  if (!result)
  {
    acl_notify_htons(thd, thd->query().str, thd->query().length);
    my_ok(thd);
  }

  /* Restore the state of binlog format */
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  if (save_binlog_row_based)
    thd->set_current_stmt_binlog_format_row();

  DBUG_RETURN(result);
}




/**
  @brief Check table level grants

  @param thd          Thread handler
  @param want_access  Bits of privileges user needs to have.
  @param tables       List of tables to check. The user should have
                      'want_access' to all tables in list.
  @param any_combination_will_do TRUE if it's enough to have any privilege for
    any combination of the table columns.
  @param number       Check at most this number of tables.
  @param no_errors    TRUE if no error should be sent directly to the client.

  If table->grant.want_privilege != 0 then the requested privileges where
  in the set of COL_ACLS but access was not granted on the table level. As
  a consequence an extra check of column privileges is required.

  Specifically if this function returns FALSE the user has some kind of
  privilege on a combination of columns in each table.

  This function is usually preceeded by check_access which establish the
  User-, Db- and Host access rights.

  @see check_access
  @see check_table_access

  @note This functions assumes that either number of tables to be inspected
     by it is limited explicitly (i.e. is is not UINT_MAX) or table list
     used and thd->lex->query_tables_own_last value correspond to each
     other (the latter should be either 0 or point to next_global member
     of one of elements of this table list).

   @return Access status
     @retval FALSE Access granted; But column privileges might need to be
      checked.
     @retval TRUE The user did not have the requested privileges on any of the
      tables.

*/

bool check_grant(THD *thd, ulong want_access, TABLE_LIST *tables,
                 bool any_combination_will_do, uint number, bool no_errors)
{
  TABLE_LIST *tl;
  TABLE_LIST *const first_not_own_table= thd->lex->first_not_own_table();
  Security_context *sctx= thd->security_ctx;
  uint i;
  ulong orig_want_access= want_access;
  DBUG_ENTER("check_grant");
  DBUG_ASSERT(number > 0);

  /*
    Walk through the list of tables that belong to the query and save the
    requested access (orig_want_privilege) to be able to use it when
    checking access rights to the underlying tables of a view. Our grant
    system gradually eliminates checked bits from want_privilege and thus
    after all checks are done we can no longer use it.
    The check that first_not_own_table is not reached is for the case when
    the given table list refers to the list for prelocking (contains tables
    of other queries). For simple queries first_not_own_table is 0.
  */
  for (i= 0, tl= tables;
       i < number  && tl != first_not_own_table;
       tl= tl->next_global, i++)
  {
    TABLE_LIST *const t_ref=
      tl->correspondent_table ? tl->correspondent_table : tl;
    /*
      Save a copy of the privileges without the SHOW_VIEW_ACL attribute.
      It will be checked during making view.
    */
    t_ref->grant.orig_want_privilege= (want_access & ~SHOW_VIEW_ACL);
  }

  mysql_rwlock_rdlock(&LOCK_grant);
  for (tl= tables;
       tl && number-- && tl != first_not_own_table;
       tl= tl->next_global)
  {
    TABLE_LIST *const t_ref=
      tl->correspondent_table ? tl->correspondent_table : tl;
    sctx = MY_TEST(t_ref->security_ctx) ? t_ref->security_ctx : thd->security_ctx;

    const ACL_internal_table_access *access=
      get_cached_table_access(&t_ref->grant.m_internal,
                              t_ref->get_db_name(),
                              t_ref->get_table_name());

    if (access)
    {
      switch(access->check(orig_want_access, &t_ref->grant.privilege))
      {
      case ACL_INTERNAL_ACCESS_GRANTED:
        /*
           Grant all access to the table to skip column checks.
           Depend on the controls in the P_S table itself.
        */
        t_ref->grant.privilege|= TMP_TABLE_ACLS;
        t_ref->grant.want_privilege= 0;
        continue;
      case ACL_INTERNAL_ACCESS_DENIED:
        goto err;
      case ACL_INTERNAL_ACCESS_CHECK_GRANT:
        break;
      }
    }

    want_access= orig_want_access;
    want_access&= ~sctx->master_access;
    if (!want_access)
      continue;                                 // ok

    if (!(~t_ref->grant.privilege & want_access) ||
        t_ref->is_anonymous_derived_table() || t_ref->schema_table)
    {
      /*
        It is subquery in the FROM clause. VIEW set t_ref->derived after
        table opening, but this function always called before table opening.
      */
      if (!t_ref->referencing_view)
      {
        /*
          If it's a temporary table created for a subquery in the FROM
          clause, or an INFORMATION_SCHEMA table, drop the request for
          a privilege.
        */
        t_ref->grant.want_privilege= 0;
      }
      continue;
    }

    if (is_temporary_table(t_ref))
    {
      /*
        If this table list element corresponds to a pre-opened temporary
        table skip checking of all relevant table-level privileges for it.
        Note that during creation of temporary table we still need to check
        if user has CREATE_TMP_ACL.
      */
      t_ref->grant.privilege|= TMP_TABLE_ACLS;
      t_ref->grant.want_privilege= 0;
      continue;
    }

    GRANT_TABLE *grant_table= table_hash_search(sctx->get_host()->ptr(),
                                                sctx->get_ip()->ptr(),
                                                t_ref->get_db_name(),
                                                sctx->priv_user,
                                                t_ref->get_table_name(),
                                                FALSE);

    if (!grant_table)
    {
      want_access &= ~t_ref->grant.privilege;
      goto err;                                 // No grants
    }

    /*
      For SHOW COLUMNS, SHOW INDEX it is enough to have some
      privileges on any column combination on the table.
    */
    if (any_combination_will_do)
      continue;

    t_ref->grant.grant_table= grant_table; // Remember for column test
    t_ref->grant.version= grant_version;
    t_ref->grant.privilege|= grant_table->privs;
    t_ref->grant.want_privilege=
      ((want_access & COL_ACLS) & ~t_ref->grant.privilege);

    if (!(~t_ref->grant.privilege & want_access))
      continue;

    if (want_access & ~(grant_table->cols | t_ref->grant.privilege))
    {
      want_access &= ~(grant_table->cols | t_ref->grant.privilege);
      goto err;                                 // impossible
    }
  }
  mysql_rwlock_unlock(&LOCK_grant);
  DBUG_RETURN(FALSE);

err:
  mysql_rwlock_unlock(&LOCK_grant);
  if (!no_errors)                               // Not a silent skip of table
  {
    char command[128];
    get_privilege_desc(command, sizeof(command), want_access);
    my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
             command,
             sctx->priv_user,
             sctx->host_or_ip,
             tl ? tl->get_table_name() : "unknown");
  }
  DBUG_RETURN(TRUE);
}


/*
  Check column rights in given security context

  SYNOPSIS
    check_grant_column()
    thd                  thread handler
    grant                grant information structure
    db_name              db name
    table_name           table  name
    name                 column name
    length               column name length
    sctx                 security context

  RETURN
    FALSE OK
    TRUE  access denied
*/

bool check_grant_column(THD *thd, GRANT_INFO *grant,
                        const char *db_name, const char *table_name,
                        const char *name, size_t length,  Security_context *sctx)
{
  GRANT_TABLE *grant_table;
  GRANT_COLUMN *grant_column;
  ulong want_access= grant->want_privilege & ~grant->privilege;
  DBUG_ENTER("check_grant_column");
  DBUG_PRINT("enter", ("table: %s  want_access: %lu", table_name, want_access));

  if (!want_access)
    DBUG_RETURN(0);                             // Already checked

  mysql_rwlock_rdlock(&LOCK_grant);

  /* reload table if someone has modified any grants */

  if (grant->version != grant_version)
  {
    grant->grant_table=
      table_hash_search(sctx->get_host()->ptr(), sctx->get_ip()->ptr(),
                        db_name, sctx->priv_user,
                        table_name, 0);         /* purecov: inspected */
    grant->version= grant_version;              /* purecov: inspected */
  }
  if (!(grant_table= grant->grant_table))
    goto err;                                   /* purecov: deadcode */

  grant_column=column_hash_search(grant_table, name, length);
  if (grant_column && !(~grant_column->rights & want_access))
  {
    mysql_rwlock_unlock(&LOCK_grant);
    DBUG_RETURN(0);
  }

err:
  mysql_rwlock_unlock(&LOCK_grant);
  char command[128];
  get_privilege_desc(command, sizeof(command), want_access);
  my_error(ER_COLUMNACCESS_DENIED_ERROR, MYF(0),
           command,
           sctx->priv_user,
           sctx->host_or_ip,
           name,
           table_name);
  DBUG_RETURN(1);
}


/*
  Check the access right to a column depending on the type of table.

  SYNOPSIS
    check_column_grant_in_table_ref()
    thd              thread handler
    table_ref        table reference where to check the field
    name             name of field to check
    length           length of name

  DESCRIPTION
    Check the access rights to a column depending on the type of table
    reference where the column is checked. The function provides a
    generic interface to check column access rights that hides the
    heterogeneity of the column representation - whether it is a view
    or a stored table colum.

  RETURN
    FALSE OK
    TRUE  access denied
*/

bool check_column_grant_in_table_ref(THD *thd, TABLE_LIST * table_ref,
                                     const char *name, size_t length)
{
  GRANT_INFO *grant;
  const char *db_name;
  const char *table_name;
  Security_context *sctx= MY_TEST(table_ref->security_ctx) ?
                          table_ref->security_ctx : thd->security_ctx;

  if (table_ref->view || table_ref->field_translation)
  {
    /* View or derived information schema table. */
    ulong view_privs;
    grant= &(table_ref->grant);
    db_name= table_ref->view_db.str;
    table_name= table_ref->view_name.str;
    if (table_ref->belong_to_view && 
        thd->lex->sql_command == SQLCOM_SHOW_FIELDS)
    {
      view_privs= get_column_grant(thd, grant, db_name, table_name, name);
      if (view_privs & VIEW_ANY_ACL)
      {
        table_ref->belong_to_view->allowed_show= TRUE;
        return FALSE;
      }
      table_ref->belong_to_view->allowed_show= FALSE;
      my_message(ER_VIEW_NO_EXPLAIN, ER(ER_VIEW_NO_EXPLAIN), MYF(0));
      return TRUE;
    }
  }
  else if (table_ref->nested_join)
  {
    bool error= FALSE;
    List_iterator<TABLE_LIST> it(table_ref->nested_join->join_list);
    TABLE_LIST *table;
    while (!error && (table= it++))
      error|= check_column_grant_in_table_ref(thd, table, name, length);
    return error;
  }
  else
  {
    /* Normal or temporary table. */
    TABLE *table= table_ref->table;
    grant= &(table->grant);
    db_name= table->s->db.str;
    table_name= table->s->table_name.str;
  }

  if (grant->want_privilege)
    return check_grant_column(thd, grant, db_name, table_name, name,
                              length, sctx);
  else
    return FALSE;

}


/** 
  @brief check if a query can access a set of columns

  @param  thd  the current thread
  @param  want_access_arg  the privileges requested
  @param  fields an iterator over the fields of a table reference.
  @return Operation status
    @retval 0 Success
    @retval 1 Falure
  @details This function walks over the columns of a table reference 
   The columns may originate from different tables, depending on the kind of
   table reference, e.g. join, view.
   For each table it will retrieve the grant information and will use it
   to check the required access privileges for the fields requested from it.
*/    
bool check_grant_all_columns(THD *thd, ulong want_access_arg, 
                             Field_iterator_table_ref *fields)
{
  Security_context *sctx= thd->security_ctx;
  ulong want_access= want_access_arg;
  const char *table_name= NULL;

  const char* db_name; 
  GRANT_INFO *grant;
  /* Initialized only to make gcc happy */
  GRANT_TABLE *grant_table= NULL;
  /* 
     Flag that gets set if privilege checking has to be performed on column
     level.
  */
  bool using_column_privileges= FALSE;

  mysql_rwlock_rdlock(&LOCK_grant);

  for (; !fields->end_of_fields(); fields->next())
  {
    const char *field_name= fields->name();

    if (table_name != fields->get_table_name())
    {
      table_name= fields->get_table_name();
      db_name= fields->get_db_name();
      grant= fields->grant();
      /* get a fresh one for each table */
      want_access= want_access_arg & ~grant->privilege;
      if (want_access)
      {
        /* reload table if someone has modified any grants */
        if (grant->version != grant_version)
        {
          grant->grant_table=
            table_hash_search(sctx->get_host()->ptr(), sctx->get_ip()->ptr(),
                              db_name, sctx->priv_user,
                              table_name, 0);   /* purecov: inspected */
          grant->version= grant_version;        /* purecov: inspected */
        }

        grant_table= grant->grant_table;
        DBUG_ASSERT (grant_table);
      }
    }

    if (want_access)
    {
      GRANT_COLUMN *grant_column=
        column_hash_search(grant_table, field_name, strlen(field_name));
      if (grant_column)
        using_column_privileges= TRUE;
      if (!grant_column || (~grant_column->rights & want_access))
        goto err;
    }
  }
  mysql_rwlock_unlock(&LOCK_grant);
  return 0;

err:
  mysql_rwlock_unlock(&LOCK_grant);

  char command[128];
  get_privilege_desc(command, sizeof(command), want_access);
  /*
    Do not give an error message listing a column name unless the user has
    privilege to see all columns.
  */
  if (using_column_privileges)
    my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
             command, sctx->priv_user,
             sctx->host_or_ip, table_name); 
  else
    my_error(ER_COLUMNACCESS_DENIED_ERROR, MYF(0),
             command,
             sctx->priv_user,
             sctx->host_or_ip,
             fields->name(),
             table_name);
  return 1;
}


static bool check_grant_db_routine(THD *thd, const char *db, HASH *hash)
{
  Security_context *sctx= thd->security_ctx;

  for (uint idx= 0; idx < hash->records; ++idx)
  {
    GRANT_NAME *item= (GRANT_NAME*) my_hash_element(hash, idx);

    if (strcmp(item->user, sctx->priv_user) == 0 &&
        strcmp(item->db, db) == 0 &&
        item->host.compare_hostname(sctx->get_host()->ptr(),
                                    sctx->get_ip()->ptr()))
    {
      return FALSE;
    }
  }

  return TRUE;
}


/*
  Check if a user has the right to access a database
  Access is accepted if he has a grant for any table/routine in the database
  Return 1 if access is denied
*/

bool check_grant_db(THD *thd,const char *db)
{
  Security_context *sctx= thd->security_ctx;
  char helping [NAME_LEN+USERNAME_LENGTH+2];
  uint len;
  bool error= TRUE;
  size_t copy_length;

  copy_length= (size_t) (strlen(sctx->priv_user ? sctx->priv_user : "") +
                 strlen(db ? db : "")) + 1; /* Added 1 at the end to avoid  
                                               buffer overflow at strmov()*/

  /*
    Make sure that my_stpcpy() operations do not result in buffer overflow.
  */
  if (copy_length >= (NAME_LEN+USERNAME_LENGTH+2))
    return 1;

  len= (uint) (my_stpcpy(my_stpcpy(helping, sctx->priv_user) + 1, db) - helping) + 1;

  mysql_rwlock_rdlock(&LOCK_grant);

  for (uint idx=0 ; idx < column_priv_hash.records ; idx++)
  {
    GRANT_TABLE *grant_table= (GRANT_TABLE*)
      my_hash_element(&column_priv_hash,
                      idx);
    if (len < grant_table->key_length &&
        !memcmp(grant_table->hash_key,helping,len) &&
        grant_table->host.compare_hostname(sctx->get_host()->ptr(),
                                           sctx->get_ip()->ptr()))
    {
      error= FALSE; /* Found match. */
      break;
    }
  }

  if (error)
    error= check_grant_db_routine(thd, db, &proc_priv_hash) &&
           check_grant_db_routine(thd, db, &func_priv_hash);

  mysql_rwlock_unlock(&LOCK_grant);

  return error;
}


/****************************************************************************
  Check routine level grants

  SYNPOSIS
   bool check_grant_routine()
   thd          Thread handler
   want_access  Bits of privileges user needs to have
   procs        List of routines to check. The user should have 'want_access'
   is_proc      True if the list is all procedures, else functions
   no_errors    If 0 then we write an error. The error is sent directly to
                the client

   RETURN
     0  ok
     1  Error: User did not have the requested privielges
****************************************************************************/

bool check_grant_routine(THD *thd, ulong want_access,
                         TABLE_LIST *procs, bool is_proc, bool no_errors)
{
  TABLE_LIST *table;
  Security_context *sctx= thd->security_ctx;
  char *user= sctx->priv_user;
  char *host= sctx->priv_host;
  DBUG_ENTER("check_grant_routine");

  want_access&= ~sctx->master_access;
  if (!want_access)
    DBUG_RETURN(0);                             // ok

  mysql_rwlock_rdlock(&LOCK_grant);
  for (table= procs; table; table= table->next_global)
  {
    GRANT_NAME *grant_proc;
    if ((grant_proc= routine_hash_search(host, sctx->get_ip()->ptr(), table->db, user,
                                         table->table_name, is_proc, 0)))
      table->grant.privilege|= grant_proc->privs;

    if (want_access & ~table->grant.privilege)
    {
      want_access &= ~table->grant.privilege;
      goto err;
    }
  }
  mysql_rwlock_unlock(&LOCK_grant);
  DBUG_RETURN(0);
err:
  mysql_rwlock_unlock(&LOCK_grant);
  if (!no_errors)
  {
    char buff[1024];
    const char *command="";
    if (table)
      strxmov(buff, table->db, ".", table->table_name, NullS);
    if (want_access & EXECUTE_ACL)
      command= "execute";
    else if (want_access & ALTER_PROC_ACL)
      command= "alter routine";
    else if (want_access & GRANT_ACL)
      command= "grant";
    my_error(ER_PROCACCESS_DENIED_ERROR, MYF(0),
             command, user, host, table ? buff : "unknown");
  }
  DBUG_RETURN(1);
}


/*
  Check if routine has any of the 
  routine level grants
  
  SYNPOSIS
   bool    check_routine_level_acl()
   thd          Thread handler
   db           Database name
   name         Routine name

  RETURN
   0            Ok 
   1            error
*/

bool check_routine_level_acl(THD *thd, const char *db, const char *name, 
                             bool is_proc)
{
  bool no_routine_acl= 1;
  GRANT_NAME *grant_proc;
  Security_context *sctx= thd->security_ctx;
  mysql_rwlock_rdlock(&LOCK_grant);
  if ((grant_proc= routine_hash_search(sctx->priv_host,
                                       sctx->get_ip()->ptr(), db,
                                       sctx->priv_user,
                                       name, is_proc, 0)))
    no_routine_acl= !(grant_proc->privs & SHOW_PROC_ACLS);
  mysql_rwlock_unlock(&LOCK_grant);
  return no_routine_acl;
}


/*****************************************************************************
  Functions to retrieve the grant for a table/column  (for SHOW functions)
*****************************************************************************/

ulong get_table_grant(THD *thd, TABLE_LIST *table)
{
  ulong privilege;
  Security_context *sctx= thd->security_ctx;
  const char *db = table->db ? table->db : thd->db().str;
  GRANT_TABLE *grant_table;

  mysql_rwlock_rdlock(&LOCK_grant);
#ifdef EMBEDDED_LIBRARY
  grant_table= NULL;
#else
  grant_table= table_hash_search(sctx->get_host()->ptr(),
                                 sctx->get_ip()->ptr(), db, sctx->priv_user,
                                 table->table_name, 0);
#endif /* EMBEDDED_LIBRARY */
  table->grant.grant_table=grant_table; // Remember for column test
  table->grant.version=grant_version;
  if (grant_table)
    table->grant.privilege|= grant_table->privs;
  privilege= table->grant.privilege;
  mysql_rwlock_unlock(&LOCK_grant);
  return privilege;
}


/*
  Determine the access priviliges for a field.

  SYNOPSIS
    get_column_grant()
    thd         thread handler
    grant       grants table descriptor
    db_name     name of database that the field belongs to
    table_name  name of table that the field belongs to
    field_name  name of field

  DESCRIPTION
    The procedure may also modify: grant->grant_table and grant->version.

  RETURN
    The access priviliges for the field db_name.table_name.field_name
*/

ulong get_column_grant(THD *thd, GRANT_INFO *grant,
                       const char *db_name, const char *table_name,
                       const char *field_name)
{
  GRANT_TABLE *grant_table;
  GRANT_COLUMN *grant_column;
  ulong priv;

  mysql_rwlock_rdlock(&LOCK_grant);
  /* reload table if someone has modified any grants */
  if (grant->version != grant_version)
  {
    Security_context *sctx= thd->security_ctx;
    grant->grant_table=
      table_hash_search(sctx->get_host()->ptr(), sctx->get_ip()->ptr(),
                        db_name, sctx->priv_user,
                        table_name, 0);         /* purecov: inspected */
    grant->version= grant_version;              /* purecov: inspected */
  }

  if (!(grant_table= grant->grant_table))
    priv= grant->privilege;
  else
  {
    grant_column= column_hash_search(grant_table, field_name,
                                     strlen(field_name));
    if (!grant_column)
      priv= (grant->privilege | grant_table->privs);
    else
      priv= (grant->privilege | grant_table->privs | grant_column->rights);
  }
  mysql_rwlock_unlock(&LOCK_grant);
  return priv;
}


/* Help function for mysql_show_grants */

static void add_user_option(String *grant, ulong value, const char *name)
{
  if (value)
  {
    char buff[22], *p; // just as in int2str
    grant->append(' ');
    grant->append(name, strlen(name));
    grant->append(' ');
    p=int10_to_str(value, buff, 10);
    grant->append(buff,p-buff);
  }
}


static int show_routine_grants(THD* thd, LEX_USER *lex_user, HASH *hash,
                               const char *type, int typelen,
                               char *buff, int buffsize)
{
  uint counter, index;
  int error= 0;
  Protocol *protocol= thd->protocol;
  /* Add routine access */
  for (index=0 ; index < hash->records ; index++)
  {
    const char *user, *host;
    GRANT_NAME *grant_proc= (GRANT_NAME*) my_hash_element(hash, index);

    if (!(user=grant_proc->user))
      user= "";
    if (!(host= grant_proc->host.get_host()))
      host= "";

    /*
      We do not make SHOW GRANTS case-sensitive here (like REVOKE),
      but make it case-insensitive because that's the way they are
      actually applied, and showing fewer privileges than are applied
      would be wrong from a security point of view.
    */

    if (!strcmp(lex_user->user.str,user) &&
        !my_strcasecmp(system_charset_info, lex_user->host.str, host))
    {
      ulong proc_access= grant_proc->privs;
      if (proc_access != 0)
      {
        String global(buff, buffsize, system_charset_info);
        ulong test_access= proc_access & ~GRANT_ACL;

        global.length(0);
        global.append(STRING_WITH_LEN("GRANT "));

        if (!test_access)
          global.append(STRING_WITH_LEN("USAGE"));
        else
        {
          /* Add specific procedure access */
          int found= 0;
          ulong j;

          for (counter= 0, j= SELECT_ACL; j <= PROC_ACLS; counter++, j<<= 1)
          {
            if (test_access & j)
            {
              if (found)
                global.append(STRING_WITH_LEN(", "));
              found= 1;
              global.append(command_array[counter],command_lengths[counter]);
            }
          }
        }
        global.append(STRING_WITH_LEN(" ON "));
        global.append(type,typelen);
        global.append(' ');
        append_identifier(thd, &global, grant_proc->db,
                          strlen(grant_proc->db));
        global.append('.');
        append_identifier(thd, &global, grant_proc->tname,
                          strlen(grant_proc->tname));
        global.append(STRING_WITH_LEN(" TO '"));
        global.append(lex_user->user.str, lex_user->user.length,
                      system_charset_info);
        global.append(STRING_WITH_LEN("'@'"));
        // host and lex_user->host are equal except for case
        global.append(host, strlen(host), system_charset_info);
        global.append('\'');
        if (proc_access & GRANT_ACL)
          global.append(STRING_WITH_LEN(" WITH GRANT OPTION"));
        protocol->prepare_for_resend();
        protocol->store(global.ptr(),global.length(),global.charset());
        if (protocol->write())
        {
          error= -1;
          break;
        }
      }
    }
  }
  return error;
}


static bool
show_proxy_grants(THD *thd, LEX_USER *user, char *buff, size_t buffsize)
{
  Protocol *protocol= thd->protocol;
  int error= 0;

  for (ACL_PROXY_USER *proxy= acl_proxy_users->begin();
       proxy != acl_proxy_users->end(); ++proxy)
  {
    if (proxy->granted_on(user->host.str, user->user.str))
    {
      String global(buff, buffsize, system_charset_info);
      global.length(0);
      proxy->print_grant(&global);
      protocol->prepare_for_resend();
      protocol->store(global.ptr(), global.length(), global.charset());
      if (protocol->write())
      {
        error= -1;
        break;
      }
    }
  }
  return error;
}


/*
  Make a clear-text version of the requested privilege.
*/

void get_privilege_desc(char *to, uint max_length, ulong access)
{
  uint pos;
  char *start=to;
  DBUG_ASSERT(max_length >= 30);                // For end ', ' removal

  if (access)
  {
    max_length--;                               // Reserve place for end-zero
    for (pos=0 ; access ; pos++, access>>=1)
    {
      if ((access & 1) &&
          command_lengths[pos] + (uint) (to-start) < max_length)
      {
        to= my_stpcpy(to, command_array[pos]);
        *to++= ',';
        *to++= ' ';
      }
    }
    to--;                                       // Remove end ' '
    to--;                                       // Remove end ','
  }
  *to=0;
}


/*
  SHOW GRANTS;  Send grants for a user to the client

  IMPLEMENTATION
   Send to client grant-like strings depicting user@host privileges
*/

bool mysql_show_grants(THD *thd,LEX_USER *lex_user)
{
  ulong want_access;
  uint counter,index;
  int  error = 0;
  ACL_USER *acl_user= NULL;
  ACL_DB *acl_db;
  char buff[1024];
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("mysql_show_grants");

  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    DBUG_RETURN(TRUE);
  }

  mysql_rwlock_rdlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  acl_user= find_acl_user(lex_user->host.str, lex_user->user.str, TRUE);
  if (!acl_user)
  {
    mysql_mutex_unlock(&acl_cache->lock);
    mysql_rwlock_unlock(&LOCK_grant);

    my_error(ER_NONEXISTING_GRANT, MYF(0),
             lex_user->user.str, lex_user->host.str);
    DBUG_RETURN(TRUE);
  }

  Item_string *field=new Item_string("",0,&my_charset_latin1);
  List<Item> field_list;
  field->max_length=1024;
  strxmov(buff,"Grants for ",lex_user->user.str,"@",
          lex_user->host.str,NullS);
  field->item_name.set(buff);
  field_list.push_back(field);
  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
  {
    mysql_mutex_unlock(&acl_cache->lock);
    mysql_rwlock_unlock(&LOCK_grant);

    DBUG_RETURN(TRUE);
  }

  /* Add first global access grants */
  {
    String global(buff,sizeof(buff),system_charset_info);
    global.length(0);
    global.append(STRING_WITH_LEN("GRANT "));

    want_access= acl_user->access;
    if (test_all_bits(want_access, (GLOBAL_ACLS & ~ GRANT_ACL)))
      global.append(STRING_WITH_LEN("ALL PRIVILEGES"));
    else if (!(want_access & ~GRANT_ACL))
      global.append(STRING_WITH_LEN("USAGE"));
    else
    {
      bool found=0;
      ulong j,test_access= want_access & ~GRANT_ACL;
      for (counter=0, j = SELECT_ACL;j <= GLOBAL_ACLS;counter++,j <<= 1)
      {
        if (test_access & j)
        {
          if (found)
            global.append(STRING_WITH_LEN(", "));
          found=1;
          global.append(command_array[counter],command_lengths[counter]);
        }
      }
    }
    global.append (STRING_WITH_LEN(" ON *.* TO '"));
    global.append(lex_user->user.str, lex_user->user.length,
                  system_charset_info);
    global.append (STRING_WITH_LEN("'@'"));
    global.append(lex_user->host.str,lex_user->host.length,
                  system_charset_info);
    global.append ('\'');    
#if defined(HAVE_OPENSSL)
    if (acl_user->plugin.str == sha256_password_plugin_name.str &&
        acl_user->auth_string.length > 0)
    {
      global.append(STRING_WITH_LEN(" IDENTIFIED BY PASSWORD"));
      if ((thd->security_ctx->master_access & SUPER_ACL) == SUPER_ACL)
      {
        global.append(" \'");
        global.append((const char *) &acl_user->auth_string.str[0]);
        global.append('\'');
      }
    }
    else
#endif /* HAVE_OPENSSL */
    if (acl_user->salt_len)
    {
      global.append(STRING_WITH_LEN(" IDENTIFIED BY PASSWORD"));
      char passwd_buff[SCRAMBLED_PASSWORD_CHAR_LENGTH+1];

      DBUG_ASSERT(acl_user->salt_len == SCRAMBLE_LENGTH);
      make_password_from_salt(passwd_buff, acl_user->salt);
      if ((thd->security_ctx->master_access & SUPER_ACL) == SUPER_ACL)
      {
        global.append(" \'");
        global.append(passwd_buff);
        global.append('\'');
      }
    }
    /* "show grants" SSL related stuff */
    if (acl_user->ssl_type == SSL_TYPE_ANY)
      global.append(STRING_WITH_LEN(" REQUIRE SSL"));
    else if (acl_user->ssl_type == SSL_TYPE_X509)
      global.append(STRING_WITH_LEN(" REQUIRE X509"));
    else if (acl_user->ssl_type == SSL_TYPE_SPECIFIED)
    {
      int ssl_options = 0;
      global.append(STRING_WITH_LEN(" REQUIRE "));
      if (acl_user->x509_issuer)
      {
        ssl_options++;
        global.append(STRING_WITH_LEN("ISSUER \'"));
        global.append(acl_user->x509_issuer,strlen(acl_user->x509_issuer));
        global.append('\'');
      }
      if (acl_user->x509_subject)
      {
        if (ssl_options++)
          global.append(' ');
        global.append(STRING_WITH_LEN("SUBJECT \'"));
        global.append(acl_user->x509_subject,strlen(acl_user->x509_subject),
                      system_charset_info);
        global.append('\'');
      }
      if (acl_user->ssl_cipher)
      {
        if (ssl_options++)
          global.append(' ');
        global.append(STRING_WITH_LEN("CIPHER '"));
        global.append(acl_user->ssl_cipher,strlen(acl_user->ssl_cipher),
                      system_charset_info);
        global.append('\'');
      }
    }
    if ((want_access & GRANT_ACL) ||
        (acl_user->user_resource.questions ||
         acl_user->user_resource.updates ||
         acl_user->user_resource.conn_per_hour ||
         acl_user->user_resource.user_conn))
    {
      global.append(STRING_WITH_LEN(" WITH"));
      if (want_access & GRANT_ACL)
        global.append(STRING_WITH_LEN(" GRANT OPTION"));
      add_user_option(&global, acl_user->user_resource.questions,
                      "MAX_QUERIES_PER_HOUR");
      add_user_option(&global, acl_user->user_resource.updates,
                      "MAX_UPDATES_PER_HOUR");
      add_user_option(&global, acl_user->user_resource.conn_per_hour,
                      "MAX_CONNECTIONS_PER_HOUR");
      add_user_option(&global, acl_user->user_resource.user_conn,
                      "MAX_USER_CONNECTIONS");
    }
    protocol->prepare_for_resend();
    protocol->store(global.ptr(),global.length(),global.charset());
    if (protocol->write())
    {
      error= -1;
      goto end;
    }
  }

  /* Add database access */
  for (acl_db= acl_dbs->begin(); acl_db != acl_dbs->end(); ++acl_db)
  {
    const char *user, *host;

    if (!(user=acl_db->user))
      user= "";
    if (!(host=acl_db->host.get_host()))
      host= "";

    /*
      We do not make SHOW GRANTS case-sensitive here (like REVOKE),
      but make it case-insensitive because that's the way they are
      actually applied, and showing fewer privileges than are applied
      would be wrong from a security point of view.
    */

    if (!strcmp(lex_user->user.str,user) &&
        !my_strcasecmp(system_charset_info, lex_user->host.str, host))
    {
      want_access=acl_db->access;
      if (want_access)
      {
        String db(buff,sizeof(buff),system_charset_info);
        db.length(0);
        db.append(STRING_WITH_LEN("GRANT "));

        if (test_all_bits(want_access,(DB_ACLS & ~GRANT_ACL)))
          db.append(STRING_WITH_LEN("ALL PRIVILEGES"));
        else if (!(want_access & ~GRANT_ACL))
          db.append(STRING_WITH_LEN("USAGE"));
        else
        {
          int found=0, cnt;
          ulong j,test_access= want_access & ~GRANT_ACL;
          for (cnt=0, j = SELECT_ACL; j <= DB_ACLS; cnt++,j <<= 1)
          {
            if (test_access & j)
            {
              if (found)
                db.append(STRING_WITH_LEN(", "));
              found = 1;
              db.append(command_array[cnt],command_lengths[cnt]);
            }
          }
        }
        db.append (STRING_WITH_LEN(" ON "));
        append_identifier(thd, &db, acl_db->db, strlen(acl_db->db));
        db.append (STRING_WITH_LEN(".* TO '"));
        db.append(lex_user->user.str, lex_user->user.length,
                  system_charset_info);
        db.append (STRING_WITH_LEN("'@'"));
        // host and lex_user->host are equal except for case
        db.append(host, strlen(host), system_charset_info);
        db.append ('\'');
        if (want_access & GRANT_ACL)
          db.append(STRING_WITH_LEN(" WITH GRANT OPTION"));
        protocol->prepare_for_resend();
        protocol->store(db.ptr(),db.length(),db.charset());
        if (protocol->write())
        {
          error= -1;
          goto end;
        }
      }
    }
  }

  /* Add table & column access */
  for (index=0 ; index < column_priv_hash.records ; index++)
  {
    const char *user, *host;
    GRANT_TABLE *grant_table= (GRANT_TABLE*)
      my_hash_element(&column_priv_hash, index);

    if (!(user=grant_table->user))
      user= "";
    if (!(host= grant_table->host.get_host()))
      host= "";

    /*
      We do not make SHOW GRANTS case-sensitive here (like REVOKE),
      but make it case-insensitive because that's the way they are
      actually applied, and showing fewer privileges than are applied
      would be wrong from a security point of view.
    */

    if (!strcmp(lex_user->user.str,user) &&
        !my_strcasecmp(system_charset_info, lex_user->host.str, host))
    {
      ulong table_access= grant_table->privs;
      if ((table_access | grant_table->cols) != 0)
      {
        String global(buff, sizeof(buff), system_charset_info);
        ulong test_access= (table_access | grant_table->cols) & ~GRANT_ACL;

        global.length(0);
        global.append(STRING_WITH_LEN("GRANT "));

        if (test_all_bits(table_access, (TABLE_ACLS & ~GRANT_ACL)))
          global.append(STRING_WITH_LEN("ALL PRIVILEGES"));
        else if (!test_access)
          global.append(STRING_WITH_LEN("USAGE"));
        else
        {
          /* Add specific column access */
          int found= 0;
          ulong j;

          for (counter= 0, j= SELECT_ACL; j <= TABLE_ACLS; counter++, j<<= 1)
          {
            if (test_access & j)
            {
              if (found)
                global.append(STRING_WITH_LEN(", "));
              found= 1;
              global.append(command_array[counter],command_lengths[counter]);

              if (grant_table->cols)
              {
                uint found_col= 0;
                for (uint col_index=0 ;
                     col_index < grant_table->hash_columns.records ;
                     col_index++)
                {
                  GRANT_COLUMN *grant_column = (GRANT_COLUMN*)
                    my_hash_element(&grant_table->hash_columns,col_index);
                  if (grant_column->rights & j)
                  {
                    if (!found_col)
                    {
                      found_col= 1;
                      /*
                        If we have a duplicated table level privilege, we
                        must write the access privilege name again.
                      */
                      if (table_access & j)
                      {
                        global.append(STRING_WITH_LEN(", "));
                        global.append(command_array[counter],
                                      command_lengths[counter]);
                      }
                      global.append(STRING_WITH_LEN(" ("));
                    }
                    else
                      global.append(STRING_WITH_LEN(", "));
                    global.append(grant_column->column,
                                  grant_column->key_length,
                                  system_charset_info);
                  }
                }
                if (found_col)
                  global.append(')');
              }
            }
          }
        }
        global.append(STRING_WITH_LEN(" ON "));
        append_identifier(thd, &global, grant_table->db,
                          strlen(grant_table->db));
        global.append('.');
        append_identifier(thd, &global, grant_table->tname,
                          strlen(grant_table->tname));
        global.append(STRING_WITH_LEN(" TO '"));
        global.append(lex_user->user.str, lex_user->user.length,
                      system_charset_info);
        global.append(STRING_WITH_LEN("'@'"));
        // host and lex_user->host are equal except for case
        global.append(host, strlen(host), system_charset_info);
        global.append('\'');
        if (table_access & GRANT_ACL)
          global.append(STRING_WITH_LEN(" WITH GRANT OPTION"));
        protocol->prepare_for_resend();
        protocol->store(global.ptr(),global.length(),global.charset());
        if (protocol->write())
        {
          error= -1;
          break;
        }
      }
    }
  }

  if (show_routine_grants(thd, lex_user, &proc_priv_hash, 
                          STRING_WITH_LEN("PROCEDURE"), buff, sizeof(buff)))
  {
    error= -1;
    goto end;
  }

  if (show_routine_grants(thd, lex_user, &func_priv_hash,
                          STRING_WITH_LEN("FUNCTION"), buff, sizeof(buff)))
  {
    error= -1;
    goto end;
  }

  if (show_proxy_grants(thd, lex_user, buff, sizeof(buff)))
  {
    error= -1;
    goto end;
  }

end:
  mysql_mutex_unlock(&acl_cache->lock);
  mysql_rwlock_unlock(&LOCK_grant);

  my_eof(thd);
  DBUG_RETURN(error);
}


/*
  Revoke all privileges from a list of users.

  SYNOPSIS
    mysql_revoke_all()
    thd                         The current thread.
    list                        The users to revoke all privileges from.

  RETURN
    > 0         Error. Error message already sent.
    0           OK.
    < 0         Error. Error message not yet sent.
*/

bool mysql_revoke_all(THD *thd,  List <LEX_USER> &list)
{
  uint revoked, is_proc;
  int result;
  ACL_DB *acl_db;
  TABLE_LIST tables[GRANT_TABLES];
  bool save_binlog_row_based;
  bool transactional_tables;
  DBUG_ENTER("mysql_revoke_all");

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
  */
  if ((save_binlog_row_based= thd->is_current_stmt_binlog_format_row()))
    thd->clear_current_stmt_binlog_format_row();

  if ((result= open_grant_tables(thd, tables, &transactional_tables)))
  {
    /* Restore the state of binlog format */
    DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
    if (save_binlog_row_based)
      thd->set_current_stmt_binlog_format_row();
    DBUG_RETURN(result != 1);
  }

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  LEX_USER *lex_user, *tmp_lex_user;
  List_iterator <LEX_USER> user_list(list);

  bool is_partial_execution= false;
  while ((tmp_lex_user= user_list++))
  {
    bool is_user_applied= true;
    if (!(lex_user= get_current_user(thd, tmp_lex_user)))
    {
      result= -1;
      continue;
    }  
    if (!find_acl_user(lex_user->host.str, lex_user->user.str, TRUE))
    {
      result= -1;
      continue;
    }

    if (replace_user_table(thd, tables[0].table,
                           lex_user, ~(ulong) 0, 1, 0, 0))
    {
      result= -1;
      continue;
    }

    /* Remove db access privileges */
    /*
      Because acl_dbs and column_priv_hash shrink and may re-order
      as privileges are removed, removal occurs in a repeated loop
      until no more privileges are revoked.
     */
    do
    {
      for (revoked= 0, acl_db= acl_dbs->begin(); acl_db != acl_dbs->end(); )
      {
        const char *user,*host;

        if (!(user=acl_db->user))
          user= "";
        if (!(host=acl_db->host.get_host()))
          host= "";

        if (!strcmp(lex_user->user.str,user) &&
            !strcmp(lex_user->host.str, host))
        {
          if (!replace_db_table(tables[1].table, acl_db->db, *lex_user,
                                ~(ulong)0, 1))
          {
            /*
              Don't increment loop variable as replace_db_table deleted the
              current element in acl_dbs.
             */
            revoked= 1;
            continue;
          }
          result= -1; // Something went wrong
          is_user_applied= false;
        }
        ++acl_db;
      }
    } while (revoked);

    /* Remove column access */
    do
    {
      uint counter;
      for (counter= 0, revoked= 0 ; counter < column_priv_hash.records ; )
      {
        const char *user,*host;
        GRANT_TABLE *grant_table=
          (GRANT_TABLE*) my_hash_element(&column_priv_hash, counter);
        if (!(user=grant_table->user))
          user= "";
        if (!(host=grant_table->host.get_host()))
          host= "";

        if (!strcmp(lex_user->user.str,user) &&
            !strcmp(lex_user->host.str, host))
        {
          if (replace_table_table(thd,grant_table,tables[2].table,*lex_user,
                                  grant_table->db,
                                  grant_table->tname,
                                  ~(ulong)0, 0, 1))
          {
            result= -1;
            is_user_applied= false;
          }
          else
          {
            if (!grant_table->cols)
            {
              revoked= 1;
              continue;
            }
            List<LEX_COLUMN> columns;
            if (!replace_column_table(grant_table,tables[3].table, *lex_user,
                                      columns,
                                      grant_table->db,
                                      grant_table->tname,
                                      ~(ulong)0, 1))
            {
              revoked= 1;
              continue;
            }
            result= -1;
            is_user_applied= false;
          }
        }
        counter++;
      }
    } while (revoked);

    /* Remove procedure access */
    for (is_proc=0; is_proc<2; is_proc++) do {
      HASH *hash= is_proc ? &proc_priv_hash : &func_priv_hash;
      uint counter;
      for (counter= 0, revoked= 0 ; counter < hash->records ; )
      {
        const char *user,*host;
        GRANT_NAME *grant_proc= (GRANT_NAME*) my_hash_element(hash, counter);
        if (!(user=grant_proc->user))
          user= "";
        if (!(host=grant_proc->host.get_host()))
          host= "";

        if (!strcmp(lex_user->user.str,user) &&
            !strcmp(lex_user->host.str, host))
        {
          if (replace_routine_table(thd,grant_proc,tables[4].table,*lex_user,
                                  grant_proc->db,
                                  grant_proc->tname,
                                  is_proc,
                                  ~(ulong)0, 1) == 0)
          {
            revoked= 1;
            continue;
          }
          result= -1;  // Something went wrong
          is_user_applied= false;
        }
        counter++;
      }
    } while (revoked);
    if (is_user_applied)
      is_partial_execution= true;
  }

  mysql_mutex_unlock(&acl_cache->lock);

  if (result)
    my_message(ER_REVOKE_GRANTS, ER(ER_REVOKE_GRANTS), MYF(0));

  /*
    Before ACLs are changed to execute fully or none at all, when
    some error happens, write an incident if one or more users are
    revoked successfully (it has a partial execution), a warning
    if no user is granted/revoked successfully.
  */
  if (result)
  {
    if (is_partial_execution)
    {
      const char* err_msg= "REVOKE failed while revoking all_privileges "
                           "from a list of users.";
      mysql_bin_log.write_incident(thd, true /* need_lock_log=true */,
                                   err_msg);
    }
    else
      sql_print_warning("Did not write failed '%s' into binary log while "
                        "revoking all_privileges from a list of users.",
                        thd->query().str);
  }
  else
  {
    result= result |
      write_bin_log(thd, FALSE, thd->query().str, thd->query().length,
                    transactional_tables);
  }

  mysql_rwlock_unlock(&LOCK_grant);

  result|= acl_trans_commit_and_close_tables(thd);

  if (!result)
    acl_notify_htons(thd, thd->query().str, thd->query().length);

  /* Restore the state of binlog format */
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  if (save_binlog_row_based)
    thd->set_current_stmt_binlog_format_row();

  DBUG_RETURN(result);
}


/**
  If the defining user for a routine does not exist, then the ACL lookup
  code should raise two errors which we should intercept.  We convert the more
  descriptive error into a warning, and consume the other.

  If any other errors are raised, then we set a flag that should indicate
  that there was some failure we should complain at a higher level.
*/
class Silence_routine_definer_errors : public Internal_error_handler
{
public:
  Silence_routine_definer_errors()
    : is_grave(FALSE)
  {}

  virtual ~Silence_routine_definer_errors()
  {}

  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char* sqlstate,
                                Sql_condition::enum_severity_level *level,
                                const char* msg,
                                Sql_condition ** cond_hdl);

  bool has_errors() { return is_grave; }

private:
  bool is_grave;
};

bool
Silence_routine_definer_errors::handle_condition(
  THD *thd,
  uint sql_errno,
  const char*,
  Sql_condition::enum_severity_level *level,
  const char* msg,
  Sql_condition ** cond_hdl)
{
  *cond_hdl= NULL;
  if (*level == Sql_condition::SL_ERROR)
  {
    switch (sql_errno)
    {
      case ER_NONEXISTING_PROC_GRANT:
        /* Convert the error into a warning. */
        push_warning(thd, Sql_condition::SL_WARNING,
                     sql_errno, msg);
        return TRUE;
      default:
        is_grave= TRUE;
    }
  }

  return FALSE;
}


/**
  Revoke privileges for all users on a stored procedure.  Use an error handler
  that converts errors about missing grants into warnings.

  @param
    thd                         The current thread.
  @param
    db                          DB of the stored procedure
  @param
    name                        Name of the stored procedure

  @retval
    0           OK.
  @retval
    < 0         Error. Error message not yet sent.
*/

bool sp_revoke_privileges(THD *thd, const char *sp_db, const char *sp_name,
                          bool is_proc)
{
  uint counter, revoked;
  int result;
  TABLE_LIST tables[GRANT_TABLES];
  HASH *hash= is_proc ? &proc_priv_hash : &func_priv_hash;
  Silence_routine_definer_errors error_handler;
  bool save_binlog_row_based;
  bool not_used;
  DBUG_ENTER("sp_revoke_privileges");

  if ((result= open_grant_tables(thd, tables, &not_used)))
    DBUG_RETURN(result != 1);

  /* Be sure to pop this before exiting this scope! */
  thd->push_internal_handler(&error_handler);

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
  */
  if ((save_binlog_row_based= thd->is_current_stmt_binlog_format_row()))
    thd->clear_current_stmt_binlog_format_row();

  /* Remove procedure access */
  do
  {
    for (counter= 0, revoked= 0 ; counter < hash->records ; )
    {
      GRANT_NAME *grant_proc= (GRANT_NAME*) my_hash_element(hash, counter);
      if (!my_strcasecmp(&my_charset_utf8_bin, grant_proc->db, sp_db) &&
          !my_strcasecmp(system_charset_info, grant_proc->tname, sp_name))
      {
        LEX_USER lex_user;
        lex_user.user.str= grant_proc->user;
        lex_user.user.length= strlen(grant_proc->user);
        lex_user.host.str= (char*) (grant_proc->host.get_host() ?
          grant_proc->host.get_host() : "");
        lex_user.host.length= grant_proc->host.get_host() ?
          strlen(grant_proc->host.get_host()) : 0;

        if (replace_routine_table(thd,grant_proc,tables[4].table,lex_user,
                                  grant_proc->db, grant_proc->tname,
                                  is_proc, ~(ulong)0, 1) == 0)
        {
          revoked= 1;
          continue;
        }
      }
      counter++;
    }
  } while (revoked);

  mysql_mutex_unlock(&acl_cache->lock);
  mysql_rwlock_unlock(&LOCK_grant);

  result= acl_trans_commit_and_close_tables(thd);

  thd->pop_internal_handler();

  /* Restore the state of binlog format */
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  if (save_binlog_row_based)
    thd->set_current_stmt_binlog_format_row();

  DBUG_RETURN(error_handler.has_errors() || result);
}


/**
  Grant EXECUTE,ALTER privilege for a stored procedure

  @param thd The current thread.
  @param sp_db
  @param sp_name
  @param is_proc

  @return
    @retval FALSE Success
    @retval TRUE An error occured. Error message not yet sent.
*/

bool sp_grant_privileges(THD *thd, const char *sp_db, const char *sp_name,
                         bool is_proc)
{
  Security_context *sctx= thd->security_ctx;
  LEX_USER *combo;
  TABLE_LIST tables[1];
  List<LEX_USER> user_list;
  bool result;
  ACL_USER *au;
  Dummy_error_handler error_handler;
  DBUG_ENTER("sp_grant_privileges");

  if (!(combo=(LEX_USER*) thd->alloc(sizeof(st_lex_user))))
    DBUG_RETURN(TRUE);

  combo->user.str= sctx->user;

  mysql_mutex_lock(&acl_cache->lock);

  if ((au= find_acl_user(combo->host.str=(char*)sctx->host_or_ip,combo->user.str,FALSE)))
    goto found_acl;
  if ((au= find_acl_user(combo->host.str=(char*)sctx->get_host()->ptr(),
                                                combo->user.str, FALSE)))
    goto found_acl;
  if ((au= find_acl_user(combo->host.str=(char*)sctx->get_ip()->ptr(),
                                                combo->user.str, FALSE)))
    goto found_acl;
  if((au= find_acl_user(combo->host.str=(char*)"%", combo->user.str, FALSE)))
    goto found_acl;

  mysql_mutex_unlock(&acl_cache->lock);
  DBUG_RETURN(TRUE);

 found_acl:
  mysql_mutex_unlock(&acl_cache->lock);

  memset(tables, 0, sizeof(TABLE_LIST));
  user_list.empty();

  tables->db= (char*)sp_db;
  tables->table_name= tables->alias= (char*)sp_name;

  thd->make_lex_string(&combo->user,
                       combo->user.str, strlen(combo->user.str), 0);
  thd->make_lex_string(&combo->host,
                       combo->host.str, strlen(combo->host.str), 0);

  combo->password= EMPTY_CSTR;
  combo->plugin= EMPTY_CSTR;
  combo->auth= EMPTY_CSTR;
  combo->uses_identified_by_clause= false;
  combo->uses_identified_with_clause= false;
  combo->uses_identified_by_password_clause= false;
  combo->uses_authentication_string_clause= false;

  if (user_list.push_back(combo))
    DBUG_RETURN(TRUE);

  thd->lex->ssl_type= SSL_TYPE_NOT_SPECIFIED;
  thd->lex->ssl_cipher= thd->lex->x509_subject= thd->lex->x509_issuer= 0;
  memset(&thd->lex->mqh, 0, sizeof(thd->lex->mqh));

  /*
    Only care about whether the operation failed or succeeded
    as all errors will be handled later.
  */
  thd->push_internal_handler(&error_handler);
  result= mysql_routine_grant(thd, tables, is_proc, user_list,
                              DEFAULT_CREATE_PROC_ACLS, FALSE, FALSE);
  thd->pop_internal_handler();
  DBUG_RETURN(result);
}


static bool update_schema_privilege(THD *thd, TABLE *table, char *buff,
                                    const char* db, const char* t_name,
                                    const char* column, size_t col_length,
                                    const char *priv, size_t priv_length,
                                    const char* is_grantable)
{
  int i= 2;
  CHARSET_INFO *cs= system_charset_info;
  restore_record(table, s->default_values);
  table->field[0]->store(buff, strlen(buff), cs);
  table->field[1]->store(STRING_WITH_LEN("def"), cs);
  if (db)
    table->field[i++]->store(db, strlen(db), cs);
  if (t_name)
    table->field[i++]->store(t_name, strlen(t_name), cs);
  if (column)
    table->field[i++]->store(column, col_length, cs);
  table->field[i++]->store(priv, priv_length, cs);
  table->field[i]->store(is_grantable, strlen(is_grantable), cs);
  return schema_table_store_record(thd, table);
}


/*
  fill effective privileges for table

  SYNOPSIS
    fill_effective_table_privileges()
    thd     thread handler
    grant   grants table descriptor
    db      db name
    table   table name
*/

void fill_effective_table_privileges(THD *thd, GRANT_INFO *grant,
                                     const char *db, const char *table)
{
  Security_context *sctx= thd->security_ctx;
  DBUG_ENTER("fill_effective_table_privileges");
  DBUG_PRINT("enter", ("Host: '%s', Ip: '%s', User: '%s', table: `%s`.`%s`",
                       sctx->priv_host, (sctx->get_ip()->length() ?
                       sctx->get_ip()->ptr() : "(NULL)"),
                       (sctx->priv_user ? sctx->priv_user : "(NULL)"),
                       db, table));
  /* --skip-grants */
  if (!initialized)
  {
    DBUG_PRINT("info", ("skip grants"));
    grant->privilege= ~NO_ACCESS;             // everything is allowed
    DBUG_PRINT("info", ("privilege 0x%lx", grant->privilege));
    DBUG_VOID_RETURN;
  }

  /* global privileges */
  grant->privilege= sctx->master_access;

  if (!sctx->priv_user)
  {
    DBUG_PRINT("info", ("privilege 0x%lx", grant->privilege));
    DBUG_VOID_RETURN;                         // it is slave
  }

  /* db privileges */
  grant->privilege|= acl_get(sctx->get_host()->ptr(), sctx->get_ip()->ptr(),
                             sctx->priv_user, db, 0);

  /* table privileges */
  mysql_rwlock_rdlock(&LOCK_grant);
  if (grant->version != grant_version)
  {
    grant->grant_table=
      table_hash_search(sctx->get_host()->ptr(), sctx->get_ip()->ptr(), db,
                        sctx->priv_user,
                        table, 0);              /* purecov: inspected */
    grant->version= grant_version;              /* purecov: inspected */
  }
  if (grant->grant_table != 0)
  {
    grant->privilege|= grant->grant_table->privs;
  }
  mysql_rwlock_unlock(&LOCK_grant);

  DBUG_PRINT("info", ("privilege 0x%lx", grant->privilege));
  DBUG_VOID_RETURN;
}


bool
acl_check_proxy_grant_access(THD *thd, const char *host, const char *user,
                             bool with_grant)
{
  DBUG_ENTER("acl_check_proxy_grant_access");
  DBUG_PRINT("info", ("user=%s host=%s with_grant=%d", user, host, 
                      (int) with_grant));
  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    DBUG_RETURN(1);
  }

  /* replication slave thread can do anything */
  if (thd->slave_thread)
  {
    DBUG_PRINT("info", ("replication slave"));
    DBUG_RETURN(FALSE);
  }

  /*
    one can grant proxy for self to others.
    Security context in THD contains two pairs of (user,host):
    1. (user,host) pair referring to inbound connection.
    2. (priv_user,priv_host) pair obtained from mysql.user table after doing
        authnetication of incoming connection.
    Privileges should be checked wrt (priv_user, priv_host) tuple, because
    (user,host) pair obtained from inbound connection may have different
    values than what is actually stored in mysql.user table and while granting
    or revoking proxy privilege, user is expected to provide entries mentioned
    in mysql.user table.
  */
  if (!strcmp(thd->security_ctx->priv_user, user) &&
      !my_strcasecmp(system_charset_info, host,
                     thd->security_ctx->priv_host))
  {
    DBUG_PRINT("info", ("strcmp (%s, %s) my_casestrcmp (%s, %s) equal", 
                        thd->security_ctx->priv_user, user,
                        host, thd->security_ctx->priv_host));
    DBUG_RETURN(FALSE);
  }

  /* check for matching WITH PROXY rights */
  for (ACL_PROXY_USER *proxy= acl_proxy_users->begin();
       proxy != acl_proxy_users->end(); ++proxy)
  {
    if (proxy->matches(thd->security_ctx->get_host()->ptr(),
                       thd->security_ctx->user,
                       thd->security_ctx->get_ip()->ptr(),
                       user) &&
        proxy->get_with_grant())
    {
      DBUG_PRINT("info", ("found"));
      DBUG_RETURN(FALSE);
    }
  }

  my_error(ER_ACCESS_DENIED_NO_PASSWORD_ERROR, MYF(0),
           thd->security_ctx->user,
           thd->security_ctx->host_or_ip);
  DBUG_RETURN(TRUE);
}


#else /* NO_EMBEDDED_ACCESS_CHECKS */

/****************************************************************************
 Dummy wrappers when we don't have any access checks
****************************************************************************/

bool check_routine_level_acl(THD *thd, const char *db, const char *name,
                             bool is_proc)
{
  return FALSE;
}


#endif /* NO_EMBEDDED_ACCESS_CHECKS */


int fill_schema_user_privileges(THD *thd, TABLE_LIST *tables, Item *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  int error= 0;
  ACL_USER *acl_user;
  ulong want_access;
  char buff[100];
  TABLE *table= tables->table;
  bool no_global_access= check_access(thd, SELECT_ACL, "mysql",
                                      NULL, NULL, 1, 1);
  char *curr_host= thd->security_ctx->priv_host_name();
  DBUG_ENTER("fill_schema_user_privileges");

  if (!initialized)
    DBUG_RETURN(0);
  mysql_mutex_lock(&acl_cache->lock);

  for (acl_user= acl_users->begin(); acl_user != acl_users->end(); ++acl_user)
  {
    const char *user,*host, *is_grantable="YES";
    if (!(user=acl_user->user))
      user= "";
    if (!(host=acl_user->host.get_host()))
      host= "";

    if (no_global_access &&
        (strcmp(thd->security_ctx->priv_user, user) ||
         my_strcasecmp(system_charset_info, curr_host, host)))
      continue;
      
    want_access= acl_user->access;
    if (!(want_access & GRANT_ACL))
      is_grantable= "NO";

    strxmov(buff,"'",user,"'@'",host,"'",NullS);
    if (!(want_access & ~GRANT_ACL))
    {
      if (update_schema_privilege(thd, table, buff, 0, 0, 0, 0,
                                  STRING_WITH_LEN("USAGE"), is_grantable))
      {
        error= 1;
        goto err;
      }
    }
    else
    {
      uint priv_id;
      ulong j,test_access= want_access & ~GRANT_ACL;
      for (priv_id=0, j = SELECT_ACL;j <= GLOBAL_ACLS; priv_id++,j <<= 1)
      {
        if (test_access & j)
        {
          if (update_schema_privilege(thd, table, buff, 0, 0, 0, 0, 
                                      command_array[priv_id],
                                      command_lengths[priv_id], is_grantable))
          {
            error= 1;
            goto err;
          }
        }
      }
    }
  }
err:
  mysql_mutex_unlock(&acl_cache->lock);

  DBUG_RETURN(error);
#else
  return(0);
#endif /* NO_EMBEDDED_ACCESS_CHECKS */
}


int fill_schema_schema_privileges(THD *thd, TABLE_LIST *tables, Item *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  int error= 0;
  ACL_DB *acl_db;
  ulong want_access;
  char buff[100];
  TABLE *table= tables->table;
  bool no_global_access= check_access(thd, SELECT_ACL, "mysql",
                                      NULL, NULL, 1, 1);
  char *curr_host= thd->security_ctx->priv_host_name();
  DBUG_ENTER("fill_schema_schema_privileges");

  if (!initialized)
    DBUG_RETURN(0);
  mysql_mutex_lock(&acl_cache->lock);

  for (acl_db= acl_dbs->begin(); acl_db != acl_dbs->end(); ++acl_db)
  {
    const char *user, *host, *is_grantable="YES";

    if (!(user=acl_db->user))
      user= "";
    if (!(host=acl_db->host.get_host()))
      host= "";

    if (no_global_access &&
        (strcmp(thd->security_ctx->priv_user, user) ||
         my_strcasecmp(system_charset_info, curr_host, host)))
      continue;

    want_access=acl_db->access;
    if (want_access)
    {
      if (!(want_access & GRANT_ACL))
      {
        is_grantable= "NO";
      }
      strxmov(buff,"'",user,"'@'",host,"'",NullS);
      if (!(want_access & ~GRANT_ACL))
      {
        if (update_schema_privilege(thd, table, buff, acl_db->db, 0, 0,
                                    0, STRING_WITH_LEN("USAGE"), is_grantable))
        {
          error= 1;
          goto err;
        }
      }
      else
      {
        int cnt;
        ulong j,test_access= want_access & ~GRANT_ACL;
        for (cnt=0, j = SELECT_ACL; j <= DB_ACLS; cnt++,j <<= 1)
          if (test_access & j)
          {
            if (update_schema_privilege(thd, table, buff, acl_db->db, 0, 0, 0,
                                        command_array[cnt], command_lengths[cnt],
                                        is_grantable))
            {
              error= 1;
              goto err;
            }
          }
      }
    }
  }
err:
  mysql_mutex_unlock(&acl_cache->lock);

  DBUG_RETURN(error);
#else
  return (0);
#endif /* NO_EMBEDDED_ACCESS_CHECKS */
}


int fill_schema_table_privileges(THD *thd, TABLE_LIST *tables, Item *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  int error= 0;
  uint index;
  char buff[100];
  TABLE *table= tables->table;
  bool no_global_access= check_access(thd, SELECT_ACL, "mysql",
                                      NULL, NULL, 1, 1);
  char *curr_host= thd->security_ctx->priv_host_name();
  DBUG_ENTER("fill_schema_table_privileges");

  mysql_rwlock_rdlock(&LOCK_grant);

  for (index=0 ; index < column_priv_hash.records ; index++)
  {
    const char *user, *host, *is_grantable= "YES";
    GRANT_TABLE *grant_table= (GRANT_TABLE*) my_hash_element(&column_priv_hash,
                                                          index);
    if (!(user=grant_table->user))
      user= "";
    if (!(host= grant_table->host.get_host()))
      host= "";

    if (no_global_access &&
        (strcmp(thd->security_ctx->priv_user, user) ||
         my_strcasecmp(system_charset_info, curr_host, host)))
      continue;

    ulong table_access= grant_table->privs;
    if (table_access)
    {
      ulong test_access= table_access & ~GRANT_ACL;
      /*
        We should skip 'usage' privilege on table if
        we have any privileges on column(s) of this table
      */
      if (!test_access && grant_table->cols)
        continue;
      if (!(table_access & GRANT_ACL))
        is_grantable= "NO";

      strxmov(buff, "'", user, "'@'", host, "'", NullS);
      if (!test_access)
      {
        if (update_schema_privilege(thd, table, buff, grant_table->db,
                                    grant_table->tname, 0, 0,
                                    STRING_WITH_LEN("USAGE"), is_grantable))
        {
          error= 1;
          goto err;
        }
      }
      else
      {
        ulong j;
        int cnt;
        for (cnt= 0, j= SELECT_ACL; j <= TABLE_ACLS; cnt++, j<<= 1)
        {
          if (test_access & j)
          {
            if (update_schema_privilege(thd, table, buff, grant_table->db,
                                        grant_table->tname, 0, 0,
                                        command_array[cnt],
                                        command_lengths[cnt], is_grantable))
            {
              error= 1;
              goto err;
            }
          }
        }
      }
    }   
  }
err:
  mysql_rwlock_unlock(&LOCK_grant);

  DBUG_RETURN(error);
#else
  return (0);
#endif /* NO_EMBEDDED_ACCESS_CHECKS */
}


int fill_schema_column_privileges(THD *thd, TABLE_LIST *tables, Item *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  int error= 0;
  uint index;
  char buff[100];
  TABLE *table= tables->table;
  bool no_global_access= check_access(thd, SELECT_ACL, "mysql",
                                      NULL, NULL, 1, 1);
  char *curr_host= thd->security_ctx->priv_host_name();
  DBUG_ENTER("fill_schema_table_privileges");

  mysql_rwlock_rdlock(&LOCK_grant);

  for (index=0 ; index < column_priv_hash.records ; index++)
  {
    const char *user, *host, *is_grantable= "YES";
    GRANT_TABLE *grant_table= (GRANT_TABLE*) my_hash_element(&column_priv_hash,
                                                          index);
    if (!(user=grant_table->user))
      user= "";
    if (!(host= grant_table->host.get_host()))
      host= "";

    if (no_global_access &&
        (strcmp(thd->security_ctx->priv_user, user) ||
         my_strcasecmp(system_charset_info, curr_host, host)))
      continue;

    ulong table_access= grant_table->cols;
    if (table_access != 0)
    {
      if (!(grant_table->privs & GRANT_ACL))
        is_grantable= "NO";

      ulong test_access= table_access & ~GRANT_ACL;
      strxmov(buff, "'", user, "'@'", host, "'", NullS);
      if (!test_access)
        continue;
      else
      {
        ulong j;
        int cnt;
        for (cnt= 0, j= SELECT_ACL; j <= TABLE_ACLS; cnt++, j<<= 1)
        {
          if (test_access & j)
          {
            for (uint col_index=0 ;
                 col_index < grant_table->hash_columns.records ;
                 col_index++)
            {
              GRANT_COLUMN *grant_column = (GRANT_COLUMN*)
                my_hash_element(&grant_table->hash_columns,col_index);
              if ((grant_column->rights & j) && (table_access & j))
              {
                if (update_schema_privilege(thd, table, buff, grant_table->db,
                                            grant_table->tname,
                                            grant_column->column,
                                            grant_column->key_length,
                                            command_array[cnt],
                                            command_lengths[cnt], is_grantable))
                {
                  error= 1;
                  goto err;
                }
              }
            }
          }
        }
      }
    }
  }
err:
  mysql_rwlock_unlock(&LOCK_grant);

  DBUG_RETURN(error);
#else
  return (0);
#endif /* NO_EMBEDDED_ACCESS_CHECKS */
}


/**
  Check if user has enough privileges for execution of SHOW statement,
  which was converted to query to one of I_S tables.

  @param thd    Thread context.
  @param table  Table list element for I_S table to be queried..

  @retval FALSE - Success.
  @retval TRUE  - Failure.
*/

static bool check_show_access(THD *thd, TABLE_LIST *table)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  switch (get_schema_table_idx(table->schema_table)) {
  case SCH_SCHEMATA:
    return (specialflag & SPECIAL_SKIP_SHOW_DB) &&
      check_global_access(thd, SHOW_DB_ACL);

  case SCH_TABLE_NAMES:
  case SCH_TABLES:
  case SCH_VIEWS:
  case SCH_TRIGGERS:
  case SCH_EVENTS:
  {
    const char *dst_db_name= table->schema_select_lex->db;

    DBUG_ASSERT(dst_db_name);

    if (check_access(thd, SELECT_ACL, dst_db_name,
                     &thd->col_access, NULL, FALSE, FALSE))
      return TRUE;

    if (!thd->col_access && check_grant_db(thd, dst_db_name))
    {
      my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
               thd->security_ctx->priv_user,
               thd->security_ctx->priv_host,
               dst_db_name);
      return TRUE;
    }

    return FALSE;
  }

  case SCH_COLUMNS:
  case SCH_STATISTICS:
  {
    TABLE_LIST *dst_table;
    dst_table= table->schema_select_lex->table_list.first;

    DBUG_ASSERT(dst_table);

    /*
      Open temporary tables to be able to detect them during privilege check.
    */
    if (open_temporary_tables(thd, dst_table))
      return TRUE;

    if (check_access(thd, SELECT_ACL, dst_table->db,
                     &dst_table->grant.privilege,
                     &dst_table->grant.m_internal,
                     FALSE, FALSE))
          return TRUE; /* Access denied */

    /*
      Check_grant will grant access if there is any column privileges on
      all of the tables thanks to the fourth parameter (bool show_table).
    */
    if (check_grant(thd, SELECT_ACL, dst_table, TRUE, UINT_MAX, FALSE))
      return TRUE; /* Access denied */

    close_thread_tables(thd);
    dst_table->table= NULL;

    /* Access granted */
    return FALSE;
  }
  default:
    break;
  }
#endif /* NO_EMBEDDED_ACCESS_CHECKS */
  return FALSE;
}


/**
  check for global access and give descriptive error message if it fails.

  @param thd			Thread handler
  @param want_access		Use should have any of these global rights

  @warning
    One gets access right if one has ANY of the rights in want_access.
    This is useful as one in most cases only need one global right,
    but in some case we want to check if the user has SUPER or
    REPL_CLIENT_ACL rights.

  @retval
    0	ok
  @retval
    1	Access denied.  In this case an error is sent to the client
*/

bool check_global_access(THD *thd, ulong want_access)
{
  DBUG_ENTER("check_global_access");
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  char command[128];
  if ((thd->security_ctx->master_access & want_access))
    DBUG_RETURN(0);
  get_privilege_desc(command, sizeof(command), want_access);
  my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), command);
  DBUG_RETURN(1);
#else
  DBUG_RETURN(0);
#endif /*NO_EMBEDDED_ACCESS_CHECKS */
}
