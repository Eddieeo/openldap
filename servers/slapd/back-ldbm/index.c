/* index.c - routines for dealing with attribute indexes */
/* $OpenLDAP$ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"
#include "back-ldbm.h"

static slap_mask_t index_mask(
	Backend *be,
	AttributeDescription *desc,
	char **dbname,
	struct berval *atname )
{
	AttributeType *at;
	slap_mask_t mask = 0;

	attr_mask( be->be_private, desc, &mask );

	if( mask ) {
		*atname = desc->ad_cname;
		*dbname = desc->ad_cname.bv_val;
		return mask;
	}

	/* If there is a language tag, did we ever index the base
	 * type? If so, check for mask, otherwise it's not there.
	 */
	if( slap_ad_is_lang( desc ) && desc != desc->ad_type->sat_ad ) {
		/* has language tag */
		attr_mask( be->be_private, desc->ad_type->sat_ad, &mask );

		if( !( mask & SLAP_INDEX_NOLANG ) ) {
			*atname = desc->ad_type->sat_cname;
			*dbname = desc->ad_type->sat_cname.bv_val;
			return mask;
		}
	}

	/* see if supertype defined mask for its subtypes */
	for( at = desc->ad_type; at != NULL ; at = at->sat_sup ) {
		/* If no AD, we've never indexed this type */
		if (!at->sat_ad)
			continue;
		
		attr_mask( be->be_private, at->sat_ad, &mask );

		if( mask & SLAP_INDEX_AUTO_SUBTYPES ) {
			*atname = desc->ad_type->sat_cname;
			*dbname = at->sat_cname.bv_val;
			return mask;
		}
		if( !( mask & SLAP_INDEX_NOSUBTYPES ) ) {
			*atname = at->sat_cname;
			*dbname = at->sat_cname.bv_val;
			return mask;
		}

		if( mask ) break;
	}

	return 0;
}

int index_is_indexed(
	Backend *be,
	AttributeDescription *desc )
{
	int rc;
	slap_mask_t mask;
	char *dbname;
	struct berval prefix;

	mask = index_mask( be, desc, &dbname, &prefix );

	if( mask == 0 ) {
		return LDAP_INAPPROPRIATE_MATCHING;
	}

    	return LDAP_SUCCESS;
}

int index_param(
	Backend *be,
	AttributeDescription *desc,
	int ftype,
	char **dbnamep,
	slap_mask_t *maskp,
	struct berval *prefixp )
{
	slap_mask_t mask;
	char *dbname;

	mask = index_mask( be, desc, &dbname, prefixp );

	if( mask == 0 ) {
		return LDAP_INAPPROPRIATE_MATCHING;
	}

	switch( ftype ) {
	case LDAP_FILTER_PRESENT:
		if( IS_SLAP_INDEX( mask, SLAP_INDEX_PRESENT ) ) {
			goto done;
		}
		break;

	case LDAP_FILTER_APPROX:
		if( IS_SLAP_INDEX( mask, SLAP_INDEX_APPROX ) ) {
			goto done;
		}
		/* fall thru */

	case LDAP_FILTER_EQUALITY:
		if( IS_SLAP_INDEX( mask, SLAP_INDEX_EQUALITY ) ) {
			goto done;
		}
		break;

	case LDAP_FILTER_SUBSTRINGS:
		if( IS_SLAP_INDEX( mask, SLAP_INDEX_SUBSTR ) ) {
			goto done;
		}
		break;

	default:
		return LDAP_OTHER;
	}

	return LDAP_INAPPROPRIATE_MATCHING;

done:
	*dbnamep = dbname;
	*maskp = mask;
	return LDAP_SUCCESS;
}

static int indexer(
	Backend *be,
	char *dbname,
	struct berval *atname,
	BerVarray vals,
	ID id,
	int op,
	slap_mask_t mask )
{
	int rc, i;
	const char *text;
    DBCache	*db;
	AttributeDescription *ad = NULL;
	struct berval *keys;

	assert( mask );

	rc = slap_bv2ad( atname, &ad, &text );

	if( rc != LDAP_SUCCESS ) return rc;

	db = ldbm_cache_open( be, dbname, LDBM_SUFFIX, LDBM_WRCREAT );
	
	if ( db == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "index", LDAP_LEVEL_ERR,
			   "index_read: Could not open db %s%s\n",
			   dbname, LDBM_SUFFIX ));
#else
		Debug( LDAP_DEBUG_ANY,
		    "<= index_read NULL (could not open %s%s)\n",
			dbname, LDBM_SUFFIX, 0 );
#endif

		return LDAP_OTHER;
	}

	if( IS_SLAP_INDEX( mask, SLAP_INDEX_PRESENT ) ) {
		key_change( be, db, atname, id, op );
	}

	if( IS_SLAP_INDEX( mask, SLAP_INDEX_EQUALITY ) ) {
		rc = ad->ad_type->sat_equality->smr_indexer(
			LDAP_FILTER_EQUALITY,
			mask,
			ad->ad_type->sat_syntax,
			ad->ad_type->sat_equality,
			atname, vals, &keys );

		if( rc == LDAP_SUCCESS && keys != NULL ) {
			for( i=0; keys[i].bv_val != NULL; i++ ) {
				key_change( be, db, &keys[i], id, op );
			}
			ber_bvarray_free( keys );
		}
	}

	if( IS_SLAP_INDEX( mask, SLAP_INDEX_APPROX ) ) {
		rc = ad->ad_type->sat_approx->smr_indexer(
			LDAP_FILTER_APPROX,
			mask,
			ad->ad_type->sat_syntax,
			ad->ad_type->sat_approx,
			atname, vals, &keys );

		if( rc == LDAP_SUCCESS && keys != NULL ) {
			for( i=0; keys[i].bv_val != NULL; i++ ) {
				key_change( be, db, &keys[i], id, op );
			}
			ber_bvarray_free( keys );
		}
	}

	if( IS_SLAP_INDEX( mask, SLAP_INDEX_SUBSTR ) ) {
		rc = ad->ad_type->sat_substr->smr_indexer(
			LDAP_FILTER_SUBSTRINGS,
			mask,
			ad->ad_type->sat_syntax,
			ad->ad_type->sat_substr,
			atname, vals, &keys );

		if( rc == LDAP_SUCCESS && keys != NULL ) {
			for( i=0; keys[i].bv_val != NULL; i++ ) {
				key_change( be, db, &keys[i], id, op );
			}
			ber_bvarray_free( keys );
		}
	}

	ldbm_cache_close( be, db );
	return LDAP_SUCCESS;
}

static int index_at_values(
	Backend *be,
	AttributeType *type,
	struct berval *lang,
	BerVarray vals,
	ID id,
	int op,
	char ** dbnamep,
	slap_mask_t *maskp )
{
	slap_mask_t mask = 0;
	slap_mask_t tmpmask = 0;

	if( type->sat_sup ) {
		/* recurse */
		(void) index_at_values( be,
			type->sat_sup, lang,
			vals, id, op,
			dbnamep, &tmpmask );
	}

	/* If this type has no AD, we've never used it before */
	if( type->sat_ad ) {
		attr_mask( be->be_private, type->sat_ad, &mask );
	}

	if( mask ) {
		*dbnamep = type->sat_cname.bv_val;
	} else if ( tmpmask & SLAP_INDEX_AUTO_SUBTYPES ) {
		mask = tmpmask;
	}

	if( mask ) {
		indexer( be, *dbnamep,
			&type->sat_cname,
			vals, id, op,
			mask );
	}

	if( lang->bv_len ) {
		char *dbname = NULL;
		struct berval lname;
		AttributeDescription *desc;

		tmpmask = 0;
		lname.bv_val = NULL;

		desc = ad_find_lang(type, lang);
		if( desc ) {
			attr_mask( be->be_private, desc, &tmpmask );
		}

		if( tmpmask ) {
			dbname = desc->ad_cname.bv_val;
			lname = desc->ad_cname;
			mask = tmpmask;
		}

		if( dbname != NULL ) {
			indexer( be, dbname, &lname,
				vals, id, op,
				mask );
			if( !tmpmask ) {
				ch_free( lname.bv_val );
			}
		}
	}

	return LDAP_SUCCESS;
}

int index_values(
	Backend *be,
	AttributeDescription *desc,
	BerVarray vals,
	ID id,
	int op )
{
	char *dbname = NULL;
	slap_mask_t mask;

	(void) index_at_values( be,
		desc->ad_type, &desc->ad_lang,
		vals, id, op,
		&dbname, &mask );

	return LDAP_SUCCESS;
}

int
index_entry(
    Backend	*be,
	int op,
    Entry *e,
	Attribute *ap )
{
#ifdef NEW_LOGGING
	LDAP_LOG(( "index", LDAP_LEVEL_ENTRY,
		   "index_entry: %s (%s)%ld\n",
		   op == SLAP_INDEX_ADD_OP ? "add" : "del",
		   e->e_dn, e->e_id ));
#else
	Debug( LDAP_DEBUG_TRACE, "=> index_entry_%s( %ld, \"%s\" )\n",
		op == SLAP_INDEX_ADD_OP ? "add" : "del",
		e->e_id, e->e_dn );
#endif

	/* add each attribute to the indexes */
	for ( ; ap != NULL; ap = ap->a_next ) {
		index_values( be, ap->a_desc, ap->a_vals, e->e_id, op );
	}

#ifdef NEW_LOGGING
	LDAP_LOG(( "index", LDAP_LEVEL_ENTRY,
		   "index_entry: success\n" ));
#else
	Debug( LDAP_DEBUG_TRACE, "<= index_entry_%s( %ld, \"%s\" ) success\n",
	    op == SLAP_INDEX_ADD_OP ? "add" : "del",
		e->e_id, e->e_dn );
#endif

	return LDAP_SUCCESS;
}

