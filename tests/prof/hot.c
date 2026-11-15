#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double work(int n)
{
	double s = 0;
	for (int i = 0; i < n; i++)
		s += (double)i * 0.5 / (i + 1);
	return s;
}

static char *grow(size_t n)
{
	char *p = malloc(n);
	memset(p, 1, n);
	return p;
}

static double hotloop(int n)
{
	double s = 0;
	for (int k = 0; k < n; k++)
		for (int i = 0; i < 1000; i++)
			s += (i ^ k) * 1.0001;
	return s;
}

static void leafy(char **keep, int i) { keep[i] = grow(1000 + (size_t)i * 64); }

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 20000;
	char *keep[2000];
	double s = 0;
	for (int i = 0; i < 2000; i++) {
		leafy(keep, i);
		if (i % 10 == 0)
			s += work(n);
	}
	s += hotloop(n);
	for (int i = 0; i < 2000; i += 2)
		free(keep[i]);
	char *big = grow(5 << 20);
	free(big);
	printf("%f\n", s);
	return 0;
}
