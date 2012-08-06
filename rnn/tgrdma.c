#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "rnn.h"
#include "rnndec.h"

static void usage()
{
	fprintf(stderr, "Usage:\n\ttgrdma [-f file.xml]\n");
	exit(2);
}

FILE *fp;
static char *line;
static size_t line_len;

static int get_next_word(unsigned long *dst)
{
	int ret;
	char *end;
restart:
	if ((ret = getline(&line, &line_len, fp)) < 0)
		return ret;

	if (line_len > 1 && *line == '#') {
		fputs(line, stdout);
		goto restart;
	}

	*dst = strtoul(line, &end, 16);
	if (!end) {
		fprintf(stderr, "cannot parse line '%s'\n", line);
		exit(1);
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int c;
	char *file = NULL;
	unsigned long x;
	struct rnndeccontext *vc;
	struct rnndb *db;

	while ((c = getopt(argc, argv, "f:")) != -1) {
		switch (c) {
		case 'f':
			file = strdup(optarg);
			break;

		default:
			usage();
		}
	}

	if (!file)
		usage();

	/* prepare rnn stuff for lookup */
	rnn_init();
	db = rnn_newdb();
	rnn_parsefile(db, "tgr_3d.xml");
	rnn_prepdb(db);
	vc = rnndec_newcontext(db);
	vc->colors = &envy_def_colors;
	struct rnndomain *dom = rnn_finddomain(db, "TGR3D");
	if (!dom) {
		fprintf(stderr, "Could not find domain TGR3D\n");
		exit(1);
	}

	if (!strcmp(file, "-"))
		fp = stdin;
	else {
		fp = fopen(file, "r");
		if (!fp) {
			perror("fopen");
			exit(1);
		}
	}

	while (get_next_word(&x) >= 0) {
		unsigned long value;
		int op, offset, address, class, mask, size, i;

		/* decode instruction */
                op = (x & 0xf0000000) >> 28;
		offset = op != 5 ? (x & 0x0fff0000) >> 16 : 0;
		address = op == 3 ? (x & 0x0fffffff) >> 4 : 0; 
		class = op == 0 ? (x & 0xffc0) >> 6 : 0;
		mask = op == 0 ? (x & 0x3f) : 0;
		size = (op == 1 || op == 2 || op == 6) ? x & 0xffff : 0;
		value = op == 4 ? x & 0xffff : 0;

		switch (op) {
		case 0:
			printf("setclass 0x%X 0x%X 0x%X\n", class, offset, mask);
			continue;

		case 1:
			printf("incr 0x%X 0x%X\n", offset, size);
			break;

		case 2:
			printf("nonincr 0x%X 0x%X\n", offset, size);
			break;

		case 3:
			printf("restart 0x%X\n", address);
			break;

		case 4:
			printf("imm 0x%X 0x%lX\n", offset, value);
			size = 1; /* hack: make sure we enter the loop */
			break;

		default:
			fprintf(stderr, "unknown opcode %d (%08lx)\n", op, x);
			exit(1);
		}

		for (i = 0; i < size; ) {
			struct rnndecaddrinfo *info = rnndec_decodeaddr(vc, dom, op != 2 ? offset + i : offset, 0);
			if (op != 4 && get_next_word(&value)) {
				fprintf(stderr, "unexpected end of file!\n");
				exit(1);
			}

			if (info && info->typeinfo)
				printf("\t%s => %s\n", info->name, rnndec_decodeval(vc, info->typeinfo, value, info->width));
			else if (info)
				printf("\t%s => 0x%lX\n", info->name, value);
			else
				printf("\t0x%X => 0x%lX\n", offset, value);

			++i;
		}
	}

	fclose(fp);
	free(line);
	return 0;
}
