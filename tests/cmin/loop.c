// A counted scan: every index provably in bounds.
int sum(int n) {
    int a[8];
    int i;
    int acc;
    acc = 0;
    i = 0;
    while (i < 8) {
        a[i] = i;
        i = i + 1;
    }
    i = 0;
    while (i < 8) {
        acc = acc + a[i];
        i = i + 1;
    }
    return acc;
}
