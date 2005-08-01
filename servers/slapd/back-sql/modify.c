/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1999-2005 The OpenLDAP Foundation.
 * Portions Copyright 1999 Dmitry Kovalev.
 * Portions Copyright 2002 Pierangelo Masarati.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by Dmitry Kovalev for inclusion
 * by OpenLDAP Software.  Additional significant contributors include
 * Pierangelo Masarati.
 */

#include "portable.h"

#include <stdio.h>
#include <sys/types.h>
#include "ac/string.h"

#include "slap.h"
#include "proto-sql.h"

int
backsql_modify( Operation *op, SlapReply *rs )
{
	backsql_info		*bi = (backsql_info*)op->o_bd->be_private;
	SQLHDBC 		dbh = SQL_NULL_HDBC;
	backsql_oc_map_rec	*oc = NULL;
	backsql_srch_info	bsi = { 0 };
	Entry			m = { 0 }, *e = NULL;
	int			manageDSAit = get_manageDSAit( op );
	SQLUSMALLINT		CompletionType = SQL_ROLLBACK;

	/*
	 * FIXME: in case part of the operation cannot be performed
	 * (missing mapping, SQL write fails or so) the entire operation
	 * should be rolled-back
	 */
	Debug( LDAP_DEBUG_TRACE, "==>backsql_modify(): modifying entry \"%s\"\n",
		op->o_req_ndn.bv_val, 0, 0 );

	rs->sr_err = backsql_get_db_conn( op, &dbh );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modify(): "
			"could not get connection handle - exiting\n", 
			0, 0, 0 );
		/*
		 * FIXME: we don't want to send back 
		 * excessively detailed messages
		 */
		rs->sr_text = ( rs->sr_err == LDAP_OTHER )
			? "SQL-backend error" : NULL;
		goto done;
	}

	bsi.bsi_e = &m;
	rs->sr_err = backsql_init_search( &bsi, &op->o_req_ndn,
			LDAP_SCOPE_BASE, 
			SLAP_NO_LIMIT, SLAP_NO_LIMIT,
			(time_t)(-1), NULL, dbh, op, rs,
			slap_anlist_all_attributes,
			( BACKSQL_ISF_MATCHED | BACKSQL_ISF_GET_ENTRY ) );
	switch ( rs->sr_err ) {
	case LDAP_SUCCESS:
		break;

	case LDAP_REFERRAL:
		if ( manageDSAit && !BER_BVISNULL( &bsi.bsi_e->e_nname ) &&
				dn_match( &op->o_req_ndn, &bsi.bsi_e->e_nname ) )
		{
			rs->sr_err = LDAP_SUCCESS;
			rs->sr_text = NULL;
			rs->sr_matched = NULL;
			if ( rs->sr_ref ) {
				ber_bvarray_free( rs->sr_ref );
				rs->sr_ref = NULL;
			}
			break;
		}
		e = &m;
		/* fallthru */

	default:
		Debug( LDAP_DEBUG_TRACE, "backsql_modify(): "
			"could not retrieve modifyDN ID - no such entry\n", 
			0, 0, 0 );
		if ( !BER_BVISNULL( &m.e_nname ) ) {
			/* FIXME: should always be true! */
			e = &m;

		} else {
			e = NULL;
		}
		goto done;
	}

#ifdef BACKSQL_ARBITRARY_KEY
	Debug( LDAP_DEBUG_TRACE, "   backsql_modify(): "
		"modifying entry \"%s\" (id=%s)\n", 
		bsi.bsi_base_id.eid_dn.bv_val,
		bsi.bsi_base_id.eid_id.bv_val, 0 );
#else /* ! BACKSQL_ARBITRARY_KEY */
	Debug( LDAP_DEBUG_TRACE, "   backsql_modify(): "
		"modifying entry \"%s\" (id=%ld)\n", 
		bsi.bsi_base_id.eid_dn.bv_val, bsi.bsi_base_id.eid_id, 0 );
#endif /* ! BACKSQL_ARBITRARY_KEY */

	if ( get_assert( op ) &&
			( test_filter( op, &m, get_assertion( op ) )
			  != LDAP_COMPARE_TRUE ))
	{
		rs->sr_err = LDAP_ASSERTION_FAILED;
		e = &m;
		goto done;
	}

	oc = backsql_id2oc( bi, bsi.bsi_base_id.eid_oc_id );
	assert( oc != NULL );

	if ( !acl_check_modlist( op, &m, op->oq_modify.rs_modlist ) ) {
		rs->sr_err = LDAP_INSUFFICIENT_ACCESS;
		e = &m;
		goto done;
	}

	rs->sr_err = backsql_modify_internal( op, rs, dbh, oc,
			&bsi.bsi_base_id, op->oq_modify.rs_modlist );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		e = &m;
		goto do_transact;
	}

	if ( BACKSQL_CHECK_SCHEMA( bi ) ) {
		char		textbuf[ SLAP_TEXT_BUFLEN ] = { '\0' };

		backsql_entry_clean( op, &m );

		bsi.bsi_e = &m;
		rs->sr_err = backsql_id2entry( &bsi, &bsi.bsi_base_id );
		if ( rs->sr_err != LDAP_SUCCESS ) {
			e = &m;
			goto do_transact;
		}

		rs->sr_err = entry_schema_check( op, &m, NULL, 0,
			&rs->sr_text, textbuf, sizeof( textbuf ) );
		if ( rs->sr_err != LDAP_SUCCESS ) {
			Debug( LDAP_DEBUG_TRACE, "   backsql_add(\"%s\"): "
				"entry failed schema check -- aborting\n",
				m.e_name.bv_val, 0, 0 );
			e = NULL;
			goto do_transact;
		}
	}

do_transact:;
	/*
	 * Commit only if all operations succeed
	 */
	if ( rs->sr_err == LDAP_SUCCESS && !op->o_noop ) {
		CompletionType = SQL_COMMIT;
	}

	SQLTransact( SQL_NULL_HENV, dbh, CompletionType );

done:;
#ifdef SLAP_ACL_HONOR_DISCLOSE
	if ( e != NULL ) {
		if ( !access_allowed( op, e, slap_schema.si_ad_entry, NULL,
					ACL_DISCLOSE, NULL ) )
		{
			rs->sr_err = LDAP_NO_SUCH_OBJECT;
			rs->sr_text = NULL;
			rs->sr_matched = NULL;
			if ( rs->sr_ref ) {
				ber_bvarray_free( rs->sr_ref );
				rs->sr_ref = NULL;
			}
		}
	}
#endif /* SLAP_ACL_HONOR_DISCLOSE */

	send_ldap_result( op, rs );

	if ( !BER_BVISNULL( &bsi.bsi_base_id.eid_ndn ) ) {
		(void)backsql_free_entryID( op, &bsi.bsi_base_id, 0 );
	}

	if ( !BER_BVISNULL( &m.e_nname ) ) {
		backsql_entry_clean( op, &m );
	}

	if ( bsi.bsi_attrs != NULL ) {
		op->o_tmpfree( bsi.bsi_attrs, op->o_tmpmemctx );
	}

	Debug( LDAP_DEBUG_TRACE, "<==backsql_modify()\n", 0, 0, 0 );

	return rs->sr_err;
}

