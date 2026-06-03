// polymorphic: 10M, 3 subtypes via tagged union.
typedef struct { int kind; int r, w, h, b; } Shape;
static int area(Shape *s) {
    if (s->kind == 0) return s->r * s->r * 3;
    else if (s->kind == 1) return s->w * s->h;
    else return (s->b * s->h) / 2;
}
int main(void) {
    Shape c = {0, 5, 0, 0, 0};
    Shape r = {1, 0, 4, 6, 0};
    Shape t = {2, 0, 0, 8, 3};
    int sum = 0; int i = 0;
    while (i < 10000000) {
        int mod = i % 3;
        if (mod == 0) sum += area(&c);
        else if (mod == 1) sum += area(&r);
        else sum += area(&t);
        i++;
    }
    return sum;
}
