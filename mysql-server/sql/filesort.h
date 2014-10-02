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

#ifndef FILESORT_INCLUDED
#define FILESORT_INCLUDED

#include "my_global.h"                          /* uint, uchar */
#include "my_base.h"                            /* ha_rows */
#include "sql_list.h"                           /* Sql_alloc */
class THD;
struct TABLE;
typedef struct st_sort_field SORT_FIELD;
typedef struct st_order ORDER;
class Addon_fields;
class Field;


class QEP_TAB;

/**
  Sorting related info.
*/
class Filesort: public Sql_alloc
{
public:
  /** List of expressions to order the table by */
  ORDER *order;
  /** Number of records to return */
  ha_rows limit;
  /** ORDER BY list with some precalculated info for filesort */
  SORT_FIELD *sortorder;
  /** true means we are using Priority Queue for order by with limit. */
  bool using_pq;
  /** Addon fields descriptor */
  Addon_fields *addon_fields;

  Filesort(ORDER *order_arg, ha_rows limit_arg):
    order(order_arg),
    limit(limit_arg),
    sortorder(NULL),
    using_pq(false),
    addon_fields(NULL)
  {
    DBUG_ASSERT(order);
  };

  /* Prepare ORDER BY list for sorting. */
  uint make_sortorder();

  Addon_fields *get_addon_fields(ulong max_length_for_sort_data,
                                 Field **ptabfield,
                                 uint sortlength, uint *plength,
                                 uint *ppackable_length);
private:
  void cleanup();
};

ha_rows filesort(THD *thd, QEP_TAB *qep_tab, Filesort *fsort, bool sort_positions,
                 ha_rows *examined_rows, ha_rows *found_rows);
void filesort_free_buffers(TABLE *table, bool full);
void change_double_for_sort(double nr,uchar *to);

/// Declared here so we can unit test it.
uint sortlength(THD *thd, SORT_FIELD *sortorder, uint s_length,
                bool *multi_byte_charset);

#endif /* FILESORT_INCLUDED */
