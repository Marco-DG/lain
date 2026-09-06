// The same shape, guarded. Should be proven check-free.
int probe(int i) {
    int a[4];
    a[0] = 1;
    a[1] = 2;
    a[2] = 3;
    a[3] = 4;
    if (i >= 0) {
        if (i < 4) {
            return a[i];
        }
    }
    return 0;
}
