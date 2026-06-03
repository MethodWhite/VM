// pic_real: 3M iter, array of 3 shapes alternated.
typedef struct { int kind; int r, w, h, b; } Shape;
static int area(Shape *s) {
    if (s->kind == 0) return s->r * s->r * 3;
    else if (s->kind == 1) return s->w * s->h;
    else return (s->b * s->h) / 2;
}
int main(void) {
    Shape shapes[3];
    shapes[0].kind = 0; shapes[0].r = 5; shapes[0].w = 0; shapes[0].h = 0; shapes[0].b = 0;
    shapes[1].kind = 1; shapes[1].r = 0; shapes[1].w = 4; shapes[1].h = 6; shapes[1].b = 0;
    shapes[2].kind = 2; shapes[2].r = 0; shapes[2].w = 0; shapes[2].h = 8; shapes[2].b = 3;
    int sum = 0; int i = 0;
    while (i < 3000000) {
        sum += area(&shapes[i % 3]);
        i++;
    }
    return sum;
}
