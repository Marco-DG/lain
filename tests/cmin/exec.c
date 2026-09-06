int add3(int a, int b, int c) {
    return a + b + c;
}
int main(void) {
    int t[4];
    int i;
    i = 0;
    while (i < 4) {
        t[i] = i * 3;
        i = i + 1;
    }
    return add3(t[1], t[2], t[3]) % 251;
}
