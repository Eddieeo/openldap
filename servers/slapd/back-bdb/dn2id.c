/* dn2id.c - routines to deal with the dn2id index */
/* $OpenLDAP$ */
/*
 * Copyright 1998-2000 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>

#include "back-bdb.h"
#include "idl.h"

int
bdb_dn2id_add(
	BackendDB	*be,
	DB_TXN *txn,
	const char	*pdn,
	Entry		*e )
{
	int		rc;
	DBT		key, data;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	DB *db = bdb->bi_dn2id->bdi_db;

	Debug( LDAP_DEBUG_TRACE, "=> bdb_dn2id_add( \"%s\", 0x%08lx )\n",
		e->e_ndn, (long) e->e_id, 0 );
	assert( e->e_id != NOID );

	DBTzero( &key );
	key.size = strlen( e->e_ndn ) + 2;
	key.data = ch_malloc( key.size );
	((char *)key.data)[0] = DN_BASE_PREFIX;
	AC_MEMCPY( &((char *)key.data)[1], e->e_ndn, key.size - 1 );

	DBTzero( &data );
	data.data = (char *) &e->e_id;
	data.size = sizeof( e->e_id );

	/* store it -- don't override */
	rc = db->put( db, txn, &key, &data, DB_NOOVERWRITE );
	if( rc != 0 ) {
		Debug( LDAP_DEBUG_ANY, "=> bdb_dn2id_add: put failed: %s %d\n",
			db_strerror(rc), rc, 0 );
		goto done;
	}

	{
		((char *)(key.data))[0] = DN_ONE_PREFIX;

		if( pdn != NULL ) {
			key.size = strlen( pdn ) + 2;
			AC_MEMCPY( &((char*)key.data)[1],
				pdn, key.size - 1 );

			rc = bdb_idl_insert_key( be, db, txn, &key, e->e_id );

			if( rc != 0 ) {
				Debug( LDAP_DEBUG_ANY,
					"=> bdb_dn2id_add: parent (%s) insert failed: %d\n",
					pdn, rc, 0 );
				goto done;
			}
		}
	}

	{
		char **subtree = dn_subtree( be, e->e_ndn );

		if( subtree != NULL ) {
			int i;
			((char *)key.data)[0] = DN_SUBTREE_PREFIX;
			for( i=0; subtree[i] != NULL; i++ ) {
				key.size = strlen( subtree[i] ) + 2;
				AC_MEMCPY( &((char *)key.data)[1],
					subtree[i], key.size - 1 );

				rc = bdb_idl_insert_key( be, db, txn, &key,
					e->e_id );

				if( rc != 0 ) {
					Debug( LDAP_DEBUG_ANY,
						"=> bdb_dn2id_add: subtree (%s) insert failed: %d\n",
						subtree[i], rc, 0 );
					break;
				}
			}

			charray_free( subtree );
		}
	}

done:
	ch_free( key.data );
	Debug( LDAP_DEBUG_TRACE, "<= bdb_dn2id_add: %d\n", rc, 0, 0 );
	return rc;
}

int
bdb_dn2id_delete(
	BackendDB	*be,
	DB_TXN *txn,
	const char	*pdn,
	const char	*dn,
	ID		id )
{
	int		rc;
	DBT		key;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	DB *db = bdb->bi_dn2id->bdi_db;

	Debug( LDAP_DEBUG_TRACE, "=> bdb_dn2id_delete( \"%s\", 0x%08lx )\n",
		dn, id, 0 );

	DBTzero( &key );
	key.size = strlen( dn ) + 2;
	key.data = ch_malloc( key.size );
	((char *)key.data)[0] = DN_BASE_PREFIX;
	AC_MEMCPY( &((char *)key.data)[1], dn, key.size - 1 );

	/* delete it */
	rc = db->del( db, txn, &key, 0 );
	if( rc != 0 ) {
		Debug( LDAP_DEBUG_ANY, "=> bdb_dn2id_delete: delete failed: %s %d\n",
			db_strerror(rc), rc, 0 );
		goto done;
	}

	{
		((char *)(key.data))[0] = DN_ONE_PREFIX;

		if( pdn != NULL ) {
			key.size = strlen( pdn ) + 2;
			AC_MEMCPY( &((char*)key.data)[1],
				pdn, key.size - 1 );

			rc = bdb_idl_delete_key( be, db, txn, &key, id );

			if( rc != 0 ) {
				Debug( LDAP_DEBUG_ANY,
					"=> bdb_dn2id_delete: parent (%s) delete failed: %d\n",
					pdn, rc, 0 );
				goto done;
			}
		}
	}

	{
		char **subtree = dn_subtree( be, dn );

		if( subtree != NULL ) {
			int i;
			((char *)key.data)[0] = DN_SUBTREE_PREFIX;
			for( i=0; subtree[i] != NULL; i++ ) {
				key.size = strlen( subtree[i] ) + 2;
				AC_MEMCPY( &((char *)key.data)[1],
					subtree[i], key.size - 1 );

				rc = bdb_idl_delete_key( be, db, txn, &key, id );

				if( rc != 0 ) {
					Debug( LDAP_DEBUG_ANY,
						"=> bdb_dn2id_delete: subtree (%s) delete failed: %d\n",
						subtree[i], rc, 0 );
					charray_free( subtree );
					goto done;
				}
			}

			charray_free( subtree );
		}
	}

done:
	ch_free( key.data );
	Debug( LDAP_DEBUG_TRACE, "<= bdb_dn2id_delete %d\n", rc, 0, 0 );
	return rc;
}

int
bdb_dn2id(
	BackendDB	*be,
	DB_TXN *txn,
	const char	*dn,
	ID *id )
{
	int		rc;
	DBT		key, data;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	DB *db = bdb->bi_dn2id->bdi_db;

	Debug( LDAP_DEBUG_TRACE, "=> bdb_dn2id( \"%s\" )\n", dn, 0, 0 );

	DBTzero( &key );
	key.size = strlen( dn ) + 2;
	key.data = ch_malloc( key.size );
	((char *)key.data)[0] = DN_BASE_PREFIX;
	AC_MEMCPY( &((char *)key.data)[1], dn, key.size - 1 );

	/* store the ID */
	DBTzero( &data );
	data.data = id;
	data.ulen = sizeof(ID);
	data.flags = DB_DBT_USERMEM;

	/* fetch it */
	rc = db->get( db, txn, &key, &data, bdb->bi_db_opflags );

	if( rc != 0 ) {
		Debug( LDAP_DEBUG_TRACE, "<= bdb_dn2id: get failed: %s (%d)\n",
			db_strerror( rc ), rc, 0 );
	} else {
		Debug( LDAP_DEBUG_TRACE, "<= bdb_dn2id: got id=0x%08lx\n",
			*id, 0, 0 );
	}

	ch_free( key.data );
	return rc;
}

int
bdb_dn2id_matched(
	BackendDB	*be,
	DB_TXN *txn,
	const char	*in,
	ID *id,
	char **matchedDN )
{
	int		rc;
	DBT		key, data;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	DB *db = bdb->bi_dn2id->bdi_db;
	const char *dn = in;
	char *tmp = NULL;

	Debug( LDAP_DEBUG_TRACE, "=> bdb_dn2id_matched( \"%s\" )\n", dn, 0, 0 );

	DBTzero( &key );
	key.size = strlen( dn ) + 2;
	key.data = ch_malloc( key.size );
	((char *)key.data)[0] = DN_BASE_PREFIX;

	/* store the ID */
	DBTzero( &data );
	data.data = id;
	data.ulen = sizeof(ID);
	data.flags = DB_DBT_USERMEM;

	*matchedDN = NULL;

	while(1) {
		AC_MEMCPY( &((char *)key.data)[1], dn, key.size - 1 );

		*id = NOID;

		/* fetch it */
		rc = db->get( db, txn, &key, &data, bdb->bi_db_opflags );

		if( rc == DB_NOTFOUND ) {
			char *pdn = dn_parent( be, dn );
			ch_free( tmp );
			tmp = NULL;

			if( pdn == NULL || *pdn == '\0' ) {
				Debug( LDAP_DEBUG_TRACE,
					"<= bdb_dn2id_matched: no match\n",
					0, 0, 0 );
				ch_free( pdn );
				break;
			}

			dn = pdn;
			tmp = pdn;
			key.size = strlen( dn ) + 2;

		} else if ( rc == 0 ) {
			if( data.size != sizeof( ID ) ) {
				Debug( LDAP_DEBUG_ANY,
					"<= bdb_dn2id_matched: get size mismatch: "
					"expected %ld, got %ld\n",
					(long) sizeof(ID), (long) data.size, 0 );
				ch_free( tmp );
			}

			if( in != dn ) {
				*matchedDN = (char *) dn;
			}

			Debug( LDAP_DEBUG_TRACE,
				"<= bdb_dn2id_matched: id=0x%08lx: %s %s\n",
				(long) *id, *matchedDN == NULL ? "entry" : "matched", dn );
			break;

		} else {
			Debug( LDAP_DEBUG_ANY,
				"<= bdb_dn2id_matched: get failed: %s (%d)\n",
				db_strerror(rc), rc, 0 );
			ch_free( tmp );
			break;
		}
	}

	ch_free( key.data );
	return rc;
}

int
bdb_dn2id_children(
	BackendDB	*be,
	DB_TXN *txn,
	const char *dn )
{
	int		rc;
	DBT		key, data;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	DB *db = bdb->bi_dn2id->bdi_db;
	ID		id;

	Debug( LDAP_DEBUG_TRACE, "=> bdb_dn2id_children( %s )\n",
		dn, 0, 0 );

	DBTzero( &key );
	key.size = strlen( dn ) + 2;
	key.data = ch_malloc( key.size );
	((char *)key.data)[0] = DN_ONE_PREFIX;
	AC_MEMCPY( &((char *)key.data)[1], dn, key.size - 1 );

	/* we actually could do a empty get... */
	DBTzero( &data );
	data.data = &id;
	data.ulen = sizeof(id);
	data.flags = DB_DBT_USERMEM;
	data.doff = 0;
	data.dlen = sizeof(id);

	rc = db->get( db, txn, &key, &data, bdb->bi_db_opflags );

	Debug( LDAP_DEBUG_TRACE, "<= bdb_dn2id_children( %s ): %schildren (%d)\n",
		dn,
		rc == 0 ? "" : ( rc == DB_NOTFOUND ? "no " :
			db_strerror(rc) ), rc );

	return rc;
}

int
bdb_dn2idl(
	BackendDB	*be,
	const char	*dn,
	int prefix,
	ID *ids )
{
	int		rc;
	DBT		key, data;
	struct bdb_info *bdb = (struct bdb_info *) be->be_private;
	DB *db = bdb->bi_dn2id->bdi_db;

	Debug( LDAP_DEBUG_TRACE, "=> bdb_dn2idl( \"%s\" )\n", dn, 0, 0 );

	if (prefix == DN_SUBTREE_PREFIX && be_issuffix(be, dn))
	{
		BDB_IDL_ALL(bdb, ids);
		return 0;
	}

	DBTzero( &key );
	key.size = strlen( dn ) + 2;
	key.data = ch_malloc( key.size );
	((char *)key.data)[0] = prefix;
	AC_MEMCPY( &((char *)key.data)[1], dn, key.size - 1 );

	/* store the ID */
	DBTzero( &data );
	data.data = ids;
	data.ulen = BDB_IDL_UM_SIZEOF;	
	data.flags = DB_DBT_USERMEM;

	/* fetch it */
	rc = db->get( db, NULL, &key, &data, bdb->bi_db_opflags );

	if( rc != 0 ) {
		Debug( LDAP_DEBUG_TRACE,
			"<= bdb_dn2idl: get failed: %s (%d)\n",
			db_strerror( rc ), rc, 0 );

	} else {
		Debug( LDAP_DEBUG_TRACE,
			"<= bdb_dn2idl: id=%ld first=%ld last=%ld\n",
			(long) ids[0],
			(long) BDB_IDL_FIRST( ids ), (long) BDB_IDL_LAST( ids ) );
	}

	ch_free( key.data );
	return rc;
}
