/*
 *  Copyright (c) 1990 Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  getentry.c
 */

#include "portable.h"

#ifndef lint 
static char copyright[] = "@(#) Copyright (c) 1990 Regents of the University of Michigan.\nAll rights reserved.\n";
#endif

#include <stdio.h>
#include <stdlib.h>

#include <ac/ctype.h>
#include <ac/socket.h>
#include <ac/string.h>
#include <ac/time.h>

#include "ldap-int.h"

/* ARGSUSED */
LDAPMessage *
ldap_first_entry( LDAP *ld, LDAPMessage *chain )
{
	if( ld == NULL || chain == NULLMSG ) {
		return NULLMSG;
	}

	return chain->lm_msgtype == LDAP_RES_SEARCH_ENTRY
		? chain
		: ldap_next_entry( ld, chain );
}

/* ARGSUSED */
LDAPMessage *
ldap_next_entry( LDAP *ld, LDAPMessage *entry )
{
	if ( ld == NULL || entry == NULLMSG ) {
		return NULLMSG;
	}

	for (
		entry = entry->lm_chain;
		entry != NULLMSG;
		entry = entry->lm_chain )
	{
		if( entry->lm_msgtype == LDAP_RES_SEARCH_ENTRY ) {
			return( entry );
		}
	}

	return( NULLMSG );
}

/* ARGSUSED */
int
ldap_count_entries( LDAP *ld, LDAPMessage *chain )
{
	int	i;

	if ( ld == NULL ) {
		return -1;
	}

	for ( i = 0; chain != NULL; chain = chain->lm_chain ) {
		if( chain->lm_msgtype == LDAP_RES_SEARCH_ENTRY ) {
			i++;
		}
	}

	return( i );
}

int
ldap_get_entry_controls(
	LDAP *ld,
	LDAPMessage *entry, 
	LDAPControl ***serverctrls)
{
	int rc;
	BerElement be;

	if ( ld == NULL || serverctrls == NULL ||
		entry == NULL || entry->lm_msgtype == LDAP_RES_SEARCH_ENTRY )
	{
		return LDAP_PARAM_ERROR;
	}

	/* make a local copy of the BerElement */
	SAFEMEMCPY(&be, entry->lm_ber, sizeof(be));

	if ( ber_scanf( &be, "{xx" /*}*/ ) == LBER_ERROR ) {
		rc = LDAP_DECODING_ERROR;
		goto cleanup_and_return;
	}

	rc = ldap_get_ber_controls( &be, serverctrls );

cleanup_and_return:
	if( rc != LDAP_SUCCESS ) {
		ld->ld_errno = rc;

		if( ld->ld_matched != NULL )
			free( ld->ld_matched );

		ld->ld_matched = NULL;

		if( ld->ld_error != NULL )
			free( ld->ld_error );

		ld->ld_error = NULL;
	}

	return rc;
}
