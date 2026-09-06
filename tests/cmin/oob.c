// An out-of-bounds read the Lain-IR bounds analysis should catch — in C, from a C front end.
int probe(int i) {
    int a[4];
    a[0] = 1;
    return a[i];          // i is unconstrained: not provably in bounds
}
