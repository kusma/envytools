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
static int echo_comments = 1, echo_commands = 0, echo_data = 0;

static int get_next_word(unsigned long *dst)
{
	int ret;
	char *end;
restart:
	if ((ret = getline(&line, &line_len, fp)) < 0)
		return ret;

	if (line_len > 1 && *line == '#') {
		if (echo_comments)
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

static unsigned long get_next_value(void)
{
	unsigned long value;

	if (get_next_word(&value)) {
		fprintf(stderr, "unexpected end of file!\n");
		exit(1);
	}

	if (echo_data)
		printf("# 0x%08lx\n", value);

	return value;
}

static struct rnndeccontext *vc;
static struct rnndb *db;
struct rnndomain *nvhost_dom, *tgr3d_dom;

static void handle_write(int class_id, int reg, unsigned long val)
{
	struct rnndomain *dom = NULL;

	switch (class_id) {
	case 1:
		dom = nvhost_dom;
		break;

	case 0x0:
	case 0x60:
		dom = tgr3d_dom;
		break;
	}

	if (dom) {
		struct rnndecaddrinfo *info = rnndec_decodeaddr(vc, dom, reg, 0);

		if (info && info->typeinfo)
			printf("\t%s => %s\n", info->name, rnndec_decodeval(vc, info->typeinfo, val, info->width));
		else if (info)
			printf("\t%s => 0x%lx\n", info->name, val);
		else
			printf("\t0x%x => 0x%lx\n", reg, val);
	} else
		printf("\t0x%x 0x%x => 0x%lx\n", class_id, reg, val);
}

int main(int argc, char *argv[])
{
	int c;
	char *file = NULL;
	unsigned long x;

	while ((c = getopt(argc, argv, "f:en")) != -1) {
		switch (c) {
		case 'f':
			file = strdup(optarg);
			break;

		case 'e':
			echo_commands = 1;
			echo_data = 1;
			break;

		case 'n':
			echo_comments = 0;
			echo_commands = 0;
			echo_data = 0;
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
	tgr3d_dom = rnn_finddomain(db, "TGR3D");
	nvhost_dom = rnn_finddomain(db, "NVHOST");
	if (!tgr3d_dom || !nvhost_dom) {
		fprintf(stderr, "Could not find domains\n");
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

	int class_id = 0;
	while (get_next_word(&x) >= 0) {
		unsigned long value;
		int op, offset, mask, count, i;

		if (echo_commands)
			printf("# 0x%08lx\n", x);

                op = (x & 0xf0000000) >> 28;

		/* decode instructions */
		switch (op) {
		case 0:
			offset = (x & 0x0fff0000) >> 16;
			class_id = (x & 0xffc0) >> 6;
			mask = x & 0x3f;
			printf("setclass 0x%x 0x%x 0x%x\n", offset, class_id, mask);

			for (i = 0; i < 32; ++i) {
				if (!(mask & (1 << i)))
					continue;
				handle_write(class_id, offset + i, get_next_value());
			}

			break;

		case 1:
			offset = (x & 0x0fff0000) >> 16;
			count = x & 0xffff;
			printf("incr 0x%x 0x%x\n", offset, count);
			for (i = 0; i < count; ++i)
				handle_write(class_id, offset + i, get_next_value());
			break;

		case 2:
			offset = (x & 0x0fff0000) >> 16;
			count = x & 0xffff;
			printf("nonincr 0x%x 0x%x\n", offset, count);
			for (i = 0; i < count; ++i)
				handle_write(class_id, offset, get_next_value());
			break;

		case 3:
			offset = (x & 0x0fff0000) >> 16;
			mask = x & 0xffff;
			printf("mask 0x%x 0x%x\n", offset, mask);

			for (i = 0; i < 32; ++i) {
				if (!(mask & (1 << i)))
					continue;
				handle_write(class_id, offset + i, get_next_value());
			}
			break;

		case 4:
			offset = (x & 0x0fff0000) >> 16;
			value = x & 0xffff;
			printf("imm 0x%x 0x%lx\n", offset, value);
			handle_write(class_id, offset, value);
			break;

#if 0
		case 5:
			address = op == 5 ? (x & 0x0fffffff) >> 4 : 0; 
			printf("restart 0x%x\n", address);
			break;

		case 6:
			offset = (x & 0x0fff0000) >> 16;
			count = x & 0xffff;
			printf("gather (unfinished) 0x%x 0x%x\n", offset, count);
			break;
#endif

		default:
			fprintf(stderr, "unknown opcode %d (%08lx)\n", op, x);
			exit(1);
		}
	}

	fclose(fp);
	free(line);
	return 0;
}
