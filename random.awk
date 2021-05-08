#!/usr/bin/gawk -f
#

function add(x, y) {
	QUEUE[++N] = x " " y
	}

function remove(n,   k, prev) {
	k = N - n;
	if (k in QUEUE) {
		prev = QUEUE[k];
		delete QUEUE[k];
		}

	return (prev);
	}

function set(x, y, r, g, b,   fn) {
	fn = sprintf ("%s/dot-%1d%1d", PATH, x, y);
	printf ("%d %d %d\n", r, g, n) >fn;
	close (fn);
	}

BEGIN {
	program = "random";
	STDERR = "/dev/stderr";

	PATH = ARGV[1]; ARGV[1] = "";
	PATH = PATH "/d";
	QLEN = 3;

	srand();
	while (1) {
		x = int(rand() * 8);
		y = int(rand() * 8);
		r = rand() * 256;
		g = rand() * 256;
		b = rand() * 256;

		if ((prev = remove(QLEN)) != "") {
			split(prev, q, " ");
			set(q[1], q[2], 0, 0, 0);
			}

		set(x, y, r, g, b);
		add(x, y);

		if (system( "sleep 1") != 0)
			break;
		}

	exit (0);
	}

