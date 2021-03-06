/***********************************************************************
* pc_access.c
*
*  Accessor/aggregate functions for points and patches in PgSQL.
*
*  PgSQL Pointcloud is free and open source software provided 
*  by the Government of Canada
*  Copyright (c) 2013 Natural Resources Canada
*
***********************************************************************/

#include "pc_pgsql.h"      /* Common PgSQL support for our type */
#include "utils/numeric.h"
#include "funcapi.h"

/* General SQL functions */
Datum pcpoint_get_value(PG_FUNCTION_ARGS);
Datum pcpatch_from_pcpoint_array(PG_FUNCTION_ARGS);
Datum pcpatch_from_pcpatch_array(PG_FUNCTION_ARGS);
Datum pcpatch_uncompress(PG_FUNCTION_ARGS);
Datum pcpatch_numpoints(PG_FUNCTION_ARGS);
Datum pcpatch_intersects(PG_FUNCTION_ARGS);
Datum pcpatch_size(PG_FUNCTION_ARGS);
Datum pcpoint_size(PG_FUNCTION_ARGS);

/* Generic aggregation functions */
Datum pointcloud_agg_transfn(PG_FUNCTION_ARGS);
Datum pointcloud_abs_in(PG_FUNCTION_ARGS);
Datum pointcloud_abs_out(PG_FUNCTION_ARGS);

/* Point finalizers */
Datum pcpoint_agg_final_pcpatch(PG_FUNCTION_ARGS);
Datum pcpoint_agg_final_array(PG_FUNCTION_ARGS);

/* Patch finalizers */
Datum pcpatch_agg_final_array(PG_FUNCTION_ARGS);
Datum pcpatch_agg_final_pcpatch(PG_FUNCTION_ARGS);

/* Deaggregation functions */
Datum pcpatch_unnest(PG_FUNCTION_ARGS);

/**
* Read a named dimension from a PCPOINT
* PC_Get(point pcpoint, dimname text) returns Numeric
*/
PG_FUNCTION_INFO_V1(pcpoint_get_value);
Datum pcpoint_get_value(PG_FUNCTION_ARGS)
{
	SERIALIZED_POINT *serpt = PG_GETARG_SERPOINT_P(0);
	text *dim_name = PG_GETARG_TEXT_P(1);
	char *dim_str;
	float8 double_result;

    PCSCHEMA *schema = pc_schema_from_pcid(serpt->pcid, fcinfo);
	PCPOINT *pt = pc_point_deserialize(serpt, schema);
	if ( ! pt )
		PG_RETURN_NULL();

	dim_str = text_to_cstring(dim_name);
	if ( ! pc_point_get_double_by_name(pt, dim_str, &double_result) )
	{
		pc_point_free(pt);
		elog(ERROR, "dimension \"%s\" does not exist in schema", dim_str);
	}
	pfree(dim_str);
	pc_point_free(pt);
	PG_RETURN_DATUM(DirectFunctionCall1(float8_numeric, Float8GetDatum(double_result)));
}

static inline bool
array_get_isnull(const bits8 *nullbitmap, int offset)
{
	if (nullbitmap == NULL)
	{
		return false; /* assume not null */
	}
	if (nullbitmap[offset / 8] & (1 << (offset % 8)))
	{
		return false; /* not null */
	}
	return true;
}

static PCPATCH *
pcpatch_from_point_array(ArrayType *array, FunctionCallInfoData *fcinfo)
{
	int nelems;
	bits8 *bitmap;
	int bitmask;
	size_t offset = 0;
	int i;
    uint32 pcid = 0;
	PCPATCH *pa;
	PCPOINTLIST *pl;
    PCSCHEMA *schema = 0;

	/* How many things in our array? */
	nelems = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));

	/* PgSQL supplies a bitmap of which array entries are null */
	bitmap = ARR_NULLBITMAP(array);

	/* Empty array? Null return */
	if ( nelems == 0 )
        return NULL;

	/* Make our holder */
	pl = pc_pointlist_make(nelems);

	offset = 0;
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;
	for ( i = 0; i < nelems; i++ )
	{
		/* Only work on non-NULL entries in the array */
		if ( ! array_get_isnull(bitmap, i) )
		{
			SERIALIZED_POINT *serpt = (SERIALIZED_POINT *)(ARR_DATA_PTR(array)+offset);
			PCPOINT *pt;

			if ( ! schema )
			{
                schema = pc_schema_from_pcid(serpt->pcid, fcinfo);
		    }

			if ( ! pcid )
			{
				pcid = serpt->pcid;
			}
			else if ( pcid != serpt->pcid )
			{
				elog(ERROR, "pcpatch_from_point_array: pcid mismatch (%d != %d)", serpt->pcid, pcid);
			}

			pt = pc_point_deserialize(serpt, schema);
			if ( ! pt )
			{
				elog(ERROR, "pcpatch_from_point_array: point deserialization failed");
			}

			pc_pointlist_add_point(pl, pt);

			offset += INTALIGN(VARSIZE(serpt));
		}

	}

	if ( pl->npoints == 0 )
		return NULL;

	pa = pc_patch_from_pointlist(pl);
	pc_pointlist_free(pl);
    return pa;
}


static PCPATCH *
pcpatch_from_patch_array(ArrayType *array, FunctionCallInfoData *fcinfo)
{
	int nelems;
	bits8 *bitmap;
	int bitmask;
	size_t offset = 0;
	int i;
    uint32 pcid = 0;
	PCPATCH *pa;
	PCPATCH **palist;
    int numpatches = 0;
    PCSCHEMA *schema = 0;

	/* How many things in our array? */
	nelems = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));

	/* PgSQL supplies a bitmap of which array entries are null */
	bitmap = ARR_NULLBITMAP(array);

	/* Empty array? Null return */
	if ( nelems == 0 )
        return NULL;

	/* Make our temporary list of patches */
	palist = pcalloc(nelems*sizeof(PCPATCH*));

    /* Read the patches out of the array and deserialize */
	offset = 0;
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;
	for ( i = 0; i < nelems; i++ )
	{
		/* Only work on non-NULL entries in the array */
		if ( ! array_get_isnull(bitmap, i) )
		{
			SERIALIZED_PATCH *serpatch = (SERIALIZED_PATCH *)(ARR_DATA_PTR(array)+offset);

			if ( ! schema )
			{
                schema = pc_schema_from_pcid(serpatch->pcid, fcinfo);
		    }

			if ( ! pcid )
			{
				pcid = serpatch->pcid;
			}
			else if ( pcid != serpatch->pcid )
			{
				elog(ERROR, "pcpatch_from_patch_array: pcid mismatch (%d != %d)", serpatch->pcid, pcid);
			}

			pa = pc_patch_deserialize(serpatch, schema);
			if ( ! pa )
			{
				elog(ERROR, "pcpatch_from_patch_array: patch deserialization failed");
			}

            palist[numpatches++] = pa;

			offset += INTALIGN(VARSIZE(serpatch));
		}

	}

	/* Can't do anything w/ NULL */
	if ( numpatches == 0 )
		return NULL;

    /* Pass to the lib to build the output patch from the list */
	pa = pc_patch_from_patchlist(palist, numpatches);

	/* Free the temporary patch list */
    for ( i = 0; i < numpatches; i++ )
    {
        pc_patch_free(palist[i]);
    }
    pcfree(palist);

    return pa;
}


PG_FUNCTION_INFO_V1(pcpatch_from_pcpatch_array);
Datum pcpatch_from_pcpatch_array(PG_FUNCTION_ARGS)
{
	ArrayType *array;
	PCPATCH *pa;
	SERIALIZED_PATCH *serpa;

    if ( PG_ARGISNULL(0) )
        PG_RETURN_NULL();

	array = DatumGetArrayTypeP(PG_GETARG_DATUM(0));
    pa = pcpatch_from_patch_array(array, fcinfo);
    if ( ! pa )
        PG_RETURN_NULL();

	serpa = pc_patch_serialize(pa, NULL);
	pc_patch_free(pa);
	PG_RETURN_POINTER(serpa);
}

PG_FUNCTION_INFO_V1(pcpatch_from_pcpoint_array);
Datum pcpatch_from_pcpoint_array(PG_FUNCTION_ARGS)
{
	ArrayType *array;
	PCPATCH *pa;
	SERIALIZED_PATCH *serpa;

    if ( PG_ARGISNULL(0) )
        PG_RETURN_NULL();

	array = DatumGetArrayTypeP(PG_GETARG_DATUM(0));
    pa = pcpatch_from_point_array(array, fcinfo);
    if ( ! pa )
        PG_RETURN_NULL();

	serpa = pc_patch_serialize(pa, NULL);
	pc_patch_free(pa);
	PG_RETURN_POINTER(serpa);
}

typedef struct
{
	ArrayBuildState *s;
} abs_trans;

PG_FUNCTION_INFO_V1(pointcloud_abs_in);
Datum pointcloud_abs_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
	               errmsg("function pointcloud_abs_in not implemented")));
	PG_RETURN_POINTER(NULL);
}

PG_FUNCTION_INFO_V1(pointcloud_abs_out);
Datum pointcloud_abs_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
	               errmsg("function pointcloud_abs_out not implemented")));
	PG_RETURN_POINTER(NULL);
}


PG_FUNCTION_INFO_V1(pointcloud_agg_transfn);
Datum pointcloud_agg_transfn(PG_FUNCTION_ARGS)
{
	Oid arg1_typeid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	MemoryContext aggcontext;
	abs_trans *a;
	ArrayBuildState *state;
	Datum elem;

	if (arg1_typeid == InvalidOid)
		ereport(ERROR,
		        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		         errmsg("could not determine input data type")));

	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		aggcontext = ((AggState *) fcinfo->context)->aggcontext;
	}
	else if (fcinfo->context && IsA(fcinfo->context, WindowAggState))
	{
		aggcontext = ((WindowAggState *) fcinfo->context)->aggcontext;
	}
	else
	{
		/* cannot be called directly because of dummy-type argument */
		elog(ERROR, "pointcloud_agg_transfn called in non-aggregate context");
		aggcontext = NULL;  /* keep compiler quiet */
	}

	if ( PG_ARGISNULL(0) )
	{
		a = (abs_trans*) palloc(sizeof(abs_trans));
		a->s = NULL;
	}
	else
	{
		a = (abs_trans*) PG_GETARG_POINTER(0);
	}
	state = a->s;
	elem = PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1);
	state = accumArrayResult(state,
	                         elem,
	                         PG_ARGISNULL(1),
	                         arg1_typeid,
	                         aggcontext);
	a->s = state;

	PG_RETURN_POINTER(a);
}




static Datum
pointcloud_agg_final(abs_trans *a, MemoryContext mctx, FunctionCallInfo fcinfo)
{
	ArrayBuildState *state;
	int dims[1];
	int lbs[1];
	state = a->s;
	dims[0] = state->nelems;
	lbs[0] = 1;
	return makeMdArrayResult(state, 1, dims, lbs, mctx, false);
}

PG_FUNCTION_INFO_V1(pcpoint_agg_final_array);
Datum pcpoint_agg_final_array(PG_FUNCTION_ARGS)
{
	abs_trans *a;
	Datum result = 0;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();   /* returns null iff no input values */

	a = (abs_trans*) PG_GETARG_POINTER(0);

	result = pointcloud_agg_final(a, CurrentMemoryContext, fcinfo);
	PG_RETURN_DATUM(result);
}


PG_FUNCTION_INFO_V1(pcpatch_agg_final_array);
Datum pcpatch_agg_final_array(PG_FUNCTION_ARGS)
{
	abs_trans *a;
	Datum result = 0;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();   /* returns null iff no input values */

	a = (abs_trans*) PG_GETARG_POINTER(0);

	result = pointcloud_agg_final(a, CurrentMemoryContext, fcinfo);
	PG_RETURN_DATUM(result);
}


PG_FUNCTION_INFO_V1(pcpoint_agg_final_pcpatch);
Datum pcpoint_agg_final_pcpatch(PG_FUNCTION_ARGS)
{
    ArrayType *array;
	abs_trans *a;
    PCPATCH *pa;
    SERIALIZED_PATCH *serpa;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();   /* returns null iff no input values */

	a = (abs_trans*) PG_GETARG_POINTER(0);

	array = DatumGetArrayTypeP(pointcloud_agg_final(a, CurrentMemoryContext, fcinfo));
    pa = pcpatch_from_point_array(array, fcinfo);
    if ( ! pa )
        PG_RETURN_NULL();

    serpa = pc_patch_serialize(pa, NULL);
    pc_patch_free(pa);
    PG_RETURN_POINTER(serpa);
}


PG_FUNCTION_INFO_V1(pcpatch_agg_final_pcpatch);
Datum pcpatch_agg_final_pcpatch(PG_FUNCTION_ARGS)
{
    ArrayType *array;
	abs_trans *a;
    PCPATCH *pa;
    SERIALIZED_PATCH *serpa;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();   /* returns null iff no input values */

	a = (abs_trans*) PG_GETARG_POINTER(0);

	array = DatumGetArrayTypeP(pointcloud_agg_final(a, CurrentMemoryContext, fcinfo));
    pa = pcpatch_from_patch_array(array, fcinfo);
    if ( ! pa )
        PG_RETURN_NULL();

    serpa = pc_patch_serialize(pa, NULL);
    pc_patch_free(pa);
    PG_RETURN_POINTER(serpa);
}


PG_FUNCTION_INFO_V1(pcpatch_unnest);
Datum pcpatch_unnest(PG_FUNCTION_ARGS)
{
	typedef struct
	{
		int nextelem;
		int numelems;
		PCPOINTLIST *pointlist;
	} pcpatch_unnest_fctx;

	FuncCallContext *funcctx;
	pcpatch_unnest_fctx *fctx;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		PCPATCH *patch;
		SERIALIZED_PATCH *serpatch;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		* switch to memory context appropriate for multiple function calls
		*/
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		* Get the patch value and detoast if needed.  We can't do this
		* earlier because if we have to detoast, we want the detoasted copy
		* to be in multi_call_memory_ctx, so it will go away when we're done
		* and not before.      (If no detoast happens, we assume the originally
		* passed array will stick around till then.)
		*/
		serpatch = PG_GETARG_SERPATCH_P(0);
		patch = pc_patch_deserialize(serpatch, pc_schema_from_pcid_uncached(serpatch->pcid));

		/* allocate memory for user context */
		fctx = (pcpatch_unnest_fctx *) palloc(sizeof(pcpatch_unnest_fctx));

		/* initialize state */
		fctx->nextelem = 0;
		fctx->numelems = patch->npoints;
		fctx->pointlist = pc_pointlist_from_patch(patch);

		/* save user context, switch back to function context */
		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	if (fctx->nextelem < fctx->numelems)
	{
		Datum elem;
		PCPOINT *pt = pc_pointlist_get_point(fctx->pointlist, fctx->nextelem);
		SERIALIZED_POINT *serpt = pc_point_serialize(pt);
		fctx->nextelem++;
		elem = PointerGetDatum(serpt);
		SRF_RETURN_NEXT(funcctx, elem);
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}

PG_FUNCTION_INFO_V1(pcpatch_uncompress);
Datum pcpatch_uncompress(PG_FUNCTION_ARGS)
{
	SERIALIZED_PATCH *serpa = PG_GETARG_SERPATCH_P(0);
    PCSCHEMA *schema = pc_schema_from_pcid(serpa->pcid, fcinfo);
    PCPATCH *patch = pc_patch_deserialize(serpa, schema);
    SERIALIZED_PATCH *serpa_out = pc_patch_serialize_to_uncompressed(patch);
    pc_patch_free(patch);
    PG_RETURN_POINTER(serpa_out);
}

PG_FUNCTION_INFO_V1(pcpatch_numpoints);
Datum pcpatch_numpoints(PG_FUNCTION_ARGS)
{
	SERIALIZED_PATCH *serpa = PG_GETARG_SERPATCH_P(0);
    PG_RETURN_INT32(serpa->npoints);
}

PG_FUNCTION_INFO_V1(pcpatch_compression);
Datum pcpatch_compression(PG_FUNCTION_ARGS)
{
	SERIALIZED_PATCH *serpa = PG_GETARG_SERPATCH_P(0);
    PG_RETURN_INT32(serpa->compression);
}

PG_FUNCTION_INFO_V1(pcpatch_intersects);
Datum pcpatch_intersects(PG_FUNCTION_ARGS)
{
	SERIALIZED_PATCH *serpa1 = PG_GETARG_SERPATCH_P(0);
	SERIALIZED_PATCH *serpa2 = PG_GETARG_SERPATCH_P(1);

	if ( serpa1->pcid != serpa2->pcid )
	    elog(ERROR, "pcpatch_intersects: pcid mismatch (%d != %d)", serpa1->pcid, serpa2->pcid);

	if ( serpa1->xmin > serpa2->xmax ||
	     serpa1->xmax < serpa2->xmin ||
	     serpa1->ymin > serpa2->ymax ||
	     serpa1->ymax < serpa2->ymin )
	{
        PG_RETURN_BOOL(FALSE);
    }

    PG_RETURN_BOOL(TRUE);
}

PG_FUNCTION_INFO_V1(pcpatch_size);
Datum pcpatch_size(PG_FUNCTION_ARGS)
{
	SERIALIZED_PATCH *serpa = PG_GETARG_SERPATCH_P(0);
    PG_RETURN_INT32(VARSIZE(serpa));
}

PG_FUNCTION_INFO_V1(pcpoint_size);
Datum pcpoint_size(PG_FUNCTION_ARGS)
{
	SERIALIZED_POINT *serpt = PG_GETARG_SERPOINT_P(0);
    PG_RETURN_INT32(VARSIZE(serpt));
}

