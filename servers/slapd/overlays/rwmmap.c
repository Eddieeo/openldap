/* rwmmap.c - rewrite/mapping routines */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1999-2004 The OpenLDAP Foundation.
 * Portions Copyright 1999-2003 Howard Chu.
 * Portions Copyright 2000-2003 Pierangelo Masarati.
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
 * This work was initially developed by the Howard Chu for inclusion
 * in OpenLDAP Software and subsequently enhanced by Pierangelo
 * Masarati.
 */

#include "portable.h"

#ifdef SLAPD_OVER_RWM

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"
#include "rwm.h"

#undef ldap_debug	/* silence a warning in ldap-int.h */
#include "../../../libraries/libldap/ldap-int.h"

int
rwm_mapping_cmp( const void *c1, const void *c2 )
{
	struct ldapmapping *map1 = (struct ldapmapping *)c1;
	struct ldapmapping *map2 = (struct ldapmapping *)c2;
	int rc = map1->m_src.bv_len - map2->m_src.bv_len;
	
	if ( rc ) {
		return rc;
	}

	return strcasecmp( map1->m_src.bv_val, map2->m_src.bv_val );
}

int
rwm_mapping_dup( void *c1, void *c2 )
{
	struct ldapmapping *map1 = (struct ldapmapping *)c1;
	struct ldapmapping *map2 = (struct ldapmapping *)c2;
	int rc = map1->m_src.bv_len - map2->m_src.bv_len;

	if ( rc ) {
		return 0;
	}

	return ( ( strcasecmp( map1->m_src.bv_val, map2->m_src.bv_val ) == 0 ) ? -1 : 0 );
}

int
rwm_map_init( struct ldapmap *lm, struct ldapmapping **m )
{
	struct ldapmapping	*mapping;
	const char		*text;
	int			rc;

	assert( m );

	*m = NULL;
	
	mapping = (struct ldapmapping *)ch_calloc( 2, 
			sizeof( struct ldapmapping ) );
	if ( mapping == NULL ) {
		return;
	}

	rc = slap_str2ad( "objectClass", &mapping->m_src_ad, &text );
	if ( rc != LDAP_SUCCESS ) {
		return rc;
	}

	mapping->m_dst_ad = mapping->m_src_ad;
	ber_dupbv( &mapping->m_dst, &mapping->m_src_ad->ad_cname );
	ber_dupbv( &mapping->m_dst, &mapping->m_src );

	mapping[1].m_src = mapping->m_src;
	mapping[1].m_dst = mapping->m_dst;

	avl_insert( &lm->map, (caddr_t)mapping, 
			rwm_mapping_cmp, rwm_mapping_dup );
	avl_insert( &lm->remap, (caddr_t)&mapping[1], 
			rwm_mapping_cmp, rwm_mapping_dup );

	*m = mapping;

	return rc;
}

int
rwm_mapping( struct ldapmap *map, struct berval *s, struct ldapmapping **m, int remap )
{
	Avlnode *tree;
	struct ldapmapping fmapping;

	assert( m );

	if ( remap == RWM_REMAP ) {
		tree = map->remap;

	} else {
		tree = map->map;
	}

	fmapping.m_src = *s;
	*m = (struct ldapmapping *)avl_find( tree, (caddr_t)&fmapping,
			rwm_mapping_cmp );

	if ( *m == NULL ) {
		return map->drop_missing;
	}

	return 0;
}

void
rwm_map( struct ldapmap *map, struct berval *s, struct berval *bv, int remap )
{
	struct ldapmapping *mapping;

	BER_BVZERO( bv );
	rwm_mapping( map, s, &mapping, remap );
	if ( mapping != NULL ) {
		if ( !BER_BVISNULL( &mapping->m_dst ) ) {
			*bv = mapping->m_dst;
		}
		return;
	}

	if ( !map->drop_missing ) {
		*bv = *s;
	}
}

/*
 * Map attribute names in place
 */
int
rwm_map_attrnames(
		struct ldapmap *at_map,
		struct ldapmap *oc_map,
		AttributeName *an,
		AttributeName **anp,
		int remap
)
{
	int		i, j;

	if ( an == NULL ) {
		return LDAP_SUCCESS;
	}

	for ( i = 0; !BER_BVISNULL( &an[i].an_name ); i++ )
		/* just count */ ;
	*anp = ch_malloc( ( i + 1 )* sizeof( AttributeName ) );

	for ( i = 0, j = 0; !BER_BVISNULL( &an[i].an_name ); i++ ) {
		struct ldapmapping	*m;
		int			at_drop_missing = 0,
					oc_drop_missing = 0;

		if ( an[i].an_desc ) {
			if ( !at_map ) {
				/* FIXME: better leave as is? */
				continue;
			}
				
			at_drop_missing = rwm_mapping( at_map, &an[i].an_name, &m, remap );
			if ( at_drop_missing || ( m && BER_BVISNULL( &m->m_dst ) ) ) {
				continue;
			}

			if ( !m ) {
				(*anp)[j] = an[i];
				j++;
				continue;
			}

			(*anp)[j] = an[i];
			if ( remap == RWM_MAP ) {
				(*anp)[j].an_name = m->m_dst;
				(*anp)[j].an_desc = m->m_dst_ad;
			} else {
				(*anp)[j].an_name = m->m_src;
				(*anp)[j].an_desc = m->m_src_ad;

			}

			j++;
			continue;

		} else if ( an[i].an_oc ) {
			if ( !oc_map ) {
				/* FIXME: better leave as is? */
				continue;
			}

			oc_drop_missing = rwm_mapping( oc_map, &an[i].an_name, &m, remap );

			if ( oc_drop_missing || ( m && BER_BVISNULL( &m->m_dst ) ) ) {
				continue;
			}

			if ( !m ) {
				(*anp)[j] = an[i];
				j++;
				continue;
			}

			(*anp)[j] = an[i];
			if ( remap == RWM_MAP ) {
				(*anp)[j].an_name = m->m_dst;
				(*anp)[j].an_oc = m->m_dst_oc;
			} else {
				(*anp)[j].an_name = m->m_src;
				(*anp)[j].an_oc = m->m_src_oc;
			}

		} else {
			at_drop_missing = rwm_mapping( at_map, &an[i].an_name, &m, remap );
		
			if ( at_drop_missing || !m ) {

				oc_drop_missing = rwm_mapping( oc_map, &an[i].an_name, &m, remap );

				/* if both at_map and oc_map required to drop missing,
				 * then do it */
				if ( oc_drop_missing && at_drop_missing ) {
					continue;
				}

				/* if no oc_map mapping was found and at_map required
				 * to drop missing, then do it; otherwise, at_map wins
				 * and an is considered an attr and is left unchanged */
				if ( !m ) {
					if ( at_drop_missing ) {
						continue;
					}
					(*anp)[j] = an[i];
					j++;
					continue;
				}
	
				if ( BER_BVISNULL( &m->m_dst ) ) {
					continue;
				}

				(*anp)[j] = an[i];
				if ( remap == RWM_MAP ) {
					(*anp)[j].an_name = m->m_dst;
					(*anp)[j].an_oc = m->m_dst_oc;
				} else {
					(*anp)[j].an_name = m->m_src;
					(*anp)[j].an_oc = m->m_src_oc;
				}
				j++;
				continue;
			}

			if ( !BER_BVISNULL( &m->m_dst ) ) {
				(*anp)[j] = an[i];
				if ( remap == RWM_MAP ) {
					(*anp)[j].an_name = m->m_dst;
					(*anp)[j].an_desc = m->m_dst_ad;
				} else {
					(*anp)[j].an_name = m->m_src;
					(*anp)[j].an_desc = m->m_src_ad;
				}
				j++;
				continue;
			}
		}
	}

	if ( j == 0 && i != 0 ) {
		memset( &(*anp)[0], 0, sizeof( AttributeName ) );
		BER_BVSTR( &(*anp)[0].an_name, LDAP_NO_ATTRS );
	}
	memset( &(*anp)[j], 0, sizeof( AttributeName ) );

	return LDAP_SUCCESS;
}

int
rwm_map_attrs(
		struct ldapmap *at_map,
		AttributeName *an,
		int remap,
		char ***mapped_attrs
)
{
	int i, j;
	char **na;

	if ( an == NULL ) {
		*mapped_attrs = NULL;
		return LDAP_SUCCESS;
	}

	for ( i = 0; !BER_BVISNULL( &an[i].an_name ); i++ ) {
		/*  */
	}

	na = (char **)ch_calloc( i + 1, sizeof( char * ) );
	if (na == NULL) {
		*mapped_attrs = NULL;
		return LDAP_NO_MEMORY;
	}

	for ( i = j = 0; !BER_BVISNULL( &an[i].an_name ); i++ ) {
		struct ldapmapping	*m;
		
		if ( rwm_mapping( at_map, &an[i].an_name, &m, remap ) ) {
			continue;
		}

		if ( !m || ( m && !BER_BVISNULL( &m->m_dst ) ) ) {
			na[j++] = m->m_dst.bv_val;
		}
	}
	if ( j == 0 && i != 0 ) {
		na[j++] = LDAP_NO_ATTRS;
	}
	na[j] = NULL;

	*mapped_attrs = na;
	return LDAP_SUCCESS;
}

static int
map_attr_value(
		dncookie		*dc,
		AttributeDescription 	*ad,
		struct berval		*mapped_attr,
		struct berval		*value,
		struct berval		*mapped_value,
		int			remap )
{
	struct berval		vtmp;
	int			freeval = 0;

	rwm_map( &dc->rwmap->rwm_at, &ad->ad_cname, mapped_attr, remap );
	if ( BER_BVISNULL( mapped_attr ) || BER_BVISEMPTY( mapped_attr ) ) {
		/*
		 * FIXME: are we sure we need to search oc_map if at_map fails?
		 */
		rwm_map( &dc->rwmap->rwm_oc, &ad->ad_cname, mapped_attr, remap );
		if ( BER_BVISNULL( mapped_attr ) || BER_BVISEMPTY( mapped_attr ) )
		{
			*mapped_attr = ad->ad_cname;
		}
	}

	if ( value == NULL ) {
		return 0;
	}

	if ( ad->ad_type->sat_syntax == slap_schema.si_syn_distinguishedName )
	{
		dncookie 	fdc = *dc;
		int		rc;

#ifdef ENABLE_REWRITE
		fdc.ctx = "searchFilterAttrDN";
#endif

		rc = rwm_dn_massage( &fdc, value, &vtmp, NULL );
		switch ( rc ) {
		case LDAP_SUCCESS:
			if ( vtmp.bv_val != value->bv_val ) {
				freeval = 1;
			}
			break;
		
		case LDAP_UNWILLING_TO_PERFORM:
		case LDAP_OTHER:
		default:
			return -1;
		}

	} else if ( ad == slap_schema.si_ad_objectClass
			|| ad == slap_schema.si_ad_structuralObjectClass )
	{
		rwm_map( &dc->rwmap->rwm_oc, value, &vtmp, remap );
		if ( BER_BVISNULL( &vtmp ) || BER_BVISEMPTY( &vtmp ) ) {
			vtmp = *value;
		}
		
	} else {
		vtmp = *value;
	}

	filter_escape_value( &vtmp, mapped_value );

	if ( freeval ) {
		ber_memfree( vtmp.bv_val );
	}
	
	return 0;
}

static int
rwm_int_filter_map_rewrite(
		dncookie		*dc,
		Filter			*f,
		struct berval		*fstr )
{
	int		i;
	Filter		*p;
	struct berval	atmp,
			vtmp,
			tmp;
	static struct berval
			ber_bvfalse = BER_BVC( "(?=false)" ),
			ber_bvtrue = BER_BVC( "(?=true)" ),
			ber_bvundefined = BER_BVC( "(?=undefined)" ),
			ber_bverror = BER_BVC( "(?=error)" ),
			ber_bvunknown = BER_BVC( "(?=unknown)" ),
			ber_bvnone = BER_BVC( "(?=none)" );
	ber_len_t	len;

	if ( f == NULL ) {
		ber_dupbv( fstr, &ber_bvnone );
		return -1;
	}

	switch ( f->f_choice ) {
	case LDAP_FILTER_EQUALITY:
		if ( map_attr_value( dc, f->f_av_desc, &atmp,
					&f->f_av_value, &vtmp, RWM_MAP ) )
		{
			return -1;
		}

		fstr->bv_len = atmp.bv_len + vtmp.bv_len + STRLENOF( "(=)" );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s=%s)",
			atmp.bv_val, vtmp.bv_val );

		ber_memfree( vtmp.bv_val );
		break;

	case LDAP_FILTER_GE:
		if ( map_attr_value( dc, f->f_av_desc, &atmp,
					&f->f_av_value, &vtmp, RWM_MAP ) )
		{
			return -1;
		}

		fstr->bv_len = atmp.bv_len + vtmp.bv_len + STRLENOF( "(>=)" );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s>=%s)",
			atmp.bv_val, vtmp.bv_val );

		ber_memfree( vtmp.bv_val );
		break;

	case LDAP_FILTER_LE:
		if ( map_attr_value( dc, f->f_av_desc, &atmp,
					&f->f_av_value, &vtmp, RWM_MAP ) )
		{
			return -1;
		}

		fstr->bv_len = atmp.bv_len + vtmp.bv_len + STRLENOF( "(<=)" );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s<=%s)",
			atmp.bv_val, vtmp.bv_val );

		ber_memfree( vtmp.bv_val );
		break;

	case LDAP_FILTER_APPROX:
		if ( map_attr_value( dc, f->f_av_desc, &atmp,
					&f->f_av_value, &vtmp, RWM_MAP ) )
		{
			return -1;
		}

		fstr->bv_len = atmp.bv_len + vtmp.bv_len + STRLENOF( "(~=)" );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s~=%s)",
			atmp.bv_val, vtmp.bv_val );

		ber_memfree( vtmp.bv_val );
		break;

	case LDAP_FILTER_SUBSTRINGS:
		if ( map_attr_value( dc, f->f_sub_desc, &atmp,
					NULL, NULL, RWM_MAP ) )
		{
			return -1;
		}

		/* cannot be a DN ... */

		fstr->bv_len = atmp.bv_len + STRLENOF( "(=*)" );
		fstr->bv_val = malloc( fstr->bv_len + 128 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s=*)",
			atmp.bv_val );

		if ( !BER_BVISNULL( &f->f_sub_initial ) ) {
			len = fstr->bv_len;

			filter_escape_value( &f->f_sub_initial, &vtmp );

			fstr->bv_len += vtmp.bv_len;
			fstr->bv_val = ch_realloc( fstr->bv_val, fstr->bv_len + 1 );

			snprintf( &fstr->bv_val[len - 2], vtmp.bv_len + 3,
				/* "(attr=" */ "%s*)",
				vtmp.bv_val );

			ber_memfree( vtmp.bv_val );
		}

		if ( f->f_sub_any != NULL ) {
			for ( i = 0; !BER_BVISNULL( &f->f_sub_any[i] ); i++ ) {
				len = fstr->bv_len;
				filter_escape_value( &f->f_sub_any[i], &vtmp );

				fstr->bv_len += vtmp.bv_len + 1;
				fstr->bv_val = ch_realloc( fstr->bv_val, fstr->bv_len + 1 );

				snprintf( &fstr->bv_val[len - 1], vtmp.bv_len + 3,
					/* "(attr=[init]*[any*]" */ "%s*)",
					vtmp.bv_val );
				ber_memfree( vtmp.bv_val );
			}
		}

		if ( !BER_BVISNULL( &f->f_sub_final ) ) {
			len = fstr->bv_len;

			filter_escape_value( &f->f_sub_final, &vtmp );

			fstr->bv_len += vtmp.bv_len;
			fstr->bv_val = ch_realloc( fstr->bv_val, fstr->bv_len + 1 );

			snprintf( &fstr->bv_val[len - 1], vtmp.bv_len + 3,
				/* "(attr=[init*][any*]" */ "%s)",
				vtmp.bv_val );

			ber_memfree( vtmp.bv_val );
		}

		break;

	case LDAP_FILTER_PRESENT:
		if ( map_attr_value( dc, f->f_desc, &atmp,
					NULL, NULL, RWM_MAP ) )
		{
			return -1;
		}

		fstr->bv_len = atmp.bv_len + STRLENOF( "(=*)" );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s=*)",
			atmp.bv_val );
		break;

	case LDAP_FILTER_AND:
	case LDAP_FILTER_OR:
	case LDAP_FILTER_NOT:
		fstr->bv_len = STRLENOF( "(%)" );
		fstr->bv_val = malloc( fstr->bv_len + 128 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%c)",
			f->f_choice == LDAP_FILTER_AND ? '&' :
			f->f_choice == LDAP_FILTER_OR ? '|' : '!' );

		for ( p = f->f_list; p != NULL; p = p->f_next ) {
			len = fstr->bv_len;

			if ( rwm_int_filter_map_rewrite( dc, p, &vtmp ) )
			{
				return -1;
			}
			
			fstr->bv_len += vtmp.bv_len;
			fstr->bv_val = ch_realloc( fstr->bv_val, fstr->bv_len + 1 );

			snprintf( &fstr->bv_val[len-1], vtmp.bv_len + 2, 
				/*"("*/ "%s)", vtmp.bv_val );

			ch_free( vtmp.bv_val );
		}

		break;

	case LDAP_FILTER_EXT: {
		if ( f->f_mr_desc ) {
			if ( map_attr_value( dc, f->f_mr_desc, &atmp,
						&f->f_mr_value, &vtmp, RWM_MAP ) )
			{
				return -1;
			}

		} else {
			BER_BVSTR( &atmp, "" );
			filter_escape_value( &f->f_mr_value, &vtmp );
		}
			

		fstr->bv_len = atmp.bv_len +
			( f->f_mr_dnattrs ? STRLENOF( ":dn" ) : 0 ) +
			( f->f_mr_rule_text.bv_len ? f->f_mr_rule_text.bv_len + 1 : 0 ) +
			vtmp.bv_len + STRLENOF( "(:=)" );
		fstr->bv_val = malloc( fstr->bv_len + 1 );

		snprintf( fstr->bv_val, fstr->bv_len + 1, "(%s%s%s%s:=%s)",
			atmp.bv_val,
			f->f_mr_dnattrs ? ":dn" : "",
			!BER_BVISEMPTY( &f->f_mr_rule_text ) ? ":" : "",
			!BER_BVISEMPTY( &f->f_mr_rule_text ) ? f->f_mr_rule_text.bv_val : "",
			vtmp.bv_val );
		ber_memfree( vtmp.bv_val );
		} break;

	case SLAPD_FILTER_COMPUTED:
		switch ( f->f_result ) {
		case LDAP_COMPARE_FALSE:
			tmp = ber_bvfalse;
			break;

		case LDAP_COMPARE_TRUE:
			tmp = ber_bvtrue;
			break;
			
		case SLAPD_COMPARE_UNDEFINED:
			tmp = ber_bvundefined;
			break;
			
		default:
			tmp = ber_bverror;
			break;
		}

		ber_dupbv( fstr, &tmp );
		break;
		
	default:
		ber_dupbv( fstr, &ber_bvunknown );
		break;
	}

	return 0;
}

int
rwm_filter_map_rewrite(
		dncookie		*dc,
		Filter			*f,
		struct berval		*fstr )
{
	int		rc;
	dncookie 	fdc;
	struct berval	ftmp;

	rc = rwm_int_filter_map_rewrite( dc, f, fstr );

#ifdef ENABLE_REWRITE
	if ( rc != LDAP_SUCCESS ) {
		return rc;
	}

	fdc = *dc;
	ftmp = *fstr;

	fdc.ctx = "searchFilter";

	switch ( rewrite_session( fdc.rwmap->rwm_rw, fdc.ctx, 
				( !BER_BVISEMPTY( &ftmp ) ? ftmp.bv_val : "" ), 
				fdc.conn, &fstr->bv_val )) {
	case REWRITE_REGEXEC_OK:
		if ( !BER_BVISNULL( fstr ) ) {
			fstr->bv_len = strlen( fstr->bv_val );
			free( ftmp.bv_val );

		} else {
			*fstr = ftmp;
		}

#ifdef NEW_LOGGING
		LDAP_LOG( BACK_LDAP, DETAIL1, 
			"[rw] %s: \"%s\" -> \"%s\"\n",
			dc->ctx, ftmp.bv_val, fstr->bv_val );		
#else /* !NEW_LOGGING */
		Debug( LDAP_DEBUG_ARGS,
			"[rw] %s: \"%s\" -> \"%s\"\n",
			dc->ctx, ftmp.bv_val, fstr->bv_val );		
#endif /* !NEW_LOGGING */
		rc = LDAP_SUCCESS;
		break;
 		
 	case REWRITE_REGEXEC_UNWILLING:
		if ( fdc.rs ) {
			fdc.rs->sr_err = LDAP_UNWILLING_TO_PERFORM;
			fdc.rs->sr_text = "Operation not allowed";
		}
		rc = LDAP_UNWILLING_TO_PERFORM;
		break;
	       	
	case REWRITE_REGEXEC_ERR:
		if ( fdc.rs ) {
			fdc.rs->sr_err = LDAP_OTHER;
			fdc.rs->sr_text = "Rewrite error";
		}
		rc = LDAP_OTHER;
		break;
	}

#endif /* ENABLE_REWRITE */
	return rc;
}

/*
 * I don't like this much, but we need two different
 * functions because different heap managers may be
 * in use in back-ldap/meta to reduce the amount of
 * calls to malloc routines, and some of the free()
 * routines may be macros with args
 */
int
rwm_dnattr_rewrite(
	Operation		*op,
	SlapReply		*rs,
	void			*cookie,
	BerVarray		a_vals,
	BerVarray		*pa_nvals )
{
	slap_overinst		*on = (slap_overinst *) op->o_bd->bd_info;
	struct ldaprwmap	*rwmap = 
			(struct ldaprwmap *)on->on_bi.bi_private;

	int			i, last;

	dncookie		dc;
	struct berval		dn, ndn, *pndn = NULL;

	assert( a_vals );

	/*
	 * Rewrite the bind dn if needed
	 */
	dc.rwmap = rwmap;
#ifdef ENABLE_REWRITE
	dc.conn = op->o_conn;
	dc.rs = rs;
	dc.ctx = (char *)cookie;
#else
	dc.tofrom = ((int *)cookie)[0];
	dc.normalized = 0;
#endif

	for ( last = 0; !BER_BVISNULL( &a_vals[last] ); last++ );
	if ( pa_nvals != NULL ) {
		pndn = &ndn;

		if ( *pa_nvals == NULL ) {
			*pa_nvals = ch_malloc( last * sizeof(struct berval) );
			memset( *pa_nvals, 0, last * sizeof(struct berval) );
		}
	}
	last--;

	for ( i = 0; !BER_BVISNULL( &a_vals[i] ); i++ ) {
		int		rc;

		rc = rwm_dn_massage( &dc, &a_vals[i], &dn, pndn );
		switch ( rc ) {
		case LDAP_UNWILLING_TO_PERFORM:
			/*
			 * FIXME: need to check if it may be considered 
			 * legal to trim values when adding/modifying;
			 * it should be when searching (e.g. ACLs).
			 */
			ch_free( a_vals[i].bv_val );
			if (last > i ) {
				a_vals[i] = a_vals[last];
				if ( pa_nvals ) {
					(*pa_nvals)[i] = (*pa_nvals)[last];
				}
			}
			BER_BVZERO( &a_vals[last] );
			if ( pa_nvals ) {
				BER_BVZERO( &(*pa_nvals)[last] );
			}
			last--;
			break;
		
		case LDAP_SUCCESS:
			if ( !BER_BVISNULL( &dn ) && dn.bv_val != a_vals[i].bv_val ) {
				ch_free( a_vals[i].bv_val );
				a_vals[i] = dn;
				if ( pa_nvals ) {
					if ( !BER_BVISNULL( &(*pa_nvals)[i] ) ) {
						ch_free( (*pa_nvals)[i].bv_val );
					}
					(*pa_nvals)[i] = *pndn;
				}
			}
			break;

		default:
			/* leave attr untouched if massage failed */
			if ( pa_nvals && BER_BVISNULL( &(*pa_nvals)[i] ) ) {
				dnNormalize( 0, NULL, NULL, &a_vals[i], &(*pa_nvals)[i], NULL );
			}
			break;
		}
	}
	
	return 0;
}

int
rwm_dnattr_result_rewrite(
	dncookie		*dc,
	BerVarray		a_vals
)
{
	int		i, last;

	for ( last = 0; !BER_BVISNULL( &a_vals[last] ); last++ );
	last--;

	for ( i = 0; !BER_BVISNULL( &a_vals[i] ); i++ ) {
		struct berval	dn;
		int		rc;
		
		rc = rwm_dn_massage( dc, &a_vals[i], &dn, NULL );
		switch ( rc ) {
		case LDAP_UNWILLING_TO_PERFORM:
			/*
			 * FIXME: need to check if it may be considered 
			 * legal to trim values when adding/modifying;
			 * it should be when searching (e.g. ACLs).
			 */
			LBER_FREE( &a_vals[i].bv_val );
			if ( last > i ) {
				a_vals[i] = a_vals[last];
			}
			BER_BVZERO( &a_vals[last] );
			last--;
			break;

		default:
			/* leave attr untouched if massage failed */
			if ( !BER_BVISNULL( &dn ) && a_vals[i].bv_val != dn.bv_val ) {
				LBER_FREE( a_vals[i].bv_val );
				a_vals[i] = dn;
			}
			break;
		}
	}

	return 0;
}

void
rwm_mapping_free( void *v_mapping )
{
	struct ldapmapping *mapping = v_mapping;

	if ( !BER_BVISNULL( &mapping[0].m_src ) ) {
		ch_free( mapping[0].m_src.bv_val );
	}

	if ( mapping[0].m_flags & RWMMAP_F_FREE_SRC ) {
		if ( mapping[0].m_flags & RWMMAP_F_IS_OC ) {
			if ( mapping[0].m_src_oc ) {
				ch_free( mapping[0].m_src_oc );
			}

		} else {
			if ( mapping[0].m_src_ad ) {
				ch_free( mapping[0].m_src_ad );
			}
		}
	}

	if ( !BER_BVISNULL( &mapping[0].m_dst ) ) {
		ch_free( mapping[0].m_dst.bv_val );
	}

	if ( mapping[0].m_flags & RWMMAP_F_FREE_DST ) {
		if ( mapping[0].m_flags & RWMMAP_F_IS_OC ) {
			if ( mapping[0].m_dst_oc ) {
				ch_free( mapping[0].m_dst_oc );
			}

		} else {
			if ( mapping[0].m_dst_ad ) {
				ch_free( mapping[0].m_dst_ad );
			}
		}
	}

	ch_free( mapping );

}

#endif /* SLAPD_OVER_RWM */
