/*
 Copyright 2011 Roman Vorobets

 This file is part of BeholdFS.

 BeholdFS is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 BeholdFS is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with BeholdFS.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <syslog.h>

#include "beholddb.h"
#include "fs.h"
#include "schema.h"

/*
struct beholddb_dir
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
};

typedef struct beholddb_dir beholddb_dir;
*/

char beholddb_tagchar;
int beholddb_new_locate;
static const char BEHOLDDB_NAME[] = ".beholddb";

static char *strrdup(const char *begin, const char *end)
{
  int len = end - begin;
  char *str = (char*)malloc(1 + len);

  memcpy(str, begin, len);
  str[len] = 0;
  return str;
}

static void beholddb_insert_tag(beholddb_tag_list_item **phead, const char *name)
{
	beholddb_tag_list_item *item = (beholddb_tag_list_item*)malloc(sizeof(beholddb_tag_list_item));
	item->name = name;
	item->next = *phead;
	*phead = item;
}

static void beholddb_delete_tag(beholddb_tag_list_item **pitem)
{
	beholddb_tag_list_item *save = *pitem;
	*pitem = save->next;
	free((char*)save->name);
	free(save);
}

static int beholddb_count_tags(beholddb_tag_list_item *head)
{
  int count = 0;

  for (; head; head = head->next)
    ++count;
  return count;
}

static void beholddb_free_tag_list(beholddb_tag_list *list)
{
	for (beholddb_tag_list_item **phead = &list->head; *phead; )
		beholddb_delete_tag(phead);
}

int beholddb_parse_path(const char *path, beholddb_path **pbpath)
{
	syslog(LOG_DEBUG, "beholddb_parse_path(path=%s)", path);

	int pathlen = strlen(path);
	beholddb_path *bpath = (beholddb_path*)malloc(sizeof(beholddb_path));
	char *pathptr = (char*)malloc(pathlen + 3);

	bpath->realpath = pathptr;
	bpath->basename = NULL;
	bpath->path.head = bpath->include.head = bpath->exclude.head = NULL;
	bpath->listing = 0;

	*pathptr++ = '.';
	while (*path)
	{
		if ('/' != *path++)
		{
			syslog(LOG_DEBUG, "beholddb_parse_path: path is relative");
			beholddb_free_path(bpath);
			*pbpath = NULL;
			return BEHOLDDB_ERROR;
		}
		switch (*path)
		{
		default:
			if (beholddb_tagchar != *path)
			{
				const char *item = path;

				*pathptr++ = '/';
				bpath->basename = pathptr;
				while (*path && '/' != *path)
					*pathptr++ = *path++;
				beholddb_insert_tag(&bpath->path, strrdup(item, path));
			} else
			do
			{
				// skip hash sign
				++path;

				// handle hash directory
				while ('/' == *path)
					++path;

				// handle empty tag
				if (!*path)
				{
					bpath->listing = 1;
					break;
				}

				// handle tag type
				beholddb_tag_list *list = '-' == *path ? ++path, &bpath->exclude : &bpath->include;
				const char *tag = path;

				// handle tag name
				while (*path && '/' != *path && beholddb_tagchar != *path)
					++path;

				// add new tag to the list
				beholddb_insert_tag(&list->head, strrdup(tag, path));

			} while (beholddb_tagchar == *path);

		case '/':
			; // remove redundant slashes
		case 0:
			; // remove trailing slash
		}
	}

	// null terminate path
	*pathptr++ = 0;

	syslog(LOG_DEBUG, "beholddb_parse_path: realpath=%s", bpath->realpath);
	*pbpath = bpath;
	return BEHOLDDB_OK;
}

int beholddb_free_path(beholddb_path *bpath)
{
	syslog(LOG_DEBUG, "beholddb_free_path(realpath=%s)", bpath->realpath);

	beholddb_free_tag_list(&bpath->include);
	beholddb_free_tag_list(&bpath->exclude);

	free((char*)bpath->realpath);
	free(bpath);
}

static int beholddb_get_name(const beholddb_path *bpath, char **pdb_name)
{
	if (!bpath->basename)
	{
		syslog(LOG_INFO, "beholddb_get_name: no basename for '%s'", bpath->realpath);
		return BEHOLDDB_ERROR;
	}

	int pathlen = bpath->basename - bpath->realpath;
	syslog(LOG_DEBUG, "beholddb_get_name: realpath=%p, basename=%p, pathlen=%d", bpath->realpath, bpath->basename, pathlen);

	*pdb_name = (char*)malloc(pathlen + sizeof(BEHOLDDB_NAME));
	memcpy(*pdb_name, bpath->realpath, pathlen);
	memcpy(*pdb_name + pathlen, BEHOLDDB_NAME, sizeof(BEHOLDDB_NAME));

	syslog(LOG_DEBUG, "beholddb_get_name: db_name=%s", *pdb_name);
	return BEHOLDDB_OK;
}

/*
typedef struct beholddb_object
{
} beholddb_object;

inline int beholddb_success(int rc)
{
  return SQLITE_OK == rc || SQLITE_DONE == rc || SQLITE_ROW == rc;
}
*/

/*
int beholddb_exec(sqlite3 *db, const char *sql)
{
	char *err;
	int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
	syslog(LOG_DEBUG, "beholddb_exec: sql=%s, rc=%d, err=%s", sql, rc, err ? err : "ok");
	sqlite3_free(err);
	return rc;
}
*/

int beholddb_exec(sqlite3 *db, const char *sql)
{
  int rc;
  sqlite3_stmt *stmt;

  (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
  (rc = sqlite3_step(stmt));
  sqlite3_finalize(stmt);
  return rc;//SQLITE_DONE == rc ? BEHOLDDB_OK : BEHOLDDB_ERROR;
}

int beholddb_exec_bind_text(sqlite3 *db, const char *sql, const char *text)
{
  int rc;
  sqlite3_stmt *stmt;

  (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
  (rc = sqlite3_bind_text(stmt, 1, text, -1, SQLITE_STATIC)) ||
  (rc = sqlite3_step(stmt));
  sqlite3_finalize(stmt);
  return rc;//SQLITE_DONE == rc ? BEHOLDDB_OK : BEHOLDDB_ERROR;
}

int beholddb_bind_text(sqlite3_stmt *stmt, const char *param, const char *value)
{
  int i = sqlite3_bind_parameter_index(stmt, param);
  return sqlite3_bind_text(stmt, i, value, -1, SQLITE_STATIC);
}

int beholddb_bind_int(sqlite3_stmt *stmt, const char *param, int value)
{
  int i = sqlite3_bind_parameter_index(stmt, param);
  return sqlite3_bind_int(stmt, i, value);
}

int beholddb_bind_int64(sqlite3_stmt *stmt, const char *param, sqlite3_int64 value)
{
  int i = sqlite3_bind_parameter_index(stmt, param);
  return sqlite3_bind_int64(stmt, i, value);
}

int beholddb_bind_blob(sqlite3_stmt *stmt, const char *param, const void *value, int size, void (*free)(void*))
{
  int i = sqlite3_bind_parameter_index(stmt, param);
  return sqlite3_bind_blob(stmt, i, value, size, free);
}

int beholddb_bind_null(sqlite3_stmt *stmt, const char *param)
{
  int i = sqlite3_bind_parameter_index(stmt, param);
  return sqlite3_bind_null(stmt, i);
}

int beholddb_begin(sqlite3 *db, const char *name)
{
  return name ?
    beholddb_exec_bind_text(db, "savepoint ?", name) :
    beholddb_exec(db, "begin");
}

int beholddb_end(sqlite3 *db, const char *name)
{
  return name ?
    beholddb_exec_bind_text(db, "release ?", name) :
    beholddb_exec(db, "end");
}

int beholddb_rollback(sqlite3 *db, const char *name)
{
  return name ?
    beholddb_exec_bind_text(db, "rollback to ?", name) :
    beholddb_exec(db, "rollback");
}

int beholddb_end_result(sqlite3 *db, const char *name, int rc)
{
  if (SQLITE_OK == rc || SQLITE_DONE == rc)
  {
    if (!beholddb_end(db, name))
      return BEHOLDDB_OK;
  }

  beholddb_rollback(db, name);
  return BEHOLDDB_ERROR;
}

typedef struct beholddb_tag_context
{
  int include;
  int exclude;
  beholddb_tag_list_set *tags;
} beholddb_tag_context;

typedef sqlite3_int64 beholddb_object;

static void beholddb_tags_step(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
  beholddb_tag_list_set *tags = (beholddb_tag_list_set*)argv[0];
  beholddb_tag_context *tctx =
    (beholddb_tag_context*)sqlite3_aggregate_context(ctx, sizeof(beholddb_tag_context));

  tctx->tags = tags;
  if (!tctx->exclude)
  {
    const char *tag = sqlite3_value_text(argv[1]);

    // TODO: optimize !!!
    for (beholddb_tag_list_item *include = tags->include.head; include; include = include->next)
      if (!strcmp(tag, include->name))
      {
        ++tctx->include; // TODO: works only with DISTINCT
        return;
      }
    for (beholddb_tag_list_item *exclude = tags->exclude.head; exclude; exclude = exclude->next)
      if (!strcmp(tag, exclude->name))
      {
        ++tctx->exclude; // TODO: works only with DISTINCT
        return;
      }
  }
}

static void beholddb_tags_final(sqlite3_context *ctx)
{
  beholddb_tag_context *tctx =
    (beholddb_tag_context*)sqlite3_aggregate_context(ctx, sizeof(beholddb_tag_context));
  beholddb_tag_list_set *tags = tctx->tags;

  sqlite3_result_int(ctx, tags && !tctx->exclude &&
    tctx->include == beholddb_count_tags(tags->include.head));
}

static void beholddb_include_exclude(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
  int include = (int)sqlite3_user_data(ctx);
  beholddb_tag_list_set *tags = (beholddb_tag_list_set*)argv[0];
  beholddb_tag_list *list = include ? &tags->include : &tags->exclude;
  const char *tag = sqlite3_value_text(argv[1]);
  int found = 0;

  for (beholddb_tag_list_item *item = list->head; item; item = item->next)
    if (!strcmp(tag, item->name))
    {
      found = 1;
      break;
    }
  sqlite3_result_int(ctx, found);
}

static int beholddb_init(sqlite3 *db)
{
  int rc;

  //sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_FKEY, 1, NULL);
  (rc = beholddb_exec(db, "pragma foreign_keys = on")) ||
  (rc = sqlite3_extended_result_codes(db, 1)) ||
  (rc = sqlite3_create_function_v2(db, "tags", 2, SQLITE_ANY, NULL, NULL, beholddb_tags_step, beholddb_tags_final, NULL)) ||
  (rc = sqlite3_create_function_v2(db, "include", 2, SQLITE_ANY, (void*)1, beholddb_include_exclude, NULL, NULL, NULL)) ||
  (rc = sqlite3_create_function_v2(db, "exclude", 2, SQLITE_ANY, (void*)0, beholddb_include_exclude, NULL, NULL, NULL));

  syslog(LOG_DEBUG, "beholddb_init: %d", rc);
  return rc ? BEHOLDDB_ERROR : BEHOLDDB_OK; // TODO: handle errors
}

static int beholddb_create_tables(sqlite3 *db)
{
  int rc;
  const char *sp = "tables";

  (rc = beholddb_begin(db, sp)) ||
  (rc = beholddb_exec(db,
    "create table if not exists objects "
    "( "
    "id integer primary key, "
    "id_parent integer references objects ( id ) on delete restrict, "
    "type integer, "
    "name text, "
    "unique ( id_parent, type, name ) "
    ") ")) ||
  (rc = beholddb_exec(db,
    "create index if not exists objects_parent on objects ( id_parent ) ")) ||
  (rc = beholddb_exec(db,
    "create index if not exists objects_name on objects ( name ) ")) ||
  (rc = beholddb_exec(db,
    "create table if not exists objects_owners "
    "( "
    "id_owner integer references objects ( id ) on delete cascade, "
    "id_object integer references objects ( id ) on delete cascade, "
    "unique ( id_owner, id_object ) " // on conflict ignore ?
    ") ")) ||
  (rc = beholddb_exec(db,
    "create index if not exists objects_owners_owner on objects_owners ( id_owner ) ")) ||
  (rc = beholddb_exec(db,
    "create index if not exists objects_owners_object on objects_owners ( id_object ) ")) ||
  (rc = beholddb_exec(db,
    "create table if not exists objects_tags "
    "( "
    "id_object integer references objects ( id ) on delete cascade, "
    "id_tag integer references objects ( id ) on delete cascade, "
    "unique ( id_object, id_tag ) " // on conflict ignore ?
    ") ")) ||
  (rc = beholddb_exec(db,
    "create index if not exists objects_tags_object on objects_tags ( id_object ) ")) ||
  (rc = beholddb_exec(db,
    "create index if not exists objects_tags_tag on objects_tags ( id_tag ) "));
  return beholddb_end_result(db, sp, rc);
}

static int beholddb_open_read(const beholddb_path *bpath, sqlite3 **pdb)
{
	syslog(LOG_DEBUG, "beholddb_open_read(path=%s)", bpath->realpath);

	int rc;
	char *db_name;

	// get name of metadata file
	if ((rc = beholddb_get_name(bpath, &db_name)))
	{
		syslog(LOG_INFO, "beholddb_open_read: rc=%d", rc);
		*pdb = NULL;
		return BEHOLDDB_OK; // if in root directory, assume success?
	}

	// open and initialize the database
	if ((rc = sqlite3_open_v2(db_name, pdb, SQLITE_OPEN_READONLY, NULL)) ||
		(rc = beholddb_init(*pdb)))
	{
		syslog(LOG_ERR, "beholddb_open_read error: rc=%d", rc);
		sqlite3_close(*pdb);
		*pdb = NULL;
	}

	free(db_name);
	return rc;
}

static int beholddb_open_write(const beholddb_path *bpath, sqlite3 **pdb)
{
	syslog(LOG_DEBUG, "beholddb_open_write(path=%s)", bpath->realpath);

	int rc;
	char *db_name;

	// get name of metadata file
	if ((rc = beholddb_get_name(bpath, &db_name)))
	{
		syslog(LOG_INFO, "beholddb_open_write: rc=%d", rc);
		*pdb = NULL;
		return BEHOLDDB_OK; // if in root directory, assume success?
	}

	// open the database
	;

	if ((rc = sqlite3_open_v2(db_name, pdb, SQLITE_OPEN_READWRITE, NULL)) &&
		SQLITE_CANTOPEN == rc)
	{
		syslog(LOG_INFO, "beholddb_open_write: create metadata file");
		sqlite3_close(*pdb);
		(rc = sqlite3_open_v2(db_name, pdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL));
	}
	if (rc || (rc = beholddb_init(*pdb)) || (rc = beholddb_create_tables(*pdb)))
	{
		syslog(LOG_ERR, "beholddb_open_write error: rc=%d", rc);
		sqlite3_close(*pdb);
		*pdb = NULL;
	}

	free(db_name);
	return rc;
}

// TODO: there should not be object type
static int beholddb_create_object(sqlite3 *db, int type, char *name, beholddb_object id_parent, beholddb_object *pid)
{
  int rc;
  beholddb_object id = -1;
  sqlite3_stmt *stmt;
  const char *sql, *sp = "create";

  rc = beholddb_begin(db, sp);
  if (!rc)
  {
    sql =
      "insert into objects "
      "( name, type, id_parent ) "
      "values "
      "( @name, @type, @id_parent )";
    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_text(stmt, "name", name)) ||
    (rc = beholddb_bind_int(stmt, "type", type)) ||
    (rc = -1 != id_parent ?
      beholddb_bind_int64(stmt, "id_parent", id_parent) :
      beholddb_bind_null(stmt, "id_parent")) ||
    (rc = sqlite3_step(stmt));
    sqlite3_finalize(stmt);
  }
  if (!rc)
  {
    id = sqlite3_last_insert_rowid(db);
    sql =
      "insert into objects_owners "
      "( id_owner, id_object ) "
      "select @id, @id "
      "union "
      "select id_owner, @id "
      "from objects_owners "
      "where id_object is @id_parent  ";
    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_int64(stmt, "id", id)) ||
    (rc = -1 != id_parent ?
      beholddb_bind_int64(stmt, "id_parent", id_parent) :
      beholddb_bind_null(stmt, "id_parent")) ||
    (rc = sqlite3_step(stmt));
    sqlite3_finalize(stmt);
  } else
  if (SQLITE_CONSTRAINT == rc)
  {
    sql =
      "select id "
      "from objects "
      "where name = @name "
      "and type = @type "
      "and id_parent is @id_parent ";
    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_text(stmt, "name", name)) ||
    (rc = beholddb_bind_int(stmt, "type", type)) ||
    (rc = -1 != id_parent ?
      beholddb_bind_int64(stmt, "id_parent", id_parent) :
      beholddb_bind_null(stmt, "id_parent")) ||
    (rc = sqlite3_step(stmt));
    if (SQLITE_ROW == rc)
    {
      id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  if (pid)
    *pid = id;
  return beholddb_end_result(db, sp, rc);
}

int beholddb_create_object_path(sqlite3 *db, int type, const beholddb_tag_list *path, beholddb_object *pid)
{
  int rc;
  beholddb_object id = -1;
  sqlite3_stmt *stmt;
  const char *sql, *sp = "create_path";

  rc = beholddb_begin(db, sp);
  if (!rc)
  {
    sql =
      "select id "
      "from objects "
      "where name is null "
      "and type = @type";
    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_int(db, "type", type)) ||
    (rc = sqlite3_step(stmt));
    if (SQLITE_ROW == rc)
    {
      id = sqlite3_column_int(stmt, 0);
      rc = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
  }
  for (beholddb_tag_list_item *item = path->head; !rc && item; item = item->next)
  {
    rc = beholddb_create_object(db, type, item->name, id, *id);
  }
  if (pid)
    *pid = id;
  return beholddb_end_result(db, sp, rc);
}

// TODO: there should not be object type
int beholddb_get_object(sqlite3 *db, int type, const beholddb_tag_list *path, const beholddb_tag_list_set *tags, beholddb_object *pid)
{
  int rc, tags = -1;
  beholddb_object id = -1;
  sqlite3_stmt *stmt;
  char *sql, *mem;
  //const char *sp = "get";

  //rc = beholddb_begin(db, sp);
  if (!rc)
  {
    // TODO: REMINDER: name=null is a special root object
    sql = mem = sqlite3_mprintf(
      "o.name is null and type = %d",
      type);
    for (beholddb_tag_list_item *item = path->head; item; item = item->next)
    {
      sql = sqlite3_mprintf(
        "o.id_parent = ( select o.id from objects o where %s ) and o.name = '%m'",
        sql, item.name);
      sqlite3_free(mem);
      mem = sql;
    }
    sql = sqlite3_mprintf(
      "select o.id, tags(@tags, distinct t.name) "
      "from objects o "
      "join objects_owners oo on oo.id_owner = o.id "
      "join objects_tags ot on ot.id_object = oo.id_object "
      "join objects_owners tt on tt.id_object = ot.id_tag "
      "join objects t on t.id = tt.id_owner "
      "where %s "
      "group by o.id ",
      sql);
    sqlite3_free(mem);

    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_blob(db, "tags", tags, sizeof(beholddb_tag_list_set), SQLITE_STATIC)) ||
    (rc = sqlite3_step(stmt));
    if (SQLITE_ROW == rc)
    {
      id = sqilte3_column_int64(stmt, 0);
      tags = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    sqlite3_free(sql);
  }
  //beholddb_end(db, sp);
  if (pid)
    *pid = id;
  switch (rc)
  {
  case SQLITE_DONE:
    // no rows returned - object not found
    return BEHOLDDB_NOTFOUND;
  case SQLITE_ROW:
    // row returned - object found, check tags
    return tags ? BEHOLDDB_OK : BEHOLDDB_HIDDEN;
  default:
    return BEHOLDDB_ERROR;
  }
}

static int beholddb_delete_object(sqlite3 *db, beholddb_object id)
{
  int rc;
  sqlite3_stmt *stmt;
  const char *sql, *sp = "delete";

  rc = beholddb_begin(db, sp);
  if (!rc)
  {
    sql =
      "delete from objects "
      "where id = @id";
    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_int64(stmt, "id", id)) ||
    (rc = sqlite3_step(stmt));
    sqlite3_finalize(stmt);
  }
  return beholddb_end_result(db, sp, rc);
}

static int beholddb_set_object_parent(sqlite3 *db, beholddb_object id, beholddb_object id_parent)
{
  int rc;
  sqlite3_stmt *stmt;
  const char *sql, *sp = "parent";

  rc = beholddb_begin(db, sp);
  if (SQLITE_DONE == rc)
  {
    sql =
      "delete from objects_owners "
      "where id_owner in ( "
      "select id_owner "
      "from objects_owners "
      "where id_object = "
      "( select id_parent from objects where id = @id ) ) "
      "and id_object in ( "
      "select id_object "
      "from objects_owners "
      "where id_owner = "
      "( select id from objects where id = @id ) ) ";
    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_int64(stmt, "id", id)) ||
    (rc = sqlite3_step(stmt));
    sqlite3_finalize(stmt);
  }
  if (SQLITE_DONE == rc)
  {
    sql =
      "insert into objects_owners "
      "( id_owner, id_object ) "
      "select own.id_owner, obj.id_object "
      "from objects_owners own, objects_owners obj "
      "where own.id_object = @id_parent "
      "and obj.id_owner = @id ";
    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_int64(stmt, "id", id)) ||
    (rc = beholddb_bind_int64(stmt, "id_parent", id_parent)) ||
    (rc = sqlite3_step(stmt));
    sqlite3_finalize(stmt);
  }
  return beholddb_end_result(db, sp, rc);
}

// TODO: tags should be object ids, not names!
static int beholddb_link_object(sqlite3 *db, beholddb_object id, int type, const beholddb_tag_list_set *tags)
{
  int rc, id;
  sqlite3_stmt *stmt;
  const char *sql, *sp = "link";

  (rc = beholddb_begin(db, sp));
  if (!rc)
  {
    sql =
      "delete from objects_tags "
      "where id = @id "
      "and exclude(@tags, (select name from objects o where o.id = objects_tags.id_tag)) ";
    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_int64(stmt, "id", id)) ||
    (rc = beholddb_bind_blob(db, "tags", tags, sizeof(beholddb_tag_list_set), SQLITE_STATIC)) ||
    (rc = sqlite3_step(stmt));
    sqlite3_finalize(stmt);
  }
  if (!rc)
  {
    sql =
      "delete from objects "
      "where type = @type "
      "and not exists(select 1 from objects_tags ot where ot.id_tag = objects.id) "
      "and not include(objects.name)";
    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_int(stmt, "type", type)) ||
    (rc = sqlite3_step(stmt));
    sqlite3_finalize(stmt);
  }
  if (!rc)
  {
    sql =
      "insert into objects "
      "( name, type, id_parent ) "
      "values "
      "( @name, @type, @id_parent ) ";
    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_int(stmt, "type", type)) ||
    (rc = beholddb_bind_null(stmt, "id_parent"));
    for (beholddb_tag_list_item *item = tags->include.head; !rc && item; item = item->next)
    {
      (rc = sqlite3_reset(stmt)) ||
      (rc = beholddb_bind_text(stmt, "name", item->name)) ||
      (rc = sqlite3_step(stmt));
      if (SQLITE_CONSTRAINT == rc)
        rc = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
  }
  if (!rc)
  {
    sql =
      "insert into objects_tags "
      "( id_object, id_tag ) "
      "select @id, id "
      "from objects "
      "where type = @type "
      "and include(@tags, name) ";
    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_int64(stmt, "id", id)) ||
    (rc = beholddb_bind_blob(db, "tags", tags, sizeof(beholddb_tag_list_set), SQLITE_STATIC)) ||
    (rc = sqlite3_step(stmt));
    sqlite3_finalize(stmt);
  }
  return beholddb_end_result(db, sp, rc);
}

typedef struct beholddb_iterator
{
  sqlite3 *db;
  sqlite3_stmt *stmt;
  const char *sp;
} beholddb_iterator;

int beholddb_open_object(sqlite3 *db, beholddb_object id, beholddb_tag_list_set *tags, beholddb_iterator **pit)
{
  int rc;
  sqlite3_stmt *stmt;
  const char *sql;
  const char *sp = "list_path";

  *pit = NULL;
  rc = beholddb_begin(db, sp);
  if (!rc)
  {
    // TODO: REMINDER: name=null is a special root object
    sql =
      "select o.name "
      "from objects o "
      "join objects_owners oo on oo.id_owner = o.id "
      "join objects_tags ot on ot.id_object = oo.id_object "
      "join objects_owners tt on tt.id_object = ot.id_tag "
      "join objects t on t.id = tt.id_owner "
      "where o.id_parent = @id "
      "group by o.id "
      "having tags(@tags, distinct t.name) ";

    (rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
    (rc = beholddb_bind_int64(db, "id", id)) ||
    (rc = beholddb_bind_blob(db, "tags", tags, sizeof(beholddb_tag_list_set), SQLITE_STATIC));
    if (!rc)
    {
      *pit = (beholddb_iterator*)sqlite3_malloc(sizeof(beholddb_iterator));
      (*pit)->db = db;
      (*pit)->sp = sp;
      (*pit)->stmt = stmt;
    }
  }
  if (rc)
  {
    beholddb_end(db, sp);
    return BEHOLDDB_ERROR;
  }
  return BEHOLDDB_OK;
}

int beholddb_next_item(beholddb_iterator *it, const char **item)
{
  switch (sqlite3_step(it->stmt))
  {
  case SQLITE_ROW:
    *item = sqlite3_column_text(it->stmt, 0);
    return BEHOLDDB_OK;
  case SQLITE_DONE:
    *item = NULL;
    return BEHOLDDB_DONE;
  default:
    *item = NULL;
    return BEHOLDDB_ERROR;
  }
}

int beholddb_close_object(beholddb_iterator *it)
{
  if (it)
  {
    sqlite3_finalize(it->stmt);
    beholddb_end(it->db, it->sp);
    sqlite3_free(it);
  }
  return BEHOLDDB_OK;
}

/*
int beholddb_set_object_time()
{
}
*/

/*
static int beholddb_begin_transaction(sqlite3 *db)
{
	return beholddb_exec(db, "begin transaction;");
}

static int beholddb_commit(sqlite3 *db)
{
	return beholddb_exec(db, "commit;");
}

static int beholddb_rollback(sqlite3 *db)
{
	return beholddb_exec(db, "rollback;");
}

static int beholddb_exec_bind_text(sqlite3 *db, const char *sql, const char *text)
{
	sqlite3_stmt *stmt;
	int rc;

	(rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) ||
	(rc = sqlite3_bind_text(stmt, 1, text, -1, SQLITE_STATIC)) ||
	SQLITE_DONE != (rc = sqlite3_step(stmt)) ||
	(rc = SQLITE_OK);

	syslog(LOG_DEBUG, "beholddb_exec_bind_text: sql=%s, text=%s, rc=%d, err=%s", sql, text, rc, sqlite3_errmsg(db));
	sqlite3_finalize(stmt);
	return rc;
}

static int beholddb_set_tags_worker(sqlite3 *db, const char *sql, const beholddb_tag_list *list)
{
	if (!list || !list->head)
		return BEHOLDDB_OK;

	int rc;
	sqlite3_stmt *stmt;

	syslog(LOG_DEBUG, "beholddb_set_tags(%s, list=%p)", sql, list);
	if (!(rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)))
	{
		for (beholddb_tag_list_item *item = list->head; !rc && item; item = item->next)
		{
			(rc = sqlite3_reset(stmt)) ||
			(rc = sqlite3_bind_text(stmt, 1, item->name, -1, SQLITE_STATIC)) ||
			SQLITE_DONE != (rc = sqlite3_step(stmt)) ||
			(rc = SQLITE_OK);
		}
	}

	if (rc)
		syslog(LOG_ERR, "beholddb_set_tags(%s): error %d", sql, rc); else
		syslog(LOG_DEBUG, "beholddb_set_tags(%s): ok", sql);
	sqlite3_finalize(stmt);
	return rc; // TODO: handle errors
}

static int beholddb_create_tags(sqlite3 *db, const beholddb_tag_list *list)
{
	return beholddb_set_tags_worker(db,
		"insert into tags ( name ) "
		"values ( ? )",
		list);
}

static const char *BEHOLDDB_DDL_FILES_TAGS =
	"create temp table if not exists include"
	"("
		"id integer primary key on conflict ignore,"
		"name text"
	");"
	"create temp table if not exists exclude"
	"("
		"id integer primary key on conflict ignore,"
		"name text"
	");";

static const char *BEHOLDDB_DDL_DIRS_TAGS =
	"create temp table if not exists dirs_include"
	"("
		"id integer primary key on conflict ignore,"
		"name text"
	");"
	"create temp table if not exists dirs_exclude"
	"("
		"id integer primary key on conflict ignore,"
		"name text"
	");";

static int beholddb_set_files_tags(sqlite3 *db,
	const beholddb_tag_list *include, const beholddb_tag_list *exclude)
{
	int rc;

	(rc = beholddb_exec(db, BEHOLDDB_DDL_FILES_TAGS)) ||
	(rc = beholddb_set_tags_worker(db,
		"insert into include "
		"select coalesce(t.id, -1), tt.name "
		"from ( select ? name ) tt "
		"left join tags t on t.name = tt.name",
		include)) ||
	(rc = beholddb_set_tags_worker(db,
		"insert into exclude "
		"select id, name from tags "
		"where name = ?",
		exclude));

	return rc;
}

static int beholddb_set_dirs_tags(sqlite3 *db,
	const beholddb_tag_list *include, const beholddb_tag_list *exclude)
{
	int rc;

	(rc = beholddb_exec(db, BEHOLDDB_DDL_DIRS_TAGS)) ||
	(rc = beholddb_set_tags_worker(db,
		"insert into dirs_include "
		"select id, name from tags "
		"where name = ?",
		include)) ||
	(rc = beholddb_set_tags_worker(db,
		"insert into dirs_exclude "
		"select id, name from tags "
		"where name = ?",
		exclude));

	return rc;
}

static int beholddb_get_tags_worker(sqlite3 *db, const char *sql, beholddb_tag_list *list)
{
	int rc;
	sqlite3_stmt *stmt;

	if (!list)
		return BEHOLDDB_OK;

	if (!(rc = sqlite3_prepare(db, sql, -1, &stmt, NULL)))
	{
		while (SQLITE_ROW == (rc = sqlite3_step(stmt)) || SQLITE_DONE == rc && (rc = SQLITE_OK))
		{
			int namelen = sqlite3_column_bytes(stmt, 0);
			char *name = namelen ? (char*)malloc(++namelen) : NULL;

			if (!name)
			{
				syslog(LOG_NOTICE, "beholddb_get_tags(%s): NULL tag", sql);
			} else
			{
				memcpy(name, sqlite3_column_text(stmt, 0), namelen);
				beholddb_insert_tag(&list->head, name);
			}
		}
	}

	if (rc)
		syslog(LOG_ERR, "beholddb_get_tags(%s): error %d", sql, rc);
	sqlite3_finalize(stmt);
	return rc;
}

static int beholddb_get_files_tags(sqlite3 *db, beholddb_tag_list_set *tags)
{
	int rc;

	(rc = beholddb_get_tags_worker(db, "select name from include", &tags->include));// ||
	(rc = beholddb_get_tags_worker(db, "select name from exclude", &tags->exclude));

	return rc; // TODO: fix error handling
}

static int beholddb_get_dirs_tags(sqlite3 *db, beholddb_tag_list_set *tags)
{
	int rc;

	(rc = beholddb_get_tags_worker(db, "select name from dirs_include", &tags->include));// ||
	(rc = beholddb_get_tags_worker(db, "select name from dirs_exclude", &tags->exclude));

	return rc; // TODO: fix error handling
}

static int beholddb_get_file_tags(sqlite3 *db, const char *file,
	beholddb_tag_list *files_tags, beholddb_tag_list *dirs_tags,
	int *type)
{
	// TODO: optimize?
	char *sql = sqlite3_mprintf(
		"create temp view v_files_tags as "
			"select t.name from tags t "
			"join files_tags ft on ft.id_tag = t.id "
			"join files f on f.id = ft.id_file "
			"where f.name = '%q';"

		"create temp view v_dirs_tags as "
			"select t.name from tags t "
			"join dirs_tags dt on dt.id_tag = t.id "
			"join files f on f.id = dt.id_file "
			"where f.name = '%q';",

		file, file);

	beholddb_exec(db, sql);
	sqlite3_free(sql);

	if (type)
	{
		int rc;
		sqlite3_stmt *stmt;
		const char *sql =
			"select type from files "
			"where name = ?";
		if (!(rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)))
		{
			(rc = sqlite3_bind_text(stmt, 1, file, -1, SQLITE_STATIC)) ||
			(rc = sqlite3_step(stmt));
			if (SQLITE_ROW == rc)
				*type = sqlite3_column_int(stmt, 1); else
				*type = 0;
		}
		sqlite3_finalize(stmt);
	}

	beholddb_get_tags_worker(db, "select name from v_files_tags", files_tags);
	beholddb_get_tags_worker(db, "select name from v_dirs_tags", dirs_tags);

	beholddb_exec(db,
		"drop view v_files_tags;"
		"drop view v_dirs_tags;");

	return SQLITE_OK; // TODO: handle errors
}

static int beholddb_readdir_worker(sqlite3_stmt *stmt, const char *name);

static int beholddb_locate_file_worker(sqlite3 *db, const beholddb_path *bpath)
{
	int rc;
	sqlite3_stmt *stmt;

	(rc = beholddb_set_files_tags(db, &bpath->include, &bpath->exclude)) ||
	(rc = sqlite3_prepare_v2(db, BEHOLDDB_DML_LOCATE, -1, &stmt, NULL)) ||
	(rc = beholddb_readdir_worker(stmt, bpath->basename));

	sqlite3_finalize(stmt);
	return rc;
}
*/

int beholddb_locate_file(const beholddb_path *bpath)
{
	syslog(LOG_DEBUG, "beholddb_locate_file(realpath=%s)", bpath->realpath);

	if (!bpath->basename)
	{
		syslog(LOG_INFO, "beholddb_locate_file: no base name in %s", bpath->realpath);
		return BEHOLDDB_OK;
	}

	if (bpath->listing)
		return BEHOLDDB_OK;

	// quick optimization
	if (!bpath->include.head && !bpath->exclude.head)
		return 0;

	int rc;
	sqlite3 *db;

	(rc = beholddb_open_read(bpath, &db)) ||
	(rc = beholddb_get_object(db, 0, &bpath->path));

	sqlite3_close(db);

	if (rc)
		syslog(LOG_ERR, "beholddb_locate_file: error %d", rc);
	return rc;
}

// find file by mixed path
// return error if not found
int beholddb_get_file(const char *path, beholddb_path **pbpath)
{
	syslog(LOG_DEBUG, "beholddb_get_file(path=%s)", path);

	int rc;

	(rc = beholddb_parse_path(path, pbpath)) ||
	(rc = beholddb_locate_file(*pbpath));

	if (rc)
		syslog(LOG_ERR, "beholddb_get_file: error %d", rc);
	return rc;
}

/*
static int beholddb_mark_object(const beholddb_path *bpath, const beholddb_tag_list_set *dirs_tags);

static int beholddb_mark_recursive(sqlite3 *db, const beholddb_path *bpath)
{
	int rc;
	beholddb_path parent;
	beholddb_tag_list_set dirs_tags;

	if (!bpath->basename)
	{
		syslog(LOG_DEBUG, "beholddb_mark_recursive: in root directory (%s)", bpath->realpath);
		return BEHOLDDB_OK;
	}

	int pathlen = bpath->basename - bpath->realpath - 1;
	char *path = (char*)malloc(pathlen + 1);

	memcpy(path, bpath->realpath, pathlen);
	path[pathlen] = 0;
	parent.realpath = path;
	parent.basename = strrchr(path, '/');
	if (parent.basename)
		++parent.basename;

	parent.include.head = bpath->include.head;
	parent.exclude.head = NULL;
	dirs_tags.include.head = NULL;
	dirs_tags.exclude.head = bpath->exclude.head;

	rc = beholddb_get_tags_worker(db,
		"select t.name from exclude t "
		"where not exists "
			"(select * from files_tags ft "
			"where ft.id_tag = t.id)",
		&parent.exclude);

	rc = beholddb_get_tags_worker(db,
		"select t.name from include t "
		"where not exists "
			"(select f.id, t.id from files f "
			"except "
			"select st.id_file, st.id_tag from strong_tags st)",
		&dirs_tags.include);

	rc = beholddb_mark_object(&parent, &dirs_tags);

	free(path);
	beholddb_free_tag_list(&parent.exclude);
	beholddb_free_tag_list(&dirs_tags.include);

	return BEHOLDDB_OK; // TODO: handle errors
}

static int beholddb_mark_worker(sqlite3 *db, const char *file, int *pchanges)
{
	int changes = 0;

	syslog(LOG_DEBUG, "beholddb_mark_worker(%s)", file);
	beholddb_exec_bind_text(db,
		"insert into files_tags ( id_file, id_tag ) "
		"select f.id, t.id "
		"from files f "
		"join include t "
		"where f.name = ?",
		file);
	changes += sqlite3_changes(db);

	beholddb_exec_bind_text(db,
		"delete from files_tags "
		"where id_file = "
			"( select id from files where name = ? ) "
		"and id_tag in "
			"( select id from exclude )",
		file);
	changes += sqlite3_changes(db);

	//if (dirs_tags)
	{
		beholddb_exec_bind_text(db,
			"insert into dirs_tags ( id_file, id_tag ) "
			"select f.id, t.id "
			"from files f "
			"join dirs_include t "
			"where f.name = ?",
			file);
		changes += sqlite3_changes(db);

		beholddb_exec_bind_text(db,
			"delete from dirs_tags "
			"where id_file = "
				"( select id from files where name = ? ) "
			"and id_tag in "
				"( select id from dirs_exclude )",
			file);
		changes += sqlite3_changes(db);
	}
	if (pchanges)
		*pchanges = changes;
	syslog(LOG_DEBUG, "beholddb_mark_worker: changes=%d", changes);
	return SQLITE_OK; // TODO: handle errors
}

static int beholddb_mark(sqlite3 *db, const beholddb_path *bpath, const beholddb_tag_list_set *dirs_tags)
{
	syslog(LOG_DEBUG, "beholddb_mark(path=%s, dirs=%p)", bpath->realpath, dirs_tags);

	if (!bpath->basename)
	{
		syslog(LOG_DEBUG, "beholddb_mark: no base name (%s)", bpath->realpath);
		return BEHOLDDB_OK;
	}

	int rc, changes;

	(rc = beholddb_create_tags(db, &bpath->include)) ||
	//??? (rc = beholddb_create_tags(db, dirs_include)) ||
	(rc = beholddb_set_files_tags(db, &bpath->include, &bpath->exclude)) ||
	(rc = beholddb_set_dirs_tags(db, &dirs_tags->include, &dirs_tags->exclude)) ||
	(rc = beholddb_mark_worker(db, bpath->basename, &changes)) ||
	changes && (rc = beholddb_mark_recursive(db, bpath));

	return rc;
}

static int beholddb_mark_object(const beholddb_path *bpath, const beholddb_tag_list_set *dirs_tags)
{
	syslog(LOG_DEBUG, "beholddb_mark_object(path=%s)", bpath->realpath);

	int rc;
	sqlite3 *db;

	(rc = beholddb_open_write(bpath, &db)) ||
	(rc = beholddb_mark(db, bpath, dirs_tags));

	sqlite3_close(db);

	if (rc)
		syslog(LOG_DEBUG, "beholddb_mark: error (%d)", rc);
	return rc;
}

static const char *no_tags[] = { NULL, NULL };

static int beholddb_create_file_with_tags(const beholddb_path *bpath,
	const beholddb_tag_list *files_tags, const beholddb_tag_list *dirs_tags,
	int type)
{
	syslog(LOG_DEBUG, "beholddb_create_file(realpath=%s)", bpath->realpath);

	if (!bpath->basename)
	{
		syslog(LOG_DEBUG, "beholddb_create_file: no basename (%s)", bpath->realpath);
		return BEHOLDDB_OK;
	}

	int rc;
	sqlite3 *db;

	if ((rc = beholddb_open_write(bpath, &db)))
	{
		syslog(LOG_DEBUG, "beholddb_create_file: error opening database (%d)", rc);
		return rc;
	}

	beholddb_begin_transaction(db);

	syslog(LOG_DEBUG, "beholddb_create_file: checkpoint 1");
	char *sql = sqlite3_mprintf(
		"insert into files ( type, name ) "
		"values ( %d, '%q' )", !!type, bpath->basename);
	rc = beholddb_exec(db, sql);
	sqlite3_free(sql);

	syslog(LOG_DEBUG, "beholddb_create_file: checkpoint 2");
	beholddb_create_tags(db, files_tags);
	if (type)
		beholddb_create_tags(db, dirs_tags); else
	{
		beholddb_create_tags(db, &bpath->include);
		beholddb_set_files_tags(db, &bpath->include, &bpath->exclude);
	}

 	beholddb_set_files_tags(db, files_tags, NULL);
	beholddb_set_dirs_tags(db, dirs_tags, NULL);

	syslog(LOG_DEBUG, "beholddb_create_file: checkpoint 3");
	beholddb_exec(db,
		"delete from include "
		"where id in "
		"(select id from exclude);"

		"insert into exclude "
		"select id, name from tags "
		"except "
		"select id, name from include;");

	int changes;

	syslog(LOG_DEBUG, "beholddb_create_file: checkpoint 4");
	beholddb_mark_worker(db, bpath->basename, &changes);

	beholddb_path rpath;

	syslog(LOG_DEBUG, "beholddb_create_file: checkpoint 5");
	memcpy(&rpath, bpath, sizeof(rpath));
	rpath.include.head = NULL;
	rpath.exclude.head = NULL;
	beholddb_get_files_tags(db, &rpath.tags);

	syslog(LOG_DEBUG, "beholddb_create_file: checkpoint 6");
	beholddb_mark_recursive(db, &rpath);

	syslog(LOG_DEBUG, "beholddb_create_file: checkpoint 7");
	beholddb_free_tag_list(&rpath.include);
	beholddb_free_tag_list(&rpath.exclude);

	syslog(LOG_DEBUG, "beholddb_create_file: checkpoint 8");
	beholddb_commit(db);
	sqlite3_close(db);

	return rc; // TODO: error handling
}
*/

int beholddb_create_file(const beholddb_path *bpath, int type)
{
	return beholddb_create_file_with_tags(bpath, NULL, NULL, type);
}

/*
static int beholddb_delete_file_with_tags(const beholddb_path *bpath,
	beholddb_tag_list *files_tags, beholddb_tag_list *dirs_tags,
	int *ptype)
{
	syslog(LOG_DEBUG, "beholddb_delete_file(realpath=%s)", bpath->realpath);

	if (!bpath->basename)
	{
		syslog(LOG_DEBUG, "beholddb_delete_file: no base name (%s)", bpath->realpath);
		return BEHOLDDB_OK;
	}

	int rc;
	sqlite3 *db;

	if ((rc = beholddb_open_write(bpath, &db)))
	{
		syslog(LOG_DEBUG, "beholddb_delete_file: error opening database (%d)", rc);
		return rc;
	}

	beholddb_begin_transaction(db);

	beholddb_get_file_tags(db, bpath->basename, files_tags, dirs_tags, ptype);
	beholddb_set_files_tags(db, NULL, NULL);
	beholddb_exec_bind_text(db,
		"delete from files "
		"where name = ?;",
		bpath->basename);
	beholddb_exec(db,
		"insert into exclude "
		"select t.id, t.name from tags t "
		"where not exists "
			"(select * from files_tags ft where ft.id_tag = t.id);"

		"delete from tags "
		"where id in "
			"(select id from exclude);"

		"insert into include "
		"select id, name from tags "
		"except "
		"select id, name from exclude;");

	beholddb_path rpath;

	syslog(LOG_DEBUG, "beholddb_delete_file: checkpoint 5");
	memcpy(&rpath, bpath, sizeof(rpath));
	rpath.include.head = NULL;
	rpath.exclude.head = NULL;
	beholddb_get_files_tags(db, &rpath.tags);

	syslog(LOG_DEBUG, "beholddb_delete_file: checkpoint 6");
	beholddb_mark_recursive(db, bpath);

	syslog(LOG_DEBUG, "beholddb_delete_file: checkpoint 7");
	beholddb_free_tag_list(&rpath.include);
	beholddb_free_tag_list(&rpath.exclude);

	syslog(LOG_DEBUG, "beholddb_delete_file: checkpoint 8");
	beholddb_commit(db);
	sqlite3_close(db);

	syslog(LOG_DEBUG, "beholddb_delete_file: result=%d", rc);
	return rc; // TODO: error handling
}
*/

int beholddb_delete_file(const beholddb_path *bpath)
{
	return beholddb_delete_file_with_tags(bpath, NULL, NULL, NULL);
}

/*
int beholddb_rename_file(const beholddb_path *oldbpath, const beholddb_path *newbpath)
{
	int type;
	beholddb_tag_list files_tags, dirs_tags;

	files_tags.head = NULL;
	dirs_tags.head = NULL;
	beholddb_delete_file_with_tags(oldbpath, &files_tags, &dirs_tags, &type);
	beholddb_create_file_with_tags(newbpath, &files_tags, &dirs_tags, type);

	beholddb_free_tag_list(&files_tags);
	beholddb_free_tag_list(&dirs_tags);
	return 0; // TODO: error handling
}

int beholddb_opendir(const beholddb_path *bpath, void **phandle)
{
	syslog(LOG_DEBUG, "beholddb_opendir(realpath=%s)", bpath->realpath);

	*phandle = NULL;

	if (!bpath->listing && !bpath->include.head && !bpath->exclude.head)
		return BEHOLDDB_OK;

	int rc;
	sqlite3 *db;
	sqlite3_stmt *stmt;

	int pathlen = strlen(bpath->realpath);
	char *path = (char*)malloc(pathlen + 3);
	beholddb_path db_path;

	memcpy(path, bpath->realpath, pathlen);
	path[pathlen++] = '/';
	db_path.realpath = path;
	db_path.basename = &path[pathlen];
	path[pathlen++] = '.';
	path[pathlen++] = 0;

	rc = beholddb_open_read(&db_path, &db);
	free(path);

	if (rc)
	{
		syslog(LOG_ERR, "beholddb_opendir: error opening database (%d)", rc);
		return BEHOLDDB_ERROR;
	}

	(rc = beholddb_set_files_tags(db, &bpath->include, &bpath->exclude));
	if (beholddb_new_locate && !bpath->listing)
	{
		rc ||
		(rc = sqlite3_exec(db, BEHOLDDB_DDL_FAST_LOCATE_START, NULL, NULL, NULL)) ||
		(rc = sqlite3_prepare_v2(db, BEHOLDDB_DML_FAST_LOCATE, -1, &stmt, NULL));
	} else
	{
		rc ||
		(rc = sqlite3_prepare_v2(db,
			bpath->listing ? BEHOLDDB_DML_TAG_LISTING : BEHOLDDB_DML_LOCATE,
			-1, &stmt, NULL));
	}
	if (rc)
		sqlite3_close(db); else
	{
		beholddb_dir *dir = (beholddb_dir*)malloc(sizeof(beholddb_dir));

		dir->db = db;
		dir->stmt = stmt;
		*phandle = (void*)dir;
	}

	if (rc)
		syslog(LOG_ERR, "beholddb_opendir: error %d", rc); else
		syslog(LOG_DEBUG, "beholddb_opendir: ok, handle=%p", *phandle);
	return rc;
}

static int beholddb_readdir_worker(sqlite3_stmt *stmt, const char *name)
{
	syslog(LOG_DEBUG, "beholddb_readdir(name=%s)", name);

	int rc;

	if ((rc = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC)))
	{
		syslog(LOG_ERR, "beholddb_readdir: error bind (%d)", rc);
		return BEHOLDDB_ERROR;
	}

	switch ((rc = sqlite3_step(stmt)))
	{
	case SQLITE_ROW:
		syslog(LOG_DEBUG, "beholddb_readdir: '%s' will be shown", name);
		sqlite3_reset(stmt);
		// filter out metadata file
		return strcmp(name, BEHOLDDB_NAME) ? BEHOLDDB_OK : BEHOLDDB_ERROR;

	case SQLITE_OK:
	case SQLITE_DONE:
		syslog(LOG_DEBUG, "beholddb_readdir: '%s' was filtered out", name);
		sqlite3_reset(stmt);
		// no such file
		return BEHOLDDB_ERROR;
	}

	syslog(LOG_ERR, "beholddb_readdir: step error (%d)", rc);
	return rc;
}

int beholddb_readdir(void *handle, const char *name)
{
	int rc;
	beholddb_dir *dir = (beholddb_dir*)handle;

	if (dir)
		rc = beholddb_readdir_worker(dir->stmt, name); else
		rc = strcmp(name, BEHOLDDB_NAME) ? BEHOLDDB_OK : BEHOLDDB_ERROR;

	return rc;
}

int beholddb_listdir(void *handle, const char **pname)
{
	syslog(LOG_DEBUG, "beholddb_listdir()");

	int rc;
	beholddb_dir *dir = (beholddb_dir*)handle;

	switch (rc = sqlite3_step(dir->stmt))
	{
	case SQLITE_ROW:
		*pname = sqlite3_column_text(dir->stmt, 0);
		syslog(LOG_DEBUG, "beholddb_listdir: added '%s'", *pname);
		rc = BEHOLDDB_OK;
		break;
	case SQLITE_DONE:
		sqlite3_finalize(dir->stmt);
		dir->stmt = NULL;
	default:
		*pname = NULL;
		syslog(LOG_DEBUG, "beholddb_listdir: no more entries");
		rc = BEHOLDDB_ERROR;
	}
	return rc;
}

int beholddb_closedir(void *handle)
{
	if (!handle)
		return BEHOLDDB_OK;

	beholddb_dir *dir = (beholddb_dir*)handle;

	if (beholddb_new_locate)
	{
		sqlite3_exec(dir->db, BEHOLDDB_DDL_FAST_LOCATE_STOP, NULL, NULL, NULL);
	}

	sqlite3_finalize(dir->stmt);
	sqlite3_close(dir->db);
	free(dir);

	return BEHOLDDB_OK;
}
*/

