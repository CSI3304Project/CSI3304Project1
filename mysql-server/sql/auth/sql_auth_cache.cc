/* Copyright (c) 2000, 2014, Oracle and/or its affiliates. All rights reserved.

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

#include "sql_show.h"                   /* append_identifier */
#include "log.h"                        /* sql_print_warning */
#include "sql_base.h"                   /* MYSQL_LOCK_IGNORE_TIMEOUT */
#include "key.h"                        /* key_copy, key_cmp_if_same */
                                        /* key_restore */

#include "auth_internal.h"
#include "sql_auth_cache.h"
#include "sql_authentication.h"
#include "sql_time.h"

#define INVALID_DATE "0000-00-00 00:00:00"

#include <algorithm>
#include <functional>
using std::min;

struct ACL_internal_schema_registry_entry
{
  const LEX_STRING *m_name;
  const ACL_internal_schema_access *m_access;
};
/**
  Internal schema registered.
  Currently, this is only:
  - performance_schema
  - information_schema,
  This can be reused later for:
  - mysql
*/
static ACL_internal_schema_registry_entry registry_array[2];
static uint m_registry_array_size= 0;

#ifndef NO_EMBEDDED_ACCESS_CHECKS
MEM_ROOT global_acl_memory;
MEM_ROOT memex;
Prealloced_array<ACL_USER, ACL_PREALLOC_SIZE> *acl_users= NULL;
Prealloced_array<ACL_PROXY_USER, ACL_PREALLOC_SIZE> *acl_proxy_users= NULL;
Prealloced_array<ACL_DB, ACL_PREALLOC_SIZE> *acl_dbs= NULL;
Prealloced_array<ACL_HOST_AND_IP, ACL_PREALLOC_SIZE> *acl_wild_hosts= NULL;

HASH column_priv_hash, proc_priv_hash, func_priv_hash;
hash_filo *acl_cache;
HASH acl_check_hosts;

bool initialized=0;
bool allow_all_hosts=1;
uint grant_version=0; /* Version of priv tables */
my_bool validate_user_plugins= TRUE;

#define FIRST_NON_YN_FIELD 26

#define IP_ADDR_STRLEN (3 + 1 + 3 + 1 + 3 + 1 + 3)
#define ACL_KEY_LENGTH (IP_ADDR_STRLEN + 1 + NAME_LEN + \
                        1 + USERNAME_LENGTH + 1)

#endif /* NO_EMBEDDED_ACCESS_CHECKS */

/**
  Add an internal schema to the registry.
  @param name the schema name
  @param access the schema ACL specific rules
*/
void ACL_internal_schema_registry::register_schema
  (const LEX_STRING &name, const ACL_internal_schema_access *access)
{
  DBUG_ASSERT(m_registry_array_size < array_elements(registry_array));

  /* Not thread safe, and does not need to be. */
  registry_array[m_registry_array_size].m_name= &name;
  registry_array[m_registry_array_size].m_access= access;
  m_registry_array_size++;
}


/**
  Search per internal schema ACL by name.
  @param name a schema name
  @return per schema rules, or NULL
*/
const ACL_internal_schema_access *
ACL_internal_schema_registry::lookup(const char *name)
{
  DBUG_ASSERT(name != NULL);

  uint i;

  for (i= 0; i<m_registry_array_size; i++)
  {
    if (my_strcasecmp(system_charset_info, registry_array[i].m_name->str,
                      name) == 0)
      return registry_array[i].m_access;
  }
  return NULL;
}


const char *
ACL_HOST_AND_IP::calc_ip(const char *ip_arg, long *val, char end)
{
  long ip_val,tmp;
  if (!(ip_arg=str2int(ip_arg,10,0,255,&ip_val)) || *ip_arg != '.')
    return 0;
  ip_val<<=24;
  if (!(ip_arg=str2int(ip_arg+1,10,0,255,&tmp)) || *ip_arg != '.')
    return 0;
  ip_val+=tmp<<16;
  if (!(ip_arg=str2int(ip_arg+1,10,0,255,&tmp)) || *ip_arg != '.')
    return 0;
  ip_val+=tmp<<8;
  if (!(ip_arg=str2int(ip_arg+1,10,0,255,&tmp)) || *ip_arg != end)
    return 0;
  *val=ip_val+tmp;
  return ip_arg;
}

/**
  @brief Update the hostname. Updates ip and ip_mask accordingly.

  @param host_arg Value to be stored
 */
void
ACL_HOST_AND_IP::update_hostname(const char *host_arg)
{
  hostname=(char*) host_arg;     // This will not be modified!
  hostname_length= hostname ? strlen( hostname ) : 0;
  if (!host_arg ||
      (!(host_arg=(char*) calc_ip(host_arg,&ip,'/')) ||
       !(host_arg=(char*) calc_ip(host_arg+1,&ip_mask,'\0'))))
  {
    ip= ip_mask=0;               // Not a masked ip
  }
}

/*
   @brief Comparing of hostnames

   @param  host_arg    Hostname to be compared with
   @param  ip_arg      IP address to be compared with

   @notes
   A hostname may be of type:
   1) hostname   (May include wildcards);   monty.pp.sci.fi
   2) ip     (May include wildcards);   192.168.0.0
   3) ip/netmask                        192.168.0.0/255.255.255.0
   A net mask of 0.0.0.0 is not allowed.

   @return
   true   if matched
   false  if not matched
 */

bool
ACL_HOST_AND_IP::compare_hostname(const char *host_arg, const char *ip_arg)
{
  long tmp;
  if (ip_mask && ip_arg && calc_ip(ip_arg,&tmp,'\0'))
  {
    return (tmp & ip_mask) == ip;
  }
  return (!hostname ||
      (host_arg &&
       !wild_case_compare(system_charset_info, host_arg, hostname)) ||
      (ip_arg && !wild_compare(ip_arg, hostname, 0)));
}

ACL_USER *
ACL_USER::copy(MEM_ROOT *root)
{
  ACL_USER *dst= (ACL_USER *) alloc_root(root, sizeof(ACL_USER));
  if (!dst)
    return 0;
  *dst= *this;
  dst->user= safe_strdup_root(root, user);
  dst->ssl_cipher= safe_strdup_root(root, ssl_cipher);
  dst->x509_issuer= safe_strdup_root(root, x509_issuer);
  dst->x509_subject= safe_strdup_root(root, x509_subject);
  /*
     If the plugin is built in we don't need to reallocate the name of the
     plugin.
   */
  if (auth_plugin_is_built_in(dst->plugin.str))
    dst->plugin= plugin;
  else
  {
    dst->plugin.str= strmake_root(root, plugin.str, plugin.length);
    dst->plugin.length= plugin.length;
  }
  dst->auth_string.str= safe_strdup_root(root, auth_string.str);
  dst->host.update_hostname(safe_strdup_root(root, host.get_host()));
  return dst;
}

void
ACL_PROXY_USER::init(const char *host_arg, const char *user_arg,
                     const char *proxied_host_arg,
                     const char *proxied_user_arg, bool with_grant_arg)
{
  user= (user_arg && *user_arg) ? user_arg : NULL;
  host.update_hostname ((host_arg && *host_arg) ? host_arg : NULL);
  proxied_user= (proxied_user_arg && *proxied_user_arg) ? 
    proxied_user_arg : NULL;
  proxied_host.update_hostname ((proxied_host_arg && *proxied_host_arg) ?
      proxied_host_arg : NULL);
  with_grant= with_grant_arg;
  sort= get_sort(4, host.get_host(), user,
      proxied_host.get_host(), proxied_user);
}

void
ACL_PROXY_USER::init(MEM_ROOT *mem, const char *host_arg, const char *user_arg,
                     const char *proxied_host_arg,
                     const char *proxied_user_arg, bool with_grant_arg)
{
  init ((host_arg && *host_arg) ? strdup_root (mem, host_arg) : NULL,
      (user_arg && *user_arg) ? strdup_root (mem, user_arg) : NULL,
      (proxied_host_arg && *proxied_host_arg) ? 
      strdup_root (mem, proxied_host_arg) : NULL,
      (proxied_user_arg && *proxied_user_arg) ? 
      strdup_root (mem, proxied_user_arg) : NULL,
      with_grant_arg);
}

void
ACL_PROXY_USER::init(TABLE *table, MEM_ROOT *mem)
{
  init (get_field(mem, table->field[MYSQL_PROXIES_PRIV_HOST]),
        get_field(mem, table->field[MYSQL_PROXIES_PRIV_USER]),
        get_field(mem, table->field[MYSQL_PROXIES_PRIV_PROXIED_HOST]),
        get_field(mem, table->field[MYSQL_PROXIES_PRIV_PROXIED_USER]),
                  table->field[MYSQL_PROXIES_PRIV_WITH_GRANT]->val_int() != 0);
}

bool
ACL_PROXY_USER::check_validity(bool check_no_resolve)
{
  if (check_no_resolve && 
      (hostname_requires_resolving(host.get_host()) ||
       hostname_requires_resolving(proxied_host.get_host())))
  {
    sql_print_warning("'proxies_priv' entry '%s@%s %s@%s' "
                      "ignored in --skip-name-resolve mode.",
                      proxied_user ? proxied_user : "",
                      proxied_host.get_host() ? proxied_host.get_host() : "",
                      user ? user : "",
                      host.get_host() ? host.get_host() : "");
    return TRUE;
  }
  return FALSE;
}

bool
ACL_PROXY_USER::matches(const char *host_arg, const char *user_arg,
                        const char *ip_arg, const char *proxied_user_arg)
{
  DBUG_ENTER("ACL_PROXY_USER::matches");
  DBUG_PRINT("info", ("compare_hostname(%s,%s,%s) &&"
             "compare_hostname(%s,%s,%s) &&"
             "wild_compare (%s,%s) &&"
             "wild_compare (%s,%s)",
             host.get_host() ? host.get_host() : "<NULL>",
             host_arg ? host_arg : "<NULL>",
             ip_arg ? ip_arg : "<NULL>",
             proxied_host.get_host() ? proxied_host.get_host() : "<NULL>",
             host_arg ? host_arg : "<NULL>",
             ip_arg ? ip_arg : "<NULL>",
             user_arg ? user_arg : "<NULL>",
             user ? user : "<NULL>",
             proxied_user_arg ? proxied_user_arg : "<NULL>",
             proxied_user ? proxied_user : "<NULL>"));
  DBUG_RETURN(host.compare_hostname(host_arg, ip_arg) &&
              proxied_host.compare_hostname(host_arg, ip_arg) &&
              (!user ||
               (user_arg && !wild_compare(user_arg, user, TRUE))) &&
              (!proxied_user || 
               (proxied_user && !wild_compare(proxied_user_arg, proxied_user,
                                              TRUE))));
}

bool
ACL_PROXY_USER::pk_equals(ACL_PROXY_USER *grant)
{
  DBUG_ENTER("pk_equals");
  DBUG_PRINT("info", ("strcmp(%s,%s) &&"
             "strcmp(%s,%s) &&"
             "wild_compare (%s,%s) &&"
             "wild_compare (%s,%s)",
             user ? user : "<NULL>",
             grant->user ? grant->user : "<NULL>",
             proxied_user ? proxied_user : "<NULL>",
             grant->proxied_user ? grant->proxied_user : "<NULL>",
             host.get_host() ? host.get_host() : "<NULL>",
             grant->host.get_host() ? grant->host.get_host() : "<NULL>",
             proxied_host.get_host() ? proxied_host.get_host() : "<NULL>",
             grant->proxied_host.get_host() ? 
             grant->proxied_host.get_host() : "<NULL>"));

  DBUG_RETURN(auth_element_equals(user, grant->user) &&
              auth_element_equals(proxied_user, grant->proxied_user) &&
              auth_element_equals(host.get_host(), grant->host.get_host()) &&
              auth_element_equals(proxied_host.get_host(), 
                                  grant->proxied_host.get_host()));
}

void
ACL_PROXY_USER::print_grant(String *str)
{
  str->append(STRING_WITH_LEN("GRANT PROXY ON '"));
  if (proxied_user)
    str->append(proxied_user, strlen(proxied_user));
  str->append(STRING_WITH_LEN("'@'"));
  if (proxied_host.get_host())
    str->append(proxied_host.get_host(), strlen(proxied_host.get_host()));
  str->append(STRING_WITH_LEN("' TO '"));
  if (user)
    str->append(user, strlen(user));
  str->append(STRING_WITH_LEN("'@'"));
  if (host.get_host())
    str->append(host.get_host(), strlen(host.get_host()));
  str->append(STRING_WITH_LEN("'"));
  if (with_grant)
    str->append(STRING_WITH_LEN(" WITH GRANT OPTION"));
}

int
ACL_PROXY_USER::store_pk(TABLE *table,
                         const LEX_CSTRING &host,
                         const LEX_CSTRING &user,
                         const LEX_CSTRING &proxied_host,
                         const LEX_CSTRING &proxied_user)
{
  DBUG_ENTER("ACL_PROXY_USER::store_pk");
  DBUG_PRINT("info", ("host=%s, user=%s, proxied_host=%s, proxied_user=%s",
                      host.str ? host.str : "<NULL>",
                      user.str ? user.str : "<NULL>",
                      proxied_host.str ? proxied_host.str : "<NULL>",
                      proxied_user.str ? proxied_user.str : "<NULL>"));
  if (table->field[MYSQL_PROXIES_PRIV_HOST]->store(host.str,
                                                   host.length,
                                                   system_charset_info))
    DBUG_RETURN(TRUE);
  if (table->field[MYSQL_PROXIES_PRIV_USER]->store(user.str,
                                                   user.length,
                                                   system_charset_info))
    DBUG_RETURN(TRUE);
  if (table->field[MYSQL_PROXIES_PRIV_PROXIED_HOST]->store(proxied_host.str,
                                                           proxied_host.length,
                                                           system_charset_info))
    DBUG_RETURN(TRUE);
  if (table->field[MYSQL_PROXIES_PRIV_PROXIED_USER]->store(proxied_user.str,
                                                           proxied_user.length,
                                                           system_charset_info))
    DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}

int
ACL_PROXY_USER::store_data_record(TABLE *table,
                                  const LEX_CSTRING &host,
                                  const LEX_CSTRING &user,
                                  const LEX_CSTRING &proxied_host,
                                  const LEX_CSTRING &proxied_user,
                                  bool with_grant,
                                  const char *grantor)
{
  DBUG_ENTER("ACL_PROXY_USER::store_pk");
  if (store_pk(table,  host, user, proxied_host, proxied_user))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("with_grant=%s", with_grant ? "TRUE" : "FALSE"));
  if (table->field[MYSQL_PROXIES_PRIV_WITH_GRANT]->store(with_grant ? 1 : 0, 
                                                         TRUE))
    DBUG_RETURN(TRUE);
  if (table->field[MYSQL_PROXIES_PRIV_GRANTOR]->store(grantor, 
                                                      strlen(grantor),
                                                      system_charset_info))
    DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}


int wild_case_compare(CHARSET_INFO *cs, const char *str,const char *wildstr)
{
  int flag;
  DBUG_ENTER("wild_case_compare");
  DBUG_PRINT("enter",("str: '%s'  wildstr: '%s'",str,wildstr));
  while (*wildstr)
  {
    while (*wildstr && *wildstr != wild_many && *wildstr != wild_one)
    {
      if (*wildstr == wild_prefix && wildstr[1])
        wildstr++;
      if (my_toupper(cs, *wildstr++) !=
          my_toupper(cs, *str++)) DBUG_RETURN(1);
    }
    if (! *wildstr ) DBUG_RETURN (*str != 0);
    if (*wildstr++ == wild_one)
    {
      if (! *str++) DBUG_RETURN (1);    /* One char; skip */
    }
    else
    {                                           /* Found '*' */
      if (!*wildstr) DBUG_RETURN(0);            /* '*' as last char: OK */
      flag=(*wildstr != wild_many && *wildstr != wild_one);
      do
      {
        if (flag)
        {
          char cmp;
          if ((cmp= *wildstr) == wild_prefix && wildstr[1])
            cmp=wildstr[1];
          cmp=my_toupper(cs, cmp);
          while (*str && my_toupper(cs, *str) != cmp)
            str++;
          if (!*str) DBUG_RETURN (1);
        }
        if (wild_case_compare(cs, str,wildstr) == 0) DBUG_RETURN (0);
      } while (*str++);
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN (*str != '\0');
}


/*
  Return a number which, if sorted 'desc', puts strings in this order:
    no wildcards
    wildcards
    empty string
*/

ulong get_sort(uint count,...)
{
  va_list args;
  va_start(args,count);
  ulong sort=0;

  /* Should not use this function with more than 4 arguments for compare. */
  DBUG_ASSERT(count <= 4);

  while (count--)
  {
    char *start, *str= va_arg(args,char*);
    uint chars= 0;
    uint wild_pos= 0;           /* first wildcard position */

    if ((start= str))
    {
      for (; *str ; str++)
      {
        if (*str == wild_prefix && str[1])
          str++;
        else if (*str == wild_many || *str == wild_one)
        {
          wild_pos= (uint) (str - start) + 1;
          break;
        }
        chars= 128;                             // Marker that chars existed
      }
    }
    sort= (sort << 8) + (wild_pos ? min(wild_pos, 127U) : chars);
  }
  va_end(args);
  return sort;
}


/**
  Check if the given host name needs to be resolved or not.
  Host name has to be resolved if it actually contains *name*.

  For example:
    192.168.1.1               --> FALSE
    192.168.1.0/255.255.255.0 --> FALSE
    %                         --> FALSE
    192.168.1.%               --> FALSE
    AB%                       --> FALSE

    AAAAFFFF                  --> TRUE (Hostname)
    AAAA:FFFF:1234:5678       --> FALSE
    ::1                       --> FALSE

  This function does not check if the given string is a valid host name or
  not. It assumes that the argument is a valid host name.

  @param hostname   the string to check.

  @return a flag telling if the argument needs to be resolved or not.
  @retval TRUE the argument is a host name and needs to be resolved.
  @retval FALSE the argument is either an IP address, or a patter and
          should not be resolved.
*/

bool hostname_requires_resolving(const char *hostname)
{
  if (!hostname)
    return FALSE;

  /* Check if hostname is the localhost. */

  size_t hostname_len= strlen(hostname);
  size_t localhost_len= strlen(my_localhost);

  if (hostname == my_localhost ||
      (hostname_len == localhost_len &&
       !my_strnncoll(system_charset_info,
                     (const uchar *) hostname,  hostname_len,
                     (const uchar *) my_localhost, strlen(my_localhost))))
  {
    return FALSE;
  }

  /*
    If the string contains any of {':', '%', '_', '/'}, it is definitely
    not a host name:
      - ':' means that the string is an IPv6 address;
      - '%' or '_' means that the string is a pattern;
      - '/' means that the string is an IPv4 network address;
  */

  for (const char *p= hostname; *p; ++p)
  {
    switch (*p) {
      case ':':
      case '%':
      case '_':
      case '/':
        return FALSE;
    }
  }

  /*
    Now we have to tell a host name (ab.cd, 12.ab) from an IPv4 address
    (12.34.56.78). The assumption is that if the string contains only
    digits and dots, it is an IPv4 address. Otherwise -- a host name.
  */

  for (const char *p= hostname; *p; ++p)
  {
    if (*p != '.' && !my_isdigit(&my_charset_latin1, *p))
      return TRUE; /* a "letter" has been found. */
  }

  return FALSE; /* all characters are either dots or digits. */
}


#ifndef NO_EMBEDDED_ACCESS_CHECKS


static uchar* get_key_column(GRANT_COLUMN *buff, size_t *length,
                             my_bool not_used __attribute__((unused)))
{
  *length=buff->key_length;
  return (uchar*) buff->column;
}


uchar* get_grant_table(GRANT_NAME *buff, size_t *length,
                       my_bool not_used __attribute__((unused)))
{
  *length=buff->key_length;
  return (uchar*) buff->hash_key;
}


GRANT_COLUMN::GRANT_COLUMN(String &c,  ulong y) :rights (y)
{
  column= (char*) memdup_root(&memex,c.ptr(), key_length=c.length());
}


void GRANT_NAME::set_user_details(const char *h, const char *d,
                                  const char *u, const char *t,
                                  bool is_routine)
{
  /* Host given by user */
  host.update_hostname(strdup_root(&memex, h));
  if (db != d)
  {
    db= strdup_root(&memex, d);
    if (lower_case_table_names)
      my_casedn_str(files_charset_info, db);
  }
  user = strdup_root(&memex,u);
  sort=  get_sort(3,host.get_host(),db,user);
  if (tname != t)
  {
    tname= strdup_root(&memex, t);
    if (lower_case_table_names || is_routine)
      my_casedn_str(files_charset_info, tname);
  }
  key_length= strlen(d) + strlen(u)+ strlen(t)+3;
  hash_key=   (char*) alloc_root(&memex,key_length);
  my_stpcpy(my_stpcpy(my_stpcpy(hash_key,user)+1,db)+1,tname);
}

GRANT_NAME::GRANT_NAME(const char *h, const char *d,const char *u,
                       const char *t, ulong p, bool is_routine)
  :db(0), tname(0), privs(p)
{
  set_user_details(h, d, u, t, is_routine);
}

GRANT_TABLE::GRANT_TABLE(const char *h, const char *d,const char *u,
                         const char *t, ulong p, ulong c)
  :GRANT_NAME(h,d,u,t,p, FALSE), cols(c)
{
  (void) my_hash_init2(&hash_columns,4,system_charset_info,
                   0,0,0, (my_hash_get_key) get_key_column,0,0);
}


GRANT_NAME::GRANT_NAME(TABLE *form, bool is_routine)
{
  host.update_hostname(get_field(&memex, form->field[0]));
  db=    get_field(&memex,form->field[1]);
  user=  get_field(&memex,form->field[2]);
  if (!user)
    user= (char*) "";
  sort=  get_sort(3, host.get_host(), db, user);
  tname= get_field(&memex,form->field[3]);
  if (!db || !tname) {
    /* Wrong table row; Ignore it */
    privs= 0;
    return;                                     /* purecov: inspected */
  }
  if (lower_case_table_names)
  {
    my_casedn_str(files_charset_info, db);
  }
  if (lower_case_table_names || is_routine)
  {
    my_casedn_str(files_charset_info, tname);
  }
  key_length= (strlen(db) + strlen(user) + strlen(tname) + 3);
  hash_key=   (char*) alloc_root(&memex, key_length);
  my_stpcpy(my_stpcpy(my_stpcpy(hash_key,user)+1,db)+1,tname);
  privs = (ulong) form->field[6]->val_int();
  privs = fix_rights_for_table(privs);
}


GRANT_TABLE::GRANT_TABLE(TABLE *form, TABLE *col_privs)
  :GRANT_NAME(form, FALSE)
{
  uchar key[MAX_KEY_LENGTH];

  if (!db || !tname)
  {
    /* Wrong table row; Ignore it */
    my_hash_clear(&hash_columns);               /* allow for destruction */
    cols= 0;
    return;
  }
  cols= (ulong) form->field[7]->val_int();
  cols =  fix_rights_for_column(cols);

  (void) my_hash_init2(&hash_columns,4,system_charset_info,
                   0,0,0, (my_hash_get_key) get_key_column,0,0);
  if (cols)
  {
    uint key_prefix_len;
    KEY_PART_INFO *key_part= col_privs->key_info->key_part;
    col_privs->field[0]->store(host.get_host(),
                               host.get_host() ? host.get_host_len() : 0,
                               system_charset_info);
    col_privs->field[1]->store(db, strlen(db), system_charset_info);
    col_privs->field[2]->store(user, strlen(user), system_charset_info);
    col_privs->field[3]->store(tname, strlen(tname), system_charset_info);

    key_prefix_len= (key_part[0].store_length +
                     key_part[1].store_length +
                     key_part[2].store_length +
                     key_part[3].store_length);
    key_copy(key, col_privs->record[0], col_privs->key_info, key_prefix_len);
    col_privs->field[4]->store("",0, &my_charset_latin1);

    if (col_privs->file->ha_index_init(0, 1))
    {
      cols= 0;
      return;
    }

    if (col_privs->file->ha_index_read_map(col_privs->record[0], (uchar*) key,
                                           (key_part_map)15, HA_READ_KEY_EXACT))
    {
      cols = 0; /* purecov: deadcode */
      col_privs->file->ha_index_end();
      return;
    }
    do
    {
      String *res,column_name;
      GRANT_COLUMN *mem_check;
      /* As column name is a string, we don't have to supply a buffer */
      res=col_privs->field[4]->val_str(&column_name);
      ulong priv= (ulong) col_privs->field[6]->val_int();
      if (!(mem_check = new GRANT_COLUMN(*res,
                                         fix_rights_for_column(priv))))
      {
        /* Don't use this entry */
        privs = cols = 0;               /* purecov: deadcode */
        return;                         /* purecov: deadcode */
      }
      if (my_hash_insert(&hash_columns, (uchar *) mem_check))
      {
        /* Invalidate this entry */
        privs= cols= 0;
        return;
      }
    } while (!col_privs->file->ha_index_next(col_privs->record[0]) &&
             !key_cmp_if_same(col_privs,key,0,key_prefix_len));
    col_privs->file->ha_index_end();
  }
}


GRANT_TABLE::~GRANT_TABLE()
{
  my_hash_free(&hash_columns);
}


/*
  Find first entry that matches the current user
*/

ACL_USER *
find_acl_user(const char *host, const char *user, my_bool exact)
{
  DBUG_ENTER("find_acl_user");
  DBUG_PRINT("enter",("host: '%s'  user: '%s'",host,user));

  mysql_mutex_assert_owner(&acl_cache->lock);

  for (ACL_USER *acl_user= acl_users->begin();
       acl_user != acl_users->end(); ++acl_user)
  {
    DBUG_PRINT("info",("strcmp('%s','%s'), compare_hostname('%s','%s'),",
                       user, acl_user->user ? acl_user->user : "",
                       host,
                       acl_user->host.get_host() ? acl_user->host.get_host() :
                       ""));
    if ((!acl_user->user && !user[0]) ||
        (acl_user->user && !strcmp(user,acl_user->user)))
    {
      if (exact ? !my_strcasecmp(system_charset_info, host,
                                 acl_user->host.get_host() ?
                                 acl_user->host.get_host() : "") :
          acl_user->host.compare_hostname(host,host))
      {
        DBUG_RETURN(acl_user);
      }
    }
  }
  DBUG_RETURN(0);
}


/*
  Find user in ACL

  SYNOPSIS
    is_acl_user()
    host                 host name
    user                 user name

  RETURN
   FALSE  user not fond
   TRUE   there are such user
*/

bool is_acl_user(const char *host, const char *user)
{
  bool res;

  /* --skip-grants */
  if (!initialized)
    return TRUE;

  mysql_mutex_lock(&acl_cache->lock);
  res= find_acl_user(host, user, TRUE) != NULL;
  mysql_mutex_unlock(&acl_cache->lock);
  return res;
}


/**
  Validate if a user can proxy as another user

  @thd                     current thread
  @param user              the logged in user (proxy user)
  @param authenticated_as  the effective user a plugin is trying to 
                           impersonate as (proxied user)
  @return                  proxy user definition
    @retval NULL           proxy user definition not found or not applicable
    @retval non-null       the proxy user data
*/

ACL_PROXY_USER *
acl_find_proxy_user(const char *user, const char *host, const char *ip, 
                    const char *authenticated_as, bool *proxy_used)
{
  /* if the proxied and proxy user are the same return OK */
  DBUG_ENTER("acl_find_proxy_user");
  DBUG_PRINT("info", ("user=%s host=%s ip=%s authenticated_as=%s",
                      user, host, ip, authenticated_as));

  if (!strcmp(authenticated_as, user))
  {
    DBUG_PRINT ("info", ("user is the same as authenticated_as"));
    DBUG_RETURN (NULL);
  }

  *proxy_used= TRUE; 
  for (ACL_PROXY_USER *proxy= acl_proxy_users->begin();
       proxy != acl_proxy_users->end(); ++proxy)
  {
    if (proxy->matches(host, user, ip, authenticated_as))
      DBUG_RETURN(proxy);
  }

  DBUG_RETURN(NULL);
}


static uchar* acl_entry_get_key(acl_entry *entry, size_t *length,
                                my_bool not_used __attribute__((unused)))
{
  *length=(uint) entry->length;
  return (uchar*) entry->key;
}


static uchar* check_get_key(ACL_USER *buff, size_t *length,
                            my_bool not_used __attribute__((unused)))
{
  *length=buff->host.get_host_len();
  return (uchar*) buff->host.get_host();
}


/*
  Get privilege for a host, user and db combination

  as db_is_pattern changes the semantics of comparison,
  acl_cache is not used if db_is_pattern is set.
*/

ulong acl_get(const char *host, const char *ip,
              const char *user, const char *db, my_bool db_is_pattern)
{
  ulong host_access= ~(ulong)0, db_access= 0;
  size_t key_length, copy_length;
  char key[ACL_KEY_LENGTH],*tmp_db,*end;
  acl_entry *entry;
  DBUG_ENTER("acl_get");

  copy_length= (size_t) (strlen(ip ? ip : "") +
                 strlen(user ? user : "") +
                 strlen(db ? db : "")) + 2; /* Added 2 at the end to avoid  
                                               buffer overflow at strmov()*/
  /*
    Make sure that my_stpcpy() operations do not result in buffer overflow.
  */
  if (copy_length >= ACL_KEY_LENGTH)
    DBUG_RETURN(0);

  mysql_mutex_lock(&acl_cache->lock);
  end=my_stpcpy((tmp_db=my_stpcpy(my_stpcpy(key, ip ? ip : "")+1,user)+1),db);
  if (lower_case_table_names)
  {
    my_casedn_str(files_charset_info, tmp_db);
    db=tmp_db;
  }
  key_length= (size_t) (end-key);
  if (!db_is_pattern && (entry=(acl_entry*) acl_cache->search((uchar*) key,
                                                              key_length)))
  {
    db_access=entry->access;
    mysql_mutex_unlock(&acl_cache->lock);
    DBUG_PRINT("exit", ("access: 0x%lx", db_access));
    DBUG_RETURN(db_access);
  }

  /*
    Check if there are some access rights for database and user
  */
  for (ACL_DB *acl_db= acl_dbs->begin(); acl_db != acl_dbs->end(); ++acl_db)
  {
    if (!acl_db->user || !strcmp(user,acl_db->user))
    {
      if (acl_db->host.compare_hostname(host,ip))
      {
        if (!acl_db->db || !wild_compare(db,acl_db->db,db_is_pattern))
        {
          db_access=acl_db->access;
          if (acl_db->host.get_host())
            goto exit;                          // Fully specified. Take it
          break; /* purecov: tested */
        }
      }
    }
  }
  if (!db_access)
    goto exit;                                  // Can't be better

exit:
  /* Save entry in cache for quick retrieval */
  if (!db_is_pattern &&
      (entry= (acl_entry*) malloc(sizeof(acl_entry)+key_length)))
  {
    entry->access=(db_access & host_access);
    entry->length=key_length;
    memcpy((uchar*) entry->key,key,key_length);
    acl_cache->add(entry);
  }
  mysql_mutex_unlock(&acl_cache->lock);
  DBUG_PRINT("exit", ("access: 0x%lx", db_access & host_access));
  DBUG_RETURN(db_access & host_access);
}


/**
  Check if the user is allowed to change password

 @param thd THD
 @param host Hostname for the user
 @param user User name
 @param new_password new password

 new_password cannot be NULL

 @return Error status
   @retval 0 OK
   @retval 1 ERROR; In this case the error is sent to the client.
*/

/*
  Check if there are any possible matching entries for this host

  NOTES
    All host names without wild cards are stored in a hash table,
    entries with wildcards are stored in a dynamic array
*/

static void init_check_host(void)
{
  DBUG_ENTER("init_check_host");
  if (acl_wild_hosts != NULL)
    acl_wild_hosts->clear();
  else
    acl_wild_hosts=
      new Prealloced_array<ACL_HOST_AND_IP, ACL_PREALLOC_SIZE>(key_memory_acl_mem);

  (void) my_hash_init(&acl_check_hosts,system_charset_info,
                      acl_users->size(), 0, 0,
                      (my_hash_get_key) check_get_key, 0, 0);
  if (!allow_all_hosts)
  {
    for (ACL_USER *acl_user= acl_users->begin();
         acl_user != acl_users->end(); ++acl_user)
    {
      if (acl_user->host.has_wildcard())
      {                                         // Has wildcard
        ACL_HOST_AND_IP *acl= NULL;
        for (acl= acl_wild_hosts->begin(); acl != acl_wild_hosts->end(); ++acl)
        {                                       // Check if host already exists
          if (!my_strcasecmp(system_charset_info,
                             acl_user->host.get_host(), acl->get_host()))
            break;                              // already stored
        }
        if (acl == acl_wild_hosts->end())       // If new
          acl_wild_hosts->push_back(acl_user->host);
      }
      else if (!my_hash_search(&acl_check_hosts,(uchar*)
                               acl_user->host.get_host(),
                               strlen(acl_user->host.get_host())))
      {
        if (my_hash_insert(&acl_check_hosts,(uchar*) acl_user))
        {                                       // End of memory
          allow_all_hosts=1;                    // Should never happen
          DBUG_VOID_RETURN;
        }
      }
    }
  }
  acl_wild_hosts->shrink_to_fit();
  freeze_size(&acl_check_hosts.array);
  DBUG_VOID_RETURN;
}


/*
  Rebuild lists used for checking of allowed hosts

  We need to rebuild 'acl_check_hosts' and 'acl_wild_hosts' after adding,
  dropping or renaming user, since they contain pointers to elements of
  'acl_user' array, which are invalidated by drop operation, and use
  ACL_USER::host::hostname as a key, which is changed by rename.
*/
void rebuild_check_host(void)
{
  delete acl_wild_hosts;
  acl_wild_hosts= NULL;
  my_hash_free(&acl_check_hosts);
  init_check_host();
}


/*
  Gets user credentials without authentication and resource limit checks.

  SYNOPSIS
    acl_getroot()
      sctx               Context which should be initialized
      user               user name
      host               host name
      ip                 IP
      db                 current data base name

  RETURN
    FALSE  OK
    TRUE   Error
*/

bool acl_getroot(Security_context *sctx, char *user, char *host,
                 char *ip, const char *db)
{
  int res= 1;
  ACL_USER *acl_user= 0;
  DBUG_ENTER("acl_getroot");

  DBUG_PRINT("enter", ("Host: '%s', Ip: '%s', User: '%s', db: '%s'",
                       (host ? host : "(NULL)"), (ip ? ip : "(NULL)"),
                       user, (db ? db : "(NULL)")));
  sctx->user= user;
  sctx->set_host(host);
  sctx->set_ip(ip);

  sctx->host_or_ip= host ? host : (ip ? ip : "");

  if (!initialized)
  {
    /*
      here if mysqld's been started with --skip-grant-tables option.
    */
    sctx->skip_grants();
    DBUG_RETURN(FALSE);
  }

  mysql_mutex_lock(&acl_cache->lock);

  sctx->master_access= 0;
  sctx->db_access= 0;
  *sctx->priv_user= *sctx->priv_host= 0;

  /*
     Find acl entry in user database.
     This is specially tailored to suit the check we do for CALL of
     a stored procedure; user is set to what is actually a
     priv_user, which can be ''.
  */
  for (ACL_USER *acl_user_tmp= acl_users->begin();
       acl_user_tmp != acl_users->end(); ++acl_user_tmp)
  {
    if ((!acl_user_tmp->user && !user[0]) ||
        (acl_user_tmp->user && strcmp(user, acl_user_tmp->user) == 0))
    {
      if (acl_user_tmp->host.compare_hostname(host, ip))
      {
        acl_user= acl_user_tmp;
        res= 0;
        break;
      }
    }
  }

  if (acl_user)
  {
    for (ACL_DB *acl_db= acl_dbs->begin(); acl_db != acl_dbs->end(); ++acl_db)
    {
      if (!acl_db->user ||
          (user && user[0] && !strcmp(user, acl_db->user)))
      {
        if (acl_db->host.compare_hostname(host, ip))
        {
          if (!acl_db->db || (db && !wild_compare(db, acl_db->db, 0)))
          {
            sctx->db_access= acl_db->access;
            break;
          }
        }
      }
    }
    sctx->master_access= acl_user->access;

    if (acl_user->user)
      strmake(sctx->priv_user, user, USERNAME_LENGTH);
    else
      *sctx->priv_user= 0;

    if (acl_user->host.get_host())
      strmake(sctx->priv_host, acl_user->host.get_host(), MAX_HOSTNAME - 1);
    else
      *sctx->priv_host= 0;

    sctx->password_expired= acl_user->password_expired;
  }
  mysql_mutex_unlock(&acl_cache->lock);
  DBUG_RETURN(res);
}


namespace {

class ACL_compare :
  public std::binary_function<ACL_ACCESS, ACL_ACCESS, bool>
{
public:
  bool operator()(const ACL_ACCESS &a, const ACL_ACCESS &b)
  {
    return a.sort > b.sort;
  }
};

} // namespace


/**
  Convert scrambled password to binary form, according to scramble type, 
  Binary form is stored in user.salt.
  
  @param acl_user The object where to store the salt
  @param password The password hash containing the salt
  @param password_len The length of the password hash
   
  Despite the name of the function it is used when loading ACLs from disk
  to store the password hash in the ACL_USER object.
  Note that it works only for native and "old" mysql authentication built-in
  plugins.
  
  Assumption : user's authentication plugin information is available.

  @return Password hash validation
    @retval false Hash is of suitable length
    @retval true Hash is of wrong length or format
*/

bool set_user_salt(ACL_USER *acl_user,
                   const char *password, size_t password_len)
{
  bool result= false;
  /* Using old password protocol */
  if (password_len == SCRAMBLED_PASSWORD_CHAR_LENGTH)
  {
    get_salt_from_password(acl_user->salt, password);
    acl_user->salt_len= SCRAMBLE_LENGTH;
  }
  else if (password_len == 0 || password == NULL)
  {
    /* This account doesn't use a password */
    acl_user->salt_len= 0;
  }
  else if (acl_user->plugin.str == native_password_plugin_name.str)
  {
    /* Unexpected format of the hash; login will probably be impossible */
    result= true;
  }

  /*
    Since we're changing the password for the user we need to reset the
    expiration flag.
  */
  acl_user->password_expired= false;
  
  return result;
}


/**
  Iterate over the user records and check for irregularities.
  Currently this includes :
   - checking if the plugin referenced is present.
   - if there's sha256 users and there's neither SSL nor RSA configured
*/
static void
validate_user_plugin_records()
{
  DBUG_ENTER("validate_user_plugin_records");
  if (!validate_user_plugins)
    DBUG_VOID_RETURN;

  lock_plugin_data();
  for (ACL_USER *acl_user= acl_users->begin();
       acl_user != acl_users->end(); ++acl_user)
  {
    struct st_plugin_int *plugin;

    if (acl_user->plugin.length)
    {
      /* rule 1 : plugin does exit */
      if (!auth_plugin_is_built_in(acl_user->plugin.str))
      {
        plugin= plugin_find_by_type(acl_user->plugin,
                                    MYSQL_AUTHENTICATION_PLUGIN);

        if (!plugin)
        {
          sql_print_warning("The plugin '%.*s' used to authenticate "
                            "user '%s'@'%.*s' is not loaded."
                            " Nobody can currently login using this account.",
                            (int) acl_user->plugin.length, acl_user->plugin.str,
                            acl_user->user,
                            static_cast<int>(acl_user->host.get_host_len()),
                            acl_user->host.get_host());
        }
      }
      if (acl_user->plugin.str == sha256_password_plugin_name.str &&
          rsa_auth_status() && !ssl_acceptor_fd)
      {
          sql_print_warning("The plugin '%s' is used to authenticate "
                            "user '%s'@'%.*s', "
#if !defined(HAVE_YASSL)
                            "but neither SSL nor RSA keys are "
#else
                            "but no SSL is "
#endif
                            "configured. "
                            "Nobody can currently login using this account.",
                            sha256_password_plugin_name.str,
                            acl_user->user,
                            static_cast<int>(acl_user->host.get_host_len()),
                            acl_user->host.get_host());
      }
    }
  }
  unlock_plugin_data();
  DBUG_VOID_RETURN;
}


/*
  Initialize structures responsible for user/db-level privilege checking and
  load privilege information for them from tables in the 'mysql' database.

  SYNOPSIS
    acl_init()
      dont_read_acl_tables  TRUE if we want to skip loading data from
                            privilege tables and disable privilege checking.

  NOTES
    This function is mostly responsible for preparatory steps, main work
    on initialization and grants loading is done in acl_reload().

  RETURN VALUES
    0   ok
    1   Could not initialize grant's
*/

my_bool acl_init(bool dont_read_acl_tables)
{
  THD  *thd;
  my_bool return_val;
  DBUG_ENTER("acl_init");

  acl_cache= new hash_filo(ACL_CACHE_SIZE, 0, 0,
                           (my_hash_get_key) acl_entry_get_key,
                           (my_hash_free_key) free,
                           &my_charset_utf8_bin);

  /*
    cache built-in native authentication plugins,
    to avoid hash searches and a global mutex lock on every connect
  */
  native_password_plugin= my_plugin_lock_by_name(0,
           native_password_plugin_name, MYSQL_AUTHENTICATION_PLUGIN);
  if (!native_password_plugin)
    DBUG_RETURN(1);

  if (dont_read_acl_tables)
  {
    DBUG_RETURN(0); /* purecov: tested */
  }

  /*
    To be able to run this from boot, we allocate a temporary THD
  */
  if (!(thd=new THD))
    DBUG_RETURN(1); /* purecov: inspected */
  thd->thread_stack= (char*) &thd;
  thd->store_globals();
  /*
    It is safe to call acl_reload() since acl_* arrays and hashes which
    will be freed there are global static objects and thus are initialized
    by zeros at startup.
  */
  return_val= acl_reload(thd);

  thd->release_resources();
  delete thd;

  DBUG_RETURN(return_val);
}


/*
  Initialize structures responsible for user/db-level privilege checking
  and load information about grants from open privilege tables.

  SYNOPSIS
    acl_load()
      thd     Current thread
      tables  List containing open "mysql.host", "mysql.user",
              "mysql.db" and "mysql.proxies_priv" tables in that order.

  RETURN VALUES
    FALSE  Success
    TRUE   Error
*/

static my_bool acl_load(THD *thd, TABLE_LIST *tables)
{
  TABLE *table;
  READ_RECORD read_record_info;
  my_bool return_val= TRUE;
  bool check_no_resolve= specialflag & SPECIAL_NO_RESOLVE;
  char tmp_name[NAME_LEN+1];
  int password_length;
  char *password;
  size_t password_len;
  sql_mode_t old_sql_mode= thd->variables.sql_mode;
  bool password_expired= false;
  DBUG_ENTER("acl_load");

  thd->variables.sql_mode&= ~MODE_PAD_CHAR_TO_FULL_LENGTH;

  grant_version++; /* Privileges updated */

  
  acl_cache->clear(1);                          // Clear locked hostname cache

  init_sql_alloc(key_memory_acl_mem,
                 &global_acl_memory, ACL_ALLOC_BLOCK_SIZE, 0);
  /*
    Prepare reading from the mysql.user table
  */
  if (init_read_record(&read_record_info, thd, table=tables[0].table,
                       NULL, 1, 1, FALSE))
    goto end;
  table->use_all_columns();
  acl_users->clear();
  
  allow_all_hosts=0;
  while (!(read_record_info.read_record(&read_record_info)))
  {
    password_expired= false;
    /* Reading record from mysql.user */
    ACL_USER user;
    memset(&user, 0, sizeof(user));

    /*
      All accounts can authenticate per default. This will change when
      we add a new field to the user table.

      Currently this flag is only set to false when authentication is attempted
      using an unknown user name.
    */
    user.can_authenticate= true;

    user.host.update_hostname(get_field(&global_acl_memory,
                                        table->field[MYSQL_USER_FIELD_HOST]));
    user.user= get_field(&global_acl_memory,
                         table->field[MYSQL_USER_FIELD_USER]);
    if (check_no_resolve && hostname_requires_resolving(user.host.get_host()))
    {
      sql_print_warning("'user' entry '%s@%s' "
                        "ignored in --skip-name-resolve mode.",
                        user.user ? user.user : "",
                        user.host.get_host() ? user.host.get_host() : "");
      continue;
    }

    /* Read legacy password */
    password= get_field(&global_acl_memory,
                        table->field[MYSQL_USER_FIELD_PASSWORD]);
    password_len= password ? strlen(password) : 0;
    user.auth_string.str= password ? password : const_cast<char*>("");
    user.auth_string.length= password_len;

    {
      uint next_field;
      user.access= get_access(table,3,&next_field) & GLOBAL_ACLS;
      /*
        if it is pre 5.0.1 privilege table then map CREATE privilege on
        CREATE VIEW & SHOW VIEW privileges
      */
      if (table->s->fields <= 31 && (user.access & CREATE_ACL))
        user.access|= (CREATE_VIEW_ACL | SHOW_VIEW_ACL);

      /*
        if it is pre 5.0.2 privilege table then map CREATE/ALTER privilege on
        CREATE PROCEDURE & ALTER PROCEDURE privileges
      */
      if (table->s->fields <= 33 && (user.access & CREATE_ACL))
        user.access|= CREATE_PROC_ACL;
      if (table->s->fields <= 33 && (user.access & ALTER_ACL))
        user.access|= ALTER_PROC_ACL;

      /*
        pre 5.0.3 did not have CREATE_USER_ACL
      */
      if (table->s->fields <= 36 && (user.access & GRANT_ACL))
        user.access|= CREATE_USER_ACL;


      /*
        if it is pre 5.1.6 privilege table then map CREATE privilege on
        CREATE|ALTER|DROP|EXECUTE EVENT
      */
      if (table->s->fields <= 37 && (user.access & SUPER_ACL))
        user.access|= EVENT_ACL;

      /*
        if it is pre 5.1.6 privilege then map TRIGGER privilege on CREATE.
      */
      if (table->s->fields <= 38 && (user.access & SUPER_ACL))
        user.access|= TRIGGER_ACL;

      user.sort= get_sort(2,user.host.get_host(),user.user);

      /* Starting from 4.0.2 we have more fields */
      if (table->s->fields >= 31)
      {
        char *ssl_type=
          get_field(thd->mem_root, table->field[MYSQL_USER_FIELD_SSL_TYPE]);
        if (!ssl_type)
          user.ssl_type=SSL_TYPE_NONE;
        else if (!strcmp(ssl_type, "ANY"))
          user.ssl_type=SSL_TYPE_ANY;
        else if (!strcmp(ssl_type, "X509"))
          user.ssl_type=SSL_TYPE_X509;
        else  /* !strcmp(ssl_type, "SPECIFIED") */
          user.ssl_type=SSL_TYPE_SPECIFIED;

        user.ssl_cipher= 
          get_field(&global_acl_memory, table->field[MYSQL_USER_FIELD_SSL_CIPHER]);
        user.x509_issuer=
          get_field(&global_acl_memory, table->field[MYSQL_USER_FIELD_X509_ISSUER]);
        user.x509_subject=
          get_field(&global_acl_memory, table->field[MYSQL_USER_FIELD_X509_SUBJECT]);

        char *ptr= get_field(thd->mem_root,
                             table->field[MYSQL_USER_FIELD_MAX_QUESTIONS]);
        user.user_resource.questions=ptr ? atoi(ptr) : 0;
        ptr= get_field(thd->mem_root,
                       table->field[MYSQL_USER_FIELD_MAX_UPDATES]);
        user.user_resource.updates=ptr ? atoi(ptr) : 0;
        ptr= get_field(thd->mem_root,
                       table->field[MYSQL_USER_FIELD_MAX_CONNECTIONS]);
        user.user_resource.conn_per_hour= ptr ? atoi(ptr) : 0;
        if (user.user_resource.questions || user.user_resource.updates ||
            user.user_resource.conn_per_hour)
          mqh_used=1;

        if (table->s->fields > MYSQL_USER_FIELD_MAX_USER_CONNECTIONS)
        {
          /* Starting from 5.0.3 we have max_user_connections field */
          ptr= get_field(thd->mem_root,
                         table->field[MYSQL_USER_FIELD_MAX_USER_CONNECTIONS]);
          user.user_resource.user_conn= ptr ? atoi(ptr) : 0;
        }

        if (table->s->fields >= 41)
        {
          /* We may have plugin & auth_String fields */
          char *tmpstr= get_field(&global_acl_memory,
                                  table->field[MYSQL_USER_FIELD_PLUGIN]);
          if (tmpstr)
          {
	    /*
	      Check if the plugin string is blank.
	      If it is, the user will be skipped.
	    */
	    if(strlen(tmpstr) == 0)
	    {
	      sql_print_warning("User entry '%s'@'%s' has an empty plugin "
				"value. The user will be ignored and no one can login "
				"with this user anymore.",
				user.user ? user.user : "",
				user.host.get_host() ? user.host.get_host() : "");
	      continue;
	    }
            /*
              By comparing the plugin with the built in plugins it is possible
              to optimize the string allocation and comparision.
            */
            if (my_strcasecmp(system_charset_info, tmpstr,
                              native_password_plugin_name.str) == 0)
              user.plugin= native_password_plugin_name;
#if defined(HAVE_OPENSSL)
            else
              if (my_strcasecmp(system_charset_info, tmpstr,
                                sha256_password_plugin_name.str) == 0)
                user.plugin= sha256_password_plugin_name;
#endif
            else
              {
                user.plugin.str= tmpstr;
                user.plugin.length= strlen(tmpstr);
              }
            if (user.auth_string.length &&
                user.plugin.str != native_password_plugin_name.str)
            {
              sql_print_warning("'user' entry '%s@%s' has both a password "
                                "and an authentication plugin specified. The "
                                "password will be ignored.",
                                user.user ? user.user : "",
                                user.host.get_host() ? user.host.get_host() : "");
            }
            user.auth_string.str=
              get_field(&global_acl_memory,
                        table->field[MYSQL_USER_FIELD_AUTHENTICATION_STRING]);
            if (!user.auth_string.str)
              user.auth_string.str= const_cast<char*>("");
            user.auth_string.length= strlen(user.auth_string.str);
          }
          else /* skip the user if plugin value is NULL */
	  {
	    sql_print_warning("User entry '%s'@'%s' has an empty plugin "
			      "value. The user will be ignored and no one can login "
			      "with this user anymore.",
			      user.user ? user.user : "",
			      user.host.get_host() ? user.host.get_host() : "");
	    continue;
	  }
        }

        if (table->s->fields > MYSQL_USER_FIELD_PASSWORD_EXPIRED)
        {
          char *tmpstr= get_field(&global_acl_memory,
                                  table->field[MYSQL_USER_FIELD_PASSWORD_EXPIRED]);
          if (tmpstr && (*tmpstr == 'Y' || *tmpstr == 'y'))
          {
            user.password_expired= true;

            if (!auth_plugin_supports_expiration(user.plugin.str))
            {
              sql_print_warning("'user' entry '%s@%s' has the password ignore "
                                "flag raised, but its authentication plugin "
                                "doesn't support password expiration. "
                                "The user id will be ignored.",
                                user.user ? user.user : "",
                                user.host.get_host() ? user.host.get_host() : "");
              continue;
            }
            password_expired= true;
          }
        }

	/*
	   Initalize the values of timestamp and expire after day
	   to error and true respectively.
	*/
	user.password_last_changed.time_type= MYSQL_TIMESTAMP_ERROR;
	user.use_default_password_lifetime= true;
	user.password_lifetime= 0;

	if (table->s->fields > MYSQL_USER_FIELD_PASSWORD_LAST_CHANGED)
        {
	  if (!table->field[MYSQL_USER_FIELD_PASSWORD_LAST_CHANGED]->is_null())
	  {
            char *password_last_changed= get_field(&global_acl_memory,
	          table->field[MYSQL_USER_FIELD_PASSWORD_LAST_CHANGED]);

	    if (password_last_changed &&
	        memcmp(password_last_changed, INVALID_DATE, sizeof(INVALID_DATE)))
	    {
	      String str(password_last_changed, &my_charset_bin);
              str_to_time_with_warn(&str,&(user.password_last_changed));
	    }
	  }
	}

        if (table->s->fields > MYSQL_USER_FIELD_PASSWORD_LIFETIME)
        {
          if (!table->
              field[MYSQL_USER_FIELD_PASSWORD_LIFETIME]->is_null())
	  {
	    char *ptr= get_field(&global_acl_memory,
		table->field[MYSQL_USER_FIELD_PASSWORD_LIFETIME]);
	    user.password_lifetime= ptr ? atoi(ptr) : 0;
	    user.use_default_password_lifetime= false;
	  }
	}

      } // end if (table->s->fields >= 31)
      else
      {
        user.ssl_type=SSL_TYPE_NONE;
        if (table->s->fields <= 13)
        {                                               // Without grant
          if (user.access & CREATE_ACL)
            user.access|=REFERENCES_ACL | INDEX_ACL | ALTER_ACL;
        }
        /* Convert old privileges */
        user.access|= LOCK_TABLES_ACL | CREATE_TMP_ACL | SHOW_DB_ACL;
        if (user.access & FILE_ACL)
          user.access|= REPL_CLIENT_ACL | REPL_SLAVE_ACL;
        if (user.access & PROCESS_ACL)
          user.access|= SUPER_ACL | EXECUTE_ACL;
      }
      /*
         Transform hex to octets and adjust the format.
       */
      if (set_user_salt(&user, password, password_len))
      {
        sql_print_warning("Found invalid password for user: '%s@%s'; "
                          "Ignoring user", user.user ? user.user : "",
                          user.host.get_host() ? user.host.get_host() : "");
        continue;
      }

      /* set_user_salt resets expiration flag so restore it */
      user.password_expired= password_expired;

      acl_users->push_back(user);
      if (user.host.check_allow_all_hosts())
        allow_all_hosts=1;                      // Anyone can connect
    }
  } // END while reading records from the mysql.user table
  
  std::sort(acl_users->begin(), acl_users->end(), ACL_compare());
  end_read_record(&read_record_info);
  acl_users->shrink_to_fit();

  /* Legacy password integrity checks ----------------------------------------*/
  { 
    password_length= table->field[MYSQL_USER_FIELD_PASSWORD]->field_length /
      table->field[MYSQL_USER_FIELD_PASSWORD]->charset()->mbmaxlen;
    if (password_length < SCRAMBLED_PASSWORD_CHAR_LENGTH)
    {
      sql_print_error("Fatal error: mysql.user table is damaged or in "
                      "unsupported pre-4.1 format.");
      goto end;
    }
  
    DBUG_PRINT("info",("user table fields: %d, password length: %d",
               table->s->fields, password_length));
  } /* End legacy password integrity checks ----------------------------------*/
  
  /*
    Prepare reading from the mysql.db table
  */
  if (init_read_record(&read_record_info, thd, table=tables[1].table,
                       NULL, 1, 1, FALSE))
    goto end;
  table->use_all_columns();
  acl_dbs->clear();

  while (!(read_record_info.read_record(&read_record_info)))
  {
    /* Reading record in mysql.db */
    ACL_DB db;
    db.host.update_hostname(get_field(&global_acl_memory, 
                            table->field[MYSQL_DB_FIELD_HOST]));
    db.db=get_field(&global_acl_memory, table->field[MYSQL_DB_FIELD_DB]);
    if (!db.db)
    {
      sql_print_warning("Found an entry in the 'db' table with empty database name; Skipped");
      continue;
    }
    db.user=get_field(&global_acl_memory, table->field[MYSQL_DB_FIELD_USER]);
    if (check_no_resolve && hostname_requires_resolving(db.host.get_host()))
    {
      sql_print_warning("'db' entry '%s %s@%s' "
                        "ignored in --skip-name-resolve mode.",
                        db.db,
                        db.user ? db.user : "",
                        db.host.get_host() ? db.host.get_host() : "");
      continue;
    }
    db.access=get_access(table,3,0);
    db.access=fix_rights_for_db(db.access);
    if (lower_case_table_names)
    {
      /*
        convert db to lower case and give a warning if the db wasn't
        already in lower case
      */
      (void)my_stpcpy(tmp_name, db.db);
      my_casedn_str(files_charset_info, db.db);
      if (strcmp(db.db, tmp_name) != 0)
      {
        sql_print_warning("'db' entry '%s %s@%s' had database in mixed "
                          "case that has been forced to lowercase because "
                          "lower_case_table_names is set. It will not be "
                          "possible to remove this privilege using REVOKE.",
                          db.db,
                          db.user ? db.user : "",
                          db.host.get_host() ? db.host.get_host() : "");
      }
    }
    db.sort=get_sort(3,db.host.get_host(),db.db,db.user);
    if (table->s->fields <=  9)
    {                                           // Without grant
      if (db.access & CREATE_ACL)
        db.access|=REFERENCES_ACL | INDEX_ACL | ALTER_ACL;
    }
    acl_dbs->push_back(db);
  } // END reading records from mysql.db tables
  
  std::sort(acl_dbs->begin(), acl_dbs->end(), ACL_compare());
  end_read_record(&read_record_info);
  acl_dbs->shrink_to_fit();

  /* Prepare to read records from the mysql.proxies_priv table */
  acl_proxy_users->clear();

  if (tables[2].table)
  {
    if (init_read_record(&read_record_info, thd, table= tables[2].table,
                         NULL, 1, 1, FALSE))
      goto end;
    table->use_all_columns();
    while (!(read_record_info.read_record(&read_record_info)))
    {
      /* Reading record in mysql.proxies_priv */
      ACL_PROXY_USER proxy;
      proxy.init(table, &global_acl_memory);
      if (proxy.check_validity(check_no_resolve))
        continue;
      if (acl_proxy_users->push_back(proxy))
      {
        end_read_record(&read_record_info);
        goto end;
      }
    } // END reading records from the mysql.proxies_priv table

    std::sort(acl_proxy_users->begin(), acl_proxy_users->end(), ACL_compare());
    end_read_record(&read_record_info);
  }
  else
  {
    sql_print_error("Missing system table mysql.proxies_priv; "
                    "please run mysql_upgrade to create it");
  }
  acl_proxy_users->shrink_to_fit();

  validate_user_plugin_records();
  init_check_host();

  initialized=1;
  return_val= FALSE;

end:
  thd->variables.sql_mode= old_sql_mode;
  DBUG_RETURN(return_val);
}


void acl_free(bool end)
{
  free_root(&global_acl_memory,MYF(0));
  delete acl_users;
  acl_users= NULL;
  delete acl_dbs;
  acl_dbs= NULL;
  delete acl_wild_hosts;
  acl_wild_hosts= NULL;
  delete acl_proxy_users;
  acl_proxy_users= NULL;
  my_hash_free(&acl_check_hosts);
  plugin_unlock(0, native_password_plugin);
  if (!end)
    acl_cache->clear(1); /* purecov: inspected */
  else
  {
    delete acl_cache;
    acl_cache=0;
  }
}


/*
  Forget current user/db-level privileges and read new privileges
  from the privilege tables.

  SYNOPSIS
    acl_reload()
      thd  Current thread

  NOTE
    All tables of calling thread which were open and locked by LOCK TABLES
    statement will be unlocked and closed.
    This function is also used for initialization of structures responsible
    for user/db-level privilege checking.

  RETURN VALUE
    FALSE  Success
    TRUE   Failure
*/

my_bool acl_reload(THD *thd)
{
  TABLE_LIST tables[3];

  MEM_ROOT old_mem;
  bool old_initialized;
  my_bool return_val= TRUE;
  DBUG_ENTER("acl_reload");

  /*
    To avoid deadlocks we should obtain table locks before
    obtaining acl_cache->lock mutex.
  */
  tables[0].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("user"), "user", TL_READ);
  tables[1].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("db"), "db", TL_READ);
  tables[2].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("proxies_priv"), 
                           "proxies_priv", TL_READ);
  tables[0].next_local= tables[0].next_global= tables + 1;
  tables[1].next_local= tables[1].next_global= tables + 2;
  tables[0].open_type= tables[1].open_type= tables[2].open_type= OT_BASE_ONLY;
  tables[2].open_strategy= TABLE_LIST::OPEN_IF_EXISTS;

  if (open_and_lock_tables(thd, tables, FALSE, MYSQL_LOCK_IGNORE_TIMEOUT))
  {
    /*
      Execution might have been interrupted; only print the error message
      if a user error condition has been raised.
    */
    if (thd->get_stmt_da()->is_error())
    {
      sql_print_error("Fatal error: Can't open and lock privilege tables: %s",
                      thd->get_stmt_da()->message_text());
    }
    close_acl_tables(thd);
    DBUG_RETURN(true);
  }

  if ((old_initialized=initialized))
    mysql_mutex_lock(&acl_cache->lock);

  Prealloced_array<ACL_USER, ACL_PREALLOC_SIZE> *old_acl_users= acl_users;
  Prealloced_array<ACL_DB, ACL_PREALLOC_SIZE> *old_acl_dbs= acl_dbs;
  Prealloced_array<ACL_PROXY_USER,
                   ACL_PREALLOC_SIZE> *old_acl_proxy_users = acl_proxy_users;

  acl_users= new Prealloced_array<ACL_USER,
                                  ACL_PREALLOC_SIZE>(key_memory_acl_mem);
  acl_dbs= new Prealloced_array<ACL_DB,
                                ACL_PREALLOC_SIZE>(key_memory_acl_mem);
  acl_proxy_users=
    new Prealloced_array<ACL_PROXY_USER,
                         ACL_PREALLOC_SIZE>(key_memory_acl_mem);  

  old_mem= global_acl_memory;
  delete acl_wild_hosts;
  acl_wild_hosts= NULL;
  my_hash_free(&acl_check_hosts);

  if ((return_val= acl_load(thd, tables)))
  {                                     // Error. Revert to old list
    DBUG_PRINT("error",("Reverting to old privileges"));
    acl_free();                         /* purecov: inspected */
    acl_users= old_acl_users;
    acl_dbs= old_acl_dbs;
    acl_proxy_users= old_acl_proxy_users;

    global_acl_memory= old_mem;
    init_check_host();
  }
  else
  {
    free_root(&old_mem,MYF(0));
    delete old_acl_users;
    delete old_acl_dbs;
    delete old_acl_proxy_users;
  }
  if (old_initialized)
    mysql_mutex_unlock(&acl_cache->lock);

  close_acl_tables(thd);
  DBUG_RETURN(return_val);
}


void acl_insert_proxy_user(ACL_PROXY_USER *new_value)
{
  DBUG_ENTER("acl_insert_proxy_user");
  mysql_mutex_assert_owner(&acl_cache->lock);
  acl_proxy_users->push_back(*new_value);
  std::sort(acl_proxy_users->begin(), acl_proxy_users->end(), ACL_compare());
  DBUG_VOID_RETURN;
}


void free_grant_table(GRANT_TABLE *grant_table)
{
  my_hash_free(&grant_table->hash_columns);
}


/* Search after a matching grant. Prefer exact grants before not exact ones */

GRANT_NAME *name_hash_search(HASH *name_hash,
                             const char *host,const char* ip,
                             const char *db,
                             const char *user, const char *tname,
                             bool exact, bool name_tolower)
{
  char helping [NAME_LEN*2+USERNAME_LENGTH+3], *name_ptr;
  uint len;
  GRANT_NAME *grant_name,*found=0;
  HASH_SEARCH_STATE state;

  name_ptr= my_stpcpy(my_stpcpy(helping, user) + 1, db) + 1;
  len  = (uint) (my_stpcpy(name_ptr, tname) - helping) + 1;
  if (name_tolower)
    my_casedn_str(files_charset_info, name_ptr);
  for (grant_name= (GRANT_NAME*) my_hash_first(name_hash, (uchar*) helping,
                                               len, &state);
       grant_name ;
       grant_name= (GRANT_NAME*) my_hash_next(name_hash,(uchar*) helping,
                                              len, &state))
  {
    if (exact)
    {
      if (!grant_name->host.get_host() ||
          (host &&
           !my_strcasecmp(system_charset_info, host,
                          grant_name->host.get_host())) ||
          (ip && !strcmp(ip, grant_name->host.get_host())))
        return grant_name;
    }
    else
    {
      if (grant_name->host.compare_hostname(host, ip) &&
          (!found || found->sort < grant_name->sort))
        found=grant_name;                                       // Host ok
    }
  }
  return found;
}


/* Free grant array if possible */

void  grant_free(void)
{
  DBUG_ENTER("grant_free");
  my_hash_free(&column_priv_hash);
  my_hash_free(&proc_priv_hash);
  my_hash_free(&func_priv_hash);
  free_root(&memex,MYF(0));
  DBUG_VOID_RETURN;
}


/**
  @brief Initialize structures responsible for table/column-level privilege
   checking and load information for them from tables in the 'mysql' database.

  @return Error status
    @retval 0 OK
    @retval 1 Could not initialize grant subsystem.
*/

my_bool grant_init()
{
  THD  *thd;
  my_bool return_val;
  DBUG_ENTER("grant_init");

  if (!(thd= new THD))
    DBUG_RETURN(1);                             /* purecov: deadcode */
  thd->thread_stack= (char*) &thd;
  thd->store_globals();

  return_val=  grant_reload(thd);

  thd->release_resources();
  delete thd;

  DBUG_RETURN(return_val);
}


/**
  @brief Helper function to grant_reload_procs_priv

  Reads the procs_priv table into memory hash.

  @param table A pointer to the procs_priv table structure.

  @see grant_reload
  @see grant_reload_procs_priv

  @return Error state
    @retval TRUE An error occurred
    @retval FALSE Success
*/

static my_bool grant_load_procs_priv(TABLE *p_table)
{
  MEM_ROOT *memex_ptr;
  my_bool return_val= 1;
  bool check_no_resolve= specialflag & SPECIAL_NO_RESOLVE;
  MEM_ROOT **save_mem_root_ptr= my_pthread_get_THR_MALLOC();
  DBUG_ENTER("grant_load_procs_priv");
  (void) my_hash_init(&proc_priv_hash, &my_charset_utf8_bin,
                      0,0,0, (my_hash_get_key) get_grant_table,
                      0,0);
  (void) my_hash_init(&func_priv_hash, &my_charset_utf8_bin,
                      0,0,0, (my_hash_get_key) get_grant_table,
                      0,0);
  if (p_table->file->ha_index_init(0, 1))
    DBUG_RETURN(TRUE);
  p_table->use_all_columns();

  if (!p_table->file->ha_index_first(p_table->record[0]))
  {
    memex_ptr= &memex;
    my_pthread_set_THR_MALLOC(&memex_ptr);
    do
    {
      GRANT_NAME *mem_check;
      HASH *hash;
      if (!(mem_check=new (memex_ptr) GRANT_NAME(p_table, TRUE)))
      {
        /* This could only happen if we are out memory */
        goto end_unlock;
      }

      if (check_no_resolve)
      {
        if (hostname_requires_resolving(mem_check->host.get_host()))
        {
          sql_print_warning("'procs_priv' entry '%s %s@%s' "
                            "ignored in --skip-name-resolve mode.",
                            mem_check->tname, mem_check->user,
                            mem_check->host.get_host() ?
                            mem_check->host.get_host() : "");
          continue;
        }
      }
      if (p_table->field[4]->val_int() == SP_TYPE_PROCEDURE)
      {
        hash= &proc_priv_hash;
      }
      else
      if (p_table->field[4]->val_int() == SP_TYPE_FUNCTION)
      {
        hash= &func_priv_hash;
      }
      else
      {
        sql_print_warning("'procs_priv' entry '%s' "
                          "ignored, bad routine type",
                          mem_check->tname);
        continue;
      }

      mem_check->privs= fix_rights_for_procedure(mem_check->privs);
      if (! mem_check->ok())
        delete mem_check;
      else if (my_hash_insert(hash, (uchar*) mem_check))
      {
        delete mem_check;
        goto end_unlock;
      }
    }
    while (!p_table->file->ha_index_next(p_table->record[0]));
  }
  /* Return ok */
  return_val= 0;

end_unlock:
  p_table->file->ha_index_end();
  my_pthread_set_THR_MALLOC(save_mem_root_ptr);
  DBUG_RETURN(return_val);
}


/**
  @brief Initialize structures responsible for table/column-level privilege
    checking and load information about grants from open privilege tables.

  @param thd Current thread
  @param tables List containing open "mysql.tables_priv" and
    "mysql.columns_priv" tables.

  @see grant_reload

  @return Error state
    @retval FALSE Success
    @retval TRUE Error
*/

static my_bool grant_load(THD *thd, TABLE_LIST *tables)
{
  MEM_ROOT *memex_ptr;
  my_bool return_val= 1;
  TABLE *t_table= 0, *c_table= 0;
  bool check_no_resolve= specialflag & SPECIAL_NO_RESOLVE;
  MEM_ROOT **save_mem_root_ptr= my_pthread_get_THR_MALLOC();
  sql_mode_t old_sql_mode= thd->variables.sql_mode;
  DBUG_ENTER("grant_load");

  thd->variables.sql_mode&= ~MODE_PAD_CHAR_TO_FULL_LENGTH;

  (void) my_hash_init(&column_priv_hash, &my_charset_utf8_bin,
                      0,0,0, (my_hash_get_key) get_grant_table,
                      (my_hash_free_key) free_grant_table,0);

  t_table = tables[0].table;
  c_table = tables[1].table;
  if (t_table->file->ha_index_init(0, 1))
    goto end_index_init;
  t_table->use_all_columns();
  c_table->use_all_columns();

  if (!t_table->file->ha_index_first(t_table->record[0]))
  {
    memex_ptr= &memex;
    my_pthread_set_THR_MALLOC(&memex_ptr);
    do
    {
      GRANT_TABLE *mem_check;
      if (!(mem_check=new (memex_ptr) GRANT_TABLE(t_table,c_table)))
      {
        /* This could only happen if we are out memory */
        goto end_unlock;
      }

      if (check_no_resolve)
      {
        if (hostname_requires_resolving(mem_check->host.get_host()))
        {
          sql_print_warning("'tables_priv' entry '%s %s@%s' "
                            "ignored in --skip-name-resolve mode.",
                            mem_check->tname,
                            mem_check->user ? mem_check->user : "",
                            mem_check->host.get_host() ?
                            mem_check->host.get_host() : "");
          continue;
        }
      }

      if (! mem_check->ok())
        delete mem_check;
      else if (my_hash_insert(&column_priv_hash,(uchar*) mem_check))
      {
        delete mem_check;
        goto end_unlock;
      }
    }
    while (!t_table->file->ha_index_next(t_table->record[0]));
  }

  return_val=0;                                 // Return ok

end_unlock:
  t_table->file->ha_index_end();
  my_pthread_set_THR_MALLOC(save_mem_root_ptr);
end_index_init:
  thd->variables.sql_mode= old_sql_mode;
  DBUG_RETURN(return_val);
}


/**
  @brief Helper function to grant_reload. Reloads procs_priv table is it
    exists.

  @param thd A pointer to the thread handler object.

  @see grant_reload

  @return Error state
    @retval FALSE Success
    @retval TRUE An error has occurred.
*/

static my_bool grant_reload_procs_priv(THD *thd)
{
  HASH old_proc_priv_hash, old_func_priv_hash;
  TABLE_LIST table;
  my_bool return_val= FALSE;
  DBUG_ENTER("grant_reload_procs_priv");

  table.init_one_table("mysql", 5, "procs_priv",
                       strlen("procs_priv"), "procs_priv",
                       TL_READ);
  table.open_type= OT_BASE_ONLY;

  if (open_and_lock_tables(thd, &table, FALSE, MYSQL_LOCK_IGNORE_TIMEOUT))
    DBUG_RETURN(TRUE);

  mysql_rwlock_wrlock(&LOCK_grant);
  /* Save a copy of the current hash if we need to undo the grant load */
  old_proc_priv_hash= proc_priv_hash;
  old_func_priv_hash= func_priv_hash;

  if ((return_val= grant_load_procs_priv(table.table)))
  {
    /* Error; Reverting to old hash */
    DBUG_PRINT("error",("Reverting to old privileges"));
    grant_free();
    proc_priv_hash= old_proc_priv_hash;
    func_priv_hash= old_func_priv_hash;
  }
  else
  {
    my_hash_free(&old_proc_priv_hash);
    my_hash_free(&old_func_priv_hash);
  }
  mysql_rwlock_unlock(&LOCK_grant);

  DBUG_RETURN(return_val);
}


/**
  @brief Reload information about table and column level privileges if possible

  @param thd Current thread

  Locked tables are checked by acl_reload() and doesn't have to be checked
  in this call.
  This function is also used for initialization of structures responsible
  for table/column-level privilege checking.

  @return Error state
    @retval FALSE Success
    @retval TRUE  Error
*/

my_bool grant_reload(THD *thd)
{
  TABLE_LIST tables[2];
  HASH old_column_priv_hash;
  MEM_ROOT old_mem;
  my_bool return_val= 1;
  DBUG_ENTER("grant_reload");

  /* Don't do anything if running with --skip-grant-tables */
  if (!initialized)
    DBUG_RETURN(0);

  tables[0].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("tables_priv"),
                           "tables_priv", TL_READ);
  tables[1].init_one_table(C_STRING_WITH_LEN("mysql"),
                           C_STRING_WITH_LEN("columns_priv"),
                           "columns_priv", TL_READ);
  tables[0].next_local= tables[0].next_global= tables+1;
  tables[0].open_type= tables[1].open_type= OT_BASE_ONLY;

  /*
    To avoid deadlocks we should obtain table locks before
    obtaining LOCK_grant rwlock.
  */
  if (open_and_lock_tables(thd, tables, FALSE, MYSQL_LOCK_IGNORE_TIMEOUT))
    goto end;

  mysql_rwlock_wrlock(&LOCK_grant);
  old_column_priv_hash= column_priv_hash;

  /*
    Create a new memory pool but save the current memory pool to make an undo
    opertion possible in case of failure.
  */
  old_mem= memex;
  init_sql_alloc(key_memory_acl_memex,
                 &memex, ACL_ALLOC_BLOCK_SIZE, 0);

  if ((return_val= grant_load(thd, tables)))
  {                                             // Error. Revert to old hash
    DBUG_PRINT("error",("Reverting to old privileges"));
    grant_free();                               /* purecov: deadcode */
    column_priv_hash= old_column_priv_hash;     /* purecov: deadcode */
    memex= old_mem;                             /* purecov: deadcode */
  }
  else
  {
    my_hash_free(&old_column_priv_hash);
    free_root(&old_mem,MYF(0));
  }
  mysql_rwlock_unlock(&LOCK_grant);
  close_acl_tables(thd);

  /*
    It is OK failing to load procs_priv table because we may be
    working with 4.1 privilege tables.
  */
  if (grant_reload_procs_priv(thd))
    return_val= 1;

  mysql_rwlock_wrlock(&LOCK_grant);
  grant_version++;
  mysql_rwlock_unlock(&LOCK_grant);

end:
  close_acl_tables(thd);
  DBUG_RETURN(return_val);
}


void acl_update_user(const char *user, const char *host,
                     const char *password, size_t password_len,
                     enum SSL_type ssl_type,
                     const char *ssl_cipher,
                     const char *x509_issuer,
                     const char *x509_subject,
                     USER_RESOURCES  *mqh,
                     ulong privileges,
                     const LEX_CSTRING &plugin,
                     const LEX_CSTRING &auth,
		     MYSQL_TIME password_change_time)
{
  DBUG_ENTER("acl_update_user");
  mysql_mutex_assert_owner(&acl_cache->lock);
  for (ACL_USER *acl_user= acl_users->begin();
       acl_user != acl_users->end(); ++acl_user)
  {
    if ((!acl_user->user && !user[0]) ||
        (acl_user->user && !strcmp(user,acl_user->user)))
    {
      if ((!acl_user->host.get_host() && !host[0]) ||
          (acl_user->host.get_host() &&
          !my_strcasecmp(system_charset_info, host, acl_user->host.get_host())))
      {
        if (plugin.length > 0)
        {
          acl_user->plugin.str= plugin.str;
          acl_user->plugin.length = plugin.length;
          optimize_plugin_compare_by_pointer(&acl_user->plugin);
          if (!auth_plugin_is_built_in(acl_user->plugin.str))
            acl_user->plugin.str= strmake_root(&global_acl_memory,
                                               plugin.str, plugin.length);
          acl_user->auth_string.str= auth.str ?
            strmake_root(&global_acl_memory, auth.str,
                         auth.length) : const_cast<char*>("");
          acl_user->auth_string.length= auth.length;
        }
        acl_user->access=privileges;
        if (mqh->specified_limits & USER_RESOURCES::QUERIES_PER_HOUR)
          acl_user->user_resource.questions=mqh->questions;
        if (mqh->specified_limits & USER_RESOURCES::UPDATES_PER_HOUR)
          acl_user->user_resource.updates=mqh->updates;
        if (mqh->specified_limits & USER_RESOURCES::CONNECTIONS_PER_HOUR)
          acl_user->user_resource.conn_per_hour= mqh->conn_per_hour;
        if (mqh->specified_limits & USER_RESOURCES::USER_CONNECTIONS)
          acl_user->user_resource.user_conn= mqh->user_conn;
        if (ssl_type != SSL_TYPE_NOT_SPECIFIED)
        {
          acl_user->ssl_type= ssl_type;
          acl_user->ssl_cipher= (ssl_cipher ? strdup_root(&global_acl_memory,
                                                    ssl_cipher) :        0);
          acl_user->x509_issuer= (x509_issuer ? strdup_root(&global_acl_memory,
                                                      x509_issuer) : 0);
          acl_user->x509_subject= (x509_subject ?
                                   strdup_root(&global_acl_memory, x509_subject) : 0);
        }
  
        if (password)
        {
          /*
            We just assert the hash is valid here since it's already
            checked in replace_user_table().
          */
          int hash_not_ok= set_user_salt(acl_user, password, password_len);

          DBUG_ASSERT(hash_not_ok == 0);
          /* dummy addition to fool the compiler */
          password_len+= hash_not_ok;

	  acl_user->password_last_changed= password_change_time;
        }
        /* search complete: */
        break;
      }
    }
  }
  DBUG_VOID_RETURN;
}


void acl_insert_user(const char *user, const char *host,
                     const char *password, size_t password_len,
                     enum SSL_type ssl_type,
                     const char *ssl_cipher,
                     const char *x509_issuer,
                     const char *x509_subject,
                     USER_RESOURCES *mqh,
                     ulong privileges,
                     const LEX_CSTRING &plugin,
                     const LEX_CSTRING &auth,
		     MYSQL_TIME password_change_time)
{
  DBUG_ENTER("acl_insert_user");
  ACL_USER acl_user;
  int hash_not_ok;

  mysql_mutex_assert_owner(&acl_cache->lock);
  /*
     All accounts can authenticate per default. This will change when
     we add a new field to the user table.

     Currently this flag is only set to false when authentication is attempted
     using an unknown user name.
  */
  acl_user.can_authenticate= true;

  acl_user.user= *user ? strdup_root(&global_acl_memory,user) : 0;
  acl_user.host.update_hostname(*host ? strdup_root(&global_acl_memory, host) : 0);
  if (plugin.str[0])
  {
    acl_user.plugin= plugin;
    optimize_plugin_compare_by_pointer(&acl_user.plugin);
    if (!auth_plugin_is_built_in(acl_user.plugin.str))
      acl_user.plugin.str= strmake_root(&global_acl_memory,
                                        plugin.str, plugin.length);
    acl_user.auth_string.str= auth.str ?
      strmake_root(&global_acl_memory, auth.str,
                   auth.length) : const_cast<char*>("");
    acl_user.auth_string.length= auth.length;

    optimize_plugin_compare_by_pointer(&acl_user.plugin);
  }
  else
  {
    acl_user.plugin= native_password_plugin_name;
    acl_user.auth_string.str= const_cast<char*>("");
    acl_user.auth_string.length= 0;
  }

  acl_user.access= privileges;
  acl_user.user_resource= *mqh;
  acl_user.sort= get_sort(2,acl_user.host.get_host(), acl_user.user);
  //acl_user.hostname_length=(uint) strlen(host);
  acl_user.ssl_type=
    (ssl_type != SSL_TYPE_NOT_SPECIFIED ? ssl_type : SSL_TYPE_NONE);
  acl_user.ssl_cipher=
    ssl_cipher ? strdup_root(&global_acl_memory, ssl_cipher) : 0;
  acl_user.x509_issuer=
    x509_issuer ? strdup_root(&global_acl_memory, x509_issuer) : 0;
  acl_user.x509_subject=
    x509_subject ? strdup_root(&global_acl_memory, x509_subject) : 0;
  /*
    During create user we can never specify a value for password expiry days field.
  */
  acl_user.use_default_password_lifetime= true;

  acl_user.password_last_changed= password_change_time;

  hash_not_ok= set_user_salt(&acl_user, password, password_len);
  DBUG_ASSERT(hash_not_ok == 0);
  /* dummy addition to fool the compiler */
  password_len+= hash_not_ok;
  

  acl_users->push_back(acl_user);
  if (acl_user.host.check_allow_all_hosts())
    allow_all_hosts=1;          // Anyone can connect /* purecov: tested */
  std::sort(acl_users->begin(), acl_users->end(), ACL_compare());

  /* Rebuild 'acl_check_hosts' since 'acl_users' has been modified */
  rebuild_check_host();
  DBUG_VOID_RETURN;
}


void acl_update_proxy_user(ACL_PROXY_USER *new_value, bool is_revoke)
{
  mysql_mutex_assert_owner(&acl_cache->lock);

  DBUG_ENTER("acl_update_proxy_user");
  for (ACL_PROXY_USER *acl_user= acl_proxy_users->begin();
       acl_user != acl_proxy_users->end(); ++acl_user)
  {
    if (acl_user->pk_equals(new_value))
    {
      if (is_revoke)
      {
        DBUG_PRINT("info", ("delting ACL_PROXY_USER"));
        acl_proxy_users->erase(acl_user);
      }
      else
      {
        DBUG_PRINT("info", ("updating ACL_PROXY_USER"));
        acl_user->set_data(new_value);
      }
      break;
    }
  }
  DBUG_VOID_RETURN;
}


void acl_update_db(const char *user, const char *host, const char *db,
                   ulong privileges)
{
  mysql_mutex_assert_owner(&acl_cache->lock);

  for (ACL_DB *acl_db= acl_dbs->begin(); acl_db < acl_dbs->end(); )
  {
    if ((!acl_db->user && !user[0]) ||
        (acl_db->user &&
        !strcmp(user,acl_db->user)))
    {
      if ((!acl_db->host.get_host() && !host[0]) ||
          (acl_db->host.get_host() &&
          !strcmp(host, acl_db->host.get_host())))
      {
        if ((!acl_db->db && !db[0]) ||
            (acl_db->db && !strcmp(db,acl_db->db)))
        {
          if (privileges)
            acl_db->access=privileges;
          else
          {
            acl_db= acl_dbs->erase(acl_db);
            // Don't increment loop variable.
            continue;
          }
        }
      }
    }
    ++acl_db;
  }
}


/*
  Insert a user/db/host combination into the global acl_cache

  SYNOPSIS
    acl_insert_db()
    user                User name
    host                Host name
    db                  Database name
    privileges          Bitmap of privileges

  NOTES
    acl_cache->lock must be locked when calling this
*/

void acl_insert_db(const char *user, const char *host, const char *db,
                   ulong privileges)
{
  ACL_DB acl_db;
  mysql_mutex_assert_owner(&acl_cache->lock);
  acl_db.user= strdup_root(&global_acl_memory,user);
  acl_db.host.update_hostname(*host ? strdup_root(&global_acl_memory, host) : 0);
  acl_db.db= strdup_root(&global_acl_memory, db);
  acl_db.access= privileges;
  acl_db.sort= get_sort(3,acl_db.host.get_host(), acl_db.db, acl_db.user);
  acl_dbs->push_back(acl_db);
  std::sort(acl_dbs->begin(), acl_dbs->end(), ACL_compare());
}


void get_mqh(const char *user, const char *host, USER_CONN *uc)
{
  ACL_USER *acl_user;

  mysql_mutex_lock(&acl_cache->lock);

  if (initialized && (acl_user= find_acl_user(host,user, FALSE)))
    uc->user_resources= acl_user->user_resource;
  else
    memset(&uc->user_resources, 0, sizeof(uc->user_resources));

  mysql_mutex_unlock(&acl_cache->lock);
}



/**
  Update the security context when updating the user

  Helper function.
  Update only if the security context is pointing to the same user.
  And return true if the update happens (i.e. we're operating on the
  user account of the current user).
  Normalize the names for a safe compare.

  @param sctx           The security context to update
  @param acl_user_ptr   User account being updated
  @param expired        new value of the expiration flag
  @return               did the update happen ?
 */
bool
update_sctx_cache(Security_context *sctx, ACL_USER *acl_user_ptr, bool expired)
{
  const char *acl_host= acl_user_ptr->host.get_host();
  const char *acl_user= acl_user_ptr->user;
  const char *sctx_user= sctx->priv_user;
  const char *sctx_host= sctx->priv_host;

  if (!acl_host)
    acl_host= "";
  if(!acl_user)
    acl_user= "";
  if (!sctx_host)
    sctx_host= "";
  if (!sctx_user)
    sctx_user= "";

  if (!strcmp(acl_user, sctx_user) && !strcmp(acl_host, sctx_host))
  {
    sctx->password_expired= expired;
    return true;
  }

  return false;
}



#endif /* NO_EMBEDDED_ACCESS_CHECKS */

