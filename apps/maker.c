#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <error.h>
#include <stdint.h>
#include <stdbool.h>
#include <getopt.h>
#include <linux/types.h>

#define BUFLEN 65536
#define min(a, b) (a < b ? a : b)

enum {
	MODE_CREAT = 1,
	MODE_MODIFY,
};

static int do_modify(const char *fname)
{
	int err = 0, fd, cnt, i, j, width;
	ssize_t len;
	char tmpbuf[32];
	unsigned long pos[10];

	fd = open(fname, O_RDWR);
	if (fd < 0) {
		err = fd;
		perror("open");
		goto out0;
	}

	srand((unsigned) time(NULL));

	cnt = 5 + rand() % 6;

	len = lseek(fd, 0, SEEK_END);
	if (len < 0) {
		err = len;
		perror("lseek");
		goto out1;
	}
	width = snprintf(tmpbuf, sizeof(tmpbuf), "%d", len);

	for (i = 0; i < cnt; i++) {
		pos[i] = rand() % len;
	}

	for (i = 0; i < cnt; ++i) {
		for (j = i + 1; j < cnt; ++j) {
			if (pos[i] > pos[j]) {
				unsigned long tmp =  pos[i];
				pos[i] = pos[j];
				pos[j] = tmp;
			}
		}
	}

	for (i = 0; i < cnt; i++) {
		ssize_t _l;
		char c[1], _c;
		err = lseek(fd, pos[i], SEEK_SET);
		if (err < 0) {
			perror("lseek");
			goto out1;
		}
		_l = read(fd, c, 1);
		if (_l < 0) {
			err = _l;
			perror("read");
			goto out1;
		}
		_c = c[0];
		c[0] += 1;
		err = lseek(fd, pos[i], SEEK_SET);
		if (err < 0) {
			perror("lseek");
			goto out1;
		}
		_l = write(fd, c, 1);
		if (_l < 0) {
			err = _l;
			perror("write");
			goto out1;
		}
		printf("\x1b[34m%*ld %*o %*o\x1b[0m\n",
				width, pos[i] + 1,
				3, _c,
				3, c[0]);
	}
	err = 0;
out1:
	close(fd);
out0:
	return err;
}

static int do_create(const char *fname, size_t len)
{
	int err = 0, fd, i;
	size_t written = 0;
	char *buf;

	printf("create a file %s\n", fname);

	buf = malloc(BUFLEN);
	if (!buf) {
		err = -ENOMEM;
		perror("malloc");
		goto out0;
	}

	fd = open(fname, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		err = fd;
		perror("open");
		goto out1;
	}

	srand((unsigned) time(NULL));

	while (written < len) {
		ssize_t tx, _len;
		_len = min(len - written, BUFLEN);
		for (i = 0; i < _len - 1; i++)
			buf[i] = 41 + rand() % 80;
		buf[i] = '\n';
		tx = write(fd, buf, _len);
		if (tx < 0) {
			err = tx;
			perror("write");
			goto out2;
		}
		written += tx;
		//do_show_tree(fname);
	}
out2:
	close(fd);
out1:
	free(buf);
out0:
	return err;
}

int main(int argc, char **argv)
{
	int err = 0, ch, mode;
	size_t len = 0;
	const char *fname = NULL;

	while ((ch = getopt(argc, argv, "f:l:b:")) != -1) {
		switch (ch) {
		case 'f':
			fname = optarg;
			break;
		case 'l':
			len = atoi(optarg);
			break;
		default:
			break;
		}
	}

	if (!fname) {
		fprintf(stderr, "please specify a file name with -f\n");
		exit(0);
	}

	if (len)
		mode = MODE_CREAT;
	else
		mode = MODE_MODIFY;

	switch (mode) {
	case MODE_CREAT:
		err = do_create(fname, len);
		break;
	case MODE_MODIFY:
		err = do_modify(fname);
		break;
	}

	return err;
}

