# libaw

Libaw is a alignment writer library, mainly targeting internal use in nucleotide sequence local alignment programs. The library can handle the SAM format and the [Graphical Pairwise Alignment (GPA)](https://github.com/ocxtal/gpa) format.

## Functions

### aw\_init

Initialize an alignment writer object.

```
aw_t *aw_init(
	char const *path,
	gref_idx_t const *idx,
	aw_params_t const *params);
```

### aw\_clean

Destroy the object.

```
void aw_clean(aw_t *aw);
```

### aw\_append\_alignment

Append an alignment result. See [libgaba](https://github.com/ocxtal/libgaba) for the details of the `struct gaba_result_s` container and [libgref](https://github.com/ocxtal/libgref) for the sequence indexer objects.

```
void aw_append_alignment(
	aw_t *aw,
	gref_idx_t const *ref,
	gref_acv_t const *query,
	struct gaba_result_s const *const *aln,
	int64_t cnt);
```

## License

MIT

Copyright (c) 2016 Hajime Suzuki
