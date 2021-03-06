/* Copyright (c) 2013-2015 PipelineDB */
/*-------------------------------------------------------------------------
 *
 * tdigest.c
 *
 *	  t-digest implementation based on: https://github.com/tdunning/t-digest
 *
 * Implementation is based on: https://github.com/tdunning/t-digest/blob/master/src/main/java/com/tdunning/math/stats/MergingDigest.java
 *
 * src/backend/pipeline/tdigest.h
 *
 */
#include <float.h>
#include <math.h>
#include <stdlib.h>

#include "pipeline/tdigest.h"
#include "utils/elog.h"
#include "utils/palloc.h"

#define DEFAULT_COMPRESSION 200
#define MIN_COMPRESSION 20
#define MAX_COMPRESSION 1000

#define interpolate(x, x0, x1) (((x) - (x0)) / ((x1) - (x0)))
#define integrated_location(compression, q) ((compression) * (asin(2 * (q) - 1) + M_PI / 2) / M_PI)
#define float_eq(f1, f2) (fabs((f1) - (f2)) <= FLT_EPSILON)

static uint32 estimate_compression_threshold(int compression)
{
	compression = Min(1000, Max(20, compression));
	return (uint32) (7.5 + 0.37 * compression - 2e-4 * compression * compression);
}

TDigest *TDigestCreate(void)
{
	return TDigestCreateWithCompression(DEFAULT_COMPRESSION);
}

TDigest *TDigestCreateWithCompression(int compression)
{
	TDigest *t = palloc0(sizeof(TDigest));

	t->compression = 1.0 * compression;
	t->threshold = estimate_compression_threshold(compression);
	t->size = ceil(compression * M_PI / 2) + 1;
	t->centroids = palloc0(sizeof(Centroid) * t->size);
	t->min = INFINITY;

	return t;
}

void TDigestDestroy(TDigest *t)
{
	list_free_deep(t->unmerged_centroids);
	pfree(t->centroids);
	pfree(t);
}

void TDigestAdd(TDigest *t, float8 x, int64 w)
{
	Centroid *c;

	if (list_length(t->unmerged_centroids) > t->threshold)
		TDigestCompress(t);

	c = palloc0(sizeof(Centroid));
	c->weight = w;
	c->mean = x;

	t->unmerged_centroids = lappend(t->unmerged_centroids, c);
}

static int centroid_cmp(const void *a, const void *b)
{
	Centroid *c1 = (Centroid *) a;
	Centroid *c2 = (Centroid *) b;
	if (c1->mean < c2->mean)
		return -1;
	if (c1->mean > c2->mean)
		return 1;
	return 0;
}

typedef struct mergeArgs
{
	TDigest *t;
	Centroid *centroids;
	int idx;
	float8 weight_so_far;
	float8 k1;
	float8 min;
	float8 max;
} mergeArgs;

static void merge_centroid(mergeArgs *args, Centroid *merge)
{
	float8 k2;
	Centroid *c = &args->centroids[args->idx];

	args->weight_so_far += merge->weight;
	k2 = integrated_location(args->t->compression,
			args->weight_so_far / args->t->total_weight);

	if (k2 - args->k1 > 1 && c->weight > 0)
	{
		args->idx++;
		args->k1 = integrated_location(args->t->compression,
				(args->weight_so_far - merge->weight) / args->t->total_weight);
	}

	c = &args->centroids[args->idx];
	c->weight += merge->weight;
	c->mean += (merge->mean - c->mean) * merge->weight / c->weight;

	if (merge->weight > 0)
	{
		args->min = Min(merge->mean, args->min);
		args->max = Max(merge->mean, args->max);
	}
}

void TDigestCompress(TDigest *t)
{
	int num_unmerged = list_length(t->unmerged_centroids);
	Centroid *unmerged_centroids;
	uint64_t unmerged_weight = 0;
	ListCell *lc;
	int i, j;
	mergeArgs *args;

	if (!num_unmerged)
		return;

	unmerged_centroids = palloc(sizeof(Centroid) * num_unmerged);

	i = 0;
	foreach(lc, t->unmerged_centroids)
	{
		Centroid *c = (Centroid *) lfirst(lc);
		memcpy(&unmerged_centroids[i], c, sizeof(Centroid));
		unmerged_weight += c->weight;
		i++;
	}

	list_free_deep(t->unmerged_centroids);
	t->unmerged_centroids = NIL;

	if (unmerged_weight == 0)
		return;

	t->total_weight += unmerged_weight;

	qsort(unmerged_centroids, num_unmerged, sizeof(Centroid), centroid_cmp);

	args = palloc0(sizeof(mergeArgs));
	args->centroids = palloc0(sizeof(Centroid) * t->size);
	args->t = t;
	args->min = INFINITY;

	i = 0;
	j = 0;
	while (i < num_unmerged && j < t->num_centroids)
	{
		Centroid *a = &unmerged_centroids[i];
		Centroid *b = &t->centroids[j];

		if (a->mean <= b->mean)
		{
			merge_centroid(args, a);
			i++;
		}
		else
		{
			merge_centroid(args, b);
			j++;
		}
	}

	while (i < num_unmerged)
		merge_centroid(args, &unmerged_centroids[i++]);

	pfree(unmerged_centroids);

	while (j < t->num_centroids)
		merge_centroid(args, &t->centroids[j++]);

	if (t->total_weight > 0)
	{
		t->min = Min(t->min, args->min);

		if (args->centroids[args->idx].weight <= 0)
			args->idx--;

		t->num_centroids = args->idx + 1;
		t->max = Max(t->max, args->max);
	}

	pfree(t->centroids);
	t->centroids = args->centroids;
	pfree(args);
}

void TDigestMerge(TDigest *t1, TDigest *t2)
{
	int i;

	TDigestCompress(t2);

	for (i = 0; i < t2->num_centroids; i++)
	{
		Centroid *c = &t2->centroids[i];
		TDigestAdd(t1, c->mean, c->weight);
	}
}

float8 TDigestCDF(TDigest *t, float8 x)
{
	int i;
	float8 left, right;
	uint64 weight_so_far;
	Centroid *a, *b, tmp;

	TDigestCompress(t);

	if (t->num_centroids == 0)
		return NAN;

	if (x < t->min)
		return 0;
	if (x > t->max)
		return 1;

	if (t->num_centroids == 1)
	{
		if (float_eq(t->max, t->min))
			return 0.5;

		return interpolate(x, t->min, t->max);
	}

	weight_so_far = 0;
	a = b = &tmp;
	b->mean = t->min;
	b->weight = 0;
	right = 0;

	for (i = 0; i < t->num_centroids; i++)
	{
		Centroid *c = &t->centroids[i];

		left = b->mean - (a->mean + right);
		a = b;
		b = c;
		right = (b->mean - a->mean) * a->weight / (a->weight + b->weight);
		if (x < a->mean + right)
			return Max((weight_so_far + a->weight * interpolate(x, a->mean - left, a->mean + right)) / t->total_weight, 0.0);

		weight_so_far += a->weight;
	}

	left = b->mean - (a->mean + right);
	a = b;
	right = t->max - a->mean;

	if (x < a->mean + right)
		return (weight_so_far + a->weight * interpolate(x, a->mean - left, a->mean + right)) / t->total_weight;

	return 1;
}

float8 TDigestQuantile(TDigest *t, float8 q)
{
	int i;
	float8 left, right, idx;
	uint64 weight_so_far;
	Centroid *a, *b, tmp;

	TDigestCompress(t);

	if (q < 0 || q > 1)
		elog(ERROR, "q should be in [0, 1], got %f", q);

	if (t->num_centroids == 0)
		return NAN;

	if (t->num_centroids == 1)
		return t->centroids[0].mean;

	if (float_eq(q, 0.0))
		return t->min;

	if (float_eq(q, 1.0))
		return t->max;

	idx = q * t->total_weight;

	weight_so_far = 0;
	b = &tmp;
	b->mean = t->min;
	b->weight = 0;
	right = t->min;

	for (i = 1; i < t->num_centroids; i++)
	{
		Centroid *c = &t->centroids[i];
		a = b;
		left = right;

		b = c;
		right = (b->weight * a->mean + a->weight * b->mean) / (a->weight + b->weight);

		if (idx < weight_so_far + a->weight)
		{
			float8 p = (idx - weight_so_far) / a->weight;
			return left * (1 - p) + right * p;
		}

		weight_so_far += a->weight;
	}

	left = right;
	a = b;
	right = t->max;

	if (idx < weight_so_far + a->weight)
	{
		float8 p = (idx - weight_so_far) / a->weight;
		return left * (1 - p) + right * p;
	}

	return t->max;
}

TDigest *TDigestCopy(TDigest *t)
{
	TDigest *cpy = palloc(sizeof(TDigest));

	TDigestCompress(t);

	memcpy(cpy, t, sizeof(TDigest));
	cpy->centroids = palloc0(sizeof(Centroid) * t->size);
	memcpy(cpy->centroids, t->centroids, sizeof(Centroid) * t->num_centroids);

	return cpy;
}
