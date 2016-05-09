
/**
 * @file aw.c
 *
 * @brief alignment writer implementation
 */

#define UNITTEST_UNIQUE_ID			21
#include "unittest.h"

#include <stdint.h>
#include <string.h>
#include "gref/gref.h"
#include "gaba/gaba.h"
#include "zf/zf.h"
#include "log.h"
#include "aw.h"


/* inline directive */
#define _force_inline				inline

/* max / min */
#define MAX2(x, y)					( (x) < (y) ? (y) : (x) )
#define MIN2(x, y)					( (x) > (y) ? (y) : (x) )

/* constants */
#define SAM_VERSION_STRING			"1.0"
#define SAM_DEFAULT_READGROUP		( 1 )

/**
 * @struct aw_conf_s
 */
struct aw_conf_s {
	char const *ext;
	char const *mode;
	void (*header)(
		aw_t *aw,
		gref_idx_t const *r,
		gref_acv_t const *q);
	void (*body)(
		aw_t *aw,
		gref_idx_t const *r,
		gref_acv_t const *q,
		gaba_result_t const *aln);
	void (*footer)(
		aw_t *aw,
		gref_idx_t const *r,
		gref_acv_t const *q);
};

/**
 * @struct aw_s
 */
struct aw_s {
	zf_t *fp;
	struct aw_conf_s conf;		/* function pointers and misc */
	uint8_t format;				/* AW_SAM, AW_GPA, ... */
	char clip;					/* cigar operation to represent clip operation ('S' or 'H') */
	uint8_t pad[2];
	uint32_t program_id;
	char *program_name;
	char *command;				/* '\t' must be substituted to ' ' */
};

/**
 * @fn sam_write_header
 */
static
void sam_write_header(
	aw_t *aw,
	gref_idx_t const *r,
	gref_acv_t const *q)
{
	/* write header */
	zfprintf(aw->fp, "@HD\tVN:%s\tSO:unsorted\n", SAM_VERSION_STRING);

	/* write reference sequence names */
	int64_t ref_cnt = gref_get_section_count(r);
	for(int64_t i = 0; i < ref_cnt; i++) {
		zfprintf(aw->fp, "@SQ\tSN:%s\tLN:%u\n",
			gref_get_name(r, gref_gid(i, 0)).str,
			gref_get_section(r, gref_gid(i, 0))->len);

		debug("i(%lld), gid(%u), name(%s), len(%u)", i,
			gref_get_section(r, gref_gid(i, 0))->gid,
			gref_get_name(r, gref_gid(i, 0)).str,
			gref_get_section(r, gref_gid(i, 0))->len);
	}

	/* write readgroup info */
	zfprintf(aw->fp, "@RG\tID:%d\n", SAM_DEFAULT_READGROUP);

	/* program info */
	if(aw->program_name != NULL || aw->command != NULL) {
		zfprintf(aw->fp, "@PG");

		if(aw->program_name != NULL) {
			zfprintf(aw->fp, "\tID:%d\tPN:%s",
				aw->program_id, aw->program_name);
		}

		if(aw->command != NULL) {
			zfprintf(aw->fp, "\tCL:%s", aw->command);
		}
		zfputc(aw->fp, '\n');
	}
	return;
}

/**
 * @fn sam_calc_flags
 */
static _force_inline
int64_t sam_calc_flags(
	gref_idx_t const *r,
	gref_acv_t const *q,
	struct gaba_path_section_s const *curr,
	struct gaba_path_section_s const *next)
{
	int64_t flags = 0;

	/* determine direction */
	flags |= (gref_dir(curr->aid) ^ gref_dir(curr->bid)) ? 0x10 : 0;
	
	return(flags);
}

/**
 * @fn sam_print_str
 */
static _force_inline
void sam_print_str(
	zf_t *fp,
	char const *str,
	uint32_t len)
{
	debug("print_str %p, %s", str, str);

	for(int64_t i = 0; i < len; i++) {
		zfputc(fp, str[i]);
	}
	return;
}

/**
 * @fn sam_print_num
 */
static _force_inline
void sam_print_num(
	zf_t *fp,
	int64_t n)
{
	debug("print_num %lld", n);

	zfprintf(fp, "%lld", n);
	return;
}

/**
 * @fn sam_seq_decode_base
 */
static _force_inline
char sam_seq_decode_base(
	uint8_t base)
{
	char const table[] = {
		'N', 'A', 'C', 'M', 'G', 'R', 'S', 'V',
		'T', 'W', 'Y', 'H', 'K', 'D', 'B', 'N'
	};
	return(table[base & 0x0f]);
}

/**
 * @fn sam_print_cigar
 */
static _force_inline
void sam_print_cigar(
	aw_t *aw,
	gref_acv_t const *q,
	struct gaba_path_section_s const *curr,
	struct gaba_path_s const *path)
{
	gref_section_t const *bsec = gref_get_section(q, curr->bid);

	debug("curr->bid(%u), bsec->gid(%u)", curr->bid, bsec->gid);

	/* determine direction and calc margins */
	int64_t hlen = (gref_dir(curr->bid) == GREF_FW)
		? curr->bpos
		: bsec->len - (curr->bpos + curr->blen);
	int64_t tlen = bsec->len - (hlen + curr->blen);

	debug("blen(%u), hlen(%lld), len(%u), tlen(%lld)", curr->blen, hlen, bsec->len, tlen);

	/* print clip at the head */
	if(hlen > 0) {
		zfprintf(aw->fp, "%lld%c", hlen, aw->clip);
	}

	/* print cigar */
	gaba_dp_print_cigar(
		(gaba_dp_fprintf_t)zfprintf,
		(void *)aw->fp,
		path->array,
		path->offset + curr->ppos,
		curr->plen);

	/* print clip at the tail */
	if(tlen > 0) {
		zfprintf(aw->fp, "%lld%c", tlen, aw->clip);
	}
	zfputc(aw->fp, '\t');
	return;
}

/**
 * @fn sam_print_seq_qual
 */
static _force_inline
void sam_print_seq_qual(
	aw_t *aw,
	gref_acv_t const *q,
	struct gaba_path_section_s const *curr)
{
	gref_section_t const *bsec = gref_get_section(q, curr->bid);
	uint8_t const *lim = gref_get_lim(q);

	/* determine direction and fix seq pointer */
	uint8_t const *seq = (gref_dir(curr->bid) == GREF_FW)
		? bsec->base
		: gref_rev_ptr(bsec->base, lim) - bsec->len;
	
	int64_t hlen = (gref_dir(curr->bid) == GREF_FW)
		? curr->bpos
		: bsec->len - (curr->bpos + curr->blen);
	int64_t tlen = bsec->len - (hlen + curr->blen);
	
	/*
	int64_t hlen = curr->bpos;
	int64_t tlen = bsec->len - (curr->bpos + curr->blen);
	*/
	debug("blen(%u), hlen(%lld), len(%u), tlen(%lld)", curr->blen, hlen, bsec->len, tlen);
	debug("print_seq, seq(%p), lim(%p), len(%lld, %u, %lld)",
		seq, lim, hlen, curr->blen, tlen);

	/* print unaligned region at the head */
	if(aw->clip == 'S') {
		for(int64_t i = 0; i < hlen; i++) {
			zfputc(aw->fp, sam_seq_decode_base(seq[i]));
		}
	}

	/* print body */
	for(int64_t i = 0; i < curr->blen; i++) {
		zfputc(aw->fp, sam_seq_decode_base(seq[hlen + i]));
	}

	/* print unaligned region at the tail */
	if(aw->clip == 'S') {
		for(int64_t i = 0; i < tlen; i++) {
			zfputc(aw->fp, sam_seq_decode_base(seq[hlen + curr->blen + i]));
		}
	}

	/* print quality string */
	zfprintf(aw->fp, "\t*\t");
	return;
}

/**
 * @fn sam_print_option_tags
 */
static _force_inline
void sam_print_option_tags(
	aw_t *aw,
	gref_acv_t const *q,
	struct gaba_path_section_s const *curr,
	struct gaba_path_s const *path)
{
	/* print alignment score */
	zfprintf(aw->fp, "RG:Z:%d", SAM_DEFAULT_READGROUP);
	return;
}

/**
 * @fn sam_write_segment
 */
static _force_inline
void sam_write_segment(
	aw_t *aw,
	gref_idx_t const *r,
	gref_acv_t const *q,
	struct gaba_path_s const *path,
	struct gaba_path_section_s const *curr,
	struct gaba_path_section_s const *next)
{
	/* query name */
	sam_print_str(aw->fp,
		gref_get_name(q, curr->bid).str,
		gref_get_name(q, curr->bid).len);
	zfputc(aw->fp, '\t');

	/* flags (revcomp indicator) */
	sam_print_num(aw->fp, sam_calc_flags(r, q, curr, next));
	zfputc(aw->fp, '\t');

	/* reference name and pos (name is skipped by default) */
	sam_print_str(aw->fp, 
		gref_get_name(r, curr->aid).str,
		gref_get_name(r, curr->aid).len);
	zfputc(aw->fp, '\t');
	sam_print_num(aw->fp, curr->apos);
	zfputc(aw->fp, '\t');

	/* mapping quality */
	sam_print_num(aw->fp, 255);
	zfputc(aw->fp, '\t');

	/* cigar */
	sam_print_cigar(aw, q, curr, path);

	/* ref name and pos of the next section */
	if(next != NULL) {
		sam_print_str(aw->fp,
			gref_get_name(r, next->aid).str,
			gref_get_name(r, next->aid).len);
		zfputc(aw->fp, '\t');
		sam_print_num(aw->fp, next->apos);
		zfputc(aw->fp, '\t');
	} else {
		/* tail */
		zfprintf(aw->fp, "*\t0\t");
	}

	/* template length */
	zfprintf(aw->fp, "0\t");

	/* seq and qual */
	sam_print_seq_qual(aw, q, curr);

	/* print option tags */
	sam_print_option_tags(aw, q, curr, path);
	zfputc(aw->fp, '\n');
	return;
}

/**
 * @fn sam_write_alignment
 */
static
void sam_write_alignment(
	aw_t *aw,
	gref_idx_t const *r,
	gref_acv_t const *q,
	gaba_result_t const *aln)
{
	debug("slen(%u)", aln->slen);
	for(int64_t i = 0; i < aln->slen - 1; i++) {
		debug("i(%lld), path(%p), &sec[i](%p), &sec[i+1](%p)",
			i, aln->path, &aln->sec[i], &aln->sec[i + 1]);
		sam_write_segment(aw, r, q, aln->path, &aln->sec[i], &aln->sec[i + 1]);
	}

	debug("i(%u), path(%p), &sec[i](%p), &sec[i+1](%p)",
		aln->slen - 1, aln->path, &aln->sec[aln->slen - 1], NULL);
	sam_write_segment(aw, r, q, aln->path, &aln->sec[aln->slen - 1], NULL);
	return;
}

/**
 * @fn aw_append_alignment
 */
void aw_append_alignment(
	aw_t *aw,
	gref_idx_t const *ref,
	gref_acv_t const *query,
	struct gaba_result_s const *const *aln,
	int64_t cnt)
{
	for(int64_t i = 0; i < cnt; i++) {
		debug("append i(%lld), ref(%p), query(%p), aln[i](%p)", i, ref, query, aln[i]);
		aw->conf.body(aw, ref, query, aln[i]);
	}
	return;
}

/**
 * @fn strdup_rm_tab
 */
char *strdup_rm_tab(
	char const *str)
{
	char *copy = strdup(str);

	for(int64_t i = 0; i < strlen(str); i++) {
		if(copy[i] == '\t') {
			copy[i] = ' ';
		}
	}
	return(copy);
}

/**
 * @fn aw_init
 *
 * @brief initialize alignment writer context
 */
aw_t *aw_init(
	char const *path,
	gref_idx_t const *idx,
	aw_params_t const *params)
{
	/* replace params if null */
	struct aw_params_s default_params = { 0 };
	params = (params != NULL) ? params : &default_params;

	/* malloc context */
	struct aw_s *aw = (struct aw_s *)malloc(sizeof(struct aw_s));
	if(aw == NULL) {
		goto _aw_init_error_handler;
	}

	struct aw_conf_s conf[] = {
		[AW_SAM] = {
			.ext = ".sam",
			.mode = "w",
			.header = sam_write_header,
			.body = sam_write_alignment,
			.footer = NULL
		}
	};

	/* detect format */
	if(params->format != 0) {
		aw->conf = conf[params->format];
	} else {
		for(int64_t i = 1; i < sizeof(conf) / sizeof(struct aw_conf_s); i++) {
			if(strncmp(path + strlen(path) - strlen(conf[i].ext), conf[i].ext, strlen(conf[i].ext)) == 0) {
				debug("format detected %s", conf[i].ext);

				aw->conf = conf[i];
			}
		}
	}
	if(aw->conf.ext == NULL) {
		goto _aw_init_error_handler;
	}

	/* copy params */
	aw->format = params->format;
	if(params->clip == 'S' || params->clip == 'H') {
		aw->clip = params->clip;
	} else {
		aw->clip = 'S';
	}
	aw->program_id = params->program_id;
	aw->program_name = (params->program_name != NULL)
		? strdup_rm_tab(params->program_name) : NULL;
	aw->command = (params->command != NULL)
		? strdup_rm_tab(params->command) : NULL;

	/* open file */
	aw->fp = zfopen(path, aw->conf.mode);
	if(aw->fp == NULL) {
		goto _aw_init_error_handler;
	}

	if(aw->conf.header != NULL) {
		aw->conf.header(aw, idx, NULL);
	}
	return((aw_t *)aw);

_aw_init_error_handler:;
	if(aw != NULL) {
		zfclose(aw->fp); aw->fp = NULL;
		free(aw->program_name); aw->program_name = NULL;
		free(aw->command); aw->command = NULL;
	}
	free(aw);
	return(NULL);
}

/**
 * @fn aw_clean
 *
 * @brief flush the content of buffer and close file
 */
void aw_clean(
	aw_t *aw)
{
	if(aw != NULL) {
		if(aw->conf.footer != NULL) {
			aw->conf.footer(aw, NULL, NULL);
		}

		zfclose(aw->fp); aw->fp = NULL;
		free(aw->program_name); aw->program_name = NULL;
		free(aw->command); aw->command = NULL;
	}
	free(aw);
	return;
}


/* unittest */

#define _str(x)		x, strlen(x)
#define _seq(x)		(uint8_t const *)(x), strlen(x)

struct aw_unittest_ctx_s {
	gref_idx_t *idx;
	gaba_result_t **res;
	int64_t cnt;
};

void *aw_unittest_init(
	void *params)
{
	struct aw_unittest_ctx_s *ctx = (struct aw_unittest_ctx_s *)malloc(
		sizeof(struct aw_unittest_ctx_s));

	gref_pool_t *pool = gref_init_pool(GREF_PARAMS(
		.k = 3,
		.seq_head_margin = 32,
		.seq_tail_margin = 32));
	gref_append_segment(pool, _str("sec0"), _seq("GGRA"));
	gref_append_segment(pool, _str("sec1"), _seq("MGGG"));
	gref_append_link(pool, _str("sec0"), 0, _str("sec1"), 0);
	gref_append_link(pool, _str("sec1"), 0, _str("sec2"), 0);
	gref_append_segment(pool, _str("sec2"), _seq("ACVVGTGT"));
	gref_append_link(pool, _str("sec0"), 0, _str("sec2"), 0);
	gref_idx_t *idx = gref_build_index(gref_freeze_pool(pool));

	ctx->idx = idx;


	struct gaba_result_s **res = (struct gaba_result_s **)malloc(
		3 * sizeof(struct gaba_result_s *));
	/* aln 0 */ {
		res[0] = (struct gaba_result_s *)malloc(
			  sizeof(struct gaba_result_s)
			+ 3 * sizeof(struct gaba_path_section_s)
			+ sizeof(struct gaba_path_s)
			+ 3 * sizeof(uint32_t));

		struct gaba_path_section_s *s = (struct gaba_path_section_s *)(res[0] + 1);
		s[0] = (struct gaba_path_section_s){ 0, 0, 0, 0, 4, 4, 8, 0 };
		s[1] = (struct gaba_path_section_s){ 2, 2, 0, 0, 4, 4, 8, 8 };
		s[2] = (struct gaba_path_section_s){ 4, 4, 0, 0, 8, 8, 16, 16 };

		struct gaba_path_s *p = (struct gaba_path_s *)(s + 3);
		p->len = 32;
		p->offset = 0;
		p->array[0] = 0x55555555;
		p->array[1] = 0x01;
		p->array[2] = 0;

		res[0]->sec = s;
		res[0]->path = p;
		res[0]->score = 10;
		res[0]->slen = 3;
		res[0]->qual = 100;
	}

	/* aln 1 */ {
		res[1] = (struct gaba_result_s *)malloc(
			  sizeof(struct gaba_result_s)
			+ 3 * sizeof(struct gaba_path_section_s)
			+ sizeof(struct gaba_path_s)
			+ 3 * sizeof(uint32_t));

		struct gaba_path_section_s *s = (struct gaba_path_section_s *)(res[1] + 1);
		s[0] = (struct gaba_path_section_s){ 0, 5, 0, 4, 4, 4, 8, 0 };
		s[1] = (struct gaba_path_section_s){ 2, 3, 0, 0, 4, 4, 8, 8 };
		s[2] = (struct gaba_path_section_s){ 4, 1, 2, 0, 2, 2, 4, 16 };

		struct gaba_path_s *p = (struct gaba_path_s *)(s + 3);
		p->len = 24;
		p->offset = 8;
		p->array[0] = 0x55555500;
		p->array[1] = 0x01;
		p->array[2] = 0;

		res[1]->sec = s;
		res[1]->path = p;
		res[1]->score = 8;
		res[1]->slen = 3;
		res[1]->qual = 110;
	}

	/* aln 2 */ {
		res[2] = (struct gaba_result_s *)malloc(
			  sizeof(struct gaba_result_s)
			+ 3 * sizeof(struct gaba_path_section_s)
			+ sizeof(struct gaba_path_s)
			+ 3 * sizeof(uint32_t));

		struct gaba_path_section_s *s = (struct gaba_path_section_s *)(res[2] + 1);
		s[0] = (struct gaba_path_section_s){ 0, 0, 0, 0, 4, 4, 8, 0 };
		s[1] = (struct gaba_path_section_s){ 4, 4, 0, 0, 8, 8, 16, 8 };

		struct gaba_path_s *p = (struct gaba_path_s *)(s + 3);
		p->len = 24;
		p->offset = 16;
		p->array[0] = 0x55550000;
		p->array[1] = 0x00000155;
		p->array[2] = 0;

		res[2]->sec = s;
		res[2]->path = p;
		res[2]->score = 6;
		res[2]->slen = 2;
		res[2]->qual = 90;
	}
	ctx->res = res;
	ctx->cnt = 3;
	return((void *)ctx);
}

void aw_unittest_clean(
	void *_ctx)
{
	struct aw_unittest_ctx_s *ctx = (struct aw_unittest_ctx_s *)_ctx;
	gref_clean(ctx->idx); ctx->idx = NULL;
	free(ctx->res[0]); ctx->res[0] = NULL;
	free(ctx->res[1]); ctx->res[1] = NULL;
	free(ctx->res[2]); ctx->res[2] = NULL;
	free(ctx->res); ctx->res = NULL;
	free(ctx);
	return;
}

#define omajinai() \
	struct aw_unittest_ctx_s *c = (struct aw_unittest_ctx_s *)gctx;

unittest_config(
	.name = "aw",
	.depends_on = { "gaba", "gref", "zf" },
	.init = aw_unittest_init,
	.clean = aw_unittest_clean
);

unittest()
{
	assert(SAM_DEFAULT_READGROUP == 1);
}

unittest()
{
	omajinai();

	aw_t *aw = aw_init("./test.sam", c->idx, NULL);

	assert(aw != NULL, "%p", aw);

	aw_clean(aw);
	remove("./test.sam");
}

unittest()
{
	omajinai();

	char const *path = "./test.sam";
	aw_t *aw = aw_init(path, c->idx, NULL);
	aw_clean(aw);

	char const *sam =
		"@HD\tVN:1.0\tSO:unsorted\n"
		"@SQ\tSN:sec0\tLN:4\n"
		"@SQ\tSN:sec1\tLN:4\n"
		"@SQ\tSN:sec2\tLN:8\n"
		"@RG\tID:1\n";
	char *rbuf = (char *)malloc(strlen(sam) + 1);

	zf_t *fp = zfopen(path, "r");
	int64_t size = zfread(fp, rbuf, strlen(sam) + 1);

	assert(size == strlen(sam), "size(%lld, %lld)", size, strlen(sam));
	assert(memcmp(rbuf, sam, MIN2(size, strlen(sam))) == 0, "%s%s", dump(rbuf, size), dump(sam, strlen(sam)));

	zfclose(fp);
	free(rbuf);
	remove(path);
}

unittest()
{
	omajinai();

	char const *path = "./test.sam";
	aw_t *aw = aw_init(path, c->idx, AW_PARAMS(
		.program_name = "hoge"));
	aw_clean(aw);

	char const *sam =
		"@HD\tVN:1.0\tSO:unsorted\n"
		"@SQ\tSN:sec0\tLN:4\n"
		"@SQ\tSN:sec1\tLN:4\n"
		"@SQ\tSN:sec2\tLN:8\n"
		"@RG\tID:1\n"
		"@PG\tID:0\tPN:hoge\n";
	char *rbuf = (char *)malloc(strlen(sam) + 1);

	zf_t *fp = zfopen(path, "r");
	int64_t size = zfread(fp, rbuf, strlen(sam) + 1);

	assert(size == strlen(sam), "size(%lld, %lld)", size, strlen(sam));
	assert(memcmp(rbuf, sam, MIN2(size, strlen(sam))) == 0, "%s%s", dump(rbuf, size), dump(sam, strlen(sam)));

	zfclose(fp);
	free(rbuf);
	remove(path);
}

unittest()
{
	omajinai();

	char const *path = "./test.sam";
	aw_t *aw = aw_init(path, c->idx, AW_PARAMS(
		.command = "--hoge=aaa --fuga=bbb\t--piyo=ccc"));
	aw_clean(aw);

	char const *sam =
		"@HD\tVN:1.0\tSO:unsorted\n"
		"@SQ\tSN:sec0\tLN:4\n"
		"@SQ\tSN:sec1\tLN:4\n"
		"@SQ\tSN:sec2\tLN:8\n"
		"@RG\tID:1\n"
		"@PG\tCL:--hoge=aaa --fuga=bbb --piyo=ccc\n";
	char *rbuf = (char *)malloc(strlen(sam) + 1);

	zf_t *fp = zfopen(path, "r");
	int64_t size = zfread(fp, rbuf, strlen(sam) + 1);

	assert(size == strlen(sam), "size(%lld, %lld)", size, strlen(sam));
	assert(memcmp(rbuf, sam, MIN2(size, strlen(sam))) == 0, "%s%s", dump(rbuf, size), dump(sam, strlen(sam)));

	zfclose(fp);
	free(rbuf);
	remove(path);
}

/* append alignment */
unittest()
{
	omajinai();

	char const *path = "./test.sam";
	aw_t *aw = aw_init(path, c->idx, NULL);
	aw_append_alignment(aw, c->idx, c->idx, (gaba_result_t const *const *)c->res, c->cnt);
	aw_clean(aw);

	char const *sam =
		"@HD\tVN:1.0\tSO:unsorted\n"
		"@SQ\tSN:sec0\tLN:4\n"
		"@SQ\tSN:sec1\tLN:4\n"
		"@SQ\tSN:sec2\tLN:8\n"
		"@RG\tID:1\n"
		"sec0\t0\tsec0\t0\t255\t4M\tsec1\t0\t0\tGGRA\t*\tRG:Z:1\n"
		"sec1\t0\tsec1\t0\t255\t4M\tsec2\t0\t0\tMGGG\t*\tRG:Z:1\n"
		"sec2\t0\tsec2\t0\t255\t8M\t*\t0\t0\tACVVGTGT\t*\tRG:Z:1\n"
		"sec2\t16\tsec0\t0\t255\t4M4S\tsec1\t0\t0\tACVVGTGT\t*\tRG:Z:1\n"
		"sec1\t16\tsec1\t0\t255\t4M\tsec2\t2\t0\tMGGG\t*\tRG:Z:1\n"
		"sec0\t16\tsec2\t2\t255\t2S2M\t*\t0\t0\tGGRA\t*\tRG:Z:1\n"
		"sec0\t0\tsec0\t0\t255\t4M\tsec2\t0\t0\tGGRA\t*\tRG:Z:1\n"
		"sec2\t0\tsec2\t0\t255\t8M\t*\t0\t0\tACVVGTGT\t*\tRG:Z:1\n";
	char *rbuf = (char *)malloc(1024);

	zf_t *fp = zfopen(path, "r");
	int64_t size = zfread(fp, rbuf, 1024);

	assert(size == strlen(sam), "size(%lld, %lld)", size, strlen(sam));
	assert(memcmp(rbuf, sam, MIN2(size, strlen(sam))) == 0, "%s%s, %s, %s", dump(rbuf, size), dump(sam, strlen(sam)), rbuf, sam);

	zfclose(fp);
	free(rbuf);
	remove(path);
}


/* append alignment (hard clip) */
unittest()
{
	omajinai();

	char const *path = "./test.sam";
	aw_t *aw = aw_init(path, c->idx, AW_PARAMS(.clip = 'H'));
	aw_append_alignment(aw, c->idx, c->idx, (gaba_result_t const *const *)c->res, c->cnt);
	aw_clean(aw);

	char const *sam =
		"@HD\tVN:1.0\tSO:unsorted\n"
		"@SQ\tSN:sec0\tLN:4\n"
		"@SQ\tSN:sec1\tLN:4\n"
		"@SQ\tSN:sec2\tLN:8\n"
		"@RG\tID:1\n"
		"sec0\t0\tsec0\t0\t255\t4M\tsec1\t0\t0\tGGRA\t*\tRG:Z:1\n"
		"sec1\t0\tsec1\t0\t255\t4M\tsec2\t0\t0\tMGGG\t*\tRG:Z:1\n"
		"sec2\t0\tsec2\t0\t255\t8M\t*\t0\t0\tACVVGTGT\t*\tRG:Z:1\n"
		"sec2\t16\tsec0\t0\t255\t4M4H\tsec1\t0\t0\tACVV\t*\tRG:Z:1\n"
		"sec1\t16\tsec1\t0\t255\t4M\tsec2\t2\t0\tMGGG\t*\tRG:Z:1\n"
		"sec0\t16\tsec2\t2\t255\t2H2M\t*\t0\t0\tRA\t*\tRG:Z:1\n"
		"sec0\t0\tsec0\t0\t255\t4M\tsec2\t0\t0\tGGRA\t*\tRG:Z:1\n"
		"sec2\t0\tsec2\t0\t255\t8M\t*\t0\t0\tACVVGTGT\t*\tRG:Z:1\n";
	char *rbuf = (char *)malloc(1024);

	zf_t *fp = zfopen(path, "r");
	int64_t size = zfread(fp, rbuf, 1024);

	assert(size == strlen(sam), "size(%lld, %lld)", size, strlen(sam));
	assert(memcmp(rbuf, sam, MIN2(size, strlen(sam))) == 0, "%s%s, %s, %s", dump(rbuf, size), dump(sam, strlen(sam)), rbuf, sam);

	zfclose(fp);
	free(rbuf);
	remove(path);
}

/**
 * end of aw.c
 */
